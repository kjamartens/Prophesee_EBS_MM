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
for prop in ("EBS-ConnectionStatus", "EBS-Model", "EBS-Serial", "EBS-ConnectionType", "EBS-Integrator"):
    print(prop, "=", core.getProperty("ProphEBSCam", prop))

core.setCameraDevice("ProphEBSCam")
core.snapImage()
img = core.getImage()
print("Snap OK. Image shape:", img.shape, "dtype:", img.dtype)
core.unloadDevice("ProphEBSCam")
print("SUCCESS")
