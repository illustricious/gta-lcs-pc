# Static recompilation pipeline

At build time, `libGame.so`, `libopenal.so` and `libVendor_mpg123.so` are translated from ARM64 to C, and clang compiles that C for x64. The executable contains no interpreter, no JIT and no Unicorn. Status and the repository map are in [../HANDOVER.md](../HANDOVER.md).

## Build

```powershell
python recomp/build.py            # lift -> bind imports -> parallel compile -> link
python recomp/build.py --no-lift  # only the host runtime changed
```

The output is `build/lcs.exe` (Windows subsystem, icon taken from the APK). The Visual Studio toolchain also writes `build/lcs.map` and `build/lcs.pdb`.

| Option | Default |
| --- | --- |
| `--libs` | `local/extracted/split_config.arm64_v8a/lib/arm64-v8a` |
| `--out` | `build/recomp` |
| `--exe` | `build/lcs.exe` |
| `--llvm` | Visual Studio's LLVM; an llvm-mingw `bin` folder also works |
| `--icon-res` | `local/extracted/base/res` |

## Files

| File | Role |
| --- | --- |
| `a64.py` | Semantics of every ARM64 instruction → C statements |
| `lift.py` | ELF analysis (symbols, FDEs, PLT, relocations), function splitting, C emission per 32 KiB of text, host hooks |
| `gen_gl.py` | AAPCS64 → host GLES2 calling-convention thunks, plus the framebuffer-0 redirect |
| `build.py` | Build orchestration and cross-module import binding |
| `rt/recomp.h` | Guest CPU state and instruction helpers, used by the generated code |
| `rt/rt.h`, `rt/rt_core.c` | ELF mapping, relocation, `rt_call`, host thunks, crash diagnostics |
| `rt/rt_libc.c` | Bionic libc/libm/pthread on Win32 |
| `rt/rt_android.c` | JNIEnv, Java method contracts, AAssetManager, ANativeWindow, EGL through SDL3 |
| `rt/rt_audio.c` | OpenSL ES service (buffer queues) on SDL3 audio |
| `rt/main.c` | Plays the role of `GameActivity`/`GameGlThread`: JNI order, input, frame loop, display modes, install layout |
| `rt/present.c` | Scene framebuffer and letterboxed scaling into the window |
| `rt/frontend.c` | Host-drawn main/pause/settings menu; actions and leaf pages use the original game |
| `rt/tweaks.c` | `lcs.ini` → engine variables (see [SETTINGS.md](SETTINGS.md)) |
| `rt/timing.c` | Frame-step fixes for high refresh rates |
| `rt/radio_probe.c` | Music WAD → the game's MP3 decoder (`--radio-probe`) |
| `installer/` | Data import tool and Inno Setup script |
| `tools/make_icon.py` | `lcs.ico` from the APK launcher icons |
| `tools/difftest.py` | Differential test of the translator against Unicorn |
| `tools/test_timing.py` | Timer regression at 30/60/120/144/240 Hz |
| `tools/extract_radio_wad.py` | Missing music from the old OBB → `data_music.wad` |
| `tools/extract_apks.py`, `bmp2png.py`, `survey.py`, `shapes.py`, `relocs.py`, `xref.py` | APK extraction and analysis helpers |

## Adding a host import

Write `IMP(name) { ... }` in one of the `rt/*.c` files; `build.py` finds it by itself.

| Macros | Use |
| --- | --- |
| `A(n)`, `P(n)`, `S(n)`, `I(n)` | Guest arguments in x0–x7 |
| `FA(n)`, `DA(n)` | Guest arguments in v0–v7 |
| `RET`, `RETI`, `RETF`, `RETD` | Return a value |

An import with no provider becomes a trap that names itself when it is called; these are listed in `build/recomp/missing_imports.txt`.

## Indirect entries

The game may call an address that was not recognised as a function start. When that happens, the runtime stops with a message and records the address in `recomp/entries_<P>.txt`, and the next build includes it. So far this has not happened.

## Translator limits

- A C++ `throw` runs to `std::terminate`; there is no unwinding to a `catch` yet.
- `setjmp` always returns 0, and `longjmp` is fatal.
- Exclusive instructions (`ldaxr`/`stlxr`) are emulated with compare-and-swap.
