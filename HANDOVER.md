# Handover: GTA LCS Android → Windows x64 (static recompilation)

Diperbarui 12 November 2026. Target: `com.rockstargames.gtalcs` 2.4.379 (versionCode 4256228), ARM64, ke Windows 11 x64. Cara pakai ada di [README.md](README.md); dokumen ini untuk orang atau agen yang melanjutkan pekerjaan.

## Status

`build/lcs.exe` boot dari klik dua kali sampai gameplay: title, layar legal, menu utama, Start New Game, cutscene pembuka, lalu mengemudi di kota dengan HUD. Semua logika dan rendering berasal dari kode game asli yang direkompilasi.

| Bagian | Keadaan |
| --- | --- |
| Penerjemah ARM64 → C | 1.566.853 instruksi di tiga library, tanpa sisa. Uji diferensial terhadap Unicorn lulus untuk semua bentuk instruksi kecuali atomik (oracle gagal sendiri). |
| Boot dan gameplay | Diverifikasi dengan input terskrip dan tangkapan framebuffer sampai mengemudi. Misi, save/load, dan sesi panjang belum diuji. |
| Audio | OpenAL Soft asli → layanan OpenSL ES host → SDL3. Paket musik 21 stream dari OBB lokal sudah dipasang; seluruh stream lulus decode mpg123 asli dan engine berhasil membuka WAD. Radio kendaraan belum didengarkan manusia. |
| Grafis | GLES2 asli diteruskan ke konteks OpenGL ES driver lewat SDL3 (diuji di RTX 2070 SUPER, ES 3.2). |
| Input | Gamepad SDL3, keyboard, mouse-kamera. Gamepad dan keyboard belum diuji dengan perangkat nyata; hanya tap terskrip yang diuji. |
| Radar | Dipindah ke kiri bawah lewat widget layout HUD engine. |
| Layar penuh, ikon, tanpa konsol | Bawaan. Ikon diambil dari `ic_launcher.png` APK saat build. |

## Peta workspace

| Path | Isi |
| --- | --- |
| `recomp/` | Seluruh sumber port. Rinciannya di [recomp/README.md](recomp/README.md). |
| `recomp/entries_<P>.txt` | Alamat entry tak langsung yang ditemukan saat runtime (dibuat otomatis bila perlu). |
| `build/lcs.exe`, `SDL3.dll`, `lcs.pdb`, `lcs.map` | Hasil build. `lcs.map` memetakan RIP host ke fungsi guest. |
| `build/recomp/` | C hasil terjemahan dan objek. Aman dihapus; dibuat ulang oleh build. |
| `local/apks/`, `local/extracted/` | APK milik pengguna dan hasil ekstraksinya. Dibutuhkan saat build dan saat jalan. |
| `local/userdata/` | Save (`GTA3LCSsf*.b`), `gta_lcs.set`, berkas kerja game. |
| `local/startup-jadx/`, `local/startup-bytecode/` | Dekompilasi sisi Java; rujukan kontrak JNI. |
| `.tools/py`, `.tools/sdl3`, `.tools/glslang` | Paket Python, SDL3, dan glslang (untuk rencana Vulkan). |
| `lcs.ini` | Pengaturan pengguna. |

Preview dan probe lama (mesh, frontend, loader, probe JIT, ANGLE, JADX, ADB) sudah dihapus dari workspace. Frontend host baru di `recomp/rt/frontend.c` dibuat atas permintaan redesign pengguna.

## Perubahan radio, timing, dan menu

- **Radio:** `obb/main.17.com.rockstargames.gtalcs.obb` memakai format DAWL terenkripsi yang sama dengan WAD. `tools/extract_radio_wad.py` mengambil hanya 21 MP3 yang hash-nya belum ada di data utama ke `local/extracted/split_data_music/assets/data_music.wad` (480.895.295 byte), dengan verifikasi penuh payload. OBB patch tidak dipakai. Startup mendaftarkan `data_music` sebagai asset pack kedua. `build/lcs.exe --radio-probe` menjalankan decoder vendor asli tanpa memulai game atau mengubah save. Bukti: `build/radio-probe-stderr.log` (21 lulus), `build/radio-mount-stderr.log` (WAD dibuka engine).
- **Timing:** `CTimer::Update` asli membatasi langkah animasi minimum 0,5 (10 ms pada 50 unit/detik). Cutscene membaca `ms_fTimeStepNonClipped/50`, sehingga 120 FPS mempercepatnya setidaknya 20%. Pembungkus `rt/timing.c` menghapus batas bawah menggunakan siklus frame dan skala waktu asli, sambil mempertahankan pause/snapshot/chase/upper clipping. `python recomp/tools/test_timing.py` menguji tubuh timer hasil lift asli: 30/60 Hz tidak berubah; 120/144/240 Hz kembali 10 detik animasi per 10 detik terukur. Live gameplay juga mencatat jumlah waktu terukur dan animasi yang sama.
- **Menu:** `rt/frontend.c` menggambar daftar kiri untuk halaman 51/52/69, memakai logo/font/sprite asli dan aksen merah. Halaman pause menyimpan gambar kamera gameplay terakhir di GPU untuk panel kanan; bukan kamera yang terus berjalan selama pause. Submenu dan aksi memakai engine asli. `definitive_menu=0` memulihkan frontend asli. Dukungan hook lifter mempertahankan fungsi asli sebagai `G_527bf4_orig` dan `G_370b30_orig`.
- **Backup:** EXE sebelum perubahan ada di `build/lcs.before-menu.exe`.

QA akhir: `build/verified.log` selesai 5.500 frame dengan exit 0; tangkapan `verified_3000.bmp` memperlihatkan Settings asli, `verified_4800.bmp` menu pause baru dengan gambar kamera, dan `verified_5400.bmp` gameplay setelah Resume. Root baru menunggu `shownLegalScreen` sebelum mengambil alih input; renderer asli tetap dipanggil di bawah lapisan baru untuk mempertahankan perubahan state/fade. Pelepasan tombol yang masuk ke game sebelum root aktif tetap diteruskan agar tidak tertahan.

Rekonstruksi paket musik bila hasil ekstraksi belum ada:

```powershell
python recomp/tools/extract_radio_wad.py obb/main.17.com.rockstargames.gtalcs.obb local/extracted/split_data_main/assets/data_main.wad local/extracted/split_data_music/assets/data_music.wad
```

Alat menolak menimpa output yang sudah ada. Setelah perubahan hook, build harus menjalankan lift sekali (`python recomp/build.py`); perubahan runtime berikutnya cukup `--no-lift`.

## Cara kerja singkat

1. `lift.py` membagi `.text` menjadi fungsi lewat simbol dan FDE `.eh_frame`, lalu `a64.py` menerjemahkan tiap instruksi menjadi C yang beroperasi pada `struct CPU`.
2. Pointer guest = pointer host. Modul dipetakan di base tetap: G `0x200000000`, O `0x210000000`, M `0x218000000`.
3. Panggilan langsung menjadi panggilan C. Panggilan tak langsung lewat `rt_call` dan tabel alamat → fungsi.
4. Impor diikat saat build: ke modul lain (libGame mengambil libc++ dari libopenal), ke shim host `IMP(nama)`, atau ke trap.
5. `rt/main.c` memanggil entry JNI asli dengan urutan `GameActivity`, lalu `viewOnDrawFrame` tiap frame.

## Diagnosis

- `--verbose` mencatat panggilan JNI dan berkas; `--watchdog S` mencetak state guest bila macet.
- Trap dan exception host mencetak register guest, fungsi guest terakhir, dan rantai frame guest.
- `python recomp/tools/difftest.py <lib.so> build/difftest` memeriksa penerjemah setelah `a64.py` diubah.
- Uji tanpa pengawasan: `--size 1280 720 --frames N --tap F X Y --shots N build/shots/x%04u.bmp`, lalu `python recomp/tools/bmp2png.py`.

Urutan tap yang membawa ke gameplay pada 60 fps: `--tap 250 0.5 0.75 --tap 800 0.5 0.75 --tap 1100 0.5 0.75 --tap 1550 0.5 0.78 --tap 1900 0.5 0.78`, lalu lewati cutscene dengan `--tap 4300 0.94 0.95 --tap 4600 0.94 0.95`.

## Keputusan yang sudah diambil

- **Gate Rockstar.** Bawaan menyelesaikan gate seperti untuk pemilik, atas pernyataan dan permintaan pemilik salinan. `--trial` menutupnya. Tidak ada verifikasi.
- **Social Club.** `GameLoad` selesai sebagai sesi offline.
- **Controller selalu ada.** Keyboard/mouse dilaporkan sebagai controller 0 (`setNoJoysticks(1)`), sehingga game memakai skema gamepad.

## Histórico da sessão anterior (antes das correções acima)

Prioritas utama pengguna selesai dan sudah di-build: suara, radar kiri bawah, gamepad, bawaan owned, layar penuh, bersih-bersih, ikon, kamera mouse, dokumentasi.

Prioritas kedua baru dimulai:

- **Jarak gambar (`draw_distance` di `lcs.ini`, 1.0–3.0): kode ada, efek visual belum diverifikasi.** `scale_draw_distance()` di [rt/tweaks.c](recomp/rt/tweaks.c) mengalikan variabel sumber engine (`dvDeviceLodDistanceScale_*`, `dvLODSettings*_*Dist*`, `gFarClip` dibatasi 6000). Run 5.200 frame dengan faktor 3 selesai tanpa crash; tangkapan `build/shots/dd3_5200.bmp` belum dibandingkan dengan faktor 1. Nilai asli yang terukur: skala LOD perangkat Low 2.0 / Medium 2.5 / High 3.0 (disalin engine ke `dvLodDistanceScale`), jarak kendaraan High 120/140/160, ped 70, far clip 2000. `CVisibilityComponents::ms_*Dist` adalah kuadrat turunan; jangan diskalakan langsung.
- `--set KUNCI NILAI` di baris perintah menimpa `lcs.ini` untuk uji.
- Belum disentuh: kait fungsi, menu tertanam, opsi resolusi di menu, Vulkan, DLSS, menu ala Definitive Edition.

## Pekerjaan berikutnya

1. **Verifikasi `draw_distance`**: bandingkan tangkapan faktor 1 dan 3 di lokasi yang sama; jika tidak ada beda, cari pembaca `dvLodDistanceScale` dan pertimbangkan `Render.DetailSettings.ForceUpdate`.
2. **Kait fungsi di lifter:** sudah tersedia untuk timer dan frontend; tambahkan entry ke `HOST_HOOKS` untuk opsi lain. Lihat [recomp/SETTINGS.md](recomp/SETTINGS.md).
3. **Pengaturan di menu Display asli**: resolusi, jarak gambar, posisi radar. Variabel preferensi engine yang relevan sudah diekspor: `CMenuManager::m_PrefsDrawDistance`, `m_PrefsMobileResolution`, `m_PrefsGraphicsDetail`, `m_PrefsRadarMode`.
4. **Menu:** redesign halaman utama/pause sudah diizinkan pengguna dan diimplementasikan; pekerjaan berikutnya dapat memperluas gaya yang sama ke submenu. Panel kanan pause memakai gambar kamera terakhir.
5. **Backend Vulkan.** Batasnya adalah ±170 fungsi GLES2 di [gen_gl.py](recomp/gen_gl.py). Jalur tercepat: ANGLE dengan backend Vulkan sebagai penyedia GLES (SDL3 bisa memakainya lewat `SDL_HINT_OPENGL_ES_DRIVER`). Penerjemah GLES2 → Vulkan tulisan sendiri adalah pekerjaan besar terpisah.
6. **DLSS.** Butuh SDK NGX NVIDIA, renderer Vulkan/D3D, serta depth dan motion vector per piksel yang tidak dihasilkan engine ini. Belum ada jalan tanpa mengubah jalur render engine; DLAA/DLSS sebaiknya dianggap riset, bukan tugas.
7. **Unwinding exception C++** dan `setjmp/longjmp` (dipakai jalur galat libpng/libjpeg).
8. **Video intro** (`CommonAPI.PlayVideo`). Paket `data_music.wad` kini sudah tersedia dan terpasang dari OBB pengguna.

> 13 Nov 2026: folder `obb/` dipindah ke Recycle Bin (tidak dibaca saat runtime; radio sudah ada di `split_data_music`). Kembalikan bila perlu menjalankan `extract_radio_wad.py` lagi.
