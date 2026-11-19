//! CPad action getters, rewritten from libGame.so 2.4.379, with per-action key bindings.
//!
//! The game reads every player action through a CPad getter (GetAccelerate, GetSprint, JumpJustDown...).
//! Each port keeps the original rules (disabled controls, control mode, touch widgets) and only replaces
//! the final button read: when the host has a binding for that action in the current context (on foot
//! or in a vehicle, see rt/main.c `act_*`), the binding is used instead of the pad button. Without
//! bindings, and always while verifying, the ports behave exactly like the originals.
//!
//! CPad offsets used here: +0x00 i16 mode; new state: +0x02/+0x04 left stick x/y, +0x0a L1, +0x0e R1,
//! +0x10 R2, +0x12/+0x14/+0x16/+0x18 d-pad up/down/left/right, +0x1e/+0x20 touch weapon buttons,
//! +0x26 Square, +0x28 Triangle, +0x2a Cross, +0x2c Circle; the old state is the same 0x34 bytes later.
//! +0x9e control scheme, +0xa2 controls disabled, +0xa4/+0xaf/+0xdd/+0xde action locks.

use crate::cpu::Cpu;
use crate::guest::call;
use crate::mem::*;
use crate::verify::{self, Ret};

/// Actions the host can bind (must match `enum Act` in rt/main.c).
#[derive(Clone, Copy)]
#[repr(i32)]
enum Act {
    Accelerate = 0,
    Brake = 1,
    Handbrake = 2,
    EnterExit = 3,
    Sprint = 4,
    Jump = 5,
    Attack = 6,
    NextWeapon = 7,
    PrevWeapon = 8,
    Pickup = 9,
    LookBehind = 10,
    MoveX = 11,
    MoveY = 12,
}

extern "C" {
    /// -1 when the action has no binding in this context, else bit 0 = held now, bit 1 = held last frame.
    fn host_action(action: i32, vehicle: i32) -> i32;
    /// i32::MIN when unbound, else -128..128.
    fn host_action_axis(action: i32, vehicle: i32) -> i32;
    /// Non-zero when any per-action binding exists.
    fn host_actions_bound() -> i32;
}

const FIND_PLAYER_VEHICLE: u64 = 0x354114;
const GOT_TOUCH_DPAD: u64 = 0x7f9bd8; // u8: the d-pad drives walking and weapon cycling
const GOT_TOUCH_WIDGETS: u64 = 0x7f9d90; // u8: touch widgets active
const GOT_HUD_LAYOUT: u64 = 0x7f9210;
const GOT_PED_LOOK_BEHIND_OFF: u64 = 0x7f9220;
const WIDGET_VALUE: u64 = 0x7cea00; // float(layout, widget id)

/// Binding state of an action for the player's current context, if bound.
unsafe fn bound(c: &mut Cpu, a: Act) -> Option<(bool, bool)> {
    if verify::enabled() || host_actions_bound() == 0 {
        return None;
    }
    let vehicle = (call(c, FIND_PLAYER_VEHICLE, &[]).0 != 0) as i32;
    let s = host_action(a as i32, vehicle);
    if s < 0 { None } else { Some((s & 1 != 0, s & 2 != 0)) }
}

unsafe fn bound_axis(c: &mut Cpu, a: Act) -> Option<i32> {
    if verify::enabled() || host_actions_bound() == 0 {
        return None;
    }
    let vehicle = (call(c, FIND_PLAYER_VEHICLE, &[]).0 != 0) as i32;
    let v = host_action_axis(a as i32, vehicle);
    if v == i32::MIN { None } else { Some(v) }
}

/// Button value of the new state (`off` from the pad) or of the binding (255 / 0).
unsafe fn held(c: &mut Cpu, pad: u64, off: u64, a: Act) -> u16 {
    match bound(c, a) {
        Some((now, _)) => if now { 255 } else { 0 },
        None => r16(pad + off),
    }
}

/// New state set and old state clear: pad offsets of the new and old button, or the binding.
unsafe fn just_down(c: &mut Cpu, pad: u64, new: u64, old: u64, a: Act) -> bool {
    match bound(c, a) {
        Some((now, prev)) => now && !prev,
        None => r16(pad + new) != 0 && r16(pad + old) == 0,
    }
}

#[inline]
unsafe fn disabled(pad: u64) -> bool {
    r16(pad + 0xa2) != 0
}
#[inline]
unsafe fn scheme(pad: u64) -> u16 {
    r16(pad + 0x9e)
}
#[inline]
unsafe fn touch_dpad() -> bool {
    r8(got(GOT_TOUCH_DPAD)) != 0
}

/// True when the touch brake widget is pushed past 0.8.
unsafe fn brake_widget(c: &mut Cpu) -> bool {
    r8(got(GOT_TOUCH_WIDGETS)) != 0 && call(c, WIDGET_VALUE, &[r64(got(GOT_HUD_LAYOUT)), 0xe]).1 > 0.8
}

/// int16 CPad::GetAccelerate()
#[no_mangle]
pub unsafe extern "C" fn G_345e84(c: *mut Cpu) {
    verify::port(c, G_345e84_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = if disabled(pad) || r8(pad + 0xdd) != 0 { 0 } else { held(c, pad, 0x2a, Act::Accelerate) };
        c.set_w(0, v as u32);
    });
}

/// int16 CPad::GetBrake()
#[no_mangle]
pub unsafe extern "C" fn G_345c20(c: *mut Cpu) {
    verify::port(c, G_345c20_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = if disabled(pad) || brake_widget(c) { 0 } else { held(c, pad, 0x26, Act::Brake) };
        c.set_w(0, v as u32);
    });
}

/// int16 CPad::GetHandBrake()
#[no_mangle]
pub unsafe extern "C" fn G_345b70(c: *mut Cpu) {
    verify::port(c, G_345b70_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let m = scheme(pad);
        let v: u32 = if disabled(pad) {
            0
        } else if m == 2 || m == 3 {
            // alternative schemes: accelerate + brake together
            if r8(pad + 0xdd) != 0 || r16(pad + 0x2a) == 0 { 0 } else { (held(c, pad, 0x26, Act::Handbrake) != 0) as u32 }
        } else if m > 1 {
            0
        } else if r8(got(GOT_TOUCH_WIDGETS)) != 0 && r16(pad + 0x26) != 0 && brake_widget(c) {
            1
        } else {
            held(c, pad, 0xe, Act::Handbrake) as u32
        };
        c.set_w(0, v);
    });
}

/// bool CPad::GetExitVehicle()
#[no_mangle]
pub unsafe extern "C" fn G_345d94(c: *mut Cpu) {
    verify::port(c, G_345d94_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = !(disabled(pad) || r8(pad + 0xa4) != 0 || r8(pad + 0xaf) != 0) && held(c, pad, 0x28, Act::EnterExit) != 0;
        c.set_w(0, v as u32);
    });
}

/// bool CPad::ExitVehicleJustDown()
#[no_mangle]
pub unsafe extern "C" fn G_345de4(c: *mut Cpu) {
    verify::port(c, G_345de4_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = !(disabled(pad) || r8(pad + 0xaf) != 0 || r8(pad + 0xa4) != 0) && just_down(c, pad, 0x28, 0x5c, Act::EnterExit);
        c.set_w(0, v as u32);
    });
}

/// bool CPad::JumpJustDown()
#[no_mangle]
pub unsafe extern "C" fn G_345c88(c: *mut Cpu) {
    verify::port(c, G_345c88_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = !(disabled(pad) || scheme(pad) > 3) && just_down(c, pad, 0x26, 0x5a, Act::Jump);
        c.set_w(0, v as u32);
    });
}

/// bool CPad::JumpDown()
#[no_mangle]
pub unsafe extern "C" fn G_345cbc(c: *mut Cpu) {
    verify::port(c, G_345cbc_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = !(disabled(pad) || scheme(pad) > 3) && held(c, pad, 0x26, Act::Jump) != 0;
        c.set_w(0, v as u32);
    });
}

/// bool CPad::GetSprint(): Cross held while the walk stick is pushed (|x| + |y| > 64).
#[no_mangle]
pub unsafe extern "C" fn G_3464e4(c: *mut Cpu) {
    verify::port(c, G_3464e4_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = if disabled(pad) || scheme(pad) > 3 || r8(pad + 0xdd) != 0 || held(c, pad, 0x2a, Act::Sprint) == 0 {
            false
        } else {
            let (ax, ay): (i32, i32) = if ri16(pad) > 6 {
                (0, 0)
            } else if let Some((x, y)) = move_axes(c) {
                (x.abs() as i16 as i32 & 0xffff, y)
            } else if touch_dpad() {
                let x = (half(ri16(pad + 0x18) as i32 - ri16(pad + 0x16) as i32) as i16) as i32;
                let y = half(ri16(pad + 0x14) as i32 - ri16(pad + 0x12) as i32);
                (x.wrapping_abs() & 0xffff, y)
            } else {
                ((ri16(pad + 2) as i32).wrapping_abs() as i16 as i32, r16(pad + 4) as i32)
            };
            let ay = (ay as i16 as i32).wrapping_abs();
            ax.wrapping_add(ay as i16 as i32) > 64
        };
        c.set_w(0, v as u32);
    });
}

/// (a - b) / 2 rounded toward zero, as the d-pad axes are built.
#[inline]
fn half(v: i32) -> i32 {
    v / 2
}

/// Bound walk keys as stick values, if any are bound.
unsafe fn move_axes(c: &mut Cpu) -> Option<(i32, i32)> {
    let x = bound_axis(c, Act::MoveX);
    let y = bound_axis(c, Act::MoveY);
    if x.is_none() && y.is_none() {
        return None;
    }
    Some((x.unwrap_or(0), y.unwrap_or(0)))
}

/// int16 CPad::GetPedWalkLeftRight()
#[no_mangle]
pub unsafe extern "C" fn G_345684(c: *mut Cpu) {
    verify::port(c, G_345684_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v: u32 = if disabled(pad) || ri16(pad) > 6 || scheme(pad) > 3 {
            0
        } else if let Some(x) = bound_axis(c, Act::MoveX) {
            x as u32
        } else if touch_dpad() {
            half(ri16(pad + 0x18) as i32 - ri16(pad + 0x16) as i32) as u32
        } else {
            r16(pad + 2) as u32
        };
        c.set_w(0, v);
    });
}

/// int16 CPad::GetPedWalkUpDown()
#[no_mangle]
pub unsafe extern "C" fn G_345780(c: *mut Cpu) {
    verify::port(c, G_345780_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v: u32 = if disabled(pad) || ri16(pad) > 6 || scheme(pad) > 3 {
            0
        } else if let Some(y) = bound_axis(c, Act::MoveY) {
            y as u32
        } else if touch_dpad() {
            half(ri16(pad + 0x14) as i32 - ri16(pad + 0x12) as i32) as u32
        } else {
            r16(pad + 4) as u32
        };
        c.set_w(0, v);
    });
}

/// int16 CPad::GetWeapon()
#[no_mangle]
pub unsafe extern "C" fn G_345e1c(c: *mut Cpu) {
    verify::port(c, G_345e1c_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = if disabled(pad) || scheme(pad) > 3 || r8(pad + 0xde) != 0 {
            0
        } else {
            held(c, pad, 0x2c, Act::Attack) as i16 as i32 as u32
        };
        c.set_w(0, v);
    });
}

/// bool CPad::WeaponJustDown()
#[no_mangle]
pub unsafe extern "C" fn G_345e48(c: *mut Cpu) {
    verify::port(c, G_345e48_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = !(disabled(pad) || scheme(pad) > 3 || r8(pad + 0xde) != 0) && just_down(c, pad, 0x2c, 0x60, Act::Attack);
        c.set_w(0, v as u32);
    });
}

/// bool CPad::CycleWeaponLeftJustDown()
#[no_mangle]
pub unsafe extern "C" fn G_346038(c: *mut Cpu) {
    verify::port(c, G_346038_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = !(disabled(pad) || scheme(pad) > 3) && {
            let (new, old) = if touch_dpad() { (0x1e, 0x52) } else { (0x16, 0x4a) };
            just_down(c, pad, new, old, Act::PrevWeapon)
        };
        c.set_w(0, v as u32);
    });
}

/// bool CPad::CycleWeaponRightJustDown()
#[no_mangle]
pub unsafe extern "C" fn G_346090(c: *mut Cpu) {
    verify::port(c, G_346090_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = !(disabled(pad) || scheme(pad) > 3) && {
            let (new, old) = if touch_dpad() { (0x20, 0x54) } else { (0x18, 0x4c) };
            just_down(c, pad, new, old, Act::NextWeapon)
        };
        c.set_w(0, v as u32);
    });
}

/// bool CPad::CollectPickupJustDown()
#[no_mangle]
pub unsafe extern "C" fn G_346758(c: *mut Cpu) {
    verify::port(c, G_346758_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = !disabled(pad) && just_down(c, pad, 0xa, 0x3e, Act::Pickup);
        c.set_w(0, v as u32);
    });
}

/// bool CPad::GetLookBehindForPed()
#[no_mangle]
pub unsafe extern "C" fn G_345a20(c: *mut Cpu) {
    verify::port(c, G_345a20_orig, Ret::W0, |c| {
        let pad = c.x[0];
        let v = r32(got(GOT_PED_LOOK_BEHIND_OFF)) == 0 && !disabled(pad) && held(c, pad, 0x10, Act::LookBehind) != 0;
        c.set_w(0, v as u32);
    });
}

extern "C" {
    fn G_345e84_orig(c: *mut Cpu);
    fn G_345c20_orig(c: *mut Cpu);
    fn G_345b70_orig(c: *mut Cpu);
    fn G_345d94_orig(c: *mut Cpu);
    fn G_345de4_orig(c: *mut Cpu);
    fn G_345c88_orig(c: *mut Cpu);
    fn G_345cbc_orig(c: *mut Cpu);
    fn G_3464e4_orig(c: *mut Cpu);
    fn G_345684_orig(c: *mut Cpu);
    fn G_345780_orig(c: *mut Cpu);
    fn G_345e1c_orig(c: *mut Cpu);
    fn G_345e48_orig(c: *mut Cpu);
    fn G_346038_orig(c: *mut Cpu);
    fn G_346090_orig(c: *mut Cpu);
    fn G_346758_orig(c: *mut Cpu);
    fn G_345a20_orig(c: *mut Cpu);
}
