//! Calling recompiled guest functions from Rust, and ARM64 floating-point helpers.

use crate::cpu::Cpu;
use crate::mem::G;

extern "C" {
    fn rt_call(c: *mut Cpu, target: u64);
}

/// Call a guest function at libGame virtual address `va` with up to 8 integer arguments.
/// The caller's CPU state is saved and restored, so a port can call helpers freely.
/// Returns (x0, s0).
pub unsafe fn call(c: *mut Cpu, va: u64, args: &[u64]) -> (u64, f32) {
    let saved: Cpu = *c;
    for (i, a) in args.iter().enumerate() {
        (*c).x[i] = *a;
    }
    rt_call(c, G + va);
    let result = ((*c).x[0], (*c).s(0));
    *c = saved;
    result
}

/// ARM64 fmadd: a * b + c with a single rounding (the recompiled C uses fmaf, i.e. vfmadd).
#[inline]
pub fn fma(a: f32, b: f32, c: f32) -> f32 {
    #[target_feature(enable = "fma")]
    unsafe fn inner(a: f32, b: f32, c: f32) -> f32 {
        use core::arch::x86_64::*;
        _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)))
    }
    unsafe { inner(a, b, c) }
}

/// Display::GetScreenWidth / GetScreenHeight (ints, at least 1).
pub unsafe fn screen_size(c: *mut Cpu) -> (i32, i32) {
    let w = call(c, 0x57785c, &[]).0 as i32;
    let h = call(c, 0x5778a8, &[]).0 as i32;
    (w, h)
}

/// fcvtzu to 32 bits: truncate, saturate, NaN -> 0 (Rust `as` casts behave the same).
#[inline]
pub fn fcvtzu(v: f32) -> u32 {
    v as u32
}

/// IEEE square root (ARM64 fsqrt), without std.
#[inline]
pub fn sqrt(v: f32) -> f32 {
    use core::arch::x86_64::*;
    unsafe { _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v))) }
}

/// Like [`call`], also passing float arguments in s0..s7.
pub unsafe fn call_f(c: *mut Cpu, va: u64, args: &[u64], fargs: &[f32]) -> (u64, f32) {
    let saved: Cpu = *c;
    for (i, a) in args.iter().enumerate() {
        (*c).x[i] = *a;
    }
    for (i, a) in fargs.iter().enumerate() {
        (*c).set_s(i, *a);
    }
    rt_call(c, G + va);
    let result = ((*c).x[0], (*c).s(0));
    *c = saved;
    result
}

/// ARM64 fmin / fmax (NaN in, NaN out; -0 is below +0).
pub fn fmin(a: f32, b: f32) -> f32 {
    if a.is_nan() || b.is_nan() {
        return f32::NAN;
    }
    if a == b {
        return f32::from_bits(a.to_bits() | b.to_bits());
    }
    if a < b { a } else { b }
}
pub fn fmax(a: f32, b: f32) -> f32 {
    if a.is_nan() || b.is_nan() {
        return f32::NAN;
    }
    if a == b {
        return f32::from_bits(a.to_bits() & b.to_bits());
    }
    if a > b { a } else { b }
}
