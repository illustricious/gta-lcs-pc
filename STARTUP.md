# Urutan startup GTA Liberty City Stories Android 2.4.379

Peta ini menggabungkan event Android yang sudah tercatat, bytecode APK lokal, disassembly ARM64, dan pengujian JIT di Windows. Targetnya `com.rockstargames.gtalcs` ARM64, versionCode `4256228`, dengan SHA-256 `libGame.so` `d581f68eaa8c6831c3796e241e11deea2be32f442b4e587f85dc34bd2cd856ab`. Semua alamat ELF di bawah adalah alamat virtual relatif ke image.

## Urutan yang terlihat di HP

Resolver Android memilih `LucidOBBDownloaderActivity` untuk intent MAIN/LAUNCHER. Event WindowManager dari satu sesi yang masih ada menunjukkan urutan:

1. `LucidOBBDownloaderActivity` dibuat dan dilanjutkan.
2. `GameActivity` dibuat dan dilanjutkan.
3. `rockstarmobile.ui.FlutterScreenHost` muncul, lalu `GameActivity` kembali dilanjutkan.
4. `MovieActivity` muncul, lalu `GameActivity` kembali dilanjutkan.

Event activity tidak mencatat isi layar Flutter, hasil autentikasi, atau frame menu. Log aplikasi untuk baris startup yang diperiksa sudah tidak tersedia di buffer yang dibaca. ADB hanya membaca resolver dan log yang ada.

## Jalur kode dari launcher ke frontend

| Fase | Jalur dan bukti |
| --- | --- |
| Asset delivery | `LucidOBBDownloaderActivity.checkAssetPack` memeriksa lokasi `data_music` melalui Play Asset Delivery. Jalur normal `onDownloadComplete` mengumpulkan `data_main` dan `data_music`, lalu meneruskan `packNames` dan `packPaths` ke `GameActivity`. Ini menjelaskan nama launcher historis meski instalasi ini memakai split dan paket aset. [Launcher Java](local/startup-jadx/sources/com/rockstargames/gtalcs/LucidOBBDownloaderActivity.java) |
| Load library | `GameActivity.onCreate` menetapkan activity host, lalu pemanggilan `GTAJNIlib` pertama memicu initializer kelas dan `System.loadLibrary("Game")`. `JNI_OnLoad` di `0x56ad5c` meminta JNI 1.6, mencari `CommonAPI`, lalu menginisialisasi lookup Android dan Rockstar Mobile. [Activity Java](local/startup-jadx/sources/com/rockstargames/gtalcs/GameActivity.java) · [GTAJNIlib Java](local/startup-jadx/sources/com/rockstargames/gtalcs/GTAJNIlib.java) |
| Konfigurasi dan layanan | `GameActivity` meneruskan `AssetManager`, direktori file privat, dan peta asset pack. Pada inisialisasi pertama, activity menjalankan `Rockstar.setup`, `initCloud`, lalu `markInitialized`. Activity juga membuat `GameView` dan memasok info perangkat. `Rockstar.setup` memulai layanan SDK, termasuk state, konfigurasi, pemeriksaan instalasi lama pada build yang mengaktifkan GTA+, dan layanan langganan. [Activity Java](local/startup-jadx/sources/com/rockstargames/gtalcs/GameActivity.java) · [Rockstar SDK Java](local/startup-jadx/sources/rockstarmobile/Rockstar.java) |
| Surface dan tick | `GameView` meneruskan lifecycle surface ke antrean `GameGlThread`. `viewOnSurfaceChanged` di `0x56c344` menginisialisasi EGL, menyiapkan surface dari `ANativeWindow`, mengatur viewport dan clear, lalu memanggil `InitGame` di `0x56c784`. `InitGame` menyiapkan platform, display, dan `GTAGameInit`. Saat view fokus, thread memanggil `viewOnDrawFrame`. Jalur render yang aktif mencapai `GTAGameTick`, lalu `OS_ApplicationTick`. [Thread render Java](local/startup-jadx/sources/com/rockstargames/gtalcs/GameGlThread.java) |
| Loading awal | Cabang state 0 pada `OS_ApplicationTick` di `0x53ed80` memiliki jalur ke `LoadingScreen`, pengaturan direktori save, pemuatan pengaturan, dan playlist. Aplikasi dapat menyiapkan tampilan loading sebelum alur `GameLoad` pada state 1. Ini belum menetapkan halaman menu utama. |
| SDK sebelum intro | State 1 memanggil `RockstarGameLoad` di `0x53ef64` setelah kondisi kesiapan internal terpenuhi. Itu memanggil `RockstarJNIlib.GameLoad` → `Rockstar.login` dalam mode `gameload`. Kode Java membuka `FlutterSocialClubLoginScreen` dengan mode `LaunchOptions` jika state SDK belum punya user. Jika user sudah ada, callback dapat berjalan langsung. Penyelesaian memanggil native `GameLoaded`, yang menetapkan flag yang ditunggu state 2 di `0x53eeb8`. [RockstarJNIlib Java](local/startup-jadx/sources/com/rockstargames/gtalcs/RockstarJNIlib.java) · [Rockstar SDK Java](local/startup-jadx/sources/rockstarmobile/Rockstar.java) |
| Intro dan frontend | State 3 memanggil `OS_MoviePlay` di `0x53ef88`. `CommonAPI.PlayVideo` membuka `MovieActivity`, yang membaca resource intro dari APK. State 4 memantau video dan input skip. State 5 memuat tekstur frontend dan memanggil loading screen. Jalur lanjutan bersyarat mencapai `GameStart` dan state 9. `isOnMainMenuScreen` di `0x56c0f0` secara terpisah menguji dua ID halaman menu, sehingga state 9 saja belum membuktikan menu terlihat. [CommonAPI Java](local/startup-jadx/sources/com/rockstargames/gtalcs/CommonAPI.java) · [MovieActivity Java](local/startup-jadx/sources/com/rockstargames/gtalcs/MovieActivity.java) |
| Gate bermain dan load | Ada callsite dari `CMenuManager::ProcessButtonPresses` ke `ShowRockstarGate` di `0x531abc`, dan dari `CMenuManager::Process` ke `ShowRockstarGateBeforeLoad` di `0x52fb38`. Keduanya memakai gate resmi SDK. Java memanggil `StartGame` atau `StartGameBeforeLoad` setelah gate melaporkan sukses. `CheckGate` membaca status trial. [RockstarJNIlib Java](local/startup-jadx/sources/com/rockstargames/gtalcs/RockstarJNIlib.java) |

Ada juga jalur perubahan state SDK ke permintaan frontend. `GameActivity.onStateChanged` meneruskan `Rockstar.pauseGameplay()`, yang berasal dari visibilitas UI SDK. Callback native `HandleStateChanged` di `0x53eca4` memeriksa state dan flag menu sebelum dapat memanggil `RequestFrontEndStartUp`. Argumennya menunjukkan visibilitas UI, sehingga callback ini tidak bisa diperlakukan sebagai bukti login atau pembelian berhasil.

## Jawaban tentang title screen dan gate

Loading awal, intro video, halaman menu, dan gate bermain adalah tahap berbeda. Kode menunjukkan intro menunggu callback asli `GameLoaded` dari alur SDK `gameload`. Log HP konsisten dengan alur ini karena activity Flutter muncul sebelum activity video. Gate bermain atau load dipanggil dari jalur frontend saat aksi terkait diperlukan. SDK juga menyiapkan state akun dan layanan akses lebih awal. Bukti yang ada tidak menetapkan kapan validasi pembelian tertentu terjadi di sesi HP atau apakah menu bisa dicapai melalui seluruh jalur resmi di Windows.

## Keadaan di Windows

Seluruh urutan di atas sekarang dijalankan oleh kode asli hasil static recompilation (`build/lcs.exe`): `JNI_OnLoad`, `InitGame`, state 0 sampai 9 pada `OS_ApplicationTick`, frontend, dan gameplay. Sisi Java diwakili kontrak layanan di [recomp/rt/rt_android.c](recomp/rt/rt_android.c) dan urutan panggilan di [recomp/rt/main.c](recomp/rt/main.c):

- `GameLoad` diselesaikan sebagai sesi offline (`GameLoaded` pada frame berikutnya).
- `PlayVideo` dilaporkan langsung selesai (`isVideoPlaying` = false).
- `ShowGate`/`ShowGateBeforeLoad` dijawab `FinishGate` lalu `StartGame`/`StartGameBeforeLoad`, kecuali dengan `--trial`.
- `CheckGate` (= `Rockstar.inTrial()`) mengembalikan false, kecuali dengan `--trial`.

JADX 1.5.6 menghasilkan keluaran dekompilasi lokal untuk APK dengan 108 error global. Temuan di atas memakai fungsi relevan yang tersedia dan dikorelasikan dengan callsite ARM64 atau event Android bila ada. Percabangan `GameLoad`, `CheckGate`, dan penyelesaian gate juga diperiksa lewat representasi instruksi DEX mode fallback. Keluaran Java bersifat rekonstruksi dan tidak dijalankan. Bukti inventaris ELF REA induk `ev_eb0eeed70dae8511eb256dde336ef926825c32c9d5ceee132a204d46de311f8e` tersedia sebagai [ringkasan REA](local/rea-native-summary.json). [Instruksi DEX RockstarJNIlib](local/startup-bytecode/com.rockstargames.gtalcs.RockstarJNIlib.txt/RockstarJNIlib.java) · [Instruksi DEX Rockstar SDK](local/startup-bytecode/rockstarmobile.Rockstar.txt/Rockstar.java)
