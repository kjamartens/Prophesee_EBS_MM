# Prophesee EBS MicroManager Device Adapter

A [Micro-Manager 2.0](https://micro-manager.org/) camera device adapter for
[Prophesee Metavision](https://www.prophesee.ai/) event-based sensors (EBS),
letting an EBS camera be controlled and acquired from Micro-Manager like any
other scientific camera — useful for putting an event-based sensor on a
microscope.

Event-based sensors report a stream of per-pixel brightness-change events
rather than frames, so the adapter integrates events over a configurable
window into a conventional 2D image for Micro-Manager's frame-based
acquisition pipeline, while also exposing the sensor's own hardware
properties (biases, filters, triggers, sync mode, etc.) as standard
Micro-Manager device properties.

## Getting started

Most users don't need to build anything — grab a prebuilt release:

1. Go to [Releases](../../releases) and download the latest `.zip`.
2. Extract it, and copy `mmgr_dal_ProphEBS.dll` plus every
   `ProphEBS_Backend_SDK*.dll` into your Micro-Manager install folder (the
   same folder as `ImageJ.exe`/`MicroManager.exe`).
3. Install the [Prophesee Metavision SDK](https://docs.prophesee.ai/stable/get_started/index.html)
   (see Requirements below) if you haven't already — needed to actually run
   against an EBS camera.
4. Launch Micro-Manager, open **Tools → Hardware Configuration Wizard**, and
   add the **ProphEBS-Camera** device.

Building from source is only needed if you want to modify the adapter — see
[Quick start](#quick-start) below.

## Features

- Camera device adapter (`mmgr_dal_ProphEBS.dll`) implementing Micro-Manager's
  `CCameraBase` interface, with a graceful no-hardware fallback (a static test
  pattern) when no EBS is connected.
- EBS connection/identification (model, serial, connection type, sensor
  generation).
- Event-to-frame integration with a configurable window (down to
  microsecond resolution via the standard `Exposure` property) and several
  view modes (Merged, OnOnly, OffOnly, NetSigned, time-decay).
- Recording to `.raw`/`.hdf5` via Micro-Manager's normal multi-dimensional
  acquisition, including automatic path discovery from Micro-Manager's own
  save settings.
- The sensor's configurable properties exposed as `EBS-*` device properties:
  biases, event-rate control (ERC), event-trail (STC) and event-rate
  band-pass filters, anti-flicker filtering, hardware trigger in/out,
  multi-camera sync mode, and live statistics (data rate, event rate,
  temperature, illumination).
- Hot-pixel handling: per-pixel blocking and on-demand automatic hot-pixel
  calibration.
- Hardware ROI and real spatial binning (2x2/4x4), both integrated with
  Micro-Manager's standard ROI/Binning UI.
- A version-based backend-shim architecture (`DeviceAdapter/ProphEBS/Backend/`)
  so the same adapter DLL runs against multiple Metavision SDK generations
  (4.3.0, 5.0.0, 5.1.0, 5.1.1), auto-detected at runtime.

## Requirements

- Windows, Visual Studio 2022 with the "Desktop development with C++"
  workload.
- A Micro-Manager 2.0 **nightly** build whose device interface version
  matches this repository's `third_party/mmCoreAndDevices` submodule (see the
  tutorial below — stable releases are generally too old).
- [Prophesee Metavision SDK](https://docs.prophesee.ai/stable/get_started/index.html)
  4.3.0, 5.0.0, 5.1.0, or 5.1.1 installed, to run the adapter against real
  hardware. **This is commercial, separately-licensed software** — this
  repository does not include or redistribute it. An EBS camera is only
  needed to test an actual sensor connection; the adapter builds and loads
  without one.

## Quick start

```
git clone --recurse-submodules <this-repo-url>
cd Prophesee_EBS_MM
```

1. Install the Metavision SDK (see Requirements) and set up a compatible
   Micro-Manager install — the easiest way is via `pymmcore-plus`:
   ```
   python -m venv tools/mm_python_env
   tools\mm_python_env\Scripts\pip install pymmcore-plus
   tools\mm_python_env\Scripts\mmcore install
   ```
2. Open `DeviceAdapter\ProphEBS\ProphEBS.sln` in Visual Studio, set the
   configuration to **Release** / **x64**, and Build Solution.
3. Copy `mmgr_dal_ProphEBS.dll` and every built `ProphEBS_Backend_SDK*.dll`
   from `DeviceAdapter\ProphEBS\build\Release\x64\` into your Micro-Manager
   install folder (`mmcore list` prints the active install's path).
4. Launch Micro-Manager, open **Tools → Hardware Configuration Wizard**, and
   add the **ProphEBS-Camera** device.

This is the condensed version — for a full walkthrough that assumes no prior
C++ or Micro-Manager experience (including troubleshooting common build
errors, and options for machines without admin rights), see
[docs/BUILD_AND_USAGE.md](docs/BUILD_AND_USAGE.md).

## Repository layout

- `DeviceAdapter/ProphEBS/` — the device adapter source and Visual Studio
  solution.
- `third_party/mmCoreAndDevices/` — Micro-Manager's own core/device-API
  source, included as a git submodule.
- `tools/` — a Python self-test script (`test_prophebs.py`) exercising the
  adapter through `pymmcore-plus`.
- `docs/` — build/usage tutorial, development log, and the original project
  brief.

## License

This project is licensed under the BSD 3-Clause License — see
[LICENSE](LICENSE). The `third_party/mmCoreAndDevices` submodule is
separate, third-party code under its own license.
