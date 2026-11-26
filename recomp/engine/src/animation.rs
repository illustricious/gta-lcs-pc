//! Animation playback bookkeeping (CAnimBlendAssociation, CAnimBlendNode), rewritten from libGame.so 2.4.379.
//! Behaviour is unchanged; pose evaluation (CAnimBlendNode::Update, key frame interpolation) is still recompiled C.
//!
//! CAnimBlendAssociation: +0x10 u16 flags (1 playing, 2 looped, 4 delete when faded out, 8 fade out at the end,
//! 0x20 speed from the caller), +0x14 f32 time step, +0x18 blend amount, +0x20 i32 node count, +0x28 nodes (0x30
//! bytes each, +0x20 sequence), +0x30 hierarchy (+0x24 f32 total time), +0x38 blend delta, +0x3c current time,
//! +0x40 speed, +0x48 u32 callback type, +0x50 callback, +0x58 its argument.
//!
//! CAnimBlendNode: +0x08 current key frame offset, +0x10 previous one, +0x18 time left in the frame, +0x20 sequence
//! (+0 u16 flags, +2 i16 frame count, +8 frames), +0x28 association.
//!
//! Ports that call back into the game are only shadow-verified on the paths without such a call.

use crate::cpu::Cpu;
use crate::guest::call;
use crate::mem::*;
use crate::verify::{self, Ret};

// PLT entries, so hooks apply
const FIND_KEY_FRAME: u64 = 0x7c97e0; // CAnimBlendNode::FindKeyFrame(float)
const NEXT_KEY_FRAME: u64 = 0x7c9930; // CAnimBlendNode::NextKeyFrame()
const ASSOC_DESTRUCT: u64 = 0x7c95c0; // CAnimBlendAssociation::~CAnimBlendAssociation
const OPERATOR_DELETE: u64 = 0x7c8b50;

const NODE_SIZE: u64 = 0x30;

extern "C" {
    fn G_2e91d4_orig(c: *mut Cpu);
    fn G_2e973c_orig(c: *mut Cpu);
    fn G_2e6d64_orig(c: *mut Cpu);
    fn G_2e6e0c_orig(c: *mut Cpu);
    fn G_2e6ed8_orig(c: *mut Cpu);
    fn G_2e6f84_orig(c: *mut Cpu);
    fn G_2e6f98_orig(c: *mut Cpu);
    fn G_2e762c_orig(c: *mut Cpu);
    fn G_2e765c_orig(c: *mut Cpu);
}

/// Call a guest function pointer (an absolute guest address).
unsafe fn callback(c: &mut Cpu, target: u64, this: u64, arg: u64) {
    call(c, target.wrapping_sub(G), &[this, arg]);
}

#[inline]
unsafe fn total_time(this: u64) -> f32 {
    rf32(r64(this + 0x30) + 0x24)
}

/// Bring the current time back into the animation: wrap when looped, else stop at the end.
unsafe fn wrap_time(this: u64) {
    let mut now = rf32(this + 0x3c);
    let total = total_time(this);
    if total <= now {
        if r16(this + 0x10) & 2 != 0 {
            loop {
                now -= total_time(this);
                w32f(this + 0x3c, now);
                if !(now >= total_time(this)) {
                    break;
                }
            }
        } else {
            w32f(this + 0x3c, total);
        }
    }
}

/// Every node with a sequence looks up its key frame for the current time.
unsafe fn find_key_frames(c: &mut Cpu, this: u64) {
    let mut i: i64 = 0;
    while i < ri32(this + 0x20) as i64 {
        let node = r64(this + 0x28).wrapping_add(i as u64 * NODE_SIZE);
        if r64(node + 0x20) != 0 {
            let time = rf32(this + 0x3c);
            crate::guest::call_f(c, FIND_KEY_FRAME, &[node], &[time]);
        }
        i += 1;
    }
}

/// Verification regions of an association and its nodes.
unsafe fn with_nodes(this: u64) -> [(u64, usize); 2] {
    let n = ri32(this + 0x20).clamp(0, 64) as usize;
    [(this, 0x60), (if n > 0 { r64(this + 0x28) } else { 0 }, n * NODE_SIZE as usize)]
}

/// void CAnimBlendAssociation::SetCurrentTime(float)
#[no_mangle]
pub unsafe extern "C" fn G_2e6d64(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_2e6d64_orig, Ret::None, &with_nodes(this), |c| {
        w32f(this + 0x3c, c.s(0));
        wrap_time(this);
        find_key_frames(c, this);
    });
}

/// void CAnimBlendAssociation::SyncAnimation(CAnimBlendAssociation *)
#[no_mangle]
pub unsafe extern "C" fn G_2e6e0c(c: *mut Cpu) {
    let (this, other) = ((*c).x[0], (*c).x[1]);
    verify::port_regions(c, G_2e6e0c_orig, Ret::None, &with_nodes(this), |c| {
        let now = rf32(other + 0x3c) / total_time(other) * total_time(this);
        w32f(this + 0x3c, now);
        wrap_time(this);
        find_key_frames(c, this);
    });
}

/// void CAnimBlendAssociation::Start(float)
#[no_mangle]
pub unsafe extern "C" fn G_2e6ed8(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_2e6ed8_orig, Ret::None, &with_nodes(this), |c| {
        w32f(this + 0x3c, c.s(0));
        w16(this + 0x10, r16(this + 0x10) | 1);
        wrap_time(this);
        find_key_frames(c, this);
    });
}

/// void CAnimBlendAssociation::SetBlendTo(float amount, float speed)
#[no_mangle]
pub unsafe extern "C" fn G_2e6f84(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_2e6f84_orig, Ret::None, &[(this, 0x60)], |c| {
        w32f(this + 0x38, (c.s(0) - rf32(this + 0x18)) * c.s(1));
    });
}

/// void CAnimBlendAssociation::SetBlend(float amount, float delta)
#[no_mangle]
pub unsafe extern "C" fn G_2e6f98(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_2e6f98_orig, Ret::None, &[(this, 0x60)], |c| {
        w32f(this + 0x18, c.s(0));
        w32f(this + 0x38, c.s(1));
    });
}

unsafe fn update_blend(c: &mut Cpu, this: u64) -> u32 {
    let delta = rf32(this + 0x38);
    let blend = rf32(this + 0x18) + delta * c.s(0);
    w32f(this + 0x18, blend);
    if blend <= 0.0 && delta < 0.0 {
        let flags = r8(this + 0x10);
        w32(this + 0x18, 0);
        w32(this + 0x38, 0);
        if flags & 4 == 0 {
            return 1;
        }
        if r32(this + 0x48).wrapping_sub(1) <= 1 {
            callback(c, r64(this + 0x50), this, r64(this + 0x58));
        }
        if this != 0 {
            call(c, ASSOC_DESTRUCT, &[this]);
            call(c, OPERATOR_DELETE, &[this]);
        }
        return 0;
    }
    if blend > 1.0 {
        w32(this + 0x18, 0x3f80_0000);
        if delta > 0.0 {
            w32(this + 0x38, 0);
        }
    }
    1
}

/// bool CAnimBlendAssociation::UpdateBlend(float step): false when the association deleted itself.
#[no_mangle]
pub unsafe extern "C" fn G_2e91d4(c: *mut Cpu) {
    let this = (*c).x[0];
    let delta = rf32(this + 0x38);
    let blend = rf32(this + 0x18) + delta * (*c).s(0);
    let deletes = blend <= 0.0 && delta < 0.0 && r8(this + 0x10) & 4 != 0;
    if deletes {
        let v = update_blend(&mut *c, this);
        (*c).set_w(0, v);
    } else {
        verify::port_regions(c, G_2e91d4_orig, Ret::W0, &[(this, 0x60)], |c| {
            let v = update_blend(c, this);
            c.set_w(0, v);
        });
    }
}

unsafe fn update_time(c: &mut Cpu, this: u64) {
    let mut flags = r16(this + 0x10);
    if flags & 1 == 0 {
        return;
    }
    let total = total_time(this);
    let now = rf32(this + 0x3c);
    if now >= total {
        w16(this + 0x10, flags & !1);
        return;
    }
    let speed = if flags & 0x20 != 0 { total * c.s(1) } else { rf32(this + 0x40) };
    let step = speed * c.s(0);
    let next = now + step;
    w32f(this + 0x14, step);
    w32f(this + 0x3c, next);
    let total = total_time(this);
    if !(next >= total) {
        return;
    }
    if flags & 2 != 0 {
        w32f(this + 0x3c, next - total);
        return;
    }
    w32f(this + 0x3c, total);
    if flags & 8 != 0 {
        flags |= 4;
        w16(this + 0x10, flags);
        w32(this + 0x38, 0xc080_0000); // fade out at -4 per second
    }
    if r32(this + 0x48) == 1 {
        let (f, arg) = (r64(this + 0x50), r64(this + 0x58));
        w32(this + 0x48, 0);
        callback(c, f, this, arg);
    }
}

/// bool CAnimBlendAssociation::UpdateTime(float step, float rate): always true.
#[no_mangle]
pub unsafe extern "C" fn G_2e973c(c: *mut Cpu) {
    let this = (*c).x[0];
    if r32(this + 0x48) == 1 {
        update_time(&mut *c, this); // may call the finish callback
        (*c).set_w(0, 1);
    } else {
        verify::port_regions(c, G_2e973c_orig, Ret::W0, &[(this, 0x60)], |c| {
            update_time(c, this);
            c.set_w(0, 1);
        });
    }
}

/// bool CAnimBlendNode::UpdateTime(): true when it moved to a next key frame.
#[no_mangle]
pub unsafe extern "C" fn G_2e762c(c: *mut Cpu) {
    let this = (*c).x[0];
    let assoc = r64(this + 0x28);
    let playing = r16(assoc + 0x10) & 1 != 0;
    let left = rf32(this + 0x18) - rf32(assoc + 0x14);
    if playing && left <= 0.0 {
        w32f(this + 0x18, left);
        let v = call(c, NEXT_KEY_FRAME, &[this]).0;
        (*c).x[0] = v;
    } else {
        verify::port_regions(c, G_2e762c_orig, Ret::W0, &[(this, 0x30)], |c| {
            if playing {
                w32f(this + 0x18, left);
            }
            c.set_w(0, 0);
        });
    }
}

/// Half-float frame duration of a packed key frame.
#[inline]
fn frame_time(h: u16) -> f32 {
    let w = h as u32;
    let shifted = w << 16;
    let mut bits = shifted & 0x8000_0000;
    bits |= ((w >> 10) & 0x1f) << 23;
    bits |= (w & 0x3ff) << 13;
    let value = if w & 0x7fff != 0 { bits.wrapping_add(0x3800_0000) } else { shifted };
    f32::from_bits(value)
}

unsafe fn next_key_frame_no_calc(this: u64) -> u32 {
    let seq = r64(this + 0x20);
    let count = ri16(seq + 2) as i32;
    if count < 2 {
        return 0;
    }
    let current = r64(this + 8);
    w64(this + 0x10, current);
    let flags = r16(seq);
    let stride: u32 = if flags & 2 != 0 { 0x10 } else { 0xa };
    let mut frames = r64(seq + 8);
    let mut time = rf32(this + 0x18);
    let mut at = frames.wrapping_add(current);
    let mut looped = 0u32;
    if !(time > 0.0 || time.is_nan()) {
        let end = frames.wrapping_add(stride.wrapping_mul(count as u32) as u64);
        loop {
            at = at.wrapping_add(stride as u64);
            if at >= end {
                if r16(r64(this + 0x28) + 0x10) & 2 == 0 {
                    w32(this + 0x18, 0);
                    return 0;
                }
                at = r64(seq + 8);
                looped = 1;
            }
            time += frame_time(r16(at + 8));
            w32f(this + 0x18, time);
            if time > 0.0 || time.is_nan() {
                break;
            }
        }
        frames = r64(seq + 8);
    }
    let offset = at.wrapping_sub(frames);
    let previous = if at == frames {
        stride.wrapping_mul((count - 1) as u32) as i32 as i64 as u64
    } else {
        offset.wrapping_sub(stride as u64)
    };
    w64(this + 8, offset);
    w64(this + 0x10, previous);
    looped & 1
}

/// bool CAnimBlendNode::NextKeyFrameNoCalc(): true when the sequence wrapped around.
#[no_mangle]
pub unsafe extern "C" fn G_2e765c(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_2e765c_orig, Ret::W0, &[(this, 0x30)], |c| {
        let v = next_key_frame_no_calc(this);
        c.set_w(0, v);
    });
}
