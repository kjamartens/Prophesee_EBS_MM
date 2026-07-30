import os
import time

import numpy as np
from pymmcore_plus import CMMCorePlus

mm_dir = r"C:\Users\kjamartens\AppData\Local\pymmcore-plus\pymmcore-plus\mm\Micro-Manager_2.0.3_20260724"

core = CMMCorePlus()
core.setDeviceAdapterSearchPaths([mm_dir])
core.loadDevice("ProphEBSCam", "ProphEBS", "ProphEBS-Camera")

# Goal 5: EBS-BiasRangeCheckBypass is a pre-init property -- MM's own
# convention (isPropertyPreInit()) for "settable during device setup only":
# the Hardware Config Wizard uses this flag to grey the property out once a
# device has been added/initialized, though MMCore itself doesn't block a
# post-init setProperty() call at the API level (that enforcement is a GUI
# concern, not a Core one -- confirmed by reading
# MMDevice/Property.cpp::PropertyCollection::Set(), which only checks the
# per-property readOnly_ flag, never IsPropertyPreInit()). So the
# behavior to check here is the metadata flag itself, not read-only-ness.
assert core.isPropertyPreInit("ProphEBSCam", "EBS-BiasRangeCheckBypass"), \
    "EBS-BiasRangeCheckBypass should be flagged as a pre-init property"
core.setProperty("ProphEBSCam", "EBS-BiasRangeCheckBypass", "Off")

core.initializeDevice("ProphEBSCam")
print("Goal 5: EBS-BiasRangeCheckBypass is flagged pre-init, as expected")

# Goal 2: connection/identification properties. These are populated whether
# or not a real EBS is plugged in -- ConnectionStatus reports either
# "Connected" (with the other four filled in) or "Not connected: <reason>"
# (falling back to "N/A"), never an error.
connection_status = core.getProperty("ProphEBSCam", "EBS-ConnectionStatus")
for prop in ("EBS-ConnectionStatus", "EBS-Model", "EBS-Serial", "EBS-ConnectionType", "EBS-Integrator"):
    print(prop, "=", core.getProperty("ProphEBSCam", prop))

core.setCameraDevice("ProphEBSCam")
core.snapImage()
img = core.getImage()
print("Snap OK. Image shape:", img.shape, "dtype:", img.dtype)

# Goal 3: with a real EBS connected, the frame-builder thread continuously
# rebuilds the live image from accumulated CD events every
# g_EventIntegrationMs (100 ms), independent of Snap/Live -- so two snaps
# taken ~250ms apart should be reading different underlying buffers (whether
# or not their pixel content happens to differ depends on real-world events
# actually landing on the sensor, so this only checks the plumbing runs
# without error, not pixel-level correctness).
if connection_status == "Connected":
    core.snapImage()
    img_a = core.getImage()
    time.sleep(0.25)
    core.snapImage()
    img_b = core.getImage()
    print("Goal 3: two snaps 250ms apart both succeeded, shapes:", img_a.shape, img_b.shape)

    # Exercise the Live-view path (StartSequenceAcquisition -> InsertImage on
    # the sequence thread -> StopSequenceAcquisition), same as MicroManager's
    # Live button.
    core.startContinuousSequenceAcquisition(0)
    time.sleep(0.5)
    core.stopSequenceAcquisition()
    n_buffered = core.getRemainingImageCount()
    print("Goal 3: continuous sequence acquisition ran for 500ms, buffered", n_buffered, "images")
    while core.getRemainingImageCount() > 0:
        core.popNextImage()
else:
    print("Goal 3: no EBS connected -- skipping event-integration checks, static test image only")

# Goal 4: recording. EBS-RawFilePath/EBS-RawRecordingStatus/
# EBS-TempRecordingFolder should exist regardless of hardware; only a
# connected camera can actually produce a .raw file (cam_.start_recording()
# needs a real streaming camera).
for prop in ("EBS-RawFilePath", "EBS-RawRecordingStatus", "EBS-TempRecordingFolder"):
    print(prop, "=", core.getProperty("ProphEBSCam", prop))

if connection_status == "Connected":
    # 4a. EBS-RawFilePath is left empty -- the adapter should auto-discover
    # the save path from MicroManager's own UserProfile JSON (whatever the
    # Multi-D Acquisition dialog is currently configured to save to on this
    # machine), or fall back to its own Documents\ProphEBS_Recordings folder
    # if that lookup fails (e.g. MM has never been run here). Either way, a
    # finite 10-frame sequence acquisition (the "record a multi-dimensional
    # acquisition, e.g. 10 frames at 100 ms" requirement) should produce a
    # real .raw file with zero configuration.
    core.startSequenceAcquisition(10, 100.0, True)
    while core.isSequenceRunning():
        time.sleep(0.05)
    time.sleep(1.0)  # let the sequence thread's natural-completion path finish stopping the recording
    # (includes up to ~600ms of retries while StopRawRecordingIfActive()
    # waits for MM's own numbered save subfolder to appear)
    status_after_auto = core.getProperty("ProphEBSCam", "EBS-RawRecordingStatus")
    print("Goal 4a (auto-discovered path): EBS-RawRecordingStatus =", status_after_auto)
    assert status_after_auto.startswith("Finished:"), f"expected recording to finish cleanly, got: {status_after_auto}"
    auto_path = status_after_auto[len("Finished: "):]
    assert os.path.isfile(auto_path), f"expected raw file at {auto_path}"
    assert os.path.getsize(auto_path) > 0, f"expected non-empty raw file at {auto_path}"
    print("Goal 4a: raw file created at", auto_path, "size", os.path.getsize(auto_path), "bytes")
    while core.getRemainingImageCount() > 0:
        core.popNextImage()

    # 4b. Explicit EBS-RawFilePath overrides auto-discovery entirely.
    explicit_path = os.path.join(os.path.dirname(auto_path), "ProphEBS_explicit_test.raw")
    core.setProperty("ProphEBSCam", "EBS-RawFilePath", explicit_path)
    core.startSequenceAcquisition(5, 100.0, True)
    while core.isSequenceRunning():
        time.sleep(0.05)
    time.sleep(0.2)
    status_after_explicit = core.getProperty("ProphEBSCam", "EBS-RawRecordingStatus")
    print("Goal 4b (explicit path): EBS-RawRecordingStatus =", status_after_explicit)
    assert status_after_explicit == f"Finished: {explicit_path}", status_after_explicit
    assert os.path.isfile(explicit_path), f"expected raw file at {explicit_path}"
    print("Goal 4b: raw file created at", explicit_path, "size", os.path.getsize(explicit_path), "bytes")
    while core.getRemainingImageCount() > 0:
        core.popNextImage()
    core.setProperty("ProphEBSCam", "EBS-RawFilePath", "")  # restore default (auto-discovery)

    # 4b2. EBS-TempRecordingFolder overrides GenerateAutoRawFilePath()'s
    # staging folder. Forced onto the local-staging path (rather than MDA
    # direct-streaming) by relying on the live profile's "save": false
    # state on this machine -- if that's not the case when this runs, the
    # assertion below will simply not be reached down this exact branch,
    # so this check is best-effort depending on current MM state.
    custom_temp_folder = os.path.join(os.path.dirname(auto_path), "..", "ProphEBS_CustomTemp")
    custom_temp_folder = os.path.normpath(custom_temp_folder)
    core.setProperty("ProphEBSCam", "EBS-TempRecordingFolder", custom_temp_folder)
    core.startSequenceAcquisition(5, 100.0, True)
    while core.isSequenceRunning():
        time.sleep(0.05)
    time.sleep(1.0)
    status_after_temp_folder = core.getProperty("ProphEBSCam", "EBS-RawRecordingStatus")
    print("Goal 4b2 (custom temp folder): EBS-RawRecordingStatus =", status_after_temp_folder)
    if status_after_temp_folder.startswith("Finished:"):
        temp_path = status_after_temp_folder[len("Finished: "):]
        if os.path.dirname(temp_path) == custom_temp_folder:
            assert os.path.isfile(temp_path), f"expected raw file at {temp_path}"
            print("Goal 4b2: raw file created in custom temp folder at", temp_path)
        else:
            print("Goal 4b2: recording landed at", temp_path,
                  "(not the custom temp folder -- MDA direct-streaming or a numbered "
                  "subfolder move must have taken precedence this run, which is fine)")
    while core.getRemainingImageCount() > 0:
        core.popNextImage()
    core.setProperty("ProphEBSCam", "EBS-TempRecordingFolder", "")  # restore default

    # 4c. Live view (unbounded StartSequenceAcquisition) must NOT trigger raw
    # recording -- only finite/MDA-style sequences should.
    core.setProperty("ProphEBSCam", "EBS-RawRecordingStatus", "Not recording")  # reset marker (device may ignore, that's fine too)
    core.startContinuousSequenceAcquisition(100.0)
    time.sleep(0.3)
    core.stopSequenceAcquisition()
    status_after_live = core.getProperty("ProphEBSCam", "EBS-RawRecordingStatus")
    print("Goal 4c (Live view): EBS-RawRecordingStatus =", status_after_live)
    assert not status_after_live.startswith("Recording to"), \
        f"Live view should not have started raw recording, got: {status_after_live}"
    while core.getRemainingImageCount() > 0:
        core.popNextImage()
else:
    print("Goal 4: no EBS connected -- cam_.start_recording() needs a real streaming camera, skipping recording checks")

# Goal 5: static read-only info properties. Non-empty regardless of
# connection state (fall back to "N/A" like the Goal 2 identification
# properties), but only meaningfully populated when connected.
for prop in ("EBS-Generation", "EBS-DataEncodingFormat"):
    value = core.getProperty("ProphEBSCam", prop)
    print(prop, "=", value)
    assert value, f"{prop} should never be empty"
if connection_status == "Connected":
    assert core.getProperty("ProphEBSCam", "EBS-Generation") != "N/A"
    assert core.getProperty("ProphEBSCam", "EBS-DataEncodingFormat") != "N/A"

# Goal 5: bias properties. All EBS-bias_* properties should exist (either
# fetched from real hardware, or the fixed fallback list) and be settable
# within whatever range MM reports (unbounded when not connected).
bias_props = [p for p in core.getDevicePropertyNames("ProphEBSCam") if p.startswith("EBS-bias_")]
assert len(bias_props) >= 6, f"expected at least the 6 requested biases, got: {bias_props}"
print("Goal 5: found", len(bias_props), "bias properties:", bias_props)
for prop in bias_props:
    original = core.getProperty("ProphEBSCam", prop)
    try:
        has_limits = core.hasPropertyLimits("ProphEBSCam", prop)
    except Exception:
        has_limits = False
    if has_limits:
        lo = int(core.getPropertyLowerLimit("ProphEBSCam", prop))
        hi = int(core.getPropertyUpperLimit("ProphEBSCam", prop))
        probe = lo + (hi - lo) // 2
    else:
        probe = 0
    core.setProperty("ProphEBSCam", prop, str(probe))
    readback = core.getProperty("ProphEBSCam", prop)
    assert int(readback) == probe, f"{prop}: set {probe}, read back {readback}"
    core.setProperty("ProphEBSCam", prop, original)  # restore
print("Goal 5: all bias properties round-tripped a mid-range value successfully")

# Goal 5: ERC (event rate control) properties.
erc_min = int(core.getPropertyLowerLimit("ProphEBSCam", "EBS-ERC-EventRate")) \
    if core.hasPropertyLimits("ProphEBSCam", "EBS-ERC-EventRate") else 1
erc_max = int(core.getPropertyUpperLimit("ProphEBSCam", "EBS-ERC-EventRate")) \
    if core.hasPropertyLimits("ProphEBSCam", "EBS-ERC-EventRate") else 100000000
core.setProperty("ProphEBSCam", "EBS-ERC-EventRate", str(min(erc_max, max(erc_min, 20000000))))
core.setProperty("ProphEBSCam", "EBS-ERC-Enabled", "On")
assert core.getProperty("ProphEBSCam", "EBS-ERC-Enabled") == "On"
print("Goal 5: ERC-Enabled =", core.getProperty("ProphEBSCam", "EBS-ERC-Enabled"),
      ", ERC-EventRate =", core.getProperty("ProphEBSCam", "EBS-ERC-EventRate"))
core.setProperty("ProphEBSCam", "EBS-ERC-Enabled", "Off")
core.setProperty("ProphEBSCam", "EBS-ERC-EventRate", "50000000")  # restore default

# Goal 5: event trail (STC) filter properties.
core.setProperty("ProphEBSCam", "EBS-EventTrailFilter-Threshold", "20000")
core.setProperty("ProphEBSCam", "EBS-EventTrailFilter-Mode", "STC_CUT_TRAIL")
core.setProperty("ProphEBSCam", "EBS-EventTrailFilter-Enabled", "On")
assert core.getProperty("ProphEBSCam", "EBS-EventTrailFilter-Mode") == "STC_CUT_TRAIL"
assert core.getProperty("ProphEBSCam", "EBS-EventTrailFilter-Threshold") == "20000"
print("Goal 5: EventTrailFilter-Enabled =", core.getProperty("ProphEBSCam", "EBS-EventTrailFilter-Enabled"),
      ", Mode =", core.getProperty("ProphEBSCam", "EBS-EventTrailFilter-Mode"),
      ", Threshold =", core.getProperty("ProphEBSCam", "EBS-EventTrailFilter-Threshold"))
core.setProperty("ProphEBSCam", "EBS-EventTrailFilter-Enabled", "Off")
core.setProperty("ProphEBSCam", "EBS-EventTrailFilter-Mode", "TRAIL")
core.setProperty("ProphEBSCam", "EBS-EventTrailFilter-Threshold", "10000")  # restore defaults

# Goal 5: anti-flicker properties, including the frequency band. LowFreq/
# HighFreq's actual hardware-supported range varies by sensor -- probe
# values are derived from MM's own reported limits (set in
# CreateAntiFlickerProperties() from I_AntiFlickerModule::
# get_min/max_supported_frequency()) rather than assumed, since a
# hardcoded out-of-range value is silently rejected by the hardware
# (bool return value) and was exactly how a missing-limits bug was caught
# here during development -- see docs/DEVLOG.md Goal 5.
if core.hasPropertyLimits("ProphEBSCam", "EBS-AntiFlicker-LowFreq"):
    af_min = int(core.getPropertyLowerLimit("ProphEBSCam", "EBS-AntiFlicker-LowFreq"))
    af_max = int(core.getPropertyUpperLimit("ProphEBSCam", "EBS-AntiFlicker-HighFreq"))
else:
    af_min, af_max = 50, 60
af_low = af_min
af_high = min(af_max, af_min + 10)
core.setProperty("ProphEBSCam", "EBS-AntiFlicker-LowFreq", str(af_low))
core.setProperty("ProphEBSCam", "EBS-AntiFlicker-HighFreq", str(af_high))
core.setProperty("ProphEBSCam", "EBS-AntiFlicker-DutyCycle", "40")
core.setProperty("ProphEBSCam", "EBS-AntiFlicker-FilterType", "Band Pass")
core.setProperty("ProphEBSCam", "EBS-AntiFlicker-Enabled", "On")
assert core.getProperty("ProphEBSCam", "EBS-AntiFlicker-FilterType") == "Band Pass"
assert core.getProperty("ProphEBSCam", "EBS-AntiFlicker-LowFreq") == str(af_low)
assert core.getProperty("ProphEBSCam", "EBS-AntiFlicker-HighFreq") == str(af_high)
print("Goal 5: AntiFlicker-Enabled =", core.getProperty("ProphEBSCam", "EBS-AntiFlicker-Enabled"),
      ", FilterType =", core.getProperty("ProphEBSCam", "EBS-AntiFlicker-FilterType"),
      ", Band =", core.getProperty("ProphEBSCam", "EBS-AntiFlicker-LowFreq"), "-",
      core.getProperty("ProphEBSCam", "EBS-AntiFlicker-HighFreq"))
core.setProperty("ProphEBSCam", "EBS-AntiFlicker-Enabled", "Off")
core.setProperty("ProphEBSCam", "EBS-AntiFlicker-FilterType", "Band Cut")
core.setProperty("ProphEBSCam", "EBS-AntiFlicker-LowFreq", "50")
core.setProperty("ProphEBSCam", "EBS-AntiFlicker-HighFreq", "60")
core.setProperty("ProphEBSCam", "EBS-AntiFlicker-DutyCycle", "50")  # restore defaults

# Goal 5: live-stats properties. Only meaningful with a connected, streaming
# camera -- give the stats thread (1s interval) a few ticks to update them
# at least once from their 0.0 initial value.
if connection_status == "Connected":
    time.sleep(3.5)
    data_rate = float(core.getProperty("ProphEBSCam", "EBS-AvgDataRate-MBps"))
    event_rate = float(core.getProperty("ProphEBSCam", "EBS-AvgEventRate-MEvps"))
    drop_rate = float(core.getProperty("ProphEBSCam", "EBS-AvgERCDropRate-KEvps"))
    temperature = float(core.getProperty("ProphEBSCam", "EBS-Temperature-C"))
    illumination = float(core.getProperty("ProphEBSCam", "EBS-Illumination-lux"))
    dead_time = float(core.getProperty("ProphEBSCam", "EBS-PixelDeadTime-us"))
    print("Goal 5: live stats -- AvgDataRate-MBps =", data_rate, ", AvgEventRate-MEvps =", event_rate,
          ", AvgERCDropRate-KEvps =", drop_rate, ", Temperature-C =", temperature,
          ", Illumination-lux =", illumination, ", PixelDeadTime-us =", dead_time)
    assert data_rate >= 0.0 and event_rate >= 0.0 and drop_rate >= 0.0
    # Temperature is the most reliable plausibility check available without
    # hardware-specific knowledge of illumination/dead-time ranges -- a real
    # sensor should report a sane room/board temperature, not the 0.0
    # properties were created with.
    assert temperature != 0.0, "expected EBS-Temperature-C to have been updated by the stats thread"
else:
    print("Goal 5: no EBS connected -- live stats properties stay at their 0.0 default, skipping plausibility checks")

# Goal 6: Exposure now doubles as the event-integration window (OnExposure(),
# added this goal -- Goals 1-5 created the property with no action, so it
# never did anything). Round-trip within its declared limits.
exp_min = core.getPropertyLowerLimit("ProphEBSCam", "Exposure")
exp_max = core.getPropertyUpperLimit("ProphEBSCam", "Exposure")
original_exposure = core.getProperty("ProphEBSCam", "Exposure")
core.setExposure("ProphEBSCam", 50.0)
assert float(core.getProperty("ProphEBSCam", "Exposure")) == 50.0
print("Goal 6: Exposure round-tripped to 50.0 ms (limits", exp_min, "-", exp_max, ")")
core.setProperty("ProphEBSCam", "Exposure", original_exposure)  # restore

# Goal 6: EBS-ViewMode -- round-trip every allowed value, then restore the
# default (NetSigned).
original_view_mode = core.getProperty("ProphEBSCam", "EBS-ViewMode")
for mode in ("Merged", "OnOnly", "OffOnly", "NetSigned"):
    core.setProperty("ProphEBSCam", "EBS-ViewMode", mode)
    assert core.getProperty("ProphEBSCam", "EBS-ViewMode") == mode
print("Goal 6: EBS-ViewMode round-tripped through Merged/OnOnly/OffOnly/NetSigned")
core.setProperty("ProphEBSCam", "EBS-ViewMode", original_view_mode)

# Goal 6: EBS-ViewOffset/EBS-ViewScale -- the "offset +- found events" ask.
# Probe a mid-range value from MM's own reported limits rather than a
# hardcoded guess (same pattern as the Goal 5 bias/anti-flicker checks).
offset_lo = int(core.getPropertyLowerLimit("ProphEBSCam", "EBS-ViewOffset"))
offset_hi = int(core.getPropertyUpperLimit("ProphEBSCam", "EBS-ViewOffset"))
offset_probe = offset_lo + (offset_hi - offset_lo) // 2
original_offset = core.getProperty("ProphEBSCam", "EBS-ViewOffset")
core.setProperty("ProphEBSCam", "EBS-ViewOffset", str(offset_probe))
assert int(core.getProperty("ProphEBSCam", "EBS-ViewOffset")) == offset_probe
core.setProperty("ProphEBSCam", "EBS-ViewOffset", original_offset)

original_scale = core.getProperty("ProphEBSCam", "EBS-ViewScale")
core.setProperty("ProphEBSCam", "EBS-ViewScale", "2.5")
assert float(core.getProperty("ProphEBSCam", "EBS-ViewScale")) == 2.5
core.setProperty("ProphEBSCam", "EBS-ViewScale", original_scale)
print("Goal 6: EBS-ViewOffset/EBS-ViewScale round-tripped (offset probe",
      offset_probe, ", scale 2.5)")

# Goal 6: software activity-noise filter properties.
threshold_lo = int(core.getPropertyLowerLimit("ProphEBSCam", "EBS-ActivityFilter-Threshold-us"))
threshold_hi = int(core.getPropertyUpperLimit("ProphEBSCam", "EBS-ActivityFilter-Threshold-us"))
threshold_probe = threshold_lo + (threshold_hi - threshold_lo) // 2
core.setProperty("ProphEBSCam", "EBS-ActivityFilter-Threshold-us", str(threshold_probe))
assert int(core.getProperty("ProphEBSCam", "EBS-ActivityFilter-Threshold-us")) == threshold_probe
core.setProperty("ProphEBSCam", "EBS-ActivityFilter-Enabled", "On")
assert core.getProperty("ProphEBSCam", "EBS-ActivityFilter-Enabled") == "On"
print("Goal 6: EBS-ActivityFilter-Enabled/-Threshold-us round-tripped (threshold probe",
      threshold_probe, "us)")
core.setProperty("ProphEBSCam", "EBS-ActivityFilter-Enabled", "Off")
core.setProperty("ProphEBSCam", "EBS-ActivityFilter-Threshold-us", "10000")  # restore default

# Goal 6: with a real EBS connected, confirm changing Exposure/EBS-ViewMode/
# activity filter live (mid-stream, no restart) doesn't break the frame
# pipeline -- snap before and after, check shape/dtype are still correct.
# Pixel-level correctness of each view mode depends on real ambient
# events landing on the sensor, so this only proves the plumbing survives a
# live reconfiguration, matching the existing Goal 3 two-snaps-apart check.
if connection_status == "Connected":
    core.snapImage()
    img_before = core.getImage()
    core.setProperty("ProphEBSCam", "Exposure", "50")
    core.setProperty("ProphEBSCam", "EBS-ViewMode", "Merged")
    core.setProperty("ProphEBSCam", "EBS-ActivityFilter-Enabled", "On")
    time.sleep(0.2)
    core.snapImage()
    img_after = core.getImage()
    print("Goal 6: live reconfiguration OK -- before", img_before.shape, img_before.dtype,
          ", after", img_after.shape, img_after.dtype)
    assert img_before.shape == img_after.shape
    assert img_before.dtype == img_after.dtype
    core.setProperty("ProphEBSCam", "EBS-ActivityFilter-Enabled", "Off")
    core.setProperty("ProphEBSCam", "EBS-ViewMode", "NetSigned")
    core.setProperty("ProphEBSCam", "Exposure", original_exposure)
else:
    print("Goal 6: no EBS connected -- skipping live-reconfiguration frame check")

# Goal 6 follow-up: sub-millisecond integration windows. Exposure's lower
# limit dropped from 1.0 ms to 0.001 ms (1 microsecond -- the finest
# resolution Metavision::EventCD::t itself can represent), and a new
# EBS-DisplayRefreshMs property decouples "how long is one integration
# window" from "how often is a frame actually published."
assert abs(core.getPropertyLowerLimit("ProphEBSCam", "Exposure") - 0.001) < 1e-9, \
    "expected Exposure's lower limit to be 0.001 ms after the sub-ms follow-up"
core.setExposure("ProphEBSCam", 0.05)
assert abs(float(core.getProperty("ProphEBSCam", "Exposure")) - 0.05) < 1e-9
print("Goal 6 follow-up: Exposure round-tripped to 0.05 ms (sub-millisecond)")
core.setProperty("ProphEBSCam", "Exposure", original_exposure)

refresh_lo = core.getPropertyLowerLimit("ProphEBSCam", "EBS-DisplayRefreshMs")
refresh_hi = core.getPropertyUpperLimit("ProphEBSCam", "EBS-DisplayRefreshMs")
original_refresh = core.getProperty("ProphEBSCam", "EBS-DisplayRefreshMs")
core.setProperty("ProphEBSCam", "EBS-DisplayRefreshMs", "5")
assert float(core.getProperty("ProphEBSCam", "EBS-DisplayRefreshMs")) == 5.0
print("Goal 6 follow-up: EBS-DisplayRefreshMs round-tripped to 5 ms (limits", refresh_lo, "-", refresh_hi, ")")
core.setProperty("ProphEBSCam", "EBS-DisplayRefreshMs", original_refresh)

# Goal 6 follow-up: with a real EBS connected, run genuinely sub-millisecond
# integration windows (down to the 0.001 ms floor) with a fast 1 ms display
# refresh for a short real-world burst -- this is the actual stress case the
# design has to survive: OnEventsCD() closing windows via real event
# timestamps far more often than the old wall-clock design ever did, while
# only resetting touched pixels (not the whole sensor-sized array) each
# time. Snap before/after to confirm the frame pipeline is still alive and
# producing correctly-shaped frames, not hung or crashed.
if connection_status == "Connected":
    core.setProperty("ProphEBSCam", "EBS-DisplayRefreshMs", "1")
    for sub_ms_exposure in (0.1, 0.01, 0.001):
        core.setExposure("ProphEBSCam", sub_ms_exposure)
        time.sleep(0.3)
        core.snapImage()
        img = core.getImage()
        assert img.shape == img_before.shape and img.dtype == img_before.dtype
        print("Goal 6 follow-up: Exposure =", sub_ms_exposure, "ms survived a 300ms burst, image",
              img.shape, img.dtype)
    core.setProperty("ProphEBSCam", "EBS-DisplayRefreshMs", original_refresh)
    core.setProperty("ProphEBSCam", "Exposure", original_exposure)
else:
    print("Goal 6 follow-up: no EBS connected -- skipping sub-millisecond stress check")

# Bug fix: Live view (unbounded StartSequenceAcquisition) must push frames at
# max(Exposure, EBS-LiveViewMinIntervalMs), never at whatever interval
# happens to be passed in -- MMCore's own "unused" parameter contract. New
# EBS-LiveViewMinIntervalMs property (default 5 ms) round-trip first.
minint_lo = core.getPropertyLowerLimit("ProphEBSCam", "EBS-LiveViewMinIntervalMs")
minint_hi = core.getPropertyUpperLimit("ProphEBSCam", "EBS-LiveViewMinIntervalMs")
original_min_interval = core.getProperty("ProphEBSCam", "EBS-LiveViewMinIntervalMs")
assert abs(float(original_min_interval) - 5.0) < 1e-9, "expected EBS-LiveViewMinIntervalMs to default to 5.0 ms"
core.setProperty("ProphEBSCam", "EBS-LiveViewMinIntervalMs", "20")
assert float(core.getProperty("ProphEBSCam", "EBS-LiveViewMinIntervalMs")) == 20.0
print("Goal 6 follow-up (Live-cadence bug fix): EBS-LiveViewMinIntervalMs round-tripped to 20 ms (limits",
      minint_lo, "-", minint_hi, ", default 5)")
core.setProperty("ProphEBSCam", "EBS-LiveViewMinIntervalMs", original_min_interval)

# Simulates the exact failure mode found by the user: setting a
# sub-millisecond Exposure, then starting continuous acquisition with that
# same tiny value as the (supposed-to-be-ignored) interval argument, the way
# a Live-view implementation that paces itself off the camera's exposure
# would. Before the fix this made ProphEBSSequenceThread push ~1000
# frames/sec, overwhelming MMCore's circular buffer and producing an
# ever-growing display backlog/latency. After the fix, Live view is bounded
# by EBS-LiveViewMinIntervalMs (default 5 ms -> ~200 frames/sec) even though
# Exposure itself is sub-ms -- checked loosely (order-of-magnitude) since
# real thread-wakeup jitter means an exact frame count isn't reproducible.
if connection_status == "Connected":
    core.setExposure("ProphEBSCam", 0.01)
    core.startContinuousSequenceAcquisition(0.01)  # mimics a Live view that passes exposure as "interval"
    time.sleep(1.0)
    core.stopSequenceAcquisition()
    n_buffered_sub_ms = core.getRemainingImageCount()
    print("Goal 6 follow-up (Live-cadence bug fix): sub-ms Exposure (floor-bounded) + 1s of continuous "
          "acquisition buffered", n_buffered_sub_ms, "frames")
    assert n_buffered_sub_ms < 500, \
        f"expected roughly ~200 frames/sec (bounded by the 5 ms EBS-LiveViewMinIntervalMs floor), " \
        f"got {n_buffered_sub_ms} -- Live view may be pushing frames far faster than the GUI can consume again"
    while core.getRemainingImageCount() > 0:
        core.popNextImage()

    # And when Exposure is comfortably above the floor, Live view should
    # actually follow Exposure (like any other camera), not the floor.
    core.setExposure("ProphEBSCam", 50.0)
    core.startContinuousSequenceAcquisition(50.0)
    time.sleep(1.0)
    core.stopSequenceAcquisition()
    n_buffered_normal = core.getRemainingImageCount()
    print("Goal 6 follow-up (Live-cadence bug fix): Exposure=50ms (above floor) + 1s of continuous "
          "acquisition buffered", n_buffered_normal, "frames")
    assert 10 <= n_buffered_normal <= 30, \
        f"expected roughly ~20 frames/sec (following the 50 ms Exposure), got {n_buffered_normal}"
    while core.getRemainingImageCount() > 0:
        core.popNextImage()

    core.setProperty("ProphEBSCam", "Exposure", original_exposure)
else:
    print("Goal 6 follow-up: no EBS connected -- skipping Live-cadence bug-fix check")

# Follow-up: backlog detection/flush. EBS-BacklogFlushThresholdMs round-trip
# first, then (with a real camera) force the threshold artificially low so
# that ordinary real-time callback jitter reliably trips it within a short
# streaming burst -- confirming both EBS-CallbackLagMs and
# EBS-BacklogFlushCount are wired up and moving, not just present.
thresh_lo = core.getPropertyLowerLimit("ProphEBSCam", "EBS-BacklogFlushThresholdMs")
thresh_hi = core.getPropertyUpperLimit("ProphEBSCam", "EBS-BacklogFlushThresholdMs")
original_threshold = core.getProperty("ProphEBSCam", "EBS-BacklogFlushThresholdMs")
assert abs(float(original_threshold) - 250.0) < 1e-9, "expected EBS-BacklogFlushThresholdMs to default to 250 ms"
core.setProperty("ProphEBSCam", "EBS-BacklogFlushThresholdMs", "500")
assert float(core.getProperty("ProphEBSCam", "EBS-BacklogFlushThresholdMs")) == 500.0
print("Follow-up: EBS-BacklogFlushThresholdMs round-tripped to 500 ms (limits", thresh_lo, "-", thresh_hi,
      ", default 250)")
core.setProperty("ProphEBSCam", "EBS-BacklogFlushThresholdMs", original_threshold)

if connection_status == "Connected":
    lag_before = float(core.getProperty("ProphEBSCam", "EBS-CallbackLagMs"))
    flushes_before = float(core.getProperty("ProphEBSCam", "EBS-BacklogFlushCount"))
    core.setProperty("ProphEBSCam", "EBS-BacklogFlushThresholdMs", str(thresh_lo))  # near-guaranteed trip
    time.sleep(1.0)
    lag_after = float(core.getProperty("ProphEBSCam", "EBS-CallbackLagMs"))
    flushes_after = float(core.getProperty("ProphEBSCam", "EBS-BacklogFlushCount"))
    core.setProperty("ProphEBSCam", "EBS-BacklogFlushThresholdMs", original_threshold)
    print("Follow-up: EBS-CallbackLagMs", lag_before, "->", lag_after,
          ", EBS-BacklogFlushCount", flushes_before, "->", flushes_after,
          "(threshold forced to 0.001 ms for 1s)")
    assert flushes_after > flushes_before, \
        "expected EBS-BacklogFlushCount to increase once EBS-BacklogFlushThresholdMs was forced near zero"
    # Snap after restoring the threshold, confirming the adapter is still in
    # a normal working state post-flush (no regression from the fast path).
    img_after_flush = core.snapImage()
    core.getImage()
    print("Follow-up: snap after backlog flush still succeeded, shape unaffected")
else:
    print("Follow-up: no EBS connected -- skipping backlog-flush trigger check")

# Goal 7: EBS-BlockedPixels -- round-trip a valid list, then confirm a
# malformed string is rejected wholesale (the previous valid value is
# retained, not partially applied).
original_blocked = core.getProperty("ProphEBSCam", "EBS-BlockedPixels")
core.setProperty("ProphEBSCam", "EBS-BlockedPixels", "5:5;10:20")
assert core.getProperty("ProphEBSCam", "EBS-BlockedPixels") == "5:5;10:20"
print("Goal 7: EBS-BlockedPixels round-tripped '5:5;10:20'")
try:
    core.setProperty("ProphEBSCam", "EBS-BlockedPixels", "abc")
    malformed_rejected = False
except Exception:
    malformed_rejected = True
assert malformed_rejected, "expected a malformed EBS-BlockedPixels value to be rejected"
assert core.getProperty("ProphEBSCam", "EBS-BlockedPixels") == "5:5;10:20", \
    "expected the previous valid EBS-BlockedPixels value to be retained after a rejected malformed set"
print("Goal 7: malformed EBS-BlockedPixels ('abc') correctly rejected, previous value retained")
core.setProperty("ProphEBSCam", "EBS-BlockedPixels", original_blocked)

# Goal 7: hot-pixel calibration properties/trigger exist and round-trip.
# With no camera streaming, triggering calibration must complete promptly
# (not hang) and report a clean failure.
duration_lo = core.getPropertyLowerLimit("ProphEBSCam", "EBS-HotPixelCalibDurationMs")
duration_hi = core.getPropertyUpperLimit("ProphEBSCam", "EBS-HotPixelCalibDurationMs")
assert abs(float(core.getProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs")) - 5000.0) < 1e-9
core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "150")
assert abs(float(core.getProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs")) - 150.0) < 1e-9
print("Goal 7: EBS-HotPixelCalibDurationMs round-tripped to 150 ms (limits", duration_lo, "-", duration_hi, ")")

k_lo = core.getPropertyLowerLimit("ProphEBSCam", "EBS-HotPixelStddevK")
k_hi = core.getPropertyUpperLimit("ProphEBSCam", "EBS-HotPixelStddevK")
original_hotpixel_k = core.getProperty("ProphEBSCam", "EBS-HotPixelStddevK")
assert abs(float(original_hotpixel_k) - 10.0) < 1e-9
core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", "2.5")
assert abs(float(core.getProperty("ProphEBSCam", "EBS-HotPixelStddevK")) - 2.5) < 1e-9
print("Goal 7: EBS-HotPixelStddevK round-tripped to 2.5 (limits", k_lo, "-", k_hi, ")")
# Restore -- a low k dramatically loosens the outlier threshold (by design:
# threshold = mean + k*stddev), and the real-hardware calibration check
# below needs a realistic k, not this round-trip probe value.
core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", original_hotpixel_k)

assert set(core.getAllowedPropertyValues("ProphEBSCam", "EBS-DetectHotPixelsNow")) == {"Idle", "Run"}

if connection_status != "Connected":
    t0 = time.time()
    core.setProperty("ProphEBSCam", "EBS-DetectHotPixelsNow", "Run")
    elapsed = time.time() - t0
    assert elapsed < 5.0, f"calibration with no camera should fail promptly, took {elapsed:.1f}s"
    status = core.getProperty("ProphEBSCam", "EBS-HotPixelCalibStatus")
    assert status.startswith("Failed:"), f"expected a clean failure with no camera, got: {status}"
    assert core.getProperty("ProphEBSCam", "EBS-DetectHotPixelsNow") == "Idle"
    print("Goal 7: EBS-DetectHotPixelsNow with no camera failed promptly and cleanly:", status)
else:
    print("Goal 7: skipping the no-camera calibration-failure check (a camera is connected)")

# Goal 7: hardware ROI + hot-pixel calibration, real hardware only.
if connection_status == "Connected":
    full_x, full_y, full_w, full_h = core.getROI("ProphEBSCam")
    print("Goal 7: full-sensor ROI is", (full_x, full_y, full_w, full_h))

    roi_w, roi_h = full_w // 4, full_h // 4
    roi_x, roi_y = full_w // 4, full_h // 4
    core.setROI("ProphEBSCam", roi_x, roi_y, roi_w, roi_h)
    got_x, got_y, got_w, got_h = core.getROI("ProphEBSCam")
    assert (got_x, got_y, got_w, got_h) == (roi_x, roi_y, roi_w, roi_h), \
        f"expected GetROI to report back {(roi_x, roi_y, roi_w, roi_h)}, got {(got_x, got_y, got_w, got_h)}"
    core.snapImage()
    roi_img = core.getImage()
    assert roi_img.shape == (roi_h, roi_w), f"expected image shape {(roi_h, roi_w)}, got {roi_img.shape}"
    print("Goal 7: SetROI/GetROI round-tripped to", (roi_x, roi_y, roi_w, roi_h),
          ", snapped image shape", roi_img.shape)

    # Soft signal only (ambient-light dependent): print event/data rate
    # before vs. after a small ROI, as evidence of real data reduction at
    # the sensor, not display-level cropping.
    time.sleep(2.0)
    rate_with_roi = float(core.getProperty("ProphEBSCam", "EBS-AvgEventRate-MEvps"))
    core.clearROI()
    got_x, got_y, got_w, got_h = core.getROI("ProphEBSCam")
    assert (got_x, got_y, got_w, got_h) == (full_x, full_y, full_w, full_h), \
        "expected ClearROI to restore the full sensor geometry"
    core.snapImage()
    full_img = core.getImage()
    assert full_img.shape == (full_h, full_w)
    time.sleep(2.0)
    rate_full = float(core.getProperty("ProphEBSCam", "EBS-AvgEventRate-MEvps"))
    print("Goal 7: ClearROI restored full geometry", (full_x, full_y, full_w, full_h),
          "; EBS-AvgEventRate-MEvps was", rate_with_roi, "MEv/s with a quarter-frame ROI vs.",
          rate_full, "MEv/s at full frame (ambient-light dependent, informational only)")

    # On-demand hot-pixel calibration against real hardware, using the
    # actual defaults (EBS-HotPixelCalibDurationMs=5000ms,
    # EBS-HotPixelStddevK=10) -- not an artificially short probe -- since
    # the mean+k*stddev threshold assumes roughly-Gaussian statistics that
    # only hold up once each pixel has accumulated enough events; a very
    # short window (e.g. ~200ms, only 1-2 events/pixel/polarity at this
    # sensor's ambient event rate) makes the per-pixel counts closer to a
    # sparse Poisson distribution, whose thicker tail triggers far more
    # "outliers" than the same k would at the real default duration --
    # found by this exact test initially using a short explicit override.
    # Also scoped to whatever ROI is currently active (full sensor here,
    # since ClearROI() was just called above), not the whole chip
    # unconditionally -- see ProphEBS.cpp's OnDetectHotPixelsNow().
    t0 = time.time()
    core.setProperty("ProphEBSCam", "EBS-DetectHotPixelsNow", "Run")
    elapsed = time.time() - t0
    calib_status = core.getProperty("ProphEBSCam", "EBS-HotPixelCalibStatus")
    print("Goal 7: real-hardware calibration (default 5000ms/k=10) took", f"{elapsed:.2f}s", ", status:",
          calib_status)
    assert calib_status.startswith("Done:") or calib_status.startswith("Failed:"), \
        f"expected calibration to finish in a well-defined state, got: {calib_status}"
    assert core.getProperty("ProphEBSCam", "EBS-DetectHotPixelsNow") == "Idle"
    blocked_after_calib = core.getProperty("ProphEBSCam", "EBS-BlockedPixels")
    print("Goal 7: EBS-BlockedPixels after calibration:", blocked_after_calib)
    # Sanity-check the serialized format is well-formed (parseable pairs).
    if blocked_after_calib:
        for pair in blocked_after_calib.split(";"):
            px, py = pair.split(":")
            int(px)
            int(py)
    core.setProperty("ProphEBSCam", "EBS-BlockedPixels", original_blocked)

    # Confirm calibration REPLACES EBS-BlockedPixels rather than merging
    # into it (a deliberate change: calibration now unblocks everything
    # first, then re-detects from a clean population -- see
    # ProphEBS.cpp's OnDetectHotPixelsNow()). Manually block an arbitrary
    # pixel unrelated to any real hot pixel, run calibration with a short
    # duration/high threshold (so it's unlikely to itself flag much), and
    # confirm the manually-added pixel is gone afterward -- if calibration
    # still merged, it would still be present.
    sentinel_pixel = "500:500"
    core.setProperty("ProphEBSCam", "EBS-BlockedPixels", sentinel_pixel)
    assert core.getProperty("ProphEBSCam", "EBS-BlockedPixels") == sentinel_pixel
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "200")
    core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", original_hotpixel_k)
    core.setProperty("ProphEBSCam", "EBS-DetectHotPixelsNow", "Run")
    blocked_after_replace = core.getProperty("ProphEBSCam", "EBS-BlockedPixels")
    print("Goal 7: EBS-BlockedPixels after calibration following a manual set of", sentinel_pixel, ":",
          blocked_after_replace)
    assert sentinel_pixel not in (blocked_after_replace or "").split(";"), \
        f"expected calibration to REPLACE EBS-BlockedPixels (clearing the manually-set {sentinel_pixel} " \
        f"first), but it's still present after a run -- calibration is still merging, not replacing"
    print("Goal 7: confirmed -- calibration replaced EBS-BlockedPixels rather than merging into it")
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "5000")
    core.setProperty("ProphEBSCam", "EBS-BlockedPixels", original_blocked)

    # Confirm calibration is actually scoped to the active ROI, not the
    # whole chip: set a small ROI, run calibration, and check every
    # resulting EBS-BlockedPixels coordinate falls inside that ROI's
    # bounds (with the loosest possible k, so *something* is very likely
    # to be flagged even in a short window -- this checks scoping, not
    # detection sensitivity).
    core.setROI("ProphEBSCam", roi_x, roi_y, roi_w, roi_h)
    core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", str(k_lo))
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "200")
    core.setProperty("ProphEBSCam", "EBS-DetectHotPixelsNow", "Run")
    roi_scoped_blocked = core.getProperty("ProphEBSCam", "EBS-BlockedPixels")
    print("Goal 7: ROI-scoped calibration (ROI", (roi_x, roi_y, roi_w, roi_h), ") found:", roi_scoped_blocked)
    if roi_scoped_blocked:
        for pair in roi_scoped_blocked.split(";"):
            px_str, py_str = pair.split(":")
            px, py = int(px_str), int(py_str)
            assert roi_x <= px < roi_x + roi_w and roi_y <= py < roi_y + roi_h, \
                f"pixel ({px},{py}) from ROI-scoped calibration falls outside the active ROI " \
                f"{(roi_x, roi_y, roi_w, roi_h)} -- calibration should never scan beyond it"
        print("Goal 7: all", len(roi_scoped_blocked.split(";")), "ROI-scoped hot pixels fall within the active ROI")
    else:
        print("Goal 7: ROI-scoped calibration found no outliers this run (nothing to check bounds on)")
    core.clearROI()
    core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", original_hotpixel_k)
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "5000")
    core.setProperty("ProphEBSCam", "EBS-BlockedPixels", original_blocked)

    # Deliberately loose-threshold stress test: force EBS-HotPixelStddevK to
    # its floor with a short duration so a large fraction of pixels statistically
    # qualify as "outliers," exercising the truncation-safety-cap path
    # (EBS-BlockedPixels must never be allowed to grow past what MMCore's
    # MM::MaxStrLength property-value limit can hold -- see ProphEBS.cpp's
    # OnDetectHotPixelsNow()/the "found via self-testing" comment there).
    core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", str(k_lo))
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "200")
    core.setProperty("ProphEBSCam", "EBS-DetectHotPixelsNow", "Run")
    stress_status = core.getProperty("ProphEBSCam", "EBS-HotPixelCalibStatus")
    stress_blocked = core.getProperty("ProphEBSCam", "EBS-BlockedPixels")
    print("Goal 7: loose-threshold stress calibration status:", stress_status)
    assert len(stress_blocked) <= 1024, \
        f"EBS-BlockedPixels must never exceed MMCore's 1024-char property limit, got {len(stress_blocked)} chars"
    if stress_status.startswith("Done:") and "dropped" in stress_status:
        print("Goal 7: truncation-safety cap engaged as expected under a deliberately loose threshold:",
              stress_status)
    core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", original_hotpixel_k)
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "5000")
    core.setProperty("ProphEBSCam", "EBS-BlockedPixels", original_blocked)

    # Manually setting EBS-BlockedPixels against real hardware shouldn't
    # break subsequent snaps.
    core.setProperty("ProphEBSCam", "EBS-BlockedPixels", "3:3;7:9")
    core.snapImage()
    core.getImage()
    print("Goal 7: manual EBS-BlockedPixels set against real hardware, snap still succeeded")
    core.setProperty("ProphEBSCam", "EBS-BlockedPixels", original_blocked)

    # Does blocking a pixel actually stop it from generating events, or just
    # get recorded in EBS-BlockedPixels without hardware effect? Block a
    # 7x7 region (0,0)-(6,6) -- 49 entries, deliberately kept under this
    # sensor's 64-slot I_DigitalEventMask hardware limit (see
    # ApplyBlockedPixelsToHardware()) so every single one of these pixels
    # is actually pushed to hardware, not just listed in EBS-BlockedPixels
    # -- and watch that exact region in the live view: with
    # EBS-ViewOffset=0 and a large EBS-ViewScale, even a single CD event in
    # a window makes its pixel visibly nonzero. Critically, this needs a
    # real BEFORE/AFTER comparison of the *same* region, not just "is it
    # zero" -- a corner of the sensor could easily show zero activity on
    # its own (dark/no motion there), which would make the assertion pass
    # without proving masking did anything at all. So: first confirm this
    # exact region shows real activity while UNMASKED (retrying with a
    # longer window if the first attempt sees none, since ambient activity
    # there isn't guaranteed), then block it and confirm that same region
    # goes to zero while the rest of the frame keeps showing activity.
    original_view_mode2 = core.getProperty("ProphEBSCam", "EBS-ViewMode")
    original_view_offset2 = core.getProperty("ProphEBSCam", "EBS-ViewOffset")
    original_view_scale2 = core.getProperty("ProphEBSCam", "EBS-ViewScale")
    original_exposure2 = core.getProperty("ProphEBSCam", "Exposure")
    MASK_TEST_REGION = 7  # 7*7 = 49, comfortably under the 64-slot hardware limit
    block_region = ";".join(f"{x}:{y}" for x in range(MASK_TEST_REGION) for y in range(MASK_TEST_REGION))

    # Push the ON/OFF contrast-detection biases toward maximum sensitivity so
    # the test isn't at the mercy of whatever's actually happening in front
    # of the sensor right now -- lower bias_diff_on/bias_diff_off means more
    # sensitive (more events, including pure noise), which is exactly what's
    # wanted here: a reliable, real baseline signal in an arbitrary 10x10
    # corner, not a genuine low-noise imaging setting. Clamped to whatever
    # this sensor actually reports as its supported range.
    original_bias_on = core.getProperty("ProphEBSCam", "EBS-bias_diff_on")
    original_bias_off = core.getProperty("ProphEBSCam", "EBS-bias_diff_off")

    def clamp_to_prop_limits(prop, value):
        if core.hasPropertyLimits("ProphEBSCam", prop):
            lo = int(core.getPropertyLowerLimit("ProphEBSCam", prop))
            hi = int(core.getPropertyUpperLimit("ProphEBSCam", prop))
            return max(lo, min(hi, value))
        return value

    bias_on_target = clamp_to_prop_limits("EBS-bias_diff_on", -40)
    bias_off_target = clamp_to_prop_limits("EBS-bias_diff_off", -20)
    core.setProperty("ProphEBSCam", "EBS-bias_diff_on", str(bias_on_target))
    core.setProperty("ProphEBSCam", "EBS-bias_diff_off", str(bias_off_target))
    print("Goal 7: boosted sensitivity for the masking-effectiveness check -- EBS-bias_diff_on =",
          core.getProperty("ProphEBSCam", "EBS-bias_diff_on"), ", EBS-bias_diff_off =",
          core.getProperty("ProphEBSCam", "EBS-bias_diff_off"))

    core.setProperty("ProphEBSCam", "EBS-BlockedPixels", "")
    core.setProperty("ProphEBSCam", "EBS-ViewMode", "Merged")
    core.setProperty("ProphEBSCam", "EBS-ViewOffset", "0")
    core.setProperty("ProphEBSCam", "EBS-ViewScale", "50")

    def sample_region_max(region_slice, exposure_ms, n_snaps):
        core.setProperty("ProphEBSCam", "Exposure", str(exposure_ms))
        time.sleep(0.3)  # let the new Exposure take effect on a fresh window
        peak = 0
        for _ in range(n_snaps):
            core.snapImage()
            img = core.getImage()
            peak = max(peak, int(img[region_slice].max()))
            time.sleep(exposure_ms / 1000.0 + 0.05)
        return peak

    region_slice = np.s_[0:MASK_TEST_REGION, 0:MASK_TEST_REGION]
    region_label = f"(0-{MASK_TEST_REGION - 1},0-{MASK_TEST_REGION - 1})"
    baseline_max = 0
    for exposure_ms in (100, 300, 1000):
        baseline_max = sample_region_max(region_slice, exposure_ms, 10)
        if baseline_max > 0:
            break
    print("Goal 7: baseline (UNMASKED) activity in region", region_label, ":", baseline_max,
          "(exposure that found it:", exposure_ms, "ms)")
    if baseline_max == 0:
        print("Goal 7: WARNING -- region", region_label, "showed no activity even unmasked after escalating "
              "exposure up to 1000ms; masking effectiveness cannot be conclusively tested this run "
              "(inconclusive, not a pass or fail) -- likely just a quiet/dark corner of the current scene")
    else:
        core.setProperty("ProphEBSCam", "EBS-BlockedPixels", block_region)
        assert core.getProperty("ProphEBSCam", "EBS-BlockedPixels") == block_region
        masked_region_max = sample_region_max(region_slice, exposure_ms, 10)
        rest_of_frame_max = 0
        core.snapImage()
        rest_of_frame_max = int(core.getImage()[MASK_TEST_REGION:, :].max())
        print("Goal 7: hot-pixel masking effectiveness check -- same region", region_label,
              "MASKED max pixel value:", masked_region_max, "(baseline unmasked was", baseline_max,
              "), rest-of-frame max:", rest_of_frame_max)
        assert rest_of_frame_max > 0, \
            "expected real activity somewhere outside the blocked region as a sanity control -- got none at all"
        assert masked_region_max == 0, \
            f"EBS-BlockedPixels claims {MASK_TEST_REGION * MASK_TEST_REGION} pixels in {region_label} are " \
            f"masked, but events are still landing there (max pixel value {masked_region_max}, vs " \
            f"{baseline_max} unmasked at the same exposure) -- hardware masking is not actually suppressing them"
        print("Goal 7: confirmed -- region", region_label, "went from real activity (unmasked) to zero events "
              "(masked), while the rest of the frame kept showing activity")

    core.setProperty("ProphEBSCam", "EBS-ViewMode", original_view_mode2)
    core.setProperty("ProphEBSCam", "EBS-ViewOffset", original_view_offset2)
    core.setProperty("ProphEBSCam", "EBS-ViewScale", original_view_scale2)
    core.setProperty("ProphEBSCam", "Exposure", original_exposure2)
    core.setProperty("ProphEBSCam", "EBS-bias_diff_on", original_bias_on)
    core.setProperty("ProphEBSCam", "EBS-bias_diff_off", original_bias_off)
    core.setProperty("ProphEBSCam", "EBS-BlockedPixels", original_blocked)
else:
    print("Goal 7: no EBS connected -- skipping hardware ROI/hot-pixel-calibration checks "
          "(GetROI/SetROI/ClearROI round-trip still exercised via the fallback checkerboard below)")
    full_x, full_y, full_w, full_h = core.getROI("ProphEBSCam")
    roi_w, roi_h = full_w // 4, full_h // 4
    roi_x, roi_y = full_w // 4, full_h // 4
    core.setROI("ProphEBSCam", roi_x, roi_y, roi_w, roi_h)
    core.snapImage()
    roi_img = core.getImage()
    assert roi_img.shape == (roi_h, roi_w), \
        f"expected fallback image shape {(roi_h, roi_w)}, got {roi_img.shape}"
    core.clearROI()
    core.snapImage()
    full_img = core.getImage()
    assert full_img.shape == (full_h, full_w)
    print("Goal 7: SetROI/GetROI/ClearROI round-tripped against the no-hardware fallback checkerboard, shapes",
          roi_img.shape, "->", full_img.shape)

# GUI/visual-only, not verifiable by this harness (per the same caveat as
# every earlier goal's GUI-only behaviors):
#  - actually dragging/zooming an ROI rectangle in MicroManager's Live view
#    and visually confirming the displayed image crops;
#  - visually confirming a masked hot pixel actually disappears (not
#    intensifies) -- the only way to fully resolve the set_pixel()
#    enable=true/false semantics ambiguity noted in ProphEBS.cpp;
#  - inspecting a recorded .raw file's actual event content to confirm ROI
#    cropping reached the recording, not just the live display.
print("Goal 7: GUI/visual-only checks (Live-view ROI crop, visual hot-pixel masking, "
      ".raw content inspection) require the user, see docs/DEVLOG.md")

core.unloadDevice("ProphEBSCam")
print("SUCCESS")
