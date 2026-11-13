# GTA: Liberty City Stories (Android 2.4.379) di Windows x64

Port ini adalah **static recompilation**: `libGame.so`, `libopenal.so`, dan `libVendor_mpg123.so` dari APK milik Anda diterjemahkan dari ARM64 ke C saat build, lalu dikompilasi ke x64. Tidak ada emulator, interpreter, atau engine yang ditulis ulang. Yang ditulis untuk Windows hanya layanan sistem: berkas, thread, JNI, jendela, OpenGL ES, audio, dan input.

Dokumen lain: [HANDOVER.md](HANDOVER.md) (status teknis), [recomp/README.md](recomp/README.md) (pipeline), [recomp/SETTINGS.md](recomp/SETTINGS.md) (menambah pengaturan dan menu), [STARTUP.md](STARTUP.md) (urutan startup Android).

## Menjalankan

Klik dua kali `build\lcs.exe`. Game langsung terbuka layar penuh di resolusi desktop. Log ada di `build\lcs.log`; kalau gagal start, pesan galat muncul di kotak dialog.

EXE membaca data dari `local\extracted\` (hasil ekstraksi lima APK Anda) dan menyimpan save serta pengaturan game di `local\userdata\`. Ketiga `.so` diperiksa SHA-256-nya; versi lain ditolak.

Paket radio dari OBB lokal sudah tersedia di `local/extracted/split_data_music/assets/data_music.wad` dan didaftarkan otomatis saat startup. `build/lcs.exe --radio-probe` memeriksa seluruh 21 stream memakai decoder MP3 asli, lalu keluar tanpa menjalankan game.

| Opsi baris perintah | Arti |
| --- | --- |
| `--windowed`, `--size W H` | Jalankan di jendela. |
| `--no-audio` | OpenAL memakai backend senyap. |
| `--trial` | Jangan selesaikan gate pembelian (lihat bawah). |
| `--verbose`, `--frames N`, `--watchdog S`, `--shots N pola%04u.bmp`, `--tap F X Y`, `--press F TOMBOL`, `--gltrace` | Diagnosis dan uji otomatis. |

## Kontrol

Gamepad (XInput/DualShock/DualSense lewat SDL3) dipetakan seperti controller Android: A/B/X/Y, L1/R1, L2/R2 analog, D-pad, L3/R3, Start. Begitu tombol gamepad atau keyboard dipakai, tombol sentuh di layar hilang.

| Keyboard / mouse | Tombol game |
| --- | --- |
| W A S D | Stik kiri |
| Gerak mouse (saat bermain) | Stik kanan = kamera |
| Klik kiri / Ctrl kiri | B |
| Klik kanan / E | R1 |
| Shift kiri / Enter | A |
| Spasi | X |
| F | Y |
| Q | L1 |
| Z / X | L2 / R2 |
| Panah, roda mouse | D-pad (roda = kiri/kanan) |
| C / V / klik tengah | L3 / R3 |
| P | Start |
| Esc | Back (pause/menu) |
| F11 | Layar penuh ↔ jendela |
| Alt+F4 | Keluar |

Di menu, kursor mouse bebas dan klik kiri berfungsi sebagai sentuhan. Saat bermain mouse ditangkap untuk kamera. Peralihan mengikuti sinyal `setInUI` dari game sendiri. Pemetaan keyboard masih sementara: aksi tiap tombol mengikuti skema gamepad game dan belum diuji satu per satu.

## Pengaturan (`lcs.ini`)

Dibuat otomatis di root saat pertama jalan.

| Kunci | Bawaan | Arti |
| --- | --- | --- |
| `fullscreen` | 1 | 0 = jendela dengan `width`/`height`. |
| `radar` | `bottom-left` | `top-left` mengembalikan posisi mobile. |
| `radar_margin` | 28 | Jarak radar dari tepi bawah, dalam baris HUD (tinggi layar = 448). |
| `mouse_look`, `mouse_sensitivity` | 1, 1.0 | Kamera dengan mouse. |
| `definitive_menu` | 1 | Menu utama vertikal di kiri; 0 mengembalikan korsel asli. |
| `draw_distance` | 1.0 | Pengali jarak gambar engine, 1.0–3.0. Eksperimental: efek visualnya belum diverifikasi. |

## Kepemilikan dan gate Rockstar

"Start New Game" memanggil gate pembelian SDK Rockstar, yang di Android dijawab berdasarkan status akun. Di Windows status itu tidak bisa diverifikasi. Atas pernyataan pemilik salinan ini bahwa ia memiliki game penuh, bawaannya gate diselesaikan seperti untuk pemilik. `--trial` mengembalikan perilaku gate tertutup. Sesi Social Club selalu offline.

## Build

```powershell
python -m pip install --only-binary :all: --target .tools/py -r recomp/requirements.txt
python recomp/build.py
```

Butuh Python 3.11+, Visual Studio Build Tools dengan komponen clang (`VC\Tools\Llvm`), dan SDL3 VC 3.4.18 di `.tools/sdl3`. Build penuh sekitar 4 menit pada 12 thread; build ulang hanya mengompilasi yang berubah. Jika `local\extracted\` belum ada: `python recomp/tools/extract_apks.py local/apks local/extracted`.

## Menu

Halaman utama dan pause memakai daftar di kiri, logo LCS, serta aksen merah. Saat pause, sisi kanan menampilkan gambar kamera terakhir sebelum game dijeda. Sebelum gameplay, sisi kanan memakai artwork asli. Keyboard (panah/Enter), gamepad, dan klik mouse didukung. Submenu pengaturan, save/load, dan dialog konfirmasi masih memakai tampilan asli.

## Belum ada

- Video intro dilewati.
- Exception C++ tidak bisa di-unwind ke `catch`; `setjmp/longjmp` belum didukung.
- Backend grafis masih OpenGL ES driver (lihat HANDOVER untuk rencana Vulkan).

## Perbaikan refresh rate tinggi

Timer animasi kini mengikuti durasi frame di bawah 10 ms. Batas minimum asli 10 ms membuat cutscene berjalan terlalu cepat pada 120 FPS, sementara audio tetap mengikuti waktu nyata. Koreksi diterapkan sesudah timer asli, dengan penanganan pause dan mode khusus tetap mengikuti game.
