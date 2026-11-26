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

## Engine settings pages drawn by the host (18 Nov 2026)
- AUDIO (5), DISPLAY (6), GAME SETTINGS (68) and LANGUAGE (7) are drawn with the host menu style, so every
  SETTINGS page now looks the same. Rows come from `aScreens`. Values come from the engine:
  - sliders: `GetCurrentLabelValueSlider` (brightness 0..512, volumes 0..127);
  - all other value text: `CFont::PrintString` (0x4772fc, a host hook) records the strings while the native page
    draw runs instead of drawing them, and the text printed right of each label becomes its value. Rows the game
    does not print on this device (REFLECTIONS) are hidden.
- Changes go through the engine with the row selected (`MENU+0xc`):
  - sliders: `CheckSliderMovement(±1)`;
  - on/off: `ProcessOnOffMenuOptions`, falling back to `ProcessMultiMenuOptions(dir)` when the value does not
    change (targeting mode);
  - multi-value options: `ProcessMultiMenuOptions(dir)`;
  - languages: `ChangeLanguage(action - 18)` (0 English, 1 French, 2 German, ...);
  - RESTORE DEFAULTS: the engine's own A press.
- Leaving a page calls `CMenuManager::SaveSettings`, as the native Back does. SIGN OUT OF SOCIAL CLUB is hidden
  (the session is offline).
- Menu text is UTF-8 on the host side and converted back to UTF-16 when drawn, so French accents and Cyrillic show.
- `LCS_DUMP_MENU=1` logs every row of every menu screen with its action, GXT key, values and target.

## Radar in Rust (18 Nov 2026)
- `radar_scale` (lcs.ini, Graphics → RADAR SIZE, 75-150%) and the bottom-left placement are applied by
  `lcs_radar_layout()` in `recomp/engine/src/hud.rs`, which moves and resizes the HUD layout's radar widget.
- The CHud radar getters and five CRadar helpers are Rust ports, shadow-verified against the originals.

## CTimeCycle in Rust; why buildings swap LOD to detail (19 Nov 2026)
- CTimeCycle (ForceUpdate, UpdateArrays, FillGaps, extra colours) is rewritten in `recomp/engine/src/timecycle.rs`
  and shadow-verified (0 mismatches over 2M calls).
- Building LOD swaps cannot be moved by settings. The city is drawn by `cWorldStream::Render` from the streamed
  sector of the camera's cell; each sector was built offline (`cWorldGeom::BuildCell`/`SetLodDistance`, which only
  run in the world builder) with detailed models for nearby buildings and LOD models beyond. At run time an instance
  is only sphere-culled (`cWorldStream::Render(sGeomInstance*)`); no distance is compared, and
  `TheCamera.LODDistMultiplier` (CCamera+0xf0) is not read by the world stream. Moving the swap needs re-baked
  sector data (detailed instances from neighbouring cells merged in, their LOD instances removed).
- Debugging: `LCS_DV="dvName=value,..."` forces DebugTweak floats every frame (rt/tweaks.c).

## Per-action bindings and CCamera in Rust (19 Nov 2026)
- 16 CPad action getters are Rust ports (`recomp/engine/src/pad.rs`). Each can be bound to its own key separately
  on foot and in a vehicle: `foot_sprint=Left Shift`, `car_accelerate=W`, `foot_move_x=A,D` (lcs.ini). Unbound
  actions keep the gamepad mapping. Checked: with `foot_move_x/y` bound and not pressed, the scripted stick no
  longer walks the player (speed 0 instead of 3-5 u/s).
- Menu: SETTINGS > CONTROLS > ON FOOT KEYS / VEHICLE KEYS. Enter (or A) then a key or mouse button binds the action,
  Delete clears it (back to the gamepad mapping), Esc cancels; CLEAR ALL resets the page. A key is unique within a
  context, so binding it removes it from the other action. Each change is saved to lcs.ini at once.
- Not bound yet: targeting and camera look on foot (their getters are still recompiled C).
- 26 CCamera/CCam queries and setters are Rust ports (`camera.rs`); the camera modes (`CCam::Process_*`,
  `CCamera::Process`, `CamControl`) are still recompiled C.

## World cull fade now follows draw distance; steering bindings (19 Nov 2026)
- Building detail popping in comes largely from `cWorldStreamEx::DrawPrimitiveRef`: beyond MinDist every world
  mesh whose screen size (`CalcScreenSize`, 0x563238) is under a threshold is faded out. The thresholds live in
  `gDeviceDetailSettings` (+0x00 world, +0x34 model; three levels of {MinDist, Range, NearSize, FarSize}), copied
  from the dv sources once at start-up, so scaling `dvWorldCullFadeSettings*` never reached them. This PC build ran
  the Low level (MinDist 50, size 0.09). `scale_world_cull` in rt/tweaks.c now writes the working copies:
  MinDist and Range x k^2, sizes / k^2 (k = draw_distance).
- Measured at an open spot (1300, -50): draw_distance 1.0 kept 76,674 and culled 22,854 meshes per second;
  2.0 and 3.0 culled none.
- The sector-level LOD swap (which buildings a cell's sector holds in detail) is still baked into the world data.
- `--verbose` logs `diag: world meshes kept N, culled by size M` once a second (CalcScreenSize host hook).
- Vehicle steering, lean, horn and look left/right/behind are Rust CPad ports with bindings (VEHICLE KEYS page).

## Two-pane menu, MAP row, GAME back (19 Nov 2026)
- Below the root, the left pane shows the parent list (the row that opened the view marked) and the right pane the
  view's own rows and values (`parent_rows`, `draw_list`). At the root, the right pane previews what the highlighted
  row opens (SETTINGS, GRAPHICS, CONTROLS, key pages) or shows the artwork.
- Left goes back to the parent list, Right opens a submenu; mouse and touch hit the pane the list is in.
- Pause menu: MAP is its own row. The map is drawn on the right while MAP is highlighted (wheel zoom, drag to move).
  The map mode stays on in the pause menu even when not drawn: it swaps the native page's gamepad icons for the map's,
  which `strip_capture` hides.
- Back from GAME navigates straight to the root list. The native Back used before was ignored after visiting Load or
  Delete, which left the title menu stuck on GAME.
- Installer version 0.2.0.

## No audio device, field of view (19 Nov 2026)
- A PC without a playback device aborted at start: the OpenSL player was refused, OpenAL threw
  `al::backend_exception: Device reset failure` and the game called abort(). The player now runs silently
  (buffers are consumed at the real-time rate and discarded), and SDL starts without its audio subsystem when no
  audio backend exists. The fatal message box shows the real log path.
- FIELD OF VIEW (GRAPHICS, lcs.ini `fov_scale` 0.9-1.4): `CDraw::SetFOV` is a Rust port (camera.rs) that widens
  camera FOVs of 50 degrees and up, capped at 120; zoomed views keep theirs.
- Android on-screen controls: `Touchscreen::Render` (0x543b7c) is skipped unless lcs.ini sets `touch_controls=1`; touch input still works.

## Camera distance (19 Nov 2026)
- The game runs `CCam::Process_FollowPed_SA` (0x318b8c, 1353 instructions) on foot and `Process_FollowCar_SA` (0x3173ac,
  1528) in vehicles; `Process_FollowPed` and `Process_BehindCar` are never called (measured with call counters).
  Both are still recompiled C: porting them to Rust is the next big camera step.
- `Process_FollowPed_SA` reads its distances from a table of three 0x3c-byte entries (GOT +0x468, first entry
  `0.6 2 0.15 2 4 0.8 ...`). Fields 1, 3 and 4 are the follow distances. `camera_distance` (lcs.ini, GRAPHICS > CAMERA
  DISTANCE, 80% .. 160%) scales them from the start-up values (`apply_camera_distance` in rt/tweaks.c).
- Checked on foot at a fixed spot: 1.6 puts the camera visibly farther back and higher. In vehicles the camera is
  unchanged (its parameters sit in separate arrays, GOT +0x418..0x450, not mapped yet).

## Default keys per context (19 Nov 2026)
- Before, the keyboard reached the game only as pad buttons, the same on foot and in a vehicle, so Space (= Cross) jumped
  on foot but accelerated/braked in a car. Now both contexts have their own default keys (rt/main.c `act_defaults`):
  - On foot: A/D and W/S move, Left Shift sprint, Space jump, F enter vehicle, Mouse1 attack, E / Q next / previous weapon.
  - In a vehicle: W accelerate, S brake/reverse, Space handbrake, A/D steer, Up/Down bike lean, F exit, H horn,
    Q / E look left / right, V look behind.
- lcs.ini `foot_<action>=` / `car_<action>=` override a default; an empty value unbinds the action (the pad button
  applies again). In the key pages, RESET ALL restores that page's defaults; Delete clears one action.
- Checked on foot: with the scripted stick pushed and no key held the player stays still (0 u/s); holding W walks at a
  steady 5.3 u/s. In-vehicle keys are not checked end to end (a gamepad on this PC adds noise to scripted drives).
- Test option `--key FROM TO NAME` holds a key between two frames for the per-action bindings.

## Dissolve support in the shaders (20 Nov 2026, step 1 of the sector cross-fade)
- Every guest fragment shader now carries `uniform mediump float uLcsDissolve` and drops a pixel when an
  interleaved-gradient noise value is >= the uniform (screen-door dither), so geometry can fade without alpha blending.
  rt/dissolve.c patches the source in the glShaderSource thunk, registers each linked program, and sets the uniform on
  all of them from `rt_dissolve_set(v)` (1.0 = draw everything, the default; the game renders as before).
- Test: `LCS_DISSOLVE_TEST=0.5` dithers the whole scene at 50%; `LCS_DUMP_SHADERS=1` writes the patched sources.
- Next steps of the cross-fade: keep the old world sector and its model slots alive after a swap, then draw it a second
  time with a falling dissolve value while the new sector draws normally.

## World sector cross-fade (`world_fade`, EXPERIMENTAL, default 0 = off) (20 Nov 2026)
- `world_fade=<seconds>` in lcs.ini (or env `LCS_WORLD_FADE`) enables rt/world_fade.c: SwapToStreamedSector keeps the old
  sector alive (RemoveOldSectorModels, 0x39bfd0, is deferred to the end of the fade) and cWorldStream::Render (0x39ef48)
  is run a second time with the old sector temporarily installed (fields +0x30..+0x5f swapped with +0x78..+0xa7).
  The new sector draws the pixels where the dither noise < t, the old one the complement (negative `uLcsDissolve`).
- Status: NOT verified. It runs through six swaps without crashing, but one run with both passes active showed the
  ground and buildings missing and dithered after the fades had ended (the baseline at the same frames was clean).
  Deferral alone (`LCS_WORLD_FADE_DBG=1`) was clean. Second pass restricted to Render(0) or to Render(!=0)
  (`LCS_WORLD_FADE_DBG=4` / `8`) looked clean in screenshots, so the cause is not isolated (Render rewrites instance
  data in place and fills the alpha queue, so a second pass has side effects; load timing of the harness also varies
  between runs). No screenshot sequence of a swap without a pop was obtained, since the scripted runs spend the swaps
  landing from the teleport. Leave it off until that is checked in a normal walk or drive.
- Debug: `LCS_WORLD_FADE_DBG` bits 1 = skip the second pass, 2 = do not defer, 4 = second pass only for Render(0),
  8 = only for Render(!=0); `LCS_DISSOLVE_LOG=1` logs every dissolve value change; `--verbose` logs `swap:` lines.

## Several pads and unmapped joysticks (20 Nov 2026)
- Each connected pad now keeps its own axes and the game sees the one pushed furthest, so a second device resting at
  0 no longer overwrites a working left stick (before, all pads wrote the same four axis slots).
- Devices SDL has no gamepad mapping for are opened as plain joysticks (axes 0/1 = left stick, 2/3 = right stick,
  hat = d-pad, buttons 0..9 = A B X Y L1 R1 Back Start L3 R3). `--verbose` logs every device name with its axes/hats/buttons.
- Not tested with the reporting user's stick (their left analog does not respond); the log line names the device.

## Traffic and pedestrian range follows draw_distance (20 Nov 2026)
- `rt/population.c` hooks the device-detail getters GetPopulationCullRange (+0x30), ...OffScreen, ...FarAway,
  GetPopulationAddMinRadius, GetPopulationAddMaxRadius and GetDriverHighDetailRange and multiplies the result by
  `draw_distance` (1.0..3.0), so cars and peds are created and kept farther out.
- Checked: `--verbose` logs the scaled values (AddMaxRadius 50 -> 100 and DriverHighDetailRange 25 -> 50 at 2.0).
  Not measured in play: the render-list entity count in a short scripted run is too noisy to show the difference.
  Shadow distances are literals inside CShadows and are not scaled yet.

## Gamepad layout: modern GTA preset (20 Nov 2026)
- `pad_preset=modern` (default) binds, through the same per-action getters as the keyboard keys (pad.rs), on foot:
  A = sprint, X = jump, RT = attack, Y = enter/exit; in a vehicle: RT = accelerate, LT = brake/reverse, RB = handbrake,
  Y = enter/exit, L3 = horn, LB = look behind. Sticks and the remaining buttons keep the game's own mapping.
  `pad_preset=classic` keeps the original Android mapping for every action.
- Override one action in lcs.ini: `padfoot_<action>=<input>` / `padcar_<action>=<input>`, input = A B X Y LB RB LT RT L3 R3
  Back Start Up Down Left Right (empty = back to the original pad button). Actions are the same names as `foot_*`/`car_*`.
  The pad binding is OR-ed with the keyboard binding. Triggers count as pressed above ~27% travel.
- Not checked with a physical pad: only that the build starts, plays and walks with the keyboard unchanged. There is no
  menu page for pad bindings yet (ini only).

## Shadow distance follows draw_distance (24 Nov 2026)
- Shadow ranges are float literals inside CShadows, so `recomp/lift.py` (`CONST_SCALE`) rewrites the instructions that load
  them to `rt_scale_f32(bits, kind)` (rt/shadows.c), scaled by `draw_distance` (1.0..3.0; k for a distance, k^2 for a
  squared one): StoreShadowForVehicle cut-off 144 u (big) / 18 u (small) with their fade distances, and the squared
  cut-off of StoreShadowForPedObject (13 u).
- Checked: five patched literals in the generated code, the build starts and plays a scripted run without a trap.
  Not compared visually. Left alone because their meaning is unclear: the 40 / 50 unit constants (static and permanent
  shadows, shadow sector lists) and CDynamicShadows.

## Far clip and fog start follow draw_distance (26 Nov 2026)
- The time cycle's far clip (CTimeCycle +0x2b8c, about 1010 units at 05:40) and fog start (+0x2b90, about 36) are
  multiplied by `draw_distance` in `recomp/engine/src/timecycle.rs` (`scale_far_clip`, skipped while verifying).
  `LCS_FAR=<factor>` overrides it; `--verbose` logs the unscaled values.
- Measured effect is small: about 2% more world meshes drawn at (1300, -50) with factor 3 than with 1, and no visible
  new buildings in the screenshots. The sector's own LOD lists only span about 600..1000 units around the cell centre,
  so beyond the original far clip there is little left to draw. Showing more city needs the LOD lists of neighbouring
  sectors loaded and drawn as well (a streaming change, not a setting).

## Units and vehicle speed (from cHandlingDataMgr::ConvertDataToGameUnits, 26 Nov 2026)
- Velocities are vectors in world units per 1/50 s tick. The handling max velocity is divided by 180 (= km/h / 3.6 / 50),
  and acceleration is multiplied by 0.0004 (= 1/50^2), so 1 world unit is treated as 1 metre.
- CCarCtrl uses 5.0 literals in its lane placement code, matching the 5 m lane width of this engine family (not traced
  literal by literal); real lanes are about 3.0..3.7 m.

## Would neighbouring sectors' LOD lists show more city? No (measured 26 Nov 2026)
- `LCS_SECTOR_DUMP=1 --verbose` logs, after each sector swap (world_fade off), the lists of the new sector: count, models
  not in the previous sector, and the bounds of their positions (relative to the cell centre).
- Adjacent cells (100 x 87 units apart) share almost all LOD chunks: list 1 gained 4, 12 and 6 models out of about
  210 (list 0: 0 or 1 of 17). A diagonal two-cell jump gained 41 of 234.
- At cell 31,20 list 1 spans world x 534..1659, y -1360..246 and list 0 x -486..719: the current sector already holds
  LOD geometry for the whole area around the player and beyond the original far clip in most directions.
- So a loader for neighbouring sectors was not built: it would add a few percent of chunks, some of them LODs of areas
  the current sector draws in detail (overlap), at high crash and memory risk. What still limits the view is the
  detail-to-LOD boundary (about 300 units, baked), not missing far geometry. If far buildings are visibly absent
  somewhere, that needs the position and a screenshot to trace (culling), not more sectors.

## Car and ped spawn distance: TheCamera.GenerationDistMultiplier (Rust, 26 Nov 2026)
- CCarCtrl spawns and removes cars at literal distances (140, 190, 70 units ...) multiplied by CCamera+0xf4, and
  CPopulation::ManagePopulation multiplies its add radius and cull range by the same field. CCamera::Process (0x328188)
  is hooked in `recomp/engine/src/camera.rs`: after the original, +0xf4 is multiplied by `draw_distance` (max 2.5).
  `LCS_GEN=<factor>` overrides it.
- This replaces rt/population.c (it scaled only the ped getter radii, which would now be applied twice; cars never
  moved).
- Measured with `--verbose` (`diag: vehicles ...`, 40 samples standing in Harwood), factor 1 vs 2: vehicles within
  80 units 8.9 -> 6.6, 80-160 units 4.2 -> 4.6, 160-400 units 2.2 -> 6.7. Cars exist farther out; the vehicle count
  does not grow, so the street right around the player is a bit emptier. Peds were not measured.
- Appearing and disappearing is still instant (no fade-in added); it now happens farther away.

## Left stick ignored; analog triggers (26 Nov 2026)
- Cause of the dead left stick: the default key bindings (foot_move_x/y = A,D / W,S; car_steer_x/y) replaced the
  stick value even with no key held (host_action_axis returned 0). Now a pushed stick (more than 12%) counts whenever
  no bound key is held. Likewise, an action that only has a key binding reads the pad button again while a gamepad is
  the last used device. Checked with a scripted stick (`--axis`): the player walks at 5.3 u/s with the defaults bound.
- RT / LT on accelerate and brake are analog: the trigger travel (5% dead zone) is passed as 0..255; keys and buttons
  still give 255. Not checked with a physical pad.

## Handling, transmission and animation playback in Rust (26 Nov 2026)
Faithful ports, no behaviour change (the handling override factors tried the same day were removed again).
- `handling.rs`: cHandlingDataMgr::ConvertDataToGameUnits.
- `transmission.rs`: cTransmission InitGearRatios, CalculateGearForSimpleCar, CalculateDriveAcceleration.
- `animation.rs`: CAnimBlendAssociation UpdateBlend, UpdateTime, SetCurrentTime, SyncAnimation, Start, SetBlend,
  SetBlendTo; CAnimBlendNode UpdateTime, NextKeyFrameNoCalc.
- Verified with `LCS_RS_VERIFY=1`: scripted sessions reach the handling and transmission ports and
  CAnimBlendAssociation::Start (5.1M port calls overall, 0 mismatches). The other animation ports were not reached in
  play; `LCS_PORT_TEST=1` (rt/porttest.c) feeds them 4000 rounds of random records at start-up: 0 mismatches. Paths
  that call back into the game (key frame lookup, delete, callbacks) are not covered by either.
- Per-frame skeleton update (26 Nov 2026): RslAnimBlendElementGroupUpdateAnimations (0x2e8e20) is also ported
  (blending, node collection, key frame sync, time advance). Shadow verification cannot run it twice on live skeletons,
  so only records without bones, deletes or callbacks are compared (4000 random rounds in porttest.c, 0 mismatches);
  the live path is covered only by a scripted walk / run session without a crash and with normal-looking animation.
- Not ported: the pose evaluation itself (the ForAllNodes callbacks, CAnimBlendNode::Update, key frame
  interpolation) and vehicle physics.

## Screen effects, colour looks and HUD parts (26 Nov 2026)
- SETTINGS > SCREEN EFFECTS and SETTINGS > HUD (menu), or lcs.ini / `--set`. Everything defaults to the original look.
- Screen effects (rt/present.c, one pass on the finished frame, so HUD and menus are affected too): `fx_sharpen` 0..1
  (contrast-adaptive), `fx_fxaa` 0/1, `fx_vignette` 0..1, `fx_brightness` / `fx_contrast` 0.7..1.4,
  `fx_saturation` 0..1.6.
- `look=original|natural|clear|moody` (Rust, timecycle.rs `apply_look`, off while verifying): natural keeps 10% of the
  weather colour filter tint (less yellow), clear is natural with fog starting 3x farther, moody is 75% ambient light.
- HUD parts: `hud` (everything CHud::Draw draws, including radar and on-screen messages), `hud_clock`, `hud_money`,
  `hud_health`, `hud_armour`. These skip the recompiled draw functions from Rust wrappers (hud.rs); the HUD drawing
  itself is not ported. Radar position and size moved from the GRAPHICS page to the HUD page.
- Checked with screenshots: both menu pages, looks and effects in a scripted run, HUD parts hidden. Not tuned on other
  weather types or times of day.

## Modern HUD style (26 Nov 2026, GTA IV layout)
- SETTINGS > HUD > STYLE (lcs.ini `hud_style=legacy|modern`, default legacy). It replaces the earlier rectangular
  GTA V layout. Modern: the round radar bottom-left inside a ring with health on the left half (green) and armour on the
  right half (blue), both filling from the bottom; the weapon picture without its frame; the clock moved down next to the
  ammo count, directly above the money.
- Rust (recomp/engine/src/hud.rs): the ring is drawn with CSprite2d::Draw2DPolygon in place of the radar ring sprite,
  so map blips stay on top; the weapon picture is drawn cut to the inside of the frame baked into its texture; the clock
  call is moved down by 0.66 picture heights. `hud_health` / `hud_armour` = 0 hide their half of the ring. The original
  health and armour bars are hidden in this style. Legacy is unchanged.
- Checked with screenshots at 1280x720 on foot, with fists and a pistol, health and armour set to partial values by
  temporary test code. Not checked: other weapon pictures, the pause map, wanted stars, other aspect ratios.

## Tyre grip after a skid: `skid_fix` (26 Nov 2026)
- Original behaviour (read from the disassembly of 2.4.379): CVehicle::ProcessWheel (cars, mark at G+0x9ca46e) and
  CVehicle::ProcessBikeWheel (bikes, G+0x9ca475) set a static "this tyre was already skidding" mark when a wheel
  arrives in a skid state, and nothing clears it. From then on every call starts with grip x the handling record's
  traction loss (bikes seen in a session: 0.82 and 0.90; other vehicles 0.70 to 0.90), so wheels lock under braking and
  break loose in corners sooner. A scripted session had both marks set in every report.
- `skid_fix=1` (lcs.ini or `--set skid_fix 1`, default 0 = original) clears the mark before each call
  (recomp/engine/src/wheel.rs). Off while verifying. Brake force and grip both scale with the time step, so by the
  code the lock threshold does not depend on the frame rate.
- Checked: with the fix on, both marks read 0 through a scripted session on foot among traffic. Not checked: braking
  distance or feel while riding.
