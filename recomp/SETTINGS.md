# Menambah pengaturan dan menanamkannya ke menu game

Pengaturan gameplay menulis nilai ke variabel yang memang dimiliki dan dibaca engine asli, atau memanggil fungsi asli. Redesign presentasi menu telah diizinkan pengguna: `definitive_menu` mengaktifkan `rt/frontend.c`, sementara aksi dan submenu tetap memakai engine. Ada tiga lapis pengaturan, dari yang paling sederhana.

## 1. Pengaturan host di `lcs.ini`

Semua ada di [rt/tweaks.c](rt/tweaks.c).

1. Tambah field di `struct Settings` dan baris bawaan di `DEFAULT_INI`.
2. Baca kuncinya di `tweaks_load()`.
3. Terapkan di `tweaks_apply()`, yang dipanggil sekali per frame sebelum `viewOnDrawFrame`.

Contoh yang sudah ada, radar kiri bawah: `CHud::GetRadarPosX/Y` (`G+0x48411c`, `G+0x48416c`) membaca widget radar dari objek layout HUD. `radar_to_bottom()` menulis ulang `y` widget itu; engine tetap menggambar radar, blip, dan tombol sentuhnya sendiri di posisi baru.

Cara mencari variabel untuk pengaturan baru:

- Cari fungsi lewat simbol: `build/recomp/G/G_exports.txt` (nama → alamat).
- Baca terjemahannya: berkas `build/recomp/G/G_<alamat/0x8000>.c` memuat tiap instruksi asli sebagai komentar. Pola `adrp` + `ldr [x, #off]` menunjukkan slot GOT; alamat host-nya `0x200000000 + VA`.
- Uji dengan `--verbose --shots`.

Alamat berlaku hanya untuk libGame 2.4.379 (SHA-256 dicek saat start), jadi tidak perlu pencarian pola saat runtime.

## 2. Variabel DebugTweak milik engine

Engine punya sistem tweak sendiri (730 variabel terdaftar saat boot), misalnya:

- `Render.Culling.PrefsDrawDistance`, `Render.FarClip`, `Render.DetailSettings.LodDistanceScaleLow/Medium/High`
- `Render.DetailSettings.LODSettings.High.*` (jarak ped/kendaraan, jumlah maksimum)
- `Render.AnisotropicLevel`, `Perf.GraphicOptions.OffscreenResScale.*`
- `HUD.AspectHack`, `Draw.*`, `Materials.*`

Saat boot engine mencari `DebugTweak.xml` di direktori kerja (`local/userdata/`) dan melaporkan "Persistent variables supported". Format berkasnya belum dipetakan; itu langkah pertama untuk pengaturan jarak gambar. Parser-nya ada di sekitar `lgDebugTweak*` dan `dvDebugVarBase::InternalInit` (`G+0x72d1e0`).

## 3. Menanamkan opsi ke menu game asli

Menu asli digerakkan tabel data `aScreens` (`G+0x81c064`, 72 layar × 2.252 byte):

| Offset dalam layar | Isi |
| --- | --- |
| `+0` | Nama layar, 8 byte (kunci GXT, mis. `FEB_DIS` = Display) |
| `+52` | Baris menu, masing-masing 20 byte, sampai 110 baris |

Tiap baris: `int32 aksi`, `char kunci_gxt[8]`, `int32`, `int32 layar_tujuan`. Layar Display (indeks 6) berisi `FED_BRI`, `FED_SHD`, `FED_REF`, `FED_GRD`, `FED_GD`, `FED_SUB`, `FEMP_R`, `FEDS_TB`.

Fungsi asli yang memproses baris: `CMenuManager::ProcessOnOffMenuOptions` (`G+0x53204c`), `ProcessMultiMenuOptions` (`G+0x5324fc`), `GetCurrentLabelValueOnOff` (`G+0x53438c`), `GetCurrentLabelValueSlider` (`G+0x534560`), `CheckSliderMovement` (`G+0x532654`). Objek menu: `FrontEndMenuManager` (`G+0x9ec538`).

Langkah menambah opsi baru supaya muncul di menu asli, bukan GUI terpisah:

1. **Baris.** Setelah `rt_run_init_arrays()`, tulis baris baru ke slot kosong layar yang dituju: kode aksi baru (di luar rentang yang dipakai game), kunci GXT baru, layar tujuan.
2. **Teks.** Kunci baru tidak ada di `english.gxt`. Kait `CText::Get` supaya kunci buatan mengembalikan string UTF-16 dari host; kunci lain diteruskan ke fungsi asli.
3. **Aksi.** Kait `ProcessOnOffMenuOptions`/`ProcessMultiMenuOptions`: kode aksi baru ditangani host (ubah `Settings`, tulis `lcs.ini`), sisanya diteruskan.
4. **Nilai yang tampil.** Kait `GetCurrentLabelValueOnOff`/`GetCurrentLabelValueSlider` dengan cara yang sama.

Kait di sini berarti fungsi host yang membungkus fungsi hasil recompile dan memanggil aslinya untuk kasus yang bukan miliknya. Dukungan ada di `HOST_HOOKS` pada `lift.py`: implementasi asli dinamai `<entry>_orig`, sedangkan semua pemanggil dan tabel dispatch tetap memakai nama entry publik. Contoh yang dipakai sekarang: `G_370b30` (timer) dan `G_527bf4` (frontend). Tambahkan alamat entry baru ke tabel itu dan sediakan pembungkus host dengan nama publik. Jalankan build dengan lift sekali setelah perubahan tabel.

## Frame rate, menu and graphics (host-drawn menu)
- `fps_cap` is 30, 60 or 120 (menu) and never goes above 120. `timing.c` removes the engine's 0.5 step floor
  (the step follows real time, taken from the host's ns timer) and `G_41e0cc` patches `CPed::CalculateNewVelocity`:
  for step < 0.5 the engine used `shift * 0.5` instead of `shift / step`, a 4x drop at the 0.5 boundary that made
  pedestrians crawl at 120 fps. Verified: autowalk speed at 120 fps matches 60 fps; the engine clock tracks real time.
  Cutscene pacing verified (NEW GAME intro: cutscene clock runs 1.00 s per real second at 60 and 120 fps; the
  `cclock` field in the `--verbose` diag line). Particles: `CParticle::Update` and `CParticleObject::UpdateAll`
  are hooked (`gated()` in timing.c) to run at most ~60 times a second with the accumulated step, because parts of
  them are per-call rather than per-step (e.g. a fixed -0.15 velocity change per call). Vehicle smoke itself was
  not observed in a test.
  Diagnostics: `LCS_STEP=<n>` forces a constant step; `--autowalk` rotates the stick every 2 s; `--mpress REL BTN`
  presses a button relative to the first host menu draw (menu appears at different frames per run).
- Menu (`frontend.c`): every left list is a `Row` array. Root (engine pages 51/52/69), host SETTINGS list
  (GRAPHICS, CONTROLS, GAMEPLAY, AUDIO, DISPLAY, LANGUAGE), host GRAPHICS values, and the engine's GAME page (2):
  its rows come from the engine's `aScreens` table (page stride 2252, entries at +56 stride 20) with labels from
  `CText::Get`, and activating one selects the native option (MENU+0xc) and sends a real A press, so save/load/new
  game run the original code. Leaf pages, Stats, Brief and Help stay native.
  Pause: the right pane shows the engine's own map (`CRadar::DrawRadarMap` + `RadarMap::Render`), shifted into
  the pane through `rt_gl_dx` (added to every glViewport/glScissor by the generated GL thunks). The map must be
  deactivated (`RadarMap::Deactivate`) before a Back action is queued. Mouse: hover selects, wheel zooms the map
  (`RadarMap+0x74`, 1..3), dragging on the map is forwarded as touch input.
- Menu sounds are synthesised on a separate SDL audio stream (the mobile menu has none).
- Draw distance: `CRenderer::ms_lodDistScale` (written by the native Draw Distance slider) is the world LOD factor;
  it is scaled together with the dv* LOD sources and `gFarClip`. Objects that popped in were mostly dropped by the
  world cull fade (`dvWorldCullFadeSettings*_NearSize/FarSize`, a minimum object size; e.g. a phone booth right in
  front of the player vanished at 1.0): sizes are divided by k^2 and `_MinDist` multiplied by k. Sector streaming reach
  (`dvStreamerCameraPreStreamDist` 50, `dvStreamerSpeedPreStreamTime` 150) is multiplied by k. Default is 2.0.
- Never scale `CRenderer::ms_lodDistScale` together with `dvDeviceLodDistanceScale_*`: `CCamera::Process`
  multiplies both into the LOD distance multiplier (the old code squared it: 9x at 3.0, objects popping).
  The device scale gets `1 + (k-1)/2` (2x at 3.0); ms_lodDistScale belongs to the native slider.
- GAME rows run the `aScreens` entry action directly (action 2 = open target, 26 = `C_PcSave::PopulateSlotInfo`
  then open target: Load 8, Delete 9, New Game 10) instead of faking an A press, which the engine resolved
  against its reset option (Load started a new game) and which then confirmed the next page as well.
- Any host call into guest code (native buttons, sounds) must save/restore the `CPU`: `release_native()` before
  `DrawFrontEndNormal` clobbered x0 (the menu) and crashed native pages.
- Menu sounds are the engine's (`cDMAudio::PlayFrontEndSound` 0xc3 move, 0xc4 select, 0xc5 back), queued from
  input and played from the next menu draw.
- Pause map: shift `MAP_SHIFT` 0.09 puts the map screen's legend column (37% of its width) exactly under the left
  panel, so nothing needs covering; the hint text/icons at the bottom are hidden by capturing the pixels under them
  (`strip_capture`, before the font flush) and drawing them back in `frontend_present`.
- Internal resolution writes the three `dv_OffscreenResScale*` floats and sets the apply flag at MENU+0x266.
- Title prompt: GXT key `SPLASH` ("Tap To Continue") is rewritten in place to "Press Any Key".

## Menu notes (latest)
- SETTINGS is the engine's page 41 drawn as a host list, so native Back from leaf pages lands on it (no native flash).
- CONTROLS is a host page: mouse look/sensitivity, what each mouse button presses, and one rebindable key per pad
  button. Stored as `bind_<button>=<SDL scancode>` and `mbind_<0..2>=<button or -1>` in lcs.ini (`host_bind_*` in main.c).
- The native A press used by GAME rows is released from `G_527bf4` on every page; before, it stayed held on native
  leaf pages (load/delete lists) and those pages ignored input.
- The pause map's legend text is flushed before the panel is drawn; its unshifted gamepad hint icons are blanked
  with a scissored clear in `frontend_present`.

## Display modes and window scaling (13 Nov 2026)
- The engine cannot rebuild its render targets after the first surface size (viewOnSurfaceChanged mid-game
  leaves it drawing at the old size in a corner). So it always renders at the desktop resolution into a host
  framebuffer (`rt/present.c`): the generated GL thunks redirect every `glBindFramebuffer(0)` to
  `rt_gl_default_fb` and report it back as 0 from `glGetIntegerv(GL_FRAMEBUFFER_BINDING)`. `present_frame()`
  scales it into the window, letterboxed; `present_map_point()` maps mouse/touch coordinates back.
- `display_mode=windowed|borderless|exclusive` (old `fullscreen=` still read), Alt+Enter / F11 toggle,
  Graphics menu row. Windowed is resizable; the size is saved to `width`/`height`.
- Focus loss: all held buttons are released (remote desktop / alt-tab can swallow key-ups); with
  `pause_on_focus_loss=1` the pause menu opens and the frame cap drops to 30 while in the background.
- Test helper: `--resize FRAME W H`; screenshots are now taken from the window after scaling.

## Draw distance, conservative (13 Nov 2026)
- World LOD swaps (`dvDeviceLodDistanceScale_*`) and sector streaming (`dvStreamer*`) are no longer scaled:
  raising them made the engine want detailed buildings beyond what streaming had loaded (pop-in).
  The setting now scales only vehicle/ped distances, the small-object cull fade and `gFarClip`.
- The pause pane no longer shows a copy of the last game frame (it flashed as a frozen snapshot);
  `pause_on_focus_loss` defaults to 0 (remote desktop / capture tools took focus and opened the menu).
