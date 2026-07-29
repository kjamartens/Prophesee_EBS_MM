///////////////////////////////////////////////////////////////////////////////
// FILE:          ProphEBS.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Device adapter for the Prophesee EBS camera (Goals 1-4).
//                See ProphEBS.h for the full design rationale.
//
// COPYRIGHT:     Koen J.A. Martens, 2026
// LICENSE:       This file is distributed under the BSD license.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include "ProphEBS.h"

#include "CameraImageMetadata.h"
#include "ModuleInterface.h"

#include <metavision/hal/facilities/i_geometry.h>
#include <metavision/hal/facilities/i_hw_identification.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <windows.h>

// Name used by MicroManager to refer to this device, and to load it from the
// "mmgr_dal_ProphEBS.dll" library (see ProphEBSModule.cpp).
const char* g_ProphEBSCameraDeviceName = "ProphEBS-Camera";

// Goal 2 read-only property names.
const char* g_PropConnectionStatus = "EBS-ConnectionStatus";
const char* g_PropModel = "EBS-Model";
const char* g_PropSerial = "EBS-Serial";
const char* g_PropConnectionType = "EBS-ConnectionType";
const char* g_PropIntegrator = "EBS-Integrator";

// Goal 4 recording property names.
const char* g_PropRawFilePath = "EBS-RawFilePath";
const char* g_PropRawRecordingStatus = "EBS-RawRecordingStatus";
const char* g_PropTempFolder = "EBS-TempRecordingFolder";

namespace
{
   ///////////////////////////////////////////////////////////////////////////
   // Goal 4: MicroManager MDA save-path auto-discovery.
   //
   // MMCore never tells a C++ device adapter where a Multi-D Acquisition is
   // saving its images -- that's Studio (Java)-level state (see the "why
   // this can't be baked in automatically" discussion in docs/DEVLOG.md).
   // This adapter discovers it anyway, purely by reading files MM already
   // writes as a side effect of normal operation -- no scripting, no
   // startup hook, nothing for the user to remember before clicking
   // Acquire!. Two independent sources are tried, in order:
   //
   // 1. TryGetMdaRootPrefixFromCoreLog() (primary) -- MM Studio logs the
   //    exact SequenceSettings JSON ("MDA Settings: {...}") to its own
   //    CoreLog file *synchronously*, right before triggering the
   //    acquisition engine, every single time an acquisition runs. Since
   //    MMCore/this DLL is loaded into the very same OS process as Studio's
   //    JVM (confirmed: the CoreLog filename embeds the process ID, which
   //    matches GetCurrentProcessId() from inside this DLL), this adapter
   //    can locate and tail its own process's CoreLog file and read back
   //    the freshest possible settings.
   // 2. TryGetMdaRootPrefixFromUserProfile() (fallback) -- MM also persists
   //    its UI state, including the MDA dialog's root/prefix, to a
   //    "Property Map" JSON file under
   //    %LOCALAPPDATA%\Micro-Manager\UserProfiles\*.json. IMPORTANT CAVEAT
   //    (found the hard way, see docs/DEVLOG.md Goal 4): this file only
   //    appears to be flushed to disk when MicroManager itself closes, NOT
   //    live as the dialog's fields change -- so it only reflects whatever
   //    was configured as of the *last* MM session, not necessarily the
   //    current one. Kept as a second-chance fallback (e.g. if the CoreLog
   //    format changes in a future MM version) rather than removed, since a
   //    stale-but-plausible path beats none at all when the primary lookup
   //    fails outright.
   //
   // Both rely on undocumented, internal MM implementation detail (a
   // specific log message string; a specific Java class's own preference
   // key) rather than any stable public API, and could change in a future
   // MM release. That's why every step is wrapped in try/catch and every
   // failure mode returns false/empty rather than throwing --
   // StopRawRecordingIfActive() always has a safe, fully independent
   // fallback (leaving the file at its GenerateAutoRawFilePath() staging
   // location) if neither lookup pans out.

   namespace pt = boost::property_tree;

   // Returns the directory this DLL itself was loaded from -- which is the
   // MicroManager installation directory, since that's where
   // mmgr_dal_ProphEBS.dll is deployed (same folder as CoreLogs\). Uses the
   // "address of a function in this module" trick (GetModuleHandleEx with
   // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) so this works with no DllMain
   // and without hardcoding the DLL's own name anywhere.
   std::string GetOwnModuleDirectory()
   {
      HMODULE hModule = nullptr;
      if (!GetModuleHandleExA(
             GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
             reinterpret_cast<LPCSTR>(&GetOwnModuleDirectory), &hModule))
         return {};

      char pathBuf[MAX_PATH];
      DWORD len = GetModuleFileNameA(hModule, pathBuf, MAX_PATH);
      if (len == 0 || len == MAX_PATH)
         return {};

      return std::filesystem::path(pathBuf).parent_path().string();
   }

   // FILETIME (100ns intervals since 1601) as a single comparable integer.
   uint64_t FileTimeToU64(const FILETIME& ft)
   {
      return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
   }

   // Locates the CoreLog file for *this* running MicroManager process --
   // MM names these "CoreLog<timestamp>_pid<PID>.txt" under a CoreLogs\
   // subfolder of the install directory, and since this DLL is loaded into
   // the same OS process as MMCore/Studio, GetCurrentProcessId() from here
   // matches the PID in that filename exactly.
   //
   // Matching by PID suffix alone isn't enough, though -- Windows reuses
   // process IDs once a process exits, so old CoreLog files from a
   // long-finished, completely unrelated earlier process can still be
   // sitting in this folder with the exact same "_pid<N>.txt" suffix as our
   // own, *currently running* process (confirmed happening on this dev
   // machine: a later pymmcore-plus test process was assigned a PID that
   // collided with an old real-GUI session's log file, and the discovery
   // logic picked that stale, unrelated file instead of failing over
   // cleanly). To rule those out, only a candidate file *created at or
   // after this process's own start time* (via GetProcessTimes()) is
   // trusted -- an old file from a previous process instance necessarily
   // predates that.
   std::string FindCurrentCoreLogPath()
   {
      std::string dllDir = GetOwnModuleDirectory();
      if (dllDir.empty())
         return {};

      namespace fs = std::filesystem;
      fs::path logsDir = fs::path(dllDir) / "CoreLogs";
      std::error_code ec;
      if (!fs::exists(logsDir, ec))
         return {};

      uint64_t processStart = 0;
      FILETIME creationTime{}, exitTime{}, kernelTime{}, userTime{};
      if (GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime))
         processStart = FileTimeToU64(creationTime);

      std::ostringstream suffixStream;
      suffixStream << "_pid" << GetCurrentProcessId() << ".txt";
      std::string suffix = suffixStream.str();

      fs::path best;
      uint64_t bestCreation = 0;
      bool found = false;
      for (const auto& entry : fs::directory_iterator(logsDir, ec))
      {
         const std::string name = entry.path().filename().string();
         if (name.size() < suffix.size())
            continue;
         if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;

         WIN32_FILE_ATTRIBUTE_DATA attrs{};
         if (!GetFileAttributesExA(entry.path().string().c_str(), GetFileExInfoStandard, &attrs))
            continue;
         uint64_t fileCreation = FileTimeToU64(attrs.ftCreationTime);

         if (processStart != 0 && fileCreation < processStart)
            continue; // predates this process -- a PID-collision leftover, not ours

         if (!found || fileCreation > bestCreation)
         {
            bestCreation = fileCreation;
            best = entry.path();
            found = true;
         }
      }
      if (!found)
         return {};

      return best.string();
   }

   // Reads the MDA dialog's currently configured save root/prefix by
   // tailing this process's own CoreLog file for the most recent
   // "MDA Settings: { ... }" block MM Studio logs right before running an
   // acquisition. MM's log format wraps multi-line continuations with a
   // "] " marker before the actual content on each line (matching the
   // fixed-width timestamp/thread-id/category prefix of a normal log line)
   // -- stripped off here before handing the reassembled text to the JSON
   // parser. Returns false if the log can't be found/read, the marker never
   // appears, or the block doesn't parse as expected -- e.g. a future MM
   // version changes this log message or its wrapping format.
   bool TryGetMdaRootPrefixFromCoreLog(std::string& outRoot, std::string& outPrefix, bool& outSave)
   {
      std::string logPath = FindCurrentCoreLogPath();
      if (logPath.empty())
         return false;

      std::ifstream in(logPath);
      if (!in)
         return false;

      std::vector<std::string> lines;
      std::string line;
      while (std::getline(in, line))
         lines.push_back(line);

      const std::string marker = "MDA Settings:";
      int markerIdx = -1;
      for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i)
      {
         if (lines[i].find(marker) != std::string::npos)
         {
            markerIdx = i;
            break;
         }
      }
      if (markerIdx < 0)
         return false;

      std::string json;
      bool sawOpenBrace = false;
      for (size_t i = static_cast<size_t>(markerIdx) + 1; i < lines.size(); ++i)
      {
         std::string content = lines[i];
         size_t markerPos = content.rfind("] ");
         if (markerPos != std::string::npos)
            content = content.substr(markerPos + 2);

         size_t firstNonSpace = content.find_first_not_of(" \t\r");
         if (firstNonSpace == std::string::npos)
            break; // blank line before the block closed -- unexpected format
         std::string trimmed = content.substr(firstNonSpace);

         if (!sawOpenBrace)
         {
            if (trimmed != "{")
               return false; // unexpected format -- bail out rather than guess
            sawOpenBrace = true;
         }

         json += content;
         json += "\n";

         if (trimmed == "}")
            break;
      }
      if (json.empty())
         return false;

      try
      {
         std::istringstream iss(json);
         pt::ptree tree;
         pt::read_json(iss, tree);
         outRoot = tree.get<std::string>("root");
         outPrefix = tree.get<std::string>("prefix");
         outSave = tree.get<bool>("save", true);
         return !outRoot.empty();
      }
      catch (...)
      {
         return false;
      }
   }

   // Finds the MM UserProfile JSON file to read. Prefers the profile named
   // first in UserProfiles\Index.json (correct for the common single-profile
   // case); if that can't be read/parsed, falls back to the most recently
   // modified *.json file in the folder (excluding Index.json itself). Only
   // used as TryGetMdaRootPrefixFromCoreLog()'s fallback -- see that
   // function's caller for why this alone isn't good enough (this file only
   // gets flushed to disk when MicroManager closes, not live).
   std::string FindMMUserProfilePath()
   {
      const char* localAppData = std::getenv("LOCALAPPDATA");
      if (localAppData == nullptr || localAppData[0] == '\0')
         return {};

      namespace fs = std::filesystem;
      fs::path profilesDir = fs::path(localAppData) / "Micro-Manager" / "UserProfiles";
      std::error_code ec;
      if (!fs::exists(profilesDir, ec))
         return {};

      std::string profileFileName;
      try
      {
         pt::ptree indexTree;
         pt::read_json((profilesDir / "Index.json").string(), indexTree);
         for (const auto& item : indexTree.get_child("map").get_child("Profiles").get_child("array"))
         {
            profileFileName = item.second.get<std::string>("File.scalar");
            break;
         }
      }
      catch (...)
      {
         profileFileName.clear();
      }

      fs::path profilePath;
      if (!profileFileName.empty())
         profilePath = profilesDir / profileFileName;

      if (profileFileName.empty() || !fs::exists(profilePath, ec))
      {
         fs::file_time_type newest{};
         bool found = false;
         for (const auto& entry : fs::directory_iterator(profilesDir, ec))
         {
            if (entry.path().extension() != ".json" || entry.path().filename() == "Index.json")
               continue;
            std::error_code mtimeEc;
            fs::file_time_type t = entry.last_write_time(mtimeEc);
            if (mtimeEc)
               continue;
            if (!found || t > newest)
            {
               newest = t;
               profilePath = entry.path();
               found = true;
            }
         }
         if (!found)
            return {};
      }

      return profilePath.string();
   }

   // Reads the MDA dialog's *last-saved-session* root/prefix out of the MM
   // UserProfile JSON located by FindMMUserProfilePath(). Fallback only --
   // see TryDiscoverMdaRootPrefix() below for why this can't be trusted as
   // the primary source. Returns false (leaving outRoot/outPrefix
   // untouched) if the profile can't be found, read, or doesn't have the
   // expected structure -- e.g. MM has never been run on this machine, or a
   // future MM version changes this internal preference layout.
   bool TryGetMdaRootPrefixFromUserProfile(std::string& outRoot, std::string& outPrefix, bool& outSave)
   {
      std::string profilePath = FindMMUserProfilePath();
      if (profilePath.empty())
         return false;

      try
      {
         pt::ptree root;
         pt::read_json(profilePath, root);

         // "org.micromanager.internal.dialogs.AcqControlDlg" contains literal
         // dots, which would otherwise be misread as ptree path separators --
         // path_type with '/' (a character that never appears in this key)
         // keeps it as a single key lookup.
         typedef pt::ptree::path_type path_type;
         const pt::ptree& prefsScalar = root.get_child("map").get_child("Preferences").get_child("scalar");
         const pt::ptree& acqDlgScalar = prefsScalar.get_child(
            path_type("org.micromanager.internal.dialogs.AcqControlDlg", '/')).get_child("scalar");
         std::string mdaSettingsJson = acqDlgScalar.get_child("MDA_SEQUENCE_SETTINGS").get<std::string>("scalar");

         // MDA_SEQUENCE_SETTINGS's "scalar" is itself a JSON string (MM
         // serializes its SequenceSettings object to JSON text before
         // embedding it in the outer property map) -- parse it a second time.
         std::istringstream iss(mdaSettingsJson);
         pt::ptree inner;
         pt::read_json(iss, inner);

         outRoot = inner.get<std::string>("root");
         outPrefix = inner.get<std::string>("prefix");
         outSave = inner.get<bool>("save", true);
         return !outRoot.empty();
      }
      catch (...)
      {
         return false;
      }
   }

   // Combined entry point: tries the live CoreLog-based lookup first
   // (accurate for the *current* session, including mid-session folder
   // changes), and only falls back to the UserProfile-based lookup
   // (accurate only as of the *last* MM session) if that fails outright --
   // a stale-but-plausible path beats none. outSave reports whether the
   // discovered MDA settings have "Save images" checked at all -- when
   // false, the caller should not go looking for a numbered save subfolder,
   // since MM won't be creating one for this run (an existing subfolder
   // found anyway would belong to some earlier, unrelated run).
   bool TryDiscoverMdaRootPrefix(std::string& outRoot, std::string& outPrefix, bool& outSave)
   {
      if (TryGetMdaRootPrefixFromCoreLog(outRoot, outPrefix, outSave))
         return true;
      return TryGetMdaRootPrefixFromUserProfile(outRoot, outPrefix, outSave);
   }

   // MM never saves directly into <root>/ -- for saving MDA modes (e.g.
   // MULTIPAGE_TIFF) it creates a per-run subfolder "<prefix>_<N>", where N
   // is one more than the highest such subfolder that already exists (a
   // rename of an existing folder to a higher number is honored too -- MM
   // just scans for the current max each time, it doesn't remember counts).
   // This mirrors that scan: finds the highest-numbered "<prefix>_<N>"
   // subfolder directly under mdaRoot. By the time an acquisition has
   // finished (this is only ever called from StopRawRecordingIfActive(),
   // i.e. after the whole MDA has run), MM has already created and been
   // writing into its own folder for *this* run throughout -- so rather
   // than re-deriving N independently (and risking an off-by-one mismatch
   // with MM's own counting), this just finds and reuses whichever
   // subfolder is already there with the highest number. Returns empty if
   // no such subfolder exists at all (e.g. a save mode that doesn't use
   // one, or saving disabled), which the caller treats as "fall back to
   // the flat, non-numbered layout."
   std::string FindHighestNumberedMdaSubfolder(const std::string& mdaRoot, const std::string& mdaPrefix)
   {
      namespace fs = std::filesystem;
      std::error_code ec;
      if (!fs::is_directory(mdaRoot, ec))
         return {};

      const std::string matchPrefix = mdaPrefix + "_";
      std::string bestName;
      long bestNumber = -1;

      for (const auto& entry : fs::directory_iterator(mdaRoot, ec))
      {
         std::error_code isDirEc;
         if (!entry.is_directory(isDirEc) || isDirEc)
            continue;

         std::string name = entry.path().filename().string();
         if (name.size() <= matchPrefix.size() || name.compare(0, matchPrefix.size(), matchPrefix) != 0)
            continue;

         std::string suffix = name.substr(matchPrefix.size());
         bool allDigits = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
         if (!allDigits)
            continue;

         long number = std::strtol(suffix.c_str(), nullptr, 10);
         if (number > bestNumber)
         {
            bestNumber = number;
            bestName = name;
         }
      }

      return bestName;
   }

   // Combines FindHighestNumberedMdaSubfolder() with a short retry (MM may
   // still be creating its own folder for this run at the exact moment
   // this is called -- see the two call sites in ProphEBS.cpp for why the
   // margin differs) and the "<N>_events_Pos0.raw" naming convention,
   // returning the full destination path -- or empty if no numbered
   // subfolder ever appears within the retry budget, which the caller
   // treats as "not available right now."
   std::string ComputeNumberedMdaDestination(const std::string& mdaRoot, const std::string& mdaPrefix,
      int maxRetries, int retryDelayMs)
   {
      namespace fs = std::filesystem;
      std::error_code ec;
      fs::create_directories(mdaRoot, ec);

      std::string numberedFolder = FindHighestNumberedMdaSubfolder(mdaRoot, mdaPrefix);
      for (int attempt = 0; numberedFolder.empty() && attempt < maxRetries; ++attempt)
      {
         CDeviceUtils::SleepMs(retryDelayMs);
         numberedFolder = FindHighestNumberedMdaSubfolder(mdaRoot, mdaPrefix);
      }
      if (numberedFolder.empty())
         return {};

      fs::path destDir = fs::path(mdaRoot) / numberedFolder;
      fs::create_directories(destDir, ec);
      return (destDir / (numberedFolder + "_events_Pos0.raw")).string();
   }
} // anonymous namespace

///////////////////////////////////////////////////////////////////////////////
// CProphEBSCamera implementation
///////////////////////////////////////////////////////////////////////////////

/**
 * Constructor. As with all MicroManager devices, no hardware access happens
 * here (there is no hardware yet, in any case) -- only cheap, local state
 * setup. Real initialization happens in Initialize().
 */
CProphEBSCamera::CProphEBSCamera() :
   initialized_(false),
   exposure_ms_(100.0),
   roiX_(0),
   roiY_(0),
   roiXSize_(g_TestImageWidth),
   roiYSize_(g_TestImageHeight),
   thd_(nullptr),
   cameraConnected_(false),
   connectionStatus_("Not connected"),
   cameraModel_("N/A"),
   cameraSerial_("N/A"),
   connectionType_("N/A"),
   integrator_("N/A"),
   sensorWidth_(g_TestImageWidth),
   sensorHeight_(g_TestImageHeight),
   frontImg_(&imgBufferA_),
   backImg_(&imgBufferB_),
   streaming_(false),
   cdCallbackId_(),
   frameBuilderThd_(nullptr),
   rawRecordingActive_(false),
   movePendingToMdaFolder_(false)
{
   InitializeDefaultErrorMessages();
   thd_ = new ProphEBSSequenceThread(this);
   frameBuilderThd_ = new ProphEBSFrameBuilderThread(this);
}

CProphEBSCamera::~CProphEBSCamera()
{
   StopSequenceAcquisition();
   StopEventStreaming();
   delete thd_;
   delete frameBuilderThd_;
}

void CProphEBSCamera::GetName(char* name) const
{
   CDeviceUtils::CopyLimitedString(name, g_ProphEBSCameraDeviceName);
}

bool CProphEBSCamera::Busy()
{
   return false;
}

/**
 * Initializes the device. Goal 2 adds a real connection attempt to a
 * Prophesee EBS via the Metavision SDK (see ConnectToCamera()); whether or
 * not that succeeds, Initialize() still logs its own debug message and
 * builds the static test image, so this adapter remains loadable/testable
 * on machines with no EBS attached (Goal 1 behavior is preserved as a
 * fallback, not replaced).
 */
int CProphEBSCamera::Initialize()
{
   if (initialized_)
      return DEVICE_OK;

   LogMessage("ProphEBS adapter initializing...", false);

   int nRet = CreateStringProperty(MM::g_Keyword_Name, g_ProphEBSCameraDeviceName, true);
   if (DEVICE_OK != nRet)
      return nRet;

   nRet = CreateStringProperty(MM::g_Keyword_Description,
      "Prophesee EBS Camera adapter (Goal 4: raw event-file recording)", true);
   if (DEVICE_OK != nRet)
      return nRet;

   nRet = CreateFloatProperty(MM::g_Keyword_Exposure, exposure_ms_, false);
   if (DEVICE_OK != nRet)
      return nRet;

   // Binning isn't meaningful yet (no real sensor) -- only 1x1 is offered,
   // matching GetBinning()/SetBinning() below.
   nRet = CreateStringProperty(MM::g_Keyword_Binning, "1", false);
   if (DEVICE_OK != nRet)
      return nRet;
   nRet = AddAllowedValue(MM::g_Keyword_Binning, "1");
   if (DEVICE_OK != nRet)
      return nRet;

   ConnectToCamera();

   nRet = CreateStringProperty(g_PropConnectionStatus, connectionStatus_.c_str(), true);
   if (DEVICE_OK != nRet)
      return nRet;
   nRet = CreateStringProperty(g_PropModel, cameraModel_.c_str(), true);
   if (DEVICE_OK != nRet)
      return nRet;
   nRet = CreateStringProperty(g_PropSerial, cameraSerial_.c_str(), true);
   if (DEVICE_OK != nRet)
      return nRet;
   nRet = CreateStringProperty(g_PropConnectionType, connectionType_.c_str(), true);
   if (DEVICE_OK != nRet)
      return nRet;
   nRet = CreateStringProperty(g_PropIntegrator, integrator_.c_str(), true);
   if (DEVICE_OK != nRet)
      return nRet;

   // Goal 4: raw event-file recording properties. EBS-RawFilePath is a
   // plain settable property (no CPropertyAction -- StartRawRecordingIfRequested()
   // just reads it back via GetProperty() when a finite sequence acquisition
   // starts); empty (the default) means "auto-discover the path." EBS-RawRecordingStatus
   // is created non-read-only so the device itself can update it via
   // SetProperty() as recording starts/stops/fails (MM silently no-ops
   // SetProperty() on properties created with the read-only flag set, even
   // from the owning device).
   nRet = CreateStringProperty(g_PropRawFilePath, "", false);
   if (DEVICE_OK != nRet)
      return nRet;

   nRet = CreateStringProperty(g_PropRawRecordingStatus, "Not recording", false);
   if (DEVICE_OK != nRet)
      return nRet;

   nRet = CreateStringProperty(g_PropTempFolder, "", false);
   if (DEVICE_OK != nRet)
      return nRet;

   if (cameraConnected_)
   {
      // Full-frame ROI over the real sensor geometry, replacing the Goal 1
      // defaults sized to the fallback test image.
      roiX_ = 0;
      roiY_ = 0;
      roiXSize_ = sensorWidth_;
      roiYSize_ = sensorHeight_;
      StartEventStreaming();
   }
   else
   {
      GenerateTestImage();
   }

   initialized_ = true;
   return DEVICE_OK;
}

int CProphEBSCamera::Shutdown()
{
   StopRawRecordingIfActive();
   StopEventStreaming();
   if (cameraConnected_)
   {
      // Release the HAL device handle by replacing cam_ with a fresh,
      // unopened Camera.
      cam_ = Metavision::Camera();
      cameraConnected_ = false;
   }
   initialized_ = false;
   return DEVICE_OK;
}

/**
 * Attempts to open the first available Prophesee EBS via the Metavision
 * SDK. On success, reads back identification info (serial, sensor name,
 * connection type, integrator) through the HAL's I_HW_Identification
 * facility so it can be exposed as read-only MM properties -- this is the
 * "report back the model number/serial so we can assure the connection is
 * good" requirement from Goal 2.
 *
 * Deliberately does not throw and does not fail Initialize(): a missing
 * camera (or missing Metavision driver/plugin) is an expected condition
 * during development without hardware attached, not a fatal adapter error.
 */
void CProphEBSCamera::ConnectToCamera()
{
   try
   {
      cam_ = Metavision::Camera::from_first_available();

      // Throws CameraException (caught below) if the facility isn't
      // available -- every real Prophesee device registers it, so this only
      // trips for unusual/unsupported hardware.
      Metavision::I_HW_Identification& hwId = cam_.get_facility<Metavision::I_HW_Identification>();

      cameraSerial_ = hwId.get_serial();
      connectionType_ = hwId.get_connection_type();
      integrator_ = hwId.get_integrator();

      Metavision::I_HW_Identification::SensorInfo sensorInfo = hwId.get_sensor_info();
      std::ostringstream modelStream;
      modelStream << sensorInfo.name_ << " (Gen " << sensorInfo.major_version_
                  << "." << sensorInfo.minor_version_ << ")";
      cameraModel_ = modelStream.str();

      // Real sensor geometry, used from here on instead of the
      // g_TestImageWidth/Height fallback (throws CameraException, caught
      // below, on the same unusual "no I_Geometry facility" condition as
      // I_HW_Identification above).
      Metavision::I_Geometry& geometry = cam_.get_facility<Metavision::I_Geometry>();
      sensorWidth_ = static_cast<unsigned>(geometry.get_width());
      sensorHeight_ = static_cast<unsigned>(geometry.get_height());

      cameraConnected_ = true;
      connectionStatus_ = "Connected";

      std::ostringstream logMsg;
      logMsg << "ProphEBS: connected to " << cameraModel_ << ", serial=" << cameraSerial_
             << ", connection=" << connectionType_ << ", geometry=" << sensorWidth_ << "x"
             << sensorHeight_;
      LogMessage(logMsg.str(), false);
   }
   catch (const std::exception& e)
   {
      // Covers Metavision::CameraException (thrown when no camera is found,
      // the driver isn't installed, or the HAL plugin can't load) as well as
      // any other std::exception the SDK might raise.
      cameraConnected_ = false;
      connectionStatus_ = std::string("Not connected: ") + e.what();

      std::ostringstream logMsg;
      logMsg << "ProphEBS: no EBS camera connected (" << e.what()
             << ") -- falling back to static test image, as in Goal 1";
      LogMessage(logMsg.str(), false);
   }
}

/**
 * Goal 3: starts real event-driven acquisition. Only called from
 * Initialize() when ConnectToCamera() has already set cameraConnected_ and
 * sensorWidth_/sensorHeight_. Sizes both frame buffers and the event
 * accumulator to the real sensor geometry, registers the CD callback,
 * starts the frame-builder thread, then starts the camera itself -- in that
 * order, so no events can arrive before there's somewhere for them to go.
 */
void CProphEBSCamera::StartEventStreaming()
{
   imgBufferA_.Resize(sensorWidth_, sensorHeight_, 1);
   imgBufferB_.Resize(sensorWidth_, sensorHeight_, 1);
   frontImg_ = &imgBufferA_;
   backImg_ = &imgBufferB_;

   eventCounts_.assign(static_cast<size_t>(sensorWidth_) * sensorHeight_, 0);

   cdCallbackId_ = cam_.cd().add_callback(
      [this](const Metavision::EventCD* begin, const Metavision::EventCD* end)
      {
         OnEventsCD(begin, end);
      });

   frameBuilderThd_->Start(g_EventIntegrationMs);

   cam_.start();
   streaming_ = true;

   std::ostringstream startMsg;
   startMsg << "ProphEBS: event streaming started, integrating " << g_EventIntegrationMs << " ms per frame";
   LogMessage(startMsg.str(), false);
}

/**
 * Stops event streaming and undoes StartEventStreaming(), in reverse order:
 * stop the camera first (no more events can arrive), then the frame-builder
 * thread, then unregister the callback. Safe to call even if streaming was
 * never started (e.g. no camera connected) or already stopped.
 */
void CProphEBSCamera::StopEventStreaming()
{
   if (!streaming_)
      return;

   cam_.stop();
   frameBuilderThd_->Stop();
   cam_.cd().remove_callback(cdCallbackId_);
   streaming_ = false;
}

/**
 * Called by the Metavision SDK on its own internal thread for each decoded
 * batch of CD events. Kept minimal -- just increments per-pixel counts under
 * eventCountsLock_ -- since this runs on the hot path for however many
 * events/sec the sensor is producing; all actual frame rendering happens
 * later, in BuildAndSwapFrame(), off this thread.
 */
void CProphEBSCamera::OnEventsCD(const Metavision::EventCD* begin, const Metavision::EventCD* end)
{
   MMThreadGuard g(eventCountsLock_);
   for (const Metavision::EventCD* ev = begin; ev != end; ++ev)
   {
      if (ev->x < sensorWidth_ && ev->y < sensorHeight_)
      {
         uint32_t& count = eventCounts_[static_cast<size_t>(ev->y) * sensorWidth_ + ev->x];
         if (count < UINT32_MAX)
            count++;
      }
   }
}

/**
 * Called by ProphEBSFrameBuilderThread every g_EventIntegrationMs. Snapshots
 * and resets the event-count accumulator (briefly locking eventCountsLock_,
 * which OnEventsCD() also briefly locks per event batch), renders those
 * counts into backImg_ as an 8-bit grayscale frame, then swaps front/back
 * under frontImgLock_ so subsequent GetImageBuffer()/InsertImage() calls
 * return the newly-built frame. backImg_ (the old frontImg_) is only
 * written again on the next call to this function, one integration window
 * later, so readers of frontImg_ never see a partially-written buffer.
 */
void CProphEBSCamera::BuildAndSwapFrame()
{
   std::vector<uint32_t> counts(static_cast<size_t>(sensorWidth_) * sensorHeight_, 0);
   {
      MMThreadGuard g(eventCountsLock_);
      counts.swap(eventCounts_);
      eventCounts_.assign(static_cast<size_t>(sensorWidth_) * sensorHeight_, 0);
   }

   unsigned char* pixels = backImg_->GetPixelsRW();
   for (size_t i = 0; i < counts.size(); i++)
   {
      unsigned value = counts[i] * g_EventIntensityScale;
      pixels[i] = static_cast<unsigned char>(value > 255 ? 255 : value);
   }

   {
      MMThreadGuard g(frontImgLock_);
      std::swap(frontImg_, backImg_);
   }
}

/**
 * Builds <folder>\ProphEBS_<timestamp>.raw, creating the folder if needed.
 * <folder> is EBS-TempRecordingFolder if the user has set it, else
 * Documents\ProphEBS_Recordings under the current user's profile (falling
 * back to a relative "ProphEBS_Recordings" if USERPROFILE isn't set
 * either). This is always the actual recording destination when
 * EBS-RawFilePath is empty and MDA-folder discovery didn't resolve a
 * direct destination at start time (see StartRawRecordingIfRequested()),
 * and remains the final location if StopRawRecordingIfActive()'s later
 * MDA-folder discovery/move also fails. This folder is always safe to
 * use: it always exists by the time cam_.start_recording() is called
 * (unlike an MDA's own save folder, which may not exist yet at that point
 * -- see the Goal 4 "doesn't get stored anywhere" incident in
 * docs/DEVLOG.md).
 */
std::string CProphEBSCamera::GenerateAutoRawFilePath() const
{
   namespace fs = std::filesystem;

   char tempFolderBuf[MM::MaxStrLength];
   GetProperty(g_PropTempFolder, tempFolderBuf);
   std::string tempFolder = tempFolderBuf;

   fs::path folder;
   if (!tempFolder.empty())
   {
      folder = fs::path(tempFolder);
   }
   else
   {
      const char* userProfile = std::getenv("USERPROFILE");
      if (userProfile != nullptr && userProfile[0] != '\0')
         folder = fs::path(userProfile) / "Documents" / "ProphEBS_Recordings";
      else
         folder = fs::path("ProphEBS_Recordings");
   }

   std::error_code ec;
   fs::create_directories(folder, ec);

   std::time_t now = std::time(nullptr);
   std::tm tmBuf;
   localtime_s(&tmBuf, &now);
   char nameBuf[64];
   std::strftime(nameBuf, sizeof(nameBuf), "ProphEBS_%Y%m%d_%H%M%S.raw", &tmBuf);

   return (folder / nameBuf).string();
}

/**
 * Called from StartSequenceAcquisition() only for a finite (MDA-style)
 * sequence on a connected camera. Resolves the recording path in priority
 * order:
 *   1. EBS-RawFilePath, if set explicitly -- used verbatim, no move at the
 *      end.
 *   2. MM's own per-run numbered save subfolder, if EBS-RawFilePath is
 *      empty, MDA root/prefix auto-discovery succeeds, "Save images" is
 *      checked, and the numbered subfolder appears within a short retry
 *      window -- streamed straight there, no local staging or move needed
 *      at all (the CoreLog-based lookup this relies on is synchronously
 *      fresh well before this point -- MM logs it when the acquisition
 *      engine starts, comfortably before it gets around to calling this
 *      device's StartSequenceAcquisition()).
 *   3. A local, guaranteed-safe staging path (GenerateAutoRawFilePath())
 *      otherwise -- e.g. discovery failed, "Save images" was unchecked (no
 *      subfolder will ever appear for this run), or MM's subfolder simply
 *      didn't appear in time. StopRawRecordingIfActive() gets a second,
 *      later chance to discover and move it once the whole acquisition
 *      has finished, which is a much wider window than this one.
 * Never fails the acquisition itself -- a bad path or SDK error is
 * reported via EBS-RawRecordingStatus and the CoreLog, not as an error
 * return, since the live image feed must keep working either way.
 */
void CProphEBSCamera::StartRawRecordingIfRequested()
{
   if (!cameraConnected_)
      return;

   char pathBuf[MM::MaxStrLength];
   GetProperty(g_PropRawFilePath, pathBuf);
   std::string explicitPath = pathBuf;

   std::string path;
   movePendingToMdaFolder_ = false;

   if (!explicitPath.empty())
   {
      path = explicitPath;
   }
   else
   {
      std::string mdaRoot, mdaPrefix;
      bool mdaSave = false;
      if (TryDiscoverMdaRootPrefix(mdaRoot, mdaPrefix, mdaSave) && mdaSave)
      {
         // Modest retry budget here -- MM has just started the acquisition
         // engine moments ago and may still be setting up its own save
         // folder for this run.
         path = ComputeNumberedMdaDestination(mdaRoot, mdaPrefix, 4, 150);
         if (!path.empty())
            LogMessage("ProphEBS: streaming directly to the Multi-D Acquisition save location -> "
               + path, false);
      }

      if (path.empty())
      {
         path = GenerateAutoRawFilePath();
         movePendingToMdaFolder_ = true;
      }
   }

   try
   {
      if (!cam_.start_recording(path))
         throw std::runtime_error("start_recording() returned false");

      rawRecordingActive_ = true;
      currentRawFilePath_ = path;
      std::string status = "Recording to " + path;
      SetProperty(g_PropRawRecordingStatus, status.c_str());
      OnPropertyChanged(g_PropRawRecordingStatus, status.c_str());
      LogMessage("ProphEBS: raw event recording started -> " + path, false);
   }
   catch (const std::exception& e)
   {
      rawRecordingActive_ = false;
      currentRawFilePath_.clear();
      movePendingToMdaFolder_ = false;
      std::string msg = std::string("Failed to start raw recording: ") + e.what();
      SetProperty(g_PropRawRecordingStatus, msg.c_str());
      OnPropertyChanged(g_PropRawRecordingStatus, msg.c_str());
      LogMessage("ProphEBS: " + msg, false);
   }
}

/**
 * Undoes StartRawRecordingIfRequested(). No-op if no recording is active
 * (safe to call unconditionally from StopSequenceAcquisition(), Shutdown(),
 * and ProphEBSSequenceThread::svc()'s natural-completion path). If the
 * recording was staged locally (EBS-RawFilePath was empty at start), this is
 * also where MDA-path auto-discovery actually happens -- as late as
 * possible, right after the Metavision SDK has finished closing the file --
 * and if it resolves to a real root/prefix, the finished file is moved
 * (renamed, or copied+deleted across drives) there. If discovery fails, or
 * the recording used an explicit EBS-RawFilePath, the file is left exactly
 * where it was recorded.
 */
void CProphEBSCamera::StopRawRecordingIfActive()
{
   if (!rawRecordingActive_)
      return;

   try
   {
      cam_.stop_recording(currentRawFilePath_);
   }
   catch (const std::exception& e)
   {
      LogMessage(std::string("ProphEBS: error stopping raw recording: ") + e.what(), false);
   }

   std::string finalPath = currentRawFilePath_;

   if (movePendingToMdaFolder_)
   {
      std::string mdaRoot, mdaPrefix;
      bool mdaSave = false;
      if (TryDiscoverMdaRootPrefix(mdaRoot, mdaPrefix, mdaSave) && mdaSave)
      {
         namespace fs = std::filesystem;
         std::error_code ec;
         fs::create_directories(mdaRoot, ec);

         // MM writes each MDA run into its own auto-numbered subfolder
         // ("<prefix>_<N>", never overwriting previous runs); find the one
         // it just created for *this* run (see FindHighestNumberedMdaSubfolder()
         // for why "highest existing number" reliably means "this run").
         // A short retry allows for MM still finishing its own folder
         // creation at the very instant this runs -- by design this fires
         // right as the acquisition ends, so normally no retry is needed.
         fs::path dest = ComputeNumberedMdaDestination(mdaRoot, mdaPrefix, 4, 150);
         if (dest.empty())
         {
            // No numbered subfolder ever appeared -- likely a save mode
            // that doesn't use one. Fall back to the flat layout directly
            // under mdaRoot.
            dest = fs::path(mdaRoot) / (mdaPrefix + "_prophesee_events.raw");
            fs::create_directories(mdaRoot, ec);
         }

         auto moveOne = [](const fs::path& from, const fs::path& to) -> std::error_code
         {
            std::error_code moveEc;
            fs::rename(from, to, moveEc);
            if (moveEc)
            {
               // rename() fails with an error across drives/volumes -- fall
               // back to copy + delete, which works in that case too.
               moveEc.clear();
               fs::copy_file(from, to, fs::copy_options::overwrite_existing, moveEc);
               if (!moveEc)
               {
                  std::error_code removeEc;
                  fs::remove(from, removeEc);
               }
            }
            return moveEc;
         };

         ec = moveOne(currentRawFilePath_, dest);

         if (!ec)
         {
            finalPath = dest.string();
            LogMessage("ProphEBS: moved raw recording to the Multi-D Acquisition save location -> "
               + finalPath, false);

            // The Metavision SDK also writes a companion <stem>.bias file
            // (a snapshot of the sensor's bias settings) next to the .raw
            // at recording time -- move it alongside so it doesn't get
            // orphaned in the local staging folder.
            fs::path srcBias = fs::path(currentRawFilePath_).replace_extension(".bias");
            std::error_code biasEc;
            if (fs::exists(srcBias, biasEc))
            {
               fs::path destBias = dest;
               destBias.replace_extension(".bias");
               std::error_code moveBiasEc = moveOne(srcBias, destBias);
               if (moveBiasEc)
                  LogMessage("ProphEBS: could not move companion .bias file (" +
                     moveBiasEc.message() + ") -- leaving it at " + srcBias.string(), false);
            }
         }
         else
         {
            LogMessage("ProphEBS: raw recording finished at " + currentRawFilePath_ +
               " but could not be moved to the Multi-D Acquisition save location (" +
               ec.message() + ") -- leaving it in place", false);
         }
      }
      else
      {
         LogMessage("ProphEBS: could not auto-discover the Multi-D Acquisition save location, or "
            "\"Save images\" was unchecked for this acquisition -- leaving the recording at "
            + currentRawFilePath_, false);
      }
   }

   std::string status = "Finished: " + finalPath;
   SetProperty(g_PropRawRecordingStatus, status.c_str());
   OnPropertyChanged(g_PropRawRecordingStatus, status.c_str());
   LogMessage("ProphEBS: raw event recording stopped -> " + finalPath, false);
   rawRecordingActive_ = false;
   currentRawFilePath_.clear();
   movePendingToMdaFolder_ = false;
}

/**
 * Fills frontImg_ with a fixed checkerboard + gradient pattern. Generated
 * once (not per-Snap) since the no-hardware fallback has no real data source
 * -- every Snap simply returns the same buffer. Only used when
 * cameraConnected_ is false; StartEventStreaming() takes over frontImg_/
 * backImg_ otherwise.
 */
void CProphEBSCamera::GenerateTestImage()
{
   frontImg_->Resize(g_TestImageWidth, g_TestImageHeight, 1);
   unsigned char* pixels = frontImg_->GetPixelsRW();

   const unsigned checkerSize = 32;
   for (unsigned y = 0; y < g_TestImageHeight; y++)
   {
      for (unsigned x = 0; x < g_TestImageWidth; x++)
      {
         bool checker = ((x / checkerSize) + (y / checkerSize)) % 2 == 0;
         unsigned char gradient = static_cast<unsigned char>((x * 255) / (g_TestImageWidth - 1));
         pixels[y * g_TestImageWidth + x] = checker ? gradient : static_cast<unsigned char>(255 - gradient);
      }
   }
}

int CProphEBSCamera::SnapImage()
{
   // No shutter to trigger -- frontImg_ already holds either the static test
   // pattern (no hardware) or the most recently built event-integration
   // frame (real camera streaming continuously via the frame-builder
   // thread); either way this is a no-op.
   return DEVICE_OK;
}

const unsigned char* CProphEBSCamera::GetImageBuffer()
{
   MMThreadGuard g(frontImgLock_);
   return frontImg_->GetPixels();
}

unsigned CProphEBSCamera::GetImageWidth() const
{
   MMThreadGuard g(frontImgLock_);
   return frontImg_->Width();
}

unsigned CProphEBSCamera::GetImageHeight() const
{
   MMThreadGuard g(frontImgLock_);
   return frontImg_->Height();
}

unsigned CProphEBSCamera::GetImageBytesPerPixel() const
{
   MMThreadGuard g(frontImgLock_);
   return frontImg_->Depth();
}

unsigned CProphEBSCamera::GetBitDepth() const
{
   return 8;
}

long CProphEBSCamera::GetImageBufferSize() const
{
   MMThreadGuard g(frontImgLock_);
   return frontImg_->Width() * frontImg_->Height() * frontImg_->Depth();
}

double CProphEBSCamera::GetExposure() const
{
   char buf[MM::MaxStrLength];
   int ret = GetProperty(MM::g_Keyword_Exposure, buf);
   if (ret != DEVICE_OK)
      return 0.0;
   return atof(buf);
}

void CProphEBSCamera::SetExposure(double exp_ms)
{
   SetProperty(MM::g_Keyword_Exposure, CDeviceUtils::ConvertToString(exp_ms));
   exposure_ms_ = exp_ms;
}

int CProphEBSCamera::SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
{
   if (xSize == 0 || ySize == 0)
      return ClearROI();

   roiX_ = x;
   roiY_ = y;
   roiXSize_ = xSize;
   roiYSize_ = ySize;
   return DEVICE_OK;
}

int CProphEBSCamera::GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize)
{
   x = roiX_;
   y = roiY_;
   xSize = roiXSize_;
   ySize = roiYSize_;
   return DEVICE_OK;
}

int CProphEBSCamera::ClearROI()
{
   roiX_ = 0;
   roiY_ = 0;
   roiXSize_ = sensorWidth_;
   roiYSize_ = sensorHeight_;
   return DEVICE_OK;
}

int CProphEBSCamera::GetBinning() const
{
   return 1;
}

int CProphEBSCamera::SetBinning(int binSize)
{
   // Binning isn't meaningful yet (no real sensor) -- only 1x1 is accepted.
   return (binSize == 1) ? DEVICE_OK : DEVICE_UNSUPPORTED_COMMAND;
}

int CProphEBSCamera::IsExposureSequenceable(bool& isSequenceable) const
{
   isSequenceable = false;
   return DEVICE_OK;
}

int CProphEBSCamera::StartSequenceAcquisition(double interval_ms)
{
   return StartSequenceAcquisition(LONG_MAX, interval_ms, false);
}

int CProphEBSCamera::StartSequenceAcquisition(long numImages, double interval_ms, bool /*stopOnOverflow*/)
{
   if (IsCapturing())
      return DEVICE_CAMERA_BUSY_ACQUIRING;

   int ret = GetCoreCallback()->PrepareForAcq(this);
   if (ret != DEVICE_OK)
      return ret;

   // Goal 4: only finite sequences (Multi-D Acquisition) trigger raw
   // recording, not MicroManager's Live view -- Live calls the
   // StartSequenceAcquisition(double) overload above, which passes
   // LONG_MAX here for an unbounded stream. Recording a .raw file for every
   // Live session would be surprising and unbounded disk usage; a
   // known-length MDA is the "record a multi-dimensional acquisition"
   // requirement from the roadmap.
   if (numImages != LONG_MAX)
      StartRawRecordingIfRequested();

   sequenceStartTime_ = GetCurrentMMTime();
   thd_->Start(numImages, interval_ms);
   return DEVICE_OK;
}

int CProphEBSCamera::StopSequenceAcquisition()
{
   if (!thd_->IsStopped())
   {
      thd_->Stop();
   }
   StopRawRecordingIfActive();
   return DEVICE_OK;
}

bool CProphEBSCamera::IsCapturing()
{
   return !thd_->IsStopped();
}

int CProphEBSCamera::InsertImage()
{
   MM::MMTime timeStamp = GetCurrentMMTime();
   char label[MM::MaxStrLength];
   GetLabel(label);

   MM::CameraImageMetadata md;
   md.AddTag(MM::g_Keyword_Metadata_CameraLabel, label);
   std::string elapsed = CDeviceUtils::ConvertToString((timeStamp - sequenceStartTime_).getMsec());
   md.AddTag(MM::g_Keyword_Elapsed_Time_ms, elapsed);

   // Snapshot frontImg_'s pointer/dimensions under lock, then insert outside
   // the lock -- InsertImage() copies synchronously, and frontImg_'s memory
   // stays untouched until at least the next integration window (see
   // BuildAndSwapFrame()), so this is safe without holding the lock across
   // the copy.
   const unsigned char* pixels;
   unsigned width, height, bytesPerPixel;
   {
      MMThreadGuard g(frontImgLock_);
      pixels = frontImg_->GetPixels();
      width = frontImg_->Width();
      height = frontImg_->Height();
      bytesPerPixel = frontImg_->Depth();
   }

   return GetCoreCallback()->InsertImage(this, pixels, width, height,
      bytesPerPixel, GetNumberOfComponents(), md.Serialize());
}

///////////////////////////////////////////////////////////////////////////////
// ProphEBSSequenceThread implementation
///////////////////////////////////////////////////////////////////////////////

ProphEBSSequenceThread::ProphEBSSequenceThread(CProphEBSCamera* pCamera) :
   camera_(pCamera),
   intervalMs_(100.0),
   numImages_(1),
   imageCounter_(0),
   stop_(true)
{
}

ProphEBSSequenceThread::~ProphEBSSequenceThread()
{
}

void ProphEBSSequenceThread::Start(long numImages, double intervalMs)
{
   numImages_ = numImages;
   intervalMs_ = intervalMs;
   imageCounter_ = 0;
   {
      MMThreadGuard g(stopLock_);
      stop_ = false;
   }
   activate();
}

void ProphEBSSequenceThread::Stop()
{
   {
      MMThreadGuard g(stopLock_);
      stop_ = true;
   }
   wait();
}

bool ProphEBSSequenceThread::IsStopped()
{
   MMThreadGuard g(stopLock_);
   return stop_;
}

int ProphEBSSequenceThread::svc()
{
   int ret = DEVICE_ERR;
   try
   {
      do
      {
         ret = camera_->InsertImage();
         if (ret != DEVICE_OK)
            break;
         CDeviceUtils::SleepMs(static_cast<long>(std::max(1.0, intervalMs_)));
      } while (!IsStopped() && ++imageCounter_ < numImages_);
   }
   catch (...)
   {
      camera_->LogMessage("Exception in ProphEBSSequenceThread::svc", false);
   }

   {
      MMThreadGuard g(stopLock_);
      stop_ = true;
   }
   // Goal 4: a finite (MDA-style) acquisition reaching its numImages_ count
   // ends the loop here, not via StopSequenceAcquisition() -- stop any raw
   // recording that StartSequenceAcquisition() started so the .raw file gets
   // finalized even when nothing external calls Stop.
   camera_->StopRawRecordingIfActive();
   camera_->GetCoreCallback()->AcqFinished(camera_, 0);
   return ret;
}

///////////////////////////////////////////////////////////////////////////////
// ProphEBSFrameBuilderThread implementation
///////////////////////////////////////////////////////////////////////////////

ProphEBSFrameBuilderThread::ProphEBSFrameBuilderThread(CProphEBSCamera* pCamera) :
   camera_(pCamera),
   intervalMs_(g_EventIntegrationMs),
   stop_(true)
{
}

ProphEBSFrameBuilderThread::~ProphEBSFrameBuilderThread()
{
}

void ProphEBSFrameBuilderThread::Start(double intervalMs)
{
   intervalMs_ = intervalMs;
   {
      MMThreadGuard g(stopLock_);
      stop_ = false;
   }
   activate();
}

void ProphEBSFrameBuilderThread::Stop()
{
   {
      MMThreadGuard g(stopLock_);
      stop_ = true;
   }
   wait();
}

bool ProphEBSFrameBuilderThread::IsStopped()
{
   MMThreadGuard g(stopLock_);
   return stop_;
}

int ProphEBSFrameBuilderThread::svc()
{
   try
   {
      while (!IsStopped())
      {
         CDeviceUtils::SleepMs(static_cast<long>(std::max(1.0, intervalMs_)));
         if (!IsStopped())
            camera_->BuildAndSwapFrame();
      }
   }
   catch (...)
   {
      camera_->LogMessage("Exception in ProphEBSFrameBuilderThread::svc", false);
   }
   return DEVICE_OK;
}
