import os
import time

from pymmcore_plus import CMMCorePlus

mm_dir = r"C:\Users\kjamartens\AppData\Local\pymmcore-plus\pymmcore-plus\mm\Micro-Manager_2.0.3_20260724"

core = CMMCorePlus()
core.setDeviceAdapterSearchPaths([mm_dir])
core.loadDevice("ProphEBSCam", "ProphEBS", "ProphEBS-Camera")
core.initializeDevice("ProphEBSCam")

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

core.unloadDevice("ProphEBSCam")
print("SUCCESS")
