# GTA: Liberty City Stories PC (static recompilation)

A native Windows x64 port of **GTA: Liberty City Stories for Android (2.4.379)**. The port is a static recompilation, so nothing is emulated and nothing is rewritten:

- The game's own ARM64 libraries (`libGame.so`, `libopenal.so`, `libVendor_mpg123.so`) are translated to C and compiled for x64.
- There is no emulator, interpreter, or rewritten engine. All game logic and rendering is the original code.
- Only the operating-system services are written for Windows: files, threads, JNI, window, OpenGL ES, audio and input.

**This repository contains no game code and no game data.** You need your own copy of the Android game. The installer copies the data from your copy and recompiles its code on your PC.

## Install

1. Download `LCS-PC-Setup.exe` from [Releases](../../releases).
2. Pick an install folder, then where the game data comes from:
   - **Your Android phone**, over USB with USB debugging on (ADB). If ADB is missing, Google's platform-tools are downloaded.
   - **APK files** already on your PC: `base.apk`, `split_config.arm64_v8a.apk`, `split_data_main.apk`.
3. Setup downloads a free toolchain and recompiles the game into `lcs.exe`. This takes about 3 minutes. The toolchain is llvm-mingw, embeddable Python, capstone and pyelftools; every file is checked by SHA-256 and removed when setup finishes.

Requirements:
- Windows 10/11 x64.
- About 4 GB of free space during setup; about 2.5 GB stays installed.
- The 64-bit Android release, version 2.4.379. Other versions are rejected by SHA-256.

**Saves and settings** are in `Documents\Rockstar Games\GTA Liberty City Stories`: the saves (`GTA3LCSsf*.b`), `gta_lcs.set`, `lcs.ini` and `lcs.log`. Uninstalling keeps this folder.

**Radio stations.** The current Android release has no radio music. To add it, do one of these:
- Put a `data_music.wad` into `<install>\data\assets\`.
- Give setup the old `main.*.com.rockstargames.gtalcs.obb`, and it builds the WAD from the OBB.

## Controls

Gamepads (XInput, DualShock, DualSense through SDL3) are mapped like the Android controller. The on-screen touch buttons disappear once a pad or the keyboard is used. Keys can be rebound in **Settings → Controls**.

| Keyboard / mouse | Game button |
| --- | --- |
| W A S D | Left stick |
| Mouse movement (in game) | Right stick (camera) |
| Left click / Left Ctrl | B |
| Right click / E | R1 |
| Left Shift / Enter | A |
| Space | X |
| F | Y |
| Q | L1 |
| Z / X | L2 / R2 |
| Arrows, mouse wheel | D-pad |
| C / V / middle click | L3 / R3 |
| P | Start |
| Esc | Back / pause |
| Alt+Enter, F11 | Window ↔ fullscreen |

## Settings

Everything is in the in-game **Settings → Graphics** menu and in `lcs.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `display_mode` | `borderless` | `windowed` (resizable), `borderless` or `exclusive` |
| `width`, `height` | 1280, 720 | Window size in windowed mode, saved when you resize |
| `fps_cap`, `vsync` | 60, 0 | Frame cap: 30, 60 or 120 |
| `draw_distance` | 2.0 | 1.0 = original, up to 3.0. Covers vehicles, peds, small objects and the far clip. Building LOD and streaming stay at the original values. |
| `res_scale` | 0 | Internal render scale, 0 = engine default |
| `radar`, `radar_margin` | `bottom-left`, 28 | `top-left` = mobile position |
| `mouse_look`, `mouse_sensitivity` | 1, 1.0 | Mouse camera |
| `pause_on_focus_loss` | 0 | Open the pause menu when the window loses focus |
| `definitive_menu` | 1 | Vertical menu on the left; 0 = the original carousel |

The engine always renders at the desktop resolution into an off-screen framebuffer. That image is scaled (letterboxed) into the window, so the window can be resized at any time.

## Ownership and the Rockstar gate

On Android, "Start New Game" asks the Rockstar SDK whether the account owns the game. Windows cannot verify that. The port therefore answers the gate as it would for an owner: the installer only works with data copied from a copy you own. `--trial` restores the closed gate. The Social Club session is always offline.

## Building from source

Development build, run from the repository root:

```powershell
python -m pip install --only-binary :all: --target .tools/py -r recomp/requirements.txt
python recomp/build.py
```

What you need:
- Python 3.11+.
- Clang: either Visual Studio Build Tools with the clang component, or llvm-mingw (pass `--llvm <dir>\bin`).
- SDL3 3.4.18 (VC package) unpacked in `.tools/sdl3`.

Your extracted APKs go in `local/extracted/`:

```powershell
python recomp/tools/extract_apks.py local/apks local/extracted
```

The output is `build/lcs.exe`. The installer is described in [recomp/installer/README.md](recomp/installer/README.md).

## Command-line options

| Option | Meaning |
| --- | --- |
| `--windowed`, `--size W H` | Run in a window |
| `--no-audio` | Silent OpenAL backend |
| `--trial` | Keep the purchase gate closed |
| `--radio-probe` | Decode every radio stream with the game's MP3 decoder, then exit |
| `--verbose`, `--frames N`, `--watchdog S`, `--shots N pattern%04u.bmp`, `--tap F X Y`, `--press F BUTTON`, `--mpress F BUTTON`, `--autowalk`, `--resize F W H`, `--set KEY VALUE`, `--gltrace` | Diagnostics and scripted tests |

## Not supported yet

- The intro video is skipped.
- C++ exceptions cannot unwind to a `catch`, and `setjmp`/`longjmp` are not supported.
- Rendering goes through the driver's OpenGL ES. There is no Vulkan backend, FSR or DLSS yet.

## Documentation

| File | Contents |
| --- | --- |
| [HANDOVER.md](HANDOVER.md) | Technical status and how the port works |
| [recomp/README.md](recomp/README.md) | The recompilation pipeline |
| [recomp/SETTINGS.md](recomp/SETTINGS.md) | Settings, the host menu and engine notes |
| [recomp/PLAN.md](recomp/PLAN.md) | Roadmap |
| [STARTUP.md](STARTUP.md) | The Android startup sequence |
