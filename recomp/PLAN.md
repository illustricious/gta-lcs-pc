# Rencana lanjutan port PC GTA LCS

Status per 13 Nov 2026. Urutan = prioritas.

## 1. Menu bergaya asli LCS (bukan gaya "generik")
Tujuan: menu kiri tetap, tapi elemen visualnya diambil dari aset dan tipografi LCS sendiri.
- Pakai font menu engine (font 0/1 milik `CFont`, huruf kapital pricedown-like) dengan warna asli:
  putih untuk baris, merah LCS (`#c8102e`-ish dari texture `fe_*`) untuk highlight, bukan kotak merah polos.
- Highlight memakai sprite "brush stroke" putih yang dipakai halaman native (lihat Load Game: pita putih
  yang memudar ke kanan). Ambil sprite-nya dari texture dictionary frontend, gambar di belakang baris terpilih.
- Judul halaman di pojok kanan bawah seperti halaman native ("Load Game" di atas garis merah), panel kanan
  pakai artwork karakter per halaman (sudah ada di MENU+0x140 dst).
- Nilai pengaturan (FPS, draw distance, dll.) pakai slider/bar gaya native (`CMenuManager::DrawSlider`
  bila ada) alih-alih teks `< 60 >`.
- Halaman Graphics/Controls: tata letak dua kolom (label kiri, nilai kanan) dengan font dan ukuran yang sama
  seperti halaman Display bawaan.

## 2. Input & QoL menu
- Ikon tombol asli (glyph A/B/X/Y dan keycap) di legenda, bukan teks; sekarang teks sudah berganti
  otomatis keyboard ↔ controller (`plat.pad_input`).
- Mouse: klik kanan = Back, scroll di daftar panjang, hover di halaman native juga.
- Ingat baris terakhir per halaman; Esc dari root pause = Resume (sudah), konfirmasi sebelum Exit Game.
- Halaman konfirmasi native (Load/Delete/New Game) default "No" seperti asli; opsional: tombol Enter langsung
  "Yes" bila diinginkan.
- Pause map: tombol keyboard untuk zoom (+/-), pan dengan WASD/drag, toggle legenda.

## 3. Grafis
- Draw distance: sekarang aman (LOD world maks 2x, streaming & cull benda kecil ikut slider). Berikutnya:
  ukur stutter saat mengemudi cepat di 3.0, naikkan `dvStreamerCreateNum*PerFrame` jika sektor telat.
- FSR 1.0 (EASU + RCAS) sebagai pass akhir di lapisan GL, terhubung ke "internal resolution".
- Sharpen (CAS), FXAA/SMAA, anisotropic 16x, MSAA bila framebuffer engine mengizinkan.
- Tahap lanjut: SSAO/color grading butuh depth buffer yang bisa dibaca — cek dulu.
- FSR 2/3, DLSS, ray tracing sungguhan: tidak realistis (tidak ada motion vector, renderer GLES2 lama).

## 4. Stabilitas & teknis
- Semua pemanggilan kode guest dari host (input, suara, tombol native) wajib simpan/pulihkan `CPU`
  — force close menu GAME berasal dari pelanggaran ini.
- Uji regresi otomatis: skrip `--mpress` untuk New/Load/Delete/Settings/Controls + cek crash di log.
- Asap kendaraan pada FPS tinggi (ditunda, perilaku sama di Android).
- Rekam/OBS: dokumentasi capture (GPU yang sama / Game Capture).

## 5. Distribusi
- Satu folder rilis: `lcs.exe`, `lcs.ini` bawaan, panduan ekstraksi data dari APK/OBB milik sendiri.
