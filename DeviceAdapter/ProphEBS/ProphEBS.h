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
//                Goal 5 (this revision) exposes the EBS's own hardware
//                settings (biases, ERC, event trail filter, anti-flicker) as
//                MM properties, plus live read-only monitoring properties
//                (data/event rate, temperature, etc).
//
//                Goal 6 (this revision) makes the live view configurable
//                instead of the Goal 3 fixed 100 ms/x32 constants: the
//                integration window is now MM's own standard Exposure
//                property (SetExposure() directly retunes how often
//                BuildAndSwapFrame() runs); CD events are accumulated
//                per-polarity (onCounts_/offCounts_ instead of one merged
//                counter) so EBS-ViewMode can render Merged/OnOnly/OffOnly/
//                NetSigned (ON minus OFF); EBS-ViewOffset/EBS-ViewScale turn
//                that raw per-pixel value into pixel = clamp(offset + raw *
//                scale, 0, 255); and an optional software
//                Metavision::ActivityNoiseFilterAlgorithm
//                (EBS-ActivityFilter-Enabled/-Threshold-us) can denoise the
//                event stream before it's accumulated at all.
//
//                Goal 6 follow-up (this revision): Exposure's integration
//                window can now go sub-millisecond, driven by real
//                Metavision::EventCD::t sensor timestamps (microsecond
//                resolution) instead of a wall-clock Sleep() loop -- the
//                naive version of this (fully clearing the dense per-pixel
//                onCounts_/offCounts_ arrays every time a sub-ms window
//                closes) doesn't scale for a megapixel-class sensor, so
//                window-close only resets the specific pixels actually
//                touched since it opened (touchedIndices_). A new
//                EBS-DisplayRefreshMs property decouples how often a frame
//                is actually published (ProphEBSFrameBuilderThread) from
//                how long each integration window is -- no point publishing
//                faster than ~1 ms since nothing downstream can show it
//                anyway. A fixed idle timeout (g_IdleWindowTimeoutMs) force-
//                closes a stale window during a quiet scene, so the display
//                still resets to the baseline offset rather than freezing on
//                the last-active frame forever.
//
//                Pixel masking and sensor ROI are added in Goal 7 (see
//                docs/DEVLOG.md at the repository root for the roadmap).
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
#include <metavision/sdk/cv/algorithms/activity_noise_filter_algorithm.h>

#include <memory>

#include <atomic>
#include <chrono>
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

// Goal 6: how BuildAndSwapFrame() turns the per-pixel ON/OFF event counts
// accumulated over one integration window (MM's own Exposure property, see
// CProphEBSCamera::integrationTimeMs_) into an 8-bit grayscale value.
// EBS-ViewMode picks which raw per-pixel quantity is used:
//   Merged   -- onCount + offCount (the Goal 3/5 behavior)
//   OnOnly   -- onCount only
//   OffOnly  -- offCount only
//   NetSigned -- onCount - offCount (signed; the default -- see below)
// then pixel = clamp(EBS-ViewOffset + raw * EBS-ViewScale, 0, 255).
// NetSigned is the default view mode because a quiet pixel then sits at the
// gray level EBS-ViewOffset (default 10) and net ON/OFF activity visibly
// pushes it up/down from there -- this is the actual Goal 6 ask ("integrate
// over whatever time is wanted, and show offset +- found events"); Merged
// is kept selectable for the old Goal 3/5 look.
extern const char* g_PropViewMode;
extern const char* g_PropViewOffset;
extern const char* g_PropViewScale;
const char* const g_ViewModeMerged = "Merged";
const char* const g_ViewModeOnOnly = "OnOnly";
const char* const g_ViewModeOffOnly = "OffOnly";
const char* const g_ViewModeNetSigned = "NetSigned";
const long g_DefaultViewOffset = 10;
const double g_DefaultViewScale = 1.0;

// Goal 6: software activity-noise filter (Metavision::
// ActivityNoiseFilterAlgorithm), applied to each CD event batch in
// OnEventsCD() before accumulation, when enabled. Off by default -- a
// genuinely optional denoising step, not required for the base view.
extern const char* g_PropActivityFilterEnabled;
extern const char* g_PropActivityFilterThresholdUs;
const long g_DefaultActivityFilterThresholdUs = 10000;

// Goal 6 follow-up: decouples "how long is one integration window" (Exposure,
// now sub-millisecond capable -- see CProphEBSCamera::integrationTimeMs_ and
// OnEventsCD()'s window-close logic) from "how often is a frame actually
// published" (EBS-DisplayRefreshMs, backing displayRefreshMs_ -- what
// ProphEBSFrameBuilderThread now sleeps on instead of the integration time).
// There is no value in publishing faster than about 1 ms since nothing
// downstream (Live view, ImageJ, a human) can show it, hence the 1 ms floor.
extern const char* g_PropDisplayRefreshMs;
const double g_DefaultDisplayRefreshMs = 1.0;

// Goal 6 follow-up: if no window has closed (i.e. no CD event has arrived)
// for this many milliseconds of wall-clock time, BuildAndSwapFrame() force-
// closes the current (stale) window itself before rendering, so a quiet/
// unchanging scene still resets to the EBS-ViewOffset baseline instead of
// freezing on the last-active frame forever. Fixed rather than a property,
// per explicit user decision ("100ms or so" is fine as a constant).
const double g_IdleWindowTimeoutMs = 100.0;

// Bug fix (found after the sub-ms follow-up above): MMCore's own
// CMMCore::startContinuousSequenceAcquisition(double unused) names its
// parameter "unused" and its own MMCore.cpp comment says "the MM::Camera
// contract now says new adapters must ignore it" -- but
// ProphEBSSequenceThread::svc() used it directly as its push interval since
// Goal 1. This was invisible as long as whatever value got passed through
// stayed near ~100 ms, but once Exposure could go sub-millisecond (and
// MicroManager's Live view plausibly passes the camera's own exposure as
// this "unused" value), Live view started requesting a frame push up to
// 1000 times/sec -- far faster than MMCore's circular buffer/the GUI can
// actually consume -- causing an ever-growing display backlog/latency
// instead of any visible sub-ms benefit. Fix: ignore the caller-supplied
// interval entirely for unbounded (Live) sequences; instead push at
// max(Exposure, EBS-LiveViewMinIntervalMs) -- Live view naturally follows
// Exposure like any other camera for normal exposure times, but never
// faster than the floor once Exposure goes below it (there's no reason to
// push more Live frames/sec than the GUI can actually display, whatever
// the sub-ms integration window is doing internally). Finite (MDA)
// sequences still honor their caller-supplied interval exactly as before,
// since that one is a real, user-configured setting (e.g. "10 frames at
// 100 ms") with no equivalent "ignore this" contract.
extern const char* g_PropLiveViewMinIntervalMs;
const double g_DefaultLiveViewMinIntervalMs = 5.0;

// Follow-up: under sustained high event rates, the Metavision SDK's own
// internal decode/callback pipeline can fall behind real (wall-clock) time --
// observed as the live view's display lag growing during a burst of activity
// and then slowly draining back down once activity calms, rather than
// staying flat. OnEventsCD() detects this directly by comparing each
// incoming batch's newest sensor timestamp (Metavision::EventCD::t,
// microsecond-resolution) against how much wall-clock time has actually
// elapsed since streaming started -- the difference is the callback's own
// lag behind real time. Once that lag exceeds
// EBS-BacklogFlushThresholdMs, rather than keep faithfully replaying every
// stale sub-window in the backlog (which is itself real CPU work that only
// prolongs the slow recovery), the current window's accumulators are wiped
// and re-anchored to "now" in one cheap O(sensor pixels) pass -- discarding
// the stale backlog's per-pixel detail instead of laboriously draining it,
// which is what actually lets the callback thread catch back up quickly.
// EBS-CallbackLagMs/EBS-BacklogFlushCount are read-only diagnostics (pushed
// on the existing Goal 5 stats cadence, see UpdateStats()) so this is
// observable in the Device/Property Browser without rebuilding.
extern const char* g_PropCallbackLagMs;
extern const char* g_PropBacklogFlushCount;
extern const char* g_PropBacklogFlushThresholdMs;
const double g_DefaultBacklogFlushThresholdMs = 250.0;

// Goal 6: the four selectable view modes, stored as CProphEBSCamera::
// viewMode_ (a std::atomic<int> holding one of these values -- see there
// for why plain int rather than std::atomic<ProphEBSViewMode>).
enum class ProphEBSViewMode : int
{
   Merged = 0,
   OnOnly = 1,
   OffOnly = 2,
   NetSigned = 3
};

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

   // Goal 6: MM's standard Exposure property now doubles as the
   // event-integration window -- AfterSet stores the new value into
   // integrationTimeMs_ (an atomic). Goal 6 follow-up: this can now be
   // sub-millisecond -- OnEventsCD() closes a window based on real
   // Metavision::EventCD::t sensor timestamps (microsecond resolution), not
   // a wall-clock Sleep() loop, so changing Exposure takes effect on the
   // next event batch without restarting streaming.
   int OnExposure(MM::PropertyBase* pProp, MM::ActionType eAct);

   // Goal 6 follow-up: how often ProphEBSFrameBuilderThread actually
   // publishes a frame (BuildAndSwapFrame()), decoupled from how long one
   // integration window is (Exposure, above) -- see g_PropDisplayRefreshMs.
   int OnDisplayRefreshMs(MM::PropertyBase* pProp, MM::ActionType eAct);

   // Bug fix: floor on Live view's frame-push cadence -- see
   // g_PropLiveViewMinIntervalMs and ProphEBSSequenceThread::svc().
   int OnLiveViewMinIntervalMs(MM::PropertyBase* pProp, MM::ActionType eAct);

   // Goal 6: view-mode/offset/scale handlers -- see g_PropViewMode above for
   // what each does. All three are plain atomic-backed settable properties;
   // AfterSet writes are single-writer (MMCore serializes property sets) and
   // BuildAndSwapFrame() is the sole reader, so no additional locking is
   // needed beyond the atomics themselves.
   int OnViewMode(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnViewOffset(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnViewScale(MM::PropertyBase* pProp, MM::ActionType eAct);

   // Goal 6: software activity-noise-filter handlers. Both guard
   // activityFilter_ (constructed only once StartEventStreaming() knows the
   // sensor geometry) under activityFilterLock_, the same lock OnEventsCD()
   // takes while calling process_events() -- needed because the algorithm's
   // internal threshold/state isn't documented as safe against a concurrent
   // setter from the property system's thread.
   int OnActivityFilterEnabled(MM::PropertyBase* pProp, MM::ActionType eAct);
   int OnActivityFilterThreshold(MM::PropertyBase* pProp, MM::ActionType eAct);

   // Follow-up: settable threshold (see g_PropBacklogFlushThresholdMs) for
   // OnEventsCD()'s backlog detection -- plain atomic-backed, same shape as
   // OnViewOffset()/OnViewScale() above. EBS-CallbackLagMs/EBS-BacklogFlushCount
   // are read-only and share OnStat() instead (added to its property list in
   // Initialize()), since they're pushed on the same stats cadence as the
   // Goal 5 live-stats properties.
   int OnBacklogFlushThresholdMs(MM::PropertyBase* pProp, MM::ActionType eAct);

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
   // thread whenever a batch of events has been decoded. Optionally runs the
   // batch through the Goal 6 software activity-noise filter first, then
   // accumulates per-pixel ON/OFF counts under eventCountsLock_. Goal 6
   // follow-up: also checks each event's own sensor timestamp against the
   // current window's start and calls CloseCurrentWindowLocked() once
   // enough sensor-time has elapsed (Exposure, converted to microseconds) --
   // this is what makes sub-millisecond integration windows possible, since
   // it's driven by the sensor's own microsecond-resolution clock rather
   // than a wall-clock Sleep(). All the actual frame rendering still happens
   // in BuildAndSwapFrame(), off this hot path.
   void OnEventsCD(const Metavision::EventCD* begin, const Metavision::EventCD* end);

   // Goal 6 follow-up: closes the current integration window -- resets only
   // the specific pixels touched since it opened (touchedIndices_), not the
   // whole onCounts_/offCounts_ arrays, since a sub-millisecond window can
   // close far more often than a full-sensor-size clear could ever keep up
   // with. Caller must already hold eventCountsLock_ (this never locks
   // itself). If fromEvent, windowStartT_ is advanced to eventT (the event
   // that triggered the close); otherwise (the idle-timeout path from
   // BuildAndSwapFrame()) windowStartT_ is left as-is -- it'll simply look
   // very stale to the next real event, which harmlessly triggers an
   // immediate (already-empty) close and a fresh windowStartT_ right then.
   void CloseCurrentWindowLocked(bool fromEvent, Metavision::timestamp eventT);

   // Follow-up: performs the actual backlog flush -- wipes onCounts_/
   // offCounts_/touchedIndices_, re-anchors windowStartT_ and the
   // streamWallStart_/streamSensorStart_ lag-tracking pair to (nowWall,
   // eventT), and records the flush for the EBS-BacklogFlushCount/
   // EBS-CallbackLagMs diagnostics. Caller must already hold
   // eventCountsLock_ (this never locks it itself), same convention as
   // CloseCurrentWindowLocked() above. Factored out of OnEventsCD() so it
   // can be called both at batch entry and, on a large/slow batch, from
   // partway through the per-event loop -- see OnEventsCD() for why a
   // batch-entry-only check let a single slow batch blow past
   // EBS-BacklogFlushThresholdMs before ever getting flushed.
   void FlushBacklogLocked(std::chrono::steady_clock::time_point nowWall,
      Metavision::timestamp eventT, double lagMs);

   // Called by ProphEBSFrameBuilderThread every EBS-DisplayRefreshMs (Goal 6
   // follow-up -- decoupled from the Exposure/integration-window length):
   // first, if no window has closed for g_IdleWindowTimeoutMs (a quiet
   // scene), force-closes the stale window so the display resets to the
   // EBS-ViewOffset baseline rather than freezing; then takes a quick locked
   // copy of the per-polarity event-count accumulators (a copy, not a
   // reset-via-swap like Goal 6 -- window resets are now exclusively
   // CloseCurrentWindowLocked()'s job), renders them into backImg_ per the
   // current EBS-ViewMode/-Offset/-Scale, then swaps front/back so
   // GetImageBuffer()/InsertImage() start returning the newly-built frame.
   // Readers of frontImg_ never see a partially-written
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

   // Goal 6: per-pixel ON/OFF CD event counts accumulated since the current
   // integration window opened, written by OnEventsCD() (Metavision's
   // callback thread) and read (copied, not reset -- see
   // CloseCurrentWindowLocked()) by BuildAndSwapFrame() (frame-builder
   // thread). Split by polarity (int32_t, not uint32_t -- Goal 3 only ever
   // needed an unsigned merged count) so EBS-ViewMode's NetSigned mode can
   // render onCounts_[i] - offCounts_[i] directly.
   std::vector<int32_t> onCounts_;
   std::vector<int32_t> offCounts_;
   MMThreadLock eventCountsLock_;

   // Goal 6 follow-up: sub-millisecond integration-window state, all guarded
   // by eventCountsLock_ (same lock as onCounts_/offCounts_ above).
   // windowStartT_ is the sensor's own microsecond-resolution timestamp
   // (Metavision::EventCD::t) at which the current window opened --
   // OnEventsCD() closes the window once an event's timestamp is far enough
   // past it. touchedIndices_ records which onCounts_/offCounts_ indices
   // were actually written to since the window opened, so
   // CloseCurrentWindowLocked() only has to reset those specific entries
   // (cost proportional to real event activity in the window) rather than
   // the whole sensor-sized array -- a full clear on every window-close
   // doesn't scale once windows can close hundreds of thousands of times a
   // second. lastWindowCloseWallTime_ is a plain (non-sensor) wall-clock
   // timestamp of the last close, used only by BuildAndSwapFrame()'s idle
   // timeout (g_IdleWindowTimeoutMs).
   Metavision::timestamp windowStartT_;
   std::vector<uint32_t> touchedIndices_;
   std::chrono::steady_clock::time_point lastWindowCloseWallTime_;

   // Follow-up: backlog detection/flush state, all touched only from
   // OnEventsCD() (Metavision's own callback thread) except
   // backlogFlushThresholdMs_ (settable from any thread via its property
   // handler, hence atomic). streamWallStart_/streamSensorStart_ are the
   // (wall-clock, sensor-clock) anchor pair a later event's ev->t is
   // compared against to detect the callback thread running behind real
   // time. streamSensorStart_ == -1 means "no anchor yet" (armed on the
   // very first event of a StartEventStreaming() session).
   //
   // Bug found after the initial version of this mechanism shipped: the
   // anchor was only ever reset on an actual flush, so a batch that
   // computed zero or negative lag (i.e. genuinely caught up, or even
   // running ahead because one callback delivered a wide sensor-time span
   // in a burst of already-decoded backlog) left the anchor untouched --
   // and lag briefly went very negative (observed: -3210 ms) after an
   // active scene. A later, genuine burst then had to climb back up through
   // that entire negative deficit before crossing
   // EBS-BacklogFlushThresholdMs again, silently defeating the threshold
   // for however long that climb took. Fix: OnEventsCD() now re-anchors to
   // (nowWall, latestT) -- and clamps the reported lag to 0 -- any time the
   // computed lag is <= 0, not just on an explicit flush, so the metric
   // continuously tracks "how far behind the most recent caught-up point,"
   // never a stale credit from having briefly run ahead.
   //
   // Second bug found at the same time: the flush check only ran once, at
   // the top of OnEventsCD(), before the per-event loop. Metavision can
   // (and did, per the report) deliver a single callback batch spanning a
   // huge sensor-time range in one call; if that one batch is itself slow
   // to process (per-event touchedIndices_ growth, window-close resets),
   // wall-clock time keeps advancing throughout it but nothing re-checks
   // lag until the *next* callback invocation -- letting one slow batch run
   // the true delay (observed: ~3460 ms) well past a 250 ms threshold
   // before anything flushes. Fix: OnEventsCD()'s per-event loop now also
   // rechecks lag every g_LagCheckEventInterval events and can flush
   // mid-batch, bounding a single batch's worst-case delay to roughly that
   // interval's own processing time instead of the whole batch's.
   //
   // callbackLagMs_/backlogFlushCount_ back the read-only diagnostic
   // properties (EBS-CallbackLagMs/EBS-BacklogFlushCount), pushed on the
   // existing Goal 5 stats cadence by UpdateStats().
   std::chrono::steady_clock::time_point streamWallStart_;
   Metavision::timestamp streamSensorStart_;
   std::atomic<double> callbackLagMs_;
   std::atomic<uint64_t> backlogFlushCount_;
   std::atomic<double> backlogFlushThresholdMs_;

   // Goal 6: event-integration window length, driven by MM's own Exposure
   // property (see OnExposure()) instead of the old fixed
   // g_EventIntegrationMs constant. Goal 6 follow-up: now sub-millisecond
   // capable -- OnEventsCD() converts this to microseconds and compares it
   // against real event timestamps (see windowStartT_ above) rather than a
   // separate thread sleeping this many milliseconds, so changing Exposure
   // takes effect on the very next event batch.
   std::atomic<double> integrationTimeMs_;

   // Goal 6 follow-up: how often ProphEBSFrameBuilderThread actually
   // publishes a frame (BuildAndSwapFrame()) -- decoupled from
   // integrationTimeMs_ above, since publishing faster than the display can
   // show is pure overhead. The friend ProphEBSFrameBuilderThread::svc()
   // reads this atomic every loop iteration, same pattern as
   // integrationTimeMs_ was read in Goal 6.
   std::atomic<double> displayRefreshMs_;

   // Bug fix: floor on how fast ProphEBSSequenceThread pushes frames into
   // MMCore during Live view (unbounded sequences only) -- see
   // g_PropLiveViewMinIntervalMs. The friend ProphEBSSequenceThread::svc()
   // reads this (and integrationTimeMs_ above) directly every loop
   // iteration to compute max(Exposure, this floor).
   std::atomic<double> liveViewMinIntervalMs_;

   // Goal 6: view-rendering parameters read by BuildAndSwapFrame() -- see
   // g_PropViewMode for the formula. viewMode_ stores a ProphEBSViewMode
   // value as a plain int (std::atomic<enum class> needs no extra machinery
   // in C++17, but keeping the atomic's value_type as int sidesteps any
   // enum-atomic edge cases across compilers).
   std::atomic<int> viewMode_;
   std::atomic<double> viewOffset_;
   std::atomic<double> viewScale_;

   // Goal 6: software activity-noise filter. Constructed only inside
   // StartEventStreaming() (needs sensorWidth_/sensorHeight_, known only
   // once connected) and destroyed in StopEventStreaming(); null the rest of
   // the time, so OnEventsCD() and the property handlers both null-check
   // before using it. activityFilterEnabled_/-ThresholdUs_ are the
   // user-facing state (settable any time, even with no camera connected,
   // mirroring EBS-EventTrailFilter-* from Goal 5); activityFilterLock_
   // guards activityFilter_ itself (both process_events() from OnEventsCD()
   // and set_threshold()/reconstruction from the property handlers).
   std::unique_ptr<Metavision::ActivityNoiseFilterAlgorithm<>> activityFilter_;
   MMThreadLock activityFilterLock_;
   bool activityFilterEnabled_;
   long activityFilterThresholdUs_;

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
// once a real EBS is connected and streaming, calling
// CProphEBSCamera::BuildAndSwapFrame() to turn the events accumulated over
// one integration window into the next displayable frame. This models the
// real sensor's own behavior of continuously producing frames while
// streaming -- SnapImage()/Live view just read whatever the latest built
// frame is, rather than driving the integration themselves.
//
// Goal 6: the integration window is no longer fixed -- svc() (a friend of
// CProphEBSCamera) reads camera_->integrationTimeMs_ fresh every loop
// iteration instead of a value snapshotted once at Start(), so changing MM's
// Exposure property takes effect on the very next frame.
//////////////////////////////////////////////////////////////////////////////
class ProphEBSFrameBuilderThread : public MMDeviceThreadBase
{
public:
   explicit ProphEBSFrameBuilderThread(CProphEBSCamera* pCamera);
   ~ProphEBSFrameBuilderThread();

   void Start();
   void Stop();
   bool IsStopped();

private:
   int svc();

   CProphEBSCamera* camera_;
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
