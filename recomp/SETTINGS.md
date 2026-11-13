# Adding settings and putting them in the game menu

Gameplay settings work in one of two ways:
- write values to variables that the original engine owns and reads, or
- call original functions.

The menu presentation is redesigned in `rt/frontend.c` (enabled by `definitive_menu`), while actions and submenus still run in the engine. Settings come in three layers, simplest first.

## 1. Host settings in `lcs.ini`

All of these live in [rt/tweaks.c](rt/tweaks.c).

1. Add a field to `struct Settings` and a default line to `DEFAULT_INI`.
2. Read the key in `tweaks_set()`.
3. Apply it in `tweaks_apply()`, which runs once per frame before `viewOnDrawFrame`.

**Example: the radar in the bottom-left corner.**
- `CHud::GetRadarPosX/Y` (`G+0x48411c`, `G+0x48416c`) read the radar widget from the HUD layout object.
- `radar_to_bottom()` rewrites that widget's `y`.
- The engine still draws the radar, the blips and its touch button itself, now at the new position.

**Finding the variable for a new setting:**
- Find functions by symbol in `build/recomp/G/G_exports.txt` (name → address).
- Read the translation: `build/recomp/G/G_<address/0x8000>.c` keeps every original instruction as a comment.
  - An `adrp` + `ldr [x, #off]` pair marks a GOT slot.
  - Its host address is `0x200000000 + VA`.
- Test with `--verbose --shots`.

Addresses are only valid for libGame 2.4.379, whose SHA-256 is checked at start, so no pattern search is needed at run time.

## 2. The engine's DebugTweak variables

The engine has its own tweak system, with 730 variables registered at boot. Examples:

- `Render.Culling.PrefsDrawDistance`, `Render.FarClip`, `Render.DetailSettings.LodDistanceScaleLow/Medium/High`
- `Render.DetailSettings.LODSettings.High.*` (ped/vehicle distances, maximum counts)
- `Render.AnisotropicLevel`, `Perf.GraphicOptions.OffscreenResScale.*`
- `HUD.AspectHack`, `Draw.*`, `Materials.*`

The values are exported as `dv*` symbols, with the value at `+0x4c`. `rt_sym()` finds them.

## 3. Putting options in the original game menu

The original menu is driven by the `aScreens` table at `G+0x81c064`: 72 screens of 2,252 bytes each.

| Offset in a screen | Contents |
| --- | --- |
| `+0` | Screen name, 8 bytes (GXT key, e.g. `FEB_DIS` = Display) |
| `+52` | Menu rows, 20 bytes each, up to 110 rows |

Each row is `int32 action`, `char gxt_key[8]`, `int32`, `int32 target_screen`. Action 2 opens the target screen; action 26 refreshes the save slots (`C_PcSave::PopulateSlotInfo`, `G+0x53e158`) and then opens it.

The menu object is `FrontEndMenuManager` (`G+0x9ec538`), with the current screen at `+0x110`. These original functions handle the rows:

| Function | Address |
| --- | --- |
| `CMenuManager::ProcessOnOffMenuOptions` | `G+0x53204c` |
| `ProcessMultiMenuOptions` | `G+0x5324fc` |
| `GetCurrentLabelValueOnOff` | `G+0x53438c` |
| `GetCurrentLabelValueSlider` | `G+0x534560` |
| `CheckSliderMovement` | `G+0x532654` |

### Host hooks

A host hook wraps a recompiled function and calls the original for every case it does not handle.

1. Add the entry address to `HOST_HOOKS` in `lift.py`. The original implementation is then named `<entry>_orig`, while every caller and dispatch table keeps the public name.
2. Provide a host wrapper under the public name.
3. Run the build with lifting once after changing the table.

Hooks in use: `G_370b30` (timer), `G_527bf4` (frontend), `G_41e0cc` (ped velocity at small steps), and the particle updates.

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
- Menu sounds are the engine's own front-end sounds (see below).
- Draw distance (`draw_distance`, default 2.0, 1.0–3.0) scales only things that do not depend on sector streaming:
  vehicle/ped distances (`dvLODSettings*`), the world cull fade (`dvWorldCullFadeSettings*`: sizes divided by k^2,
  `_MinDist` multiplied by k; at 1.0 even a phone booth in front of the player could vanish) and `gFarClip`.
- Do not scale `CRenderer::ms_lodDistScale` (the native slider's) together with `dvDeviceLodDistanceScale_*`:
  `CCamera::Process` multiplies both (the first version squared the factor). Raising the device scale or the streamer
  reach (`dvStreamerCameraPreStreamDist`, `dvStreamerSpeedPreStreamTime`) made detailed buildings pop in, so both
  stay at the original values.
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
