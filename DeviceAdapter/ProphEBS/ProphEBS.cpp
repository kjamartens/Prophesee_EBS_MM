///////////////////////////////////////////////////////////////////////////////
// FILE:          ProphEBS.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Device adapter for the Prophesee EBS camera (Goals 1-3).
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
   integrator_("N/A"),
   sensorWidth_(g_TestImageWidth),
   sensorHeight_(g_TestImageHeight),
   frontImg_(&imgBufferA_),
   backImg_(&imgBufferB_),
   streaming_(false),
   cdCallbackId_(),
   frameBuilderThd_(nullptr)
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
      "Prophesee EBS Camera adapter (Goal 3: minimal event-integration video feed)", true);
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
