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
#include <metavision/hal/facilities/i_monitoring.h>
#include <metavision/hal/utils/device_config.h>

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
#include <set>
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

// Goal 5 property names.
const char* g_PropBiasRangeCheckBypass = "EBS-BiasRangeCheckBypass";

const char* const g_FallbackBiasNames[6] = {
   "bias_diff", "bias_diff_off", "bias_diff_on", "bias_fo", "bias_hpf", "bias_refr"
};

const char* g_PropErcEnabled = "EBS-ERC-Enabled";
const char* g_PropErcEventRate = "EBS-ERC-EventRate";

const char* g_PropEventTrailFilterEnabled = "EBS-EventTrailFilter-Enabled";
const char* g_PropEventTrailFilterThreshold = "EBS-EventTrailFilter-Threshold";
const char* g_PropEventTrailFilterMode = "EBS-EventTrailFilter-Mode";

const char* g_PropAntiFlickerEnabled = "EBS-AntiFlicker-Enabled";
const char* g_PropAntiFlickerStartThreshold = "EBS-AntiFlicker-StartThreshold";
const char* g_PropAntiFlickerStopThreshold = "EBS-AntiFlicker-StopThreshold";
const char* g_PropAntiFlickerDutyCycle = "EBS-AntiFlicker-DutyCycle";
const char* g_PropAntiFlickerFilterType = "EBS-AntiFlicker-FilterType";
const char* g_PropAntiFlickerLowFreq = "EBS-AntiFlicker-LowFreq";
const char* g_PropAntiFlickerHighFreq = "EBS-AntiFlicker-HighFreq";

const char* g_PropGeneration = "EBS-Generation";
const char* g_PropDataEncodingFormat = "EBS-DataEncodingFormat";

const char* g_PropAvgDataRate = "EBS-AvgDataRate-MBps";
const char* g_PropAvgEventRate = "EBS-AvgEventRate-MEvps";
const char* g_PropAvgErcDropRate = "EBS-AvgERCDropRate-KEvps";
const char* g_PropTemperature = "EBS-Temperature-C";
const char* g_PropIllumination = "EBS-Illumination-lux";
const char* g_PropPixelDeadTime = "EBS-PixelDeadTime-us";

// Filter-type string values for EBS-AntiFlicker-FilterType. Metavision
// Studio's UI calls Metavision::I_AntiFlickerModule::AntiFlickerMode::
// BAND_STOP "Band Cut" and BAND_PASS "Band Pass" -- these strings match that
// UI wording rather than the SDK's own enum names, since that's the
// terminology the user asked for.
const char* const g_AntiFlickerFilterTypeBandPass = "Band Pass";
const char* const g_AntiFlickerFilterTypeBandCut = "Band Cut";

// Event-trail-filter Type <-> string conversions (Metavision::
// I_EventTrailFilterModule::Type has no built-in stream operator).
const char* EventTrailFilterTypeToString(Metavision::I_EventTrailFilterModule::Type type)
{
   switch (type)
   {
      case Metavision::I_EventTrailFilterModule::Type::TRAIL: return "TRAIL";
      case Metavision::I_EventTrailFilterModule::Type::STC_CUT_TRAIL: return "STC_CUT_TRAIL";
      case Metavision::I_EventTrailFilterModule::Type::STC_KEEP_TRAIL: return "STC_KEEP_TRAIL";
   }
   return "TRAIL";
}

bool EventTrailFilterTypeFromString(const std::string& s, Metavision::I_EventTrailFilterModule::Type& outType)
{
   if (s == "TRAIL") { outType = Metavision::I_EventTrailFilterModule::Type::TRAIL; return true; }
   if (s == "STC_CUT_TRAIL") { outType = Metavision::I_EventTrailFilterModule::Type::STC_CUT_TRAIL; return true; }
   if (s == "STC_KEEP_TRAIL") { outType = Metavision::I_EventTrailFilterModule::Type::STC_KEEP_TRAIL; return true; }
   return false;
}

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
   movePendingToMdaFolder_(false),
   localErcEnabled_(false),
   localErcEventRate_(50000000),
   localEventTrailFilterEnabled_(false),
   localEventTrailFilterThreshold_(10000),
   localEventTrailFilterMode_("TRAIL"),
   localAntiFlickerEnabled_(false),
   localAntiFlickerStartThreshold_(6),
   localAntiFlickerStopThreshold_(4),
   localAntiFlickerDutyCycle_(50.0),
   localAntiFlickerFilterType_(g_AntiFlickerFilterTypeBandCut),
   localAntiFlickerLowFreq_(50),
   localAntiFlickerHighFreq_(60),
   generation_("N/A"),
   dataEncodingFormat_("N/A"),
   totalRawBytes_(0),
   totalEventCount_(0),
   rawDataCallbackId_(),
   statsThd_(nullptr),
   avgDataRateMBps_(0.0),
   avgEventRateMEvps_(0.0),
   avgErcDropRateKEvps_(0.0),
   temperatureC_(0.0),
   illuminationLux_(0.0),
   pixelDeadTimeUs_(0.0)
{
   InitializeDefaultErrorMessages();
   thd_ = new ProphEBSSequenceThread(this);
   frameBuilderThd_ = new ProphEBSFrameBuilderThread(this);
   statsThd_ = new ProphEBSStatsThread(this);

   for (const char* name : g_FallbackBiasNames)
      localBiasValues_[name] = 0;

   // Goal 5: pre-init-only property -- must be created in the constructor
   // (not Initialize()) and with isPreInitProperty=true so MM enforces it
   // read-only once the device is initialized. See g_PropBiasRangeCheckBypass
   // for what it does.
   CreateStringProperty(g_PropBiasRangeCheckBypass, "Off", false, nullptr, true);
   AddAllowedValue(g_PropBiasRangeCheckBypass, "Off");
   AddAllowedValue(g_PropBiasRangeCheckBypass, "On");
}

CProphEBSCamera::~CProphEBSCamera()
{
   StopSequenceAcquisition();
   StopEventStreaming();
   delete thd_;
   delete frameBuilderThd_;
   delete statsThd_;
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

   // Goal 5: static read-only info, alongside the Goal 2 identification
   // properties above.
   nRet = CreateStringProperty(g_PropGeneration, generation_.c_str(), true);
   if (DEVICE_OK != nRet)
      return nRet;
   nRet = CreateStringProperty(g_PropDataEncodingFormat, dataEncodingFormat_.c_str(), true);
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

   // Goal 5: hardware-setting properties (biases, ERC, event trail filter,
   // anti-flicker) and the live read-only monitoring properties (data/event
   // rate, ERC drop-rate estimate, temperature, illumination, pixel dead
   // time).
   CreateBiasProperties();
   CreateErcProperties();
   CreateEventTrailFilterProperties();
   CreateAntiFlickerProperties();

   // Goal 5: live-stats properties are read-only, backed by the
   // avgDataRateMBps_ etc. atomics UpdateStats() writes -- see
   // g_PropAvgDataRate for why these can't be safely pushed via
   // SetProperty() from ProphEBSStatsThread's background thread. Each gets
   // its own CPropertyAction instance -- see CreateBiasProperties() for why
   // sharing one across properties isn't safe (double-free on shutdown).
   for (const char* prop : { g_PropAvgDataRate, g_PropAvgEventRate, g_PropAvgErcDropRate,
           g_PropTemperature, g_PropIllumination, g_PropPixelDeadTime })
   {
      nRet = CreateFloatProperty(prop, 0.0, true, new CPropertyAction(this, &CProphEBSCamera::OnStat));
      if (DEVICE_OK != nRet)
         return nRet;
   }

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
      // Goal 5: EBS-BiasRangeCheckBypass is a pre-init property (see the
      // constructor), so it's safe to read here -- it can no longer change
      // once Initialize()/ConnectToCamera() runs. Mirrors Metavision
      // Studio's "bypass biases range check" checkbox.
      char bypassBuf[MM::MaxStrLength];
      GetProperty(g_PropBiasRangeCheckBypass, bypassBuf);
      Metavision::DeviceConfig deviceConfig;
      deviceConfig.enable_biases_range_check_bypass(std::string(bypassBuf) == "On");

      cam_ = Metavision::Camera::from_first_available(deviceConfig);

      // Throws CameraException (caught below) if the facility isn't
      // available -- every real Prophesee device registers it, so this only
      // trips for unusual/unsupported hardware.
      Metavision::I_HW_Identification& hwId = cam_.get_facility<Metavision::I_HW_Identification>();

      cameraSerial_ = hwId.get_serial();
      connectionType_ = hwId.get_connection_type();
      integrator_ = hwId.get_integrator();
      dataEncodingFormat_ = hwId.get_current_data_encoding_format();

      Metavision::I_HW_Identification::SensorInfo sensorInfo = hwId.get_sensor_info();
      std::ostringstream modelStream;
      modelStream << sensorInfo.name_ << " (Gen " << sensorInfo.major_version_
                  << "." << sensorInfo.minor_version_ << ")";
      cameraModel_ = modelStream.str();

      std::ostringstream genStream;
      genStream << sensorInfo.major_version_ << "." << sensorInfo.minor_version_;
      generation_ = genStream.str();

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
 * Goal 5: creates one Integer property per bias. If cameraConnected_,
 * enumerates the real biases the sensor reports via
 * Metavision::I_LL_Biases::get_all_biases(), setting each property's range
 * from LL_Bias_Info::get_bias_range() and its default to the value already
 * on the sensor (i.e. whatever it booted with -- its own hardware default,
 * no separate "load defaults" step needed). If not connected, falls back to
 * g_FallbackBiasNames so the properties the user asked for still exist,
 * defaulting to 0 with no range limit and backed by localBiasValues_
 * instead of hardware.
 */
void CProphEBSCamera::CreateBiasProperties()
{
   biasNames_.clear();

   // Each MM::Property owns (and deletes) its own ActionFunctor -- sharing
   // one CPropertyAction pointer across multiple CreateProperty() calls
   // causes a double-free when the properties are destroyed (found via
   // self-test: an intermittent crash on device shutdown, root-caused by
   // reading MMDevice/Property.h's Property::~Property(), which calls
   // `delete fpAction_` unconditionally per property). A fresh `new
   // CPropertyAction(...)` is required per property, same pattern
   // DemoCamera's OnTestProperty loop already uses for the same reason.

   if (cameraConnected_)
   {
      try
      {
         Metavision::I_LL_Biases& biases = cam_.get_facility<Metavision::I_LL_Biases>();
         std::map<std::string, int> allBiases = biases.get_all_biases();
         for (const auto& kv : allBiases)
         {
            biasNames_.push_back(kv.first);
            std::string propName = std::string("EBS-") + kv.first;
            CreateIntegerProperty(propName.c_str(), kv.second, false,
               new CPropertyAction(this, &CProphEBSCamera::OnBias));

            Metavision::LL_Bias_Info info;
            if (biases.get_bias_info(kv.first, info))
            {
               std::pair<int, int> range = info.get_bias_range();
               SetPropertyLimits(propName.c_str(), range.first, range.second);
            }
         }
         return;
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: could not read biases from hardware (") + e.what() +
            ") -- falling back to the default bias name list", false);
      }
   }

   for (const char* name : g_FallbackBiasNames)
   {
      biasNames_.push_back(name);
      std::string propName = std::string("EBS-") + name;
      CreateIntegerProperty(propName.c_str(), 0, false, new CPropertyAction(this, &CProphEBSCamera::OnBias));
   }
}

/**
 * Goal 5: shared handler for every bias property (both the real,
 * hardware-backed ones and the local fallback ones). Looks up which bias by
 * stripping the "EBS-" prefix off the property's own name rather than
 * needing one handler per bias -- the actual set of biases isn't known
 * until CreateBiasProperties() runs (it's fetched from the camera).
 */
int CProphEBSCamera::OnBias(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   std::string propName = pProp->GetName();
   const std::string prefix = "EBS-";
   std::string biasName = (propName.rfind(prefix, 0) == 0) ? propName.substr(prefix.size()) : propName;

   if (cameraConnected_)
   {
      try
      {
         Metavision::I_LL_Biases& biases = cam_.get_facility<Metavision::I_LL_Biases>();
         if (eAct == MM::BeforeGet)
         {
            pProp->Set(static_cast<long>(biases.get(biasName)));
         }
         else if (eAct == MM::AfterSet)
         {
            long value;
            pProp->Get(value);
            if (!biases.set(biasName, static_cast<int>(value)))
               LogMessage("ProphEBS: setting bias " + biasName + " to " + std::to_string(value) +
                  " was rejected by the hardware", false);
         }
      }
      catch (const std::exception& e)
      {
         LogMessage("ProphEBS: error accessing bias " + biasName + ": " + e.what(), false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
      {
         pProp->Set(static_cast<long>(localBiasValues_[biasName]));
      }
      else if (eAct == MM::AfterSet)
      {
         long value;
         pProp->Get(value);
         localBiasValues_[biasName] = value;
      }
   }
   return DEVICE_OK;
}

/**
 * Goal 5: creates the ERC (event rate control) properties. Range-limits
 * EBS-ERC-EventRate to the hardware-reported min/max when connected.
 */
void CProphEBSCamera::CreateErcProperties()
{
   CPropertyAction* pActEnabled = new CPropertyAction(this, &CProphEBSCamera::OnErcEnabled);
   CreateStringProperty(g_PropErcEnabled, "Off", false, pActEnabled);
   AddAllowedValue(g_PropErcEnabled, "Off");
   AddAllowedValue(g_PropErcEnabled, "On");

   CPropertyAction* pActRate = new CPropertyAction(this, &CProphEBSCamera::OnErcEventRate);
   CreateIntegerProperty(g_PropErcEventRate, localErcEventRate_, false, pActRate);

   if (cameraConnected_)
   {
      try
      {
         Metavision::I_ErcModule& erc = cam_.get_facility<Metavision::I_ErcModule>();
         SetPropertyLimits(g_PropErcEventRate, erc.get_min_supported_cd_event_rate(),
            erc.get_max_supported_cd_event_rate());
         // Apply the requested default (50 Mev/s) to the hardware now, so
         // EBS-ERC-EventRate reads back what's actually configured even
         // before the user touches the property.
         erc.set_cd_event_rate(static_cast<uint32_t>(localErcEventRate_));
         erc.enable(false);
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: could not initialize ERC settings: ") + e.what(), false);
      }
   }
}

int CProphEBSCamera::OnErcEnabled(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_ErcModule& erc = cam_.get_facility<Metavision::I_ErcModule>();
         if (eAct == MM::BeforeGet)
            pProp->Set(erc.is_enabled() ? "On" : "Off");
         else if (eAct == MM::AfterSet)
         {
            std::string value;
            pProp->Get(value);
            erc.enable(value == "On");
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing ERC enable state: ") + e.what(), false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localErcEnabled_ ? "On" : "Off");
      else if (eAct == MM::AfterSet)
      {
         std::string value;
         pProp->Get(value);
         localErcEnabled_ = (value == "On");
      }
   }
   return DEVICE_OK;
}

int CProphEBSCamera::OnErcEventRate(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_ErcModule& erc = cam_.get_facility<Metavision::I_ErcModule>();
         if (eAct == MM::BeforeGet)
         {
            pProp->Set(static_cast<long>(erc.get_cd_event_rate()));
         }
         else if (eAct == MM::AfterSet)
         {
            long value;
            pProp->Get(value);
            erc.set_cd_event_rate(static_cast<uint32_t>(value));
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing ERC event rate: ") + e.what(), false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localErcEventRate_);
      else if (eAct == MM::AfterSet)
         pProp->Get(localErcEventRate_);
   }
   return DEVICE_OK;
}

/**
 * Goal 5: creates the event trail (STC) filter properties. Threshold
 * default is 10 ms, expressed in the microsecond units the SDK uses.
 * EBS-EventTrailFilter-Mode's allowed values are restricted to
 * get_available_types() when connected -- not every sensor supports all
 * three filter types.
 */
void CProphEBSCamera::CreateEventTrailFilterProperties()
{
   CPropertyAction* pActEnabled = new CPropertyAction(this, &CProphEBSCamera::OnEventTrailFilterEnabled);
   CreateStringProperty(g_PropEventTrailFilterEnabled, "Off", false, pActEnabled);
   AddAllowedValue(g_PropEventTrailFilterEnabled, "Off");
   AddAllowedValue(g_PropEventTrailFilterEnabled, "On");

   CPropertyAction* pActThreshold = new CPropertyAction(this, &CProphEBSCamera::OnEventTrailFilterThreshold);
   CreateIntegerProperty(g_PropEventTrailFilterThreshold, localEventTrailFilterThreshold_, false, pActThreshold);

   CPropertyAction* pActMode = new CPropertyAction(this, &CProphEBSCamera::OnEventTrailFilterMode);
   CreateStringProperty(g_PropEventTrailFilterMode, localEventTrailFilterMode_.c_str(), false, pActMode);

   if (cameraConnected_)
   {
      try
      {
         Metavision::I_EventTrailFilterModule& filter =
            cam_.get_facility<Metavision::I_EventTrailFilterModule>();
         std::set<Metavision::I_EventTrailFilterModule::Type> available = filter.get_available_types();
         for (const auto& type : available)
            AddAllowedValue(g_PropEventTrailFilterMode, EventTrailFilterTypeToString(type));

         SetPropertyLimits(g_PropEventTrailFilterThreshold, filter.get_min_supported_threshold(),
            filter.get_max_supported_threshold());

         filter.set_threshold(static_cast<uint32_t>(localEventTrailFilterThreshold_));
         if (available.count(Metavision::I_EventTrailFilterModule::Type::TRAIL))
            filter.set_type(Metavision::I_EventTrailFilterModule::Type::TRAIL);
         filter.enable(false);
         return;
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: could not initialize event trail filter settings: ") + e.what(),
            false);
      }
   }

   AddAllowedValue(g_PropEventTrailFilterMode, "TRAIL");
   AddAllowedValue(g_PropEventTrailFilterMode, "STC_CUT_TRAIL");
   AddAllowedValue(g_PropEventTrailFilterMode, "STC_KEEP_TRAIL");
}

int CProphEBSCamera::OnEventTrailFilterEnabled(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_EventTrailFilterModule& filter =
            cam_.get_facility<Metavision::I_EventTrailFilterModule>();
         if (eAct == MM::BeforeGet)
            pProp->Set(filter.is_enabled() ? "On" : "Off");
         else if (eAct == MM::AfterSet)
         {
            std::string value;
            pProp->Get(value);
            filter.enable(value == "On");
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing event trail filter enable state: ") + e.what(),
            false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localEventTrailFilterEnabled_ ? "On" : "Off");
      else if (eAct == MM::AfterSet)
      {
         std::string value;
         pProp->Get(value);
         localEventTrailFilterEnabled_ = (value == "On");
      }
   }
   return DEVICE_OK;
}

int CProphEBSCamera::OnEventTrailFilterThreshold(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_EventTrailFilterModule& filter =
            cam_.get_facility<Metavision::I_EventTrailFilterModule>();
         if (eAct == MM::BeforeGet)
         {
            pProp->Set(static_cast<long>(filter.get_threshold()));
         }
         else if (eAct == MM::AfterSet)
         {
            long value;
            pProp->Get(value);
            filter.set_threshold(static_cast<uint32_t>(value));
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing event trail filter threshold: ") + e.what(),
            false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localEventTrailFilterThreshold_);
      else if (eAct == MM::AfterSet)
         pProp->Get(localEventTrailFilterThreshold_);
   }
   return DEVICE_OK;
}

int CProphEBSCamera::OnEventTrailFilterMode(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_EventTrailFilterModule& filter =
            cam_.get_facility<Metavision::I_EventTrailFilterModule>();
         if (eAct == MM::BeforeGet)
         {
            pProp->Set(EventTrailFilterTypeToString(filter.get_type()));
         }
         else if (eAct == MM::AfterSet)
         {
            std::string value;
            pProp->Get(value);
            Metavision::I_EventTrailFilterModule::Type type;
            if (EventTrailFilterTypeFromString(value, type))
               filter.set_type(type);
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing event trail filter mode: ") + e.what(), false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localEventTrailFilterMode_.c_str());
      else if (eAct == MM::AfterSet)
         pProp->Get(localEventTrailFilterMode_);
   }
   return DEVICE_OK;
}

/**
 * Goal 5: creates the anti-flicker properties, including the mandatory
 * frequency band (not in the original request, but required for the filter
 * to do anything -- added per user decision, defaulting to 50/60 Hz mains
 * hum). EBS-AntiFlicker-FilterType uses Metavision Studio's "Band Pass"/
 * "Band Cut" wording rather than the SDK's own BAND_PASS/BAND_STOP enum
 * names -- see g_AntiFlickerFilterTypeBandCut.
 */
void CProphEBSCamera::CreateAntiFlickerProperties()
{
   CPropertyAction* pActEnabled = new CPropertyAction(this, &CProphEBSCamera::OnAntiFlickerEnabled);
   CreateStringProperty(g_PropAntiFlickerEnabled, "Off", false, pActEnabled);
   AddAllowedValue(g_PropAntiFlickerEnabled, "Off");
   AddAllowedValue(g_PropAntiFlickerEnabled, "On");

   CPropertyAction* pActStart = new CPropertyAction(this, &CProphEBSCamera::OnAntiFlickerStartThreshold);
   CreateIntegerProperty(g_PropAntiFlickerStartThreshold, localAntiFlickerStartThreshold_, false, pActStart);

   CPropertyAction* pActStop = new CPropertyAction(this, &CProphEBSCamera::OnAntiFlickerStopThreshold);
   CreateIntegerProperty(g_PropAntiFlickerStopThreshold, localAntiFlickerStopThreshold_, false, pActStop);

   CPropertyAction* pActDuty = new CPropertyAction(this, &CProphEBSCamera::OnAntiFlickerDutyCycle);
   CreateFloatProperty(g_PropAntiFlickerDutyCycle, localAntiFlickerDutyCycle_, false, pActDuty);

   CPropertyAction* pActType = new CPropertyAction(this, &CProphEBSCamera::OnAntiFlickerFilterType);
   CreateStringProperty(g_PropAntiFlickerFilterType, localAntiFlickerFilterType_.c_str(), false, pActType);
   AddAllowedValue(g_PropAntiFlickerFilterType, g_AntiFlickerFilterTypeBandPass);
   AddAllowedValue(g_PropAntiFlickerFilterType, g_AntiFlickerFilterTypeBandCut);

   CPropertyAction* pActLow = new CPropertyAction(this, &CProphEBSCamera::OnAntiFlickerLowFreq);
   CreateIntegerProperty(g_PropAntiFlickerLowFreq, localAntiFlickerLowFreq_, false, pActLow);

   CPropertyAction* pActHigh = new CPropertyAction(this, &CProphEBSCamera::OnAntiFlickerHighFreq);
   CreateIntegerProperty(g_PropAntiFlickerHighFreq, localAntiFlickerHighFreq_, false, pActHigh);

   if (cameraConnected_)
   {
      try
      {
         Metavision::I_AntiFlickerModule& af = cam_.get_facility<Metavision::I_AntiFlickerModule>();
         SetPropertyLimits(g_PropAntiFlickerStartThreshold, af.get_min_supported_start_threshold(),
            af.get_max_supported_start_threshold());
         SetPropertyLimits(g_PropAntiFlickerStopThreshold, af.get_min_supported_stop_threshold(),
            af.get_max_supported_stop_threshold());
         SetPropertyLimits(g_PropAntiFlickerDutyCycle, af.get_min_supported_duty_cycle(),
            af.get_max_supported_duty_cycle());
         // Goal 5 bug fix: LowFreq/HighFreq had no range limits set, so a
         // value outside the hardware-supported band was silently rejected
         // by set_frequency_band() (bool return value, not checked) --
         // discovered via self-test: setting LowFreq=45 on this sensor
         // (min supported frequency is 50 Hz) left the property reading
         // back the old default instead of erroring visibly. Setting
         // limits here means MM itself rejects out-of-range values before
         // they ever reach the hardware.
         SetPropertyLimits(g_PropAntiFlickerLowFreq, af.get_min_supported_frequency(),
            af.get_max_supported_frequency());
         SetPropertyLimits(g_PropAntiFlickerHighFreq, af.get_min_supported_frequency(),
            af.get_max_supported_frequency());

         if (!af.set_frequency_band(static_cast<uint32_t>(localAntiFlickerLowFreq_),
            static_cast<uint32_t>(localAntiFlickerHighFreq_)))
            LogMessage("ProphEBS: default anti-flicker frequency band (50/60 Hz) was rejected by the "
               "hardware -- check EBS-AntiFlicker-LowFreq/HighFreq's actual supported range", false);
         af.set_filtering_mode(Metavision::I_AntiFlickerModule::BAND_STOP);
         af.set_duty_cycle(static_cast<float>(localAntiFlickerDutyCycle_));
         af.set_start_threshold(static_cast<uint32_t>(localAntiFlickerStartThreshold_));
         af.set_stop_threshold(static_cast<uint32_t>(localAntiFlickerStopThreshold_));
         af.enable(false);
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: could not initialize anti-flicker settings: ") + e.what(), false);
      }
   }
}

int CProphEBSCamera::OnAntiFlickerEnabled(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_AntiFlickerModule& af = cam_.get_facility<Metavision::I_AntiFlickerModule>();
         if (eAct == MM::BeforeGet)
            pProp->Set(af.is_enabled() ? "On" : "Off");
         else if (eAct == MM::AfterSet)
         {
            std::string value;
            pProp->Get(value);
            af.enable(value == "On");
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing anti-flicker enable state: ") + e.what(), false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localAntiFlickerEnabled_ ? "On" : "Off");
      else if (eAct == MM::AfterSet)
      {
         std::string value;
         pProp->Get(value);
         localAntiFlickerEnabled_ = (value == "On");
      }
   }
   return DEVICE_OK;
}

int CProphEBSCamera::OnAntiFlickerStartThreshold(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_AntiFlickerModule& af = cam_.get_facility<Metavision::I_AntiFlickerModule>();
         if (eAct == MM::BeforeGet)
         {
            pProp->Set(static_cast<long>(af.get_start_threshold()));
         }
         else if (eAct == MM::AfterSet)
         {
            long value;
            pProp->Get(value);
            af.set_start_threshold(static_cast<uint32_t>(value));
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing anti-flicker start threshold: ") + e.what(),
            false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localAntiFlickerStartThreshold_);
      else if (eAct == MM::AfterSet)
         pProp->Get(localAntiFlickerStartThreshold_);
   }
   return DEVICE_OK;
}

int CProphEBSCamera::OnAntiFlickerStopThreshold(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_AntiFlickerModule& af = cam_.get_facility<Metavision::I_AntiFlickerModule>();
         if (eAct == MM::BeforeGet)
         {
            pProp->Set(static_cast<long>(af.get_stop_threshold()));
         }
         else if (eAct == MM::AfterSet)
         {
            long value;
            pProp->Get(value);
            af.set_stop_threshold(static_cast<uint32_t>(value));
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing anti-flicker stop threshold: ") + e.what(),
            false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localAntiFlickerStopThreshold_);
      else if (eAct == MM::AfterSet)
         pProp->Get(localAntiFlickerStopThreshold_);
   }
   return DEVICE_OK;
}

int CProphEBSCamera::OnAntiFlickerDutyCycle(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_AntiFlickerModule& af = cam_.get_facility<Metavision::I_AntiFlickerModule>();
         if (eAct == MM::BeforeGet)
         {
            pProp->Set(static_cast<double>(af.get_duty_cycle()));
         }
         else if (eAct == MM::AfterSet)
         {
            double value;
            pProp->Get(value);
            af.set_duty_cycle(static_cast<float>(value));
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing anti-flicker duty cycle: ") + e.what(), false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localAntiFlickerDutyCycle_);
      else if (eAct == MM::AfterSet)
         pProp->Get(localAntiFlickerDutyCycle_);
   }
   return DEVICE_OK;
}

int CProphEBSCamera::OnAntiFlickerFilterType(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_AntiFlickerModule& af = cam_.get_facility<Metavision::I_AntiFlickerModule>();
         if (eAct == MM::BeforeGet)
         {
            pProp->Set(af.get_filtering_mode() == Metavision::I_AntiFlickerModule::BAND_PASS
               ? g_AntiFlickerFilterTypeBandPass : g_AntiFlickerFilterTypeBandCut);
         }
         else if (eAct == MM::AfterSet)
         {
            std::string value;
            pProp->Get(value);
            af.set_filtering_mode(value == g_AntiFlickerFilterTypeBandPass
               ? Metavision::I_AntiFlickerModule::BAND_PASS : Metavision::I_AntiFlickerModule::BAND_STOP);
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing anti-flicker filter type: ") + e.what(), false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localAntiFlickerFilterType_.c_str());
      else if (eAct == MM::AfterSet)
         pProp->Get(localAntiFlickerFilterType_);
   }
   return DEVICE_OK;
}

int CProphEBSCamera::OnAntiFlickerLowFreq(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_AntiFlickerModule& af = cam_.get_facility<Metavision::I_AntiFlickerModule>();
         if (eAct == MM::BeforeGet)
         {
            pProp->Set(static_cast<long>(af.get_band_low_frequency()));
         }
         else if (eAct == MM::AfterSet)
         {
            long value;
            pProp->Get(value);
            long highValue = localAntiFlickerHighFreq_;
            GetProperty(g_PropAntiFlickerHighFreq, highValue);
            if (!af.set_frequency_band(static_cast<uint32_t>(value), static_cast<uint32_t>(highValue)))
               LogMessage("ProphEBS: anti-flicker frequency band (" + std::to_string(value) + "/" +
                  std::to_string(highValue) + " Hz) rejected by hardware", false);
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing anti-flicker low frequency: ") + e.what(), false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localAntiFlickerLowFreq_);
      else if (eAct == MM::AfterSet)
         pProp->Get(localAntiFlickerLowFreq_);
   }
   return DEVICE_OK;
}

int CProphEBSCamera::OnAntiFlickerHighFreq(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (cameraConnected_)
   {
      try
      {
         Metavision::I_AntiFlickerModule& af = cam_.get_facility<Metavision::I_AntiFlickerModule>();
         if (eAct == MM::BeforeGet)
         {
            pProp->Set(static_cast<long>(af.get_band_high_frequency()));
         }
         else if (eAct == MM::AfterSet)
         {
            long value;
            pProp->Get(value);
            long lowValue = localAntiFlickerLowFreq_;
            GetProperty(g_PropAntiFlickerLowFreq, lowValue);
            if (!af.set_frequency_band(static_cast<uint32_t>(lowValue), static_cast<uint32_t>(value)))
               LogMessage("ProphEBS: anti-flicker frequency band (" + std::to_string(lowValue) + "/" +
                  std::to_string(value) + " Hz) rejected by hardware", false);
         }
      }
      catch (const std::exception& e)
      {
         LogMessage(std::string("ProphEBS: error accessing anti-flicker high frequency: ") + e.what(),
            false);
      }
   }
   else
   {
      if (eAct == MM::BeforeGet)
         pProp->Set(localAntiFlickerHighFreq_);
      else if (eAct == MM::AfterSet)
         pProp->Get(localAntiFlickerHighFreq_);
   }
   return DEVICE_OK;
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
   totalRawBytes_ = 0;
   totalEventCount_ = 0;

   cdCallbackId_ = cam_.cd().add_callback(
      [this](const Metavision::EventCD* begin, const Metavision::EventCD* end)
      {
         OnEventsCD(begin, end);
      });

   // Goal 5: raw-byte counter for EBS-AvgDataRate-MBps -- cheap, single
   // atomic add per buffer, same "off the hot path" design as OnEventsCD().
   rawDataCallbackId_ = cam_.raw_data().add_callback(
      [this](const uint8_t* /*data*/, size_t size)
      {
         totalRawBytes_ += size;
      });

   frameBuilderThd_->Start(g_EventIntegrationMs);
   statsThd_->Start(g_StatsIntervalMs);

   cam_.start();
   streaming_ = true;

   std::ostringstream startMsg;
   startMsg << "ProphEBS: event streaming started, integrating " << g_EventIntegrationMs << " ms per frame";
   LogMessage(startMsg.str(), false);
}

/**
 * Stops event streaming and undoes StartEventStreaming(), in reverse order:
 * stop the camera first (no more events can arrive), then the frame-builder/
 * stats threads, then unregister the callbacks. Safe to call even if
 * streaming was never started (e.g. no camera connected) or already
 * stopped.
 */
void CProphEBSCamera::StopEventStreaming()
{
   if (!streaming_)
      return;

   cam_.stop();
   frameBuilderThd_->Stop();
   statsThd_->Stop();
   cam_.cd().remove_callback(cdCallbackId_);
   cam_.raw_data().remove_callback(rawDataCallbackId_);
   streaming_ = false;
}

/**
 * Called by the Metavision SDK on its own internal thread for each decoded
 * batch of CD events. Kept minimal -- just increments per-pixel counts under
 * eventCountsLock_, plus a single atomic add to totalEventCount_ for
 * EBS-AvgEventRate-MEvps -- since this runs on the hot path for however many
 * events/sec the sensor is producing; all actual frame rendering happens
 * later, in BuildAndSwapFrame(), off this thread.
 */
void CProphEBSCamera::OnEventsCD(const Metavision::EventCD* begin, const Metavision::EventCD* end)
{
   totalEventCount_ += static_cast<uint64_t>(end - begin);

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
 * Goal 5: called every g_StatsIntervalMs by ProphEBSStatsThread while
 * streaming_. Reads-and-resets totalRawBytes_/totalEventCount_ to compute
 * average data rate (MB/s) and event rate (MEv/s) over the interval,
 * estimates the ERC drop rate (target minus measured, per user decision --
 * there's no hardware-reported drop count, see docs/DEVLOG.md), and polls
 * I_Monitoring for temperature/illumination/pixel dead time. Writes only to
 * the avgDataRateMBps_ etc. std::atomic<double> members -- never touches
 * the MM property system directly (see g_PropAvgDataRate for why: a
 * background thread calling SetProperty() isn't safe against the main
 * thread's own concurrent property access). OnStat() reads these atomics
 * back synchronously when MMCore actually queries a property.
 * I_Monitoring's three readings are polled independently (separate
 * try/catch each) since this sensor throws on get_illumination() but not
 * get_temperature()/get_pixel_dead_time() -- one failing metric shouldn't
 * suppress the others.
 */
void CProphEBSCamera::UpdateStats()
{
   if (!cameraConnected_)
      return;

   uint64_t bytes = totalRawBytes_.exchange(0);
   uint64_t events = totalEventCount_.exchange(0);
   double seconds = g_StatsIntervalMs / 1000.0;

   avgDataRateMBps_ = (static_cast<double>(bytes) / seconds) / (1024.0 * 1024.0);
   avgEventRateMEvps_ = (static_cast<double>(events) / seconds) / 1.0e6;

   try
   {
      Metavision::I_ErcModule& erc = cam_.get_facility<Metavision::I_ErcModule>();
      double dropRateKEvps = 0.0;
      if (erc.is_enabled())
      {
         double targetEvps = static_cast<double>(erc.get_cd_event_rate());
         double measuredEvps = static_cast<double>(events) / seconds;
         double dropEvps = targetEvps - measuredEvps;
         if (dropEvps > 0.0)
            dropRateKEvps = dropEvps / 1000.0;
      }
      avgErcDropRateKEvps_ = dropRateKEvps;
   }
   catch (const std::exception&)
   {
      // ERC not available on this sensor -- leave the estimate at 0.
   }

   try
   {
      Metavision::I_Monitoring& mon = cam_.get_facility<Metavision::I_Monitoring>();

      try { temperatureC_ = static_cast<double>(mon.get_temperature()); }
      catch (const std::exception&) { /* leave at last good value */ }

      try { illuminationLux_ = static_cast<double>(mon.get_illumination()); }
      catch (const std::exception&) { /* leave at last good value */ }

      try { pixelDeadTimeUs_ = static_cast<double>(mon.get_pixel_dead_time()); }
      catch (const std::exception&) { /* leave at last good value */ }
   }
   catch (const std::exception&)
   {
      // I_Monitoring facility itself not available on this sensor.
   }

   // Push the freshly-updated values into MMCore's stateCache_ so the
   // Device/Property Browser actually live-updates -- BeforeGet/OnStat()
   // alone only take effect when something else prompts MMCore to call
   // GetProperty() (e.g. the user changing an unrelated property), which is
   // what "doesn't update until I touch another property" looked like
   // before this was added. This is the *singular* per-property
   // OnPropertyChanged(name, value), not SetProperty() -- unlike
   // SetProperty() (which mutates this device's own PropertyCollection,
   // the cause of the crash fixed above), OnPropertyChanged() only touches
   // MMCore's separately-locked stateCache_ (see
   // CoreCallback::OnPropertyChanged()'s own std::lock_guard), so it's
   // safe to call from this background thread.
   OnPropertyChanged(g_PropAvgDataRate, CDeviceUtils::ConvertToString(avgDataRateMBps_.load()));
   OnPropertyChanged(g_PropAvgEventRate, CDeviceUtils::ConvertToString(avgEventRateMEvps_.load()));
   OnPropertyChanged(g_PropAvgErcDropRate, CDeviceUtils::ConvertToString(avgErcDropRateKEvps_.load()));
   OnPropertyChanged(g_PropTemperature, CDeviceUtils::ConvertToString(temperatureC_.load()));
   OnPropertyChanged(g_PropIllumination, CDeviceUtils::ConvertToString(illuminationLux_.load()));
   OnPropertyChanged(g_PropPixelDeadTime, CDeviceUtils::ConvertToString(pixelDeadTimeUs_.load()));
}

/**
 * Goal 5: shared BeforeGet-only handler for the six live-stats properties
 * -- just reads back whichever atomic UpdateStats() last wrote, matched by
 * property name. These are read-only properties (no AfterSet case).
 */
int CProphEBSCamera::OnStat(MM::PropertyBase* pProp, MM::ActionType eAct)
{
   if (eAct != MM::BeforeGet)
      return DEVICE_OK;

   std::string propName = pProp->GetName();
   if (propName == g_PropAvgDataRate)
      pProp->Set(avgDataRateMBps_.load());
   else if (propName == g_PropAvgEventRate)
      pProp->Set(avgEventRateMEvps_.load());
   else if (propName == g_PropAvgErcDropRate)
      pProp->Set(avgErcDropRateKEvps_.load());
   else if (propName == g_PropTemperature)
      pProp->Set(temperatureC_.load());
   else if (propName == g_PropIllumination)
      pProp->Set(illuminationLux_.load());
   else if (propName == g_PropPixelDeadTime)
      pProp->Set(pixelDeadTimeUs_.load());

   return DEVICE_OK;
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

///////////////////////////////////////////////////////////////////////////////
// ProphEBSStatsThread implementation
///////////////////////////////////////////////////////////////////////////////

ProphEBSStatsThread::ProphEBSStatsThread(CProphEBSCamera* pCamera) :
   camera_(pCamera),
   intervalMs_(g_StatsIntervalMs),
   stop_(true)
{
}

ProphEBSStatsThread::~ProphEBSStatsThread()
{
}

void ProphEBSStatsThread::Start(double intervalMs)
{
   intervalMs_ = intervalMs;
   {
      MMThreadGuard g(stopLock_);
      stop_ = false;
   }
   activate();
}

void ProphEBSStatsThread::Stop()
{
   {
      MMThreadGuard g(stopLock_);
      stop_ = true;
   }
   wait();
}

bool ProphEBSStatsThread::IsStopped()
{
   MMThreadGuard g(stopLock_);
   return stop_;
}

int ProphEBSStatsThread::svc()
{
   try
   {
      while (!IsStopped())
      {
         CDeviceUtils::SleepMs(static_cast<long>(std::max(1.0, intervalMs_)));
         if (!IsStopped())
            camera_->UpdateStats();
      }
   }
   catch (...)
   {
      camera_->LogMessage("Exception in ProphEBSStatsThread::svc", false);
   }
   return DEVICE_OK;
}
