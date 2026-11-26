//! Rust reimplementations of engine functions of GTA: Liberty City Stories (Android 2.4.379).
//!
//! The recompiled game calls every function through its public C symbol (`G_<address>`). For a
//! function listed in `HOST_HOOKS` (recomp/lift.py) the recompiled body is renamed `G_<address>_orig`,
//! and the symbol exported here takes its place. Each port keeps the original ABI: arguments and
//! results stay in the guest registers of the [`Cpu`] it receives.
//!
//! Verification: with `LCS_RS_VERIFY=1` (see rt/engine_bridge.c) every ported call also runs the
//! original on a copy of the CPU and counts differences; new behaviour (such as the far LOD scale)
//! is switched off while verifying so the ports must match the original bit for bit.
#![no_std]

mod animation;
mod camera;
mod cpu;
mod guest;
mod handling;
mod hud;
mod lod;
mod mem;
mod pad;
mod timecycle;
mod transmission;
pub mod verify;

pub use cpu::Cpu;

#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    extern "C" {
        fn rt_engine_panic() -> !;
    }
    unsafe { rt_engine_panic() }
}
