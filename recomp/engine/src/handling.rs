//! cHandlingDataMgr::ConvertDataToGameUnits, rewritten from libGame.so 2.4.379 (behaviour unchanged).
//!
//! LoadHandlingData reads handling.cfg into tHandlingData records (human units) and calls this once per vehicle to
//! turn them into per-tick engine units. cTransmission is in transmission.rs.
//!
//! tHandlingData fields used: +0x00 f32 mass, +0x0c f32 turn mass factor, +0x10 u32 handling id, +0x14 f32 (set to
//! 1/mass), +0x18 f32 drag, +0x2c u8 percent submerged, +0x30 f32 buoyancy (out), +0x34 the transmission
//! (cTransmission, ratios built by InitGearRatios), +0x7c u8 drive type ('4' = four wheel drive), +0x80 f32
//! acceleration, +0x88 f32 top speed (km/h), +0x8c f32 cruise speed (out), +0x90 f32 reverse speed (out),
//! +0x98 f32 brake deceleration.

use crate::cpu::Cpu;
use crate::guest::{call, fma};
use crate::mem::*;
use crate::verify::{self, Ret};

const INIT_GEAR_RATIOS: u64 = 0x7d74e0; // PLT: cTransmission::InitGearRatios

extern "C" {
    fn G_4ec4d4_orig(c: *mut Cpu);
}

unsafe fn convert(c: &mut Cpu, h: u64) {
    let mass = rf32(h);
    let submerged = r8(h + 0x2c) as f32;
    let top_kmh = rf32(h + 0x88);
    w32f(h + 0x14, 1.0 / mass);
    let buoyancy = mass * f32::from_bits(0x3c03_126f) * 100.0 / submerged;
    let turn = rf32(h + 0x0c) * 2000.0 / mass;
    let per_tick2 = f32::from_bits(0x39d1_b717); // 1 / 50^2
    let brake = rf32(h + 0x98) * per_tick2;
    let top = top_kmh * f32::from_bits(0x3bb6_0b6a); // km/h to units per tick
    let accel = rf32(h + 0x80) * per_tick2;
    w32f(h + 0x30, buoyancy);
    w32f(h + 0x98, brake);
    w32f(h + 0x88, top);
    w32f(h + 0x0c, turn);

    // Lower the speed until the engine can overcome the drag at it.
    let mut speed = top;
    if top > 0.0 {
        let drag = rf32(h + 0x18);
        let quadratic = drag * 0.5 / 1000.0;
        let limit = (accel as f64 * rf64(G + 0x232cc8)) as f32;
        let step = rf64(G + 0x2331c8);
        loop {
            let low_drag = drag < f32::from_bits(0x3c23_d70a);
            speed = (speed as f64 + step) as f32;
            let again = if low_drag {
                let v = 1.0 / fma(drag * speed, speed, 1.0) + -1.0;
                v * -speed > limit
            } else {
                !(speed * speed * quadratic <= limit)
            };
            // b.gt / b.le differ for unordered values: the low-drag branch continues only on an ordered greater
            if !(again && speed > 0.0) {
                break;
            }
        }
    }

    let id = r32(h + 0x10);
    if id == 0x27 {
        w32f(h + 0x8c, top);
        w32f(h + 0x90, -top);
    } else {
        w32f(h + 0x88, f32::from_bits(0x3f99_999a) * speed);
        w32f(h + 0x8c, speed);
        let reverse = if id.wrapping_sub(0x41) <= 6 { 0xbd4c_cccd } else { 0xbe4c_cccd };
        w32(h + 0x90, reverse);
    }
    let share = if r8(h + 0x7c) == b'4' { 0.25 } else { 0.5 };
    w32f(h + 0x80, accel * share);
    call(c, INIT_GEAR_RATIOS, &[h + 0x34]);
}

/// void cHandlingDataMgr::ConvertDataToGameUnits(tHandlingData *)
#[no_mangle]
pub unsafe extern "C" fn G_4ec4d4(c: *mut Cpu) {
    let h = (*c).x[1];
    verify::port_regions(c, G_4ec4d4_orig, Ret::None, &[(h, 0xa0)], |c| convert(c, h));
}
