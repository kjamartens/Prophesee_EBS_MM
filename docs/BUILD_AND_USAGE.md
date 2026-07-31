# Build & Usage Tutorial

This tutorial assumes no prior experience building C++ software or using
MicroManager. Follow it top to bottom the first time. Goal 1 setup (below)
still applies to Goal 2 — Goal 2 only adds one extra software install
(section 1d) and new things to check in MicroManager (section 8).

## Goal 1 (Barebones Device Adapter)

## What you're building

A MicroManager "camera" that isn't connected to any real hardware yet. When
you add it in MicroManager and click **Live** or **Snap**, you'll see a
checkerboard test pattern, and MicroManager's log will contain a message
confirming the adapter initialized. This proves the whole chain — writing
C++ against the MicroManager device API, compiling it into a DLL, and
loading it in MicroManager — works before we connect any Prophesee hardware.

## 1. Software you need to install

### 1a. Git
If `git --version` in a terminal doesn't print a version, install Git from
https://git-scm.com/download/win (default options are fine).

### 1b. Visual Studio 2022 (Community edition is free)
Download from https://visualstudio.microsoft.com/vs/community/.
When the installer opens, you need to pick a **workload** — this is the
screen with big tiles like ".NET desktop development", "Web development", etc.

- Check the box for **"Desktop development with C++"**.
- On the right-hand "Installation details" panel, make sure these are checked
  (they usually are by default when you pick the workload above):
  - **MSVC v143 - VS 2022 C++ x64/x86 build tools** (the actual compiler)
  - **Windows 10 SDK** or **Windows 11 SDK** (whichever matches your OS)
- Click **Install**. This can take 20–40 minutes and several GB of disk space.

You do **not** need any other workload (no Python, no Node.js, etc.) for this
project.

### 1c. MicroManager 2.0

**Important gotcha:** every MicroManager release and every device adapter
DLL (including ours) is built against a numbered **device interface
version**. MMCore silently refuses to load any adapter DLL whose interface
version doesn't match exactly — it shows up in the Hardware Configuration
Wizard as an entry with **"(unavailable)"** next to it instead of an
expandable list of devices, with no further explanation in the GUI. If that
happens to you, it almost always means your installed MicroManager build is
older or newer than the adapter DLL. This project's `third_party/mmCoreAndDevices`
submodule tracks the bleeding-edge `main` branch, so you need a reasonably
recent MicroManager **nightly** build, not an old stable release.

There are two ways to get MicroManager. Use **Option A** unless you have a
specific reason not to — it makes checking/updating your interface version a
one-line command instead of a guessing game.

#### Option A (recommended): install/manage MicroManager via `pymmcore-plus`

`pymmcore-plus` is a Python package that can download and manage MicroManager
nightly builds for you, and tells you directly whether a given install's
device interface version is compatible.

1. Install Python 3.10+ if you don't have it: <https://www.python.org/downloads/>
   (during install, check **"Add python.exe to PATH"**).
2. Open a terminal in this repository and create a small, dedicated virtual
   environment just for this tool (keeps it isolated from any other Python
   projects you have):
   ```
   python -m venv tools/mm_python_env
   tools\mm_python_env\Scripts\pip install pymmcore-plus
   ```
3. Check what MicroManager device interface version your build currently
   needs, and what (if anything) is already installed:
   ```
   tools\mm_python_env\Scripts\mmcore list
   ```
   This prints the required interface version and lists any installs found,
   marking each **compatible** or **incompatible**.
4. If you don't have a compatible install yet, fetch the latest one:
   ```
   tools\mm_python_env\Scripts\mmcore install
   ```
   This downloads the current nightly installer and (if you have admin
   rights) installs it silently into
   `%LOCALAPPDATA%\pymmcore-plus\pymmcore-plus\mm\Micro-Manager_<version>_<date>\`.
   Run `mmcore list` again afterwards to confirm it now shows `(active)`
   next to that install.

   **If you don't have admin rights** (the installer will pop up a Windows
   UAC prompt asking for admin, and `mmcore install` will fail with a
   `CalledProcessError` if you can't approve it), see the
   [No-admin-rights installer workaround](#no-admin-rights-installer-workaround)
   section below — you can still get a working install without any admin
   access at all.
5. To actually see the MicroManager/ImageJ GUI (for the Hardware
   Configuration Wizard steps below), run:
   ```
   tools\mm_python_env\Scripts\mmcore mmstudio
   ```
   This launches `ImageJ.exe` from the managed install folder.

#### Option B: the standard installer (needs admin rights)

Download the Windows installer from <https://micro-manager.org/downloads/> —
pick a **nightly build**, not the old stable 2.0.3 release, so its device
interface version has a chance of matching this project's submodule — and
run it (needs admin rights to install; default install location is fine,
typically `C:\Program Files\Micro-Manager-2.0`). You should be able to launch
`ImageJ.exe` (or `MicroManager.exe`, depending on the installer version) from
that folder and see the MicroManager window with an ImageJ window behind it.

#### No-admin-rights installer workaround

MicroManager's nightly installer is built with an older Inno Setup that
*requires* admin privileges to run — there is no command-line flag to skip
that. But since it's a standard Inno Setup installer, you can extract its
contents directly (without ever running its installer logic, so no
elevation is needed at all) using the free `innounp` tool.

1. Download `innounp` from <https://sourceforge.net/projects/innounp/> (or, if
   you use the [Scoop](https://scoop.sh/) package manager: `scoop install innounp`).
2. Download the installer .exe manually from
   <https://download.micro-manager.org/latest/windows/MMSetup_x64_latest.exe>
   (don't run it — just save it, e.g. to your Downloads folder).
3. Extract it into a temporary folder:
   ```
   mkdir extracted
   cd extracted
   innounp -x -y "..\MMSetup_x64_latest.exe"
   ```
4. This creates a folder named `{app}` inside `extracted\` containing the
   full MicroManager application (ImageJ.exe, all `mmgr_dal_*.dll` files,
   the bundled Java runtime, etc.) — exactly what the installer would have
   placed on disk, just without needing admin rights to get it there.
5. Copy the **contents** of that `{app}` folder into a new folder named
   `Micro-Manager_<version>_<date>` (matching the naming `mmcore install`
   uses) under
   `%LOCALAPPDATA%\pymmcore-plus\pymmcore-plus\mm\`. Create the parent
   folders if they don't exist yet.
6. Run `tools\mm_python_env\Scripts\mmcore list` again — your manually
   extracted copy should now show up, hopefully marked `(active)` if its
   interface version matches.

### 1d. Prophesee Metavision SDK (needed from Goal 2 onward)

Goal 2 adds a dependency on Prophesee's own **Metavision SDK** (the software
that talks to the EBS hardware). If you're only building Goal 1, you can
skip this. From Goal 2 onward, the adapter won't build without it.

1. Download and run the Metavision SDK installer from
   <https://docs.prophesee.ai/stable/get_started/index.html> (the "Installation"
   page links to the current Windows installer). Use the default install
   location, **`C:\Program Files\Prophesee`** — the project's build files
   assume this path unless you override it (see below).
2. The installer adds `C:\Program Files\Prophesee\bin` to your system `PATH`.
   This matters twice: once at build time (not directly — the `.vcxproj`
   references the install folder explicitly) and once at *run* time, because
   `mmgr_dal_ProphEBS.dll` depends on `metavision_hal.dll`,
   `metavision_sdk_base.dll`, and `metavision_sdk_stream.dll`, which
   MicroManager needs to be able to find when it loads our adapter. If you
   installed to a non-default location, or `PATH` didn't get updated, add
   `<your-install-dir>\bin` to your `PATH` manually (Windows Settings → search
   "environment variables" → **Edit environment variables for your account**
   → `Path` → **New**), then log out/in (or restart your terminal) for it to
   take effect.
3. **If you installed Metavision somewhere other than
   `C:\Program Files\Prophesee`**, pass its location to MSBuild when building
   (Build → Configuration Manager won't have this option — use a terminal
   build instead):
   ```
   MSBuild ProphEBS.sln /p:Configuration=Release /p:Platform=x64 /p:MetavisionSdkRoot="D:\Prophesee"
   ```
   (Building from inside the Visual Studio IDE always uses the default
   `C:\Program Files\Prophesee` path.)
4. You do **not** need an EBS camera physically connected to build or load
   the adapter — Goal 2's connection attempt is designed to fail gracefully
   (see the Goal 2 section below) so you can develop and test without
   hardware. You only need the camera plugged in to verify the *connection*
   itself works.

## 2. Get the source code

Open a terminal (PowerShell is fine) and run:

```
git clone --recurse-submodules https://github.com/<your-org>/Prophesee_EBS_MM.git
cd Prophesee_EBS_MM
```

The `--recurse-submodules` flag is important — this repository references
Micro-Manager's own source code (`third_party/mmCoreAndDevices`) as a
"submodule" (a pointer to another Git repository at a specific commit).
Without that flag, that folder will be empty and the build will fail.

**If you already cloned without that flag**, fix it with:
```
git submodule update --init --recursive
```

## 3. Build the adapter

1. In File Explorer, navigate to `DeviceAdapter\ProphEBS\` inside the cloned
   repository.
2. Double-click **`ProphEBS.sln`**. This opens Visual Studio with two
   projects loaded: `ProphEBS` (our adapter) and `MMDevice-SharedRuntime`
   (Micro-Manager's own support library, which our adapter needs).
3. Near the top of the Visual Studio window there are two dropdowns that
   default to "Debug" and "x64" (or sometimes "Any CPU"). Set them to:
   - **Release**
   - **x64**
   (MicroManager itself is a 64-bit application, so the adapter DLL must
   also be built for x64 — Win32/x86 will not load.)
4. Menu bar → **Build → Build Solution** (or press `Ctrl+Shift+B`).
5. Watch the **Output** window (usually docked at the bottom). A successful
   build ends with something like:
   ```
   ProphEBS.vcxproj -> ...\DeviceAdapter\ProphEBS\build\Release\x64\mmgr_dal_ProphEBS.dll
   ========== Build: 2 succeeded, 0 failed ==========
   ```
6. The file you need is:
   `DeviceAdapter\ProphEBS\build\Release\x64\mmgr_dal_ProphEBS.dll`

### Common first-build errors

| Error | Cause | Fix |
|---|---|---|
| `Cannot open include file: 'DeviceBase.h'` | Submodule wasn't cloned | Run `git submodule update --init --recursive` from the repo root, then reload the solution |
| `TRACKER : error TRK0005: Failed to locate: "CL.exe"` | The C++ build tools component of Visual Studio isn't installed, or you launched a plain terminal instead of the IDE | Re-run the VS Installer and confirm "Desktop development with C++" is checked; if building from a terminal instead of the IDE, use the **"Developer Command Prompt for VS 2022"** (search for it in the Start menu) rather than a plain PowerShell/cmd window |
| Solution won't load / "unsupported" project errors | Very old Visual Studio version | Make sure you installed **Visual Studio 2022** (not 2019 or earlier) |
| `Cannot open include file: 'metavision/sdk/stream/camera.h'` or `'opencv2/core.hpp'` (Goal 2+) | Metavision SDK isn't installed, or is installed somewhere other than `C:\Program Files\Prophesee` | Install it (section 1d) or pass `/p:MetavisionSdkRoot="<your path>"` to MSBuild |
| `unresolved external symbol` referencing `Metavision::...` at link time (Goal 2+) | Metavision `.lib` files not found/linked | Confirm `C:\Program Files\Prophesee\lib` (or your `MetavisionSdkRoot`) contains `metavision_hal.lib`, `metavision_sdk_base.lib`, `metavision_sdk_stream.lib` |
| Build succeeds but into a `Debug` folder instead of `Release` | Configuration dropdown was left on "Debug" | Switch the dropdown to Release and rebuild (Debug DLLs also work in MicroManager, they're just slower and bulkier — Release is recommended) |

## 4. Install the adapter into MicroManager

1. Copy `mmgr_dal_ProphEBS.dll` from the build output folder.
2. Paste it directly into your MicroManager installation folder — the same
   folder that contains `ImageJ.exe`/`MicroManager.exe` and the other
   `mmgr_dal_*.dll` files (e.g. `mmgr_dal_DemoCamera.dll`). Where that is
   depends on which install method you used in step 1c:
   - **Option A (pymmcore-plus)**: the active install's path is printed by
     `mmcore list` (marked `(active)`), e.g.
     `%LOCALAPPDATA%\pymmcore-plus\pymmcore-plus\mm\Micro-Manager_<version>_<date>\`
   - **Option B (standard installer)**: typically
     `C:\Program Files\Micro-Manager-2.0\`

## 5. Add the device in MicroManager

1. Launch MicroManager.
2. If it doesn't prompt automatically, open the **Hardware Configuration
   Wizard**: menu **Tools → Hardware Configuration Wizard**.
3. Choose **"Create new configuration"** → Next.
4. In the device list on the left, scroll to find **`ProphEBS`** — expand
   it and you'll see **`ProphEBS-Camera`**. Double-click it (or select it
   and click **Add**).
5. It will ask for a **Label** (name) for this device instance — the default
   is fine, click **OK**.
6. Click through the rest of the wizard (Next → Next → ... → Finish),
   accepting defaults. When asked to save the configuration file, save it
   anywhere memorable (e.g. `ProphEBS_test.cfg` on your Desktop) — you can
   reload it next time via **File → Load Hardware Configuration** instead of
   redoing the wizard.

## 6. Confirm the debug message appears

MicroManager writes a detailed log file (the "CoreLog") every time it runs.

- Easiest path: in the main MicroManager window, go to **Help → About**, or
  look for **"Show CoreLog"** in the Help/Tools menu — this opens the log
  file directly.
- Alternatively, find the log file on disk: it's created in the
  MicroManager installation folder (or `%USERPROFILE%\CoreLogs\` in some
  versions), named like `CoreLog<timestamp>.txt`. Open the most recent one
  in Notepad.
- Search for the line:
  ```
  ProphEBS adapter initialized (Goal 1 barebones - no EBS hardware connected yet)
  ```
  If you see it, the adapter loaded and ran its `Initialize()` method
  successfully.

## 7. See the static test image

1. In the main MicroManager window, make sure the camera dropdown at the top
   shows your `ProphEBS-Camera` device.
2. Click **Live** (or **Snap** for a single image). An ImageJ window should
   pop up showing a black/white checkerboard pattern with a horizontal
   brightness gradient. That image is hardcoded — it does not change over
   time, and it is not coming from any real camera.
3. If **Live** shows the image updating (even though it looks identical
   every time), that confirms MicroManager's continuous-acquisition path
   works too, not just single Snap.

If you see this image and the log message from step 6, **Goal 1 is
verified working** — let the project owner know so we can tag this as
`v0.1` and move on to Goal 2 (connecting to the real EBS hardware).

## Optional: verify without the GUI, via `tools/test_prophebs.py`

If you set up the `pymmcore-plus` Python environment in step 1c, there's a
faster way to sanity-check the adapter than going through the full GUI wizard
each time: `tools/test_prophebs.py` loads `ProphEBS-Camera` directly, snaps
one image, and prints its shape/dtype. Edit the `mm_dir` variable at the top
of the script to match your install path (from `mmcore list`), then run:

```
tools\mm_python_env\Scripts\python.exe tools\test_prophebs.py
```

A successful run prints `Snap OK. Image shape: (480, 640) dtype: uint8` and
`SUCCESS`. This is useful for quickly confirming a rebuild still works after
code changes, without reopening MicroManager each time — but it doesn't
replace actually checking Live/Snap in the real GUI at least once.

## Goal 2 (Connection to the EBS)

### What's new

The adapter now tries to connect to a real Prophesee EBS camera when it
initializes, using the Metavision SDK (see section 1d above for installing
it). Live/Snap still show the same Goal 1 static checkerboard — Goal 2 is
only about *detecting and identifying* the camera, not yet pulling real
image data from it (that's Goal 3).

### 8. Check the connection properties

1. Make sure your EBS camera is plugged in (USB) **before** you add/initialize
   the device in MicroManager — the connection attempt happens once, during
   `Initialize()`, which runs when the device is added via the Hardware
   Configuration Wizard (or when `pymmcore-plus`/similar calls
   `initializeDevice`). If you plug the camera in afterward, remove and
   re-add the device (or restart MicroManager) to retry.
2. In MicroManager, open the **Device Property Browser**: menu
   **Tools → Device/Property Browser**.
3. Find the row for your `ProphEBS-Camera` device and look for these
   properties:
   - **EBS-ConnectionStatus** — should read `Connected` if a camera was found.
   - **EBS-Model** — the sensor name and generation (e.g. `Gen4.1 (Gen 4.1)`).
   - **EBS-Serial** — the camera's serial number.
   - **EBS-ConnectionType** — e.g. `USB`.
   - **EBS-Integrator** — the hardware integrator name.

   These are all read-only (grayed out) — they report what the adapter found,
   you can't change them here.
4. **If `EBS-ConnectionStatus` instead reads something like
   `Not connected: ... Error 101001: Camera not found...`**, that means the
   Metavision SDK loaded fine but didn't detect a camera. Check, in order:
   - Is the camera actually plugged in and powered? Try unplugging/replugging
     the USB cable.
   - Does Prophesee's own tooling see it? Run
     `"C:\Program Files\Prophesee\bin\metavision_platform_info.exe"` (or
     similar bundled diagnostic tool — check `C:\Program Files\Prophesee\bin`
     for what's available) to confirm the camera is visible outside
     MicroManager entirely. If Prophesee's own tools don't see it either,
     this is a hardware/driver problem, not an adapter problem.
   - Re-add the device in MicroManager (Initialize only runs once per
     add/load) after fixing the above.
5. This message string is also written to the CoreLog (see step 6 above) —
   search for `ProphEBS:` to find both the "connected to ..." success line
   and the "no EBS camera connected (...)" fallback line.

If `EBS-ConnectionStatus` reads `Connected` with a real model/serial filled
in, **Goal 2 is verified working** — let the project owner know so we can
tag this as `v0.2` and move on to Goal 3 (minimal video feed from real
events).

## Goal 3 (Minimal video feed)

### What's new

With a real EBS connected, the adapter now streams actual events from the
camera instead of showing the static checkerboard: every 100 ms, the CD
(contrast-detection) events the sensor produced during that window are
integrated into a per-pixel count and rendered as an 8-bit grayscale image —
brighter pixels mean more events landed there recently. This runs
continuously in the background as long as the camera is connected, not just
while Live is open. With no camera connected, nothing changes from Goal 1/2
— you still get the static checkerboard.

### 9. Watch a live event feed

1. Make sure your EBS camera is plugged in **before** adding/initializing the
   device (same requirement as Goal 2, step 8.1).
2. In the main MicroManager window, select `ProphEBS-Camera` and click
   **Live**.
3. Wave a hand, move an object, or change the lighting in front of the
   sensor. You should see bright pixels appear and move in the ImageJ Live
   window, tracking whatever is moving/changing in the sensor's field of
   view. A perfectly static, unchanging scene will look mostly black — that
   is correct behavior for an event camera (it only reports *changes* in
   brightness, not a continuous image like a regular camera).
4. Try **Snap** too — it should show a snapshot of whatever the current
   100 ms integration window looks like at that instant.
5. Open the **Device/Property Browser** and confirm the `EBS-*` properties
   from Goal 2 still read correctly — Goal 3 doesn't change those.

If you see the feed reacting to real movement/light changes in front of the
sensor (not just a static or blank image), **Goal 3 is verified working** —
let the project owner know so we can tag this as `v0.3` and move on to
Goal 4 (recording capabilities).

## Goal 4 (Recording capabilities)

### What's new

The adapter can now produce a real Prophesee `.raw` event file — the same
format Metavision's own tools write — while MicroManager runs a Multi-D
Acquisition (MDA), and it lands **next to the MDA's own saved images
automatically** — no script to run, no setting to remember, nothing to
configure before clicking Acquire!. Two properties are involved:

| Property | Meaning |
|---|---|
| `EBS-RawFilePath` | Manual override. Leave empty (the default) for fully automatic behavior. Set it to a specific path to use that instead, skipping auto-discovery entirely. |
| `EBS-RawRecordingStatus` | Read-write status the adapter itself updates: `Not recording`, `Recording to <path>`, `Finished: <path>`, or `Failed: <reason>`. Check this after an acquisition to confirm the `.raw` file was actually written, and where. |
| `EBS-RawTempRecordingFolder` | Where recordings land when the MDA folder can't be determined (e.g. "Save images" unchecked, or auto-discovery fails) — also used as a brief staging spot in some direct-streaming edge cases. Leave empty for the default (`Documents\ProphEBS_Recordings\`), or set it to any folder you'd rather use instead. |

Recording only triggers for a **finite** sequence acquisition (i.e. an MDA
with a fixed number of frames, like "10 frames at 100 ms") — not for Live
view, which is an unbounded stream and would otherwise record indefinitely
every time you click Live.

**Follows MicroManager's own per-run folder convention.** For a saving MDA
(e.g. `MULTIPAGE_TIFF`), MicroManager doesn't save directly into your
chosen root folder — it creates a numbered subfolder per run
(`<prefix>_<N>/`, where `N` auto-increments so previous runs are never
overwritten) and names the image stack after that folder, e.g.
`test_2_MMStack_Pos0.ome.tif`. The `.raw`/`.bias` files follow the exact
same convention: `<root>\<prefix>_<N>\<prefix>_<N>_events_Pos0.raw` (and
`.bias`), landing right next to that run's own images. If a run doesn't use
a numbered subfolder (e.g. saving is disabled, or a different save mode),
the files fall back to `<root>\<prefix>_prophesee_events.raw` instead.

**How the automatic path-matching works, and why it had to be done this
way:** MicroManager's MDA save directory is a Studio (Java)-level setting
that never reaches the C++ device adapter/MMCore layer directly — there is
no message in the MMCore API that carries a file path, so the adapter cannot
simply ask Core for it. It turns out MM Studio logs the *exact*, live MDA
settings (including root/prefix) to its own CoreLog file right before every
single acquisition — the adapter tails that same file (it runs in the same
OS process as MicroManager's Java side, so it can reliably find its own
process's CoreLog) to discover the current save location with **zero
user-facing setup**, correctly tracking changes made mid-session without
restarting MicroManager. If that fails for any reason, it falls back to a
second, less reliable source (MicroManager's `UserProfile` JSON file, which
only reflects the *previous* session's settings), and finally to the
adapter's own `Documents\ProphEBS_Recordings\` folder if even that isn't
available — recording never blocks or fails an acquisition because of this.

This relies on internal, undocumented parts of MicroManager (a specific log
message; a specific settings file layout), not a stable public API, which is
why the fallbacks exist — if a future MM update ever breaks the primary
lookup, recording will keep working, just not necessarily next to the MDA's
images until the adapter is updated to match. See `docs/DEVLOG.md` (Goal 4)
for the full story of how this was found, including two earlier approaches
(a Beanshell script the user had to run; reading a settings file that turned
out to only update when MicroManager closes) that were tried and abandoned
along the way.

### 10. Record a Multi-D Acquisition

1. Make sure your EBS camera is plugged in and `EBS-ConnectionStatus` reads
   `Connected` (open the Device/Property Browser to check).
2. In the main MicroManager window, open **Multi-Dimensional Acquisition**
   (usually a toolbar button or under the **Devices** menu).
3. Set it up for **10 frames** (or "Time points": 10) with a **100 ms**
   interval, and pick/confirm a save location as usual.
4. Click **Acquire!** and let it run to completion. Nothing else to set up
   — the `.raw` file's location is discovered automatically.
5. Open the **Device/Property Browser**, find `ProphEBS-Camera`, and check
   `EBS-RawRecordingStatus`. It should read `Finished: <path>`, and that path
   should be inside (or right next to) the folder you picked in step 3.
6. Navigate to that path in Windows Explorer and confirm the `.raw` file
   exists and has a non-trivial size (a few MB or more, depending on how much
   event activity happened during the ~1 second acquisition).

If the `.raw` file exists and has real content, **Goal 4 is verified
working** — let the project owner know so we can tag this as `v0.4` and move
on to Goal 5 (adding EBS hardware properties).

## Goal 5 (Adding hardware-setting properties)

### What's new

Every commonly-adjusted EBS hardware setting is now exposed as a normal MM
property — visible and settable from the Device/Property Browser exactly
like any other camera's gain/offset/binning, with no special UI. If a real
sensor is connected, each property is backed by the actual hardware
(reading its current on-sensor value, and writing changes straight to the
sensor); if no camera is connected, the same properties still exist as
local, non-hardware-backed values so the adapter stays fully
inspectable/testable without an EBS attached.

| Property group | Properties |
|---|---|
| Bias range check | `EBS-biasRangeCheckBypass` (**pre-init only** — set before "Add Device"/`initializeDevice`, greyed out afterward; mirrors Metavision Studio's "bypass biases range check" checkbox) |
| Biases | One `EBS-bias_*` Integer property per bias the sensor reports (e.g. `EBS-bias_diff`, `EBS-bias_diff_off`, `EBS-bias_diff_on`, `EBS-bias_fo`, `EBS-bias_hpf`, `EBS-bias_refr` on the IMX636) — each range-limited to what the hardware actually supports |
| Event rate control (ERC) | `EBS-ERC-Enabled` (Off/On, default Off), `EBS-ERC-EventRate` (default 50,000,000 events/s) |
| Event trail (STC) filter | `EBS-EventTrailFilter-Enabled` (Off/On, default Off), `-Threshold` (default 10,000 µs), `-Mode` (`TRAIL`/`STC_CUT_TRAIL`/`STC_KEEP_TRAIL`, default `TRAIL`) |
| Anti-flicker | `EBS-AntiFlicker-Enabled` (Off/On, default Off), `-StartThreshold`/`-StopThreshold` (default 6/4), `-DutyCycle` (default 50%), `-FilterType` (`Band Pass`/`Band Cut`, default `Band Cut`), `-LowFreq`/`-HighFreq` (default 50/60 Hz) |
| Static info | `EBS-Generation` (e.g. `4.2`), `EBS-DataEncodingFormat` (e.g. `EVT3`) — read-only, alongside the Goal 2 identification properties |
| Live monitoring | `EBS-AvgDataRate-MBps`, `EBS-AvgEventRate-MEvps`, `EBS-AvgERCDropRate-KEvps`, `EBS-Temperature-C`, `EBS-Illumination-lux`, `EBS-PixelDeadTime-us` — read-only, refresh roughly once per second while the camera is streaming |

`EBS-AvgERCDropRate-KEvps` is an estimate (target minus measured event
rate), since the hardware doesn't report a real dropped-event count.
`EBS-Illumination-lux` may stay at `0.0` on some sensors/SDK versions —
this specific IMX636 unit throws on that one hardware query while
temperature and pixel-dead-time both work fine; handled gracefully (that
one metric just doesn't update, the others still do).

### 11. Adjust EBS hardware settings

1. Make sure your EBS camera is plugged in and `EBS-ConnectionStatus` reads
   `Connected` (open the Device/Property Browser to check).
2. In the Device/Property Browser, find `ProphEBS-Camera` and scroll through
   its property list — you should see the bias, ERC, event trail filter,
   anti-flicker, and live monitoring properties listed above alongside the
   properties from earlier goals.
3. Try changing a bias value (e.g. `EBS-bias_hpf`) to a different value
   within its shown min/max range, then click elsewhere to apply it — this
   writes straight to the sensor.
4. Turn `EBS-AntiFlicker-Enabled` **On** and watch `EBS-AntiFlicker-LowFreq`/
   `-HighFreq` reject anything outside the range the sensor supports (the
   Browser should just refuse an out-of-range value rather than silently
   ignoring it).
5. Watch `EBS-AvgDataRate-MBps`/`EBS-AvgEventRate-MEvps`/`EBS-Temperature-C`
   update on their own every second or so while **Live** is running, with no
   need to touch any other property first.

If bias/filter changes take effect on the sensor and the live monitoring
properties update on their own, **Goal 5 is verified working** — let the
project owner know so we can tag this as `v0.5` and move on to Goal 6
(custom view methods).

## Goal 6 (Custom view methods)

### What's new

The live view is now fully configurable instead of the fixed "100 ms
window, ×32 brightness" behavior from Goal 3. Four things changed:

| Property | Meaning |
|---|---|
| `Exposure` | MicroManager's own standard exposure control **is** the event-integration window now — set it like you would on any camera (Device/Property Browser, MDA dialog, or a script's `setExposure()`). Can go down to **0.001 ms (1 microsecond)** — see the sub-millisecond note below — and changes take effect on the very next event, no restart needed. |
| `EBS-ViewDisplayRefreshMs` | How often the live image is actually published/refreshed (default **1 ms**), separate from `Exposure`. There's no benefit setting this below ~1 ms since nothing downstream (Live view, a human eye) can show it any faster — this just controls how often the displayed frame updates, independent of how short each integration window is. |
| `EBS-ViewLiveMinIntervalMs` | Floor (default **5 ms**) on how fast MicroManager's **Live** view specifically is allowed to push new frames. Live view otherwise follows `Exposure` directly (like any normal camera) — this floor only kicks in once `Exposure` drops below it, since pushing Live frames faster than the GUI can actually display them just causes a growing display backlog/delay, not a smoother picture. Doesn't affect Snap or Multi-D Acquisition, only continuous Live streaming. |
| `EBS-ViewMode` | Which per-pixel quantity is rendered: `NetSigned` (default) shows ON-event-count minus OFF-event-count, signed; `Merged` is the old Goal 3/5 look (ON+OFF combined); `OnOnly`/`OffOnly` show just one polarity. |
| `EBS-ViewOffset` | The gray level a quiet (no-activity) pixel sits at — default **100**. In `NetSigned` mode this is what lets net OFF activity show up as *darker than* this baseline instead of being clipped to 0. |

Formula: `pixel = clamp(EBS-ViewOffset + raw_count, 0, 255)`. (There used to be
an `EBS-ViewScale` multiplier here too; removed as an unneeded extra control
— scale is always 1.)

**Sub-millisecond `Exposure` note**: internally, the integration window is
now measured against the sensor's own event timestamps (microsecond
resolution) rather than a fixed timer, so values well under 1 ms genuinely
work — tested down to the 0.001 ms (1 µs) floor on real hardware at ~10
million events/second with no issues. If the sensor's scene goes fully
quiet (no events at all) for about 100 ms, the display automatically resets
to the `EBS-ViewOffset` baseline rather than freezing on the last frame.
If you drop `Exposure` below `EBS-ViewLiveMinIntervalMs` while **Live** is
running, the live feed itself won't refresh faster than that floor (even
though the integration window internally is that short) — this is
intentional, since MicroManager's own display/GUI can't usefully show
updates faster than a few milliseconds anyway, and trying to push faster
than that just builds up a growing display delay instead of a smoother
picture. Lower `EBS-ViewLiveMinIntervalMs` if you want to test pushing
Live view harder, but don't expect it to help once you're faster than the
GUI can actually draw.

There's also an optional software denoising filter, independent of the
Goal 5 hardware event-trail (STC) filter:

| Property | Meaning |
|---|---|
| `EBS-ActivityFilter-Enabled` | `Off` (default) / `On`. When on, events are run through Metavision's own activity-noise filter *before* being counted, dropping isolated events with no similar neighbor recently. |
| `EBS-ActivityFilter-Threshold-us` | How far back (in microseconds) to look for a "similar recent event" — default 10,000 µs (10 ms). Larger values filter more aggressively. |

**Bug fix — image orientation (`TransposeCorrection`/`TransposeMirrorX`/
`TransposeMirrorY`/`TransposeXY`).** MicroManager creates these four standard
properties on every camera automatically, but MMCore itself never applies
them — each adapter is responsible for its own pixel transform, and this one
previously wasn't, so setting them had no visible effect. They now work:
`TransposeCorrection` is the master switch (must be `1` for Mirror/`TransposeXY`
to do anything, matching the convention other camera adapters use), and
`TransposeXY` swaps the reported image width/height. With a real camera
streaming this takes effect on the very next displayed frame; with no camera
connected (the static test pattern), it takes effect at the next
`Exposure`-independent redraw — an ROI or `Binning` change.

### 12. Try the new view controls

1. Make sure your EBS camera is plugged in and connected, and open **Live**.
2. Open the **Device/Property Browser** for `ProphEBS-Camera`.
3. Change `Exposure` (e.g. to 250 to integrate longer, or 30 for a faster,
   noisier feed) and watch the Live window respond within about one new
   exposure interval. Try a sub-millisecond value too, e.g. `0.1` or even
   the floor `0.001` — the feed should keep updating smoothly, just
   integrating a much shorter slice of activity per displayed frame.
4. Change `EBS-ViewMode` between `NetSigned`, `Merged`, `OnOnly`, and
   `OffOnly` while something is moving in front of the sensor, and compare
   how each looks.
5. Turn `EBS-ActivityFilter-Enabled` **On** in a noisy/low-light scene and
   compare the amount of stray single-pixel flicker before and after.

If the live view visibly responds to each of these properties as described,
**Goal 6 is verified working** — let the project owner know so we can tag
this as `v0.6` and move on to Goal 7 (pixel masking and sensor ROI).

## Goal 7 (Pixel-by-pixel differences)

### What's new

Two independent features: blocking individual "hot" pixels (ones that fire
constantly regardless of the scene, a common event-camera hardware defect),
and a hardware region-of-interest (ROI) that crops the sensor itself rather
than just the displayed image.

| Property | Meaning |
|---|---|
| `EBS-HotPixelBlockedPixels` | A semicolon-separated list of `x:y` pixel coordinates to mask, e.g. `114:239;500:12`. Empty by default. Can be typed by hand, or filled in automatically by calibration (below). Rejects a malformed value outright (keeps the previous valid list) rather than accepting garbage. |
| `EBS-HotPixelDetectNow` | `Idle` (default) / `Run`. Setting this to `Run` clears `EBS-HotPixelBlockedPixels`, watches the sensor for `EBS-HotPixelCalibDurationMs` milliseconds, and replaces the list with whatever pixels came out as statistical outliers — see below for why it *replaces* rather than adds to the existing list. Automatically resets to `Idle` when done. |
| `EBS-HotPixelCalibDurationMs` | How long a calibration run watches the sensor (default **5000 ms**). |
| `EBS-HotPixelStddevK` | How many standard deviations above the mean a pixel's event count must be to get flagged as an outlier (default **10**, range 0.1–100). Lower = more aggressive masking (more false positives); higher = only the most extreme outliers get caught. |
| `EBS-HotPixelCalibStatus` | Read-only. Reports the outcome of the last calibration run, e.g. `Done: 64 masked (64 new, 31 dropped (length/hardware-slot limit))`, or a failure reason if no camera was streaming. |

Masking is enforced by the sensor's own hardware pixel mask, which has a
**fixed 64-pixel limit** on this sensor generation (IMX636/Gen4.1) — if
calibration or a manually-typed list would exceed that (or MMCore's
1024-character property-string limit, whichever is tighter), the least
significant outliers are silently dropped from what's actually enforced,
and `EBS-HotPixelCalibStatus` says so explicitly.

**Calibrate against a static/dark scene for best results.** Because the
statistics are computed across whatever's currently visible (within the
active ROI), real scene motion or uneven lighting during a calibration run
can pull genuinely normal pixels above the threshold alongside real
hardware defects. Covering the lens (or pointing it at a plain, static
surface) during calibration gives a cleaner, hot-pixel-only result.

Hardware ROI uses MicroManager's own standard `SetROI`/`GetROI`/`ClearROI`
API (the same one every other MM camera adapter uses) — no `EBS-*`
property needed. Setting an ROI crops the sensor itself (fewer raw events
leave the chip at all), not just the displayed/recorded image, so it also
meaningfully reduces `EBS-AvgEventRate-MEvps`/`EBS-AvgDataRate-MBps` and
recording file size. Hot-pixel calibration automatically scopes itself to
whatever ROI is currently active, rather than always scanning the full
chip.

### 13. Try hot-pixel masking and hardware ROI

1. Make sure your EBS camera is plugged in and connected, and open **Live**.
2. In the Device/Property Browser, set `EBS-HotPixelCalibDurationMs` and
   `EBS-HotPixelStddevK` if you want non-default values, then set
   `EBS-HotPixelDetectNow` to `Run`. Wait for it to reset to `Idle` on its
   own, then check `EBS-HotPixelCalibStatus` and
   `EBS-HotPixelBlockedPixels` for the result.
3. Use MicroManager's own ROI tool (drag a rectangle on the Live image, or
   use the toolbar's "Set ROI" button) to crop to a smaller region, then
   watch `EBS-AvgEventRate-MEvps` drop compared to the full-frame value.
   Use "Clear ROI" (or `ClearROI()` via a script) to restore the full
   sensor.

If calibration produces a plausible `EBS-HotPixelBlockedPixels` list and the
ROI tool visibly crops the Live view (with a lower event rate to match),
**Goal 7 is verified working** — let the project owner know so we can tag
this as `v0.7` and move on to Goal 8 (additional hardware/SDK features).

## Goal 8 (Additional hardware/SDK features)

### What's new

Five more hardware capabilities, picked by scanning the Metavision SDK and
MM's own camera API for anything Goals 1-7 hadn't already covered.

| Property group | Properties |
|---|---|
| Hardware trigger in/out | `EBS-TriggerIn-Channel` (which physical input channel), `EBS-TriggerIn-Enabled`, `EBS-TriggerIn-Count` (read-only, counts real trigger pulses seen); `EBS-TriggerOut-Enabled`, `-PeriodUs`, `-DutyCycle` (configures the camera's own output pulse) |
| Sensor event-rate filter | `EBS-EventRateFilter-Enabled` plus `-LowerStart`/`-LowerStop`/`-UpperStart`/`-UpperStop` (evt/s hysteresis thresholds) — drops events **at the sensor itself** whenever the overall event rate falls outside this band, saving USB bandwidth. Distinct from the Goal 6 software `EBS-ActivityFilter-*` and the Goal 5 `EBS-EventTrailFilter-*` (per-pixel, not rate-based). |
| Time-decay view mode | A fifth `EBS-ViewMode` value, `TimeDecay` — each pixel fades from bright to the baseline over `EBS-ViewModeTimeDecay_DecayTime_Constant-us` microseconds since it last fired, instead of showing a fixed integration window. Gives a persistence/trail-like look. |
| Binning | MicroManager's standard `Binning` property now does real 1×/2×/4× spatial binning — sums raw event counts over each block into one output pixel (higher apparent sensitivity, lower resolution), same idea as a real camera's charge binning. |
| Camera sync mode | `EBS-SyncMode` (`Standalone`/`Master`/`Slave`, **pre-init only** — set before "Add Device", greyed out afterward) — for linking multiple EBS cameras' timestamps together. Needs a second, physically-linked EBS to actually test; on a single camera only `Standalone` is meaningful. |

**Trigger-in is a monitoring input, not a capture gate.** Unlike a typical
frame camera, enabling `EBS-TriggerIn-Enabled` does not pause or gate Live
view — the EBS sensor is free-running by design, and trigger-in pulses are
just recorded as timestamped markers alongside the normal event stream (for
aligning an external stimulus to event data afterward). `EBS-TriggerIn-Count`
staying at `0` with nothing physically wired to the trigger-in pin is
expected, not a bug — it's there so you can confirm the monitoring path is
alive once you do wire something up.

### 14. Try the Goal 8 features

1. Make sure your EBS camera is plugged in and connected, and open **Live**.
2. Change `Binning` (the standard MM property, same place as any other
   camera) between `1`, `2`, and `4` and watch the Live image's resolution
   change accordingly (and, if the scene is dim, look visibly brighter at
   higher binning).
3. Set `EBS-ViewMode` to `TimeDecay` and compare the trailing/persistence
   look against `NetSigned`/`Merged` from Goal 6. Try changing
   `EBS-ViewModeTimeDecay_DecayTime_Constant-us` to see faster/slower fade.
4. If you have a signal generator or other pulse source wired to the
   sensor's trigger-in pin, turn `EBS-TriggerIn-Enabled` on and confirm
   `EBS-TriggerIn-Count` increases with real pulses. Without one wired up,
   just confirm the property exists and Live view is unaffected either way.
5. `EBS-EventRateFilter-*` and `EBS-TriggerOut-*` can be round-tripped in
   the Device/Property Browser even without external hardware to observe
   their effect directly.

If Binning visibly changes the Live view's resolution/sensitivity and
TimeDecay mode shows a distinct persistence-style look, **Goal 8 is
verified working** — let the project owner know so we can tag this as
`v0.8` and move on to Goal 9 (full suite polishing).

## Troubleshooting MicroManager itself

| Symptom | Likely cause |
|---|---|
| `ProphEBS` doesn't appear in the Hardware Configuration Wizard device list | The DLL isn't in the MicroManager install folder, or it's a 32-bit build (must be x64/Release), or MicroManager itself is a 32-bit install (rare, would need a 32-bit rebuild) |
| `ProphEBS` appears as a flat entry marked **"(unavailable)"** instead of an expandable folder with `ProphEBS-Camera` inside it, **and** `tools\mm_python_env\Scripts\mmcore list` shows your active install's interface version *matching* what the DLL needs | **Metavision SDK DLLs missing/not on `PATH`** at MicroManager's load time (`metavision_hal.dll`, `metavision_sdk_base.dll`, `metavision_sdk_stream.dll`). This looks identical to the device-interface-version case below in the GUI (Windows can't load the DLL at all, so MMCore never gets far enough to report a specific reason) — the interface-version check above is what tells them apart. Confirm `C:\Program Files\Prophesee\bin` (or wherever you installed the SDK) is on your system `PATH` (see step 1d) — the SDK installer normally does this for you, but a manual/non-default install or a `PATH` edited afterward can undo it. After fixing `PATH`, restart MicroManager (a `PATH` change doesn't apply to already-running programs) |
| `ProphEBS` appears as a flat entry marked **"(unavailable)"**, **and** `mmcore list` shows a version *mismatch* | **Device interface version mismatch** — your installed MicroManager build and the `mmgr_dal_ProphEBS.dll` you built expect different interface versions. Get a newer/matching nightly build via `mmcore install` (or the no-admin workaround, see step 1c) |
| Building fails with compiler errors like `cannot open include file 'metavision/sdk/...'` or linker errors about `metavision_hal.lib` | **Metavision SDK not installed, or `MetavisionSdkRoot` points at the wrong folder** — this is a raw MSBuild/compiler error, not a custom message from this project, so it can look like something else is wrong. Confirm the SDK is actually installed (step 1d) and, if it's not at the default `C:\Program Files\Prophesee`, pass `/p:MetavisionSdkRoot="<your path>"` on the MSBuild command line |
| MicroManager crashes or shows a popup error when adding the device | Copy the exact error text and the relevant lines from the CoreLog around the crash — this is the most useful debugging info |
| Live/Snap shows a black image or an error instead of the checkerboard | Note down what MicroManager's status bar / log says at that moment |
| Goal 3: Live view stays completely black even while waving a hand in front of the sensor | Check `EBS-ConnectionStatus` is `Connected` first (if not, you're still seeing the Goal 1/2 static checkerboard, not a live feed). If connected, check the sensor lens isn't covered, and that you're close enough / moving enough to actually generate events — event cameras only report brightness *changes* |
| Goal 4: `EBS-RawRecordingStatus` reads `Failed: ...` | The resolved path is likely invalid (e.g. a folder that couldn't be created, or no write permission) — check the exact error text and the CoreLog. If `EBS-RawFilePath` is empty (auto-discovery), check the CoreLog for "auto-discovered" vs. "could not auto-discover" to see which path was actually used |
| Goal 4: the `.raw` file didn't land next to the MDA's images | Auto-discovery likely fell back to `Documents\ProphEBS_Recordings\` — check the CoreLog around acquisition start for "could not auto-discover the Multi-D Acquisition save location." This can happen if MicroManager's UserProfile JSON format changed in your installed version (see `docs/DEVLOG.md`, Goal 4) — the recording still succeeds, just not in the expected folder |
| Goal 4: no `.raw` file was created at all | Confirm `EBS-ConnectionStatus` was `Connected` — recording needs a real streaming camera; it's a no-op with no hardware attached (by design, same as Goal 2/3's fallback behavior) |
| Goal 5: a bias value silently reverts to something other than what you typed (e.g. setting `bias_refr` to 255 reads back 235) | Not a bug — the sensor firmware enforces its own safety clamp tighter than the range `EBS-biasRangeCheckBypass`/`SetPropertyLimits` reports as allowed. The property is updated to reflect the true hardware value immediately, and the CoreLog explains the clamp when it happens |
| Goal 7: hot-pixel calibration masks far more pixels than expected | Likely ran against a busy/moving scene — the statistics are computed over whatever's currently active within the ROI, so real motion or uneven lighting inflates the count and can flag normal pixels as outliers alongside genuine hardware defects. Re-run calibration against a static/dark scene (lens covered) for a cleaner result, or raise `EBS-HotPixelStddevK` |
| Goal 8: `EBS-SyncMode` seems to "reset itself" back to `Standalone` after being set to `Master`/`Slave` | Expected — `EBS-SyncMode` is a **pre-init-only** property (same category as `EBS-biasRangeCheckBypass`), only ever applied once, before the camera starts streaming. Set it in the Hardware Configuration Wizard *before* clicking "Add Device", not afterward in the Device/Property Browser (it's greyed out there) |
| Goal 8: Live view doesn't pause/gate even with `EBS-TriggerIn-Enabled` on and nothing wired to the trigger-in pin | Expected, not a bug — trigger-in only monitors an external pin for timestamp-marker purposes; the EBS sensor is free-running by design and never waits for a trigger to produce CD events (unlike a typical frame camera's trigger-in). See the Goal 8 section above |
