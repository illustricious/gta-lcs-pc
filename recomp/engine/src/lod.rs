//! CSimpleModelInfo level-of-detail queries (libGame 0x496d04..0x496e94).
//!
//! Model layout (CSimpleModelInfo):
//!   +0x40  RslElement **elements   (one per LOD level)
//!   +0x48  float lodDistance[3]
//!   +0x50  float nearDistance       (big buildings: where the LOD hands over to the detailed model)
//!   +0x54  u8    numElements
//!   +0x56  u16   flags              (bits 0-1: related element count, bit 3, bit 4 = big building / LOD model)
//! Every distance is multiplied by TheCamera.LODDistMultiplier (TheCamera + 0xf0).
//!
//! New behaviour: big-building LOD models (flag 0x10) can be drawn farther by `far_lod_scale`. Their
//! near distance is left alone, so the swap to the detailed model, and with it streaming, is unchanged.

use crate::cpu::Cpu;
use crate::mem::{r16, r64, r8, rf32, G};
use crate::verify::{self, Ret};
use core::sync::atomic::{AtomicU32, Ordering};

const GOT_THE_CAMERA: u64 = G + 0x7f9190;
const CAM_LOD_MULT: u64 = 0xf0;
const ELEMENTS: u64 = 0x40;
const LOD_DIST: u64 = 0x48;
const NEAR_DIST: u64 = 0x50;
const NUM_ELEMENTS: u64 = 0x54;
const FLAGS: u64 = 0x56;
const FLAG_BIG_BUILDING: u16 = 0x10;

static FAR_LOD_SCALE: AtomicU32 = AtomicU32::new(0x3f80_0000); // 1.0

/// Host setting: how much farther big-building LOD models are drawn (1.0 = original).
#[no_mangle]
pub extern "C" fn lcs_set_far_lod_scale(scale: f32) {
    let s = if scale.is_finite() && scale >= 1.0 { scale } else { 1.0 };
    FAR_LOD_SCALE.store(s.to_bits(), Ordering::Relaxed);
}

#[inline]
unsafe fn lod_mult() -> f32 {
    rf32(r64(GOT_THE_CAMERA) + CAM_LOD_MULT)
}

/// lodDistance[i] * LODDistMultiplier, with the far scale for big buildings.
#[inline]
unsafe fn lod_distance(mi: u64, i: u64) -> f32 {
    let d = rf32(mi + LOD_DIST + i * 4) * lod_mult();
    if !verify::enabled() && r16(mi + FLAGS) & FLAG_BIG_BUILDING != 0 {
        d * f32::from_bits(FAR_LOD_SCALE.load(Ordering::Relaxed))
    } else {
        d
    }
}

/// Index of the last LOD level: the related-element count when bit 3 is clear, else numElements - 1.
#[inline]
unsafe fn last_index(mi: u64) -> u64 {
    let flags = r16(mi + FLAGS);
    let related = (flags & 3) as u64;
    if related != 0 && flags & 8 == 0 {
        related - 1
    } else {
        (r8(mi + NUM_ELEMENTS) as u64).wrapping_sub(1)
    }
}

#[inline]
unsafe fn element(mi: u64, i: u64) -> u64 {
    r64(r64(mi + ELEMENTS) + i * 8)
}

/// float CSimpleModelInfo::GetLodDistance(int i)
#[no_mangle]
pub unsafe extern "C" fn G_496d04(c: *mut Cpu) {
    verify::port(c, G_496d04_orig, Ret::S0, |c| {
        let i = c.w(1) as i32 as i64 as u64;
        c.set_s(0, lod_distance(c.x[0], i));
    })
}

/// RslElement *CSimpleModelInfo::GetElementFromDistance(float d)
#[no_mangle]
pub unsafe extern "C" fn G_496d20(c: *mut Cpu) {
    verify::port(c, G_496d20_orig, Ret::X0, |c| {
        let mi = c.x[0];
        let d = c.s(0);
        let mut result = 0;
        if r64(mi + ELEMENTS) != 0 {
            let flags = r16(mi + FLAGS);
            let first = if flags & 8 != 0 { (flags & 3) as u64 } else { 0 };
            let n = r8(mi + NUM_ELEMENTS) as u64;
            for i in first..n {
                if lod_distance(mi, i) > d {
                    result = element(mi, i);
                    break;
                }
            }
        }
        c.x[0] = result;
    })
}

/// float CSimpleModelInfo::GetLargestLodDistance()
#[no_mangle]
pub unsafe extern "C" fn G_496d80(c: *mut Cpu) {
    verify::port(c, G_496d80_orig, Ret::S0, |c| {
        let mi = c.x[0];
        c.set_s(0, lod_distance(mi, last_index(mi)));
    })
}

/// RslElement *CSimpleModelInfo::GetLastElement()
#[no_mangle]
pub unsafe extern "C" fn G_496dc4(c: *mut Cpu) {
    verify::port(c, G_496dc4_orig, Ret::X0, |c| {
        let mi = c.x[0];
        c.x[0] = if r64(mi + ELEMENTS) == 0 { 0 } else { element(mi, last_index(mi)) };
    })
}

/// RslElement *CSimpleModelInfo::GetLastElement(float d): the last level if it is still in range.
#[no_mangle]
pub unsafe extern "C" fn G_496e2c(c: *mut Cpu) {
    verify::port(c, G_496e2c_orig, Ret::X0, |c| {
        let mi = c.x[0];
        let d = c.s(0);
        let mut result = 0;
        if r64(mi + ELEMENTS) != 0 {
            let i = last_index(mi);
            if lod_distance(mi, i) > d {
                result = element(mi, i);
            }
        }
        c.x[0] = result;
    })
}

/// float CSimpleModelInfo::GetNearDistance(): never scaled (it decides the swap to the detailed model).
#[no_mangle]
pub unsafe extern "C" fn G_496e80(c: *mut Cpu) {
    verify::port(c, G_496e80_orig, Ret::S0, |c| {
        let mi = c.x[0];
        c.set_s(0, rf32(mi + NEAR_DIST) * lod_mult());
    })
}

extern "C" {
    fn G_496d04_orig(c: *mut Cpu);
    fn G_496d20_orig(c: *mut Cpu);
    fn G_496d80_orig(c: *mut Cpu);
    fn G_496dc4_orig(c: *mut Cpu);
    fn G_496e2c_orig(c: *mut Cpu);
    fn G_496e80_orig(c: *mut Cpu);
}
