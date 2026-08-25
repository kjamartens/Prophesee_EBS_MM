///////////////////////////////////////////////////////////////////////////////
// FILE:          IProphEBSBackend.h
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters / ProphEBS
//-----------------------------------------------------------------------------
// DESCRIPTION:   Stable interface between mmgr_dal_ProphEBS.dll (the only
//                thing MicroManager ever loads as the "ProphEBS-Camera"
//                device) and a small per-Metavision-SDK-generation backend
//                DLL (ProphEBS_Backend_<tag>.dll) that actually talks to the
//                Prophesee Metavision SDK/HAL.
//
//                Why this exists: two Metavision SDK generations (e.g. 4.3.0
//                and current 5.x) ship Metavision::Camera from differently
//                named DLLs (metavision_sdk_driver.dll vs
//                metavision_sdk_stream.dll) with a differently-shaped class
//                (pointer- vs throwing-reference get_facility()). That is an
//                OS-loader/link-time fact, not a runtime capability query --
//                a single statically-linked binary cannot import both, and
//                two translation units compiled against the two different
//                header sets cannot be linked into one binary either (both
//                define the identical mangled symbol `Metavision::Camera::*`,
//                so the linker would silently bind both to whichever .lib is
//                listed first). See docs/DEVLOG.md for the incident that
//                prompted this design.
//
//                So instead: mmgr_dal_ProphEBS.dll contains zero
//                "metavision/..." includes and holds no Metavision::Camera.
//                At Initialize() time it probes which Metavision SDK
//                generation is actually installed (BackendLoader.h) and
//                dynamically loads the matching backend DLL, each of which
//                is a completely separate binary statically linked against
//                exactly one SDK generation and implementing this interface.
//
//                Rules for anything crossing this boundary (see
//                docs/BUILD_AND_USAGE.md's "Adding a new Metavision SDK
//                generation backend" section for the full recipe):
//                - POD types, fixed-size char buffers, and plain C function
//                  pointers only -- no std::string/std::vector/std::function/
//                  exceptions in any virtual signature. The main adapter
//                  converts to/from std::string etc. on its own side.
//                - A facility genuinely absent on a given SDK generation
//                  returns ProphEBSResult::Unsupported, never throws.
//                - The main DLL and every backend DLL are always built and
//                  shipped together from the same repo commit (this is not a
//                  public, independently-versioned plugin ABI) -- so a plain
//                  C++ vtable interface is fine. ProphEBSBackendAbiTag() is
//                  only a sanity check against an accidental mixed-version
//                  deployment (e.g. a stale backend DLL left behind by a
//                  partial copy), not a real compatibility negotiation.
//
// COPYRIGHT:     Koen J.A. Martens, 2026
// LICENSE:       This file is distributed under the BSD license, consistent
//                with the rest of the Micro-Manager device adapter kit.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#pragma once

#include <cstddef>
#include <cstdint>

// Bump only if a virtual signature in this file actually changes shape.
// BackendLoader refuses to use a backend DLL whose ProphEBSBackendAbiTag()
// doesn't match this exact string.
#define PROPHEBS_BACKEND_ABI_TAG "ProphEBSBackend-ABI-1"

// Every backend DLL must export these two, extern "C", by exact name:
//   IProphEBSBackend* CreateProphEBSBackend();
//   void DestroyProphEBSBackend(IProphEBSBackend*);
//   const char* ProphEBSBackendAbiTag();  // returns PROPHEBS_BACKEND_ABI_TAG
#define PROPHEBS_BACKEND_CREATE_FN_NAME    "CreateProphEBSBackend"
#define PROPHEBS_BACKEND_DESTROY_FN_NAME   "DestroyProphEBSBackend"
#define PROPHEBS_BACKEND_ABITAG_FN_NAME    "ProphEBSBackendAbiTag"

extern "C" typedef const char* (*ProphEBSBackendAbiTagFn)();

// ---------------------------------------------------------------------------
// POD event/data shapes
// ---------------------------------------------------------------------------

// Mirrors the fields of Metavision::EventCD actually used by ProphEBS.cpp.
struct ProphEBSEvent
{
   uint16_t x;
   uint16_t y;
   int8_t polarity;    // 0 = OFF, 1 = ON (matches EventCD::p's 0/1 convention)
   int64_t tUs;         // Metavision::timestamp -- microsecond sensor clock
};

// Mirrors Metavision::EventExtTrigger's fields actually used.
struct ProphEBSExtTriggerEvent
{
   int64_t tUs;
   int16_t channelId;
   int8_t value;
};

typedef void (*ProphEBSCdCallback)(void* ctx, const ProphEBSEvent* begin, size_t count);
typedef void (*ProphEBSRawDataCallback)(void* ctx, size_t sizeBytes);
typedef void (*ProphEBSExtTriggerCallback)(void* ctx, const ProphEBSExtTriggerEvent* begin, size_t count);

// Generic per-call outcome for any hardware/SDK facility that may genuinely
// not exist on a given SDK generation or sensor. "Unsupported" means: no
// exception was involved, the caller should apply the exact same
// no-camera-style fallback behavior ProphEBS.cpp already has for every other
// "facility not available" condition. "Error" means the facility exists but
// this specific call failed/was rejected by hardware -- caller logs and
// keeps previous state, same as today's catch(std::exception) blocks.
enum class ProphEBSResult : int
{
   Ok = 0,
   Unsupported = 1,
   Error = 2
};

// ---------------------------------------------------------------------------
// Identification / geometry (Goal 2/3 -- I_HW_Identification, I_Geometry)
// ---------------------------------------------------------------------------
struct ProphEBSIdentification
{
   char serial[128];
   char connectionType[64];
   char integrator[64];
   char dataEncodingFormat[64];
   char sensorName[64];
   int sensorMajorVersion;
   int sensorMinorVersion;
   unsigned width;
   unsigned height;
};

// ---------------------------------------------------------------------------
// Biases (Goal 5 -- I_LL_Biases)
// ---------------------------------------------------------------------------
struct ProphEBSBiasInfo
{
   char name[64];
   int value;
   int minValue;
   int maxValue;
   bool hasRange; // false if get_bias_info() failed for this bias (rare)
};

// ---------------------------------------------------------------------------
// Event-rate activity filter thresholds (Goal 8 -- I_EventRateActivityFilterModule)
// ---------------------------------------------------------------------------
struct ProphEBSEventRateThresholds
{
   uint32_t lowerBoundStart;
   uint32_t lowerBoundStop;
   uint32_t upperBoundStart;
   uint32_t upperBoundStop;
};

// ---------------------------------------------------------------------------
// IProphEBSBackend
// ---------------------------------------------------------------------------
class IProphEBSBackend
{
public:
   virtual ~IProphEBSBackend() {}

   // --- Connection -----------------------------------------------------
   // Mirrors ConnectToCamera(): tries Camera::from_first_available() with
   // biasRangeCheckBypass applied via DeviceConfig. Returns false (with a
   // human-readable reason copied into errorOut, truncated to errorOutLen)
   // on any failure -- never throws across the boundary. Never fails
   // "loudly" -- same never-fail-Initialize() contract ProphEBS.cpp already
   // has; the main adapter decides what "not connected" means to MM.
   virtual bool Connect(bool biasRangeCheckBypass, char* errorOut, size_t errorOutLen) = 0;

   // Releases the HAL device handle. Safe to call whether or not Connect()
   // succeeded; safe to call more than once.
   virtual void Disconnect() = 0;

   // Valid only after a successful Connect(). Returns false if the
   // identification/geometry facilities themselves are unavailable (mirrors
   // the original code's single try/catch around both).
   virtual bool GetIdentification(ProphEBSIdentification& out) = 0;

   // --- Streaming lifecycle (Goal 3/5/8 -- cam_.cd()/.raw_data()/.ext_trigger(), cam_.start()/.stop()) --
   // Registers are additive; call before Start(). ext-trigger registration
   // may legitimately fail (mirrors the original try/catch around
   // cam_.ext_trigger().add_callback()) -- returns false, main adapter logs
   // and continues without trigger-in counting, exactly as today.
   virtual void RegisterCdCallback(ProphEBSCdCallback cb, void* ctx) = 0;
   virtual void RegisterRawDataCallback(ProphEBSRawDataCallback cb, void* ctx) = 0;
   virtual bool RegisterExtTriggerCallback(ProphEBSExtTriggerCallback cb, void* ctx) = 0;
   virtual void UnregisterCallbacks() = 0;
   virtual bool Start() = 0;
   virtual void Stop() = 0;

   // --- Recording (Goal 4 -- cam_.start_recording()/stop_recording()) --
   virtual bool StartRecording(const char* path) = 0;
   virtual void StopRecording(const char* path) = 0;

   // --- Biases (Goal 5 -- I_LL_Biases) ----------------------------------
   // Enumerates once (right after Connect()); GetBiasValue/SetBiasValue are
   // called on every property Get/Set thereafter, looked up by name (same
   // shape as the original OnBias()).
   virtual size_t GetBiasCount() = 0;
   virtual bool GetBiasInfo(size_t index, ProphEBSBiasInfo& out) = 0;
   virtual bool GetBiasValue(const char* name, int& out) = 0;
   // Returns the actual post-set hardware value in actualOut (the original
   // code always re-reads after set() to catch a firmware-level clamp) --
   // return value is whether the underlying set() call itself was accepted.
   virtual bool SetBiasValue(const char* name, int value, int& actualOut) = 0;

   // --- ERC (Goal 5/9 -- I_ErcModule) -----------------------------------
   virtual ProphEBSResult ErcGetRange(uint32_t& minRate, uint32_t& maxRate) = 0;
   virtual ProphEBSResult ErcIsEnabled(bool& out) = 0;
   virtual ProphEBSResult ErcEnable(bool enable) = 0;
   virtual ProphEBSResult ErcGetRate(uint32_t& out) = 0;
   virtual ProphEBSResult ErcSetRate(uint32_t rate) = 0;

   // --- Event trail (STC) filter (Goal 5 -- I_EventTrailFilterModule) ---
   // availableTypesCsv receives a comma-separated subset of
   // {"TRAIL","STC_CUT_TRAIL","STC_KEEP_TRAIL"}, e.g. "TRAIL,STC_CUT_TRAIL".
   virtual ProphEBSResult EventTrailFilterGetAvailableTypes(char availableTypesCsv[128]) = 0;
   virtual ProphEBSResult EventTrailFilterGetThresholdRange(uint32_t& minT, uint32_t& maxT) = 0;
   virtual ProphEBSResult EventTrailFilterIsEnabled(bool& out) = 0;
   virtual ProphEBSResult EventTrailFilterEnable(bool enable) = 0;
   virtual ProphEBSResult EventTrailFilterGetThreshold(uint32_t& out) = 0;
   virtual ProphEBSResult EventTrailFilterSetThreshold(uint32_t value) = 0;
   // type is one of the three literal strings above.
   virtual ProphEBSResult EventTrailFilterGetType(char typeOut[32]) = 0;
   virtual ProphEBSResult EventTrailFilterSetType(const char* type) = 0;

   // --- Anti-flicker (Goal 5 -- I_AntiFlickerModule) --------------------
   virtual ProphEBSResult AntiFlickerGetThresholdRanges(
      uint32_t& minStart, uint32_t& maxStart, uint32_t& minStop, uint32_t& maxStop) = 0;
   virtual ProphEBSResult AntiFlickerGetDutyCycleRange(float& minDc, float& maxDc) = 0;
   virtual ProphEBSResult AntiFlickerGetFrequencyRange(uint32_t& minFreq, uint32_t& maxFreq) = 0;
   virtual ProphEBSResult AntiFlickerIsEnabled(bool& out) = 0;
   virtual ProphEBSResult AntiFlickerEnable(bool enable) = 0;
   virtual ProphEBSResult AntiFlickerGetStartThreshold(uint32_t& out) = 0;
   virtual ProphEBSResult AntiFlickerSetStartThreshold(uint32_t value) = 0;
   virtual ProphEBSResult AntiFlickerGetStopThreshold(uint32_t& out) = 0;
   virtual ProphEBSResult AntiFlickerSetStopThreshold(uint32_t value) = 0;
   virtual ProphEBSResult AntiFlickerGetDutyCycle(float& out) = 0;
   virtual ProphEBSResult AntiFlickerSetDutyCycle(float value) = 0;
   // isBandPass: true = BAND_PASS, false = BAND_STOP.
   virtual ProphEBSResult AntiFlickerGetFilterType(bool& isBandPass) = 0;
   virtual ProphEBSResult AntiFlickerSetFilterType(bool isBandPass) = 0;
   virtual ProphEBSResult AntiFlickerGetFrequencyBand(uint32_t& lowFreq, uint32_t& highFreq) = 0;
   virtual ProphEBSResult AntiFlickerSetFrequencyBand(uint32_t lowFreq, uint32_t highFreq) = 0;

   // --- Hardware trigger in/out (Goal 8 -- I_TriggerIn / I_TriggerOut) --
   // channelsCsv receives a comma-separated subset of {"Main","Aux","Loopback"}.
   virtual ProphEBSResult TriggerInGetAvailableChannels(char channelsCsv[64]) = 0;
   virtual ProphEBSResult TriggerInIsEnabled(const char* channel, bool& out) = 0;
   virtual ProphEBSResult TriggerInEnable(const char* channel, bool enable) = 0;
   virtual ProphEBSResult TriggerOutSetPeriod(uint32_t periodUs) = 0;
   virtual ProphEBSResult TriggerOutSetDutyCycle(double dutyCycle) = 0;
   virtual ProphEBSResult TriggerOutEnable(bool enable) = 0; // I_TriggerOut has no is_enabled() query

   // --- Sensor-level event-rate band-pass filter (Goal 8 -- I_EventRateActivityFilterModule) --
   // Unsupported on any SDK generation that lacks this facility entirely
   // (e.g. 4.3.0) -- backend returns Unsupported for every one of these,
   // main adapter's existing local-fallback path takes over unchanged.
   virtual ProphEBSResult EventRateFilterGetThresholdRanges(
      ProphEBSEventRateThresholds& minT, ProphEBSEventRateThresholds& maxT) = 0;
   virtual ProphEBSResult EventRateFilterGetThresholds(ProphEBSEventRateThresholds& out) = 0;
   virtual ProphEBSResult EventRateFilterSetThresholds(const ProphEBSEventRateThresholds& value) = 0;
   virtual ProphEBSResult EventRateFilterIsEnabled(bool& out) = 0;
   virtual ProphEBSResult EventRateFilterEnable(bool enable) = 0;

   // --- Camera sync mode (Goal 8 -- I_CameraSynchronization) ------------
   // mode is one of "Standalone" / "Master" / "Slave".
   virtual ProphEBSResult SetSyncMode(const char* mode) = 0;

   // --- Hardware ROI (Goal 7 -- I_ROI) ----------------------------------
   virtual ProphEBSResult RoiSetWindow(unsigned x, unsigned y, unsigned xSize, unsigned ySize) = 0;
   virtual ProphEBSResult RoiDisable() = 0;

   // --- Hot-pixel masking (Goal 7 -- I_DigitalEventMask, I_RoiPixelMask) --
   // I_DigitalEventMask has a fixed number of hardware mask slots; the
   // original ApplyBlockedPixelsToHardware() only ever masks the first
   // GetDigitalEventMaskSlotCount() entries of the caller-supplied list.
   // Returns Unsupported if I_DigitalEventMask itself doesn't exist on this
   // sensor -- caller falls back to relying on ApplyRoiPixelMask() alone.
   virtual ProphEBSResult GetDigitalEventMaskSlotCount(size_t& out) = 0;
   virtual ProphEBSResult ApplyDigitalEventMask(const unsigned* xs, const unsigned* ys, size_t count) = 0;
   // Belt-and-suspenders secondary mechanism; Unsupported on SDK generations
   // without I_RoiPixelMask at all (e.g. 4.3.0) -- caller already tolerates
   // this being a no-op today.
   virtual ProphEBSResult ApplyRoiPixelMask(const unsigned* xs, const unsigned* ys, size_t count) = 0;

   // --- Monitoring (Goal 5 -- I_Monitoring) -----------------------------
   virtual ProphEBSResult GetTemperatureC(double& out) = 0;
   virtual ProphEBSResult GetIlluminationLux(double& out) = 0;
   virtual ProphEBSResult GetPixelDeadTimeUs(double& out) = 0;

   // --- Software activity-noise filter (Goal 6 -- Metavision::ActivityNoiseFilterAlgorithm) --
   // Kept behind the interface (rather than hand-rolled in the main
   // adapter) so its exact filtering semantics never drift from whatever
   // the real SDK algorithm does. thresholdUs matches
   // EBS-ActivityFilter-Threshold-us. Construct/Destroy bracket exactly one
   // filter instance per StartEventStreaming()/StopEventStreaming() cycle,
   // same lifetime as the original activityFilter_ member.
   virtual bool ActivityFilterConstruct(unsigned width, unsigned height, int64_t thresholdUs) = 0;
   virtual void ActivityFilterSetThreshold(int64_t thresholdUs) = 0;
   virtual void ActivityFilterDestroy() = 0;
   // Filters events in-place into a caller-owned scratch buffer (capacity
   // >= inCount); outCount receives how many survived. Must only be called
   // between a successful ActivityFilterConstruct() and ActivityFilterDestroy().
   virtual void ActivityFilterProcess(const ProphEBSEvent* in, size_t inCount,
      ProphEBSEvent* outScratch, size_t& outCount) = 0;
};
