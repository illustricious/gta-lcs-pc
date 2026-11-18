//! Guest CPU state, laid out exactly like `struct CPU` in rt/recomp.h
//! (rt/engine_bridge.c checks the size at compile time).

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Cpu {
    pub x: [u64; 32], // x[31] is SP
    pub v: [[u64; 2]; 32],
    pub nf: u32,
    pub zf: u32,
    pub cf: u32,
    pub vf: u32,
    pub tpidr: u64,
    pub excl_addr: u64,
    pub excl_val: u64,
    pub trace: [u64; 64],
    pub trace_pos: u32,
    pub depth: u32,
    pub host: *mut core::ffi::c_void,
}

impl Cpu {
    /// Low 32 bits of a general register (wN).
    #[inline]
    pub fn w(&self, n: usize) -> u32 {
        self.x[n] as u32
    }
    /// sN: the low float of vector register N.
    #[inline]
    pub fn s(&self, n: usize) -> f32 {
        f32::from_bits(self.v[n][0] as u32)
    }
    /// Writing sN clears the rest of the register, as on ARM64.
    #[inline]
    pub fn set_s(&mut self, n: usize, value: f32) {
        self.v[n] = [value.to_bits() as u64, 0];
    }
    /// Writing wN clears the upper half of xN.
    #[inline]
    pub fn set_w(&mut self, n: usize, value: u32) {
        self.x[n] = value as u64;
    }
}
