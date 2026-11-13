# Installer

`LCS-PC-Setup.exe` (about 3.5 MB) contains no game code and no game data. On the player's PC it does three things:

1. **Imports the data** from the player's own Android copy with `lcs-data.exe`. The source is the phone over ADB, or APK files already on the PC.
2. **Downloads a free toolchain** on the wizard's download page. Each file is checked against a pinned SHA-256:
   - llvm-mingw 20261020 (ucrt, x86_64)
   - Python 3.11.9 embeddable
   - capstone 5.0.6 and pyelftools 0.32
3. **Recompiles `libGame.so`** (plus `libopenal.so` and `libVendor_mpg123.so`) into `<install>/lcs.exe` with `recomp/build.py`. Afterwards it deletes the toolchain and the generated C.

On a 16-thread PC the whole install took 2 min 53 s, from download to finished `lcs.exe`.

## Building the installer

1. `clang -O2 -fuse-ld=lld -o build/installer/lcs-data.exe recomp/installer/lcs_data.c`
2. `iscc recomp/installer/lcs.iss` (Inno Setup 6). This writes `build/installer/LCS-PC-Setup.exe`.

## Unattended install and tests

```
LCS-PC-Setup.exe /VERYSILENT /CURRENTUSER /DIR="C:\Games\LCS" /APKS="C:\path\to\apks"
```

## Install layout

| Location | What is stored there |
| --- | --- |
| `<install>/lcs.exe` | Built on this PC |
| `<install>/SDL3.dll`, `lcs-data.exe`, `licenses/` | Shipped |
| `<install>/data/lib/` | `libGame.so`, `libopenal.so`, `libVendor_mpg123.so` (arm64, from the player's APK) |
| `<install>/data/assets/` | `assets/` from every APK merged into one folder, plus `data_music.wad` when an old OBB was given |
| `<install>/data/res/` | Launcher icons, used for `lcs.exe`'s icon |
| `<install>/src/` | Recompiler, runtime and SDL headers. `src/build.cmd` rebuilds once the toolchain is there. |
| `Documents/Rockstar Games/GTA Liberty City Stories/` | Saves, `gta_lcs.set`, `lcs.ini`, `lcs.log`. Uninstalling keeps this folder. |

`lcs.exe` uses this layout whenever `data/lib/libGame.so` sits next to it.

## Build toolchains

`build.py` builds with either toolchain:

- **llvm-mingw:** pass `--llvm <dir>\bin`. This adds `-D_WIN32_WINNT=0x0A00 -DSDL_USE_BUILTIN_OPENGL_DEFINITIONS` and links statically.
- **Visual Studio's clang:** the default for development.

Other options:

| Option | Default |
| --- | --- |
| `--libs` | `local/extracted/...` |
| `--out` | `build/recomp` |
| `--exe` | `build/lcs.exe` |
| `--icon-res` | `local/extracted/base/res` |

## For the GitHub repository

Do not commit `recomp/res/lcs.ico`. It is made from the APK's launcher icon. The same goes for `local/` and `build/`.
