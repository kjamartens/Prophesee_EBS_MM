# Build & Usage Tutorial — Goal 1 (Barebones Device Adapter)

This tutorial assumes no prior experience building C++ software or using
MicroManager. Follow it top to bottom the first time.

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

## Troubleshooting MicroManager itself

| Symptom | Likely cause |
|---|---|
| `ProphEBS` doesn't appear in the Hardware Configuration Wizard device list | The DLL isn't in the MicroManager install folder, or it's a 32-bit build (must be x64/Release), or MicroManager itself is a 32-bit install (rare, would need a 32-bit rebuild) |
| `ProphEBS` appears as a flat entry marked **"(unavailable)"** instead of an expandable folder with `ProphEBS-Camera` inside it | **Device interface version mismatch** — your installed MicroManager build and the `mmgr_dal_ProphEBS.dll` you built expect different interface versions. Run `tools\mm_python_env\Scripts\mmcore list` (see step 1c) to see the required version and whether your active install matches; if not, get a newer/matching nightly build via `mmcore install` (or the no-admin workaround) |
| MicroManager crashes or shows a popup error when adding the device | Copy the exact error text and the relevant lines from the CoreLog around the crash — this is the most useful debugging info |
| Live/Snap shows a black image or an error instead of the checkerboard | Note down what MicroManager's status bar / log says at that moment |
