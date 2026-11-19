# Handover: GTA LCS Android → Windows x64 (static recompilation)

Updated 13 November 2026.

- **Target:** `com.rockstargames.gtalcs` 2.4.379 (versionCode 4256228), ARM64.
- **Platform:** Windows 10/11 x64.
- **Usage:** see [README.md](README.md). This document is for people continuing the work.

## Status

The game boots to gameplay and plays: title, legal screens, menu, new game, load, cutscenes, and driving with the HUD. All logic and rendering is the original recompiled code.

| Part | State |
| --- | --- |
| ARM64 → C translator | 1,566,853 instructions in three libraries, none left untranslated. The differential test against Unicorn passes for every instruction shape except atomics, where the oracle itself fails. |
| Boot, save/load, gameplay | Verified with scripted input and framebuffer captures. Long sessions and missions have only had light testing. |
| Audio | The original OpenAL Soft drives a host OpenSL ES service on SDL3. Radio comes from a `data_music.wad` built from the old OBB; all 21 streams decode with the game's mpg123. |
| Graphics | GLES2 calls go to the driver's OpenGL ES context through SDL3. The scene is rendered into a host framebuffer at the desktop resolution and scaled into the window (`rt/present.c`). |
| Input | SDL3 gamepad, keyboard and mouse camera. Bindings are editable in the menu. |
| Menu | Host-drawn main/pause menu in `rt/frontend.c`. It uses the original fonts, sprites, sounds and actions, and shows the live map in the pause pane. |
| Display | Windowed, borderless and exclusive modes; Alt+Enter / F11; resizable window. |
| Rust engine ports | `recomp/engine` (no_std staticlib) replaces hooked functions. The 6 `CSimpleModelInfo` LOD queries are ported and shadow-verified: over 1M calls, 0 mismatches. See [recomp/engine/README.md](recomp/engine/README.md). |
| Distribution | `LCS-PC-Setup.exe` builds the game on the player's PC from their own APKs (see [recomp/installer/README.md](recomp/installer/README.md)). |

## Repository map

| Path | Contents |
| --- | --- |
| `recomp/` | All port sources. See [recomp/README.md](recomp/README.md). |
| `recomp/installer/` | `lcs_data.c` (data import over ADB / from APKs) and `lcs.iss` (Inno Setup). |
| `recomp/entries_<P>.txt` | Indirect entry addresses found at run time, created when needed. |
| `build/` | Not in git: `lcs.exe`, `SDL3.dll`, `lcs.map`, generated C in `build/recomp/`, `build/installer/`. |
| `local/` | Not in git: your APKs (`local/apks`), their extraction (`local/extracted`), and dev saves (`local/userdata`). |
| `.tools/` | Not in git: Python packages (`py`), SDL3 (`sdl3`). |

## How it works

1. `lift.py` splits `.text` into functions using the symbols and the `.eh_frame` FDEs. `a64.py` then translates every instruction into C that operates on a `struct CPU`.
2. Guest pointers are host pointers. The modules are mapped at fixed bases: G `0x200000000`, O `0x210000000`, M `0x218000000`.
3. Direct calls become C calls. Indirect calls go through `rt_call` and an address → function table.
4. Imports are bound at build time to one of three things:
   - another module (libGame takes libc++ from libopenal);
   - a host shim `IMP(name)`;
   - a trap.
5. `rt/main.c` calls the original JNI entry points in `GameActivity` order, then calls `viewOnDrawFrame` every frame.
6. Host hooks (`HOST_HOOKS` in `lift.py`) rename an original function to `<entry>_orig` and let a host wrapper take its public name. They are used for the timer (`G_370b30`), the frontend (`G_527bf4`) and a few engine fixes.

## Diagnostics

- `--verbose` logs JNI calls and file access. `--watchdog S` prints the guest state when the game hangs.
- Traps and host exceptions print the guest registers, the last guest function and the guest frame chain.
- After changing `a64.py`, run `python recomp/tools/difftest.py <lib.so> build/difftest` to check the translator.
- Unattended runs: `--size 1280 720 --frames N --press F BUTTON --shots N build/shots/x%04u.bmp`, then `python recomp/tools/bmp2png.py`. With a save present, `--press 700 0 --press 1900 0` dismisses the title and presses CONTINUE.
- Host code that calls guest code (sounds, native buttons) must save and restore the `CPU`. A clobbered x0 crashed native menu pages.

## Decisions

- **Rockstar gate.** By default the gate is answered as for an owner. The installer only builds from data copied from the user's own copy. `--trial` closes the gate. Nothing is verified online.
- **Social Club.** `GameLoad` completes as an offline session.
- **Controller always present.** The keyboard and mouse are reported as controller 0 (`setNoJoysticks(1)`), so the game uses its gamepad scheme.
- **Draw distance is conservative.** Building LOD swaps and sector streaming stay at the original values, because raising them caused pop-in. Only vehicle and ped distances, the small-object cull fade and the far clip are scaled (see [recomp/SETTINGS.md](recomp/SETTINGS.md)). The building LOD-to-detail distance is baked into the world sector data, so changing it needs a sector re-bake tool.
- **No game code in releases.** The installer ships sources and recompiles the player's own `libGame.so`.

## Next work

See [recomp/PLAN.md](recomp/PLAN.md). In short:

- FSR 1 / sharpening on the scene framebuffer.
- Button glyphs in the menu.
- Testing the phone (ADB) import and the radio OBB path.
- C++ exception unwinding and `setjmp`/`longjmp`.
- The intro video.
- Vulkan (most likely ANGLE through `SDL_HINT_OPENGL_ES_DRIVER`).
