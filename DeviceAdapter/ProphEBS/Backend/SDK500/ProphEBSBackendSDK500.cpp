///////////////////////////////////////////////////////////////////////////////
// FILE:          ProphEBSBackendSDK500.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters / ProphEBS / Backend / SDK500
//-----------------------------------------------------------------------------
// See ProphEBSBackendSDK500.h for the design rationale. This is a 1:1
// translation of the original (pre-backend-shim) ProphEBS.cpp's
// Metavision-facing logic behind the IProphEBSBackend vtable -- see
// docs/DEVLOG.md.
//
// COPYRIGHT:     Koen J.A. Martens, 2026
// LICENSE:       This file is distributed under the BSD license.
///////////////////////////////////////////////////////////////////////////////

#include "ProphEBSBackendSDK500.h"

#include <metavision/hal/facilities/i_antiflicker_module.h>
#include <metavision/hal/facilities/i_camera_synchronization.h>
#include <metavision/hal/facilities/i_digital_event_mask.h>
#include <metavision/hal/facilities/i_erc_module.h>
#include <metavision/hal/facilities/i_event_rate_activity_filter_module.h>
#include <metavision/hal/facilities/i_event_trail_filter_module.h>
#include <metavision/hal/facilities/i_geometry.h>
#include <metavision/hal/facilities/i_hw_identification.h>
#include <metavision/hal/facilities/i_ll_biases.h>
#include <metavision/hal/facilities/i_monitoring.h>
#include <metavision/hal/facilities/i_roi.h>
#include <metavision/hal/facilities/i_roi_pixel_mask.h>
#include <metavision/hal/facilities/i_trigger_in.h>
#include <metavision/hal/facilities/i_trigger_out.h>
#include <metavision/hal/utils/device_config.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace
{
   void CopyToBuffer(const std::string& src, char* dst, size_t dstLen)
   {
      if (dst == nullptr || dstLen == 0)
         return;
      size_t n = std::min(src.size(), dstLen - 1);
      std::memcpy(dst, src.data(), n);
      dst[n] = '\0';
   }

   // Duplicated in the main adapter's tiny amount purely for logging purposes
   // has been removed there entirely -- these two conversions are the only
   // place Metavision::I_EventTrailFilterModule::Type is spelled out as a
   // string, kept local to this backend so the type itself never crosses the
   // IProphEBSBackend boundary (see EventTrailFilterGetType()'s comment).
   const char* TrailTypeToString(Metavision::I_EventTrailFilterModule::Type type)
   {
      switch (type)
      {
         case Metavision::I_EventTrailFilterModule::Type::TRAIL: return "TRAIL";
         case Metavision::I_EventTrailFilterModule::Type::STC_CUT_TRAIL: return "STC_CUT_TRAIL";
         case Metavision::I_EventTrailFilterModule::Type::STC_KEEP_TRAIL: return "STC_KEEP_TRAIL";
      }
      return "TRAIL";
   }

   bool TrailTypeFromString(const std::string& s, Metavision::I_EventTrailFilterModule::Type& outType)
   {
      if (s == "TRAIL") { outType = Metavision::I_EventTrailFilterModule::Type::TRAIL; return true; }
      if (s == "STC_CUT_TRAIL") { outType = Metavision::I_EventTrailFilterModule::Type::STC_CUT_TRAIL; return true; }
      if (s == "STC_KEEP_TRAIL") { outType = Metavision::I_EventTrailFilterModule::Type::STC_KEEP_TRAIL; return true; }
      return false;
   }

   const char* TriggerChannelToString(Metavision::I_TriggerIn::Channel ch)
   {
      switch (ch)
      {
         case Metavision::I_TriggerIn::Channel::Main: return "Main";
         case Metavision::I_TriggerIn::Channel::Aux: return "Aux";
         case Metavision::I_TriggerIn::Channel::Loopback: return "Loopback";
      }
      return "Main";
   }

   bool TriggerChannelFromString(const std::string& s, Metavision::I_TriggerIn::Channel& outCh)
   {
      if (s == "Main") { outCh = Metavision::I_TriggerIn::Channel::Main; return true; }
      if (s == "Aux") { outCh = Metavision::I_TriggerIn::Channel::Aux; return true; }
      if (s == "Loopback") { outCh = Metavision::I_TriggerIn::Channel::Loopback; return true; }
      return false;
   }
} // anonymous namespace

ProphEBSBackendSDK500::~ProphEBSBackendSDK500()
{
   Disconnect();
}

bool ProphEBSBackendSDK500::Connect(bool biasRangeCheckBypass, char* errorOut, size_t errorOutLen)
{
   try
   {
      Metavision::DeviceConfig deviceConfig;
      deviceConfig.enable_biases_range_check_bypass(biasRangeCheckBypass);
      cam_ = Metavision::Camera::from_first_available(deviceConfig);
      connected_ = true;

      // Goal 5: cache the sorted bias-name list once, right after connect --
      // GetBiasCount()/GetBiasInfo() index into this rather than re-querying
      // the SDK on every call. Not fatal if I_LL_Biases isn't available on
      // this device; GetBiasCount() will just report 0.
      try
      {
         std::map<std::string, int> allBiases = Facility<Metavision::I_LL_Biases>().get_all_biases();
         biasNames_.clear();
         for (const auto& kv : allBiases)
            biasNames_.push_back(kv.first);
      }
      catch (const std::exception&)
      {
         biasNames_.clear();
      }

      return true;
   }
   catch (const std::exception& e)
   {
      connected_ = false;
      CopyToBuffer(e.what(), errorOut, errorOutLen);
      return false;
   }
}

void ProphEBSBackendSDK500::Disconnect()
{
   if (connected_)
   {
      try { UnregisterCallbacks(); } catch (const std::exception&) {}
      cam_ = Metavision::Camera();
   }
   connected_ = false;
}

bool ProphEBSBackendSDK500::GetIdentification(ProphEBSIdentification& out)
{
   try
   {
      Metavision::I_HW_Identification& hwId = Facility<Metavision::I_HW_Identification>();
      CopyToBuffer(hwId.get_serial(), out.serial, sizeof(out.serial));
      CopyToBuffer(hwId.get_connection_type(), out.connectionType, sizeof(out.connectionType));
      CopyToBuffer(hwId.get_integrator(), out.integrator, sizeof(out.integrator));
      CopyToBuffer(hwId.get_current_data_encoding_format(), out.dataEncodingFormat, sizeof(out.dataEncodingFormat));

      Metavision::I_HW_Identification::SensorInfo sensorInfo = hwId.get_sensor_info();
      CopyToBuffer(sensorInfo.name_, out.sensorName, sizeof(out.sensorName));
      out.sensorMajorVersion = sensorInfo.major_version_;
      out.sensorMinorVersion = sensorInfo.minor_version_;

      Metavision::I_Geometry& geometry = Facility<Metavision::I_Geometry>();
      out.width = static_cast<unsigned>(geometry.get_width());
      out.height = static_cast<unsigned>(geometry.get_height());
      return true;
   }
   catch (const std::exception&)
   {
      return false;
   }
}

void ProphEBSBackendSDK500::RegisterCdCallback(ProphEBSCdCallback cb, void* ctx)
{
   cdCb_ = cb;
   cdCtx_ = ctx;
   cdCallbackId_ = cam_.cd().add_callback(
      [this](const Metavision::EventCD* begin, const Metavision::EventCD* end)
      {
         if (!cdCb_)
            return;
         size_t n = static_cast<size_t>(end - begin);
         cdScratch_.resize(n);
         for (size_t i = 0; i < n; i++)
         {
            const Metavision::EventCD& ev = begin[i];
            cdScratch_[i].x = static_cast<uint16_t>(ev.x);
            cdScratch_[i].y = static_cast<uint16_t>(ev.y);
            cdScratch_[i].polarity = static_cast<int8_t>(ev.p);
            cdScratch_[i].tUs = static_cast<int64_t>(ev.t);
         }
         cdCb_(cdCtx_, cdScratch_.data(), cdScratch_.size());
      });
   cdRegistered_ = true;
}

void ProphEBSBackendSDK500::RegisterRawDataCallback(ProphEBSRawDataCallback cb, void* ctx)
{
   rawCb_ = cb;
   rawCtx_ = ctx;
   rawDataCallbackId_ = cam_.raw_data().add_callback(
      [this](const uint8_t* /*data*/, size_t size)
      {
         if (rawCb_)
            rawCb_(rawCtx_, size);
      });
   rawRegistered_ = true;
}

bool ProphEBSBackendSDK500::RegisterExtTriggerCallback(ProphEBSExtTriggerCallback cb, void* ctx)
{
   trigCb_ = cb;
   trigCtx_ = ctx;
   try
   {
      triggerInCallbackId_ = cam_.ext_trigger().add_callback(
         [this](const Metavision::EventExtTrigger* begin, const Metavision::EventExtTrigger* end)
         {
            if (!trigCb_)
               return;
            size_t n = static_cast<size_t>(end - begin);
            trigScratch_.resize(n);
            for (size_t i = 0; i < n; i++)
            {
               const Metavision::EventExtTrigger& ev = begin[i];
               trigScratch_[i].tUs = static_cast<int64_t>(ev.t);
               trigScratch_[i].channelId = static_cast<int16_t>(ev.id);
               trigScratch_[i].value = static_cast<int8_t>(ev.p);
            }
            trigCb_(trigCtx_, trigScratch_.data(), trigScratch_.size());
         });
      trigRegistered_ = true;
      return true;
   }
   catch (const std::exception&)
   {
      trigRegistered_ = false;
      return false;
   }
}

void ProphEBSBackendSDK500::UnregisterCallbacks()
{
   // Mirrors the original ProphEBS.cpp's StopEventStreaming(): cd()/
   // raw_data()'s remove_callback() are expected to succeed (both facilities
   // always exist once connected); ext_trigger()'s removal is wrapped in
   // try/catch since its registration can legitimately fail (see
   // RegisterExtTriggerCallback()), in which case there is nothing to remove.
   if (cdRegistered_)
   {
      try { cam_.cd().remove_callback(cdCallbackId_); } catch (const std::exception&) {}
      cdRegistered_ = false;
   }
   if (rawRegistered_)
   {
      try { cam_.raw_data().remove_callback(rawDataCallbackId_); } catch (const std::exception&) {}
      rawRegistered_ = false;
   }
   if (trigRegistered_)
   {
      try { cam_.ext_trigger().remove_callback(triggerInCallbackId_); } catch (const std::exception&) {}
      trigRegistered_ = false;
   }
}

bool ProphEBSBackendSDK500::Start()
{
   try { cam_.start(); return true; }
   catch (const std::exception&) { return false; }
}

void ProphEBSBackendSDK500::Stop()
{
   try { cam_.stop(); }
   catch (const std::exception&) {}
}

bool ProphEBSBackendSDK500::StartRecording(const char* path)
{
   try { return cam_.start_recording(path); }
   catch (const std::exception&) { return false; }
}

void ProphEBSBackendSDK500::StopRecording(const char* path)
{
   try { cam_.stop_recording(path); }
   catch (const std::exception&) { /* swallowed -- see IProphEBSBackend.h contract */ }
}

// --- Biases ------------------------------------------------------------

size_t ProphEBSBackendSDK500::GetBiasCount()
{
   return biasNames_.size();
}

bool ProphEBSBackendSDK500::GetBiasInfo(size_t index, ProphEBSBiasInfo& out)
{
   if (index >= biasNames_.size())
      return false;
   try
   {
      Metavision::I_LL_Biases& biases = Facility<Metavision::I_LL_Biases>();
      const std::string& name = biasNames_[index];
      CopyToBuffer(name, out.name, sizeof(out.name));
      out.value = biases.get(name);

      Metavision::LL_Bias_Info info;
      if (biases.get_bias_info(name, info))
      {
         std::pair<int, int> range = info.get_bias_range();
         out.minValue = range.first;
         out.maxValue = range.second;
         out.hasRange = true;
      }
      else
      {
         out.minValue = 0;
         out.maxValue = 0;
         out.hasRange = false;
      }
      return true;
   }
   catch (const std::exception&)
   {
      return false;
   }
}

bool ProphEBSBackendSDK500::GetBiasValue(const char* name, int& out)
{
   try
   {
      out = Facility<Metavision::I_LL_Biases>().get(name);
      return true;
   }
   catch (const std::exception&)
   {
      return false;
   }
}

bool ProphEBSBackendSDK500::SetBiasValue(const char* name, int value, int& actualOut)
{
   try
   {
      Metavision::I_LL_Biases& biases = Facility<Metavision::I_LL_Biases>();
      bool ok = biases.set(name, value);
      // Re-read after set() -- the original OnBias()'s firmware-clamp
      // detection: set() can return true while silently clamping to a
      // tighter firmware-internal limit.
      actualOut = biases.get(name);
      return ok;
   }
   catch (const std::exception&)
   {
      actualOut = value;
      return false;
   }
}

// --- ERC -----------------------------------------------------------

ProphEBSResult ProphEBSBackendSDK500::ErcGetRange(uint32_t& minRate, uint32_t& maxRate)
{
   try
   {
      Metavision::I_ErcModule& erc = Facility<Metavision::I_ErcModule>();
      minRate = static_cast<uint32_t>(erc.get_min_supported_cd_event_rate());
      maxRate = static_cast<uint32_t>(erc.get_max_supported_cd_event_rate());
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::ErcIsEnabled(bool& out)
{
   try { out = Facility<Metavision::I_ErcModule>().is_enabled(); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::ErcEnable(bool enable)
{
   try { Facility<Metavision::I_ErcModule>().enable(enable); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::ErcGetRate(uint32_t& out)
{
   try { out = static_cast<uint32_t>(Facility<Metavision::I_ErcModule>().get_cd_event_rate()); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::ErcSetRate(uint32_t rate)
{
   try { Facility<Metavision::I_ErcModule>().set_cd_event_rate(rate); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

// --- Event trail (STC) filter ------------------------------------------

ProphEBSResult ProphEBSBackendSDK500::EventTrailFilterGetAvailableTypes(char availableTypesCsv[128])
{
   try
   {
      auto available = Facility<Metavision::I_EventTrailFilterModule>().get_available_types();
      std::string csv;
      for (const auto& t : available)
      {
         if (!csv.empty())
            csv += ",";
         csv += TrailTypeToString(t);
      }
      CopyToBuffer(csv, availableTypesCsv, 128);
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventTrailFilterGetThresholdRange(uint32_t& minT, uint32_t& maxT)
{
   try
   {
      Metavision::I_EventTrailFilterModule& filter = Facility<Metavision::I_EventTrailFilterModule>();
      minT = filter.get_min_supported_threshold();
      maxT = filter.get_max_supported_threshold();
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventTrailFilterIsEnabled(bool& out)
{
   try { out = Facility<Metavision::I_EventTrailFilterModule>().is_enabled(); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventTrailFilterEnable(bool enable)
{
   try { Facility<Metavision::I_EventTrailFilterModule>().enable(enable); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventTrailFilterGetThreshold(uint32_t& out)
{
   try { out = Facility<Metavision::I_EventTrailFilterModule>().get_threshold(); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventTrailFilterSetThreshold(uint32_t value)
{
   try { Facility<Metavision::I_EventTrailFilterModule>().set_threshold(value); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventTrailFilterGetType(char typeOut[32])
{
   try
   {
      CopyToBuffer(TrailTypeToString(Facility<Metavision::I_EventTrailFilterModule>().get_type()), typeOut, 32);
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventTrailFilterSetType(const char* type)
{
   try
   {
      Metavision::I_EventTrailFilterModule::Type t;
      if (!TrailTypeFromString(type ? type : "", t))
         return ProphEBSResult::Error;
      Facility<Metavision::I_EventTrailFilterModule>().set_type(t);
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

// --- Anti-flicker --------------------------------------------------

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerGetThresholdRanges(
   uint32_t& minStart, uint32_t& maxStart, uint32_t& minStop, uint32_t& maxStop)
{
   try
   {
      Metavision::I_AntiFlickerModule& af = Facility<Metavision::I_AntiFlickerModule>();
      minStart = af.get_min_supported_start_threshold();
      maxStart = af.get_max_supported_start_threshold();
      minStop = af.get_min_supported_stop_threshold();
      maxStop = af.get_max_supported_stop_threshold();
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerGetDutyCycleRange(float& minDc, float& maxDc)
{
   try
   {
      Metavision::I_AntiFlickerModule& af = Facility<Metavision::I_AntiFlickerModule>();
      minDc = af.get_min_supported_duty_cycle();
      maxDc = af.get_max_supported_duty_cycle();
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerGetFrequencyRange(uint32_t& minFreq, uint32_t& maxFreq)
{
   try
   {
      Metavision::I_AntiFlickerModule& af = Facility<Metavision::I_AntiFlickerModule>();
      minFreq = af.get_min_supported_frequency();
      maxFreq = af.get_max_supported_frequency();
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerIsEnabled(bool& out)
{
   try { out = Facility<Metavision::I_AntiFlickerModule>().is_enabled(); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerEnable(bool enable)
{
   try { Facility<Metavision::I_AntiFlickerModule>().enable(enable); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerGetStartThreshold(uint32_t& out)
{
   try { out = Facility<Metavision::I_AntiFlickerModule>().get_start_threshold(); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerSetStartThreshold(uint32_t value)
{
   try { Facility<Metavision::I_AntiFlickerModule>().set_start_threshold(value); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerGetStopThreshold(uint32_t& out)
{
   try { out = Facility<Metavision::I_AntiFlickerModule>().get_stop_threshold(); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerSetStopThreshold(uint32_t value)
{
   try { Facility<Metavision::I_AntiFlickerModule>().set_stop_threshold(value); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerGetDutyCycle(float& out)
{
   try { out = Facility<Metavision::I_AntiFlickerModule>().get_duty_cycle(); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerSetDutyCycle(float value)
{
   try { Facility<Metavision::I_AntiFlickerModule>().set_duty_cycle(value); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerGetFilterType(bool& isBandPass)
{
   try
   {
      isBandPass = Facility<Metavision::I_AntiFlickerModule>().get_filtering_mode()
         == Metavision::I_AntiFlickerModule::BAND_PASS;
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerSetFilterType(bool isBandPass)
{
   try
   {
      Facility<Metavision::I_AntiFlickerModule>().set_filtering_mode(
         isBandPass ? Metavision::I_AntiFlickerModule::BAND_PASS : Metavision::I_AntiFlickerModule::BAND_STOP);
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerGetFrequencyBand(uint32_t& lowFreq, uint32_t& highFreq)
{
   try
   {
      Metavision::I_AntiFlickerModule& af = Facility<Metavision::I_AntiFlickerModule>();
      lowFreq = af.get_band_low_frequency();
      highFreq = af.get_band_high_frequency();
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::AntiFlickerSetFrequencyBand(uint32_t lowFreq, uint32_t highFreq)
{
   try
   {
      return Facility<Metavision::I_AntiFlickerModule>().set_frequency_band(lowFreq, highFreq)
         ? ProphEBSResult::Ok : ProphEBSResult::Error;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

// --- Hardware trigger in/out ------------------------------------------

ProphEBSResult ProphEBSBackendSDK500::TriggerInGetAvailableChannels(char channelsCsv[64])
{
   try
   {
      auto available = Facility<Metavision::I_TriggerIn>().get_available_channels();
      std::string csv;
      for (const auto& kv : available)
      {
         if (!csv.empty())
            csv += ",";
         csv += TriggerChannelToString(kv.first);
      }
      CopyToBuffer(csv, channelsCsv, 64);
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::TriggerInIsEnabled(const char* channel, bool& out)
{
   try
   {
      Metavision::I_TriggerIn::Channel ch;
      if (!TriggerChannelFromString(channel ? channel : "", ch))
         return ProphEBSResult::Error;
      out = Facility<Metavision::I_TriggerIn>().is_enabled(ch);
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::TriggerInEnable(const char* channel, bool enable)
{
   try
   {
      Metavision::I_TriggerIn::Channel ch;
      if (!TriggerChannelFromString(channel ? channel : "", ch))
         return ProphEBSResult::Error;
      Metavision::I_TriggerIn& triggerIn = Facility<Metavision::I_TriggerIn>();
      if (enable)
         triggerIn.enable(ch);
      else
         triggerIn.disable(ch);
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::TriggerOutSetPeriod(uint32_t periodUs)
{
   try
   {
      return Facility<Metavision::I_TriggerOut>().set_period(periodUs) ? ProphEBSResult::Ok : ProphEBSResult::Error;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::TriggerOutSetDutyCycle(double dutyCycle)
{
   try
   {
      return Facility<Metavision::I_TriggerOut>().set_duty_cycle(dutyCycle)
         ? ProphEBSResult::Ok : ProphEBSResult::Error;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::TriggerOutEnable(bool enable)
{
   try
   {
      Metavision::I_TriggerOut& triggerOut = Facility<Metavision::I_TriggerOut>();
      if (enable)
         triggerOut.enable();
      else
         triggerOut.disable();
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

// --- Sensor-level event-rate band-pass filter --------------------------
// This backend (SDK 5.1.1) always has I_EventRateActivityFilterModule -- no
// Unsupported fallback needed here (unlike a hypothetical SDK43 backend).

namespace
{
   Metavision::I_EventRateActivityFilterModule::thresholds ToSdkThresholds(const ProphEBSEventRateThresholds& t)
   {
      Metavision::I_EventRateActivityFilterModule::thresholds out{};
      out.lower_bound_start = t.lowerBoundStart;
      out.lower_bound_stop = t.lowerBoundStop;
      out.upper_bound_start = t.upperBoundStart;
      out.upper_bound_stop = t.upperBoundStop;
      return out;
   }

   ProphEBSEventRateThresholds FromSdkThresholds(const Metavision::I_EventRateActivityFilterModule::thresholds& t)
   {
      ProphEBSEventRateThresholds out{};
      out.lowerBoundStart = t.lower_bound_start;
      out.lowerBoundStop = t.lower_bound_stop;
      out.upperBoundStart = t.upper_bound_start;
      out.upperBoundStop = t.upper_bound_stop;
      return out;
   }
}

ProphEBSResult ProphEBSBackendSDK500::EventRateFilterGetThresholdRanges(
   ProphEBSEventRateThresholds& minT, ProphEBSEventRateThresholds& maxT)
{
   try
   {
      Metavision::I_EventRateActivityFilterModule& filter = Facility<Metavision::I_EventRateActivityFilterModule>();
      minT = FromSdkThresholds(filter.get_min_supported_thresholds());
      maxT = FromSdkThresholds(filter.get_max_supported_thresholds());
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventRateFilterGetThresholds(ProphEBSEventRateThresholds& out)
{
   try
   {
      out = FromSdkThresholds(Facility<Metavision::I_EventRateActivityFilterModule>().get_thresholds());
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventRateFilterSetThresholds(const ProphEBSEventRateThresholds& value)
{
   try
   {
      return Facility<Metavision::I_EventRateActivityFilterModule>().set_thresholds(ToSdkThresholds(value))
         ? ProphEBSResult::Ok : ProphEBSResult::Error;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventRateFilterIsEnabled(bool& out)
{
   try { out = Facility<Metavision::I_EventRateActivityFilterModule>().is_enabled(); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::EventRateFilterEnable(bool enable)
{
   try { Facility<Metavision::I_EventRateActivityFilterModule>().enable(enable); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

// --- Sync mode -----------------------------------------------------

ProphEBSResult ProphEBSBackendSDK500::SetSyncMode(const char* mode)
{
   try
   {
      Metavision::I_CameraSynchronization& sync = Facility<Metavision::I_CameraSynchronization>();
      std::string m = mode ? mode : "";
      bool ok;
      if (m == "Master")
         ok = sync.set_mode_master();
      else if (m == "Slave")
         ok = sync.set_mode_slave();
      else
         ok = sync.set_mode_standalone();
      return ok ? ProphEBSResult::Ok : ProphEBSResult::Error;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

// --- Hardware ROI ----------------------------------------------------

ProphEBSResult ProphEBSBackendSDK500::RoiSetWindow(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
{
   try
   {
      Metavision::I_ROI& roi = Facility<Metavision::I_ROI>();
      roi.set_mode(Metavision::I_ROI::Mode::ROI);
      roi.set_window(Metavision::I_ROI::Window(
         static_cast<int>(x), static_cast<int>(y), static_cast<int>(xSize), static_cast<int>(ySize)));
      roi.enable(true);
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::RoiDisable()
{
   try { Facility<Metavision::I_ROI>().enable(false); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

// --- Hot-pixel masking -----------------------------------------------

ProphEBSResult ProphEBSBackendSDK500::GetDigitalEventMaskSlotCount(size_t& out)
{
   try
   {
      out = Facility<Metavision::I_DigitalEventMask>().get_pixel_masks().size();
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::ApplyDigitalEventMask(const unsigned* xs, const unsigned* ys, size_t count)
{
   try
   {
      Metavision::I_DigitalEventMask& dem = Facility<Metavision::I_DigitalEventMask>();
      const std::vector<Metavision::I_DigitalEventMask::I_PixelMaskPtr>& slots = dem.get_pixel_masks();
      size_t applied = std::min(count, slots.size());
      for (size_t i = 0; i < slots.size(); i++)
      {
         if (i < applied)
            slots[i]->set_mask(xs[i], ys[i], true);
         else
            slots[i]->set_mask(0, 0, false); // clear any previously-used slot no longer needed
      }
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::ApplyRoiPixelMask(const unsigned* xs, const unsigned* ys, size_t count)
{
   try
   {
      Metavision::I_RoiPixelMask& mask = Facility<Metavision::I_RoiPixelMask>();
      mask.reset_pixels();
      for (size_t i = 0; i < count; i++)
         mask.set_pixel(xs[i], ys[i], true);
      mask.apply_pixels();
      return ProphEBSResult::Ok;
   }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

// --- Monitoring ------------------------------------------------------

ProphEBSResult ProphEBSBackendSDK500::GetTemperatureC(double& out)
{
   try { out = static_cast<double>(Facility<Metavision::I_Monitoring>().get_temperature()); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::GetIlluminationLux(double& out)
{
   try { out = static_cast<double>(Facility<Metavision::I_Monitoring>().get_illumination()); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

ProphEBSResult ProphEBSBackendSDK500::GetPixelDeadTimeUs(double& out)
{
   try { out = static_cast<double>(Facility<Metavision::I_Monitoring>().get_pixel_dead_time()); return ProphEBSResult::Ok; }
   catch (const std::exception&) { return ProphEBSResult::Unsupported; }
}

// --- Software activity-noise filter ------------------------------------

bool ProphEBSBackendSDK500::ActivityFilterConstruct(unsigned width, unsigned height, int64_t thresholdUs)
{
   try
   {
      activityFilter_ = std::make_unique<Metavision::ActivityNoiseFilterAlgorithm<>>(
         width, height, static_cast<Metavision::timestamp>(thresholdUs));
      return true;
   }
   catch (const std::exception&)
   {
      activityFilter_.reset();
      return false;
   }
}

void ProphEBSBackendSDK500::ActivityFilterSetThreshold(int64_t thresholdUs)
{
   if (activityFilter_)
      activityFilter_->set_threshold(static_cast<Metavision::timestamp>(thresholdUs));
}

void ProphEBSBackendSDK500::ActivityFilterDestroy()
{
   activityFilter_.reset();
}

void ProphEBSBackendSDK500::ActivityFilterProcess(const ProphEBSEvent* in, size_t inCount,
   ProphEBSEvent* outScratch, size_t& outCount)
{
   outCount = 0;
   if (!activityFilter_ || in == nullptr || inCount == 0)
      return;

   activityFilterInScratch_.resize(inCount);
   for (size_t i = 0; i < inCount; i++)
   {
      Metavision::EventCD& ev = activityFilterInScratch_[i];
      ev.x = in[i].x;
      ev.y = in[i].y;
      ev.p = in[i].polarity;
      ev.t = static_cast<Metavision::timestamp>(in[i].tUs);
   }

   // Metavision::ActivityNoiseFilterAlgorithm only ever removes events (it's
   // a noise filter, not a generator), so the output can never exceed
   // inCount -- the caller (the main adapter) sizes outScratch to inCount on
   // that assumption. Still capped defensively below in case a future SDK
   // version's semantics ever change.
   activityFilterOutScratch_.clear();
   activityFilterOutScratch_.reserve(inCount);
   activityFilter_->process_events(activityFilterInScratch_.begin(), activityFilterInScratch_.end(),
      std::back_inserter(activityFilterOutScratch_));

   size_t n = std::min(activityFilterOutScratch_.size(), inCount);
   for (size_t i = 0; i < n; i++)
   {
      const Metavision::EventCD& ev = activityFilterOutScratch_[i];
      outScratch[i].x = static_cast<uint16_t>(ev.x);
      outScratch[i].y = static_cast<uint16_t>(ev.y);
      outScratch[i].polarity = static_cast<int8_t>(ev.p);
      outScratch[i].tUs = static_cast<int64_t>(ev.t);
   }
   outCount = n;
}
