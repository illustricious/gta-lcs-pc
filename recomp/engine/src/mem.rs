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
#[inline]
pub unsafe fn r32(a: u64) -> u32 {
    core::ptr::read_unaligned(a as *const u32)
}
#[inline]
pub unsafe fn rf64(a: u64) -> f64 {
    core::ptr::read_unaligned(a as *const f64)
}
#[inline]
pub unsafe fn w32f(a: u64, v: f32) {
    core::ptr::write_unaligned(a as *mut f32, v)
}
#[inline]
pub unsafe fn ri8(a: u64) -> i8 {
    core::ptr::read_unaligned(a as *const i8)
}
#[inline]
pub unsafe fn ri16(a: u64) -> i16 {
    core::ptr::read_unaligned(a as *const i16)
}
#[inline]
pub unsafe fn ri32(a: u64) -> i32 {
    core::ptr::read_unaligned(a as *const i32)
}
#[inline]
pub unsafe fn w8(a: u64, v: u8) {
    core::ptr::write_unaligned(a as *mut u8, v)
}
#[inline]
pub unsafe fn w16(a: u64, v: u16) {
    core::ptr::write_unaligned(a as *mut u16, v)
}
#[inline]
pub unsafe fn w32(a: u64, v: u32) {
    core::ptr::write_unaligned(a as *mut u32, v)
}
#[inline]
pub unsafe fn w64(a: u64, v: u64) {
    core::ptr::write_unaligned(a as *mut u64, v)
}
/// Pointer stored in a GOT slot of libGame (`*(G + slot)`).
#[inline]
pub unsafe fn got(slot: u64) -> u64 {
    r64(G + slot)
}
