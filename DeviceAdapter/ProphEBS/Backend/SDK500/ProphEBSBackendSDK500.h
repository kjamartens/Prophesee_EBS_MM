///////////////////////////////////////////////////////////////////////////////
// FILE:          ProphEBSBackendSDK500.h
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters / ProphEBS / Backend / SDK500
//-----------------------------------------------------------------------------
// DESCRIPTION:   IProphEBSBackend implementation statically linked against
//                Metavision SDK 5.1.1's "stream" module (metavision_sdk_stream,
//                confirmed present as 5.1.1 on this dev machine). Ships as
//                ProphEBS_Backend_SDK500.dll, loaded dynamically at runtime by
//                mmgr_dal_ProphEBS.dll via BackendLoader -- see
//                IProphEBSBackend.h for the full rationale.
//
//                This is a 1:1 translation of ProphEBS.cpp's original
//                Metavision-facing logic (before the backend-shim refactor)
//                behind the IProphEBSBackend vtable -- see docs/DEVLOG.md for
//                the refactor that produced this split.
//
// COPYRIGHT:     Koen J.A. Martens, 2026
// LICENSE:       This file is distributed under the BSD license, consistent
//                with the rest of the Micro-Manager device adapter kit.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#pragma once

#include "../IProphEBSBackend.h"

#include <metavision/sdk/stream/camera.h>
#include <metavision/sdk/cv/algorithms/activity_noise_filter_algorithm.h>

#include <memory>
#include <string>
#include <vector>

class ProphEBSBackendSDK500 : public IProphEBSBackend
{
public:
   ProphEBSBackendSDK500() = default;
   ~ProphEBSBackendSDK500() override;

   // --- Connection -----------------------------------------------------
   bool Connect(bool biasRangeCheckBypass, char* errorOut, size_t errorOutLen) override;
   void Disconnect() override;
   bool GetIdentification(ProphEBSIdentification& out) override;

   // --- Streaming lifecycle ---------------------------------------------
   void RegisterCdCallback(ProphEBSCdCallback cb, void* ctx) override;
   void RegisterRawDataCallback(ProphEBSRawDataCallback cb, void* ctx) override;
   bool RegisterExtTriggerCallback(ProphEBSExtTriggerCallback cb, void* ctx) override;
   void UnregisterCallbacks() override;
   bool Start() override;
   void Stop() override;

   // --- Recording ---------------------------------------------------------
   bool StartRecording(const char* path) override;
   void StopRecording(const char* path) override;

   // --- Biases ------------------------------------------------------------
   size_t GetBiasCount() override;
   bool GetBiasInfo(size_t index, ProphEBSBiasInfo& out) override;
   bool GetBiasValue(const char* name, int& out) override;
   bool SetBiasValue(const char* name, int value, int& actualOut) override;

   // --- ERC -----------------------------------------------------------
   ProphEBSResult ErcGetRange(uint32_t& minRate, uint32_t& maxRate) override;
   ProphEBSResult ErcIsEnabled(bool& out) override;
   ProphEBSResult ErcEnable(bool enable) override;
   ProphEBSResult ErcGetRate(uint32_t& out) override;
   ProphEBSResult ErcSetRate(uint32_t rate) override;

   // --- Event trail (STC) filter ------------------------------------------
   ProphEBSResult EventTrailFilterGetAvailableTypes(char availableTypesCsv[128]) override;
   ProphEBSResult EventTrailFilterGetThresholdRange(uint32_t& minT, uint32_t& maxT) override;
   ProphEBSResult EventTrailFilterIsEnabled(bool& out) override;
   ProphEBSResult EventTrailFilterEnable(bool enable) override;
   ProphEBSResult EventTrailFilterGetThreshold(uint32_t& out) override;
   ProphEBSResult EventTrailFilterSetThreshold(uint32_t value) override;
   ProphEBSResult EventTrailFilterGetType(char typeOut[32]) override;
   ProphEBSResult EventTrailFilterSetType(const char* type) override;

   // --- Anti-flicker --------------------------------------------------
   ProphEBSResult AntiFlickerGetThresholdRanges(
      uint32_t& minStart, uint32_t& maxStart, uint32_t& minStop, uint32_t& maxStop) override;
   ProphEBSResult AntiFlickerGetDutyCycleRange(float& minDc, float& maxDc) override;
   ProphEBSResult AntiFlickerGetFrequencyRange(uint32_t& minFreq, uint32_t& maxFreq) override;
   ProphEBSResult AntiFlickerIsEnabled(bool& out) override;
   ProphEBSResult AntiFlickerEnable(bool enable) override;
   ProphEBSResult AntiFlickerGetStartThreshold(uint32_t& out) override;
   ProphEBSResult AntiFlickerSetStartThreshold(uint32_t value) override;
   ProphEBSResult AntiFlickerGetStopThreshold(uint32_t& out) override;
   ProphEBSResult AntiFlickerSetStopThreshold(uint32_t value) override;
   ProphEBSResult AntiFlickerGetDutyCycle(float& out) override;
   ProphEBSResult AntiFlickerSetDutyCycle(float value) override;
   ProphEBSResult AntiFlickerGetFilterType(bool& isBandPass) override;
   ProphEBSResult AntiFlickerSetFilterType(bool isBandPass) override;
   ProphEBSResult AntiFlickerGetFrequencyBand(uint32_t& lowFreq, uint32_t& highFreq) override;
   ProphEBSResult AntiFlickerSetFrequencyBand(uint32_t lowFreq, uint32_t highFreq) override;

   // --- Hardware trigger in/out ------------------------------------------
   ProphEBSResult TriggerInGetAvailableChannels(char channelsCsv[64]) override;
   ProphEBSResult TriggerInIsEnabled(const char* channel, bool& out) override;
   ProphEBSResult TriggerInEnable(const char* channel, bool enable) override;
   ProphEBSResult TriggerOutSetPeriod(uint32_t periodUs) override;
   ProphEBSResult TriggerOutSetDutyCycle(double dutyCycle) override;
   ProphEBSResult TriggerOutEnable(bool enable) override;

   // --- Sensor-level event-rate band-pass filter --------------------------
   ProphEBSResult EventRateFilterGetThresholdRanges(
      ProphEBSEventRateThresholds& minT, ProphEBSEventRateThresholds& maxT) override;
   ProphEBSResult EventRateFilterGetThresholds(ProphEBSEventRateThresholds& out) override;
   ProphEBSResult EventRateFilterSetThresholds(const ProphEBSEventRateThresholds& value) override;
   ProphEBSResult EventRateFilterIsEnabled(bool& out) override;
   ProphEBSResult EventRateFilterEnable(bool enable) override;

   // --- Sync mode -----------------------------------------------------
   ProphEBSResult SetSyncMode(const char* mode) override;

   // --- Hardware ROI ----------------------------------------------------
   ProphEBSResult RoiSetWindow(unsigned x, unsigned y, unsigned xSize, unsigned ySize) override;
   ProphEBSResult RoiDisable() override;

   // --- Hot-pixel masking -----------------------------------------------
   ProphEBSResult GetDigitalEventMaskSlotCount(size_t& out) override;
   ProphEBSResult ApplyDigitalEventMask(const unsigned* xs, const unsigned* ys, size_t count) override;
   ProphEBSResult ApplyRoiPixelMask(const unsigned* xs, const unsigned* ys, size_t count) override;

   // --- Monitoring ------------------------------------------------------
   ProphEBSResult GetTemperatureC(double& out) override;
   ProphEBSResult GetIlluminationLux(double& out) override;
   ProphEBSResult GetPixelDeadTimeUs(double& out) override;

   // --- Software activity-noise filter ------------------------------------
   bool ActivityFilterConstruct(unsigned width, unsigned height, int64_t thresholdUs) override;
   void ActivityFilterSetThreshold(int64_t thresholdUs) override;
   void ActivityFilterDestroy() override;
   void ActivityFilterProcess(const ProphEBSEvent* in, size_t inCount,
      ProphEBSEvent* outScratch, size_t& outCount) override;

private:
   // Camera::get_facility<T>() (the "stream"-module convenience method)
   // throws and returns T& directly -- every call site in this backend is
   // already written as a try/catch around it, same convention the original
   // ProphEBS.cpp's GetCamFacility<T>() normalized both SDK generations onto.
   template <typename T>
   T& Facility() { return cam_.get_facility<T>(); }

   Metavision::Camera cam_;
   bool connected_ = false;

   // CD/raw-data/ext-trigger callback registration -- one slot each, plain C
   // function pointer + opaque ctx per IProphEBSBackend.h's contract. Reusable
   // scratch buffers avoid a per-batch heap allocation on the hot callback
   // path.
   ProphEBSCdCallback cdCb_ = nullptr;
   void* cdCtx_ = nullptr;
   ProphEBSRawDataCallback rawCb_ = nullptr;
   void* rawCtx_ = nullptr;
   ProphEBSExtTriggerCallback trigCb_ = nullptr;
   void* trigCtx_ = nullptr;
   bool trigRegistered_ = false;
   bool cdRegistered_ = false;
   bool rawRegistered_ = false;

   Metavision::CallbackId cdCallbackId_{};
   Metavision::CallbackId rawDataCallbackId_{};
   Metavision::CallbackId triggerInCallbackId_{};

   std::vector<ProphEBSEvent> cdScratch_;
   std::vector<ProphEBSExtTriggerEvent> trigScratch_;

   // Goal 5 bias enumeration cache, built once in Connect() -- see
   // GetBiasCount()/GetBiasInfo().
   std::vector<std::string> biasNames_;

   // Software activity-noise filter -- constructed/destroyed once per
   // StartEventStreaming()/StopEventStreaming() cycle by the main adapter via
   // ActivityFilterConstruct()/ActivityFilterDestroy().
   std::unique_ptr<Metavision::ActivityNoiseFilterAlgorithm<>> activityFilter_;
   std::vector<Metavision::EventCD> activityFilterInScratch_;
   std::vector<Metavision::EventCD> activityFilterOutScratch_;
};
