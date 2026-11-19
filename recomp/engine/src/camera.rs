//! CCamera and CCam: state queries, fades, widescreen and script setters, rewritten from libGame.so
//! 2.4.379 and shadow-verified. The camera modes themselves (CCam::Process_*) are still recompiled C.
//!
//! CCamera (`this`): +0x8f active CCam index; per-cam fields sit at `this + index * 0x298 + offset`
//! (+0x1bc mode, +0x1d8 look direction, +0x264 FOV); +0x1980 fading, +0x1981 fading for script,
//! +0x1984 fade alpha (0..255), +0x1a90 fade direction.

use crate::cpu::Cpu;
use crate::mem::*;
use crate::verify::{self, Ret};

const CAM_SIZE: u64 = 0x298;

/// Address of the active CCam plus `off` (offsets relative to the CCamera, as the original uses them).
#[inline]
unsafe fn active(this: u64, off: u64) -> u64 {
    this + r8(this + 0x8f) as u64 * CAM_SIZE + off
}

// ---- queries --------------------------------------------------------------------------------------

/// CVector& CCamera::GetGameCamPosition()
#[no_mangle]
pub unsafe extern "C" fn G_3135fc(c: *mut Cpu) {
    verify::port(c, G_3135fc_orig, Ret::X0, |c| c.x[0] += 0xaa0);
}

/// bool CCamera::Get_Just_Switched_Status()
#[no_mangle]
pub unsafe extern "C" fn G_329e10(c: *mut Cpu) {
    verify::port(c, G_329e10_orig, Ret::W0, |c| {
        let v = r8(c.x[0] + 0x72);
        c.set_w(0, v as u32);
    });
}

/// float CCamera::GetPositionAlongSpline()
#[no_mangle]
pub unsafe extern "C" fn G_329ee8(c: *mut Cpu) {
    verify::port(c, G_329ee8_orig, Ret::S0, |c| {
        let v = rf32(c.x[0] + 0x150);
        c.set_s(0, v);
    });
}

/// bool CCamera::GetFading()
#[no_mangle]
pub unsafe extern "C" fn G_329dd4(c: *mut Cpu) {
    verify::port(c, G_329dd4_orig, Ret::W0, |c| {
        let v = r8(c.x[0] + 0x1980);
        c.set_w(0, v as u32);
    });
}

/// bool CCamera::GetFadingStatusForScript()
#[no_mangle]
pub unsafe extern "C" fn G_329de0(c: *mut Cpu) {
    verify::port(c, G_329de0_orig, Ret::W0, |c| {
        let v = r8(c.x[0] + 0x1981);
        c.set_w(0, v as u32);
    });
}

/// int CCamera::GetScreenFadeStatus(): 0 clear, 1 fading, 2 black.
#[no_mangle]
pub unsafe extern "C" fn G_328164(c: *mut Cpu) {
    verify::port(c, G_328164_orig, Ret::W0, |c| {
        let a = rf32(c.x[0] + 0x1984);
        let v = if a == 0.0 { 0 } else if a == 255.0 { 2 } else { 1 };
        c.set_w(0, v);
    });
}

/// int CCamera::GetFadingDirection(): 2 when not fading, else whether fading out.
#[no_mangle]
pub unsafe extern "C" fn G_329dec(c: *mut Cpu) {
    verify::port(c, G_329dec_orig, Ret::W0, |c| {
        let this = c.x[0];
        let v = if r8(this + 0x1980) == 0 { 2 } else { (r16(this + 0x1a90) == 1) as u32 };
        c.set_w(0, v);
    });
}

/// float CCamera::FindCamFOV()
#[no_mangle]
pub unsafe extern "C" fn G_3299c4(c: *mut Cpu) {
    verify::port(c, G_3299c4_orig, Ret::S0, |c| {
        let v = rf32(active(c.x[0], 0x264));
        c.set_s(0, v);
    });
}

/// bool CCamera::ShouldPedControlsBeRelative(): the active cam is in mode 16 (first person).
#[no_mangle]
pub unsafe extern "C" fn G_32e3f4(c: *mut Cpu) {
    verify::port(c, G_32e3f4_orig, Ret::W0, |c| {
        let v = r16(active(c.x[0], 0x1bc)) == 0x10;
        c.set_w(0, v as u32);
    });
}

/// bool CCamera::GetLookingLRBFirstPerson(): first person and looking left, right or behind.
#[no_mangle]
pub unsafe extern "C" fn G_329e18(c: *mut Cpu) {
    verify::port(c, G_329e18_orig, Ret::W0, |c| {
        let this = c.x[0];
        let v = r16(active(this, 0x1bc)) == 0x10 && r32(active(this, 0x1d8)) != 3;
        c.set_w(0, v as u32);
    });
}

/// bool CCamera::GetLookingForwardFirstPerson()
#[no_mangle]
pub unsafe extern "C" fn G_329eac(c: *mut Cpu) {
    verify::port(c, G_329eac_orig, Ret::W0, |c| {
        let this = c.x[0];
        let v = r16(active(this, 0x1bc)) == 0x10 && r32(active(this, 0x1d8)) == 3;
        c.set_w(0, v as u32);
    });
}

/// bool CCamera::Using1stPersonWeaponMode(): player weapon modes 7, 8, 34, 45 and 46.
#[no_mangle]
pub unsafe extern "C" fn G_32e1f8(c: *mut Cpu) {
    verify::port(c, G_32e1f8_orig, Ret::W0, |c| {
        let m = (r16(c.x[0] + 0x984) as u32).wrapping_sub(7);
        let v = m < 0x28 && (0x00c0_0800_0003u64 >> m) & 1 != 0;
        c.set_w(0, v as u32);
    });
}

/// bool CCamera::GetArrPosForVehicleType(eVehicleAppearance, int& index)
#[no_mangle]
pub unsafe extern "C" fn G_31fb84(c: *mut Cpu) {
    let out = (*c).x[2];
    verify::port_regions(c, G_31fb84_orig, Ret::W0, &[(out, 4)], |c| {
        let i = c.w(1).wrapping_sub(1);
        if i <= 4 {
            w32(out, r32(G + 0x23a348 + i as u64 * 4));
        }
        c.set_w(0, (i < 5) as u32);
    });
}

/// bool CCam::GetWeaponFirstPersonOn(): the cam's target is a ped whose current weapon is in first person.
#[no_mangle]
pub unsafe extern "C" fn G_313a78(c: *mut Cpu) {
    verify::port(c, G_313a78_orig, Ret::W0, |c| {
        let e = r64(c.x[0] + 0x250);
        let v = e != 0 && r64(e + 0x58) & 0xe == 6 && {
            let slot = ri8(e + 0x7cc) as i64 as u64;
            r8(e.wrapping_add(slot.wrapping_mul(0x1c)) + 0x6bc) != 0
        };
        c.set_w(0, v as u32);
    });
}

// ---- setters --------------------------------------------------------------------------------------

/// void CCamera::Enable1rstPersonCamCntrlsScript()
#[no_mangle]
pub unsafe extern "C" fn G_32807c(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_32807c_orig, Ret::None, &[(this + 0x89, 1)], |_| w8(this + 0x89, 1));
}

/// void CCamera::Enable1rstPersonWeaponsCamera()
#[no_mangle]
pub unsafe extern "C" fn G_328088(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_328088_orig, Ret::None, &[(this + 0x8a, 1)], |_| w8(this + 0x8a, 1));
}

/// void CCamera::DontProcessObbeCinemaCamera()
#[no_mangle]
pub unsafe extern "C" fn G_327324(c: *mut Cpu) {
    let flag = got(0x7f9630);
    verify::port_regions(c, G_327324_orig, Ret::None, &[(flag, 1)], |_| w8(flag, 0));
}

/// void CCamera::SetNearClipScript(float)
#[no_mangle]
pub unsafe extern "C" fn G_32af08(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_32af08_orig, Ret::None, &[(this + 0x79, 1), (this + 0x13c, 4)], |c| {
        w32f(this + 0x13c, c.s(0));
        w8(this + 0x79, 1);
    });
}

/// void CCamera::SetWideScreenOn()
#[no_mangle]
pub unsafe extern "C" fn G_32dffc(c: *mut Cpu) {
    let (this, flag) = ((*c).x[0], got(0x7f9640));
    verify::port_regions(c, G_32dffc_orig, Ret::None, &[(this + 0x85, 1), (flag, 1)], |_| {
        w8(this + 0x85, 1);
        w8(flag, 0);
    });
}

/// void CCamera::SetWideScreenOff()
#[no_mangle]
pub unsafe extern "C" fn G_326a20(c: *mut Cpu) {
    let (this, flag, dst) = ((*c).x[0], got(0x7f9640), got(0x7f9648));
    verify::port_regions(c, G_326a20_orig, Ret::None, &[(this + 0x84, 1), (flag, 1), (dst, 8)], |_| {
        w8(flag, 1);
        w8(this + 0x84, r8(this + 0x85));
        w64(dst, r64(got(0x7f9638)));
    });
}

/// void CCamera::ProcessWideScreenOn(): borders and FOV for widescreen cutscenes.
#[no_mangle]
pub unsafe extern "C" fn G_32bf70(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_32bf70_orig, Ret::None, &[(this + 0x84, 2), (this + 0x138, 4), (this + 0x154, 4), (this + 0x184, 4)], |_| {
        let (a, b) = if r8(this + 0x84) != 0 {
            w16(this + 0x84, 0);
            (0.0, 0.0)
        } else {
            (rf32(active(this, 0x264)) * 0.0, 1.0)
        };
        w32f(this + 0x138, a);
        w32f(this + 0x184, b);
        w32(this + 0x154, 0);
    });
}

/// void CCamera::SetCamCutSceneOffSet(const CVector&)
#[no_mangle]
pub unsafe extern "C" fn G_32e080(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_32e080_orig, Ret::None, &[(this + 0x9f0, 16)], |c| copy4(this + 0x9f0, c.x[1]));
}

/// void CCamera::UpdateAimingCoors(const CVector&)
#[no_mangle]
pub unsafe extern "C" fn G_32e438(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_32e438_orig, Ret::None, &[(this + 0x9b0, 16)], |c| copy4(this + 0x9b0, c.x[1]));
}

/// Four floats, one at a time like the original (the source may overlap nothing else).
unsafe fn copy4(dst: u64, src: u64) {
    for i in 0..4 {
        w32(dst + 4 * i, r32(src + 4 * i));
    }
}

/// void CCamera::TakeControlWithSpline(short mode)
#[no_mangle]
pub unsafe extern "C" fn G_32e410(c: *mut Cpu) {
    let this = (*c).x[0];
    let regions = [(this + 0x65, 1), (this + 0x73, 2), (this + 0x7a, 1), (this + 0x1a98, 2), (this + 0x1a9c, 2), (this + 0x1ae4, 1)];
    verify::port_regions(c, G_32e410_orig, Ret::None, &regions, |c| {
        w16(this + 0x73, 0);
        w8(this + 0x65, 0);
        w8(this + 0x1ae4, 1);
        w16(this + 0x1a98, 0x11);
        w16(this + 0x1a9c, c.w(1) as u16);
        w8(this + 0x7a, 1);
    });
}

/// void CCamera::SetParametersForScriptInterpolation(float stay, float through, unsigned time)
#[no_mangle]
pub unsafe extern "C" fn G_32e220(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_32e220_orig, Ret::None, &[(this + 0x198a, 1), (this + 0x1a84, 12)], |c| {
        w32(this + 0x1a8c, c.w(1));
        w8(this + 0x198a, 1);
        w32f(this + 0x1a84, c.s(0) / 100.0);
        w32f(this + 0x1a88, c.s(1) / 100.0);
    });
}

/// void CCamera::SetZoomValueFollowPedScript(short zoom)
#[no_mangle]
pub unsafe extern "C" fn G_32e310(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_32e310_orig, Ret::None, &[(this + 0x80, 1), (this + 0x148, 4)], |c| {
        let z = c.w(1);
        if z & 0xffff <= 2 {
            w32(this + 0x148, r32(G + 0x23a35c + (z as u16 as i16 as i64 as u64) * 4));
        }
        w8(this + 0x80, 1);
    });
}

extern "C" {
    fn G_3135fc_orig(c: *mut Cpu);
    fn G_329e10_orig(c: *mut Cpu);
    fn G_329ee8_orig(c: *mut Cpu);
    fn G_329dd4_orig(c: *mut Cpu);
    fn G_329de0_orig(c: *mut Cpu);
    fn G_328164_orig(c: *mut Cpu);
    fn G_329dec_orig(c: *mut Cpu);
    fn G_3299c4_orig(c: *mut Cpu);
    fn G_32e3f4_orig(c: *mut Cpu);
    fn G_329e18_orig(c: *mut Cpu);
    fn G_329eac_orig(c: *mut Cpu);
    fn G_32e1f8_orig(c: *mut Cpu);
    fn G_31fb84_orig(c: *mut Cpu);
    fn G_313a78_orig(c: *mut Cpu);
    fn G_32807c_orig(c: *mut Cpu);
    fn G_328088_orig(c: *mut Cpu);
    fn G_327324_orig(c: *mut Cpu);
    fn G_32af08_orig(c: *mut Cpu);
    fn G_32dffc_orig(c: *mut Cpu);
    fn G_326a20_orig(c: *mut Cpu);
    fn G_32bf70_orig(c: *mut Cpu);
    fn G_32e080_orig(c: *mut Cpu);
    fn G_32e438_orig(c: *mut Cpu);
    fn G_32e410_orig(c: *mut Cpu);
    fn G_32e220_orig(c: *mut Cpu);
    fn G_32e310_orig(c: *mut Cpu);
}
