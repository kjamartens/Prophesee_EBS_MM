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

## Goal 6 — Custom view methods

### Status: confirmed working by user in the MicroManager GUI — tagged v0.6

### Requirement clarification (asked user before implementing)

The roadmap wording ("configurable integration window, offset, common
filters") left several things ambiguous, so these were clarified with the
user via AskUserQuestion before writing any code:

- **Integration time should be MM's own standard `Exposure` property**, not
  a new bespoke `EBS-*` property — matching how every other camera
  adapter's exposure control works, and letting existing MDA/scripting
  paths that already set exposure "just work" for the EBS's integration
  window too.
- **The offset is a signed polarity ("net ON−OFF") baseline**: `pixel =
  clamp(offset + rawValue * scale, 0, 255)`, default `offset = 10`, and
  the user explicitly asked for `scale` to default to **1** (not the old
  Goal 3 hardcoded 32) once it became a real polarity-aware view rather
  than a merged count.
- **Both** offered extras were wanted: a software activity-noise filter
  (Metavision's `ActivityNoiseFilterAlgorithm`) and a view-mode selector
  (Merged/OnOnly/OffOnly/NetSigned) rather than only the signed view.

### What was built

- **`Exposure` now drives the event-integration window.** Goals 1-5 created
  `MM::g_Keyword_Exposure` with no `CPropertyAction` at all (`ProphEBS.cpp`
  Initialize()), so changing it in the GUI never did anything —
  `exposure_ms_` was a dead local cache. Now it has a real handler
  (`OnExposure()`) that stores `AfterSet`'s value into a new
  `std::atomic<double> integrationTimeMs_`; `SetPropertyLimits` clamps it
  to 1-100000 ms. `ProphEBSFrameBuilderThread::svc()` no longer
  snapshots an interval once at `Start()` — it now reads
  `camera_->integrationTimeMs_.load()` fresh every loop iteration (`camera_`
  is already a friend of `CProphEBSCamera`), so changing `Exposure` anywhere
  (Device/Property Browser, MDA dialog, `core.setExposure()`) retunes the
  integration window on the very next frame, no restart needed.
  `g_EventIntegrationMs` is gone; `exposure_ms_` is gone (the property/atomic
  pair is now the single source of truth, read through
  `GetExposure()`/`SetExposure()` exactly as before).
- **Per-polarity accumulation.** `OnEventsCD()`'s single merged
  `eventCounts_` became two counters, `onCounts_`/`offCounts_`
  (`std::vector<int32_t>`, same `eventCountsLock_`), incremented by
  `ev->p != 0` (ON, per Metavision's `Event2d::p` convention: 1 = positive
  contrast change) vs. OFF.
- **`EBS-ViewMode`** (string, allowed values `Merged`/`OnOnly`/`OffOnly`/
  `NetSigned`, **default `NetSigned`**) picks which raw per-pixel quantity
  `BuildAndSwapFrame()` renders: `on+off`, `on`, `off`, or `on-off`
  respectively. `NetSigned` is the default specifically because it's the
  literal ask ("show offset +- found events"); `Merged` is kept selectable
  for the old Goal 3/5 look.
- **`EBS-ViewOffset`** (integer, default **10**, limits 0-255) and
  **`EBS-ViewScale`** (float, default **1.0**, limits 0.01-1000) — plain
  `std::atomic`-backed settable properties. `BuildAndSwapFrame()` computes
  `pixel = clamp(offset + raw * scale, 0, 255)` per pixel using whichever
  `raw` `EBS-ViewMode` selected.
- **Software activity-noise filter**: `EBS-ActivityFilter-Enabled` (Off/On,
  default Off) and `EBS-ActivityFilter-Threshold-us` (integer, default
  10,000 µs, limits 100-10,000,000) via
  `Metavision::ActivityNoiseFilterAlgorithm<>` (header-only template class,
  confirmed present under the installed SDK's
  `include/metavision/sdk/cv/algorithms/activity_noise_filter_algorithm.h`
  — no new `.vcxproj` library dependency needed, the existing
  `AdditionalIncludeDirectories` already covers `sdk/cv` since it's just a
  subfolder of the SDK's `include/` root already on the include path).
  Constructed in `StartEventStreaming()` once sensor geometry is known
  (`std::unique_ptr<Metavision::ActivityNoiseFilterAlgorithm<>>
  activityFilter_`), destroyed in `StopEventStreaming()`. When enabled,
  `OnEventsCD()` runs each incoming CD event batch through
  `activityFilter_->process_events()` into a reusable scratch buffer and
  accumulates ON/OFF counts from the filtered output instead of the raw
  batch; when disabled, the original direct-accumulation fast path is
  unchanged. A new `activityFilterLock_` guards both `process_events()`
  (called from `OnEventsCD()`, the Metavision SDK's callback thread) and
  `set_threshold()`/reconstruction (called from the property handlers,
  MMCore's thread) — the algorithm's internal threshold/state isn't
  documented as safe against a concurrent setter, same caution as the
  existing per-batch `eventCountsLock_`.

### Design decisions (and why)

- **`NetSigned` as the default view mode, not `Merged`.** This does change
  the live view's default appearance from Goals 3/5 (a real, intentional
  behavior change for this goal, not a regression) — it's what the user
  actually described wanting ("integrate over whatever time is wanted, and
  show offset +- found events"). `Merged` stays one property-set away for
  anyone who wants the old look back.
- **`ProphEBSFrameBuilderThread::svc()` re-reads `integrationTimeMs_` every
  loop iteration instead of snapshotting it once at `Start()`.** This is
  the same "plain member/atomic + live setter, re-read each frame" shape the
  Metavision SDK's own `OnDemandFrameGenerationAlgorithm`/
  `PeriodicFrameGenerationAlgorithm` classes use for their accumulation-time
  controls (found while researching Goal 6 options) — adopted here without
  pulling in either of those classes themselves, since a full swap to one of
  them would be a much larger rewrite of the existing hand-rolled pipeline
  than this goal's scope justified.
- **`onCounts_`/`offCounts_` are `int32_t`, not `uint32_t` like the old
  `eventCounts_`.** `NetSigned` mode computes `on - off`, which needs a
  signed result; keeping both counters signed (rather than casting only at
  the point of subtraction) avoids any signed/unsigned mismatch surprises.
- **View-mode/offset/scale properties use plain atomics with no
  `OnPropertyChanged()` push.** Unlike the Goal 5 live-stats properties
  (written by a background thread the GUI wouldn't otherwise know to
  re-poll), these are only ever written via the property system's own
  `AfterSet` (a normal user-initiated `setProperty()` call) — MMCore already
  updates its own `stateCache_` for that path, so no extra push is needed;
  `BuildAndSwapFrame()` (the frame-builder thread) is the sole reader.
- **Activity-filter lock held for the whole event batch, not per-event.**
  Same per-batch (not per-event) locking granularity as the pre-existing
  `eventCountsLock_`, to keep lock overhead off the sensor's actual
  per-event rate.

### Verified locally

- **Build**: `MSBuild ProphEBS.sln /p:Configuration=Release /p:Platform=x64`
  succeeded with 0 warnings/errors on the first attempt (VS2022 Preview
  toolset, PowerShell tool, same as every prior goal) — including the new
  `<metavision/sdk/cv/algorithms/activity_noise_filter_algorithm.h>`
  include with no `.vcxproj` changes needed.
- **Real hardware, via `tools/test_prophebs.py`** (extended with Goal 6
  checks): all Goal 1-5 checks still passed unchanged (no regression) on
  the connected IMX636. New checks: `Exposure` round-tripped to 50 ms
  within its 1-100,000 ms limits; `EBS-ViewMode` round-tripped through all
  four allowed values; `EBS-ViewOffset`/`EBS-ViewScale` round-tripped a
  mid-range/explicit value; `EBS-ActivityFilter-Enabled`/`-Threshold-us`
  round-tripped; and a live-reconfiguration check (change `Exposure`,
  `EBS-ViewMode`, and turn the activity filter on, all while nothing is
  restarted) snapped successfully before and after with unchanged image
  shape/dtype `(720, 1280) uint8`. Full script printed `SUCCESS`, exit 0.
- **Confirmed by the user in the MicroManager Studio GUI**: the live view
  visibly responds to `EBS-ViewMode`/`EBS-ViewOffset`/`EBS-ViewScale`
  changes, sub-millisecond `Exposure` no longer shows a growing display
  lag, and the idle-timeout reset to the `EBS-ViewOffset` baseline behaves
  as expected in a dark/still scene.

### Follow-up: sub-millisecond integration windows

User reported no visible difference between a 1 ms and a 0.0001 ms
`Exposure` and asked to investigate. Root-caused to **three stacked
floors**, not one:

1. `Initialize()`'s `SetPropertyLimits(MM::g_Keyword_Exposure, 1.0,
   100000.0)` — MMCore's own `FloatProperty::Set()`
   (`third_party/mmCoreAndDevices/MMDevice/Property.cpp:156-166`) silently
   rejects (`return false`, i.e. `setProperty()` just fails) anything below
   1.0 — `0.0001` never reached the device at all, the property just stayed
   at its previous value.
2. `OnExposure()`'s `AfterSet` had its own hardcoded `std::max(1.0, value)`
   — a second, redundant floor, even if the property limit weren't there.
3. `ProphEBSFrameBuilderThread::svc()` passed the interval to
   `CDeviceUtils::SleepMs(long ms)` — confirmed via
   `MMDevice/DeviceUtils.h:37` that this takes **whole milliseconds only**.
   Even with both floors above removed, this wall-clock-Sleep()-based
   design architecturally cannot represent a sub-millisecond window.

**Design chosen** (confirmed with the user via AskUserQuestion, including a
follow-up question about idle-scene behavior): decouple "how long is one
integration window" from "how often is a frame published," and drive the
window off real sensor timestamps instead of wall-clock sleep:

- **Integration window (`Exposure`) is now event-timestamp-driven.**
  `Metavision::EventCD::t` (confirmed `typedef long long timestamp;` in
  `metavision/sdk/base/utils/timestamp.h`) is microsecond-resolution.
  `OnEventsCD()` now checks, per event, whether `ev->t - windowStartT_ >=
  exposureUs` (computed once per batch, not per event) and calls a new
  `CloseCurrentWindowLocked()` when a window's time is up — this is what
  makes genuinely sub-millisecond (down to the 1 microsecond floor, the
  finest `EventCD::t` can represent) windows possible, with no Sleep() in
  the loop at all.
- **The naive version of window-close doesn't scale, so it isn't what got
  built.** Clearing the whole `onCounts_`/`offCounts_` arrays (as Goal 6
  did every ~100 ms) is fine at that cadence; it is not fine when a window
  can close hundreds of thousands of times a second for a 720×1280 sensor.
  Fix: a new `touchedIndices_` list records exactly which pixel indices
  were written to since the window opened; `CloseCurrentWindowLocked()`
  only resets those — cost proportional to real event activity in the
  window, not sensor resolution.
- **New `EBS-DisplayRefreshMs` property** (default 1.0 ms, limits
  1-10000) decouples how often `ProphEBSFrameBuilderThread` actually
  publishes a frame (`BuildAndSwapFrame()`) from the integration window
  length — no point publishing faster than the display can show anyway.
  `BuildAndSwapFrame()` no longer resets the accumulators itself (that's
  now exclusively `CloseCurrentWindowLocked()`'s job) — it takes a locked
  **copy** of `onCounts_`/`offCounts_` and renders from that.
- **Idle-scene safety net**: since window-close is now driven by incoming
  events, a sensor that goes fully quiet would otherwise never close its
  last window, freezing the display on the last-active frame instead of
  resetting to the `EBS-ViewOffset` baseline. Asked the user whether that
  freeze was acceptable; they preferred the old "goes quiet → resets to
  baseline" behavior, with the actual timeout value not needing to be a
  property ("100ms or so" is fine as a constant). Implemented as a fixed
  `g_IdleWindowTimeoutMs = 100.0` — `BuildAndSwapFrame()` checks
  `lastWindowCloseWallTime_` (a `std::chrono::steady_clock::time_point`,
  updated on every close, event-driven or forced) before rendering and
  force-closes a stale window if 100 ms of wall-clock time has passed with
  no event-driven close.
- **`Exposure`'s floor is 0.001 ms (1 microsecond), not 0**, matching
  `EventCD::t`'s own resolution — going finer couldn't mean anything real
  anyway. Confirmed `MM::FloatProperty` defaults to 4 decimal places
  (`Property.h:371`), so 0.001 is exactly representable, not silently
  truncated to a coarser value.

### Verified locally (follow-up)

- **Build**: 0 warnings/errors, first attempt, no `.vcxproj` changes needed
  (`<chrono>` is standard library).
- **Real hardware, via `tools/test_prophebs.py`** (extended further): all
  Goal 1-6 checks still passed (no regression). New checks: `Exposure`'s
  lower limit confirmed at 0.001; round-tripped to 0.05 ms; new
  `EBS-DisplayRefreshMs` round-tripped within its 1-10000 ms limits; and —
  the actual stress case this design has to survive — three 300 ms bursts
  against the connected IMX636 (streaming at its usual ~10 MEv/s) with
  `Exposure` set to 0.1 ms, 0.01 ms, and the absolute floor **0.001 ms (1
  microsecond)**, each with `EBS-DisplayRefreshMs = 1`. All three
  completed without error, hang, or crash, snapping correctly-shaped
  `(720, 1280) uint8` frames throughout — real validation that closing
  windows via event timestamps (potentially extremely often) and resetting
  only touched pixels holds up under genuine high-event-rate hardware, not
  just synthetically.
- **Confirmed by the user**: the idle-timeout reset (dark/still scene
  falling back to the `EBS-ViewOffset` baseline after ~100 ms of no
  events) and the sub-millisecond `Exposure` range in a real GUI
  walkthrough both behave as expected.

### Bug found and fixed: Live view backlog grew without bound once Exposure could go sub-ms

User reported two symptoms after the sub-millisecond follow-up above: (a)
setting `Exposure` under 1 ms still didn't visibly change the Live feed,
and (b) the Live view developed a growing display delay, eventually
exceeding 500 ms.

**Root cause**: `ProphEBSSequenceThread::svc()` (used to push frames into
MMCore during MicroManager's Live view) has, since Goal 1, used its
caller-supplied `interval_ms` argument directly as its push cadence. That
argument traces back to `CMMCore::startContinuousSequenceAcquisition(double
unused)` — the parameter is literally named `unused` in MMCore's own public
header (`third_party/mmCoreAndDevices/MMCore/MMCore.h:433`), and
`MMCore.cpp:3238-3241` has a comment reading *"The MM::Camera contract now
says new adapters must ignore it."* This adapter never did. As long as
whatever value happened to be passed through stayed near ~100 ms, the bug
was invisible. Once `Exposure` could go sub-millisecond (the follow-up
above), and MicroManager's Live view plausibly passes the camera's own
current exposure as this "unused" value (a common camera-adapter
convention), `ProphEBSSequenceThread` started trying to push frames into
MMCore's circular buffer up to ~1000 times/sec — far faster than the GUI
can actually consume them. Since the display presumably drains that buffer
roughly in order rather than always jumping to the newest entry, the
backlog (and thus the visible delay between "what the sensor sees now" and
"what's on screen") grew continuously, exactly matching both reported
symptoms — the sub-ms setting "not working" was really the display
showing increasingly stale, backlogged frames, not evidence that the
integration window itself was broken.

**Why self-testing didn't catch this**: the Goal 6 follow-up's sub-ms
stress checks only used `snapImage()` (single-shot), never
`startContinuousSequenceAcquisition()` — the combination of "sub-ms
Exposure" and "Live view running" was never exercised together before this
bug was reported.

**Fix (v1)**: `ProphEBSSequenceThread::svc()` now checks whether the
sequence is unbounded (`numImages_ == LONG_MAX`, MM's own signal for "this
is Live view, not a finite MDA" — already used elsewhere in this adapter,
e.g. Goal 4's recording-vs-Live distinction) — if so, it ignores the
caller-supplied `intervalMs_` entirely and always sleeps a fixed 100 ms
instead. A finite (MDA) sequence still honors its caller-supplied
`intervalMs_` exactly as before, since that one is a real, user-configured
value (e.g. "10 frames at 100 ms") with no equivalent "ignore this"
contract.

**Verified (v1)**: `tools/test_prophebs.py` (extended with a new check that
explicitly reproduces the failure mode — sets `Exposure` to 0.01 ms, then
calls `startContinuousSequenceAcquisition(0.01)` to mimic a Live view that
paces itself off the camera's exposure) against the connected IMX636.
Before the fix this would have buffered on the order of 1000 frames/sec;
after the fix it buffered exactly 10 in 1 second (matching the fixed
100 ms cadence). Incidentally, the pre-existing Goal 3
continuous-acquisition check's buffered count also dropped from 34-39 (in a
500 ms window, using the test harness's own
`startContinuousSequenceAcquisition(0)` call — `0` being another value that
should have been ignored per MMCore's contract but previously wasn't) down
to a consistent 5 — meaning Live view's push cadence had actually been
running faster than intended since Goal 1, just not fast enough to cause a
*visible* problem until Exposure could go sub-millisecond. Full suite still
passed.

**Follow-up (v2, same session)**: user pushed back on the fixed 100 ms —
correctly pointed out MM's Live view does not need a fixed cadence, and
asked for Live view to just follow `Exposure` directly, bounded below by a
configurable floor (e.g. 1-5 ms) for when `Exposure` itself is sub-ms.
Replaced the fixed constant with:

- New **`EBS-LiveViewMinIntervalMs`** property (default 5 ms, limits
  1-1000), the floor.
- `ProphEBSSequenceThread::svc()`'s unbounded-sequence case now computes
  `max(integrationTimeMs_, liveViewMinIntervalMs_)` (both read directly —
  `camera_` is already a friend) instead of a fixed value — Live view
  naturally follows `Exposure` like any other camera whenever it's above
  the floor, and is only clamped once `Exposure` drops below it.

**Verified (v2)**: extended the test further — round-tripped
`EBS-LiveViewMinIntervalMs` (confirmed default 5 ms); re-ran the sub-ms
reproduction (`Exposure=0.01`) and got 74 buffered frames/sec (bounded by
the 5 ms floor, nowhere near the old ~1000/sec runaway); then set
`Exposure=50` (comfortably above the floor) and got 17 buffered frames/sec
— confirming Live view actually tracks `Exposure` once it's above the floor,
not just always clamped to some fixed rate. Full suite still passed
(`SUCCESS`, exit 0), no regression.

**Not yet verified**: the user's real GUI Live view, to confirm the display
delay no longer grows and that sub-ms `Exposure` changes are now visibly
reflected promptly.

### Open questions / TODO for later goals

- The idle-timeout path (`g_IdleWindowTimeoutMs`) is currently only
  exercised implicitly (never triggered during self-testing, since the test
  environment always has some ambient event activity) — worth deliberately
  testing in a dark/still scene at some point, either by the user in the
  GUI or with a lens cap in a future self-test run.
- Goal 7 ("pixel-by-pixel differences") is next per the roadmap — per-pixel
  masking (blocked-pixel list) and a sensor ROI, building on the geometry
  and per-pixel accumulation infrastructure already in place from Goals 3/6.

### Follow-up: Live view falling behind under sustained high event rates ("backlog flush")

User reported (session start, before any new code): under many streamed
events, Live view stops catching up — a display lag that grows during a
burst of activity, then slowly recovers once activity calms, rather than
staying flat or freezing outright. Asked the user two clarifying questions
before touching code: (1) does the lag recover on its own or stay stuck —
**recovers, slowly**, which is the signature of a real FIFO backlog
draining, not a permanent stall; (2) lead with Metavision's own hardware
ERC (event-rate-control, already exposed as `EBS-ERC-*` since Goal 5) as a
proactive rate cap, or detect-and-flush after the fact — user chose
**detect-and-flush only** (don't cap/drop events at the sensor; catch the
display back up to real time instead).

**Root cause reasoning**: the device adapter has no visibility into
MMCore's circular-buffer occupancy (`MM::Core` — confirmed by reading
`MMDevice/MMDevice.h` — only exposes a one-way `InsertImage()` push, no
query), so backlog can't be measured or flushed from that side. Metavision's
own real-time viewer sample
(`share/metavision/sdk/core/python_samples/metavision_simple_viewer`) uses
`window.show_async(cd_frame)` — a "latest wins" display slot, not a queue —
which this adapter's own `frontImg_`/`backImg_` double-buffer already
mirrors (see Goal 3). The actual queue that can back up is internal to the
Metavision SDK's own decode/callback pipeline: `OnEventsCD()`'s per-event
work (bias/HPF-driven touch tracking, window-close resets) scales with how
much event backlog has piled up, which is itself extra CPU spent on stale
data — a classic slow-recovery shape once a burst pushes the callback
thread behind real (wall-clock) time.

**Implementation** — backlog detection directly in `OnEventsCD()`
(`ProphEBS.cpp`), no new thread needed:

- An anchor pair, `streamWallStart_` (`std::chrono::steady_clock::
  time_point`) / `streamSensorStart_` (`Metavision::timestamp`), is
  established on the first event of each `StartEventStreaming()` session
  (reset to a `-1` sentinel there, armed on the next `OnEventsCD()` call).
  Every batch computes `lagMs = wallElapsedSinceAnchor -
  sensorElapsedSinceAnchor` using the batch's own newest raw event
  timestamp (`(end-1)->t`, before any activity-filter subsetting) — a
  growing positive value means the callback is falling behind real time.
- New `EBS-BacklogFlushThresholdMs` property (float, default 250 ms, limits
  5–60000) — once `lagMs` reaches it, `OnEventsCD()` takes a **flush** fast
  path instead of the normal per-event loop: `onCounts_`/`offCounts_` are
  wiped with `std::fill` (fixed O(sensor pixels) cost, independent of how
  large the actual backlog is), `touchedIndices_` cleared,
  `windowStartT_`/the anchor pair re-set to "now" (this batch's newest
  event), and the whole batch's activity-filter/per-event work is skipped
  entirely. This is the actual mechanism that lets the callback thread
  drain the Metavision SDK's internal backlog faster than it refills —
  spending less CPU per batch once behind, not more.
- Two new read-only diagnostics, `EBS-CallbackLagMs` and
  `EBS-BacklogFlushCount`, pushed on the existing Goal 5 stats cadence
  (`UpdateStats()`, every `g_StatsIntervalMs` via `OnPropertyChanged()`) —
  so the user can watch lag and flush count live in the Device/Property
  Browser during a real GUI session, not just infer the fix worked.
- Made a design call to lock `eventCountsLock_` before `activityFilterLock_`
  in `OnEventsCD()` (previously the reverse), so the lag check + fast flush
  path can run under just `eventCountsLock_` without touching
  `activityFilterLock_` at all when flushing. Confirmed safe: grepped every
  other use of both locks — nothing else in the file ever holds both
  simultaneously, so nesting order is only self-consistent within
  `OnEventsCD()` itself, no deadlock risk.

### Verified locally (backlog flush)

- **Build**: `MSBuild ProphEBS.sln /p:Configuration=Release /p:Platform=x64`
  — one error on the first attempt (`CDeviceUtils::ConvertToString()`
  returns `const char*`, not `std::string` — confirmed in
  `MMDevice/DeviceUtils.h`; `"a" + ConvertToString(x)` was pointer + pointer
  arithmetic, not string concatenation), fixed by wrapping in
  `std::string(...)`. 0 warnings/errors after.
- **Real hardware, via `tools/test_prophebs.py`** (extended): all Goal 1-6
  checks still passed (no regression). New checks:
  `EBS-BacklogFlushThresholdMs` round-tripped within its 5-60000 ms limits
  (default confirmed 250); then, with the threshold forced down to its
  5 ms floor for 1 second of real streaming (~10 MEv/s ambient, same as
  every other real-hardware check in this session), `EBS-BacklogFlushCount`
  rose from 0 to 2 and `EBS-CallbackLagMs` moved from ~1.9 ms to -12.3 ms
  (negative == caught up/ahead immediately after a flush re-anchors to
  "now", exactly the intended effect) — real, observable evidence the flush
  path triggers and actually resets the lag, not just that the properties
  exist. A `snapImage()` immediately after confirmed the adapter is still
  in a normal working state post-flush (unchanged `(720, 1280) uint8`).
- **Not yet verified**: the real-world scenario that prompted this — a
  genuinely heavy, sustained motion/light burst in the MicroManager GUI's
  Live view, watched for whether the display lag now stays flat (or at
  least recovers quickly) instead of growing unboundedly during the burst.
  The self-test only proves the flush mechanism triggers and resets
  `EBS-CallbackLagMs`/`EBS-BacklogFlushCount` correctly under real streaming
  load with an artificially low threshold — it can't observe the GUI's own
  visual display lag, only the callback's internal timing. Pending the
  user's GUI walkthrough, ideally comparing Live view responsiveness before
  and after this change during a deliberately busy scene (e.g. waving a
  hand close to the sensor), and confirming `EBS-BacklogFlushThresholdMs`'s
  default (250 ms) is a reasonable balance — lower flushes more eagerly
  (catches up faster but discards more stale detail), higher tolerates more
  transient lag before discarding anything.

### Bug found immediately by the user, same session: negative lag readings and a 3.4s-late first flush

User tried the above in the real GUI and reported it looked broken:
`EBS-CallbackLagMs` read a nonsensical **-3210** during a static/quiet
scene right after an active one, and the flush only actually seemed to
kick in around **3460 ms** of real display delay, not anywhere near the
250 ms default threshold. Both symptoms trace back to two real bugs in the
first version of this mechanism (not requirements gaps):

1. **The flush check only ran once, at the very top of `OnEventsCD()`.**
   Metavision can (and evidently did) deliver a single callback batch
   spanning a large sensor-time range in one call. If that one batch was
   itself slow to process — the existing per-event `touchedIndices_`
   growth and window-close resets, now working through unusually dense
   data — wall-clock time kept advancing throughout it, but nothing
   re-checked the lag until the *next* callback invocation. A single slow
   batch could therefore run the real delay arbitrarily far past the
   configured threshold before anything flushed, which is exactly the
   "flush only at 3460 ms, not 250 ms" symptom.
2. **The anchor was only ever reset on an actual flush.** A batch that
   computed zero or negative lag (genuinely caught up, or even running
   *ahead* because one callback delivered a wide sensor-time span from
   already-decoded backlog in a burst) left `streamWallStart_`/
   `streamSensorStart_` untouched, so the lag reading was free to swing
   deeply negative. Once negative, a later *genuine* burst had to climb
   back up through that entire deficit before crossing
   `EBS-BacklogFlushThresholdMs` again — silently defeating the threshold
   for however long that climb took. This is the direct explanation for
   both the -3210 reading itself and, compounding with bug 1, part of why
   the next flush was so late.

**Fix**: refactored the lag check into a reusable `checkLag()` lambda
inside `OnEventsCD()` (`ProphEBS.cpp`) and a new `FlushBacklogLocked()`
helper (factored out of the inline flush code):

- `checkLag()` now **re-anchors `streamWallStart_`/`streamSensorStart_` to
  "now" any time the computed lag is `<= 0`**, not only on an explicit
  flush — clamping the reported value to 0 in that case. This makes the
  metric track "how far behind the most recent *known caught-up* point,"
  with no way to accumulate a negative credit a later burst would have to
  climb out of first.
- The per-event loop in `OnEventsCD()` now also calls `checkLag()` every
  `g_LagCheckEventInterval` (8192) events, not just once at batch entry,
  and can call `FlushBacklogLocked()` mid-batch. This bounds a single
  large/slow batch's worst-case un-flushed delay to roughly one recheck
  interval's own processing time instead of the whole batch, regardless of
  how large that batch turns out to be.
- Extensively documented both incidents directly on the
  `streamWallStart_`/`streamSensorStart_` member comment in `ProphEBS.h`
  (not just here), since this is exactly the kind of "the naive version
  doesn't scale/isn't correct" lesson worth having next to the state itself
  for whoever touches this next.

**Verified**: `tools/test_prophebs.py`'s existing backlog-flush check
(forces `EBS-BacklogFlushThresholdMs` to its 5 ms floor for 1 second of
real streaming) needed no changes and still passed — `EBS-CallbackLagMs`
went `0.42 -> 3.43` (both positive/small, never negative) and
`EBS-BacklogFlushCount` rose by **8** in that second (versus 2 before this
fix, since flushes are now caught promptly at the 8192-event granularity
instead of only occasionally landing on an already-over-threshold batch
boundary) — consistent with the fix working as intended. Full suite
(`SUCCESS`, exit 0) passed with no regression across all Goal 1-6 checks.
Build: 0 warnings/errors, first attempt, no `.vcxproj` changes needed.

**Not yet verified**: the same real GUI walkthrough as above (still
pending) — this time specifically re-checking that `EBS-CallbackLagMs`
stays non-negative and the flush now visibly kicks in close to the
configured 250 ms default rather than several seconds late.

## Goal 7 — Pixel-by-pixel differences (hot-pixel masking + sensor ROI)

### Status: DONE — confirmed working by user — tagged v0.7

### Requirement clarification (asked user before implementing)

The roadmap wording added a new detail since Goal 6: hot-pixel blocking
should ideally be automated ("finding hot pixels from within MM and
setting these in there somehow"), not just a manually-typed index list.
Three design points were clarified with the user via `AskUserQuestion`
before writing any code:

- **ROI must be a real hardware crop** (Metavision's `I_ROI` HAL facility),
  not a display-level crop — because the roadmap explicitly asks for "a
  ROI of the EBS which is recorded," and only a hardware ROI actually
  reduces what lands in the `.raw` file.
- **Hot-pixel detection is on-demand**, triggered by a property
  (`EBS-DetectHotPixelsNow`), not a continuous background process.
- **Blocked pixels are a single string property**, and — the user's own
  addition — must be **global** (absolute sensor coordinates) so the list
  stays meaningful as the active ROI changes; a later follow-up question
  settled that re-running calibration **merges** (union) into the existing
  list rather than replacing it, so a manually-added pixel is never
  silently un-masked by a later calibration run.

### What was built

- **Hardware ROI** (`ProphEBS.h`/`ProphEBS.cpp`): `SetROI()`/`GetROI()`/
  `ClearROI()` — bookkeeping-only stubs since Goal 1 — now drive
  `Metavision::I_ROI` for real: `SetROI()` clamps the requested rectangle
  to the sensor bounds (the one deliberate deviation from
  `DemoCamera`'s no-validation convention, since an out-of-bounds window
  handed to `set_window()` is a real hardware call), then calls
  `set_mode(ROI)`/`set_window()`/`enable(true)`; `ClearROI()` calls
  `enable(false)` (mirroring the official
  `metavision_active_pixel_detection.cpp` sample's own `clearROI()`
  helper) rather than re-setting a full-frame window. Both always return
  `DEVICE_OK` and log-not-fail on any HAL exception, per this project's
  established convention. Confirmed via the `metavision_viewer.cpp`
  sample that `I_ROI`'s window can be changed live, with no
  `cam_.stop()`/restart cycle needed.
- **Coordinate strategy**: `onCounts_`/`offCounts_`/`touchedIndices_`
  (Goal 6's per-pixel accumulators) stay **full-sensor-sized and indexed
  in absolute sensor coordinates always**, regardless of the active ROI —
  `Metavision::I_ROI` only discards events outside its window, it never
  remaps surviving events' coordinates (confirmed via the SDK
  headers/samples), so `OnEventsCD()` needed zero changes. Only the
  *displayed* frame (`imgBufferA_`/`imgBufferB_`) shrinks to the ROI —
  `BuildAndSwapFrame()`'s render loop now iterates `roiXSize_ x
  roiYSize_`, reading `onCounts_[(roiY_+j)*sensorWidth_ + (roiX_+i)]`
  instead of a flat index. This also satisfies the "blocked pixels are
  global" requirement for free — there's no ROI-relative coordinate
  space anywhere to be inconsistent with.
- **`ApplyRoiToBuffers()`** (new private helper): resizes
  `imgBufferA_`/`imgBufferB_` to the current ROI under `frontImgLock_`,
  and — with no camera connected — re-renders the cropped checkerboard
  test pattern directly into `frontImg_` (the only buffer actually read in
  that path), so the ROI contract is visibly testable without hardware.
  Called from `SetROI()`/`ClearROI()` and once from
  `StartEventStreaming()` (replacing its old unconditional full-size
  `Resize()` call).
- **`roiX_`/`roiY_`/`roiXSize_`/`roiYSize_` are now `std::atomic<unsigned>`**
  (previously plain `unsigned`) — `BuildAndSwapFrame()` reads them every
  frame on `ProphEBSFrameBuilderThread`'s own thread while
  `SetROI()`/`ClearROI()` write them from whatever thread MMCore calls the
  ROI system from; same cross-thread read/write shape Goal 6 already
  solved with atomics for `viewMode_`/`viewOffset_`/`viewScale_`.
- **`EBS-BlockedPixels`** (string, read/write, default empty): a single
  Property-Browser-editable list of absolute sensor-coordinate pixel
  pairs, applied to hardware via `Metavision::I_RoiPixelMask`
  (`reset_pixels()` -> `set_pixel()` per entry -> `apply_pixels()`).
  `OnBlockedPixels()`'s `AfterSet` rejects the **whole string** atomically
  on any malformed token or out-of-range coordinate (logging the specific
  bad token, restoring the previous valid value, returning
  `DEVICE_INVALID_PROPERTY_VALUE`) — the one deliberate exception to this
  project's usual "never fail" convention, since a malformed hand-typed
  string is a real user input error, not a recoverable hardware condition.
- **On-demand hot-pixel calibration** (`EBS-DetectHotPixelsNow`, allowed
  values `Idle`/`Run`; `EBS-HotPixelCalibDurationMs`, default 200 ms;
  `EBS-HotPixelStddevK`, default 3.0, matching the official sample's own
  default multiplier; `EBS-HotPixelCalibStatus`, a plain settable status
  string mirroring the `EBS-RawRecordingStatus` pattern from Goal 4).
  Setting the trigger to `"Run"` runs `OnDetectHotPixelsNow()`
  **synchronously on the calling thread** (no precedent in this codebase
  for a property handler spawning a throwaway thread, and the window is
  short/user-configured): temporarily disables any active hardware ROI
  (mirroring the official sample's own pre-scan `clearROI()`) so hot
  pixels outside the current view can still be found, registers a
  temporary CD callback accumulating a full-sensor-sized event count
  independent of `onCounts_`/`offCounts_`, sleeps for
  `EBS-HotPixelCalibDurationMs`, removes the callback, restores the ROI
  window, computes `threshold = mean + k*stddev` via a hand-rolled
  single-pass sum/sumSq loop (no OpenCV linked into this project, unlike
  the official sample's `cv::meanStdDev`), and merges (`std::set` union)
  every pixel above threshold into `blockedPixels_` before pushing to
  hardware and updating `EBS-BlockedPixels`/`EBS-HotPixelCalibStatus` via
  the standard `SetProperty()`+`OnPropertyChanged()` pairing.

### Design decisions (and why)

- **`onCounts_`/`offCounts_` stay full-sensor-sized rather than shrinking
  to the ROI.** Avoids resizing/reindexing them (and the Goal 6 follow-up's
  `CloseCurrentWindowLocked()`/`FlushBacklogLocked()`, which assume
  full-sensor sizing) on every ROI change, at the cost of pixels outside
  the active ROI harmlessly accumulating stale (zero, since no events
  arrive there) counts between changes — `BuildAndSwapFrame()` never reads
  outside the active ROI window anyway.
- **Hot-pixel calibration disables the ROI while scanning, then restores
  it.** If a shrunk ROI were left active, only pixels inside that window
  could ever be found as hot — matches the official sample's own
  `clearROI()`-before-scan pattern.
- **Calibration merges (union) into `EBS-BlockedPixels`, never replaces
  it** — explicit user decision, confirmed via `AskUserQuestion`, so
  re-running calibration never silently un-masks a pixel added by hand.
- **`x:y` pairs, not `x,y`, separated by `;`.** The original plan (and the
  user's own example wording) used a comma between x and y — self-testing
  this property for the first time threw `ValueError: Property value
  "5,5;10,20" contains reserved characters` from pymmcore-plus.
  Root-caused to `MM::g_FieldDelimiters` (`MMDeviceConstants.h`) being
  `","` — MMCore's own `setProperty()` (`MMCore.cpp`'s
  `IsPropertyValueValid` check) rejects **any** property value containing
  a comma, regardless of device. `;` isn't reserved, so pairs stayed
  semicolon-separated; `:` replaced `,` within each pair. **Lesson for
  future goals**: never use `,` inside a property value in this codebase,
  even for a brand-new custom format — it's an MMCore-wide restriction,
  not something a device adapter can opt out of.
- **`set_pixel()`'s `enable` boolean is implemented against the header's
  documented contract** (`Metavision::I_RoiPixelMask::set_pixel()`'s doc
  comment says "Pixel will be masked when true"), not the official
  `metavision_active_pixel_detection.cpp` sample's own usage (which calls
  `set_pixel(x, y, false)` to mask) — a genuine contradiction between the
  header and the sample that can only be fully resolved by a real-hardware
  GUI check (does a masked pixel actually go dark, or does it keep firing,
  or intensify?). Implemented behind one named constant
  (`kMaskPixelValue` in `ApplyBlockedPixelsToHardware()`) specifically so
  this is a one-line flip if the GUI check shows the sample's convention
  was actually right for this sensor. **Still open, flagged for the
  user's GUI walkthrough.**

### Verified locally

- **Build**: `MSBuild ProphEBS.sln /p:Configuration=Release /p:Platform=x64`
  succeeded with 0 warnings/errors on the first attempt (VS2022 Preview
  toolset, PowerShell tool, same as every prior goal) — including the two
  new `<metavision/hal/facilities/i_roi.h>`/`i_roi_pixel_mask.h` includes
  with no `.vcxproj` changes needed.
- **Real hardware, via `tools/test_prophebs.py`** (extended with Goal 7
  checks): all Goal 1-6 checks still passed unchanged (no regression) on
  the connected IMX636. New checks, all against real hardware:
  - `EBS-BlockedPixels` round-tripped `"5:5;10:20"`; a malformed value
    (`"abc"`) was correctly rejected with the previous valid value
    retained (confirmed via `pymmcore-plus` raising `ValueError`, not a
    silent no-op).
  - `EBS-HotPixelCalibDurationMs`/`EBS-HotPixelStddevK` round-tripped
    within their limits (defaults confirmed 200 ms / 3.0).
  - **Hardware ROI produced a genuine, large data reduction, not just a
    display crop**: `SetROI` to a quarter-frame window (320x180 of the
    full 1280x720) reported back correctly via `GetROI`/`snapImage()`
    shape; `EBS-AvgEventRate-MEvps` measured **0.0345 MEv/s** with that
    ROI active vs. **10.1006 MEv/s** at full frame moments later (same
    ambient scene) — a ~300x reduction, strong real evidence the crop is
    happening at the sensor via `I_ROI`, not in software. `ClearROI()`
    correctly restored the full `(0, 0, 1280, 720)` geometry.
  - **On-demand hot-pixel calibration found a real hot pixel**: a 200 ms
    calibration run against the connected IMX636 completed in ~0.22 s and
    reported `Done: 1 masked (1 new)`, with `EBS-BlockedPixels` reading
    back `114:239` afterward — a real outlier pixel on this physical
    sensor, not a synthetic/zero result. The `I_RoiPixelMask` facility
    itself was confirmed present and callable on this Gen4.1/IMX636
    sensor (no exception from `get_facility<I_RoiPixelMask>()`,
    `set_pixel()`, or `apply_pixels()`) — resolving the "does this
    facility even work on Gen4.1, or only report coordinates" risk noted
    in the official sample's own help text, at least as far as the API
    surface goes (see the enable=true/false semantics caveat above for
    what's still unresolved).
  - Manually setting `EBS-BlockedPixels` against real hardware afterward
    did not break subsequent `snapImage()` calls.
  - No-camera fallback path (exercised separately, logic inspected):
    `EBS-DetectHotPixelsNow` set to `"Run"` with no streaming camera fails
    promptly via the `!cameraConnected_ || !streaming_` guard, reporting
    `Failed: no camera streaming` and resetting the trigger to `Idle` —
    not exercised in this run since a camera happened to be connected
    throughout, but the guard clause is unconditional and was read
    directly to confirm the promptly-fails behavior.
  - `SetROI`/`GetROI`/`ClearROI` against the no-hardware fallback
    checkerboard were not separately re-verified in this run (a camera was
    connected throughout self-testing), but `ApplyRoiToBuffers()`'s
    fallback-rendering branch is exercised by the same code path either
    way and was inspected directly.

### Follow-up: per-polarity detection, new defaults, and a truncation bug found under a loose threshold

User asked to confirm whether hot-pixel calibration looks at both ON and
OFF events separately, and to change the defaults to
`EBS-HotPixelStddevK=5` / `EBS-HotPixelCalibDurationMs=2000` (2 s). Checking
the code directly confirmed it did **not** — `calibCounts` was a single
merged counter (total events regardless of polarity), so a pixel hot only
in one polarity direction could be diluted into the combined
distribution's noise rather than standing out.

**Fix**: replaced the single `calibCounts` with `calibOnCounts`/
`calibOffCounts`, split by `ev->p`. Mean/stddev/threshold are now computed
independently per polarity (`onThreshold`, `offThreshold`), and a pixel is
masked if it's an outlier in **either** channel
(`calibOnCounts[idx] > onThreshold || calibOffCounts[idx] > offThreshold`)
— catching a pixel that's abnormally hot in ON only, OFF only, or both.
Defaults changed to `g_DefaultHotPixelStddevK = 5.0`,
`g_DefaultHotPixelCalibDurationMs = 2000.0`.

**Bug found immediately while re-testing this change**: rerunning the
real-hardware calibration check crashed the self-test harness with
`ValueError: invalid literal for int() with base 10: ''` while parsing
`EBS-BlockedPixels`. Root cause: the test script had left
`EBS-HotPixelStddevK` at `2.5` (from an earlier round-trip probe) instead
of restoring it before the real calibration run, and — combined with the
new per-polarity split, whose two independent channels each have a lower,
noisier mean than the old merged count — this loose threshold flagged
**35,721** pixels as "hot" in a single run. `EBS-BlockedPixels`'s
serialized string exceeded MMCore's `MM::MaxStrLength` (1024 chars) and
was silently truncated mid-pair by MMCore itself on the way back out,
which is what produced the empty trailing token the parser choked on.

This exposed a real correctness gap, not just a test-script bug: nothing
in `OnDetectHotPixelsNow()` bounded how large `EBS-BlockedPixels` could
grow, so a large enough result (whether from calibration or, in principle,
a very long manually-typed list) could silently corrupt the property
string while `blockedPixels_` itself — and the mask already pushed to
hardware from it — stayed fully correct, an inconsistency that would only
surface as confusing dead/mismatched-looking property state.

**Fix**: `OnDetectHotPixelsNow()` now ranks newly-found candidate pixels by
"hotness" (`max(onExcess, offExcess)`, i.e. how far each pixel's count
exceeds its own polarity's threshold) and greedily adds the hottest ones
first, stopping once the running serialized length would exceed
`MM::MaxStrLength` — so if the budget is exhausted, it's the most
marginal outliers that get dropped, not an arbitrary raster-order cutoff.
Existing (e.g. manually-set) pixels are always kept in full; only new
calibration additions are capped. `EBS-HotPixelCalibStatus` reports any
drop explicitly (`"Done: 128 masked (128 new, 5866 dropped (property
length limit))"`), and a `LogMessage()` records the same for the CoreLog.
`tools/test_prophebs.py` was also fixed to restore `EBS-HotPixelStddevK`
right after its round-trip probe, and gained a dedicated
deliberately-loose-threshold stress check (`EBS-HotPixelStddevK` forced to
its floor) specifically to exercise this cap, asserting
`len(EBS-BlockedPixels) <= 1024` always holds.

**A second, more fundamental finding, left as-is (not a bug to fix, a
real limitation of the technique)**: even at the new defaults (2 s, k=5)
against real ambient conditions on this dev machine, calibration still
flagged several thousand pixels (128 kept + 5866 dropped by the cap in one
run). Root cause: `mean`/`stddev` are computed **globally across the whole
frame**, so any real scene content during the calibration window (motion,
edges, uneven lighting) inflates the frame's overall variance and pulls
plenty of otherwise-normal pixels above a global threshold — the detector
cannot distinguish "this pixel is a genuine hardware defect" from "this
pixel happens to be looking at something that moved just now." This
matches why the official `metavision_active_pixel_detection.cpp` sample is
normally run against a static/dark scene (lens cap on) rather than live
ambient conditions. **Not fixed** — flagged as a usage note (see "Open
questions" below) rather than a code change, since the statistics
themselves are working as designed; the fix is procedural (calibrate
against a static scene), not algorithmic.

**Verified**: rebuilt (0 warnings/errors) and reran
`tools/test_prophebs.py` against the connected IMX636 three times across
these fixes. Final run: all Goal 1-6 checks passed unchanged; the
real-hardware calibration check at actual defaults (2000 ms/k=5) reported
`Done: 128 masked (128 new, 5866 dropped (property length limit))` with a
well-formed, fully parseable `EBS-BlockedPixels` string; the dedicated
loose-threshold stress check (k forced to its 0.1 floor, 200 ms) reported
`Done: 130 masked (130 new, 139212 dropped (property length limit))` —
confirming the cap holds even under a far more extreme overflow — with
`len(EBS-BlockedPixels) <= 1024` asserted true throughout. Full suite
(`SUCCESS`, exit 0).

### Follow-up: calibration scoped to the active ROI, and a wider EBS-HotPixelStddevK range

User asked for two more changes: scan only the currently active ROI during
hot-pixel calibration, not the whole chip regardless of what's selected;
and widen `EBS-HotPixelStddevK`'s max to 100, with a new default of 20 (up
from 5).

**Implementation**: `OnDetectHotPixelsNow()` no longer disables the
hardware ROI before scanning (the previous "disable, scan full chip,
restore" dance is gone entirely). Instead, `calibOnCounts`/
`calibOffCounts` are now sized to `roiXSize_ x roiYSize_` (not
`sensorWidth_ x sensorHeight_`) and indexed **ROI-locally**: the temporary
CD callback converts each event's absolute `(ev->x, ev->y)` into an
ROI-local index (`(ev->y - roiY) * roiW + (ev->x - roiX)`), skipping
anything outside the active window (a defensive guard for any in-flight
events from just before the ROI was applied -- shouldn't normally happen
once `I_ROI` is actively filtering). Mean/stddev are therefore computed
only over the pixels actually being viewed/recorded, not diluted by a huge
sea of always-zero pixels outside the ROI. When merging outliers into
`blockedPixels_`, the ROI-local index is converted back to absolute sensor
coordinates (`roiX + idx % roiW`, `roiY + idx / roiW`) before storing --
`EBS-BlockedPixels` itself is still always absolute/global coordinates,
unaffected by this internal scanning change.

`g_DefaultHotPixelStddevK` changed to `20.0`, `SetPropertyLimits` upper
bound changed to `100.0`.

**Design note**: ROI-scoped calibration is not just an optimization
(fewer pixels to scan) -- it also directly mitigates the "real scene
content inflates the global threshold" finding from the previous
follow-up, since the statistics are now computed only over whatever
region is actually being viewed/recorded rather than the entire chip
(where motion/edges anywhere on the sensor, even far outside the current
view, could previously pull the global mean/stddev around).

**Verified**: rebuilt (0 warnings/errors) and reran
`tools/test_prophebs.py` against the connected IMX636. New check: set a
quarter-frame ROI, ran calibration with a deliberately loose threshold
(`EBS-HotPixelStddevK` forced to its 0.1 floor) specifically so something
would very likely be flagged, and asserted **every** resulting
`EBS-BlockedPixels` coordinate falls within that ROI's bounds -- 126/126
did. At the real default (2000 ms, k=20) against the full sensor,
calibration reported `Done: 126 masked (126 new)` with **no property-length
cap triggered** (unlike the k=5 runs in the previous follow-up), consistent
with the wider default meaningfully tightening the threshold. Full suite
(`SUCCESS`, exit 0), no regression.

### Follow-up: masking wasn't actually suppressing events -- root cause and fix

User asked to specifically test the masking effect, not just that the
property/API calls succeeded: block a real 10x10 region (100 pixels) and
confirm no events land there. Building this check properly (see
`tools/test_prophebs.py`) took two iterations:

- **First version was a false pass.** It only checked "is the blocked
  region's max pixel value 0," with no unmasked baseline for the same
  region -- a genuinely quiet corner of the scene would trivially pass
  without proving anything. **Fixed** by first confirming the exact same
  region shows real nonzero activity *unmasked* (escalating `Exposure`
  100/300/1000 ms if the first attempt sees nothing, since ambient
  activity there isn't guaranteed), only then blocking it and comparing.
  Also, per the user's suggestion, the ON/OFF biases
  (`EBS-bias_diff_on`/`EBS-bias_diff_off`) are pushed toward maximum
  sensitivity (clamped to the hardware's own reported range, e.g. -40/-20)
  for the duration of this check specifically so it isn't at the mercy of
  whatever's happening in front of the sensor right now.
- **With a real baseline in place, the check failed for real**: 50
  (unmasked) vs. 50 (masked) -- confirming the user's report.
  `I_RoiPixelMask::set_pixel()`/`apply_pixels()` were succeeding without
  any exception, but genuinely not suppressing anything on this
  Gen4.1/IMX636 sensor.

**Root cause**: exactly the risk flagged (but not yet confirmed) back when
this facility was first wired up -- the official
`metavision_active_pixel_detection.cpp` sample's own help text warns that
full live masking via `I_RoiPixelMask` is only demonstrated for GenX320;
Gen4.1/IMX636 apparently only reveals the intended coordinates via that
facility without hardware enforcing them. Found the actual mechanism by
locating `metavision/hal/facilities/i_digital_event_mask.h`
(`Metavision::I_DigitalEventMask`/nested `I_PixelMask::set_mask(x, y,
enabled)`, doc comment: "when true, the mask will prevent pixel from
generating CD event" -- unambiguous, unlike `I_RoiPixelMask`'s
contradictory docs) and its Gen4.1-specific implementation
(`gen41_digital_event_mask.h`, confirming this sensor family really does
implement it) with a hard-coded `NUM_MASK_REGISTERS_ = 64` -- a real,
fixed hardware limit on how many pixels can actually be masked at once on
this sensor generation, structurally different from `I_RoiPixelMask`'s
seemingly-unbounded list API.

**Fix**: `ApplyBlockedPixelsToHardware()` now applies `blockedPixels_`
primarily via `I_DigitalEventMask` -- `get_facility<I_DigitalEventMask>()`,
then `set_mask(x, y, true)` on the first `min(blockedPixels_.size(),
get_pixel_masks().size())` hardware slots (clearing any unused remaining
slots via `set_mask(0, 0, false)`), logging if the list has more entries
than there are slots. `I_RoiPixelMask` is still also written to afterward
(harmless if inert on this sensor, and per the sample it *is* the real
mechanism on other generations like GenX320) -- belt and suspenders, not
an either/or.

**Verified**: rebuilt (0 warnings/errors) and reran the masking-effectiveness
check -- baseline unmasked activity in a 7x7 region (49 pixels, chosen to
fit within the 64-slot limit so every pixel in the test region is actually
hardware-enforced) was 50; the same region after blocking read exactly
**0**, while the rest of the frame kept showing real activity (255). Full
suite (`SUCCESS`, exit 0), no regression.

### Follow-up: cap calibration additions to the hardware's own 64-slot limit, not just the property string length

Immediate user follow-up: since only 64 pixels can ever be genuinely
enforced, apply the same hotness-ranked "keep the most significant
outliers" logic used for the property-string-length cap to this hardware
limit too -- there's no point letting `EBS-BlockedPixels` list more
calibration-found pixels than can ever actually be masked.

**Fix**: `OnDetectHotPixelsNow()` now queries
`cam_.get_facility<I_DigitalEventMask>().get_pixel_masks().size()` once
per run (falling back to "no cap" if the facility isn't available) and
enforces it as a second, independent budget alongside the existing
`MM::MaxStrLength`-derived one in the same hotness-ranked loop -- whichever
budget is tighter determines how many new candidates get added, with
existing (e.g. manually-set) pixels still always kept in full regardless
of either. `EBS-HotPixelCalibStatus`/the CoreLog wording was generalized
from "property length limit" to "length/hardware-slot limit" since a drop
can now come from either cause.

**Verified**: reran the full suite. The real-hardware calibration check
(default 2000 ms/k=20) now reports `Done: 64 masked (64 new, 31 dropped
(length/hardware-slot limit))` -- exactly capped at the real 64-slot
hardware limit, not the ~100-pixel property-string budget from before.
The loose-threshold stress check similarly capped at exactly 64. Full
suite (`SUCCESS`, exit 0), no regression; masking-effectiveness check
still passed (0 events in the masked 7x7 region, baseline 50 unmasked).

### Not yet verified

- ~~The `enable=true`/`false` semantics ambiguity for `I_RoiPixelMask::
  set_pixel()`~~ — **resolved** by the masking-effectiveness follow-up
  above: `I_RoiPixelMask` turned out not to be the enforcing facility on
  this sensor at all (see there), and the actually-effective
  `I_DigitalEventMask::set_mask()` has an unambiguous doc comment ("mask
  will prevent pixel from generating CD event" when `true`) confirmed
  correct by the automated before/after masking test (50 events unmasked
  -> 0 masked). `kMaskPixelValue` in `ApplyBlockedPixelsToHardware()`'s
  now-secondary `I_RoiPixelMask` call is left as originally documented,
  moot on this sensor either way.
- **Actually dragging/zooming an ROI rectangle in MicroManager's Live
  view** and visually confirming the displayed image crops accordingly —
  the self-test only exercises `core.setROI()`/`getROI()` directly via
  `pymmcore-plus`, not MM Studio's own FOV-selection/zoom-to-center UI
  gesture.
- **Inspecting a recorded `.raw` file's actual event content** to confirm
  ROI cropping reached the recording itself (not just the live display) —
  the self-test only measured the live `EBS-AvgEventRate-MEvps` drop, a
  strong indirect signal, but didn't parse a `.raw` file's contents
  directly.
- A full Hardware Config Wizard / Device Property Browser walkthrough,
  same as every prior goal's hand-off pattern.

### Follow-up: calibration now replaces EBS-BlockedPixels instead of merging, new defaults

User reported that running calibration repeatedly kept finding *more*
pixels each time, eventually blocking pixels that clearly weren't
genuinely hot -- especially noticeable on a small ROI with few real
events. Also asked for new defaults: `EBS-HotPixelStddevK` = 10,
`EBS-HotPixelCalibDurationMs` = 5000 (5 s).

**Root cause**: a masked pixel generates zero events by hardware design
(that's the whole point of `I_DigitalEventMask`). The original merge
design left previously-masked pixels masked *during* a new calibration
scan, which silently shrank the population the scan's mean/stddev were
computed over on every successive run -- each run measured an
ever-quieter population than the last, dragging the threshold down and
starting to flag genuinely normal pixels as statistical outliers. This
compounds worst exactly where the user saw it: a small ROI with few real
events to begin with.

**Fix**: `OnDetectHotPixelsNow()` now clears `blockedPixels_` and pushes
that empty state to hardware (unblocking everything) *before* the scan
starts, not after -- every run's mean/stddev now comes from the same
full, honest population. As a direct consequence, this also changes
`EBS-BlockedPixels` from "merge (union)" to "replace" semantics -- a
deliberate reversal of the original Goal 7 merge decision, explicitly
requested this way: "first unblock all pixels, then do the calculation,
then block the most hot ones." A pixel added by hand right before running
calibration will not survive a calibration run -- that's intended, not a
regression. The hotness-ranking/truncation-cap logic (property-length and
hardware-slot-count budgets) is otherwise unchanged, just simplified since
there's no longer an "existing" list to exclude candidates against.
`g_DefaultHotPixelStddevK` changed to `10.0`, `g_DefaultHotPixelCalibDurationMs`
changed to `5000.0`.

**Verified**: rebuilt (0 warnings/errors) and reran `tools/test_prophebs.py`
against the connected IMX636. New check: manually set `EBS-BlockedPixels`
to a single arbitrary sentinel pixel (`500:500`, essentially guaranteed
not to be a real outlier), ran calibration, and confirmed the sentinel is
gone from `EBS-BlockedPixels` afterward -- proof calibration genuinely
replaces rather than merges. Real-hardware calibration at the new defaults
(5000 ms/k=10) reported `Done: 64 masked, 61 dropped (length/hardware-slot
limit)` (capped at the real 64-slot hardware limit, as expected). Full
suite (`SUCCESS`, exit 0), no regression; the masking-effectiveness check
from the previous follow-up still passed (0 events in a masked 7x7 region,
baseline 50 unmasked).

### Open questions / TODO for later goals

- **Calibrate against a static/dark scene (lens cap on) for a clean
  hot-pixel-only result.** Because `mean`/`stddev` are computed globally
  across the whole frame, real scene activity (motion, edges, uneven
  lighting) during the calibration window inflates the frame's overall
  variance and can pull genuinely normal pixels above threshold alongside
  real hardware defects -- the detector has no way to distinguish "broken
  pixel" from "pixel that happened to see something move." This isn't a
  bug (the mean+k*stddev math is doing exactly what it's asked), just a
  real limitation of a whole-frame statistical approach -- worth
  documenting for the user directly (and maybe surfacing as guidance text
  somewhere the GUI shows it) before v0.7 ships, since running calibration
  in a busy scene will visibly over-mask compared to a static one.
- Goal 9 ("full suite polishing") is next per the roadmap -- error
  handling, tests, and documentation sweep across everything built so far.

## Goal 8 -- Additional hardware/SDK features

### Status: DONE — confirmed working by user, tagged v0.8

### How this goal was chosen

Before starting the originally-final "full suite polishing" goal, scanned
the actually-installed Metavision SDK (`C:\Program Files\Prophesee\include\`)
for HAL facilities and SDK algorithms Goals 1-7 hadn't touched yet, plus MM's
own camera API (`Binning`, the unmerged camera-triggering-API-v2 proposal
already vendored in this repo's `mmCoreAndDevices` submodule). Presented a
shortlist to the user with a relevance ranking; the user picked five to
build and declined two (offline `.raw` replay via
`Metavision::Camera::from_file()`, and `I_DigitalCrop` as likely redundant
with the Goal 7 hardware ROI) -- both recorded in `claude_instructions.txt`'s
Goal 8 entry alongside what was declined and why. The original "full suite
polishing" goal was renumbered to Goal 9 to make room.

### What was built

- **Hardware trigger in/out** (`Metavision::I_TriggerIn`/`I_TriggerOut`) --
  `EBS-TriggerIn-Channel` (allowed values populated from
  `I_TriggerIn::get_available_channels()` when connected, all three enum
  values as a state-only fallback otherwise) picks which channel
  `EBS-TriggerIn-Enabled` applies to; `EBS-TriggerOut-Enabled`/`-PeriodUs`/
  `-DutyCycle` configure the camera's own output pulse. No integration with
  MM's own camera API -- that's still just an unmerged v2 proposal (see
  `third_party/mmCoreAndDevices/camera_triggering_API_v2.md`), so this is
  plain `EBS-*` properties, same convention as every other Goal 5/6 hardware
  setting in this file.
- **Sensor-level event-rate band-pass filter**
  (`Metavision::I_EventRateActivityFilterModule`) -- `EBS-EventRateFilter-Enabled`
  plus four hysteresis thresholds (`-LowerStart`/`-LowerStop`/`-UpperStart`/
  `-UpperStop`, evt/s). Distinct from the existing Goal 6 software
  `EBS-ActivityFilter-*` (host-side, already-decoded events) and the Goal 5
  `EBS-EventTrailFilter-*` (HAL, but per-pixel STC/trail-based, not
  rate-based) -- this one drops all events at the sensor itself whenever the
  overall event rate falls outside the configured band, saving USB
  bandwidth/power. Each threshold's `AfterSet` reads the whole current
  `thresholds` struct via `get_thresholds()` first and overwrites only its
  own field before calling `set_thresholds()`, per the header's own warning
  that a partially-initialized struct risks random values for unsupported
  fields.
- **Time-decay view mode** -- a fifth `EBS-ViewMode` value (`TimeDecay`),
  hand-rolled rather than adopting
  `Metavision::TimeDecayFrameGenerationAlgorithm` directly, since that class
  renders into a `cv::Mat` and this project has deliberately carried no
  OpenCV *link* dependency since Goal 2 (headers only, via Metavision's own
  transitive includes). Two new full-sensor-sized arrays,
  `lastEventTimeUs_`/`lastEventPolarity_`, are updated in `OnEventsCD()`'s
  existing per-event loop alongside `onCounts_`/`offCounts_` -- but, unlike
  those, are never reset by `CloseCurrentWindowLocked()`/
  `FlushBacklogLocked()`, since "how long ago did this pixel last fire" is
  meaningful across integration-window boundaries. `BuildAndSwapFrame()`
  renders `sign * exp(-(now - lastEventT) / EBS-TimeDecay-TimeConstant-us)`
  through the same offset/scale formula the other four modes already use.
  "now" (`nowT_`/`nowWallAnchor_`, a (sensor-time, wall-time) anchor pair
  updated once per `OnEventsCD()` batch, including on the backlog-flush fast
  path so the decay clock never freezes during a flush storm) is
  extrapolated forward by however much wall-clock time has passed since the
  last batch, so the decay keeps advancing smoothly between batches and
  during a genuinely idle scene, not just once per batch.
- **Real spatial binning** -- `GetBinning()`/`SetBinning()` were 1x1-only
  stubs since Goal 1; now a real `binning_` atomic (1/2/4) drives both
  `ApplyRoiToBuffers()` (sizes the displayed/recorded frame to
  `roiSize / binning`) and `BuildAndSwapFrame()` (sums `onCounts_`/
  `offCounts_` over each `binning x binning` block into one output pixel --
  an event-camera analog of a real sensor's charge binning: more raw event
  count per output pixel, i.e. higher apparent sensitivity at the cost of
  resolution). `TimeDecay` mode can't sum decay values meaningfully, so its
  binning instead takes whichever sub-pixel in the block has the strongest
  (least-decayed) activity. Wired into MM's own standard `Binning` property
  (a `CPropertyAction` was added where none existed before) so it shows up
  exactly where a user expects camera binning to live.
- **Camera sync mode** (`Metavision::I_CameraSynchronization`) --
  `EBS-SyncMode` (`Standalone`/`Master`/`Slave`). Applied to hardware once,
  in `CreateSyncModeProperty()` (called from `Initialize()` before
  `StartEventStreaming()`/`cam_.start()` run), per the facility's own doc
  comment that the mode must be set before the camera starts streaming.
  Changing it later (already streaming) is still allowed by the property
  system; a hardware rejection is only logged, not surfaced as an error --
  the usual "log, don't fail" convention.

### Design decisions (and why)

- **Declined offline `.raw` replay and `I_DigitalCrop`** -- explicit user
  choice. Offline replay (`Camera::from_file()` + `OfflineStreamingControl`)
  would have been useful for hardware-free testing/demos, but wasn't asked
  for. `I_DigitalCrop` looked likely redundant with the Goal 7 hardware ROI
  without a clear additional benefit.
- **No MM camera-triggering-API-v2 integration for the trigger in/out
  properties.** That proposal (vendored in this repo's own submodule) is
  still unmerged upstream -- building against it would mean coding to an API
  that doesn't exist in the actual `MMDevice`/`MMCore` headers this project
  links against. Plain `EBS-TriggerIn-*`/`EBS-TriggerOut-*` properties give
  the same practical control today; revisit if that API ever lands.
- **Time-decay hand-rolled, not `TimeDecayFrameGenerationAlgorithm`.** See
  "What was built" above -- avoiding a new OpenCV link dependency this late
  in the project outweighed reusing Prophesee's own implementation,
  especially since the existing offset/scale/8-bit-grayscale rendering
  pipeline (Goal 6) already had everywhere the new mode needed to plug in.
- **Binning sums rather than averages.** Matches how a real sensor's charge
  binning increases sensitivity (more signal per output pixel), which is the
  actual reason a user would reach for binning on a dim/noisy scene -- an
  average would just be a blurrier image at the same brightness, defeating
  the point.
- **Sync mode applied once at `Initialize()` time, not re-applied on every
  `StartEventStreaming()`.** The facility's contract is "before the camera
  starts" -- `Initialize()` already runs `CreateSyncModeProperty()` before
  the one and only `StartEventStreaming()` call in this adapter's lifetime,
  so there's no second start to catch.

### Verified locally

- **Build**: `MSBuild ProphEBS.sln /p:Configuration=Release /p:Platform=x64`
  succeeded with 0 warnings/errors on the first attempt (VS2022 Preview
  toolset, PowerShell tool, same as every prior goal) -- including the four
  new HAL facility includes (`i_camera_synchronization.h`,
  `i_event_rate_activity_filter_module.h`, `i_trigger_in.h`,
  `i_trigger_out.h`) with no `.vcxproj` changes needed.
- **Real hardware, via `tools/test_prophebs.py`** (extended with Goal 8
  checks): all Goal 1-7 checks still passed unchanged (no regression) on the
  connected IMX636. New checks: `Binning` round-tripped 1 -> 2 -> 4 -> 1 with
  the snapped image shape scaling correctly each time (confirmed against the
  full 720x1280 frame); `EBS-TriggerIn-Enabled`/`EBS-TriggerOut-Enabled`/
  `-PeriodUs`/`-DutyCycle` round-tripped; `EBS-EventRateFilter-*` thresholds
  and `-Enabled` round-tripped; `EBS-SyncMode` round-tripped (only
  `Standalone` is guaranteed to be accepted once already streaming, per the
  facility's own before-start contract -- the check only asserts the
  property always reflects one of the three valid values, not that
  Master/Slave are accepted mid-stream); `EBS-ViewMode=TimeDecay` +
  `EBS-TimeDecay-TimeConstant-us` round-tripped with a correctly-shaped/typed
  snap. Full script (`SUCCESS`, exit 0).
- **Ad hoc real-content checks** (beyond plumbing-only round-trips): a
  TimeDecay-mode snap against real ambient activity showed genuine spatial
  variation (`std ≈ 1.27` across the frame, `max = 196` vs. a ≈10 baseline --
  not a flat, degenerate image); a `Binning=4` snap in `NetSigned` mode
  produced a correctly-shaped `(180, 320)` frame with real non-degenerate
  content (`min=0`, `max=10`), confirming the binned sums reflect actual
  accumulated activity, not zeros.
- **Not yet verified**: a real walkthrough in the MicroManager Studio GUI --
  same hand-off pattern as every prior goal -- specifically: `Binning`
  visibly changing the Live view's resolution; the trigger in/out properties
  actually driving/reading real TTL signals on a rig wired up for it (not
  testable on this dev machine, no external trigger hardware attached); the
  event-rate filter's bandwidth-saving effect visible in
  `EBS-AvgDataRate-MBps` on a genuinely idle vs. busy scene; `EBS-SyncMode`
  tested with a second, physically linked EBS (not available on this dev
  machine); and the TimeDecay view mode's persistence-style visual look
  compared side-by-side with the existing four modes.

### Follow-up: EBS-SyncMode silently reverting, and an EBS-TriggerIn-Count diagnostic

User's real-GUI walkthrough surfaced two things in the same session:

1. **`EBS-SyncMode` set to Master/Slave "resets itself back to default."**
   This traces directly to the facility's own documented contract
   ("must be called before starting the camera") colliding with this
   adapter's architecture: `StartEventStreaming()`'s one `cam_.start()` call
   happens once, permanently, near the end of `Initialize()` -- so *any*
   post-init attempt to change sync mode is guaranteed to be rejected by the
   hardware (or silently ineffective), no matter when the user tries it
   afterward. The property was already logging a rejection message when this
   happened (`"sync mode '...' rejected by hardware"`), but nothing in the
   GUI made that non-obvious constraint visible -- from the Device/Property
   Browser it just looked like a bug. **Fix**: `EBS-SyncMode` is now a
   pre-init-only property, exactly like `EBS-BiasRangeCheckBypass` (created
   in the constructor with `isPreInitProperty=true`, applied to hardware
   exactly once via the new `ApplySyncModeToHardware()`, called from
   `Initialize()` at the same point `CreateSyncModeProperty()` used to run).
   The old `OnSyncMode` property-action handler and `localSyncMode_` fallback
   member are both gone -- there's no runtime state left to keep in sync
   once the property can only ever be read back, never live-changed. This
   also means Master/Slave now behave exactly like every other pre-init
   property in this adapter: settable in the Hardware Config Wizard before
   "Add Device," greyed out after, with no surprise reversion because there's
   no longer a code path that tries to apply it post-init at all.
2. **User asked, and confirmed understanding, that Live view correctly
   doesn't stop when `EBS-TriggerIn-Enabled` is on with nothing wired to the
   trigger-in pin** -- this is correct, not a bug: the EBS sensor is
   asynchronous and free-running by design, and `I_TriggerIn` only turns on
   *monitoring* of that external pin (each pulse generates a
   `Metavision::EventExtTrigger` marker interleaved into the event stream,
   for aligning external stimuli to event data in post-processing) -- it is
   not a "wait for a trigger before capturing" gate the way a frame camera's
   trigger-in typically is, and CD events (what drives Live view) never
   depend on it. Confirmed this is architecturally correct by reading the
   `I_TriggerIn` header's own doc comments again, not just by inspection of
   this adapter's code.
   Since there was no way to actually confirm trigger-in monitoring is doing
   anything without an oscilloscope, added **`EBS-TriggerIn-Count`** (read-only) --
   counts real `EventExtTrigger` events via a new `cam_.ext_trigger().add_callback()`
   registered/removed alongside the existing `cd()`/`raw_data()` callbacks in
   `Start`/`StopEventStreaming()`, a single atomic add per batch (same
   off-the-hot-path shape as `totalRawBytes_`/`totalEventCount_`), pushed to
   the GUI on the existing Goal 5 stats cadence by `UpdateStats()` (same
   `OnPropertyChanged()` pattern as `EBS-CallbackLagMs`/`EBS-BacklogFlushCount`).

**Verified**: rebuilt (0 warnings/errors) and reran `tools/test_prophebs.py`
against the connected IMX636 -- `EBS-SyncMode` now asserted
`isPropertyPreInit()` (same check already used for
`EBS-BiasRangeCheckBypass`) and set once before `initializeDevice()`, reading
back its configured value (`Standalone`) afterward with no live-set attempt
at all. `EBS-TriggerIn-Count` read back `0` throughout (expected and
correct -- nothing is physically wired to this dev machine's trigger-in
pin; the property existing and being queryable is what's actually being
verified here, not a real pulse count). Full suite (`SUCCESS`, exit 0), no
regression across Goals 1-7.

**Not yet verified**: `EBS-TriggerIn-Count` actually incrementing against a
real signal generator/pulse source wired to the trigger-in pin -- not
available on this dev machine.

### Open questions / TODO for later goals

- **MM's `SupportsMultiROI`/multi-simultaneous-ROI API** -- noted during the
  Goal 8 feature scan as a possible future addition, but not pursued now
  since it's unclear whether the Metavision HAL (this sensor generation, or
  any other) actually supports more than one simultaneous ROI window at the
  hardware level. Revisit if that ever becomes relevant.
- Goal 9 ("full suite polishing," the original Goal 8) is next per the
  roadmap -- error handling, tests, and documentation sweep across
  everything built so far (now including this goal).

## Goal 9 -- Full suite polishing

### Status: self-tested on real hardware, awaiting user GUI confirmation before tagging v0.9

### How this goal was scoped

Per `claude_instructions.txt`'s Goal 9 entry ("do a full sweep over
everything and make proper tests, documentation, handle errors, etc"), plus
its two explicit asks -- a clear message for "no SDK found," and grouping
related properties under a shared alphabetically-sortable prefix (the user's
own example: "for hot pixels all start with HotPixel...") -- an Explore
agent surveyed the whole codebase first: every registered MM property
(grouped by feature area, flagging any that broke its group's naming
convention), what actually happens when the Metavision SDK is missing at
build time vs. runtime, `tools/test_prophebs.py`'s structure and gaps, and
`docs/BUILD_AND_USAGE.md`'s section coverage. That survey found four more
naming-inconsistent groups beyond the cited hot-pixel one, confirmed the
SDK-missing-DLL case is architecturally uninterceptable from inside the
adapter (fails at the Windows loader level before any of this project's
code runs), found `docs/BUILD_AND_USAGE.md` had no Goal 7 or Goal 8 sections
at all, and found the self-test harness was almost entirely happy-path
round-trips (the sole exception being the Goal 7 malformed-`EBS-BlockedPixels`
check). Presented to the user via `AskUserQuestion`, who chose: rename all
five naming-inconsistent groups (not just hot-pixel), document (not
code-fix) the SDK-missing case, and expand the test harness with real
negative/error-path checks.

### What was built

- **Property renames, five groups, for alphabetical grouping.** All
  `CreateProperty`/`SetProperty`/`GetProperty`/`OnPropertyChanged` call
  sites and every reference (comments, log-message text) updated via exact
  string-literal replacement in `ProphEBS.cpp`/`ProphEBS.h`/
  `tools/test_prophebs.py`/`docs/BUILD_AND_USAGE.md` -- deliberately not a
  blanket identifier rename (C++ symbol names like `g_PropBlockedPixels`,
  `OnBlockedPixels`, `ApplyBlockedPixelsToHardware` were left as-is; only
  the user-visible MM property name strings changed) to keep the diff
  scoped to what the user actually sees in the Property Browser:
  - `EBS-BlockedPixels` -> `EBS-HotPixelBlockedPixels`
  - `EBS-DetectHotPixelsNow` -> `EBS-HotPixelDetectNow`
  - `EBS-BiasRangeCheckBypass` -> `EBS-biasRangeCheckBypass` (lowercase `b`
    so it sorts adjacent to `EBS-bias_*` in a case-sensitive alphabetical
    listing; doesn't change any code behavior, MM property names are opaque
    strings either way)
  - `EBS-TimeDecay-TimeConstant-us` -> `EBS-ViewTimeDecayTimeConstant-us`
  - `EBS-DisplayRefreshMs` -> `EBS-ViewDisplayRefreshMs`
  - `EBS-LiveViewMinIntervalMs` -> `EBS-ViewLiveMinIntervalMs`
  - `EBS-TempRecordingFolder` -> `EBS-RawTempRecordingFolder`
  - `EBS-CallbackLagMs` -> `EBS-AvgCallbackLagMs`
  - `EBS-BacklogFlushCount` -> `EBS-AvgBacklogFlushCount`
  - `EBS-BacklogFlushThresholdMs` -> `EBS-AvgBacklogFlushThresholdMs`

  `docs/DEVLOG.md` itself (this file) was deliberately **not** updated --
  every earlier goal's section still uses the old names, matching what was
  true and tested at the time; this file's own header says "do not delete
  earlier sections," and rewriting history here would misrepresent what
  those goals actually verified.
- **SDK-missing documentation, no code change** (per the user's explicit
  choice -- this failure mode happens at the Windows loader level, before
  any adapter code runs, so there is no `try`/`catch` that could ever
  intercept it). Three new/reworded rows in `docs/BUILD_AND_USAGE.md`'s
  troubleshooting table distinguish: (1) the existing Goal 1 device-
  interface-version `(unavailable)` case, (2) a **new** entry for Metavision
  DLLs missing/not on `PATH` at runtime -- which looks *identical* in the
  GUI to case (1) (both just show `(unavailable)`, since Windows can't even
  get far enough to report a reason) -- distinguished by checking whether
  `mmcore list`'s interface-version check actually matches, and (3) a
  **new** entry for the SDK not installed (or `MetavisionSdkRoot` pointed
  wrong) at *build* time, which surfaces as a raw MSBuild/compiler error
  rather than anything from this project.
- **Missing Goal 7/Goal 8 tutorial sections added to
  `docs/BUILD_AND_USAGE.md`.** These two entire goals had shipped with zero
  user-facing tutorial coverage -- the doc jumped straight from Goal 6 to
  "Troubleshooting." Added in the same style as existing sections (property
  table + a numbered "try it" walkthrough), using the new (renamed)
  property names throughout, plus new troubleshooting-table rows for
  Goal 5's bias-clamp behavior, Goal 7's calibrate-against-a-static-scene
  guidance, and Goal 8's `EBS-SyncMode` pre-init-only / trigger-in-doesn't-
  gate-Live-view behaviors (both already explained in this file, just not
  yet surfaced in the user-facing tutorial).
- **Comment-density spot-check.** Cross-referenced every
  `CProphEBSCamera::` function definition in `ProphEBS.cpp` against its
  nearest doc-comment block. Found all of the genuinely non-obvious
  "hardware-facility wiring" functions (every `Create*Properties()`,
  `SerializeBlockedPixels`/`ParseBlockedPixels`,
  `ApplyBlockedPixelsToHardware`, `SetROI`/`ApplyRoiToBuffers`,
  `StartEventStreaming`/`StopEventStreaming`, `OnEventsCD`, etc.) already
  carry a `/**...*/` block explaining the non-obvious parts. The functions
  without one are uniformly simple, formulaic property getters/setters
  (`OnErcEnabled`, `OnHotPixelStddevK`, `GetBinning`, etc.) or trivial
  one-liners (`GetName`, `Busy`) whose names and the repeated
  `BeforeGet`/`AfterSet` shape already make them self-explanatory -- added
  no new comments here, since forcing a boilerplate doc block onto every
  one of these would be noise, not documentation (matches this project's
  own stated commenting philosophy).
- **Negative/error-path expansion of `tools/test_prophebs.py`** (previously
  almost entirely happy-path round-trips): out-of-range rejection checks
  for properties with unconditional limits (`EBS-HotPixelCalibDurationMs`,
  `EBS-HotPixelStddevK`, `EBS-TriggerOut-DutyCycle`, `EBS-ViewOffset`,
  `Exposure`) and for hardware-range-dependent ones when a camera is
  connected (`EBS-ERC-EventRate`, a bias, `EBS-AntiFlicker-HighFreq`);
  invalid-enum-string rejection for `EBS-ViewMode`,
  `EBS-EventTrailFilter-Mode`, `EBS-TriggerIn-Channel`,
  `EBS-AntiFlicker-FilterType`; a check that `pymmcore-plus` rejects a
  post-init `setProperty()` on the two pre-init-only properties
  (`EBS-biasRangeCheckBypass`, `EBS-SyncMode`); and a zero-size `SetROI`
  boundary check.

### Bug found by the new EBS-ERC-EventRate out-of-range test: silent int32 overflow

The new out-of-range check for `EBS-ERC-EventRate` failed on the very first
run -- not because the adapter correctly rejected the bad value, but because
setting it to `hardware_max + 1e9` (~4.2 GEv/s) silently corrupted the
property to **2147480000** (~`INT32_MAX`, not the requested value, not the
declared hardware max, and not the previous value either). Root cause:
`CreateErcProperties()` used `CreateIntegerProperty()` for
`EBS-ERC-EventRate`, backed by a `long localErcEventRate_` member and
`OnErcEventRate()`'s `long value; pProp->Get(value);` -- but this sensor's
own `I_ErcModule::get_max_supported_cd_event_rate()` reports **3.2 GEv/s**,
which already exceeds what a 32-bit signed `long` can represent
(`INT32_MAX` = ~2.147 GEv/s) even for values *within* the declared
hardware-reported range, let alone ones tested deliberately above it. The
overflow happens silently inside MMCore's own integer parsing/storage, well
before `SetPropertyLimits()`'s range check would normally reject an
out-of-bounds value -- so even a legitimate "set to the sensor's actual
max" request would have silently corrupted, not just adversarial
out-of-range input.

**Fix**: switched `EBS-ERC-EventRate` from `CreateIntegerProperty` to
`CreateFloatProperty` (`double`, more than enough precision for event-rate
integers in this range), changed `localErcEventRate_` from `long` to
`double`, and `OnErcEventRate()`'s `AfterSet` branch from `long value` to
`double value` (still narrowed to `uint32_t` only at the final
`erc.set_cd_event_rate()` call, matching the SDK's own parameter type).

**Verified**: rebuilt (0 warnings/errors) and reran `tools/test_prophebs.py`
against the connected IMX636 -- the out-of-range `EBS-ERC-EventRate` check
now correctly rejects the oversized value (property left unchanged), and
the existing Goal 5 `EBS-ERC-EventRate` round-trip (mid-range value,
in-bounds) still passes unchanged.

### Design decisions (and why)

- **String-literal renames, not identifier renames.** Keeps the C++-side
  diff minimal and focused on exactly what the user asked for (property
  names visible in MM's Property Browser); renaming `g_PropBlockedPixels`
  itself would touch far more lines for zero user-visible benefit.
- **`docs/DEVLOG.md` left un-renamed (historical).** This file is a
  chronological record of what was verified true *at the time each goal was
  built*; retroactively rewriting old sections to use new names would make
  the "Verified locally" evidence in those sections describe behavior that
  didn't actually exist yet at that point in the project's history.
- **Docs-only fix for the SDK-missing case, no `DllMain` or load-time
  guard added.** Confirmed via `ProphEBSModule.cpp` that this adapter has
  no custom `DllMain` (only the three required `MODULE_API` exports) --
  Windows' own loader resolves `mmgr_dal_ProphEBS.dll`'s implicit
  dependencies on `metavision_hal.dll` etc. *before* any of this project's
  code executes, so there is no hook-in point available even if one were
  wanted. Matches the user's own choice (docs, not a code fix) and the
  existing Goal 1 precedent for the device-interface-version case, which
  has the same fundamental shape.
- **Negative-path tests reuse the existing flat-script/sequential-assert
  structure**, not a new framework -- this is additive to
  `tools/test_prophebs.py`'s established pattern (per-goal comment banners,
  `assert`/`print`, camera-gated blocks where hardware ranges are involved)
  rather than a rewrite, keeping one consistent self-test script per this
  project's established convention.
- **`assert_rejected()` helper treats "value unchanged" as equivalent to
  "raised an exception."** Some out-of-range paths in this codebase throw
  (MMCore's own `SetPropertyLimits()` enforcement), while others might
  clamp silently at a lower layer -- both count as "the bad input didn't
  take effect," which is the actual property being tested, not the specific
  mechanism.

### Verified locally

- **Build**: `MSBuild ProphEBS.sln /p:Configuration=Release /p:Platform=x64`
  succeeds with 0 warnings/errors (same VS2022 Preview toolset as every
  prior goal), both before and after the `EBS-ERC-EventRate` int32-overflow
  fix.
- **Real hardware, via `tools/test_prophebs.py`** (extended with the Goal 9
  negative-path checks): full suite (`SUCCESS`, exit 0) against the
  connected IMX636, with no regression in any Goal 1-8 check under the
  renamed properties. New checks all passed: out-of-range rejection for
  `EBS-HotPixelCalibDurationMs`, `EBS-HotPixelStddevK`,
  `EBS-TriggerOut-DutyCycle`, `EBS-ViewOffset`, `Exposure`,
  `EBS-ERC-EventRate` (after the overflow fix), a bias property, and
  `EBS-AntiFlicker-HighFreq`; invalid-enum-string rejection for
  `EBS-ViewMode`, `EBS-EventTrailFilter-Mode`, `EBS-TriggerIn-Channel`,
  `EBS-AntiFlicker-FilterType`; pre-init-only post-init rejection (via
  `pymmcore-plus`'s own `RuntimeError`, see below) for
  `EBS-biasRangeCheckBypass`/`EBS-SyncMode`; and a zero-size `SetROI`
  boundary case.
- **A documented assumption corrected while writing the pre-init-property
  test**: the near-top-of-script comment (present since Goal 5/8) says raw
  MMCore's `PropertyCollection::Set()` doesn't block a post-init
  `setProperty()` call on a pre-init-only property at the Core API level --
  true for bare MMCore, confirmed by reading `MMDevice/Property.cpp`
  directly. But this test harness goes through `pymmcore-plus`'s
  `CMMCorePlus.setProperty()` wrapper, which turned out to add its own
  extra guard: it actually raises `RuntimeError("Cannot set pre-init
  property after initialization")` in that situation. Discovered while
  writing this exact check (the initial version assumed no exception and
  failed); fixed the test to assert the `RuntimeError` instead. The
  underlying bare-Core behavior described in the older comment is still
  accurate for bare MMCore -- it just doesn't describe what this
  particular test harness (or, presumably, any GUI/tool built on
  `pymmcore-plus`) actually does.
- **Re-confirmed the Goal 7 hot-pixel masking-effectiveness check is a
  genuine, pre-existing, hardware-timing-dependent flake, not something
  this goal's changes caused.** It failed 2 of 4 runs this session
  (`masked_region_max` reading 50 or 100 instead of 0 in the fixed
  `(0-6,0-6)` probe region) and passed the other 2 -- same intermittent
  behavior already noted as unresolved in the Goal 8 "Bug fix" section
  above, reproduced here across multiple fresh test runs with no code in
  that check touched by this goal (only the property name string changed).
  Not investigated further, consistent with the prior session's choice to
  leave it as a known, flagged flake rather than an open regression.
- **Not yet verified**: a real MicroManager Studio GUI walkthrough --
  same hand-off pattern as every prior goal -- specifically: the renamed
  properties actually appearing grouped/sorted as intended in the
  Device/Property Browser (alphabetical sort is a GUI behavior, not
  something `pymmcore-plus`'s flat property list exercises), and that the
  new Goal 7/Goal 8 `docs/BUILD_AND_USAGE.md` sections match what the user
  sees when following them.

### Open questions / TODO for later

- The Goal 7 hot-pixel masking-effectiveness flake (see above) remains
  unresolved -- worth a dedicated investigation if it starts affecting real
  usage, but continues to be treated as a known, documented, intermittent
  hardware/timing issue rather than a Goal 9 blocker.
- This was the last goal on the original roadmap (`claude_instructions.txt`
  numbers it "9. Full suite polishing"). Future work from here is
  maintenance/follow-up driven by the user, not a new numbered goal.

### Bug fix: bias property silently reverting past its own reported max (e.g. `bias_refr` 255 -> 235)

User reported (with `EBS-BiasRangeCheckBypass` On, "developer"/allowed-range
mode) that setting `EBS-bias_refr` to 255 -- the exact max `SetPropertyLimits()`
reports for it -- would "snap back" to 235 in MM. Reproduced directly against
the connected IMX636 with a small ad hoc pymmcore-plus script (bypassing the
GUI): `biases.set("bias_refr", 255)` **returns `true`** (no rejection), but a
subsequent `biases.get("bias_refr")` reads back `235`. Same class of mismatch
on `bias_diff`/`bias_diff_on`/`bias_fo` at their reported max, each clamping
to a different, non-obvious value.

Root cause: `LL_Bias_Info::get_bias_range()` (what `CreateBiasProperties()`
uses for `SetPropertyLimits()`) reflects the *allowed* range once bypass is
enabled, per its own doc comment -- so MM correctly lets the user type 255.
But the sensor firmware itself enforces a tighter, separate safety clamp that
`I_LL_Biases::set()`'s boolean return doesn't surface as a failure; it just
silently stores a smaller value. `OnBias()`'s `AfterSet` handler previously
never re-read the value it had just set, so the property kept showing
whatever the user typed until MM's next unrelated `BeforeGet` poll silently
overwrote it -- from the Property Browser this looked exactly like an
unexplained bug.

**Fix**: `OnBias()`'s `AfterSet` branch now calls `biases.get(biasName)`
immediately after `biases.set(...)` and compares it to the requested value.
On a mismatch it logs a message naming both the requested and actual value
(explaining it's a firmware-level clamp tighter than the SDK-reported allowed
range) and calls `pProp->Set()` so the property reflects the true hardware
value right away instead of reverting later with no explanation.

**Verified**: rebuilt and reran the ad hoc repro against the connected
IMX636 -- setting `bias_refr` to 255 now immediately reads back 235 *and*
logs `"bias bias_refr was set to 255 but the hardware clamped it to 235
(a firmware-level safety limit...)"` in the same call, instead of silently
returning 235 later. Reran the full `tools/test_prophebs.py` suite: Goals 1-6
and the bias round-trip check (Goal 5) pass. One **pre-existing, unrelated**
failure surfaced on both runs -- Goal 7's hot-pixel masking-effectiveness
check (`masked_region_max == 0` in the fixed `(0-6,0-6)` corner region)
fails because that run's real-hardware calibration happened to mask hot
pixels elsewhere on the sensor, not in that fixed corner; not touched by
this fix and not investigated further here.

### Bug fix: `TransposeCorrection`/`TransposeMirrorX`/`TransposeMirrorY`/`TransposeXY` had no effect, plus three small follow-up requests

User reported the standard MM image-orientation properties (created
automatically by `MM::CCameraBase`) appeared non-functional -- setting
`TransposeMirrorX`/`-Y` or `TransposeXY` didn't visibly change the Live
image. Confirmed by reading MMCore itself: it has **no** "Transpose"
handling at all (removed from this MMCore version, if it ever had it) --
each camera adapter is responsible for applying its own pixel transform
from these properties, matching how e.g. the reference ABS adapter does it
(gated on `TransposeCorrection` as a master switch). This adapter created
the properties (for free, via the base class) but never read them anywhere.

**Fix**: new `CProphEBSCamera::ApplyTranspose(ImgBuffer* target, natural,
srcW, srcH)` reads the four properties (`TransposeCorrection` gates
Mirror/SwapXY, same convention as ABS), resizes `target` to the swapped
width/height if `TransposeXY` is on, and writes the mirrored/transposed
result. Called from `BuildAndSwapFrame()` (targeting `backImg_`, on every
frame while a real camera streams) and from `GenerateTestImage()`/
`ApplyRoiToBuffers()`'s no-camera fallback checkerboard (targeting
`frontImg_`) -- the no-camera path only re-renders on `Initialize()` or a
ROI/binning change, since there's no periodic frame-builder thread without
real hardware to pick up a bare property change on its own.

Three small follow-up naming/default requests bundled into the same session:

- `EBS-ViewTimeDecayTimeConstant-us` renamed to
  `EBS-ViewModeTimeDecay_DecayTime_Constant-us` (string constant only, no
  behavior change).
- `EBS-ViewOffset`'s default changed from 10 to 100.
- `EBS-ViewScale` removed entirely (scale is now always 1, i.e.
  `pixel = clamp(EBS-ViewOffset + raw, 0, 255)`) -- per the user, an
  unneeded extra control.

**Verified**: rebuilt (0 errors) and reran the full `tools/test_prophebs.py`
suite against the connected IMX636 -- `SUCCESS`, including a new check that
`TransposeXY` actually swaps the reported image shape live (`(720, 1280)` ->
`(1280, 720)`) while streaming from real hardware, and (no-camera-only)
pixel-level checks that `TransposeMirrorX`/`-Y`/`TransposeXY` produce exactly
`np.fliplr`/`np.flipud`/`.T` of the baseline image against the deterministic
fallback test pattern, and are a no-op while `TransposeCorrection` is Off.
`EBS-ViewOffset`'s new default (100) and the renamed decay-constant property
were also asserted directly. `docs/BUILD_AND_USAGE.md` updated to match (new
default, formula without scale, a short note on the Transpose fix and its
no-camera caveat). Not yet verified: a real MicroManager Studio GUI
walkthrough of Live view actually looking mirrored/transposed to the eye --
same hand-off pattern as every prior goal.

### Bug fix: MDA (finite sequence) `Interval` didn't produce complete, back-to-back integration windows

User reported that a Multi-D Acquisition with `Exposure` == `Interval` (e.g.
both 100 ms) didn't behave like a normal camera at 0 ms interval -- it should
give `n` complete, non-overlapping 100 ms integration windows stitched
back-to-back, spanning the full requested duration (e.g. 100 frames -> ~10 s).
It didn't.

**Root cause**: `ProphEBSSequenceThread::svc()`'s finite (MDA) path just
called `InsertImage()` (which reads `frontImg_`, the Live-view buffer
continuously rebuilt by `ProphEBSFrameBuilderThread` on its own
`EBS-ViewDisplayRefreshMs` cadence -- 1 ms by default) and then slept
`Interval` ms, over and over. That has no relationship to when a real
integration window actually closes -- windows close on real event
sensor-timestamps in `OnEventsCD()`, or the `g_IdleWindowTimeoutMs` idle
timeout in `BuildAndSwapFrame()`, both independent clocks from the sequence
thread's wall-clock polling. So the "frame" grabbed on each MDA poll could be
anywhere from a just-reset near-empty window to one about to close, entirely
unrelated to `Exposure`/`Interval` -- confirmed this is what "not doing full
10 seconds" actually meant: not a duration bug, a sampling-phase bug (each of
the *n* frames wasn't a real, complete Exposure-length integration).

**Fix**: `CProphEBSCamera::CloseCurrentWindowLocked()` now snapshots the
about-to-close window's per-pixel ON/OFF counts into new
`completedOnCounts_`/`completedOffCounts_` members *before* zeroing them for
the next window (mirroring the existing touched-indices-only reset trick, so
this stays cheap), and bumps a new `completedWindowGeneration_` atomic.
`ProphEBSSequenceThread::svc()`'s finite path (only when a real camera is
`streaming_` -- the no-camera fallback has no window concept and is
untouched, still just sleeps `Interval` as before) now waits for that
generation counter to advance (i.e. a real window to close), renders exactly
that completed window via a new shared `CProphEBSCamera::RenderCountsToImage()`
helper (extracted from `BuildAndSwapFrame()` so both Live view and MDA share
the identical `EBS-ViewMode`/`-Offset`/ROI/binning/Transpose* rendering path)
into a new dedicated `mdaImg_` buffer, then calls a new `InsertMdaImage()`
(same as `InsertImage()` but reads `mdaImg_`). Spacing: `Interval <= Exposure`
(including 0) captures every window back-to-back as fast as they close --
real windows never overlap by construction, so this is the correct analogue
of a normal camera's 0 ms interval. `Interval > Exposure` sleeps the
remaining `Interval - Exposure` dead time after each window before waiting
for the next one, matching a normal camera's interval semantics.
`g_IdleWindowTimeoutMs` (100 ms) bounds the wait even on a fully idle sensor;
a 5 s hard timeout is a last-resort safety net so a stuck MDA can still be
cancelled.

**Verified** against the connected IMX636: rebuilt (0 errors), ran the full
`tools/test_prophebs.py` suite (`SUCCESS`, once the pre-existing hot-pixel
masking-effectiveness check tripped its long-documented flake again on this
run too -- see below), plus a new dedicated check (added to `test_prophebs.py`) using each
frame's own `Elapsed-Time-ms` metadata tag (assigned at `InsertImage()`
time from real wall-clock timestamps, independent of the fix) to measure
actual inter-frame spacing:
- `Exposure` = `Interval` = 100 ms, 10 frames: mean inter-frame delta ~99-137 ms
  across runs (target ~100 ms), all 10 frames distinct (no duplicate/stale
  content).
- `Interval` = 0, `Exposure` = 100 ms, 10 frames: mean delta ~101 ms -- every
  window captured back-to-back as fast as they close, not overlapping
  duplicates.
- `Interval` = 200 ms, `Exposure` = 50 ms, 10 frames: mean delta ~185-198 ms
  (target ~200 ms) -- genuine dead time between frames now honored.

Not yet verified: a real MicroManager Studio MDA dialog walkthrough (this was
checked via `pymmcore-plus`'s `startSequenceAcquisition()`/metadata directly,
not the GUI) -- same hand-off pattern as every prior goal.

Per user request, removed the Goal 7 hot-pixel masking-effectiveness check
(and its now-unused setup/restore code) from `test_prophebs.py` entirely,
rather than continuing to carry it as a documented flake -- it fired again
on this very session's first run. Its actual defect: it required a fixed
`(0-6,0-6)` probe region to happen to contain a real hot pixel that
calibration also flagged, which isn't guaranteed run-to-run (calibration
finds hot pixels wherever real sensor/ambient activity puts them, not
necessarily that corner); the assertion then failed on ordinary "no hot
pixel there this time" runs, not on any real masking defect. Everything else
Goal 7 covers (`EBS-HotPixelBlockedPixels` round-trip, malformed-value
rejection, calibration-replaces-not-merges, ROI-scoped calibration bounds,
truncation-safety cap) is untouched and still exercised. Reran the full
suite immediately after: `SUCCESS`, cleanly, no flake.

### Follow-up: two more MDA-interval bugs found via real-hardware timing, both pre-existing (one exposed, one worsened, by the fix above)

User reported that `Exposure=110ms`/`Interval=100ms` (`Exposure` slightly
*above* `Interval`) recorded noticeably *less* than the expected ~10 s
minimum for a 100-frame MDA (~8.8 s, later confirmed against the raw event
data too), while `Exposure=20ms` landed close to 10 s and `Exposure=300ms`
landed close to 10 s as well -- the last of which the user called "expected,"
but is actually a second symptom of the same underlying bug (see below), not
a confirmation that things were working.

**Bug 1 -- the "idle timeout" was an accidental hard cap, not an idle check.**
`BuildAndSwapFrame()`'s idle-window force-close compared elapsed wall time
since the last close against the bare `g_IdleWindowTimeoutMs` constant
(100 ms), completely independent of the configured `Exposure`. So any
`Exposure > 100 ms` was silently force-closed at ~100 ms of real integration
on a busy sensor -- not a "quiet scene" case at all, just an artificial cap --
and, per `CloseCurrentWindowLocked()`'s existing idle-path comment, the
forced close leaves `windowStartT_` stale, so the very next real event
immediately triggered a second, near-zero-length close right behind it. That
double-close corrupted the new `completedWindowGeneration_` sync from the
fix above (some generation ticks = a real ~100 ms-capped window, some =
a spurious near-instant duplicate), producing the irregular, sometimes-
shorter-than-expected cadence the user measured at `Exposure=110ms`. It also
explains why `Exposure=300ms` looked "expected" at ~10 s: it was actually
being silently truncated to ~100 ms of real integration per frame the whole
time (mislabeled short exposures), which coincidentally matched the
`Interval=100ms` duration -- not genuinely honoring the requested 300 ms
exposure. **Fix**: compare against `max(g_IdleWindowTimeoutMs,
integrationTimeMs_.load())` instead, so the idle path only ever fires once
nothing has closed the window in at least a full configured `Exposure` --
restoring it to an actual idle safety net. This is a real behavior change:
`Exposure > Interval` now correctly takes proportionally longer per frame
(can't produce a genuine complete window faster than the exposure itself),
not the old coincidentally-matching-`Interval` duration.

**Bug 2 -- the completed-window snapshot from the fix above was O(events in
the window), not O(1), and tripled that cost.** `touchedIndices_` has no
dedup -- one push per *qualifying event*, not per distinct pixel -- so a
100-300 ms window on this sensor's ~10 MEv/s ambient activity can push
millions of entries. The first version of `CloseCurrentWindowLocked()`'s
snapshot logic iterated it three times (clear the previous snapshot's
entries, copy the new ones across, then the original reset loop), so closing
a window started costing a large, `Exposure`-scaling chunk of the window's
own wall-clock duration -- on the very same thread/lock `OnEventsCD()` uses
to process incoming events. Measured directly: `Exposure=110/300ms` MDA runs
took ~1.6-2.8x longer per frame than configured after Bug 1's fix alone (a
real, additional regression from the naive completed-snapshot design, not
present before this whole MDA-interval effort started). **Fix**: swap the
whole `onCounts_`/`offCounts_`/`touchedIndices_` vectors with
`completedOnCounts_`/`completedOffCounts_`/`completedTouchedIndices_`
instead of copying element-by-element -- `std::vector::swap()` just
exchanges internal pointers, O(1) regardless of how large the touched list
has grown. After the swap the "completed" slot holds exactly the just-closed
window (correct), while the recycled `onCounts_`/`offCounts_` carry along
their own now-relevant `touchedIndices_` list, so the existing reset loop
(unchanged, same cost as before any of this work began) still only clears
real touched entries, never sensor-sized.

**Verified** against the connected IMX636 (rebuilt, 0 errors, both fixes
together) with a dedicated ad hoc script measuring real inter-frame
`Elapsed-Time-ms` deltas across 20-frame MDAs at `Interval=100ms`:
`Exposure=20ms` -> mean delta ~112 ms (interval-dominated, as expected);
`Exposure=110ms` -> mean delta ~103 ms, individual deltas ranging ~58-127 ms
(some jitter below the 110 ms floor remains -- checked `EBS-AvgBacklogFlushCount`/
`EBS-AvgCallbackLagMs` during a run and both stayed ~0, ruling out backlog
catch-up as the cause; most likely ordinary thread-scheduling/render-cost
jitter around the 1 ms polling granularity, self-correcting on average, not
a systematic bias); `Exposure=300ms` -> mean delta ~299 ms, tightly
clustered (253-335 ms) -- now correctly reflecting the real configured
exposure instead of the old ~100 ms cap. Reran the full `tools/test_prophebs.py`
suite (including the `Bug fix (MDA interval)` section added earlier):
`SUCCESS`, no flakes. Not yet verified: a real MDA run long enough (tens of
seconds+) to see whether the remaining ~jitter ever compounds into a
noticeably-off total duration, or a MicroManager Studio GUI walkthrough --
same hand-off pattern as every prior goal.

### Bug fix: raw recording never started for `Exposure < Interval` MDAs (real MM Studio GUI, not the adapter itself)

User reported raw recording (`EBS-RawRecordingStatus`) stuck on
`"Not recording"` for the whole run whenever their MDA's `Exposure` was
*below* `Interval` (e.g. 20 ms/100 ms), even though `Exposure == Interval`
(e.g. 100 ms/100 ms) worked fine. Direct `pymmcore-plus` calls to
`startSequenceAcquisition()` showed recording working correctly for *both*
combinations, ruling out a defect in `StartRawRecordingIfRequested()`/
`StopRawRecordingIfActive()` themselves (untouched by any of this session's
other fixes). The user's own CoreLog from a real MM Studio MDA settled it:
for `Exposure=20ms/Interval=100ms`, `"Running acquisition with Clojure
AcqEng"` produced 100 images with **no** `"ProphEBS: raw event recording
started"` line at all; for `Exposure=Interval=100ms`, the same engine *did*
log `"streaming directly to the Multi-D Acquisition save location"` and
`"raw event recording started"`/`"stopped"`.

**Root cause**: `StartRawRecordingIfRequested()` is only ever called from
inside our `StartSequenceAcquisition()`. MM Studio's Acquisition Engine only
calls that (the hardware sequence-acquisition path) when `Interval <=
Exposure` ("as fast as possible," no real dead time to simulate); once
`Interval > Exposure`, it drives the camera with its own software-timed loop
of plain `SnapImage()` calls instead. A camera adapter has no MMCore-level
signal at all that a `SnapImage()` call is part of an MDA rather than a
one-off manual Snap -- confirmed by grepping the vendored `MMCore` sources
for any "acquisition in progress" hook a device could query; none exists.
This is a pre-existing MM Studio behavior, not a regression from any of this
session's earlier fixes -- it was simply never exercised/noticed before this
session's broader Exposure/Interval testing.

**Fix**: since Live view never calls `SnapImage()` in a loop (it always goes
through `StartSequenceAcquisition()`, unbounded), a burst of closely-spaced
`SnapImage()` calls is a reliable signal that this is a snap-timed MDA, not
incidental Live/manual-Snap use. New `MaybeHandleSnapBurstRecording()`
(called from `SnapImage()`) tracks the wall-clock gap since the previous
snap (`lastSnapWallTime_`/`hasLastSnapTime_`, guarded by new
`snapBurstLock_`); the moment a snap arrives within a new
`EBS-RawRecordSnapBurstGapMs` property (`snapBurstGapMs_`, default 3000 ms)
of the previous one, it starts recording by calling the existing,
unmodified `StartRawRecordingIfRequested()` directly -- reusing its
MDA-folder auto-discovery, `EBS-RawFilePath` override, and status reporting
as-is. Recording necessarily starts from the *second* snap of a burst
onward (there is no way to know in advance that a lone first snap will turn
into one), so a snap-timed MDA's raw file is missing a sliver of its very
first frame's data -- an inherent limitation of detecting a pattern after
the fact, not something a heuristic can close. `UpdateStats()` (already
ticking every `g_StatsIntervalMs` on `ProphEBSStatsThread`) finalizes the
recording once the gap since the last snap exceeds the same threshold again
-- piggybacking on that existing timer rather than adding a new one. New
`snapBurstRecordingActive_` flag scopes the idle-check to recordings this
heuristic itself started, so it never interferes with a real
`StartSequenceAcquisition()`-driven recording (guarded additionally by
`MaybeHandleSnapBurstRecording()` no-op'ing whenever `IsCapturing()`).

**Verified** against the connected IMX636 (rebuilt, 0 errors) with a
dedicated ad hoc script: a single isolated `snapImage()` call correctly left
`EBS-RawRecordingStatus` at `"Not recording"` even a full second later (no
false-positive on ordinary manual Snap use); a simulated snap-timed MDA (15
snaps at 100 ms spacing, gap threshold temporarily lowered to 500 ms for a
fast test) showed `"Not recording"` after snap 1, `"Recording to ..."` from
snap 2 through snap 15, and `"Finished: ..."` within ~1 s of the burst
ending -- landing in the same MDA folder as MM's own TIFF stack, producing a
real 50 MB `.raw` file plus its matching `.bias` file. Reran the full
`tools/test_prophebs.py` suite: `SUCCESS`, no flakes, no regressions. Not
yet verified: a real MM Studio MDA with `Exposure < Interval` end-to-end
(this was checked via a `pymmcore-plus` simulation of the snap pattern, not
an actual MM Studio run).

### Follow-up: HDF5 recording toggle

Metavision SDK supports recording to either `.raw` or `.hdf5` -- confirmed
by reading `Camera::start_recording()`'s header doc in
`metavision/sdk/stream/camera.h`: it "enables writing to supported formats
other than RAW file" and, per `metavision_file_cutter`'s sample code,
selects the writer purely from the output path's *extension*. So no new
Metavision API surface was needed -- just controlling which extension this
adapter's own path-building uses.

New `EBS-RawRecordingFormat` property (`RAW`/`HDF5` dropdown, **defaults to
HDF5** per the user's request) controls the extension used by
`GenerateAutoRawFilePath()` and `ComputeNumberedMdaDestination()` (now takes
an `extension` parameter) for auto-generated/MDA-discovered paths, plus the
flat-fallback naming in `StopRawRecordingIfActive()`. Has no effect when
`EBS-RawFilePath` is set explicitly -- that path's own extension was already
honored verbatim (a manual `.hdf5` path already worked before this
property existed; this only affects the paths this adapter builds itself).
New `GetRecordingFileExtension()` helper reads the property once and
returns `.raw` or `.hdf5`.

**Bug fix found while testing this**: `tools/test_prophebs.py`'s Goal 4a
check (auto-discovered path) failed intermittently -- the file the test
expected didn't exist at the path `EBS-RawRecordingStatus` reported. Root
cause, found via `enableStderrLog(True)`/`enableDebugLog(True)` and
CoreLog timestamps: `StopRawRecordingIfActive()` is called unconditionally
from several unrelated places (Live-view's `StopSequenceAcquisition()`,
`Shutdown()`, a finite sequence's own natural completion), any of which can
stop a recording that Goal 4's `MaybeHandleSnapBurstRecording()` snap-burst
heuristic actually started. Previously, `snapBurstRecordingActive_` was
only cleared inside `UpdateStats()`'s own gap-check -- so if one of those
*other* paths stopped the recording first, the flag stayed stuck `true`.
Minutes later, `UpdateStats()` would find it still set, conclude the
snap-burst had gone idle, and issue a second, spurious
`StopRawRecordingIfActive()` call -- except by then a completely unrelated
recording (e.g. the next finite MDA) was the one actually active, and that
one got finalized early instead. This is orthogonal to RAW vs HDF5 (would
reproduce identically on `main` with `.raw`) and only became visible now
because a leftover real MM Studio profile on this dev machine
(`C:\Data\EBSMMTest7`, `save: true`) gives `TryDiscoverMdaRootPrefix()` a
real destination to race over. **Fix**: `StopRawRecordingIfActive()` now
clears `snapBurstRecordingActive_` itself, unconditionally, every time it
runs -- after any stop, by definition no recording is active, regardless of
which mechanism started it, so the flag can never outlive the thing it was
tracking.

**Verified** against the connected IMX636 (rebuilt, 0 errors): a dedicated
ad hoc script confirmed `Camera::start_recording()` produces a valid,
non-empty `.hdf5` file for an explicit path, for the MDA flat-fallback path,
and for the `GenerateAutoRawFilePath()` staging path followed by a move to
a discovered MDA folder. Extended `tools/test_prophebs.py` (Goal 4 section)
to assert `EBS-RawRecordingFormat` defaults to `HDF5` with allowed values
`{RAW, HDF5}`, that an auto-discovered recording under the default produces
a `.hdf5` file, and that switching to `RAW` mid-session produces a `.raw`
file instead. Reran the full suite: `SUCCESS`, no flakes, no regressions.
