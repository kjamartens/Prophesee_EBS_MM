///////////////////////////////////////////////////////////////////////////////
// FILE:          ProphEBS.h
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Prophesee event-based sensor (EBS) camera device adapter.
//
//                Goal 1 (barebones): proved that a custom MicroManager camera
//                DeviceAdapter can be built, loaded, and used from
//                MicroManager (Hardware Configuration Wizard, Live view,
//                Snap) with no real hardware -- SnapImage() always returned a
//                static test pattern.
//
//                Goal 2 (this revision) adds real Metavision SDK connectivity:
//                Initialize() now tries to open the first available Prophesee
//                camera via Metavision::Camera::from_first_available() and,
//                if successful, reads back its model/serial/connection type
//                via the HAL's I_HW_Identification facility and exposes them
//                as read-only MM properties. If no camera is found (or the
//                Metavision SDK/driver isn't installed), Initialize() does
//                NOT fail -- it logs why and falls back to the Goal 1 static
//                test image, so the adapter still loads and is testable on a
//                machine with no EBS attached. SnapImage() still returns the
//                static test pattern; real event-driven frames are Goal 3.
//
//                Goal 3 (this revision) replaces the static test image, when
//                a real EBS is connected, with an actual event-integration
//                frame: CD (contrast detection) events are accumulated into a
//                per-pixel counter, and every g_EventIntegrationMs
//                milliseconds that accumulator is rendered into an 8-bit
//                grayscale frame and swapped in as the buffer SnapImage()/
//                Live view return. With no camera connected, the Goal 1
//                static checkerboard fallback is unchanged.
//
//                Goal 4 (this revision) adds recording: when a finite
//                (multi-D-acquisition-style) sequence acquisition starts on a
//                connected camera, the adapter calls the Metavision SDK's own
//                cam_.start_recording()/stop_recording() to write the real
//                Prophesee .raw event file alongside whatever image data
//                MicroManager itself saves -- see g_PropRawFilePath/
//                g_PropRawAutoPath/g_PropRawRecordingStatus below.
//
//                Tunable properties and ROI/pixel masking are added in later
//                goals (see docs/DEVLOG.md at the repository root for the
//                roadmap).
//
// COPYRIGHT:     Koen J.A. Martens, 2026
// LICENSE:       This file is distributed under the BSD license, consistent
//                with the rest of the Micro-Manager device adapter kit.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#pragma once

#include "DeviceBase.h"
#include "DeviceThreads.h"
#include "ImgBuffer.h"

#include <metavision/sdk/stream/camera.h>
#include <metavision/hal/facilities/i_antiflicker_module.h>
#include <metavision/hal/facilities/i_erc_module.h>
#include <metavision/hal/facilities/i_event_trail_filter_module.h>
#include <metavision/hal/facilities/i_ll_biases.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Device name (defined in ProphEBS.cpp, referenced by ProphEBSModule.cpp)
extern const char* g_ProphEBSCameraDeviceName;

// Read-only MM property names used to surface Goal 2 connection info.
extern const char* g_PropConnectionStatus;
extern const char* g_PropModel;
extern const char* g_PropSerial;
extern const char* g_PropConnectionType;
extern const char* g_PropIntegrator;

// Goal 4: raw event-file recording properties.
// - g_PropRawFilePath: manual override. Empty (the default) means "figure
//   out the path automatically" -- see StopRawRecordingIfActive() in
//   ProphEBS.cpp, which reads this process's own MicroManager CoreLog file
//   (Studio logs the exact MDA settings there, live, right before every
//   acquisition) to discover the Multi-D Acquisition dialog's *current*
//   save root/prefix, with no MM scripting or user action required at all.
//   Falls back to MM's on-close-only UserProfile JSON, and then to an
//   auto-generated path under Documents\ProphEBS_Recordings, if that lookup
//   fails for any reason -- recording never blocks on this. Setting this
//   property to a non-empty path skips auto-discovery and uses that path
//   verbatim instead, for the rare case where the automatic behavior isn't
//   wanted.
// - g_PropRawRecordingStatus: read-write status string the device itself
//   updates ("Not recording", "Recording to <path>", "Finished: <path>",
//   "Failed: <reason>") so progress/success is visible from the Device
//   Property Browser without digging through the CoreLog.
// - g_PropTempFolder: overrides the folder GenerateAutoRawFilePath() stages
//   into (or records to permanently, if MDA-folder discovery never
//   resolves). Empty (the default) keeps using
//   Documents\ProphEBS_Recordings.
extern const char* g_PropRawFilePath;
extern const char* g_PropRawRecordingStatus;
extern const char* g_PropTempFolder;

// Goal 5: pre-init-only property. Must be set (in the Hardware Configuration
// Wizard, or via loadDevice/before initializeDevice) before Initialize() --
// MM's PropertyCollection enforces pre-init properties as read-only once the
// device is initialized, which is exactly "settable during device setup
// only." Mirrors Metavision Studio's "bypass biases range check" checkbox
// (Metavision::DeviceConfig::enable_biases_range_check_bypass) -- widens
// every bias' allowed range from the recommended range to the full
// hardware-allowed range. Read once, in ConnectToCamera(), before opening
// the camera.
extern const char* g_PropBiasRangeCheckBypass;

// Goal 5: fallback bias names exposed (as local, non-hardware-backed
// properties defaulting to 0) when no camera is connected. When a camera
// *is* connected, the actual set of bias properties is fetched from
// Metavision::I_LL_Biases::get_all_biases() instead -- this list is only a
// fallback so the properties the user explicitly asked for still exist and
// are inspectable/settable (locally) without hardware attached.
extern const char* const g_FallbackBiasNames[6];

// Goal 5: ERC (event rate control) property names.
extern const char* g_PropErcEnabled;
extern const char* g_PropErcEventRate;

// Goal 5: event trail (STC) filter property names.
extern const char* g_PropEventTrailFilterEnabled;
extern const char* g_PropEventTrailFilterThreshold;
extern const char* g_PropEventTrailFilterMode;

// Goal 5: anti-flicker property names.
extern const char* g_PropAntiFlickerEnabled;
extern const char* g_PropAntiFlickerStartThreshold;
extern const char* g_PropAntiFlickerStopThreshold;
extern const char* g_PropAntiFlickerDutyCycle;
extern const char* g_PropAntiFlickerFilterType;
extern const char* g_PropAntiFlickerLowFreq;
extern const char* g_PropAntiFlickerHighFreq;

// Goal 5: static read-only info properties (beyond the Goal 2 ones).
extern const char* g_PropGeneration;
extern const char* g_PropDataEncodingFormat;

// Goal 5: live, periodically-refreshed read-only monitoring properties.
// Unlike g_PropRawRecordingStatus in Goal 4, these are NOT updated via
// SetProperty()/OnPropertyChanged() from ProphEBSStatsThread's background
// thread -- MM's PropertyCollection isn't documented/verified thread-safe
// against a background-thread SetProperty() racing the main thread's own
// property reads/writes (unlike Goal 4's status string, which only updates
// once per recording, this thread ticks every g_StatsIntervalMs for the
// entire time the camera is connected, so the same race that was
// theoretically possible in Goal 4 became practically hit here during
// self-testing (an intermittent crash, root-caused to this). Instead,
// UpdateStats() only writes to plain std::atomic<double> members, and each
// property is a normal read-only property with a CPropertyAction handler
// that reads the cached atomic in BeforeGet -- computed off the property
// system entirely, thread-safe by construction, no push needed.
extern const char* g_PropAvgDataRate;
extern const char* g_PropAvgEventRate;
extern const char* g_PropAvgErcDropRate;
extern const char* g_PropTemperature;
extern const char* g_PropIllumination;
extern const char* g_PropPixelDeadTime;

// Goal 5: stats refresh period for the new ProphEBSStatsThread.
const double g_StatsIntervalMs = 1000.0;

// Fixed test-image geometry for Goal 1 / the no-hardware fallback. Real
// sensor geometry (from Metavision::I_Geometry) is used instead whenever a
// camera is actually connected -- see CProphEBSCamera::sensorWidth_/
// sensorHeight_.
const unsigned g_TestImageWidth = 640;
const unsigned g_TestImageHeight = 480;

// Goal 3: fixed event-integration window. Every g_EventIntegrationMs
// milliseconds, the accumulated per-pixel CD event counts since the last
// window are rendered into a frame and swapped in as the live image. Not yet
// user-configurable -- that's Goal 6 ("custom view methods").
const double g_EventIntegrationMs = 100.0;

// Maps an accumulated per-pixel event count over one integration window to
// an 8-bit grayscale value (count * scale, clamped to 255). Chosen so a
// handful of events per pixel per 100 ms window is already visibly bright;
// not derived from any calibration, just a reasonable default for Goal 3.
const unsigned g_EventIntensityScale = 32;

class ProphEBSSequenceThread;
class ProphEBSFrameBuilderThread;
class ProphEBSStatsThread;

//////////////////////////////////////////////////////////////////////////////
// CProphEBSCamera class
//
// Minimal MM::CameraDevice implementation. See the MM::Camera pure-virtual
// list in MMDevice/MMDevice.h and MMDevice/DeviceBase.h (CCameraBase<U>) for
// exactly which methods below are required overrides versus which are
// already given sane defaults by CCameraBase (e.g. multi-ROI, exposure
// sequencing) and therefore intentionally not overridden here.
//////////////////////////////////////////////////////////////////////////////
class CProphEBSCamera : public CCameraBase<CProphEBSCamera>
{
public:
   CProphEBSCamera();
   ~CProphEBSCamera();

   // MMDevice API
   // ------------
   int Initialize();
   int Shutdown();
   void GetName(char* name) const;
   bool Busy();

   // MMCamera API
   // ------------
   int SnapImage();
   const unsigned char* GetImageBuffer();
   unsigned GetImageWidth() const;
   unsigned GetImageHeight() const;
   unsigned GetImageBytesPerPixel() const;
   unsigned GetBitDepth() const;
   long GetImageBufferSize() const;
   double GetExposure() const;
   void SetExposure(double exp_ms);
   int SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize);
   int GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize);
   int ClearROI();
   int GetBinning() const;
   int SetBinning(int binSize);
   int IsExposureSequenceable(bool& isSequenceable) const;

   // Continuous/sequence acquisition (used by MicroManager's Live view)
   int StartSequenceAcquisition(double interval_ms);
   int StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow);
   int StopSequenceAcquisition();
   bool IsCapturing();

   // Called by ProphEBSSequenceThread on its worker thread to push one frame
   // into the MMCore circular buffer.
   int InsertImage();

   // Goal 5: property action handlers. One shared OnBias() handles every
   // bias property (looks up which one via pProp->GetName()) rather than a
   // handler per bias, since the actual set of biases is fetched from the
   // camera at runtime (see ConnectToCamera()/CreateBiasProperties()) and
   // isn't known at compile time.
   int OnBias(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnErcEnabled(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnErcEventRate(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnEventTrailFilterEnabled(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnEventTrailFilterThreshold(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnEventTrailFilterMode(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnAntiFlickerEnabled(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnAntiFlickerStartThreshold(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnAntiFlickerStopThreshold(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnAntiFlickerDutyCycle(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnAntiFlickerFilterType(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnAntiFlickerLowFreq(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnAntiFlickerHighFreq(MM::PropertyBase* pProp, MM::ActionType eAct);

   // Goal 5: one shared read-only handler for all six live-stats
   // properties -- BeforeGet just reads the matching cached
   // std::atomic<double> (looked up via pProp->GetName(), same pattern as
   // OnBias()), no hardware/thread interaction at all on this call path.
   int OnStat(MM::PropertyBase* pProp, MM::ActionType eAct);

   // Goal 5: called every g_StatsIntervalMs by ProphEBSStatsThread while
   // streaming_ -- reads and resets the raw-byte/event-count atomics to
   // compute average data/event rate, estimates the ERC drop rate, and
   // polls I_Monitoring for temperature/illumination/pixel dead time.
   // Writes only to the std::atomic<double> stat cache members below, never
   // touches the MM property system -- see g_PropAvgDataRate for why.
   void UpdateStats();

private:
   void GenerateTestImage();

   // Tries Metavision::Camera::from_first_available(). Never throws and
   // never fails Initialize() -- on any error it logs the reason and leaves
   // cameraConnected_ false so the rest of Initialize() can fall back to
   // Goal 1 behavior (static test image, no hardware-derived properties).
   void ConnectToCamera();

   // Goal 5: creates one Integer property per bias. If cameraConnected_,
   // enumerates Metavision::I_LL_Biases::get_all_biases() and sets each
   // property's range from LL_Bias_Info::get_bias_range(); otherwise falls
   // back to g_FallbackBiasNames, defaulting each to 0 with no range limit
   // (there's no hardware to validate against). Called from Initialize().
   void CreateBiasProperties();

   // Goal 5: creates the ERC/event-trail-filter/anti-flicker properties.
   // Called from Initialize(), after ConnectToCamera(), so range-limited
   // properties (e.g. EBS-ERC-EventRate) can use the real hardware-reported
   // min/max when connected.
   void CreateErcProperties();
   void CreateEventTrailFilterProperties();
   void CreateAntiFlickerProperties();

   // Goal 3: starts real event-driven acquisition once ConnectToCamera() has
   // succeeded -- queries sensor geometry, sizes the frame buffers/event
   // accumulator, registers the CD callback, starts the frame-builder
   // thread, and finally starts the camera streaming.
   void StartEventStreaming();
   void StopEventStreaming();

   // CD event callback, invoked by the Metavision SDK on its own internal
   // thread whenever a batch of events has been decoded. Just accumulates
   // per-pixel counts under eventCountsLock_ -- all the actual frame
   // rendering happens in BuildAndSwapFrame(), off this hot path.
   void OnEventsCD(const Metavision::EventCD* begin, const Metavision::EventCD* end);

   // Called by ProphEBSFrameBuilderThread every g_EventIntegrationMs: snapshots
   // and resets the event-count accumulator, renders it into backImg_, then
   // swaps front/back so GetImageBuffer()/InsertImage() start returning the
   // newly-built frame. Readers of frontImg_ never see a partially-written
   // buffer because the frame builder only ever writes to backImg_.
   void BuildAndSwapFrame();

   // Goal 4: builds a local staging raw-file path (Documents\
   // ProphEBS_Recordings\ProphEBS_<timestamp>.raw), creating the folder if
   // needed. Used by StartRawRecordingIfRequested() as the actual recording
   // destination whenever EBS-RawFilePath is empty -- MDA-folder
   // auto-discovery deliberately happens later, in
   // StopRawRecordingIfActive(), not here (see that method's comment for
   // why) -- and remains the final location if that later discovery fails.
   std::string GenerateAutoRawFilePath() const;

   // Called from StartSequenceAcquisition() only for finite (MDA-style)
   // sequences with a connected camera -- resolves the recording path
   // (EBS-RawFilePath verbatim if set, else GenerateAutoRawFilePath() as a
   // local staging location -- MDA auto-discovery is intentionally deferred
   // to StopRawRecordingIfActive()), calls cam_.start_recording(), and
   // updates EBS-RawRecordingStatus either way. Never fails the acquisition
   // itself: a raw-recording failure is logged/reported via the status
   // property, not returned as an error, since the MM image feed must keep
   // working even if the Metavision-side recording can't start (e.g. a bad
   // path). Every SetProperty() on g_PropRawRecordingStatus here is paired
   // with an OnPropertyChanged() call -- SetProperty() alone only updates
   // this device's own internal property map, but MMCore keeps a separate
   // stateCache_ (what the Device/Property Browser actually displays) that
   // is only refreshed on a Core-initiated setProperty() or when the device
   // calls OnPropertyChanged() to report a self-initiated change. Without
   // that call the GUI shows a stale status (e.g. the previous recording's
   // path) even though a direct GetProperty()/getProperty() query already
   // returns the correct new value -- this bit us once already, see
   // docs/DEVLOG.md Goal 4 follow-up.
   void StartRawRecordingIfRequested();

   // Called from StopSequenceAcquisition() and from
   // ProphEBSSequenceThread::svc()'s natural-completion path (a finite MDA
   // sequence finishing on its own doesn't otherwise call
   // StopSequenceAcquisition()). No-op if no raw recording is active. If
   // movePendingToMdaFolder_ is set (recording was staged locally, i.e.
   // EBS-RawFilePath was empty), this is where MDA-folder auto-discovery
   // actually runs -- deliberately as late as possible, since MicroManager
   // doesn't necessarily flush its UserProfile JSON to disk the instant the
   // MDA dialog's fields change, so reading it here (after the whole
   // acquisition has run, not at its start) gives MicroManager's own save
   // timing far more time to catch up. Same SetProperty()+OnPropertyChanged()
   // pairing as StartRawRecordingIfRequested() above, for the same reason.
   void StopRawRecordingIfActive();

   bool initialized_;
   double exposure_ms_;
   unsigned roiX_;
   unsigned roiY_;
   unsigned roiXSize_;
   unsigned roiYSize_;
   MM::MMTime sequenceStartTime_;

   ProphEBSSequenceThread* thd_;
   friend class ProphEBSSequenceThread;

   // Goal 2: real EBS connection state. cam_ is default-constructed (not
   // connected to anything) until ConnectToCamera() succeeds.
   Metavision::Camera cam_;
   bool cameraConnected_;
   std::string connectionStatus_;
   std::string cameraModel_;
   std::string cameraSerial_;
   std::string connectionType_;
   std::string integrator_;

   // Goal 3: real sensor geometry (from Metavision::I_Geometry), valid only
   // when cameraConnected_ is true; otherwise the g_TestImageWidth/Height
   // fallback geometry applies instead.
   unsigned sensorWidth_;
   unsigned sensorHeight_;

   // Double-buffered frame: frontImg_ is what GetImageBuffer()/InsertImage()
   // return; backImg_ is what BuildAndSwapFrame() renders into before the
   // pointers are swapped under frontImgLock_. In the no-hardware fallback
   // path only frontImg_ (holding the static test pattern) is ever used.
   ImgBuffer imgBufferA_;
   ImgBuffer imgBufferB_;
   ImgBuffer* frontImg_;
   ImgBuffer* backImg_;
   mutable MMThreadLock frontImgLock_;

   // Per-pixel CD event counts accumulated since the last integration
   // window, written by OnEventsCD() (Metavision's callback thread) and
   // consumed/reset by BuildAndSwapFrame() (frame-builder thread).
   std::vector<uint32_t> eventCounts_;
   MMThreadLock eventCountsLock_;

   bool streaming_;
   Metavision::CallbackId cdCallbackId_;
   ProphEBSFrameBuilderThread* frameBuilderThd_;
   friend class ProphEBSFrameBuilderThread;

   // Goal 4: raw event-file recording state. rawRecordingActive_/
   // currentRawFilePath_/movePendingToMdaFolder_ are only meaningful between
   // a successful StartRawRecordingIfRequested() and the matching
   // StopRawRecordingIfActive(). movePendingToMdaFolder_ is true whenever
   // EBS-RawFilePath was empty at start time (i.e. currentRawFilePath_ is a
   // local staging path from GenerateAutoRawFilePath()) -- it tells
   // StopRawRecordingIfActive() whether to attempt the MDA-folder
   // auto-discovery + move at all, versus leaving an explicit
   // EBS-RawFilePath recording exactly where the user put it.
   bool rawRecordingActive_;
   std::string currentRawFilePath_;
   bool movePendingToMdaFolder_;

   // Goal 5: bias property state. When cameraConnected_, biasNames_ holds
   // the SDK-reported keys from I_LL_Biases::get_all_biases() (property
   // name is "EBS-" + key); otherwise it holds g_FallbackBiasNames and
   // localBiasValues_ is the backing store OnBias() reads/writes instead of
   // touching hardware.
   std::vector<std::string> biasNames_;
   std::map<std::string, long> localBiasValues_;

   // Goal 5: local fallback storage for ERC/event-trail-filter/anti-flicker
   // properties when no camera is connected -- mirrors the bias fallback
   // above so OnErcEnabled() etc. have somewhere to read/write without
   // touching cam_.
   bool localErcEnabled_;
   long localErcEventRate_;
   bool localEventTrailFilterEnabled_;
   long localEventTrailFilterThreshold_;
   std::string localEventTrailFilterMode_;
   bool localAntiFlickerEnabled_;
   long localAntiFlickerStartThreshold_;
   long localAntiFlickerStopThreshold_;
   double localAntiFlickerDutyCycle_;
   std::string localAntiFlickerFilterType_;
   long localAntiFlickerLowFreq_;
   long localAntiFlickerHighFreq_;

   // Goal 5: static read-only info, read once in ConnectToCamera().
   std::string generation_;
   std::string dataEncodingFormat_;

   // Goal 5: live-stats state. totalRawBytes_/totalEventCount_ are
   // incremented on the Metavision SDK's own callback threads (raw_data()
   // and cd(), respectively) and read-and-reset by UpdateStats() on
   // statsThd_'s thread -- std::atomic avoids needing a dedicated lock for
   // what's just a running total.
   std::atomic<uint64_t> totalRawBytes_;
   std::atomic<uint64_t> totalEventCount_;
   Metavision::CallbackId rawDataCallbackId_;
   ProphEBSStatsThread* statsThd_;
   friend class ProphEBSStatsThread;

   // Goal 5: cached stat values, written only by UpdateStats() (on
   // statsThd_'s thread) and read only by OnStat() (called synchronously by
   // MMCore on whatever thread is querying the property) -- std::atomic is
   // sufficient since each is an independent value with no cross-field
   // consistency requirement.
   std::atomic<double> avgDataRateMBps_;
   std::atomic<double> avgEventRateMEvps_;
   std::atomic<double> avgErcDropRateKEvps_;
   std::atomic<double> temperatureC_;
   std::atomic<double> illuminationLux_;
   std::atomic<double> pixelDeadTimeUs_;
};

//////////////////////////////////////////////////////////////////////////////
// ProphEBSSequenceThread
//
// Drives MicroManager's Live view by repeatedly re-inserting the same static
// test image at roughly the requested interval. There is no real hardware
// polling loop yet; this only exists so Goal 1 can be visually verified in
// the Live window, not just via single Snap.
//////////////////////////////////////////////////////////////////////////////
class ProphEBSSequenceThread : public MMDeviceThreadBase
{
public:
   explicit ProphEBSSequenceThread(CProphEBSCamera* pCamera);
   ~ProphEBSSequenceThread();

   void Start(long numImages, double intervalMs);
   void Stop();
   bool IsStopped();

private:
   int svc();

   CProphEBSCamera* camera_;
   double intervalMs_;
   long numImages_;
   long imageCounter_;
   bool stop_;
   MMThreadLock stopLock_;
};

//////////////////////////////////////////////////////////////////////////////
// ProphEBSFrameBuilderThread
//
// Goal 3: runs continuously (independent of MicroManager's Live/Snap state)
// once a real EBS is connected and streaming. Every g_EventIntegrationMs it
// calls CProphEBSCamera::BuildAndSwapFrame() to turn the events accumulated
// over that window into the next displayable frame. This models the real
// sensor's own behavior of continuously producing frames while streaming --
// SnapImage()/Live view just read whatever the latest built frame is,
// rather than driving the integration themselves.
//////////////////////////////////////////////////////////////////////////////
class ProphEBSFrameBuilderThread : public MMDeviceThreadBase
{
public:
   explicit ProphEBSFrameBuilderThread(CProphEBSCamera* pCamera);
   ~ProphEBSFrameBuilderThread();

   void Start(double intervalMs);
   void Stop();
   bool IsStopped();

private:
   int svc();

   CProphEBSCamera* camera_;
   double intervalMs_;
   bool stop_;
   MMThreadLock stopLock_;
};

//////////////////////////////////////////////////////////////////////////////
// ProphEBSStatsThread
//
// Goal 5: runs continuously while a connected camera is streaming, calling
// CProphEBSCamera::UpdateStats() every g_StatsIntervalMs to refresh the
// live read-only monitoring properties (data/event rate, ERC drop-rate
// estimate, temperature, illumination, pixel dead time). Mirrors
// ProphEBSFrameBuilderThread's pattern exactly, just on a coarser interval
// suited to human-readable stats rather than frame building.
//////////////////////////////////////////////////////////////////////////////
class ProphEBSStatsThread : public MMDeviceThreadBase
{
public:
   explicit ProphEBSStatsThread(CProphEBSCamera* pCamera);
   ~ProphEBSStatsThread();

   void Start(double intervalMs);
   void Stop();
   bool IsStopped();

private:
   int svc();

   CProphEBSCamera* camera_;
   double intervalMs_;
   bool stop_;
   MMThreadLock stopLock_;
};
