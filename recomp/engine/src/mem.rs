//! Guest memory access. Guest pointers are host pointers (modules are mapped at their link
//! bases), so a guest address is dereferenced directly. Reads are unaligned-safe.

/// libGame.so is mapped at this base; `G + va` is the host address of a virtual address.
pub const G: u64 = 0x2_0000_0000;

#[inline]
pub unsafe fn r8(a: u64) -> u8 {
    core::ptr::read_unaligned(a as *const u8)
}
#[inline]
pub unsafe fn r16(a: u64) -> u16 {
    core::ptr::read_unaligned(a as *const u16)
}
#[inline]
pub unsafe fn r64(a: u64) -> u64 {
    core::ptr::read_unaligned(a as *const u64)
}
#[inline]
pub unsafe fn rf32(a: u64) -> f32 {
    core::ptr::read_unaligned(a as *const f32)
}
