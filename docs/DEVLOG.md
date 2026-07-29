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

## Goal 3 — Minimal video feed

### Status: DONE — confirmed working by user, tagged v0.3

### What was built
- **Real sensor geometry.** `ConnectToCamera()` now also reads `cam_.get_facility<Metavision::I_Geometry>()` for `sensorWidth_`/`sensorHeight_`, replacing the `g_TestImageWidth`/`g_TestImageHeight` fallback constants whenever a camera is connected (confirmed on hardware: IMX636 reports 1280×720). `Initialize()` resets the ROI to the full real frame in that case.
- **Event-driven frame builder.** `CProphEBSCamera::StartEventStreaming()` (called from `Initialize()` only when `cameraConnected_`) sizes the frame buffers and a per-pixel `eventCounts_` accumulator to the real sensor geometry, registers a CD (contrast-detection) event callback via `cam_.cd().add_callback(...)`, starts a new `ProphEBSFrameBuilderThread`, then calls `cam_.start()` — in that order, so nothing can be dropped before there's somewhere for it to go.
  - `CProphEBSCamera::OnEventsCD()` — the CD callback itself, invoked by the Metavision SDK on its own internal thread for every decoded event batch. Deliberately minimal: just increments `eventCounts_[y*w+x]` (clamped) under `eventCountsLock_`, since this runs on the sensor's own event-rate hot path.
  - `ProphEBSFrameBuilderThread` (new, mirrors `ProphEBSSequenceThread`'s pattern) — runs continuously once streaming starts, independent of whether MicroManager's Live view is active. Every `g_EventIntegrationMs` (100 ms, fixed for this goal) it calls `CProphEBSCamera::BuildAndSwapFrame()`.
  - `BuildAndSwapFrame()` — snapshots and resets `eventCounts_`, renders the counts into an 8-bit grayscale frame (`count * g_EventIntensityScale`, clamped to 255) in the back buffer, then swaps front/back pointers under `frontImgLock_`. This is the "integrate events over the last 100 ms into a frame, refreshed every 100 ms" requirement from the roadmap.
- **Double-buffered frame (`imgBufferA_`/`imgBufferB_` + `frontImg_`/`backImg_` pointers)**, replacing the single `img_` member from Goals 1/2. `GetImageBuffer()`/`GetImageWidth()`/`GetImageHeight()`/`GetImageBytesPerPixel()`/`GetImageBufferSize()`/`InsertImage()` all read through `frontImg_` under `frontImgLock_`. `SnapImage()` stays a no-op — `frontImg_` is already whatever's most current, whether that's the static test pattern (no camera) or the latest built event frame (camera streaming continuously in the background).
- **No hardware, no change**: with no EBS connected, `cameraConnected_` is false, `StartEventStreaming()` is never called, and behavior is byte-for-byte the Goal 1 static checkerboard — same code path (`GenerateTestImage()`), just now writing into `frontImg_` instead of a bare `img_`.
- `Shutdown()`/the destructor now call `StopEventStreaming()` (stop camera → stop frame-builder thread → remove CD callback, in that order) before releasing the HAL handle.
- Extended `tools/test_prophebs.py`: when `EBS-ConnectionStatus == "Connected"`, additionally snaps twice 250 ms apart (checks the event-integration plumbing runs without error) and runs a 500 ms `startContinuousSequenceAcquisition` burst (checks the Live-view path still works with the new frame source).

### Design decisions (and why)
- **Event-count accumulation, not a fancier representation (no polarity color, no exponential decay).** The roadmap step is explicitly "minimal video feed" — a per-pixel count of events in the window, linearly scaled to grayscale, is the simplest thing that visibly shows sensor activity. Polarity-aware (on/off) coloring, decay/persistence effects, and configurable integration windows are explicitly later goals (5/6).
- **Frame builder runs continuously once connected, not only while Live/Snap is active.** This matches how a real event camera behaves (always producing frames while streaming) and decouples "is MicroManager asking for frames" from "is the sensor producing them" — `SnapImage()` and the existing Goal-1 `ProphEBSSequenceThread` both just read whatever `frontImg_` currently is, no new coordination needed between them.
- **Double-buffer pointer swap over a shared locked buffer.** Considered locking one shared `img_` for the whole duration of every read; rejected because `GetImageBuffer()` returns a raw pointer that MMCore/pymmcore-plus read *after* the call returns, so a lock held only inside the accessor wouldn't actually prevent the frame builder from mutating memory mid-read (tearing). Swapping two whole-frame pointers under a lock is cheap and means a reader's pointer, once obtained, is never written to again until at least the next 100 ms window — no torn frames possible.
- **Per-event-batch locking in `OnEventsCD()`, not per-event.** The callback receives a `[begin, end)` batch already decoded by the SDK; locking once per batch (not once per event) keeps lock overhead off the sensor's actual per-event rate.
- **`g_EventIntegrationMs` and `g_EventIntensityScale` are fixed constants, not properties yet.** Goal 6 ("custom view methods") is explicitly where configurable integration window/offset/filters belong; adding a property for this now would be scope creep ahead of that goal.

### Verified locally
- **Build**: `MSBuild ProphEBS.sln /p:Configuration=Release /p:Platform=x64` succeeds with 0 warnings/errors (same VS2022 Preview toolset as Goals 1/2).
- **Real hardware, via `tools/test_prophebs.py`**: `EBS-ConnectionStatus = Connected`, `EBS-Model = IMX636 (Gen 4.2)`; `snapImage`/`getImage` returned `(720, 1280) uint8` — the real sensor geometry, not the Goal 1/2 640×480 fallback. Two snaps 250 ms apart both succeeded; a 500 ms continuous Live-style sequence acquisition buffered 34 frames without error.
- **Real event data confirmed, not zeros**: an ad hoc script snapping 5 frames ~150 ms apart showed nonzero pixel counts climbing from 89 to 124811 (out of 921600 pixels) with `max()=255` throughout — real ambient-light/motion-driven CD events landing on the sensor and being integrated correctly, not a stub or all-black frame.
- **Full MicroManager GUI Live/Snap view confirmed by user**: the live feed visibly reacted to real movement/light changes in front of the sensor.

### Open questions / TODO for later goals
- `g_EventIntensityScale = 32` is an arbitrary default, not calibrated against any particular scene/lighting — expect the live image to look very dim or oversaturated depending on ambient conditions until Goal 6 makes integration/gain configurable.
- No polarity distinction yet (ON vs OFF events both just increment the same counter) — a future goal could split these into separate channels or a diverging color map if that turns out to be useful.
- Goal 4 (recording) is where these same per-frame buffers (or the raw event stream itself, still TBD) get written out to a real `.raw` file via multi-dimensional acquisition.

## Goal 4 — Recording capabilities

### Status: DONE — self-tested on real hardware, awaiting user GUI confirmation before tagging v0.4

### Requirement clarification (asked user before implementing)

The roadmap wording ("record a multi-dimensional acquisition ... which then
creates simply the .raw file that is normally expected") was ambiguous
between (a) just proving MM's own MDA/TIFF-stack saving works with this
camera, or (b) producing the actual Metavision `.raw` event file via the
SDK's native recording API. Asked the user directly — they confirmed **(b)**:
hook MM's acquisition start/stop to call the Metavision SDK's own
`cam_.start_recording()`/`stop_recording()`, producing the real Prophesee
`.raw` event file.

A second question came up during design: device adapters have no visibility
into MM's MDA save directory (confirmed by grepping the `mmCoreAndDevices`
submodule — no such API exists; PVCAM's real adapter hits the same wall and
solves it with a plain settable property the user fills in by hand). The
user pushed back asking why this can't be automatic. Investigated further:
MM's save directory is a **Studio (Java)** concept, one layer above MMCore —
Studio calls generic MMCore functions with no path info attached, so there
is no hook inside the C++ device adapter to intercept it, full stop. Given
that hard constraint, the user chose: keep the scripted approach (a small
Beanshell script run in MM's Scripting Console bridges Studio's known path
down into the device via `core.setProperty(...)`), **plus** a property to
switch back to a fully automatic (if MDA-decoupled) path when the script
isn't used.

### What was built

- **Metavision native raw recording**, wired into the existing
  `StartSequenceAcquisition`/`StopSequenceAcquisition` methods (`ProphEBS.cpp`):
  - `StartSequenceAcquisition(long numImages, double interval_ms, bool)` —
    when `numImages != LONG_MAX` (i.e. **not** MicroManager's Live view,
    which calls the unbounded overload with `LONG_MAX`) and a camera is
    connected, calls the new `StartRawRecordingIfRequested()` before starting
    the sequence thread.
  - `StopSequenceAcquisition()` — calls the new `StopRawRecordingIfActive()`
    after stopping the sequence thread.
  - `ProphEBSSequenceThread::svc()` — a finite sequence reaching its
    `numImages_` count ends the loop on its own thread, without ever calling
    `StopSequenceAcquisition()`; added the same `StopRawRecordingIfActive()`
    call right before `AcqFinished()` so the `.raw` file is always finalized,
    not just on a user-triggered abort.
- **Two new properties**:
  - `EBS-RawFilePath` (string, read/write, default empty) — manual override.
    Non-empty: used verbatim, skipping auto-discovery. Empty (the default):
    the path is fully auto-discovered, see below.
  - `EBS-RawRecordingStatus` (string, read/write — **not** created read-only,
    since MM's `PropertyCollection::Set()` silently no-ops `SetProperty()`
    calls on read-only properties even from the owning device, and this one
    needs the device to update it live) — `Not recording`, `Recording to
    <path>`, `Finished: <path>`, or `Failed: <reason>`.
- **Automatic MDA-path discovery, entirely in C++, no scripting** (see the
  "Getting to zero-configuration" section below for how this was arrived
  at) — `TryGetMMAcquisitionRootPrefix()` and `FindMMUserProfilePath()`
  (anonymous namespace, top of `ProphEBS.cpp`) read
  `%LOCALAPPDATA%\Micro-Manager\UserProfiles\*.json` (MM Studio's own
  persisted UI-state file) using `boost::property_tree`'s JSON parser
  (header-only, already vendored under
  `C:\Program Files\Prophesee\third_party\include\boost\` by the Metavision
  SDK install -- no new `.vcxproj` dependency needed) to extract the Multi-D
  Acquisition dialog's current save root/prefix. `GenerateAutoRawFilePath()`
  (`Documents\ProphEBS_Recordings\ProphEBS_TIMESTAMP.raw`) is the last-resort
  fallback if that lookup fails for any reason.
- Extended `tools/test_prophebs.py` with three checks (only run when
  `EBS-ConnectionStatus == "Connected"`, same guard as the Goal 3 checks):
  a 10-frame acquisition with `EBS-RawFilePath` left empty produces a
  non-empty `.raw` file at the auto-discovered path; an explicit
  `EBS-RawFilePath` override does the same at the exact path given; and a
  Live-view burst (`startContinuousSequenceAcquisition`) does **not** start
  any recording (`EBS-RawRecordingStatus` never reads `Recording to ...`
  afterward) — confirming the Live-vs-MDA distinction actually holds.

### Design decisions (and why)

- **Distinguish MDA from Live view via `numImages == LONG_MAX`, not a new
  property.** `StartSequenceAcquisition(double)` (what Live view calls) was
  already implemented as a thin wrapper forwarding `LONG_MAX` to the finite
  overload (see Goal 1) — this existing signal is exactly "unbounded stream"
  vs. "known-length sequence," which maps directly onto "don't record Live"
  vs. "do record an MDA" with no new state needed.
- **Raw recording failure never fails the acquisition.** Wrapped in
  try/catch, reported via `EBS-RawRecordingStatus` and the CoreLog. The image
  feed (what Live/Snap/MDA actually display) must keep working even if the
  Metavision-side recording can't start for some reason (bad path, disk
  full, etc.) — consistent with the Goal 2 principle of never breaking the
  adapter's core loadable/usable state over a secondary feature. This same
  principle is why UserProfile auto-discovery failing is *also* not an
  error -- it just falls back to `GenerateAutoRawFilePath()`.
- **`EBS-RawRecordingStatus` created non-read-only**, unlike the Goal 2
  identification properties. Those are set once at `Initialize()` and never
  change again, so `true` (read-only) was fine. This one must change
  multiple times per adapter lifetime as recordings start/stop, and MM's
  `PropertyCollection::Set()` (confirmed by reading
  `third_party/mmCoreAndDevices/MMDevice/Property.cpp`) silently ignores
  `SetProperty()` on anything created read-only, with no error — so it had
  to be created writable for the device's own updates to actually take
  effect. (Being technically user-writable too is a harmless side effect,
  not a security/correctness concern for a local device property.)
- **Every `SetProperty(g_PropRawRecordingStatus, ...)` call is paired with an
  `OnPropertyChanged()` call.** `SetProperty()` alone only updates this
  device's own internal property map; MMCore keeps a separate `stateCache_`
  that the Device/Property Browser GUI actually displays from, refreshed
  only on a Core-initiated `setProperty()` or a device-initiated
  `OnPropertyChanged()` callback. Missing this made the GUI show a stale
  status even though a direct `getProperty()` already returned the correct
  value -- see the bug-hunting narrative below for how this was found.
- **Auto-path fallback folder is `Documents\ProphEBS_Recordings\`, not a temp
  or install-relative folder.** Always writable without admin rights (same
  constraint noted in Goal 1's device-interface-version incident), and
  discoverable by a non-technical user browsing Windows Explorer.

### Getting to zero-configuration: three iterations, in order

This goal went through three designs before landing on the final one. Kept
here in full because the reasoning for *rejecting* the first two is exactly
the kind of thing worth remembering before reinventing them in a later goal.

**Iteration 1 -- `EBS-RawAutoPath` on/off toggle + manual Beanshell script.**
First implementation: a private path under `Documents\ProphEBS_Recordings\`
by default (`EBS-RawAutoPath=On`), or an `EBS-RawFilePath` the user sets by
hand (`EBS-RawAutoPath=Off`) via a Beanshell script
(`scripts/SetProphEBSRawPath.bsh`, run manually in the Scripting Console
before clicking Acquire!) that read Studio's `SequenceSettings` root/prefix
and pushed it into the property. Self-tested successfully via
`tools/test_prophebs.py` (auto and explicit paths both produced real,
non-empty `.raw` files on the connected IMX636). User tried it for real in
the GUI and initially reported it matched their MDA folder correctly --
but this turned out to be based on reviewing the script's logic, not a full
successful run; see Iteration 3.

Along the way the user asked whether the script could be avoided entirely
("baked into the dll"). At the time this looked impossible: MM's MDA save
directory is Studio (Java)-level state that never reaches MMCore or the C++
device layer -- Studio only ever calls generic MMCore functions with no path
info attached. This is true as far as the *live* Studio object model goes,
but turned out not to be the whole story -- see Iteration 3.

**Iteration 2 -- auto-attaching Beanshell hook.** As a partial answer to
"can this be automatic," added `scripts/AttachProphEBSRawPathHook.bsh`,
using MM's `IAcquisitionEngine2010.attachRunnable(frame, position, channel,
slice, Runnable)` hook API to attach the same path-sync logic once per MM
session (or via a Startup script) instead of re-running it before every
MDA. This still required *some* one-time Beanshell setup, which the user
explicitly did not want ("I do NOT want users to fiddle around with a bsh
script or have a startup thing"), so both scripts were later deleted
entirely in favor of Iteration 3.

**Bug found while testing Iteration 1/2 in the real GUI**:
`EBS-RawRecordingStatus` displayed a stale path in the Device/Property
Browser. Root cause (see "Design decisions" above): missing
`OnPropertyChanged()` calls after `SetProperty()` on that property, so
MMCore's `stateCache_` (what the GUI reads) never refreshed, even though a
direct `getProperty()` query already returned the correct value -- exactly
why `tools/test_prophebs.py` (which uses `core.getProperty()`) never caught
it. **Lesson for future goals**: any property a device updates on its own
initiative needs a paired `OnPropertyChanged()` call, and the Python
self-test harness cannot verify GUI-cache-consistency bugs like this one --
only a real Device/Property Browser check can. Fixed by adding the missing
calls (kept in the final design).

**The actual "file doesn't get stored anywhere" bug**: after the
`OnPropertyChanged()` fix, the user tried a real MDA and reported the
reported path was correct but *no file appeared there at all* -- the MDA
only produced its own `.tif`. Root cause (not fully certain, but consistent
with all observed symptoms): MicroManager's own save folder may not exist
yet at the exact moment `StartSequenceAcquisition()` fires and
`cam_.start_recording()` is called -- Studio likely creates the folder
lazily around the first saved image, not synchronously when Acquire! is
clicked -- so asking the Metavision SDK to write into a not-yet-existing
directory can silently fail (its writer thread runs asynchronously per the
SDK docs, so a failure there doesn't necessarily propagate back as an
exception or a `false` return). The user's own suggestion at this point was
"copy the file after it's created instead" -- sidestepping the whole timing
question by writing to a guaranteed-safe folder first and moving the result
afterward, once the MDA's folder is certain to exist. A
`scripts/CopyProphEBSRawToMDA.bsh` was drafted around this, but immediately
made moot by Iteration 3, discovered minutes later.

**Iteration 3 (final) -- read MM's own persisted UserProfile JSON,
directly from C++, zero scripts.** Prompted by the user's explicit ask to
"really dive into all documentation... latch onto unorthodox methods if you
have to" rather than accept another scripting workaround. Investigated
where MM Studio persists its UI state and found
`%LOCALAPPDATA%\Micro-Manager\UserProfiles\<profile>.json` -- a "Micro-Manager
Property Map" format JSON file MM writes as a normal side effect of using
the application (confirmed present and populated on the dev machine without
any special setup). Inside it,
`map.Preferences.scalar["org.micromanager.internal.dialogs.AcqControlDlg"].scalar.MDA_SEQUENCE_SETTINGS.scalar`
holds a second, embedded JSON string (MM serializes its `SequenceSettings`
object to text before storing it) containing the *exact* `root`/`prefix`
values currently configured in the MDA dialog.

Verified this was parseable before touching the adapter: wrote a standalone
scratch `.cpp` using `boost::property_tree::json_parser` (already vendored,
header-only, under `C:\Program Files\Prophesee\third_party\include\boost\`
-- confirmed by grepping for `json_parser.hpp`/`json.hpp` there -- so no new
`AdditionaIncludeDirectories`/link dependency needed), compiled it standalone
against the real profile file on the dev machine, and got back the correct
`root=C:\Data\EBSMMTest3`, `prefix=test` that matched what the user's real
MDA dialog was configured to. Only after that stand-alone proof did this get
wired into `ProphEBS.cpp` as `FindMMUserProfilePath()` +
`TryGetMMAcquisitionRootPrefix()`.

This is undocumented/internal MM implementation detail (a specific Java
class's own preference key, not any kind of stable public API), which is
exactly why it's wrapped in try/catch at every step with a safe fallback
(`GenerateAutoRawFilePath()`) rather than treated as guaranteed to work
forever. `EBS-RawAutoPath` was removed entirely (no longer needed -- there's
only one path-resolution behavior now, with `EBS-RawFilePath` as a manual
override for the rare case auto-discovery isn't wanted), and both
`.bsh` scripts and the top-level `scripts/` folder were deleted.

### Follow-up: auto-discovered path was stale when changed mid-session

User confirmed the UserProfile auto-discovery approach worked, but reported
that changing the MDA's save folder in the dialog wasn't reflected -- the
`.raw` kept landing in the *previous* folder. Investigated by re-checking
the actual profile JSON's `root`/`prefix` values against what the user had
just changed them to on their machine, and confirmed the file **does**
eventually update to the new value -- but not necessarily by the moment
`StartSequenceAcquisition()` fires. Most likely explanation: MicroManager's
`UserProfile` save mechanism doesn't necessarily flush every keystroke/field
edit straight to disk (probably some internal debounce/periodic-save
behavior) -- so reading the file at the *instant* recording starts risks
catching a value that's a few seconds stale if the user only just edited the
path before clicking Acquire!.

**Fix**: moved auto-discovery from `StartRawRecordingIfRequested()` to
`StopRawRecordingIfActive()`, and changed the whole strategy from "write
directly to the (guessed) MDA path" to "always stage to a local,
guaranteed-safe path during recording, then move the finished file to the
freshly-re-discovered MDA path afterward":

- `StartRawRecordingIfRequested()` now always records to
  `GenerateAutoRawFilePath()`'s local staging path when `EBS-RawFilePath` is
  empty (an explicit `EBS-RawFilePath` is still used verbatim, un-staged).
  A new `movePendingToMdaFolder_` flag records whether this was a staged
  (auto) recording or an explicit one.
- `StopRawRecordingIfActive()` -- called only after the whole acquisition
  has finished, giving MicroManager's own save timing the maximum possible
  window to have flushed the new path to disk -- re-runs
  `TryGetMMAcquisitionRootPrefix()` fresh at that point (when
  `movePendingToMdaFolder_` is set) and moves (`fs::rename`, falling back to
  copy+delete for cross-drive moves) the just-finished file there.
- This also happens to fully replace the earlier "MDA folder might not exist
  yet when recording starts" concern from the previous incident (see above)
  with something strictly more robust: recording no longer ever touches the
  MDA folder directly at all during the write -- only a same-machine
  filesystem move afterward, once both the local file and (presumably) the
  MDA folder already exist.

**Second bug found while verifying this fix**: the Metavision SDK also
writes a companion `<same-stem>.bias` file (a snapshot of the sensor's bias
settings) next to the `.raw` at recording time -- undocumented in the
headers available here, discovered by inspecting the staging folder after a
test run and finding an orphaned `.bias` file left behind while the `.raw`
had moved. Fixed by moving (or copy+delete-ing) `<stem>.bias` alongside the
`.raw`, if it exists, using the same move-with-fallback helper (refactored
into a small local lambda since it's now needed twice).

**Verified**: re-ran `tools/test_prophebs.py` against real hardware after
this fix. The auto-discovery check correctly picked up
`C:\Data\EBSMMTest5` / `test2` (values the user had changed to *after* the
original `C:\Data\EBSMMTest3` / `test` test, confirming the fix actually
uses the current, not stale, profile value) and both the `.raw` and its
companion `.bias` file were confirmed present together at the destination,
with none left orphaned in the staging folder.

**Note**: the user's own earlier real-GUI test recordings (several
`.raw`/`.bias` pairs from the same dev session, timestamped `09:37`-`09:48`)
are still sitting in `Documents\ProphEBS_Recordings\` from before this fix
was deployed -- left in place since they're the user's own test data, not
cleaned up automatically.

### Follow-up: the deferred-read fix *still* didn't pick up mid-session folder changes

User reported it still wasn't working: rebuilt/redeployed the DLL with the
above fix, fully closed and reopened MicroManager (ruling out a stale-DLL
explanation), then set an MDA to `EBSMMTest4`/`test`, ran it (worked
correctly), changed the MDA to `EBSMMTest5`/`test2` *without restarting MM*,
ran again -- and the `.raw` still landed in `EBSMMTest4`. Confirmed via a
direct question that this was all one continuous MM session, never
restarted in between.

**Root cause, this time for real**: the `UserProfile` JSON only gets
flushed to disk when MicroManager *closes* -- not live, not on any
periodic timer, not on field-blur. Every earlier observation of the file
reflecting a "fresh" value coincidentally followed an MM restart (needed
anyway to reload the rebuilt DLL each time), which masked this. So
deferring the read from start-of-recording to end-of-recording (the
previous fix) bought at most ~1 extra second of margin -- utterly
irrelevant against a source that might not update again until the
application closes. The whole `UserProfile`-based approach can only ever
reflect the *previous* session's settings during a live session -- a
fundamental dead end for the realistic "change folder, acquire again"
workflow, not a timing bug fixable by reading it later.

**The actual live source, found by reading the adapter's own CoreLog**:
grepped the real GUI session's CoreLog file (the same file
`LogMessage()` calls from this adapter already write into) for context
around the reported failure, and found MicroManager Studio logs the block

```text
[IFO,App] MDA Settings:
{
  ...
  "root": "C:\\Data\\EBSMMTest4",
  "prefix": "test",
  ...
}
```

synchronously, right before triggering the acquisition engine, for *every*
acquisition run in the session -- and each occurrence had the exact,
current, freshly-changed root/prefix from that specific run. This is a
different mechanism than the `UserProfile` file entirely: it's a direct
per-acquisition log write, not a periodically-flushed cache.
Confirmed the process-ID linkage that makes this readable from C++ without
Java: `MMCore`/this DLL loads into the *same OS process* as Studio's JVM,
and the CoreLog filename embeds that process's PID
(`CoreLog<timestamp>_pid<PID>.txt`) -- `GetCurrentProcessId()` from inside
this DLL matches it exactly.

**Fix**: added `TryGetMdaRootPrefixFromCoreLog()` (new primary discovery
method) alongside the existing `TryGetMdaRootPrefixFromUserProfile()`
(demoted to fallback, kept since a stale-but-plausible path still beats
none), combined via `TryDiscoverMdaRootPrefix()`:

- `GetOwnModuleDirectory()` -- finds this DLL's own directory (== the MM
  install directory, where `CoreLogs\` lives) via the "address of a
  function in this module" `GetModuleHandleEx` trick, needing no `DllMain`.
- `FindCurrentCoreLogPath()` -- finds the CoreLog file whose name ends in
  `_pid<GetCurrentProcessId()>.txt`.
- `TryGetMdaRootPrefixFromCoreLog()` -- tails that file for the last
  `"MDA Settings:"` block, strips MM's per-line log-continuation prefix
  (`...[       ]`) off each wrapped line, and parses the reassembled text
  as JSON for `root`/`prefix`.
- `StopRawRecordingIfActive()` now calls `TryDiscoverMdaRootPrefix()`
  instead of the UserProfile lookup directly.

**Second bug found immediately while verifying this fix**: an
automated-test run (`tools/test_prophebs.py`, via `pymmcore-plus`, no Java
Studio at all) picked up a completely wrong, stale root/prefix
(`EBSMMTest4`/`test3`, from a CoreLog file whose last entry showed
`"Core session ended"` **11 minutes earlier**). Root cause: Windows reuses
process IDs once a process exits, and the freshly-started `python.exe` test
process had been assigned a PID that collided with an old, already-finished
MicroManager session's leftover CoreLog file -- `FindCurrentCoreLogPath()`
was matching by filename suffix alone, with no way to tell "this file
belongs to a dead process that used to have my PID" apart from "this file
belongs to me." **Fix**: compare the candidate file's own creation time
(`GetFileAttributesEx` -> `ftCreationTime`) against this process's start
time (`GetProcessTimes` -> `creationTime`); only trust files created at or
after this process began. An old PID-collision leftover necessarily
predates the current process and gets correctly rejected now.

**Real limitation of the self-test harness, found while investigating the
above**: `tools/test_prophebs.py` never exercises
`TryGetMdaRootPrefixFromCoreLog()`'s intended (primary) codepath at all.
`pymmcore-plus` calls `core.setPrimaryLogFile()` itself, pointing MMCore's
logging at its own package-managed location instead of
`<install_dir>/CoreLogs/` -- so no matching, freshly-created CoreLog file
ever exists under the DLL's own directory during one of these test runs,
and `TryDiscoverMdaRootPrefix()` *always* falls through to the
`UserProfile` fallback in this harness, by construction. This means the
whole point of this fix -- correctly tracking a same-session MDA folder
change -- is **only verifiable by the user, live, in the real MicroManager
GUI** (which does write to the default `CoreLogs\` location). The
automated test still passing after this fix is evidence of "no regression
in the fallback path," not evidence the primary fix works.

**User confirmed working** in the real GUI: set an MDA to one folder,
acquired, changed to a different folder without restarting MM, acquired
again -- the second `.raw` correctly landed in the new folder. The
CoreLog-based primary discovery mechanism is now considered proven, not
just self-tested via the fallback path.

### Follow-up: match MM's own per-run numbered subfolder convention

User pointed out MM doesn't save directly into `<root>/` -- for a saving
MDA (e.g. `MULTIPAGE_TIFF`) it creates a per-run subfolder
`<prefix>_<N>/`, where `N` is one more than the highest such subfolder
number that already exists under `<root>` (confirmed by the user: renaming
an existing `test2_1` folder to `test2_34` makes MM's *next* run
`test2_35` -- it's a live filesystem scan each time, not a remembered
counter, and this number never appears in the CoreLog). Inside that
folder, the image stack is also named after it, e.g.
`test2_2_MMStack_Pos0.ome.tif`. User wants the `.raw`/`.bias` to follow the
exact same convention: `<root>/<prefix>_<N>/<prefix>_<N>_events_Pos0.raw`
(and `.bias`).

**Implementation** (`FindHighestNumberedMdaSubfolder()`, new): rather than
re-deriving `N` independently (which risks an off-by-one mismatch against
MM's own counting logic, especially since MM's rule is "highest existing +
1" rather than "remembered count + 1"), this scans `mdaRoot` for
subfolders matching `<mdaPrefix>_<digits>` and reuses whichever one has
the highest number. This is safe specifically *because*
`StopRawRecordingIfActive()` only ever runs after the whole acquisition has
finished -- by then, MM has already created and been writing into its own
folder for the current run throughout, so it's guaranteed to exist and
(being the most recent) to be the highest-numbered one. A short retry loop
(up to 4 attempts, 150ms apart) covers the unlikely case that this runs
before MM's own folder-creation has landed on disk. If no numbered
subfolder ever appears (e.g. a save mode that doesn't use one, or saving
disabled), falls back to the previous flat layout
(`<root>/<prefix>_prophesee_events.raw`) unchanged.

**Verified**: simulated MM's own convention by hand-creating
`test2_1/` and `test2_2/test2_2_MMStack_Pos0.ome.tif` under a discovered
root, then ran `tools/test_prophebs.py` -- the recording correctly landed
at `test2_2\test2_2_events_Pos0.raw` (matching the *highest*-numbered
folder, `test2_2`, not `test2_1`), with its `.bias` companion alongside it
and the pre-existing `.ome.tif` from the simulated MM run untouched. Test
folders/files cleaned up afterward.

### Verified locally

- **Build**: `MSBuild ProphEBS.sln /p:Configuration=Release /p:Platform=x64`
  succeeds with 0 warnings/errors (same VS2022 Preview toolset as Goals 1-3),
  including the new `boost::property_tree` include.
- **Real hardware, via `tools/test_prophebs.py`** (final version): all three
  Goal 4 checks passed against the real connected IMX636 sensor. Critically,
  the auto-discovery check produced
  `EBS-RawRecordingStatus = Finished: C:\Data\EBSMMTest3\test_prophesee_events.raw`
  -- `C:\Data\EBSMMTest3` / `test` read directly out of the user's real,
  live MM UserProfile JSON, with the property left completely empty (no
  script, no manual property setting) -- a 27 MB non-empty `.raw` file
  actually existed there. The explicit-`EBS-RawFilePath` override and the
  Live-view-doesn't-record checks also passed. All test-generated `.raw`
  files were deleted afterward (proof-of-plumbing only, not meaningful
  recordings worth keeping).
- **Not yet verified**: a real Multi-D Acquisition run entirely through the
  MicroManager Studio GUI (as opposed to `pymmcore-plus` driving the same
  calls) -- per `docs/BUILD_AND_USAGE.md` Goal 4 section, this is the
  hand-off step for the user. Given the self-test now reads the *real*
  UserProfile file the GUI itself writes to, this should now "just work,"
  but the GUI round-trip itself is still unverified by this harness.

### Follow-up: stream directly to the MDA folder instead of staging-then-moving

User asked for two more things in the same session. First: since
`TryGetMdaRootPrefixFromCoreLog()` is synchronously fresh (unlike the old
`UserProfile` approach), why still stage every recording locally and move
it afterward, for large datasets that's an unnecessary double write.

**Implementation**: `StartRawRecordingIfRequested()` now tries MDA
auto-discovery immediately, and if it resolves (root/prefix found, `"save"`
is `true`, and MM's numbered subfolder appears within a short retry window
-- `ComputeNumberedMdaDestination()`, factored out of the stop-time move
logic so both call sites share it), `cam_.start_recording()` writes
straight to the final destination, no local staging or later move at all.
Local staging (`GenerateAutoRawFilePath()`) is now purely the fallback for
when discovery fails, `"save"` is unchecked, or the folder didn't appear in
time -- in which case `StopRawRecordingIfActive()`'s existing move-at-the-end
logic is still there as a second chance, unchanged.

Also fixed a related correctness gap surfaced by this change: the MDA
settings JSON has a `"save"` boolean (whether "Save images" is checked at
all). Both `TryGetMdaRootPrefixFromCoreLog()` and
`TryGetMdaRootPrefixFromUserProfile()` now also return this, and neither
call site attempts numbered-subfolder discovery when it's `false` -- without
this check, an unsaved acquisition could have been mis-attributed into some
unrelated *older* run's numbered folder (whichever happened to be the
highest-numbered one lying around), which would have been actively wrong,
not just a missed optimization.

**Verified**: `tools/test_prophebs.py` incidentally exercised the `"save":
false` path for real -- the user's live profile happened to have "Save
images" unchecked at test time, and the recording correctly fell back to
local staging rather than guessing a folder. Note (same caveat as the
CoreLog work generally): this harness still can't exercise the *true*
start-time direct-streaming path itself (no CoreLog written by
`pymmcore-plus` in the expected location -- see the earlier follow-up on
this), since discovery here always fell through to the `UserProfile`
fallback; the shared `ComputeNumberedMdaDestination()`/
`FindHighestNumberedMdaSubfolder()` logic itself was already proven
correct (same code, exercised from the stop-time call site in the previous
follow-up). GUI confirmation from the user still pending for this specific
change.

### Investigated and declined: recording an unsaved MDA that's saved manually afterward

User's other ask: when an MDA runs with "Save images" *unchecked*, MM keeps
the images in a RAM-backed datastore and displays them in a viewer; the
user can later click that window's **Save** button to write it to disk,
choosing the location at that point -- well after this adapter's own
recording (and `StopRawRecordingIfActive()`) has already finished. Wanted
the `.raw`/`.bias` moved to match, using the same naming convention, at
that later point.

**This was investigated properly, not just log-grepped, and declined --
there is no live signal to hook.** The user had already confirmed the
CoreLog has nothing for this. Went a level deeper: extracted
`MMJ_.jar` (`plugins/Micro-Manager/MMJ_.jar` in the MM install --
confirmed via `unzip -l | grep DefaultDatastore` that this jar holds the
Studio classes referenced elsewhere in this log) and inspected the actual
compiled classes involved in a manual save:

- `SaveButton` (the image window's Save button) just wires a click to
  `Datastore.save(...)` -- no logging of any kind in the class itself.
- `DefaultDatastore.save()` / `DefaultDataSaver` (the `SwingWorker` that
  actually writes the files in the background) contain no log statement
  that includes the destination path -- confirmed by reading every string
  constant in both compiled classes; the only related string is the
  generic error `"Failed to save data"`.
- The Save dialog's "last used directory" is remembered via
  `FileDialogs.setDirectory()`/`getDirectory()`, which -- checked directly
  in its decompiled constant pool -- goes through MM's own `UserProfile`
  object, i.e. the *exact same* flush-only-on-MM-close mechanism already
  ruled out for this purpose (confirmed it's not a separate,
  faster-flushing `java.util.prefs`-backed store).

So unlike the earlier CoreLog breakthrough, there is no textual, live
signal to grab onto here at all -- not "we haven't found it yet," but "the
relevant compiled code doesn't write one anywhere queryable while MM is
still running." Presented this finding plus two options (a manual
`EBS-MoveLastRecordingTo`-style property vs. leaving it as-is); user chose
to leave it as-is. **No code changes made for this request** -- an unsaved
MDA's recording simply stays in `Documents\ProphEBS_Recordings\` (already
the existing fallback behavior), and matching a later manual save is a
manual step for the user if/when it comes up.

### Follow-up: make the staging/fallback folder itself configurable

Final ask of the session: expose the `Documents\ProphEBS_Recordings\`
staging folder (used whenever MDA-folder discovery doesn't resolve a direct
destination -- "Save images" unchecked, discovery failure, etc.) as a
property instead of a hardcoded path.

**Implementation**: new `EBS-TempRecordingFolder` property (string,
read/write, default empty). `GenerateAutoRawFilePath()` now reads it via
`GetProperty()`; non-empty overrides the `Documents\ProphEBS_Recordings\`
default with the given folder (created if needed), empty keeps the
existing default. No other logic changed -- this only affects where the
one fallback/staging function writes.

**Verified**: `tools/test_prophebs.py` extended with a 4b2 check -- set
`EBS-TempRecordingFolder` to a scratch folder, ran a 5-frame acquisition
(landed there because the live profile's `"save": false` state on this
machine routes it through the fallback path, same as earlier follow-ups),
confirmed the `.raw` appeared in the custom folder, not the default one.
Test folder cleaned up afterward.

### Open questions / TODO for later goals

- The exact MM UserProfile JSON key path
  (`org.micromanager.internal.dialogs.AcqControlDlg` / `MDA_SEQUENCE_SETTINGS`)
  is internal MM implementation detail, observed on this dev machine's
  MicroManager 2.0.3 install -- if a future MM version restructures this,
  auto-discovery will silently fall back to `GenerateAutoRawFilePath()`
  (never breaks recording, just stops landing in the MDA folder). Worth
  rechecking this key path if the adapter is ever tested against a
  significantly newer/older MM release.
- The MM UserProfile file reflects whatever was last flushed to disk by
  Studio's own (not fully understood) save-timing -- if a user edits the
  root/prefix field and clicks Acquire! within under a second, there's a
  theoretical chance the on-disk value hasn't caught up yet. Not something
  we've been able to trigger or verify either way; flag if a user reports
  the auto-discovered path looking "one acquisition behind."
- If multiple MM user profiles exist (`Index.json` lists more than one),
  `FindMMUserProfilePath()` uses the first entry, falling back to the most
  recently modified `*.json` file if that one's unreadable -- untested with
  a genuinely multi-profile MM setup (this dev machine only has "Default
  User").
- No attempt yet to stitch the recorded `.raw` file's timing back to
  MicroManager's own saved image stack/metadata (e.g. embedding the `.raw`
  path as OME metadata on the MM-side images) — out of scope unless the user
  asks for it later.
- Goal 5 (adding properties) is where the EBS's own hardware settings
  (bias_on, hpf, etc.) become MM properties — recording itself is now
  considered functionally complete pending the user's GUI confirmation.

## Goal 5 — Adding hardware-setting properties

### Status: self-tested on real hardware, awaiting user GUI confirmation before tagging v0.5

### What was built

Confirmed every Metavision HAL facility used below actually exists (read
directly from the installed SDK headers under
`C:\Program Files\Prophesee\include\metavision\hal\facilities\`, not
guessed) before writing any code against it: `I_LL_Biases`, `I_ErcModule`,
`I_EventTrailFilterModule`, `I_AntiFlickerModule`, `I_Monitoring`, plus
`I_HW_Identification::get_current_data_encoding_format()` and
`DeviceConfig::enable_biases_range_check_bypass()`.

- **`EBS-BiasRangeCheckBypass`** — a pre-init-only property ("Off"/"On",
  default "Off"), created in the *constructor* (not `Initialize()`) via
  `CreateStringProperty(..., isPreInitProperty=true)`, mirroring
  Metavision Studio's "bypass biases range check" checkbox.
  `ConnectToCamera()` reads it and passes a `Metavision::DeviceConfig`
  with `enable_biases_range_check_bypass(...)` into the
  `Camera::from_first_available(const DeviceConfig&)` overload.
- **Bias properties, fetched from the camera, not hardcoded** —
  `CreateBiasProperties()` calls `I_LL_Biases::get_all_biases()` on a
  connected camera and creates one `EBS-<biasname>` Integer property per
  key the sensor actually reports (`EBS-bias_diff`, `EBS-bias_diff_off`,
  `EBS-bias_diff_on`, `EBS-bias_fo`, `EBS-bias_hpf`, `EBS-bias_refr` on
  the IMX636 here, but generalizes to any sensor), defaulting to the
  value already on the sensor (its own boot-time default) with range
  limits from `LL_Bias_Info::get_bias_range()`. A single `OnBias()`
  handler (keyed off `pProp->GetName()`) backs all of them. When no
  camera is connected, falls back to the fixed
  `{bias_diff, bias_diff_off, bias_diff_on, bias_fo, bias_hpf, bias_refr}`
  list from the user's original request, defaulting to 0 and backed by a
  local `std::map` instead of hardware.
- **ERC (event rate control)** — `EBS-ERC-Enabled` (default Off) and
  `EBS-ERC-EventRate` (default 50,000,000 = 50 Mev/s), via
  `I_ErcModule`, range-limited from `get_min/max_supported_cd_event_rate()`
  when connected.
- **Event trail (STC) filter** — `EBS-EventTrailFilter-Enabled` (default
  Off), `-Threshold` (default 10,000 µs = 10 ms), `-Mode` (string enum
  `TRAIL`/`STC_CUT_TRAIL`/`STC_KEEP_TRAIL`, default `TRAIL`, restricted to
  `I_EventTrailFilterModule::get_available_types()` when connected). Via
  `I_EventTrailFilterModule`.
- **Anti-flicker** — `EBS-AntiFlicker-Enabled` (default Off),
  `-StartThreshold`/`-StopThreshold` (default 6/4), `-DutyCycle` (default
  50%), `-FilterType` (`"Band Pass"`/`"Band Cut"`, default `"Band Cut"` —
  mapped to the SDK's `BAND_PASS`/`BAND_STOP` respectively, using
  Metavision Studio's UI wording rather than the SDK's own enum names).
  Also `-LowFreq`/`-HighFreq` (default 50/60 Hz, mains hum) — the
  mandatory frequency band the filter needs to do anything, not in the
  user's original list but added per their explicit decision when asked.
  Via `I_AntiFlickerModule`.
- **Static read-only info** — `EBS-Generation` (e.g. `"4.2"`, from
  `SensorInfo`) and `EBS-DataEncodingFormat` (e.g. `"EVT3"`, from
  `get_current_data_encoding_format()`), read once in `ConnectToCamera()`
  alongside the existing Goal 2 identification properties.
- **Live monitoring properties** — `EBS-AvgDataRate-MBps`,
  `EBS-AvgEventRate-MEvps`, `EBS-AvgERCDropRate-KEvps`,
  `EBS-Temperature-C`, `EBS-Illumination-lux`, `EBS-PixelDeadTime-us`. A
  new `ProphEBSStatsThread` (mirrors `ProphEBSFrameBuilderThread`) calls
  `UpdateStats()` every `g_StatsIntervalMs` (1 s) while streaming:
  - Data/event rate are measured, not estimated — two new
    `std::atomic<uint64_t>` counters (`totalRawBytes_`, `totalEventCount_`)
    incremented by a new `cam_.raw_data().add_callback(...)` (real byte
    buffer sizes) and one added line in the existing `OnEventsCD()`
    (`end - begin`), both cheap single atomic adds per batch, same
    off-the-hot-path design as the existing `eventCounts_` accumulator.
  - ERC drop-rate is an **estimate**, per explicit user decision, since
    the HAL has no API reporting actual hardware-dropped event count:
    `enabled ? max(0, target_cd_event_rate − measured_event_rate) : 0`.
  - Temperature/illumination/pixel-dead-time are polled directly from
    `I_Monitoring`, each in its own `try`/`catch` (this sensor throws on
    `get_illumination()` specifically but not the other two — see bug
    below).

### Design decisions (and why)

- **Live-stats properties read a cached `std::atomic<double>` in
  `BeforeGet`, rather than being pushed via `SetProperty()` +
  `OnPropertyChanged()` from the stats thread.** This was the *original*
  design (following the `EBS-RawRecordingStatus` pattern from Goal 4) and
  it caused a real, repeatable crash during self-testing — see "Bug
  found" below for the root cause and why Goal 4's pattern doesn't
  actually generalize to a property that updates continuously for the
  device's entire lifetime.
- **`EBS-BiasRangeCheckBypass` is flagged pre-init via
  `isPropertyPreInit()`, not actually made read-only at the MMCore level
  after `Initialize()`.** Investigated this by reading
  `MMDevice/Property.cpp::PropertyCollection::Set()` directly: it only
  ever checks the per-property `readOnly_` flag, never
  `IsPropertyPreInit()` — so a post-init `setProperty()` call is not
  blocked by MMCore itself. The "settable during setup only" enforcement
  is a Hardware Configuration Wizard (GUI) convention built on top of
  this flag, not a Core-level guarantee. Adjusted `tools/test_prophebs.py`
  to assert `isPropertyPreInit()` rather than a read-only transition,
  since the latter isn't actually true.
- **Anti-flicker's mandatory frequency band exposed as two more
  properties, not hardcoded**, and **range-limited from
  `get_min/max_supported_frequency()`** — the second half of this
  decision was only added after self-testing surfaced the "Bug found"
  below.
- **`EBS-AvgDataRate-MBps` measures real transferred bytes via
  `raw_data().add_callback()`, not an estimate from event count ×
  assumed bytes/event** — EVT2/EVT3 have variable bytes-per-event, so a
  computed estimate would have been meaningfully wrong; the actual byte
  buffers the SDK already hands back are simpler and exact.

### Bug found while self-testing: shared `CPropertyAction*` causes a double-free on shutdown

`CreateBiasProperties()`'s original implementation allocated **one**
`CPropertyAction` and passed the same pointer to `CreateIntegerProperty()`
for every bias (and, separately, the six live-stats properties shared one
`CPropertyAction` the same way). This compiled and worked for ordinary
property reads/writes — the crash only showed up at device shutdown, as
an intermittent process crash (observed as the self-test harness exiting
with no Python traceback and lost stdout, consistent with a hard native
crash rather than a normal exception).

Root-caused by reading `MMDevice/Property.h`: `MM::Property`'s destructor
(`~Property() { delete fpAction_; }`) unconditionally deletes its own
action functor. Every property created with the *same* `CPropertyAction*`
therefore deletes that same pointer when it's destroyed — the first one
frees it validly, every subsequent one is a double-free, corrupting the
heap. `micromanager_examples/DemoCamera/DemoCamera.cpp`'s own
`OnTestProperty` loop already does the right thing (`new
CPropertyActionEx(...)` freshly *inside* the loop, once per property) —
confirmed against that reference before fixing. **Fix**: both
`CreateBiasProperties()` and the live-stats property loop in
`Initialize()` now allocate a fresh `new CPropertyAction(...)` per
property, never reusing one across multiple `CreateProperty()` calls.

**Lesson for future goals**: any loop that creates multiple MM properties
must allocate a new action-functor instance per property, full stop —
this is easy to get wrong once (it compiles, links, and passes ordinary
property read/write tests fine) and only manifests as a shutdown-time
crash.

### Bug found while self-testing: background-thread `SetProperty()` racing the main thread

Related to the above but a separate issue: the first working version of
the live-stats properties followed Goal 4's `EBS-RawRecordingStatus`
pattern exactly (`SetProperty()` + `OnPropertyChanged()` from
`UpdateStats()`, running on `ProphEBSStatsThread`'s own thread). This
also produced an intermittent crash. Unlike Goal 4's status string
(updated a handful of times per recording), `ProphEBSStatsThread` ticks
continuously, once per second, for the *entire* time a camera is
connected — heavily overlapping with the self-test's own
`core.setProperty()`/`core.getProperty()` calls happening from the main
thread throughout. `MM::PropertyCollection`'s thread-safety against a
background-thread `SetProperty()` racing a main-thread property access is
not documented or verified anywhere in the MMDevice headers, and Goal 4's
one-shot pattern never exercised this path enough to hit it.

**Fix**: redesigned so `UpdateStats()` never calls `SetProperty()` (the
device's own, apparently-not-thread-safe `PropertyCollection` mutator) —
it only writes to plain `std::atomic<double>` members (`avgDataRateMBps_`
etc.), and a new shared `OnStat()` handler reads the matching atomic in
`BeforeGet`, computed synchronously on whatever thread MMCore is already
calling from (e.g. a Python `getProperty()` call, or the GUI). The six
live-stats properties are created read-only.

**Verified fixed**: after both fixes, `tools/test_prophebs.py` ran
start-to-finish repeatedly against the real connected IMX636 with no
crash, `exit=0`, `SUCCESS` printed.

### Follow-up: stats properties weren't live-updating in the GUI

User reported the six live-stats properties only changed value when some
*other* property was touched, not continuously — the `BeforeGet`-only
design above is correct for on-demand reads (e.g. the self-test's
`getProperty()` calls) but does nothing to make the Device/Property
Browser refresh on its own, since nothing was prompting MMCore to call
`GetProperty()` in the first place. Root cause: `SetProperty()` was
removed (correctly, see the crash above) but so was any call to
`OnPropertyChanged()` — and per the Goal 4 lesson, the GUI reads from
MMCore's `stateCache_`, which is *only* updated by `OnPropertyChanged()`
(or a Core-initiated `setProperty()`), never by `BeforeGet` alone.

**Fix**: added six `OnPropertyChanged(name, value)` calls at the end of
`UpdateStats()`, using the freshly-computed atomic values directly (no
extra `GetProperty()`/`SetProperty()` round-trip needed). This is safe
from the background thread specifically because `OnPropertyChanged()` is
a *different* code path than `SetProperty()` — confirmed by reading
`CoreCallback::OnPropertyChanged()` in `MMCore/CoreCallback.cpp`: it only
touches MMCore's separately-locked `stateCache_` under its own
`std::lock_guard`, never this device's own `PropertyCollection` (the
thing `SetProperty()` mutates, and the actual source of the earlier
crash). So the final design keeps both fixes together: atomics +
`BeforeGet` for correctness/thread-safety of the *value*, plus
`OnPropertyChanged()` for pushing live updates to the GUI, without ever
calling this device's own `SetProperty()` from a background thread.

**Verified**: re-ran `tools/test_prophebs.py` after this change — still
`exit=0`, `SUCCESS`, no crash. The live-update behavior itself (GUI
refreshing every ~1s without touching another property) can only be
confirmed by the user in the real Device/Property Browser, not by this
harness (same caveat as other GUI-only behaviors throughout this log).

### Bug found while self-testing: anti-flicker frequency band silently rejected

`CreateAntiFlickerProperties()` originally didn't call `SetPropertyLimits()`
on `EBS-AntiFlicker-LowFreq`/`HighFreq`, and `set_frequency_band()`'s
`bool` return value wasn't checked. Setting `LowFreq=45` on this sensor
(whose actual minimum supported frequency is 50 Hz) was silently rejected
by the hardware; the property kept reading back its old value with no
error anywhere. **Fix**: added `SetPropertyLimits()` from
`get_min/max_supported_frequency()` (so MM itself rejects an out-of-range
value before it reaches the hardware) and a `LogMessage()` warning if
`set_frequency_band()` ever returns `false` despite passing MM's own
limits. `tools/test_prophebs.py` now derives its probe values from the
property's own reported limits instead of a hardcoded guess.

### Verified locally

- **Build**: `MSBuild ProphEBS.sln /p:Configuration=Release /p:Platform=x64`
  succeeds with 0 warnings/errors (VS2022 Preview toolset, same as Goals
  1-4). Note: on this dev machine, building via the **PowerShell tool**
  was required — the identical command via the Bash tool's `cmd /c '...'`
  reliably produced truncated/no output even though MSBuild output
  streamed correctly through PowerShell; see
  `docs/BUILD_AND_USAGE.md`/build-environment notes.
- **Real hardware, via `tools/test_prophebs.py`** (extended with Goal 5
  checks): `EBS-BiasRangeCheckBypass` correctly flagged pre-init;
  `EBS-Generation`/`EBS-DataEncodingFormat` read back `"4.2"`/`"EVT3"`;
  all 6 real (hardware-fetched) bias properties round-tripped a mid-range
  value within their reported limits; ERC/event-trail-filter/anti-flicker
  enable+value round-trips all passed; live-stats properties settled to
  plausible non-zero values after ~3.5 s of streaming
  (`AvgDataRate-MBps ≈ 24`, `AvgEventRate-MEvps ≈ 10`,
  `Temperature-C = 22`, `PixelDeadTime-us = 75`) — `Illumination-lux`
  stayed at its `0.0` default since `I_Monitoring::get_illumination()`
  throws on this particular sensor (logged as `[HAL][ERROR] Failed to get
  illumination`, harmless, handled by the per-metric try/catch). Full
  script (`SUCCESS`, exit 0) including all Goal 1-4 checks passed
  unchanged, confirming no regression.
- **Not yet verified**: a real walkthrough in the MicroManager Studio GUI
  — per the usual handoff pattern, this is pending the user, specifically
  checking that `EBS-BiasRangeCheckBypass` is editable before "Add
  Device" and greyed out afterward in the Hardware Config Wizard/Device
  Property Browser, and that bias property sliders respect their fetched
  min/max ranges visually.

### Open questions / TODO for later goals

- `EBS-AvgERCDropRate-KEvps` is explicitly an estimate
  (`target − measured`), not a hardware-reported value — there is no HAL
  API for the real dropped-event count. If Prophesee ever exposes one,
  this should switch to it.
- `I_Monitoring::get_illumination()` throwing on the connected IMX636
  (while `get_temperature()`/`get_pixel_dead_time()` work fine) is
  unexplained — possibly a HAL/firmware limitation specific to this
  sensor or SDK version, not investigated further since the per-metric
  try/catch already handles it gracefully (property stays at its safe
  0.0 default rather than blocking the other metrics).
- Goal 6 ("custom view methods") is next per the roadmap — configurable
  integration window, offset, and common filters for the live view.
