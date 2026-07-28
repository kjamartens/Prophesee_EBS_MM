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

**Before handing a goal to the user for GUI testing**: build the adapter,
copy the DLL into the active MicroManager install (path from
`mmcore list`), and self-test via
`tools\mm_python_env\Scripts\python.exe tools\test_prophebs.py` (extend that
script with checks for any new behavior/properties the current goal adds).
This exercises the same `Initialize()`/property code path as the real GUI —
including real hardware detection, if an EBS happens to be plugged into the
dev machine — and catches real bugs (bad property values, a stale/locked DLL
copy, etc.) before the user spends time in the GUI. Only hand off to the user
for the parts that genuinely need the GUI or physical interaction (Hardware
Config Wizard, Live/Snap visual confirmation, Device/Property Browser).

## Goal 1 — Barebones device adapter

### Status: DONE — confirmed working by user, tagged v0.1

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
- **Loading in MicroManager verified end-to-end** via `pymmcore-plus` (see `tools/test_prophebs.py`): `loadDevice` → `initializeDevice` → `snapImage` → `getImage` succeeded, returning a `(480, 640) uint8` array — confirming both the module-load path and the static test image generation work correctly. Full in-GUI verification (Hardware Config Wizard, CoreLog message, Live view) is still up to the user per `docs/BUILD_AND_USAGE.md`, but the underlying adapter code is now known-good.

### Incident: device interface version mismatch (found & fixed same day)

The user's existing MicroManager install (`Micro-Manager_2.0.3_20260225`, from `pymmcore-plus`'s cache) was device interface version **74**, while our `mmCoreAndDevices` submodule (pinned to `main`) requires version **75** (see `DEVICE_INTERFACE_VERSION` in `MMDevice/MMDevice.h`). MMCore refuses to load any adapter whose interface version doesn't match exactly, and surfaces this as an unexpandable **"(unavailable)"** entry in the Hardware Configuration Wizard — with no further explanation in the GUI. This is generic to *any* custom adapter built against a bleeding-edge submodule commit, not specific to something we did wrong in `ProphEBS`.

**Fix applied:**

- Set up `tools/mm_python_env/` — a small dedicated Python venv with `pymmcore-plus` installed, used to query/manage MicroManager installs (`mmcore list`, `mmcore install`).
- `mmcore install` downloads MM's nightly installer, but it's built with an old Inno Setup requiring admin rights with no bypass flag — and the user does not have admin rights on this machine. Worked around this by downloading the installer manually and extracting its payload directly with `innounp` (an Inno Setup unpacker that reads the archive without ever running installer logic, so no elevation is needed), then copying the extracted `{app}\*` contents into the `Micro-Manager_2.0.3_20260724` folder under `pymmcore-plus`'s managed install directory. `mmcore list` now shows this install as `(active)` and DIV-75-compatible.
- Copied `mmgr_dal_ProphEBS.dll` into that new install folder and confirmed it loads (see "Verified locally" above).
- Documented both the standard installer path and this no-admin workaround in `docs/BUILD_AND_USAGE.md` (section 1c), since the user's machine isn't the only one this could happen on.

**Why this matters going forward:** every time the `mmCoreAndDevices` submodule is updated to a newer commit that bumps `DEVICE_INTERFACE_VERSION`, the installed MicroManager build may need refreshing too via `mmcore install` (or the no-admin workaround). If in a future goal the adapter suddenly shows "(unavailable)" again after a submodule update, check this first before assuming a code bug.

### Open questions / TODO for later goals
- Real sensor geometry (width/height) will replace the hardcoded `g_TestImageWidth`/`g_TestImageHeight` constants once Goal 3 builds the real event-integration frame.

## Goal 2 — Connection to the EBS

### Status: DONE — confirmed working by user, tagged v0.2

### What was built
- **Metavision SDK dependency wired into the build.** `ProphEBS.vcxproj` gained a `MetavisionSdkRoot` MSBuild property (defaults to `C:\Program Files\Prophesee`, overridable via `/p:MetavisionSdkRoot=...`), `AdditionalIncludeDirectories` for both `$(MetavisionSdkRoot)\include` (Metavision headers) and `$(MetavisionSdkRoot)\third_party\include` (the SDK's vendored OpenCV headers — `metavision/sdk/stream/camera.h` transitively includes `opencv2/core.hpp`), `AdditionalLibraryDirectories` for `$(MetavisionSdkRoot)\lib`, and `AdditionalDependencies` on `metavision_hal.lib`, `metavision_sdk_base.lib`, `metavision_sdk_stream.lib` (Debug config links the `_d` suffixed variants). `LanguageStandard` bumped to `stdcpp17` (Metavision's public headers use `std::filesystem`). Note: no OpenCV `.lib` needed at link time — our code never instantiates `cv::Mat` itself, only Metavision headers that forward-declare/use it.
- **`CProphEBSCamera::ConnectToCamera()`** (`ProphEBS.cpp`) — called from `Initialize()`. Tries `Metavision::Camera::from_first_available()`; on success, reads `cam_.get_facility<Metavision::I_HW_Identification>()` for serial, sensor name/generation, connection type, and integrator, and logs a one-line summary to the CoreLog. On any `std::exception` (covers `Metavision::CameraException` for "no camera found", missing driver/plugin, etc.) it does **not** fail `Initialize()` — it logs why and leaves the adapter in the Goal 1 fallback state.
- **Five new read-only string properties**, populated either way: `EBS-ConnectionStatus`, `EBS-Model`, `EBS-Serial`, `EBS-ConnectionType`, `EBS-Integrator`. These are what let the user "assure the connection is good" per the Goal 2 spec, directly from the Micro-Manager Device Property Browser — no code changes needed to inspect them.
- `cam_` (a `Metavision::Camera` member) is default-constructed and only replaced by `ConnectToCamera()`; it is **not** `start()`-ed — no event streaming happens yet, that's Goal 3. `Shutdown()` resets it to a fresh unopened `Camera` to release the HAL device handle.
- `SnapImage()`/Live view behavior is **unchanged** from Goal 1 (still the static checkerboard test pattern) — Goal 2 is scoped to connection/identification only, not image data.

### Design decisions (and why)
- **Never fail `Initialize()` on a missing camera.** The adapter must stay loadable and testable (via the Hardware Config Wizard, `tools/test_prophebs.py`, etc.) on a dev machine with no EBS attached — exactly like this one. A hard failure here would break that workflow and make every future goal impossible to iterate on without hardware present. The `EBS-ConnectionStatus` property surfaces the real reason (e.g. `Not connected: ... Error 101001: Camera not found...`) instead.
- **`cam_.get_facility<T>()` (throwing, Camera-level) over `cam_.get_device().get_facility<T>()` (pointer, Device-level).** Both exist in the SDK; the Camera-level one throws `CameraException` on an unsupported facility, which our existing `catch (const std::exception&)` around the whole connection attempt already handles — avoids a separate null-check branch for what would be a very unusual condition (a real Prophesee device without `I_HW_Identification`).
- **Metavision include/lib paths added directly to the `.vcxproj`, not via a `.props` file.** Only one project depends on the SDK so far; a shared `.props` (like the submodule's `MMDeviceAdapter.props`) would be premature until a second adapter/project needs the same paths.
- **`MetavisionSdkRoot` as an overridable MSBuild property, defaulting to the standard installer location.** Keeps the common case (`C:\Program Files\Prophesee`) a no-config build while still allowing a different SDK location without editing the `.vcxproj` by hand.

### Verified locally
- **Build**: `MSBuild ProphEBS.sln /p:Configuration=Release /p:Platform=x64` succeeds with 0 errors (Visual Studio 2022 Preview toolset, same as Goal 1).
- **Load + no-hardware fallback path**, via a `pymmcore-plus` script (ad hoc, same pattern as `tools/test_prophebs.py`): `loadDevice` → `initializeDevice` succeeded; `EBS-ConnectionStatus` correctly reported `Not connected: ... Error 101001: Camera not found. Check that a camera is plugged on your system and retry.` (the real Metavision SDK exception text, not a stub), all four identification properties correctly fell back to `"N/A"`, and `snapImage`/`getImage` still returned the Goal 1 test pattern `(480, 640) uint8` — confirming the whole chain (SDK link, HAL plugin discovery via `C:\Program Files\Prophesee\bin` on `PATH`, graceful no-camera handling, Goal 1 regression) works end-to-end on this machine, which has the Metavision SDK/runtime installed but no EBS physically attached.
- **Real hardware path also verified**: this dev machine turned out to have a real EBS plugged in. Re-running the same script with it connected gave `EBS-ConnectionStatus = Connected`, `EBS-Model = IMX636 (Gen 4.2)`, `EBS-Serial = 00051754`, `EBS-ConnectionType = USB`, `EBS-Integrator = Prophesee` — real values read back from the sensor via `I_HW_Identification`, not placeholders. Note: rebuilding and copying `mmgr_dal_ProphEBS.dll` into the MicroManager install folder occasionally needs a second `cp` attempt — Windows sometimes reports a stale/locked destination file as gone rather than overwritten on the first try (harmless, just re-copy and re-run if `loadDevice` reports the adapter name isn't recognized at all).
- **Full MicroManager GUI walkthrough confirmed by user**: Hardware Config Wizard → Device/Property Browser correctly showed the five `EBS-*` properties with real values from the connected sensor.

### Open questions / TODO for later goals
- If the user's machine doesn't already have the Metavision SDK installed at `C:\Program Files\Prophesee` (this dev machine does), `docs/BUILD_AND_USAGE.md` needs a Metavision SDK install section before Goal 2 can even build there — added.
- Runtime DLL search: `mmgr_dal_ProphEBS.dll` depends on `metavision_hal.dll` / `metavision_sdk_base.dll` / `metavision_sdk_stream.dll` at load time. On this machine the Metavision installer put `C:\Program Files\Prophesee\bin` on the system `PATH`, so Micro-Manager (and pymmcore-plus) find them automatically. If a future machine's installer doesn't do that, the DLL will fail to load with an "unavailable"-style error distinct from the Goal 1 device-interface-version one — worth remembering as a second entry in the troubleshooting table if it comes up.
- Goal 3 (minimal video feed) is where `cam_.start()` and the actual `cd().add_callback(...)` event stream get wired up, replacing the static test image with real 100 ms event-integration frames.
