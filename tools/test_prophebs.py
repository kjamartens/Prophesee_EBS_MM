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

core.unloadDevice("ProphEBSCam")
print("SUCCESS")
