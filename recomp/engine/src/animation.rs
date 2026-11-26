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
use crate::guest::{call, fma};
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
    fn G_2e8e20_orig(c: *mut Cpu);
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

// ---- per-frame update of a skeleton ----------------------------------------------------------------
//
// RslElementGroup +0x08 frame, +0x30 clump data. Clump data: +0x00 first association (associations start with the
// next link), +0x10 i32 bone count, +0x18 pointer (non-null: bones flagged 8 are skipped), +0x20 bone data (0x20
// bytes each, byte 0 flags). The pose itself is written by the ForAllNodes callbacks, which stay recompiled C.

const GET_NODE: u64 = 0x7c9800; // CAnimBlendAssociation::GetNode(int)
const FOR_ALL_NODES: u64 = 0x7c9870; // CAnimBlendElementGroupData::ForAllNodes(callback, data)
const IS_SKINNED: u64 = 0x7c9660;
const UPDATE_FRAMES: u64 = 0x7c9a60;
const CURRENT_CLUMP: u64 = G + 0x847d20;
const GOT_CALLBACK_FIRST: u64 = 0x7f8fe0;
const GOT_CALLBACK_PLAIN: u64 = 0x7f8fe8;
const GOT_CALLBACK_SKINNED: u64 = 0x7f8ff0;
const MAX_BLENDED: usize = 11;

/// AnimBlendUpdateData: a flag and the node arrays of the associations in play, null-terminated.
#[repr(C)]
struct UpdateData {
    moving: u32,
    _pad: u32,
    nodes: [u64; MAX_BLENDED + 2],
}

/// Blend step of one association. Returns false when it deleted itself.
unsafe fn blend_step(c: &mut Cpu, assoc: u64, step: f32) -> bool {
    let delta = rf32(assoc + 0x38);
    let blend = rf32(assoc + 0x18) + delta * step;
    w32f(assoc + 0x18, blend);
    if blend <= 0.0 && delta < 0.0 {
        let flags = r8(assoc + 0x10);
        w32(assoc + 0x18, 0);
        w32(assoc + 0x38, 0);
        if flags & 4 != 0 {
            if r32(assoc + 0x48).wrapping_sub(1) <= 1 {
                callback(c, r64(assoc + 0x50), assoc, r64(assoc + 0x58));
            }
            call(c, ASSOC_DESTRUCT, &[assoc]);
            call(c, OPERATOR_DELETE, &[assoc]);
            return false;
        }
    } else if blend > 1.0 {
        w32(assoc + 0x18, 0x3f80_0000);
        if delta > 0.0 {
            w32(assoc + 0x38, 0);
        }
    }
    true
}

/// Advance one association's time. `sync` is the shared rate of the associations that run in step (flag 0x20),
/// None when their weighted length is zero.
unsafe fn time_step(c: &mut Cpu, assoc: u64, step: f32, sync: Option<f32>) {
    let mut flags = r16(assoc + 0x10);
    if flags & 1 == 0 {
        return;
    }
    let now = rf32(assoc + 0x3c);
    let total = total_time(assoc);
    if now >= total {
        w16(assoc + 0x10, flags & !1);
        return;
    }
    let rate = if flags & 0x20 == 0 {
        rf32(assoc + 0x40)
    } else {
        match sync {
            Some(s) => s * total,
            None => total,
        }
    };
    let advance = rate * step;
    let next = now + advance;
    w32f(assoc + 0x14, advance);
    w32f(assoc + 0x3c, next);
    let total = total_time(assoc);
    if !(next >= total) {
        return;
    }
    if flags & 2 != 0 {
        w32f(assoc + 0x3c, next - total);
        return;
    }
    w32f(assoc + 0x3c, total);
    if flags & 8 != 0 {
        flags |= 4;
        w32(assoc + 0x38, 0xc080_0000);
        w16(assoc + 0x10, flags);
    }
    if r32(assoc + 0x48) == 1 {
        let (f, arg) = (r64(assoc + 0x50), r64(assoc + 0x58));
        w32(assoc + 0x48, 0);
        callback(c, f, assoc, arg);
    }
}

unsafe fn update_animations(c: &mut Cpu, group: u64, step: f32, second_path: bool) {
    let clump = r64(group + 0x30);
    w64(CURRENT_CLUMP, clump);
    let mut assoc = r64(clump);
    if assoc == 0 {
        return;
    }
    let mut data = UpdateData { moving: 0, _pad: 0, nodes: [0; MAX_BLENDED + 2] };
    let mut count = 0usize;
    let mut blend_sum = 0.0f32; // of the synchronised associations
    let mut length_sum = 0.0f32; // total time / speed, weighted by blend
    loop {
        let next = r64(assoc);
        if (step == 0.0 || blend_step(c, assoc, step)) && r64(r64(assoc + 0x30)) != 0 {
            data.nodes[count] = call(c, GET_NODE, &[assoc, 0]).0;
            count += 1;
            if r16(assoc + 0x10) & 0x20 != 0 {
                let length = total_time(assoc) / rf32(assoc + 0x40);
                let blend = rf32(assoc + 0x18);
                blend_sum += blend;
                length_sum = fma(length, blend, length_sum);
            } else {
                data.moving = 1;
            }
        }
        if next == 0 {
            break;
        }
        assoc = next;
        if count >= MAX_BLENDED {
            break;
        }
    }
    data.nodes[count] = 0;
    let pointer = core::ptr::addr_of_mut!(data) as u64;

    if !second_path {
        call(c, FOR_ALL_NODES, &[clump, got(GOT_CALLBACK_FIRST), pointer]);
        let bones = r64(clump + 0x20);
        w8(bones, r8(bones) | 0x20);
    } else {
        let bones = r64(clump + 0x20);
        let first = r64(pointer + 8);
        if r8(bones) & 0x20 != 0 && first != 0 {
            let n = ri32(clump + 0x10);
            let mut slot = pointer + 8;
            while r64(slot) != 0 {
                if n >= 1 {
                    let owner = r64(r64(slot) + 0x28);
                    for k in 0..n as u64 {
                        if r8(bones + k * 0x20) & 8 != 0 && r64(r64(CURRENT_CLUMP) + 0x18) != 0 {
                            continue;
                        }
                        let node = r64(slot) + k * NODE_SIZE;
                        if r64(node + 0x20) != 0 {
                            let time = rf32(owner + 0x3c) - rf32(owner + 0x14);
                            crate::guest::call_f(c, FIND_KEY_FRAME, &[node], &[time]);
                        }
                    }
                }
                slot += 8;
            }
        }
        let skinned = call(c, IS_SKINNED, &[group]).0 & 1 != 0;
        let f = got(if skinned { GOT_CALLBACK_SKINNED } else { GOT_CALLBACK_PLAIN });
        call(c, FOR_ALL_NODES, &[clump, f, pointer]);
        let bones = r64(clump + 0x20);
        w8(bones, r8(bones) & !0x20);
    }

    if step > 0.0 {
        let mut assoc = r64(clump);
        if assoc != 0 {
            let rate = blend_sum / length_sum;
            while assoc != 0 {
                let sync = if length_sum == 0.0 { None } else { Some(rate) };
                time_step(c, assoc, step, sync);
                assoc = r64(assoc);
            }
        }
    }
    call(c, UPDATE_FRAMES, &[r64(group + 8)]);
}

/// Shadow verification runs the original and the port on the same records, so it only fits updates that call nothing
/// that changes memory twice over: no bones, the first path, no deletes or callbacks (rt/porttest.c builds such
/// records). Returns the region holding the associations.
unsafe fn verifiable(group: u64, second_path: bool) -> Option<(u64, usize)> {
    if !verify::enabled() || second_path {
        return None;
    }
    let clump = r64(group + 0x30);
    if ri32(clump + 0x10) != 0 {
        return None;
    }
    let (mut a, mut low, mut high, mut n) = (r64(clump), u64::MAX, 0u64, 0);
    while a != 0 && n < 16 {
        if r8(a + 0x10) & 4 != 0 || r32(a + 0x48) != 0 {
            return None;
        }
        low = low.min(a);
        high = high.max(a + 0x60);
        a = r64(a);
        n += 1;
    }
    if a != 0 || (n > 0 && high - low > 0x800) {
        return None;
    }
    Some(if n == 0 { (0, 0) } else { (low, (high - low) as usize) })
}

/// void RslAnimBlendElementGroupUpdateAnimations(RslElementGroup *, float step, bool second_path)
#[no_mangle]
pub unsafe extern "C" fn G_2e8e20(c: *mut Cpu) {
    let group = (*c).x[0];
    let step = (*c).s(0);
    let second = (*c).x[1] & 1 != 0;
    if let Some(assocs) = verifiable(group, second) {
        let bones = r64(r64(group + 0x30) + 0x20);
        let frame = r64(group + 8);
        let regions = [assocs, (CURRENT_CLUMP, 8), (bones, 1), (frame, 4), (r64(frame + 0xb0), 4)];
        verify::port_regions(c, G_2e8e20_orig, Ret::None, &regions, |c| update_animations(c, group, step, second));
    } else {
        update_animations(&mut *c, group, step, second);
    }
}
