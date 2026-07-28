///////////////////////////////////////////////////////////////////////////////
// FILE:          ProphEBS.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Device adapter for the Prophesee EBS camera (Goal 1 + 2).
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

#include <metavision/hal/facilities/i_hw_identification.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <sstream>

// Name used by MicroManager to refer to this device, and to load it from the
// "mmgr_dal_ProphEBS.dll" library (see ProphEBSModule.cpp).
const char* g_ProphEBSCameraDeviceName = "ProphEBS-Camera";

// Goal 2 read-only property names.
const char* g_PropConnectionStatus = "EBS-ConnectionStatus";
const char* g_PropModel = "EBS-Model";
const char* g_PropSerial = "EBS-Serial";
const char* g_PropConnectionType = "EBS-ConnectionType";
const char* g_PropIntegrator = "EBS-Integrator";

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
   integrator_("N/A")
{
   InitializeDefaultErrorMessages();
   thd_ = new ProphEBSSequenceThread(this);
}

CProphEBSCamera::~CProphEBSCamera()
{
   StopSequenceAcquisition();
   delete thd_;
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
      "Prophesee EBS Camera adapter (Goal 2: EBS connection + identification)", true);
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

   GenerateTestImage();

   initialized_ = true;
   return DEVICE_OK;
}

int CProphEBSCamera::Shutdown()
{
   if (cameraConnected_)
   {
      // Camera was never start()-ed (no event streaming yet in Goal 2), so
      // there is nothing to stop -- just release the HAL device handle by
      // replacing cam_ with a fresh, unopened Camera.
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

      cameraConnected_ = true;
      connectionStatus_ = "Connected";

      std::ostringstream logMsg;
      logMsg << "ProphEBS: connected to " << cameraModel_ << ", serial=" << cameraSerial_
             << ", connection=" << connectionType_;
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
 * Fills img_ with a fixed checkerboard + gradient pattern. Generated once
 * (not per-Snap) since Goal 1 has no real data source -- every Snap simply
 * returns the same buffer. Goal 3 replaces this with the actual event
 * integration frame.
 */
void CProphEBSCamera::GenerateTestImage()
{
   img_.Resize(g_TestImageWidth, g_TestImageHeight, 1);
   unsigned char* pixels = img_.GetPixelsRW();

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
   // No hardware to trigger yet -- the buffer already holds the static test
   // pattern generated in Initialize().
   return DEVICE_OK;
}

const unsigned char* CProphEBSCamera::GetImageBuffer()
{
   return img_.GetPixels();
}

unsigned CProphEBSCamera::GetImageWidth() const
{
   return img_.Width();
}

unsigned CProphEBSCamera::GetImageHeight() const
{
   return img_.Height();
}

unsigned CProphEBSCamera::GetImageBytesPerPixel() const
{
   return img_.Depth();
}

unsigned CProphEBSCamera::GetBitDepth() const
{
   return 8;
}

long CProphEBSCamera::GetImageBufferSize() const
{
   return img_.Width() * img_.Height() * img_.Depth();
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
   roiXSize_ = g_TestImageWidth;
   roiYSize_ = g_TestImageHeight;
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

   return GetCoreCallback()->InsertImage(this, img_.GetPixels(), GetImageWidth(), GetImageHeight(),
      GetImageBytesPerPixel(), GetNumberOfComponents(), md.Serialize());
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
   camera_->GetCoreCallback()->AcqFinished(camera_, 0);
   return ret;
}
