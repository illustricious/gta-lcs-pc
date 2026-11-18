//! Shadow verification of ports against the recompiled originals.

use crate::cpu::Cpu;
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};

static ENABLED: AtomicBool = AtomicBool::new(false);
static CALLS: AtomicU64 = AtomicU64::new(0);
static MISMATCHES: AtomicU64 = AtomicU64::new(0);

/// Which register holds a function's result.
#[derive(Clone, Copy)]
pub enum Ret {
    X0,
    S0,
}

#[inline]
pub fn enabled() -> bool {
    ENABLED.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn lcs_engine_verify(on: i32) {
    ENABLED.store(on != 0, Ordering::Relaxed);
}

/// out[0] = verified calls, out[1] = calls whose result differed from the original.
#[no_mangle]
pub unsafe extern "C" fn lcs_engine_verify_counts(out: *mut u64) {
    *out = CALLS.load(Ordering::Relaxed);
    *out.add(1) = MISMATCHES.load(Ordering::Relaxed);
}

extern "C" {
    /// Host log for a mismatch: original function, Rust result, original result.
    fn rt_engine_mismatch(original: usize, ours: u64, theirs: u64);
}

/// Run a port. While verifying, the original also runs on a copy of the CPU (the ported functions
/// are leaves that only touch registers) and the results are compared bit for bit.
#[inline]
pub unsafe fn port(c: *mut Cpu, original: unsafe extern "C" fn(*mut Cpu), ret: Ret, f: impl FnOnce(&mut Cpu)) {
    if !enabled() {
        f(&mut *c);
        return;
    }
    let mut shadow: Cpu = *c;
    original(&mut shadow);
    f(&mut *c);
    let (ours, theirs) = match ret {
        Ret::X0 => ((*c).x[0], shadow.x[0]),
        Ret::S0 => ((*c).v[0][0] as u32 as u64, shadow.v[0][0] as u32 as u64),
    };
    CALLS.fetch_add(1, Ordering::Relaxed);
    if ours != theirs {
        MISMATCHES.fetch_add(1, Ordering::Relaxed);
        rt_engine_mismatch(original as usize, ours, theirs);
    }
}
