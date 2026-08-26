import os
import sys
import time

import numpy as np
from pymmcore_plus import CMMCorePlus
from pymmcore_plus._util import USER_DATA_DIR


def find_active_mm_dir() -> str:
    """Locate the active pymmcore-plus-managed MicroManager install.

    Mirrors what `mmcore list` reports as "(active)": the most recently
    installed Micro-Manager_* folder under pymmcore-plus's managed mm/
    directory. Override with the MM_DIR environment variable if you're
    pointing at a different install (e.g. one set up via the no-admin-rights
    innounp workaround in docs/BUILD_AND_USAGE.md).
    """
    env_override = os.environ.get("MM_DIR")
    if env_override:
        return env_override
    mm_root = USER_DATA_DIR / "mm"
    candidates = sorted(mm_root.glob("Micro-Manager_*"))
    if not candidates:
        sys.exit(
            f"No MicroManager install found under {mm_root}.\n"
            "Run `mmcore install` first, or set the MM_DIR environment "
            "variable to point at your install (see docs/BUILD_AND_USAGE.md)."
        )
    return str(candidates[-1])


mm_dir = find_active_mm_dir()

core = CMMCorePlus()
core.setDeviceAdapterSearchPaths([mm_dir])
core.loadDevice("ProphEBSCam", "ProphEBS", "ProphEBS-Camera")

# Goal 5: EBS-biasRangeCheckBypass is a pre-init property -- MM's own
# convention (isPropertyPreInit()) for "settable during device setup only":
# the Hardware Config Wizard uses this flag to grey the property out once a
# device has been added/initialized, though MMCore itself doesn't block a
# post-init setProperty() call at the API level (that enforcement is a GUI
# concern, not a Core one -- confirmed by reading
# MMDevice/Property.cpp::PropertyCollection::Set(), which only checks the
# per-property readOnly_ flag, never IsPropertyPreInit()). So the
# behavior to check here is the metadata flag itself, not read-only-ness.
assert core.isPropertyPreInit("ProphEBSCam", "EBS-biasRangeCheckBypass"), \
    "EBS-biasRangeCheckBypass should be flagged as a pre-init property"
core.setProperty("ProphEBSCam", "EBS-biasRangeCheckBypass", "Off")

# Goal 8 follow-up: EBS-SyncMode is also pre-init-only -- see ProphEBS.h.
# Metavision::I_CameraSynchronization documents that the mode must be set
# before the camera starts streaming, and this adapter's one cam_.start()
# call happens once, permanently, inside Initialize() -- so there is no
# post-init moment at which changing it could ever take effect. It used to
# be a live-settable property that silently reverted after Initialize()
# (confusing, reported during a user GUI walkthrough); now it's pre-init,
# same convention as EBS-biasRangeCheckBypass.
assert core.isPropertyPreInit("ProphEBSCam", "EBS-SyncMode"), \
    "EBS-SyncMode should be flagged as a pre-init property"
core.setProperty("ProphEBSCam", "EBS-SyncMode", "Standalone")

core.initializeDevice("ProphEBSCam")
print("Goal 5: EBS-biasRangeCheckBypass is flagged pre-init, as expected")
print("Goal 8: EBS-SyncMode is flagged pre-init, as expected")

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
# EBS-RawTempRecordingFolder should exist regardless of hardware; only a
# connected camera can actually produce a .raw file (cam_.start_recording()
# needs a real streaming camera).
for prop in ("EBS-RawFilePath", "EBS-RawRecordingStatus", "EBS-RawTempRecordingFolder", "EBS-RawRecordingFormat"):
    print(prop, "=", core.getProperty("ProphEBSCam", prop))

# EBS-RawRecordingFormat: RAW vs HDF5 toggle for auto-generated/MDA-discovered
# paths. Metavision's Camera::start_recording() picks the format purely from
# the path's extension, so this just controls which extension
# GenerateAutoRawFilePath()/ComputeNumberedMdaDestination() use. Defaults to
# HDF5 per the user's request.
assert core.getProperty("ProphEBSCam", "EBS-RawRecordingFormat") == "HDF5", \
    "expected EBS-RawRecordingFormat to default to HDF5"
assert set(core.getAllowedPropertyValues("ProphEBSCam", "EBS-RawRecordingFormat")) == {"RAW", "HDF5"}
print("EBS-RawRecordingFormat defaults to HDF5, allowed values {RAW, HDF5} confirmed")

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
    assert auto_path.endswith(".hdf5"), \
        f"expected auto-generated recording to use .hdf5 (default EBS-RawRecordingFormat), got {auto_path}"
    while core.getRemainingImageCount() > 0:
        core.popNextImage()

    # EBS-RawRecordingFormat=RAW should switch the auto-generated/MDA-discovered
    # extension back to .raw.
    core.setProperty("ProphEBSCam", "EBS-RawRecordingFormat", "RAW")
    core.startSequenceAcquisition(5, 100.0, True)
    while core.isSequenceRunning():
        time.sleep(0.05)
    time.sleep(1.0)
    status_after_raw_format = core.getProperty("ProphEBSCam", "EBS-RawRecordingStatus")
    print("EBS-RawRecordingFormat=RAW: EBS-RawRecordingStatus =", status_after_raw_format)
    assert status_after_raw_format.startswith("Finished:"), \
        f"expected recording to finish cleanly, got: {status_after_raw_format}"
    raw_format_path = status_after_raw_format[len("Finished: "):]
    assert raw_format_path.endswith(".raw"), \
        f"expected EBS-RawRecordingFormat=RAW to produce a .raw file, got {raw_format_path}"
    assert os.path.isfile(raw_format_path) and os.path.getsize(raw_format_path) > 0
    print("EBS-RawRecordingFormat=RAW: .raw file created at", raw_format_path)
    core.setProperty("ProphEBSCam", "EBS-RawRecordingFormat", "HDF5")  # restore default
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

    # 4b2. EBS-RawTempRecordingFolder overrides GenerateAutoRawFilePath()'s
    # staging folder. Forced onto the local-staging path (rather than MDA
    # direct-streaming) by relying on the live profile's "save": false
    # state on this machine -- if that's not the case when this runs, the
    # assertion below will simply not be reached down this exact branch,
    # so this check is best-effort depending on current MM state.
    custom_temp_folder = os.path.join(os.path.dirname(auto_path), "..", "ProphEBS_CustomTemp")
    custom_temp_folder = os.path.normpath(custom_temp_folder)
    core.setProperty("ProphEBSCam", "EBS-RawTempRecordingFolder", custom_temp_folder)
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
    core.setProperty("ProphEBSCam", "EBS-RawTempRecordingFolder", "")  # restore default

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

# Bug fix: MDA interval. User reported that a finite (MDA) sequence with
# Exposure == Interval didn't produce back-to-back, complete Exposure-length
# windows spanning the requested total duration -- e.g. 100 frames at 100 ms
# Exposure/100 ms Interval should span ~10s, each frame a genuine, complete
# 100 ms integration, the same way a normal camera's "0 ms interval" gives
# back-to-back full exposures. Root cause: ProphEBSSequenceThread's finite
# path used to just sample frontImg_ (the Live-view buffer, continuously
# rebuilt on its own EBS-ViewDisplayRefreshMs cadence) at an arbitrary
# wall-clock phase every Interval, with no relationship to when a real
# integration window (driven by event sensor-timestamps) actually closed --
# so the "frame" grabbed could be anywhere from just-reset to about-to-close.
# Fixed by having the finite path wait for and render the actual
# completed-window snapshot (CProphEBSCamera::completedOnCounts_/
# completedWindowGeneration_, captured in CloseCurrentWindowLocked()) instead
# of the live accumulator. Checked here via each frame's own
# Elapsed-Time-ms metadata tag (real wall-clock timestamps assigned at
# InsertImage() time, independent of the fix itself) rather than pixel
# content, since real ambient event data isn't deterministic. Real
# hardware only -- the no-camera fallback has no window concept to
# synchronize to and is untouched by this fix (still just sleeps Interval).
if connection_status == "Connected":
    _exposure_before_mda_check = core.getProperty("ProphEBSCam", "Exposure")

    def _run_mda_and_get_deltas(n, interval_ms, exposure_ms):
        core.setExposure("ProphEBSCam", exposure_ms)
        core.startSequenceAcquisition(n, interval_ms, True)
        while core.isSequenceRunning():
            time.sleep(0.01)
        elapsed_vals = []
        while core.getRemainingImageCount() > 0:
            _, md = core.popNextImageAndMD()
            elapsed_vals.append(float(md["ElapsedTime-ms"]))
        return [b - a for a, b in zip(elapsed_vals, elapsed_vals[1:])]

    # Exposure == Interval: each frame should land roughly one Exposure
    # apart (allowing generous slack for real thread-wakeup/event-timing
    # jitter -- this checks the fix's order-of-magnitude effect, not exact
    # real-time scheduling precision).
    deltas_equal = _run_mda_and_get_deltas(10, 100.0, 100.0)
    mean_equal = sum(deltas_equal) / len(deltas_equal)
    print("Bug fix (MDA interval): Exposure=Interval=100ms, inter-frame deltas (ms):",
          [f"{d:.1f}" for d in deltas_equal], ", mean:", f"{mean_equal:.1f}")
    assert 60.0 <= mean_equal <= 160.0, \
        f"expected ~100 ms between MDA frames with Exposure=Interval=100ms, got mean delta {mean_equal:.1f} ms"

    # Interval > Exposure: real dead time between frames, spacing should
    # follow Interval (not Exposure).
    deltas_gap = _run_mda_and_get_deltas(10, 200.0, 50.0)
    mean_gap = sum(deltas_gap) / len(deltas_gap)
    print("Bug fix (MDA interval): Exposure=50ms, Interval=200ms, inter-frame deltas (ms):",
          [f"{d:.1f}" for d in deltas_gap], ", mean:", f"{mean_gap:.1f}")
    assert 140.0 <= mean_gap <= 260.0, \
        f"expected ~200 ms between MDA frames with Interval=200ms > Exposure=50ms, got mean delta {mean_gap:.1f} ms"

    core.setProperty("ProphEBSCam", "Exposure", _exposure_before_mda_check)
    print("Bug fix (MDA interval): confirmed MDA frame spacing follows max(Exposure, Interval), "
          "not an arbitrary Live-view sample phase")
else:
    print("Bug fix (MDA interval): no EBS connected -- skipping (needs real completed-window timing)")

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

# Goal 6: EBS-ViewOffset -- the "offset +- found events" ask. Probe a
# mid-range value from MM's own reported limits rather than a hardcoded
# guess (same pattern as the Goal 5 bias/anti-flicker checks).
offset_lo = int(core.getPropertyLowerLimit("ProphEBSCam", "EBS-ViewOffset"))
offset_hi = int(core.getPropertyUpperLimit("ProphEBSCam", "EBS-ViewOffset"))
offset_probe = offset_lo + (offset_hi - offset_lo) // 2
original_offset = core.getProperty("ProphEBSCam", "EBS-ViewOffset")
assert int(original_offset) == 100, "expected EBS-ViewOffset default of 100, got " + original_offset
core.setProperty("ProphEBSCam", "EBS-ViewOffset", str(offset_probe))
assert int(core.getProperty("ProphEBSCam", "EBS-ViewOffset")) == offset_probe
core.setProperty("ProphEBSCam", "EBS-ViewOffset", original_offset)
print("Goal 6: EBS-ViewOffset round-tripped (offset probe", offset_probe, ", default 100 confirmed)")

# Bug fix: TransposeCorrection/-MirrorX/-MirrorY/TransposeXY are created
# automatically by MM::CCameraBase, but previously had no actual effect on
# the image -- MMCore itself does not apply them, so each adapter is
# responsible for its own pixel transform (this adapter wasn't).
# Exact pixel-level comparison only makes sense with no real camera
# connected (the deterministic checkerboard+gradient fallback pattern --
# GenerateTestImage()/ApplyRoiToBuffers() apply the same transform as
# BuildAndSwapFrame(), see ApplyTranspose() in ProphEBS.cpp); with a real
# camera streaming, live event data keeps changing between snaps, so only
# the shape-swap from TransposeXY (deterministic regardless of pixel
# content) is checked there.
original_correction = core.getProperty("ProphEBSCam", "TransposeCorrection")
original_mirror_x = core.getProperty("ProphEBSCam", "TransposeMirrorX")
original_mirror_y = core.getProperty("ProphEBSCam", "TransposeMirrorY")
original_swap_xy = core.getProperty("ProphEBSCam", "TransposeXY")

core.setProperty("ProphEBSCam", "TransposeCorrection", "0")
core.setProperty("ProphEBSCam", "TransposeMirrorX", "0")
core.setProperty("ProphEBSCam", "TransposeMirrorY", "0")
core.setProperty("ProphEBSCam", "TransposeXY", "0")
core.snapImage()
baseline_img = core.getImage().copy()

if connection_status != "Connected":
    # No real camera means no periodic frame-builder thread -- the fallback
    # pattern only re-renders on Initialize() or a ROI/binning change (see
    # ApplyTranspose()'s comment in ProphEBS.cpp), so a harmless clearROI()
    # round-trip (already exercised elsewhere in this script) is used here
    # purely to force ApplyRoiToBuffers() to pick up each new Transpose*
    # value -- with a real camera, BuildAndSwapFrame() picks it up on its own
    # every frame and this round-trip isn't needed (see the else branch).

    # With TransposeCorrection off, MirrorX/Y/SwapXY must have no effect at all.
    core.setProperty("ProphEBSCam", "TransposeMirrorX", "1")
    core.setProperty("ProphEBSCam", "TransposeMirrorY", "1")
    core.setProperty("ProphEBSCam", "TransposeXY", "1")
    core.clearROI()
    core.snapImage()
    assert np.array_equal(core.getImage(), baseline_img), \
        "TransposeMirrorX/Y/SwapXY changed the image while TransposeCorrection is Off -- should be a no-op"
    print("Goal 9 bug fix: TransposeCorrection=Off correctly disables Mirror/SwapXY")

    core.setProperty("ProphEBSCam", "TransposeCorrection", "1")
    core.setProperty("ProphEBSCam", "TransposeMirrorX", "1")
    core.setProperty("ProphEBSCam", "TransposeMirrorY", "0")
    core.setProperty("ProphEBSCam", "TransposeXY", "0")
    core.clearROI()
    core.snapImage()
    assert np.array_equal(core.getImage(), np.fliplr(baseline_img)), \
        "TransposeMirrorX (Correction On) did not horizontally flip the image"
    print("Goal 9 bug fix: TransposeCorrection+MirrorX horizontally flips the image")

    core.setProperty("ProphEBSCam", "TransposeMirrorX", "0")
    core.setProperty("ProphEBSCam", "TransposeMirrorY", "1")
    core.clearROI()
    core.snapImage()
    assert np.array_equal(core.getImage(), np.flipud(baseline_img)), \
        "TransposeMirrorY (Correction On) did not vertically flip the image"
    print("Goal 9 bug fix: TransposeCorrection+MirrorY vertically flips the image")

    core.setProperty("ProphEBSCam", "TransposeMirrorY", "0")
    core.setProperty("ProphEBSCam", "TransposeXY", "1")
    core.clearROI()
    core.snapImage()
    swapped_img = core.getImage()
    assert swapped_img.shape == (baseline_img.shape[1], baseline_img.shape[0]), \
        f"TransposeXY should swap image dimensions {baseline_img.shape} -> " \
        f"{(baseline_img.shape[1], baseline_img.shape[0])}, got {swapped_img.shape}"
    assert np.array_equal(swapped_img, baseline_img.T), \
        "TransposeXY (Correction On) did not transpose the image"
    print("Goal 9 bug fix: TransposeCorrection+TransposeXY transposes the image (shape",
          baseline_img.shape, "->", swapped_img.shape, ")")
else:
    # Real camera: BuildAndSwapFrame() reads Transpose* fresh every frame on
    # its own frame-builder thread, no clearROI() nudge needed -- just give
    # it one refresh cycle (EBS-ViewDisplayRefreshMs, 1ms default) to react.
    core.setProperty("ProphEBSCam", "TransposeCorrection", "1")
    core.setProperty("ProphEBSCam", "TransposeXY", "1")
    time.sleep(0.1)
    core.snapImage()
    swapped_img = core.getImage()
    assert swapped_img.shape == (baseline_img.shape[1], baseline_img.shape[0]), \
        f"TransposeXY should swap image dimensions {baseline_img.shape} -> " \
        f"{(baseline_img.shape[1], baseline_img.shape[0])}, got {swapped_img.shape}"
    print("Goal 9 bug fix: TransposeCorrection+TransposeXY transposes image shape",
          baseline_img.shape, "->", swapped_img.shape,
          "(pixel-level Mirror/SwapXY checks skipped -- real camera streaming, frame content isn't static)")

core.setProperty("ProphEBSCam", "TransposeCorrection", original_correction)
core.setProperty("ProphEBSCam", "TransposeMirrorX", original_mirror_x)
core.setProperty("ProphEBSCam", "TransposeMirrorY", original_mirror_y)
core.setProperty("ProphEBSCam", "TransposeXY", original_swap_xy)
if connection_status != "Connected":
    core.clearROI()  # picks up the restored (non-swapped) Transpose* values
else:
    # Real camera: give the frame-builder thread at least one refresh cycle
    # to actually rebuild backImg_ at the restored (non-swapped) dimensions
    # before anything downstream snaps -- otherwise a snap taken immediately
    # after restoring TransposeXY can still catch the still-swapped frame
    # that was mid-flight when the property was set back.
    time.sleep(0.1)
core.snapImage()  # restore image dimensions before any following ROI-shape assertions

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
# EBS-ViewDisplayRefreshMs property decouples "how long is one integration
# window" from "how often is a frame actually published."
assert abs(core.getPropertyLowerLimit("ProphEBSCam", "Exposure") - 0.001) < 1e-9, \
    "expected Exposure's lower limit to be 0.001 ms after the sub-ms follow-up"
core.setExposure("ProphEBSCam", 0.05)
assert abs(float(core.getProperty("ProphEBSCam", "Exposure")) - 0.05) < 1e-9
print("Goal 6 follow-up: Exposure round-tripped to 0.05 ms (sub-millisecond)")
core.setProperty("ProphEBSCam", "Exposure", original_exposure)

refresh_lo = core.getPropertyLowerLimit("ProphEBSCam", "EBS-ViewDisplayRefreshMs")
refresh_hi = core.getPropertyUpperLimit("ProphEBSCam", "EBS-ViewDisplayRefreshMs")
original_refresh = core.getProperty("ProphEBSCam", "EBS-ViewDisplayRefreshMs")
core.setProperty("ProphEBSCam", "EBS-ViewDisplayRefreshMs", "5")
assert float(core.getProperty("ProphEBSCam", "EBS-ViewDisplayRefreshMs")) == 5.0
print("Goal 6 follow-up: EBS-ViewDisplayRefreshMs round-tripped to 5 ms (limits", refresh_lo, "-", refresh_hi, ")")
core.setProperty("ProphEBSCam", "EBS-ViewDisplayRefreshMs", original_refresh)

# Goal 6 follow-up: with a real EBS connected, run genuinely sub-millisecond
# integration windows (down to the 0.001 ms floor) with a fast 1 ms display
# refresh for a short real-world burst -- this is the actual stress case the
# design has to survive: OnEventsCD() closing windows via real event
# timestamps far more often than the old wall-clock design ever did, while
# only resetting touched pixels (not the whole sensor-sized array) each
# time. Snap before/after to confirm the frame pipeline is still alive and
# producing correctly-shaped frames, not hung or crashed.
if connection_status == "Connected":
    core.setProperty("ProphEBSCam", "EBS-ViewDisplayRefreshMs", "1")
    for sub_ms_exposure in (0.1, 0.01, 0.001):
        core.setExposure("ProphEBSCam", sub_ms_exposure)
        time.sleep(0.3)
        core.snapImage()
        img = core.getImage()
        assert img.shape == img_before.shape and img.dtype == img_before.dtype
        print("Goal 6 follow-up: Exposure =", sub_ms_exposure, "ms survived a 300ms burst, image",
              img.shape, img.dtype)
    core.setProperty("ProphEBSCam", "EBS-ViewDisplayRefreshMs", original_refresh)
    core.setProperty("ProphEBSCam", "Exposure", original_exposure)
else:
    print("Goal 6 follow-up: no EBS connected -- skipping sub-millisecond stress check")

# Bug fix: Live view (unbounded StartSequenceAcquisition) must push frames at
# max(Exposure, EBS-ViewLiveMinIntervalMs), never at whatever interval
# happens to be passed in -- MMCore's own "unused" parameter contract. New
# EBS-ViewLiveMinIntervalMs property (default 5 ms) round-trip first.
minint_lo = core.getPropertyLowerLimit("ProphEBSCam", "EBS-ViewLiveMinIntervalMs")
minint_hi = core.getPropertyUpperLimit("ProphEBSCam", "EBS-ViewLiveMinIntervalMs")
original_min_interval = core.getProperty("ProphEBSCam", "EBS-ViewLiveMinIntervalMs")
assert abs(float(original_min_interval) - 5.0) < 1e-9, "expected EBS-ViewLiveMinIntervalMs to default to 5.0 ms"
core.setProperty("ProphEBSCam", "EBS-ViewLiveMinIntervalMs", "20")
assert float(core.getProperty("ProphEBSCam", "EBS-ViewLiveMinIntervalMs")) == 20.0
print("Goal 6 follow-up (Live-cadence bug fix): EBS-ViewLiveMinIntervalMs round-tripped to 20 ms (limits",
      minint_lo, "-", minint_hi, ", default 5)")
core.setProperty("ProphEBSCam", "EBS-ViewLiveMinIntervalMs", original_min_interval)

# Simulates the exact failure mode found by the user: setting a
# sub-millisecond Exposure, then starting continuous acquisition with that
# same tiny value as the (supposed-to-be-ignored) interval argument, the way
# a Live-view implementation that paces itself off the camera's exposure
# would. Before the fix this made ProphEBSSequenceThread push ~1000
# frames/sec, overwhelming MMCore's circular buffer and producing an
# ever-growing display backlog/latency. After the fix, Live view is bounded
# by EBS-ViewLiveMinIntervalMs (default 5 ms -> ~200 frames/sec) even though
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
        f"expected roughly ~200 frames/sec (bounded by the 5 ms EBS-ViewLiveMinIntervalMs floor), " \
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

# Follow-up: backlog detection/flush. EBS-AvgBacklogFlushThresholdMs round-trip
# first, then (with a real camera) force the threshold artificially low so
# that ordinary real-time callback jitter reliably trips it within a short
# streaming burst -- confirming both EBS-AvgCallbackLagMs and
# EBS-AvgBacklogFlushCount are wired up and moving, not just present.
thresh_lo = core.getPropertyLowerLimit("ProphEBSCam", "EBS-AvgBacklogFlushThresholdMs")
thresh_hi = core.getPropertyUpperLimit("ProphEBSCam", "EBS-AvgBacklogFlushThresholdMs")
original_threshold = core.getProperty("ProphEBSCam", "EBS-AvgBacklogFlushThresholdMs")
assert abs(float(original_threshold) - 250.0) < 1e-9, "expected EBS-AvgBacklogFlushThresholdMs to default to 250 ms"
core.setProperty("ProphEBSCam", "EBS-AvgBacklogFlushThresholdMs", "500")
assert float(core.getProperty("ProphEBSCam", "EBS-AvgBacklogFlushThresholdMs")) == 500.0
print("Follow-up: EBS-AvgBacklogFlushThresholdMs round-tripped to 500 ms (limits", thresh_lo, "-", thresh_hi,
      ", default 250)")
core.setProperty("ProphEBSCam", "EBS-AvgBacklogFlushThresholdMs", original_threshold)

if connection_status == "Connected":
    lag_before = float(core.getProperty("ProphEBSCam", "EBS-AvgCallbackLagMs"))
    flushes_before = float(core.getProperty("ProphEBSCam", "EBS-AvgBacklogFlushCount"))
    core.setProperty("ProphEBSCam", "EBS-AvgBacklogFlushThresholdMs", str(thresh_lo))  # near-guaranteed trip
    time.sleep(1.0)
    lag_after = float(core.getProperty("ProphEBSCam", "EBS-AvgCallbackLagMs"))
    flushes_after = float(core.getProperty("ProphEBSCam", "EBS-AvgBacklogFlushCount"))
    core.setProperty("ProphEBSCam", "EBS-AvgBacklogFlushThresholdMs", original_threshold)
    print("Follow-up: EBS-AvgCallbackLagMs", lag_before, "->", lag_after,
          ", EBS-AvgBacklogFlushCount", flushes_before, "->", flushes_after,
          "(threshold forced to 0.001 ms for 1s)")
    assert flushes_after > flushes_before, \
        "expected EBS-AvgBacklogFlushCount to increase once EBS-AvgBacklogFlushThresholdMs was forced near zero"
    # Snap after restoring the threshold, confirming the adapter is still in
    # a normal working state post-flush (no regression from the fast path).
    img_after_flush = core.snapImage()
    core.getImage()
    print("Follow-up: snap after backlog flush still succeeded, shape unaffected")
else:
    print("Follow-up: no EBS connected -- skipping backlog-flush trigger check")

# Goal 7: EBS-HotPixelBlockedPixels -- round-trip a valid list, then confirm a
# malformed string is rejected wholesale (the previous valid value is
# retained, not partially applied).
original_blocked = core.getProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels")
core.setProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels", "5:5;10:20")
assert core.getProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels") == "5:5;10:20"
print("Goal 7: EBS-HotPixelBlockedPixels round-tripped '5:5;10:20'")
try:
    core.setProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels", "abc")
    malformed_rejected = False
except Exception:
    malformed_rejected = True
assert malformed_rejected, "expected a malformed EBS-HotPixelBlockedPixels value to be rejected"
assert core.getProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels") == "5:5;10:20", \
    "expected the previous valid EBS-HotPixelBlockedPixels value to be retained after a rejected malformed set"
print("Goal 7: malformed EBS-HotPixelBlockedPixels ('abc') correctly rejected, previous value retained")
core.setProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels", original_blocked)

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

assert set(core.getAllowedPropertyValues("ProphEBSCam", "EBS-HotPixelDetectNow")) == {"Idle", "Run"}

if connection_status != "Connected":
    t0 = time.time()
    core.setProperty("ProphEBSCam", "EBS-HotPixelDetectNow", "Run")
    elapsed = time.time() - t0
    assert elapsed < 5.0, f"calibration with no camera should fail promptly, took {elapsed:.1f}s"
    status = core.getProperty("ProphEBSCam", "EBS-HotPixelCalibStatus")
    assert status.startswith("Failed:"), f"expected a clean failure with no camera, got: {status}"
    assert core.getProperty("ProphEBSCam", "EBS-HotPixelDetectNow") == "Idle"
    print("Goal 7: EBS-HotPixelDetectNow with no camera failed promptly and cleanly:", status)
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
    core.setProperty("ProphEBSCam", "EBS-HotPixelDetectNow", "Run")
    elapsed = time.time() - t0
    calib_status = core.getProperty("ProphEBSCam", "EBS-HotPixelCalibStatus")
    print("Goal 7: real-hardware calibration (default 5000ms/k=10) took", f"{elapsed:.2f}s", ", status:",
          calib_status)
    assert calib_status.startswith("Done:") or calib_status.startswith("Failed:"), \
        f"expected calibration to finish in a well-defined state, got: {calib_status}"
    assert core.getProperty("ProphEBSCam", "EBS-HotPixelDetectNow") == "Idle"
    blocked_after_calib = core.getProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels")
    print("Goal 7: EBS-HotPixelBlockedPixels after calibration:", blocked_after_calib)
    # Sanity-check the serialized format is well-formed (parseable pairs).
    if blocked_after_calib:
        for pair in blocked_after_calib.split(";"):
            px, py = pair.split(":")
            int(px)
            int(py)
    core.setProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels", original_blocked)

    # Confirm calibration REPLACES EBS-HotPixelBlockedPixels rather than merging
    # into it (a deliberate change: calibration now unblocks everything
    # first, then re-detects from a clean population -- see
    # ProphEBS.cpp's OnDetectHotPixelsNow()). Manually block an arbitrary
    # pixel unrelated to any real hot pixel, run calibration with a short
    # duration/high threshold (so it's unlikely to itself flag much), and
    # confirm the manually-added pixel is gone afterward -- if calibration
    # still merged, it would still be present.
    sentinel_pixel = "500:500"
    core.setProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels", sentinel_pixel)
    assert core.getProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels") == sentinel_pixel
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "200")
    core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", original_hotpixel_k)
    core.setProperty("ProphEBSCam", "EBS-HotPixelDetectNow", "Run")
    blocked_after_replace = core.getProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels")
    print("Goal 7: EBS-HotPixelBlockedPixels after calibration following a manual set of", sentinel_pixel, ":",
          blocked_after_replace)
    assert sentinel_pixel not in (blocked_after_replace or "").split(";"), \
        f"expected calibration to REPLACE EBS-HotPixelBlockedPixels (clearing the manually-set {sentinel_pixel} " \
        f"first), but it's still present after a run -- calibration is still merging, not replacing"
    print("Goal 7: confirmed -- calibration replaced EBS-HotPixelBlockedPixels rather than merging into it")
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "5000")
    core.setProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels", original_blocked)

    # Confirm calibration is actually scoped to the active ROI, not the
    # whole chip: set a small ROI, run calibration, and check every
    # resulting EBS-HotPixelBlockedPixels coordinate falls inside that ROI's
    # bounds (with the loosest possible k, so *something* is very likely
    # to be flagged even in a short window -- this checks scoping, not
    # detection sensitivity).
    core.setROI("ProphEBSCam", roi_x, roi_y, roi_w, roi_h)
    core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", str(k_lo))
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "200")
    core.setProperty("ProphEBSCam", "EBS-HotPixelDetectNow", "Run")
    roi_scoped_blocked = core.getProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels")
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
    core.setProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels", original_blocked)

    # Deliberately loose-threshold stress test: force EBS-HotPixelStddevK to
    # its floor with a short duration so a large fraction of pixels statistically
    # qualify as "outliers," exercising the truncation-safety-cap path
    # (EBS-HotPixelBlockedPixels must never be allowed to grow past what MMCore's
    # MM::MaxStrLength property-value limit can hold -- see ProphEBS.cpp's
    # OnDetectHotPixelsNow()/the "found via self-testing" comment there).
    core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", str(k_lo))
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "200")
    core.setProperty("ProphEBSCam", "EBS-HotPixelDetectNow", "Run")
    stress_status = core.getProperty("ProphEBSCam", "EBS-HotPixelCalibStatus")
    stress_blocked = core.getProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels")
    print("Goal 7: loose-threshold stress calibration status:", stress_status)
    assert len(stress_blocked) <= 1024, \
        f"EBS-HotPixelBlockedPixels must never exceed MMCore's 1024-char property limit, got {len(stress_blocked)} chars"
    if stress_status.startswith("Done:") and "dropped" in stress_status:
        print("Goal 7: truncation-safety cap engaged as expected under a deliberately loose threshold:",
              stress_status)
    core.setProperty("ProphEBSCam", "EBS-HotPixelStddevK", original_hotpixel_k)
    core.setProperty("ProphEBSCam", "EBS-HotPixelCalibDurationMs", "5000")
    core.setProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels", original_blocked)

    # Manually setting EBS-HotPixelBlockedPixels against real hardware shouldn't
    # break subsequent snaps.
    core.setProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels", "3:3;7:9")
    core.snapImage()
    core.getImage()
    print("Goal 7: manual EBS-HotPixelBlockedPixels set against real hardware, snap still succeeded")
    core.setProperty("ProphEBSCam", "EBS-HotPixelBlockedPixels", original_blocked)
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

# Goal 8: real spatial binning. Exercised unconditionally (works against the
# no-hardware fallback checkerboard too, since ApplyRoiToBuffers() is what
# actually resizes the buffers) -- ROI is full-frame at this point in the
# script either way (both branches above end with clearROI()).
full_x, full_y, full_w, full_h = core.getROI("ProphEBSCam")
assert core.getProperty("ProphEBSCam", "Binning") == "1"
for bin_factor in (2, 4, 1):
    core.setProperty("ProphEBSCam", "Binning", str(bin_factor))
    assert core.getProperty("ProphEBSCam", "Binning") == str(bin_factor)
    core.snapImage()
    binned_img = core.getImage()
    expected_shape = (full_h // bin_factor, full_w // bin_factor)
    assert binned_img.shape == expected_shape, \
        f"Binning={bin_factor}: expected image shape {expected_shape}, got {binned_img.shape}"
print("Goal 8: Binning round-tripped 1 -> 2 -> 4 -> 1, image shape scaled as expected each time "
      f"(full frame {full_h}x{full_w})")

# Goal 8: hardware trigger in/out properties. Present and round-trippable
# whether or not a camera is connected (state-only fallback otherwise).
trigger_channels = ["Main"]
try:
    core.setProperty("ProphEBSCam", "EBS-TriggerIn-Channel", "Main")
except Exception:
    pass
for prop, value in (
    ("EBS-TriggerIn-Enabled", "On"),
    ("EBS-TriggerOut-Enabled", "On"),
    ("EBS-TriggerOut-PeriodUs", "2000"),
    ("EBS-TriggerOut-DutyCycle", "0.25"),
):
    core.setProperty("ProphEBSCam", prop, value)
    readback = core.getProperty("ProphEBSCam", prop)
    print(f"Goal 8: {prop} set to {value}, read back {readback}")
# Leave everything disabled again -- no reason to leave a trigger output
# actively pulsing after the self-test finishes.
core.setProperty("ProphEBSCam", "EBS-TriggerIn-Enabled", "Off")
core.setProperty("ProphEBSCam", "EBS-TriggerOut-Enabled", "Off")
print("Goal 8: hardware trigger in/out properties round-tripped")

# Goal 8 follow-up: EBS-TriggerIn-Count -- the diagnostic that lets
# EBS-TriggerIn-Enabled actually be confirmed working without external
# trigger hardware wired up. With nothing wired to the trigger-in pin on
# this dev machine, no real EventExtTrigger events can arrive -- so this
# only checks the property exists and reads back a non-negative number
# (staying at 0 the whole time is the expected/correct result here, not a
# failure), not that it counts something real. A user with a signal
# generator on the trigger-in pin is the only way to verify the count
# actually increments.
trigger_in_count = float(core.getProperty("ProphEBSCam", "EBS-TriggerIn-Count"))
assert trigger_in_count >= 0.0
print("Goal 8: EBS-TriggerIn-Count readable, current value:", trigger_in_count,
      "(expected to be 0 on this dev machine -- nothing physically wired to trigger-in)")

# Goal 8: sensor-level event-rate band-pass filter. Round-trip all four
# thresholds plus Enabled; note the *value* actually accepted may be clamped
# by the hardware's own supported range when connected, so this only checks
# the round-trip doesn't error and Enabled reflects what was set.
for prop, value in (
    ("EBS-EventRateFilter-LowerStart", "1000"),
    ("EBS-EventRateFilter-LowerStop", "5000"),
):
    core.setProperty("ProphEBSCam", prop, value)
    print(f"Goal 8: {prop} set to {value}, read back {core.getProperty('ProphEBSCam', prop)}")
core.setProperty("ProphEBSCam", "EBS-EventRateFilter-Enabled", "On")
assert core.getProperty("ProphEBSCam", "EBS-EventRateFilter-Enabled") == "On"
core.setProperty("ProphEBSCam", "EBS-EventRateFilter-Enabled", "Off")
assert core.getProperty("ProphEBSCam", "EBS-EventRateFilter-Enabled") == "Off"
print("Goal 8: sensor-level event-rate band-pass filter properties round-tripped")

# Goal 8 follow-up: EBS-SyncMode is pre-init (checked/set near the top of
# this script, before initializeDevice()) -- its own I_CameraSynchronization
# contract means it can never take effect after Initialize()'s one and only
# cam_.start() call, so it's no longer live-settable here at all. Just
# confirm it still reads back the value it was set to before init.
sync_value = core.getProperty("ProphEBSCam", "EBS-SyncMode")
assert sync_value == "Standalone", f"expected EBS-SyncMode to still read back its pre-init value, got {sync_value}"
print("Goal 8: EBS-SyncMode (pre-init) still reads back its configured value:", sync_value)

# Goal 8: time-decay view mode. Switches EBS-ViewMode to TimeDecay, adjusts
# the new time-constant property, and confirms Snap still produces a
# correctly-shaped/typed frame (pixel-level decay behavior itself needs a
# real, changing scene to observe meaningfully -- see the user's GUI
# walkthrough for that).
original_view_mode3 = core.getProperty("ProphEBSCam", "EBS-ViewMode")
original_decay_us = core.getProperty("ProphEBSCam", "EBS-ViewModeTimeDecay_DecayTime_Constant-us")
core.setProperty("ProphEBSCam", "EBS-ViewMode", "TimeDecay")
assert core.getProperty("ProphEBSCam", "EBS-ViewMode") == "TimeDecay"
core.setProperty("ProphEBSCam", "EBS-ViewModeTimeDecay_DecayTime_Constant-us", "5000")
assert float(core.getProperty("ProphEBSCam", "EBS-ViewModeTimeDecay_DecayTime_Constant-us")) == 5000.0
if connection_status == "Connected":
    time.sleep(0.3)  # let a few real events accumulate under the new mode
core.snapImage()
decay_img = core.getImage()
assert decay_img.shape == (full_h, full_w) and decay_img.dtype == np.uint8, \
    f"TimeDecay mode: expected shape {(full_h, full_w)} uint8, got {decay_img.shape} {decay_img.dtype}"
core.setProperty("ProphEBSCam", "EBS-ViewMode", original_view_mode3)
core.setProperty("ProphEBSCam", "EBS-ViewModeTimeDecay_DecayTime_Constant-us", original_decay_us)
print("Goal 8: TimeDecay view mode round-tripped, snap shape/dtype:", decay_img.shape, decay_img.dtype)

# ---------------------------------------------------------------------------
# Goal 9: full-suite polishing -- negative/error-path checks. Goals 1-8's
# self-test was almost entirely happy-path round-trips (the one exception
# being the malformed EBS-HotPixelBlockedPixels check above); the checks
# below specifically probe MMCore's SetPropertyLimits()/AddAllowedValue()
# enforcement (rejecting bad input), since that enforcement is the actual
# "error handling" this adapter relies on -- a broken/missing
# SetPropertyLimits()/AddAllowedValue() call would otherwise slip by
# unnoticed by a purely happy-path suite.
# ---------------------------------------------------------------------------


def assert_rejected(prop, bad_value):
    """True if MMCore refused bad_value for prop -- either by raising, or by
    silently leaving the property at its prior value (some limit paths clamp
    rather than throw). Restores the property to its prior value either way."""
    before = core.getProperty("ProphEBSCam", prop)
    try:
        core.setProperty("ProphEBSCam", prop, str(bad_value))
        rejected = False
    except Exception:
        rejected = True
    after = core.getProperty("ProphEBSCam", prop)
    if not rejected:
        rejected = (after == before)
    core.setProperty("ProphEBSCam", prop, before)
    return rejected


# Out-of-range checks against properties whose limits are set unconditionally
# in Initialize() (i.e. regardless of whether a camera is connected), so
# these always run.
duration_hi_oob = core.getPropertyUpperLimit("ProphEBSCam", "EBS-HotPixelCalibDurationMs") + 100000.0
assert assert_rejected("EBS-HotPixelCalibDurationMs", duration_hi_oob), \
    "expected an out-of-range EBS-HotPixelCalibDurationMs to be rejected"
print("Goal 9: out-of-range EBS-HotPixelCalibDurationMs correctly rejected")

k_hi_oob = core.getPropertyUpperLimit("ProphEBSCam", "EBS-HotPixelStddevK") + 1000.0
assert assert_rejected("EBS-HotPixelStddevK", k_hi_oob), \
    "expected an out-of-range EBS-HotPixelStddevK to be rejected"
print("Goal 9: out-of-range EBS-HotPixelStddevK correctly rejected")

assert assert_rejected("EBS-TriggerOut-DutyCycle", 5.0), \
    "expected an out-of-range EBS-TriggerOut-DutyCycle (5.0, limit is 0.0-1.0) to be rejected"
print("Goal 9: out-of-range EBS-TriggerOut-DutyCycle correctly rejected")

assert assert_rejected("EBS-ViewOffset", 99999), \
    "expected an out-of-range EBS-ViewOffset (limit is 0-255) to be rejected"
print("Goal 9: out-of-range EBS-ViewOffset correctly rejected")

assert assert_rejected("Exposure", -5.0), \
    "expected a negative Exposure (limit floor is 0.001 ms) to be rejected"
print("Goal 9: negative Exposure correctly rejected")

# Out-of-range checks against properties whose limits are only established
# once a real camera reports its supported range (Goal 5/8) -- only
# meaningful, and only run, with hardware connected.
if connection_status == "Connected":
    if core.hasPropertyLimits("ProphEBSCam", "EBS-ERC-EventRate"):
        erc_hi_oob = core.getPropertyUpperLimit("ProphEBSCam", "EBS-ERC-EventRate") + 1000000000.0
        assert assert_rejected("EBS-ERC-EventRate", erc_hi_oob), \
            "expected an out-of-range EBS-ERC-EventRate to be rejected"
        print("Goal 9: out-of-range EBS-ERC-EventRate correctly rejected")

    if bias_props and core.hasPropertyLimits("ProphEBSCam", bias_props[0]):
        bias_hi_oob = core.getPropertyUpperLimit("ProphEBSCam", bias_props[0]) + 100000
        assert assert_rejected(bias_props[0], bias_hi_oob), \
            f"expected an out-of-range {bias_props[0]} to be rejected"
        print(f"Goal 9: out-of-range {bias_props[0]} correctly rejected")

    if core.hasPropertyLimits("ProphEBSCam", "EBS-AntiFlicker-HighFreq"):
        af_hi_oob = core.getPropertyUpperLimit("ProphEBSCam", "EBS-AntiFlicker-HighFreq") + 100000
        assert assert_rejected("EBS-AntiFlicker-HighFreq", af_hi_oob), \
            "expected an out-of-range EBS-AntiFlicker-HighFreq to be rejected"
        print("Goal 9: out-of-range EBS-AntiFlicker-HighFreq correctly rejected")

    if core.hasPropertyLimits("ProphEBSCam", "EBS-EventRateFilter-UpperStop"):
        erf_hi_oob = core.getPropertyUpperLimit("ProphEBSCam", "EBS-EventRateFilter-UpperStop") + 1000000000.0
        assert assert_rejected("EBS-EventRateFilter-UpperStop", erf_hi_oob), \
            "expected an out-of-range EBS-EventRateFilter-UpperStop to be rejected"
        print("Goal 9: out-of-range EBS-EventRateFilter-UpperStop correctly rejected")
else:
    print("Goal 9: no EBS connected -- skipping out-of-range checks for hardware-range-dependent "
          "properties (bias/ERC/anti-flicker/event-rate-filter have no limits set without a "
          "connected camera, so there is nothing to enforce)")

# Invalid-enum-string rejection: each of these uses AddAllowedValue(), so a
# nonsense string should be rejected outright by MMCore, with the previous
# valid value retained -- not silently substituted or accepted.
for prop, valid_restore in (
    ("EBS-ViewMode", "NetSigned"),
    ("EBS-EventTrailFilter-Mode", "TRAIL"),
    ("EBS-TriggerIn-Channel", None),
    ("EBS-AntiFlicker-FilterType", "Band Cut"),
):
    before = core.getProperty("ProphEBSCam", prop)
    try:
        core.setProperty("ProphEBSCam", prop, "NotARealValue")
        rejected = False
    except Exception:
        rejected = True
    after = core.getProperty("ProphEBSCam", prop)
    assert rejected or after == before, f"expected an invalid enum string for {prop} to be rejected"
    if valid_restore is not None:
        core.setProperty("ProphEBSCam", prop, valid_restore)
    print(f"Goal 9: invalid enum value for {prop} correctly rejected, previous value retained")

# Pre-init-only properties (EBS-biasRangeCheckBypass, EBS-SyncMode): raw
# MMCore's own PropertyCollection::Set() (MMDevice/Property.cpp) only ever
# checks the per-property readOnly_ flag, never IsPropertyPreInit() -- so at
# the bare Core API level a post-init setProperty() call is not blocked
# there (see the comment near the top of this script). But this harness
# goes through pymmcore-plus's CMMCorePlus.setProperty(), which adds its own
# extra guard on top of that and actually raises RuntimeError for a
# pre-init property once the device is initialized -- discovered while
# writing this very check (an assumption from prose in docs/DEVLOG.md turned
# out to only describe the bare-Core layer, not this harness's actual
# wrapper). Confirms the real, observed behavior through this test harness:
# post-init attempts are rejected, and the value is left unchanged.
for prop, current_value in (
    ("EBS-biasRangeCheckBypass", "Off"),
    ("EBS-SyncMode", "Standalone"),
):
    try:
        core.setProperty("ProphEBSCam", prop, current_value)
        preinit_rejected = False
    except RuntimeError:
        preinit_rejected = True
    assert preinit_rejected, f"expected pymmcore-plus to reject a post-init setProperty() on {prop}"
    readback = core.getProperty("ProphEBSCam", prop)
    assert readback == current_value, f"expected {prop} to still read back {current_value}"
print("Goal 9: post-init setProperty() on pre-init-only properties "
      "(EBS-biasRangeCheckBypass, EBS-SyncMode) is correctly rejected by pymmcore-plus "
      "(RuntimeError), value unchanged")

# Negative/zero SetROI bounds -- confirm a degenerate window doesn't crash
# the adapter or corrupt the current ROI state. A zero-size window is
# documented (ProphEBS.cpp, SetROI()) to behave like ClearROI() rather than
# being rejected outright.
before_roi = core.getROI("ProphEBSCam")
try:
    core.setROI("ProphEBSCam", 0, 0, 0, 0)
    zero_size_ok = True
except Exception:
    zero_size_ok = False
assert zero_size_ok, "expected a zero-size SetROI to be handled gracefully (equivalent to ClearROI())"
after_zero = core.getROI("ProphEBSCam")
print("Goal 9: zero-size SetROI handled gracefully (ROI now", after_zero, ") -- restoring")
core.setROI("ProphEBSCam", *before_roi)
assert core.getROI("ProphEBSCam") == before_roi
print("Goal 9: SetROI restored to", before_roi, "after the zero-size probe, no crash/corruption")

core.unloadDevice("ProphEBSCam")
print("SUCCESS")
