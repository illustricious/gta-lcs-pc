//! cTransmission (gears and drive force), rewritten from libGame.so 2.4.379. Behaviour is unchanged.
//!
//! `this`: six gears of 12 bytes from +0x00 (+0 maximum speed, +4 up-shift speed, +8 down-shift speed), +0x4a u8
//! number of gears, +0x4b u8 drive flags, +0x4c f32 engine acceleration, +0x50 f32 engine inertia, +0x54 f32
//! maximum speed, +0x5c f32 maximum reverse speed, +0x60 f32 the speed last seen.

use crate::cpu::Cpu;
use crate::guest::{fma, fmax, fmin};
use crate::mem::*;
use crate::verify::{self, Ret};

const GEAR: u64 = 12;

// globals
const LAST_GEAR_PTRS: u64 = G + 0x9ca448; // two pointers InitGearRatios leaves behind
const DRIVE_RESULT: u64 = G + 0x9ca458; // float returned by CalculateDriveAcceleration
const DRIVE_SPEED: u64 = G + 0x9ca45c; // speed it works on
const GOT_TIMESTEP: u64 = 0x7f91e8;
const GOT_CHEAT2_FACTOR: u64 = 0x7fcd00;
const GOT_CHEAT1_FACTOR: u64 = 0x7fcd08;
const GOT_CHEAT_INERTIA: u64 = 0x7fcd10;
const GOT_MODE1_INERTIA: u64 = 0x7fcd18;
const GOT_MODE2_INERTIA: u64 = 0x7fcd20;
const GOT_INERTIA_BLEND: u64 = 0x7fcd28;

extern "C" {
    fn G_4f65fc_orig(c: *mut Cpu);
    fn G_4f6aa4_orig(c: *mut Cpu);
    fn G_4f66fc_orig(c: *mut Cpu);
}

#[inline]
fn gear(this: u64, i: u32) -> u64 {
    this + i as u64 * GEAR
}

unsafe fn init_gear_ratios(this: u64) {
    for off in (0..0x48u64).step_by(8) {
        w64(this + off, 0);
    }
    let n = r8(this + 0x4a);
    if n != 0 {
        let step = 1.0 / n as f32;
        let top = rf32(this + 0x54);
        let base = top * (step * 0.5);
        let range = top - base;
        let mut i: u32 = 1;
        let (mut last, mut before);
        loop {
            let k = i & 0xff;
            last = gear(this, i);
            before = last - GEAR;
            let speed = fma(step * k as f32, range, base);
            w32f(last, speed);
            let count = r8(this + 0x4a) as u32;
            let previous = rf32(before);
            let up = if count > k {
                let diff = speed - previous;
                w32f(gear(this, i + 1) + 8, fma(diff, f32::from_bits(0x3ed7_0a3d), previous));
                fma(diff, f32::from_bits(0x3f2a_acda), rf32(before))
            } else {
                rf32(this + 0x54)
            };
            w32f(gear(this, i) + 4, up);
            i = (i + 1) & 0xff;
            if i > r8(this + 0x4a) as u32 {
                break;
            }
        }
        w64(LAST_GEAR_PTRS, last);
        w64(LAST_GEAR_PTRS + 8, before);
    }
    let reverse = rf32(this + 0x5c);
    w32f(this, reverse);
    w32(this + 4, 0xbc23_d70a);
    w32f(this + 8, reverse);
    w32(this + 0x14, 0xbc23_d70a);
}

/// void cTransmission::InitGearRatios()
#[no_mangle]
pub unsafe extern "C" fn G_4f65fc(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_4f65fc_orig, Ret::None, &[(this, 0x64), (LAST_GEAR_PTRS, 16)], |_| init_gear_ratios(this));
}

/// void cTransmission::CalculateGearForSimpleCar(float speed, uint8 &gear)
#[no_mangle]
pub unsafe extern "C" fn G_4f6aa4(c: *mut Cpu) {
    let (this, out) = ((*c).x[0], (*c).x[1]);
    verify::port_regions(c, G_4f6aa4_orig, Ret::None, &[(this, 0x64), (out, 1)], |c| {
        let speed = c.s(0);
        w32f(this + 0x60, speed);
        let g = r8(out) as u32;
        if rf32(gear(this, g) + 4) < speed {
            let n = r8(this + 0x4a) as u32;
            w8(out, if g + 1 > n { n as u8 } else { (g + 1) as u8 });
        } else if rf32(gear(this, g) + 8) > speed {
            w8(out, if g < 1 { 0 } else { (g - 1) as u8 });
        }
    });
}

struct Args {
    this: u64,
    pedal: u64,
    gear: u64,
    velocity: u64,
    inertia_a: u64,
    inertia_b: u64,
    cheat: bool,
    mode: u8,
}

unsafe fn drive_acceleration(p: &Args) -> f32 {
    let this = p.this;
    let mut speed = rf32(p.velocity);
    w32f(DRIVE_SPEED, speed);
    if speed < rf32(this + 0x5c) {
        return 0.0;
    }
    let (mut mode, mut cheat, mut a, mut b) = (p.mode, p.cheat, p.inertia_a, p.inertia_b);

    // Shift until the speed fits the gear, then pick the gear's torque curve.
    let (g, curve_gear, curve, top, gear_up) = loop {
        let top = rf32(this + 0x54);
        if speed > top {
            return 0.0;
        }
        w32f(this + 0x60, speed);
        let g = r8(p.gear) as u32;
        let up = rf32(gear(this, g) + 4);
        let shift: u32 = if speed > up {
            if g != 0 || rf32(p.pedal) > 0.0 {
                1
            } else {
                let curve = if r8(this + 0x4a) == 1 { 1.0 } else { 4.5 };
                break (0, 0, curve, top, up);
            }
        } else {
            let down = rf32(gear(this, g) + 8);
            if speed < down && g != 0 {
                if g != 1 || rf32(p.pedal) < 0.0 {
                    0xff
                } else if r8(this + 0x4a) == 1 {
                    break (1, 1, 1.0, top, up);
                } else {
                    break (g, g, torque_curve(this, g), top, up);
                }
            } else if r8(this + 0x4a) == 1 {
                break (g, g, 1.0, top, up);
            } else if g == 0 {
                break (g, 0, 4.5, top, up);
            } else {
                break (g, g, torque_curve(this, g), top, up);
            }
        };
        w8(p.gear, g.wrapping_add(shift) as u8);
        mode = 0;
        cheat = false;
        b = 0;
        a = 0;
        speed = rf32(DRIVE_SPEED);
        if speed < rf32(this + 0x5c) {
            return 0.0;
        }
    };

    let mode2 = if mode == 2 { rf32(got(GOT_CHEAT2_FACTOR)) } else { 1.0 };
    let engine = (if mode == 1 { 1.0 } else { mode2 }) * rf32(this + 0x4c);
    let scale = if mode == 1 { rf32(got(GOT_CHEAT1_FACTOR)) } else { 1.0 };
    let force = scale * (curve * engine) / 2.5;
    let pedal = rf32(p.pedal);
    let timestep = rf32(got(GOT_TIMESTEP));
    let result = timestep * (pedal * force);
    w32f(DRIVE_RESULT, result);

    if a != 0 && b != 0 {
        if !cheat {
            let rise = timestep * (pedal.abs() * rf32(got(GOT_CHEAT_INERTIA))) / rf32(this + 0x50);
            w32f(a, fmin(rf32(a) + rise, 1.0));
            w32(b, 0x3dcc_cccd);
        } else {
            let third = f32::from_bits(0x3eaa_a64c);
            let n = r8(this + 0x4a) as f32;
            let (num, den) = if g == 1 {
                let off = top / n * third;
                (speed + off, gear_up + off)
            } else if g != 0 {
                let down = rf32(gear(this, curve_gear) + 8);
                (speed - down, gear_up - down)
            } else {
                let off = top / n * third;
                (off - speed, off - rf32(gear(this, curve_gear) + 8))
            };
            let revs = num / den;
            let mut change = revs - rf32(a);
            if mode == 1 {
                change *= rf32(got(GOT_MODE1_INERTIA));
            } else if mode == 2 {
                change *= rf32(got(GOT_MODE2_INERTIA));
            }
            let blend = rf32(got(GOT_INERTIA_BLEND));
            let target = fmax(fmin(fma(-change, rf32(this + 0x50), 1.0), 1.0), f32::from_bits(0x3dcc_cccd));
            let smoothed = fma(blend, rf32(b), (1.0 - blend) * target);
            w32f(DRIVE_RESULT, result * smoothed);
            w32f(b, smoothed);
            w32f(a, revs);
        }
    }

    // Fade the force out near the maximum speed of the gear.
    let limit = rf32(gear(this, r8(p.gear) as u32));
    let current = rf32(DRIVE_SPEED);
    let over = if limit < 0.0 && current < scale * limit {
        Some(fma(scale, limit, -current))
    } else if limit > 0.0 && current > scale * limit {
        Some(fma(-scale, limit, current))
    } else {
        None
    };
    if let Some(over) = over {
        let fade = fmin(over / f32::from_bits(0x3d4c_cccd), 1.0);
        w32f(DRIVE_RESULT, (1.0 - fade) * rf32(DRIVE_RESULT));
    }
    rf32(DRIVE_RESULT)
}

/// Torque curve of gear `g` (1 and up) by the drive flags: higher in the low gears.
unsafe fn torque_curve(this: u64, g: u32) -> f32 {
    let n = r8(this + 0x4a) as f32;
    let t = 1.0 - (g as f32 + -1.0) / (n + -1.0);
    let flags = r8(this + 0x4b);
    if flags & 1 != 0 {
        fma(t * 5.0, t, 1.0)
    } else {
        let k = if flags & 2 != 0 { 4.0 } else { 3.0 };
        fma(t * k, t, 1.0)
    }
}

/// float cTransmission::CalculateDriveAcceleration(const float &pedal, uint8 &gear, float &, const float &velocity,
/// float *a, float *b, uint8 cheat, uint8 mode (on the stack))
#[no_mangle]
pub unsafe extern "C" fn G_4f66fc(c: *mut Cpu) {
    let x = (*c).x;
    let p = Args {
        this: x[0],
        pedal: x[1],
        gear: x[2],
        velocity: x[4],
        inertia_a: x[5],
        inertia_b: x[6],
        cheat: x[7] & 0xff != 0,
        mode: r8(x[31]),
    };
    let regions = [(p.this, 0x64), (DRIVE_RESULT, 8), (p.gear, 1), (p.inertia_a, 4), (p.inertia_b, 4)];
    verify::port_regions(c, G_4f66fc_orig, Ret::S0, &regions, |c| {
        let v = drive_acceleration(&p);
        c.set_s(0, v);
    });
}
