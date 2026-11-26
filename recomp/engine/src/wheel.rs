//! Optional fix for tyres that keep reduced grip after the first skid (libGame.so 2.4.379).
//!
//! CVehicle::ProcessWheel (cars) and CVehicle::ProcessBikeWheel mean to lower a tyre's grip by the handling
//! record's traction loss while that tyre is skidding. The "was already skidding" mark is a static in .bss: it is
//! set when a wheel arrives with a skid state and nothing ever clears it. From the first skid of the session on,
//! every later call of the same function, for any vehicle and wheel, starts with grip x traction loss, so tyres
//! break loose and lock under braking sooner. With the fix on, the mark is cleared before each call, which makes it
//! the per-call value the surrounding code treats it as. The original code runs unchanged; off while verifying.

use crate::cpu::Cpu;
use crate::mem::{w8, G};
use crate::verify;
use core::sync::atomic::{AtomicBool, Ordering};

const CAR_SKIDDING: u64 = G + 0x9ca46e;
const BIKE_SKIDDING: u64 = G + 0x9ca475;

static FIX: AtomicBool = AtomicBool::new(false);

/// Host setting (lcs.ini `skid_fix`).
#[no_mangle]
pub extern "C" fn lcs_set_skid_fix(on: i32) {
    FIX.store(on != 0, Ordering::Relaxed);
}

#[inline]
unsafe fn clear(mark: u64) {
    if FIX.load(Ordering::Relaxed) && !verify::enabled() {
        w8(mark, 0);
    }
}

/// void CVehicle::ProcessWheel(...)
#[no_mangle]
pub unsafe extern "C" fn G_4f7414(c: *mut Cpu) {
    clear(CAR_SKIDDING);
    G_4f7414_orig(c);
}

/// void CVehicle::ProcessBikeWheel(...)
#[no_mangle]
pub unsafe extern "C" fn G_4f7aa4(c: *mut Cpu) {
    clear(BIKE_SKIDDING);
    G_4f7aa4_orig(c);
}

extern "C" {
    fn G_4f7414_orig(c: *mut Cpu);
    fn G_4f7aa4_orig(c: *mut Cpu);
}
