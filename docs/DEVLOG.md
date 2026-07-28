# Development Log — Prophesee EBS MicroManager Device Adapter

This file is the persistent memory for this project: what's been built, why,
and what's next. Read this first when picking the project back up in a new
session. It grows by one section per goal; do not delete earlier sections.

## Project roadmap (from `claude_instructions.txt`)

1. **Barebones device adapter** — loads in MicroManager, proves the build chain works. *(current)*
2. **Connection to the EBS** — find/select the camera over its interface, report model/serial.
3. **Minimal video feed** — integrate events over the last 100 ms into a frame, refreshed every 100 ms.
4. **Recording capabilities** — multi-dimensional acquisition writes a real `.raw` file.
5. **Adding properties** — expose EBS hardware settings (bias_on, hpf, etc.) as MM properties.
6. **Custom view methods** — configurable integration window, offset, common filters.
7. **Pixel-by-pixel differences** — pixel masking (blocked-pixel list) and ROI on the sensor.
8. **Full suite polishing** — tests, docs, error handling.

Each goal ends in a state the user tests locally before moving on; each is
tagged as a release (v0.1, v0.2, …) once confirmed working.

## Goal 1 — Barebones device adapter

### Status: implemented, builds successfully, awaiting user hardware/MM test

### What was built
- `third_party/mmCoreAndDevices` — git submodule (upstream Micro-Manager core/device-adapter kit), pinned at whatever commit was current when added (`main` branch, 2026-07-24). Provides `MMDevice.h`, `DeviceBase.h`, `ImgBuffer.h`, `ModuleInterface.h`, the `.props` build-config files, and the `MMDevice-SharedRuntime` project our adapter links against.
- `DeviceAdapter/ProphEBS/` — the new adapter:
  - `ProphEBS.h` / `ProphEBS.cpp` — `CProphEBSCamera : public CCameraBase<CProphEBSCamera>`. `Initialize()` logs `"ProphEBS adapter initialized (Goal 1 barebones - no EBS hardware connected yet)"` to the MM CoreLog and generates a static 640×480 8-bit checkerboard+gradient test pattern. `SnapImage()` always returns that same buffer — there is no real camera or event data involved yet. A small `ProphEBSSequenceThread` repeatedly re-inserts the same frame so MicroManager's **Live** view (not just single Snap) also works.
  - `ProphEBSModule.cpp` — the three required `MODULE_API` exports (`InitializeModuleData`, `CreateDevice`, `DeleteDevice`), registering one `MM::CameraDevice` named `"ProphEBS-Camera"`.
  - `ProphEBS.vcxproj` / `.vcxproj.filters` / `ProphEBS.sln` — builds a `DynamicLibrary` named `mmgr_dal_ProphEBS.dll` (the `mmgr_dal_` prefix is required by MMCore's device discovery and comes from the `TargetName` macro in the submodule's `MMDeviceAdapter.props`).

### Design decisions (and why)
- **Submodule, not vendored headers.** Keeps the MMDevice API in sync with upstream and lets us pull in future MM core fixes; the alternative (copy/pasting headers) would drift silently. Confirmed with user.
- **New top-level `DeviceAdapter/` folder, not inside the submodule's `DeviceAdapters/`.** Keeps our own code clearly out of the vendored tree so `git submodule update` never touches it, and avoids confusing it with the reference-only `micromanager_examples/` dump. Confirmed with user.
- **Standalone `ProphEBS.sln`** (just our project + `MMDevice-SharedRuntime`) instead of building inside the full upstream `micromanager.sln`. The full upstream solution pulls in dozens of adapters and expects Boost/protobuf/SWIG under `../../3rdpartypublic` that we don't have and don't need for this project — building only what we need avoids that entirely. Verified: `MMDeviceAdapter.props` adds a Boost include/lib path as a `AdditionalIncludeDirectories`/`AdditionalLibraryDirectories` entry, but since we never `#include` any Boost header, a non-existent path there is harmless.
- **No Metavision/Prophesee SDK dependency yet.** Deliberately out of scope until Goal 2 — nothing in Goal 1 touches hardware.
- **Sequence-acquisition thread included (not deferred).** Technically optional for "just prove it loads," but MicroManager's Live view exercises `StartSequenceAcquisition`, and getting that path working now (with fake data) means Goal 2/3 only need to swap the data source, not build threading from scratch.
- **Naming**: DLL/project `ProphEBS` → output `mmgr_dal_ProphEBS.dll`; class `CProphEBSCamera`; MM device name `"ProphEBS-Camera"`.

### Verified locally
- Built successfully with MSBuild from Visual Studio 2022 (this dev machine has the VS2022 **Preview** edition installed under `...\2022\Preview\...` — note this if `MSBuild.exe`/`cl.exe` aren't found under `...\2022\Community\...` on another machine; the tutorial in `docs/BUILD_AND_USAGE.md` covers both). Release|x64 build produced `DeviceAdapter/ProphEBS/build/Release/x64/mmgr_dal_ProphEBS.dll` with 0 warnings/errors.
- **Not yet verified**: actually loading the DLL inside MicroManager itself (Hardware Configuration Wizard, CoreLog message, Live view). That's the next step — waiting on the user to test on their machine per `docs/BUILD_AND_USAGE.md`.

### Open questions / TODO for later goals
- Goal 2 will need to decide: does the Prophesee camera enumerate over a COM port, USB, or does the Metavision HAL abstract that entirely (likely the latter — `Metavision::Camera::from_first_available()` / `from_serial()` from the SDK, no manual COM port handling needed). Investigate `Metavision::Device` / `I_HW_Identification::get_header()` (seen in `prophesee_examples/cpp/samples/metavision_event_frame_generation/metavision_event_frame_generation.cpp:199`) for model/serial reporting.
- When Goal 2 adds the Metavision SDK dependency, it will need its own `find_package`/include-path wiring — the current `.vcxproj` has no Metavision include/lib paths yet.
- Real sensor geometry (width/height) will replace the hardcoded `g_TestImageWidth`/`g_TestImageHeight` constants once Goal 2/3 can query the camera.
