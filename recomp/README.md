# Pipeline static recompilation

Seluruh `libGame.so`, `libopenal.so`, dan `libVendor_mpg123.so` diterjemahkan dari ARM64 ke C saat build, lalu dikompilasi clang ke x64. Tidak ada interpreter, JIT, atau Unicorn di executable. Status dan peta workspace ada di [../HANDOVER.md](../HANDOVER.md).

## Build

```powershell
python recomp/build.py            # lift -> ikat impor -> kompilasi paralel -> link
python recomp/build.py --no-lift  # hanya runtime host yang berubah
```

Keluaran: `build/lcs.exe` (subsistem Windows, ikon dari APK), `build/lcs.map`, `build/lcs.pdb`.

## Berkas

| Berkas | Peran |
| --- | --- |
| `a64.py` | Semantik tiap instruksi ARM64 → pernyataan C. |
| `lift.py` | Analisis ELF (simbol, FDE, PLT, relokasi), pembagian fungsi, emisi C per 32 KiB teks. |
| `gen_gl.py` | Thunk konvensi pemanggilan AAPCS64 → GLES2 host. |
| `build.py` | Orkestrasi build dan pengikatan impor antar-modul. |
| `rt/recomp.h` | State CPU guest dan helper instruksi (dipakai kode hasil terjemahan). |
| `rt/rt.h`, `rt/rt_core.c` | Pemetaan ELF, relokasi, `rt_call`, thunk host, diagnosis crash. |
| `rt/rt_libc.c` | Bionic libc/libm/pthread di atas Win32. |
| `rt/rt_android.c` | JNIEnv, kontrak metode Java, AAssetManager, ANativeWindow, EGL lewat SDL3. |
| `rt/rt_audio.c` | Layanan OpenSL ES (buffer queue) di atas SDL3 audio. |
| `rt/tweaks.c` | `lcs.ini` → variabel engine. Lihat [SETTINGS.md](SETTINGS.md). |
| `rt/timing.c` | Koreksi batas bawah timer animasi untuk refresh rate di atas 100 Hz. |
| `rt/frontend.c` | Presentasi menu utama/pause dan gambar kamera di kanan; aksi/submenu memakai game asli. |
| `rt/radio_probe.c` | Diagnostik WAD musik → decoder MP3 asli (`--radio-probe`). |
| `rt/main.c` | Peran `GameActivity`/`GameGlThread`: urutan JNI, input, loop frame. |
| `res/lcs.rc` | Resource ikon; `lcs.ico` dibuat `tools/make_icon.py` dari APK. |
| `tools/difftest.py` | Uji diferensial penerjemah terhadap Unicorn. |
| `tools/test_timing.py` | Regresi timer asli pada 30/60/120/144/240 Hz, pause, dan mode khusus. |
| `tools/extract_radio_wad.py` | Ekstrak hanya musik yang hilang dari OBB lokal menjadi paket terpisah. |
| `tools/extract_apks.py`, `tools/bmp2png.py`, `tools/survey.py`, `tools/shapes.py`, `tools/relocs.py` | Ekstraksi APK dan alat analisis. |

## Menambah impor host

Tulis `IMP(nama) { ... }` di salah satu `rt/*.c`. Argumen guest: `A(n)`/`P(n)`/`S(n)`/`I(n)` untuk x0–x7, `FA(n)`/`DA(n)` untuk v0–v7; hasil lewat `RET`, `RETI`, `RETF`, `RETD`. `build.py` menemukannya sendiri. Impor tanpa penyedia menjadi trap yang menyebut namanya saat dipanggil (`build/recomp/missing_imports.txt`).

## Entry tak langsung

Jika game memanggil alamat yang belum dikenali sebagai awal fungsi, runtime berhenti dengan pesan dan mencatat alamatnya di `recomp/entries_<P>.txt`. Build ulang menambahkannya. Sampai gameplay awal, ini belum pernah terjadi.

## Batas penerjemah

- `throw` C++ berjalan sampai `std::terminate`; unwinding ke `catch` belum ada.
- `setjmp` selalu mengembalikan 0 dan `longjmp` fatal.
- Instruksi eksklusif (`ldaxr/stlxr`) diemulasi dengan compare-and-swap.
