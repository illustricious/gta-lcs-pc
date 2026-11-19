# lcs_engine (Rust)

This crate holds Rust reimplementations of engine functions. It is a `no_std` static library, linked into `lcs.exe` next to the recompiled C.

## How a port replaces a function

1. Its address goes into `HOST_HOOKS` in `recomp/lift.py`. The recompiled body is then renamed `G_<addr>_orig`.
2. The crate exports `G_<addr>` with the same guest ABI: the arguments and the result stay in the `Cpu` registers.

Every caller, including indirect calls and PLT stubs, now reaches the Rust version.

## Verification

Set `LCS_RS_VERIFY=1` to check the ports.

- Each ported call also runs the original on a copy of the CPU, and the two results are compared bit for bit.
- New behaviour is switched off while verifying.
- The counts go to the log once a second: `engine: verified N calls, M mismatches`.

## Ported

| Function | Address | Verified |
| --- | --- | --- |
| `CSimpleModelInfo::GetLodDistance(int)` | 0x496d04 | ✓ |
| `CSimpleModelInfo::GetElementFromDistance(float)` | 0x496d20 | ✓ |
| `CSimpleModelInfo::GetLargestLodDistance()` | 0x496d80 | ✓ |
| `CSimpleModelInfo::GetLastElement()` | 0x496dc4 | ✓ |
| `CSimpleModelInfo::GetLastElement(float)` | 0x496e2c | ✓ |
| `CSimpleModelInfo::GetNearDistance()` | 0x496e80 | ✓ |
| `CHud::GetRadarPosX()` | 0x48411c | ✓ |
| `CHud::GetRadarPosY()` | 0x48416c | ✓ |
| `CHud::GetRadarSizeX()` | 0x4841c8 | ✓ |
| `CHud::GetRadarSizeY()` | 0x484224 | ✓ |
| `CRadar::AdjustForWideScreen(float)` | 0x35a400 | ✓ |
| `CRadar::TransformRealWorldPointToRadarSpace` | 0x358b34 | ✓ |
| `CRadar::LimitRadarPoint(CVector2D&)` | 0x358b98 | ✓ |
| `CRadar::CalculateBlipAlpha(float)` | 0x358bd8 | ✓ |
| `CTimeCycle::ForceUpdate()` | 0x36d570 | ✓ |
| `CTimeCycle::Update()` | 0x3705f0 | calls ForceUpdate (not shadowed: ForceUpdate has state) |
| `CTimeCycle::StartExtraColour(int, bool)` | 0x37060c | ✓ |
| `CTimeCycle::StopExtraColour(bool)` | 0x370630 | ✓ |
| `CTimeCycle::UpdateArrays()` | 0x36d194 | ✓ |
| `FillGaps<unsigned char>` | 0x36f090 | ✓ |
| `FillGaps<signed char>` | 0x36f66c | ✓ |
| `FillGaps<short>` | 0x36fd14 | ✓ |
| `CTimeCycle::Interpolate (u8, i8, i16, int, float)` | 0x3703bc, 0x370428, 0x370494, 0x370518, 0x37058c | never called (inlined in the game) |
| `CPad::GetAccelerate` | 0x345e84 | ✓ |
| `CPad::GetBrake` | 0x345c20 | ✓ |
| `CPad::GetHandBrake` | 0x345b70 | ✓ |
| `CPad::GetExitVehicle` | 0x345d94 | ✓ |
| `CPad::ExitVehicleJustDown` | 0x345de4 | ✓ |
| `CPad::JumpJustDown` | 0x345c88 | ✓ |
| `CPad::JumpDown` | 0x345cbc | ✓ |
| `CPad::GetSprint` | 0x3464e4 | ✓ |
| `CPad::GetPedWalkLeftRight` | 0x345684 | ✓ |
| `CPad::GetPedWalkUpDown` | 0x345780 | ✓ |
| `CPad::GetWeapon` | 0x345e1c | ✓ |
| `CPad::WeaponJustDown` | 0x345e48 | ✓ |
| `CPad::CycleWeaponLeftJustDown` | 0x346038 | ✓ |
| `CPad::CycleWeaponRightJustDown` | 0x346090 | ✓ |
| `CPad::CollectPickupJustDown` | 0x346758 | ✓ |
| `CPad::GetLookBehindForPed` | 0x345a20 | ✓ |
| `CCamera::GetGameCamPosition` | 0x3135fc | ✓ |
| `CCamera::Get_Just_Switched_Status` | 0x329e10 | ✓ |
| `CCamera::GetPositionAlongSpline` | 0x329ee8 | ✓ |
| `CCamera::GetFading` | 0x329dd4 | ✓ |
| `CCamera::GetFadingStatusForScript` | 0x329de0 | ✓ |
| `CCamera::GetScreenFadeStatus` | 0x328164 | ✓ |
| `CCamera::GetFadingDirection` | 0x329dec | ✓ |
| `CCamera::FindCamFOV` | 0x3299c4 | ✓ |
| `CCamera::ShouldPedControlsBeRelative` | 0x32e3f4 | ✓ |
| `CCamera::GetLookingLRBFirstPerson` | 0x329e18 | ✓ |
| `CCamera::GetLookingForwardFirstPerson` | 0x329eac | ✓ |
| `CCamera::Using1stPersonWeaponMode` | 0x32e1f8 | ✓ |
| `CCamera::GetArrPosForVehicleType` | 0x31fb84 | ✓ |
| `CCam::GetWeaponFirstPersonOn` | 0x313a78 | ✓ |
| `CCamera::Enable1rstPersonCamCntrlsScript` | 0x32807c | ✓ |
| `CCamera::Enable1rstPersonWeaponsCamera` | 0x328088 | ✓ |
| `CCamera::DontProcessObbeCinemaCamera` | 0x327324 | ✓ |
| `CCamera::SetNearClipScript` | 0x32af08 | ✓ |
| `CCamera::SetWideScreenOn` | 0x32dffc | ✓ |
| `CCamera::SetWideScreenOff` | 0x326a20 | ✓ |
| `CCamera::ProcessWideScreenOn` | 0x32bf70 | ✓ |
| `CCamera::SetCamCutSceneOffSet` | 0x32e080 | ✓ |
| `CCamera::UpdateAimingCoors` | 0x32e438 | ✓ |
| `CCamera::TakeControlWithSpline` | 0x32e410 | ✓ |
| `CCamera::SetParametersForScriptInterpolation` | 0x32e220 | ✓ |
| `CCamera::SetZoomValueFollowPedScript` | 0x32e310 | ✓ |

Functions that write through a pointer are verified with `verify::port_mem`; functions that write several object fields and globals (CTimeCycle) use `verify::port_regions`, whose scratch is a stack because verified ports call other verified ports. The memory is restored between the original and the port, and both the memory and the return register are compared.

Ports call other guest functions through `guest::call`, which saves and restores the CPU. ARM64 `fmadd`/`fmsub` map to `guest::fma`, a single-rounding FMA identical to the `fmaf` used by the recompiled C. `fsqrt` maps to `guest::sqrt`.

The second verification run, after the HUD ports, covered over 1,000,000 calls with 0 mismatches. The run after CTimeCycle (title, load, safehouse interior with its extra colour, street) covered 2,071,130 calls with 0 mismatches. With the CPad and CCamera ports added: 3,275,492 calls, 0 mismatches.

## Per-action bindings (`pad.rs`)

The CPad getters keep the original rules (disabled controls, control scheme, touch widgets) and only swap the final
button read for a host binding when one exists for that action in the player's context (`FindPlayerVehicle`):
`foot_<action>` or `car_<action>` in lcs.ini (see rt/main.c `host_action`). Bindings are ignored while verifying.

## Radar placement (`hud.rs`)

`lcs_radar_layout()` runs once per frame. It moves the HUD layout's radar widget to the bottom-left corner (`radar`, `radar_margin`) and scales it (`radar_scale`, Graphics → RADAR SIZE). Because everything reads that widget, the ring sprite, the map, the blips and the getters all follow.

When the engine rebuilds its layout, the original values are recorded again. This replaces the old C code that rewrote only the widget's y.

The verification run covered more than 1,000,000 calls in gameplay with 0 mismatches (18 Nov 2026).

## `far_lod_scale` (set from `draw_distance`)

`far_lod_scale` lengthens the draw distance of big-building LOD models (flag `0x10`) without moving the swap to the detailed model.

In LCS mobile, however, the city is not drawn through these models. On a Portland street the `CRenderer` list held 73 entities and none of them were big buildings. Buildings and roads are sector geometry drawn by `cWorldStream::Render`, which uses `cWorldGeom` instances with near/far distances (`cWorldGeom::IsVisible`, `SetLodDistance`).

The next ports for draw distance are therefore:
1. `cWorldGeom::IsVisible`, `cWorldGeom::SetLodDistance`
2. `cWorldStream::Render`, `cWorldStream::Update`, `SwapToStreamedSector`

## Build

`recomp/build.py` runs `cargo build --release` for the toolchain in use:
- `x86_64-pc-windows-msvc` with Visual Studio's clang;
- `x86_64-pc-windows-gnu` with llvm-mingw.

Without cargo, as in the installer, the build uses `prebuilt/x86_64-pc-windows-gnu/liblcs_engine.a`. Rebuild that file after changing the crate:

```
cargo build --release --target x86_64-pc-windows-gnu --manifest-path recomp/engine/Cargo.toml --target-dir build/recomp/engine
```

Build it with `--remap-path-prefix` for the repository folder and your home folder (set through `CARGO_ENCODED_RUSTFLAGS`), so no local paths end up in the library. Then copy `liblcs_engine.a` into `prebuilt/`.
