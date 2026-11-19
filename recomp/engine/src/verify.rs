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
    W0,
    S0,
    None,
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
        Ret::W0 => ((*c).x[0] as u32 as u64, shadow.x[0] as u32 as u64),
        Ret::S0 => ((*c).v[0][0] as u32 as u64, shadow.v[0][0] as u32 as u64),
        Ret::None => (0, 0),
    };
    CALLS.fetch_add(1, Ordering::Relaxed);
    if ours != theirs {
        MISMATCHES.fetch_add(1, Ordering::Relaxed);
        rt_engine_mismatch(original as usize, ours, theirs);
    }
}

/// Like [`port`], for functions that also write `len` bytes through the pointer in x`reg`. The memory is
/// restored between the original and the port, then both results are compared.
#[inline]
pub unsafe fn port_mem(c: *mut Cpu, original: unsafe extern "C" fn(*mut Cpu), ret: Ret, reg: usize, len: usize, f: impl FnOnce(&mut Cpu)) {
    if !enabled() {
        f(&mut *c);
        return;
    }
    let ptr = (*c).x[reg] as *mut u8;
    let mut before = [0u8; 64];
    let mut theirs = [0u8; 64];
    let len = len.min(64);
    core::ptr::copy_nonoverlapping(ptr, before.as_mut_ptr(), len);
    let mut shadow: Cpu = *c;
    original(&mut shadow);
    core::ptr::copy_nonoverlapping(ptr, theirs.as_mut_ptr(), len);
    core::ptr::copy_nonoverlapping(before.as_ptr(), ptr, len);
    f(&mut *c);
    CALLS.fetch_add(1, Ordering::Relaxed);
    let mut differ = core::slice::from_raw_parts(ptr, len) != &theirs[..len];
    let (ours, other) = match ret {
        Ret::X0 => ((*c).x[0], shadow.x[0]),
        Ret::W0 => ((*c).x[0] as u32 as u64, shadow.x[0] as u32 as u64),
        Ret::S0 => ((*c).v[0][0] as u32 as u64, shadow.v[0][0] as u32 as u64),
        Ret::None => (0, 0),
    };
    differ |= ours != other;
    if differ {
        MISMATCHES.fetch_add(1, Ordering::Relaxed);
        rt_engine_mismatch(original as usize, ours, other);
    }
}

/// Scratch for [`port_regions`], used as a stack because verified ports call other verified ports.
const SCRATCH: usize = 1 << 16;
static mut BEFORE: [u8; SCRATCH] = [0; SCRATCH];
static mut THEIRS: [u8; SCRATCH] = [0; SCRATCH];
static mut USED: usize = 0;

/// Like [`port_mem`], for functions that write several memory ranges (`(address, length)`), such as
/// object fields and globals. `regions` is evaluated before either version runs.
#[inline]
pub unsafe fn port_regions(c: *mut Cpu, original: unsafe extern "C" fn(*mut Cpu), ret: Ret, regions: &[(u64, usize)], f: impl FnOnce(&mut Cpu)) {
    if !enabled() {
        f(&mut *c);
        return;
    }
    let base = USED;
    let total: usize = regions.iter().filter(|r| r.0 != 0).map(|r| r.1).sum();
    if base + total > SCRATCH {
        f(&mut *c); // too deep to verify
        return;
    }
    USED = base + total;
    let before = (core::ptr::addr_of_mut!(BEFORE) as *mut u8).add(base);
    let theirs = (core::ptr::addr_of_mut!(THEIRS) as *mut u8).add(base);
    let mut at = 0;
    for &(p, n) in regions {
        if p == 0 {
            continue;
        }
        core::ptr::copy_nonoverlapping(p as *const u8, before.add(at), n);
        at += n;
    }
    let mut shadow: Cpu = *c;
    original(&mut shadow);
    at = 0;
    for &(p, n) in regions {
        if p == 0 {
            continue;
        }
        core::ptr::copy_nonoverlapping(p as *const u8, theirs.add(at), n);
        core::ptr::copy_nonoverlapping(before.add(at), p as *mut u8, n);
        at += n;
    }
    f(&mut *c);
    CALLS.fetch_add(1, Ordering::Relaxed);
    let mut differ = false;
    let mut first_diff = 0u64;
    let mut first_base = 0u64;
    at = 0;
    for &(p, n) in regions {
        if p == 0 {
            continue;
        }
        let ours = core::slice::from_raw_parts(p as *const u8, n);
        let other = core::slice::from_raw_parts(theirs.add(at), n);
        if let Some(i) = (0..n).find(|&i| ours[i] != other[i]) {
            if !differ {
                first_diff = p + i as u64;
                first_base = p;
            }
            differ = true;
        }
        at += n;
    }
    USED = base;
    let (ours, other) = match ret {
        Ret::X0 => ((*c).x[0], shadow.x[0]),
        Ret::W0 => ((*c).x[0] as u32 as u64, shadow.x[0] as u32 as u64),
        Ret::S0 => ((*c).v[0][0] as u32 as u64, shadow.v[0][0] as u32 as u64),
        Ret::None => (0, 0),
    };
    if differ || ours != other {
        MISMATCHES.fetch_add(1, Ordering::Relaxed);
        // memory differences report the first differing address and its region instead of the results
        if differ {
            rt_engine_mismatch(original as usize, first_diff, first_base);
        } else {
            rt_engine_mismatch(original as usize, ours, other);
        }
    }
}
