//! CHud radar layout getters and CRadar coordinate helpers.
//!
//! The radar is placed by four CHud getters that read the radar widget of the HUD layout object:
//!   layout = *(*GOT 0x7f9210); widget = *(layout + 0xe0)
//!   widget + 0x8/0xc/0x10/0x14 = x, y, w, h (layout units); layout + 0x4bc/0x4c0/0x4c4 = scale x, y, size
//! CRadar::TransformRadarPointToScreenSpace and the radar drawing ask these getters, so the radar,
//! its blips and its mask follow whatever they return.
//!
//! New behaviour (off while verifying): `lcs_radar_layout` moves the radar widget to the bottom-left
//! corner and resizes it.

use crate::cpu::Cpu;
use crate::guest::{call, call_f, fcvtzu, fma, screen_size, sqrt};
use crate::mem::{r32, r64, r8, rf32, rf64, w32f, G};
use crate::verify::{self, Ret};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

const GOT_HUD_LAYOUT: u64 = G + 0x7f9210;
const GOT_TV_DEVICE: u64 = G + 0x7fe458;
const RADAR_WIDGET: u64 = 0xe0;
const SCALE_X: u64 = 0x4bc;
const SCALE_Y: u64 = 0x4c0;
const SCALE_SIZE: u64 = 0x4c4;
const WIDE: f32 = f32::from_bits(0x3fb6_db6e); // 1.4285715 = 1 / 0.7

static RADAR_BOTTOM: AtomicBool = AtomicBool::new(false);
static RADAR_MARGIN: AtomicU32 = AtomicU32::new(0); // HUD lines (screen height = 448)
static RADAR_SCALE: AtomicU32 = AtomicU32::new(0x3f80_0000); // 1.0

/// Host settings: bottom-left radar, its distance from the bottom edge, and its size factor.
#[no_mangle]
pub extern "C" fn lcs_set_radar(bottom: i32, margin: f32, scale: f32) {
    RADAR_BOTTOM.store(bottom != 0, Ordering::Relaxed);
    RADAR_MARGIN.store(margin.to_bits(), Ordering::Relaxed);
    let s = if scale.is_finite() && scale > 0.25 && scale < 4.0 { scale } else { 1.0 };
    RADAR_SCALE.store(s.to_bits(), Ordering::Relaxed);
}

#[inline]
unsafe fn layout() -> u64 {
    let g = r64(GOT_HUD_LAYOUT);
    if g == 0 { 0 } else { r64(g) }
}
#[inline]
unsafe fn widget(l: u64) -> u64 {
    r64(l + RADAR_WIDGET)
}
#[inline]
unsafe fn tv_device() -> bool {
    r32(r64(GOT_TV_DEVICE)) == 1
}

/// Radar placement, once per frame from the host. The radar widget itself is moved and resized, so
/// everything that reads it follows: the getters below, the blips and map, and the ring sprite the
/// layout draws straight from the widget. When the engine rebuilds its layout the original values
/// come back; they are recorded again and the placement is reapplied. Skipped while verifying.
#[no_mangle]
pub unsafe extern "C" fn lcs_radar_layout() {
    static mut WRITTEN: [f32; 3] = [f32::NAN; 3]; // y, w, h as last written
    static mut ORIGINAL: [f32; 3] = [0.0; 3];
    static mut APPLIED: [u32; 4] = [u32::MAX; 4]; // settings the written values were made for
    if verify::enabled() {
        return;
    }
    let l = layout();
    if l == 0 || widget(l) == 0 {
        return;
    }
    let w = widget(l);
    let (sy, ss) = (rf32(l + SCALE_Y), rf32(l + SCALE_SIZE));
    if !(sy > 0.0) || !(ss > 0.0) {
        return;
    }
    let now = [rf32(w + 0xc), rf32(w + 0x10), rf32(w + 0x14)];
    let settings = [RADAR_BOTTOM.load(Ordering::Relaxed) as u32, RADAR_MARGIN.load(Ordering::Relaxed), RADAR_SCALE.load(Ordering::Relaxed), MODERN.load(Ordering::Relaxed) as u32];
    let ours = now[0].to_bits() == WRITTEN[0].to_bits() && now[1].to_bits() == WRITTEN[1].to_bits() && now[2].to_bits() == WRITTEN[2].to_bits();
    if ours && settings == APPLIED {
        return;
    }
    if !ours {
        ORIGINAL = now; // the engine (re)built its layout
    }
    let scale = f32::from_bits(settings[2]);
    let new_style = settings[3] != 0;
    let (width, height) = (ORIGINAL[1] * scale, ORIGINAL[2] * scale);
    // HUD space is 448 lines tall: position = y * scaleY, size = h * scaleSize
    let y = if settings[0] != 0 || new_style {
        (448.0 - ORIGINAL[0] * sy - height * ss - f32::from_bits(settings[1])) / sy
    } else {
        ORIGINAL[0]
    };
    // the modern style keeps room for its ring at the left edge
    static mut X: [f32; 2] = [f32::NAN, 0.0]; // as last written, original
    let x_now = rf32(w + 8);
    if x_now.to_bits() != X[0].to_bits() {
        X[1] = x_now;
    }
    X[0] = X[1] + if new_style { LEFT_GAP } else { 0.0 };
    w32f(w + 8, X[0]);
    w32f(w + 0xc, y);
    w32f(w + 0x10, width);
    w32f(w + 0x14, height);
    WRITTEN = [y, width, height];
    APPLIED = settings;
}

/// float CHud::GetRadarPosX()
#[no_mangle]
pub unsafe extern "C" fn G_48411c(c: *mut Cpu) {
    verify::port(c, G_48411c_orig, Ret::S0, |c| {
        let l = layout();
        let x = rf32(widget(l) + 8) * rf32(l + SCALE_X);
        // TV devices: 19.2 more, added in double precision
        let x = if tv_device() { (x as f64 + f64::from_bits(0x4033_3333_3333_3333)) as f32 } else { x };
        c.set_s(0, x);
    })
}

/// float CHud::GetRadarPosY()
#[no_mangle]
pub unsafe extern "C" fn G_48416c(c: *mut Cpu) {
    verify::port(c, G_48416c_orig, Ret::S0, |c| {
        let l = layout();
        let w = widget(l);
        let y = if tv_device() {
            ((rf32(w + 8) * rf32(l + SCALE_X)) as f64 + rf64(G + 0x2330f8)) as f32
        } else {
            rf32(w + 0xc) * rf32(l + SCALE_Y)
        };
        c.set_s(0, y);
    })
}

/// float CHud::GetRadarSizeX(): widened by 1/0.7 and corrected for the screen aspect ratio.
#[no_mangle]
pub unsafe extern "C" fn G_4841c8(c: *mut Cpu) {
    verify::port(c, G_4841c8_orig, Ret::S0, |c| {
        let l = layout();
        let s = rf32(widget(l) + 0x10) * rf32(l + SCALE_SIZE) * WIDE;
        let (sw, sh) = screen_size(c);
        c.set_s(0, s / (sw as f32 / sh as f32));
    })
}

/// float CHud::GetRadarSizeY()
#[no_mangle]
pub unsafe extern "C" fn G_484224(c: *mut Cpu) {
    verify::port(c, G_484224_orig, Ret::S0, |c| {
        let l = layout();
        c.set_s(0, rf32(widget(l) + 0x14) * rf32(l + SCALE_SIZE));
    })
}

/// float CRadar::AdjustForWideScreen(float)
#[no_mangle]
pub unsafe extern "C" fn G_35a400(c: *mut Cpu) {
    verify::port(c, G_35a400_orig, Ret::S0, |c| {
        let v = c.s(0) * WIDE;
        let (sw, sh) = screen_size(c);
        c.set_s(0, v / (sw as f32 / sh as f32));
    })
}

/// void CRadar::TransformRealWorldPointToRadarSpace(CVector2D &out, CVector2D const &in)
/// Rotates and scales a world point around the radar origin (cached sin/cos, radar range).
#[no_mangle]
pub unsafe extern "C" fn G_358b34(c: *mut Cpu) {
    verify::port_mem(c, G_358b34_orig, Ret::None, 0, 8, |c| {
        let inv = 1.0 / rf32(r64(G + 0x7f9f50));
        let (px, py) = (rf32(c.x[1]), rf32(c.x[1] + 4));
        let dy = (py - rf32(G + 0x8577ac)) * inv;
        let dx = (px - rf32(G + 0x8577a8)) * inv;
        let (cs, sn) = (rf32(G + 0x857900), rf32(G + 0x857904));
        let out_x = fma(sn, dx, cs * dy);
        let out_y = fma(-cs, dx, sn * dy);
        w32f(c.x[0], out_x);
        w32f(c.x[0] + 4, out_y);
    })
}

#[inline]
unsafe fn frontend_map_flag() -> u8 {
    r8(r64(G + 0x7f92c0) + 0x11)
}

/// float CRadar::LimitRadarPoint(CVector2D &p): clamps the point to the radar circle (not on the
/// frontend map) and returns its original length.
#[no_mangle]
pub unsafe extern "C" fn G_358b98(c: *mut Cpu) {
    verify::port_mem(c, G_358b98_orig, Ret::S0, 0, 8, |c| {
        let p = c.x[0];
        let (x, y) = (rf32(p), rf32(p + 4));
        let len = sqrt(fma(x, x, y * y));
        if frontend_map_flag() == 0 && len > 1.0 {
            let k = 1.0 / len;
            w32f(p, x * k);
            w32f(p + 4, y * k);
        }
        c.set_s(0, len);
    })
}

/// int CRadar::CalculateBlipAlpha(float distance): fades blips between 1 and 10 radar units.
#[no_mangle]
pub unsafe extern "C" fn G_358bd8(c: *mut Cpu) {
    verify::port(c, G_358bd8_orig, Ret::W0, |c| {
        let d = c.s(0);
        let alpha = if !(d > 1.0) || frontend_map_flag() != 0 {
            0xff
        } else if d > 10.0 {
            0x80
        } else {
            let t = (d - 1.0) / 9.0;
            let a = fcvtzu(t * 128.0).min(0xff); // fcvtzu #7: fixed point with 7 fraction bits
            let b = fcvtzu((1.0 - t) * 255.0).min(0xff);
            let sum = (a + b) & 0xff;
            let bright = r8(r64(G + 0x7f9f58)) as f32 / 255.0;
            fcvtzu(bright * sum as f32).min(0xff)
        };
        c.set_w(0, alpha);
    })
}

extern "C" {
    fn G_48411c_orig(c: *mut Cpu);
    fn G_48416c_orig(c: *mut Cpu);
    fn G_4841c8_orig(c: *mut Cpu);
    fn G_484224_orig(c: *mut Cpu);
    fn G_35a400_orig(c: *mut Cpu);
    fn G_358b34_orig(c: *mut Cpu);
    fn G_358b98_orig(c: *mut Cpu);
    fn G_358bd8_orig(c: *mut Cpu);
}

// ---- showing and hiding HUD parts ------------------------------------------------------------------

static HIDDEN: AtomicU32 = AtomicU32::new(0); // bit 0 whole HUD, 1 clock, 2 money, 3 health, 4 armour

/// Host setting (lcs.ini `hud*`, SETTINGS > HUD).
#[no_mangle]
pub extern "C" fn lcs_set_hud(hidden: i32) {
    HIDDEN.store(hidden as u32, Ordering::Relaxed);
}

/// The recompiled draw function, skipped when its part is hidden (never while verifying).
macro_rules! hud_part {
    ($name:ident, $orig:ident, $bit:expr) => {
        #[no_mangle]
        pub unsafe extern "C" fn $name(c: *mut Cpu) {
            // health (3) and armour (4) are the ring around the radar in the modern style
            let replaced = $bit >= 3 && modern();
            if verify::enabled() || (HIDDEN.load(Ordering::Relaxed) & (1 << $bit) == 0 && !replaced) {
                $orig(c);
            }
        }
    };
}
extern "C" {
    fn G_483a98_orig(c: *mut Cpu);
    fn G_4a3c40_orig(c: *mut Cpu);
    fn G_4a4408_orig(c: *mut Cpu);
    fn G_47db64_orig(c: *mut Cpu);
    fn G_483db0_orig(c: *mut Cpu);
    fn G_482950_orig(c: *mut Cpu);
    fn G_482b48_orig(c: *mut Cpu);
    fn G_483364_orig(c: *mut Cpu);
}
hud_part!(G_47db64, G_47db64_orig, 0); // CHud::Draw
hud_part!(G_483db0, G_483db0_orig, 1); // CHud::DrawTime
hud_part!(G_482950, G_482950_orig, 2); // CHud::DrawCash
hud_part!(G_482b48, G_482b48_orig, 3); // CHud::DrawHealthBar
hud_part!(G_483364, G_483364_orig, 4); // CHud::DrawArmourBar

// ---- modern HUD style ------------------------------------------------------------------------------
//
// SETTINGS > HUD > STYLE = MODERN (lcs.ini hud_style=modern), in the manner of GTA IV: the round radar in the
// bottom-left corner inside a ring that shows health (left half, green) and armour (right half, blue), both filling
// from the bottom; the weapon picture without its frame; the clock directly above the money. The ring is drawn with
// the engine's own 2D polygons where the radar ring sprite was, so the map blips stay on top of it.

const LEFT_GAP: f32 = 12.0; // layout units added to the radar x
const RING_CALL: u64 = G + 0x481330; // return address of the radar ring sprite in CHud::Draw
const WEAPON_CALLS: [u64; 2] = [G + 0x47edcc, G + 0x47ee10]; // of the two weapon picture draws
const CLOCK_CALL: u64 = G + 0x483ff8; // of DrawTimeAndCashNumbers in CHud::DrawTime
const GOT_PLAYER_INDEX: u64 = G + 0x7f9118;
const GOT_PLAYERS: u64 = G + 0x7f9120;
const GET_RADAR_SIZE_X: u64 = 0x4841c8;
const GET_RADAR_SIZE_Y: u64 = 0x484224;
const SPRITE_DRAW_UV: u64 = 0x4a3fa4; // CSprite2d::Draw(rect, colour, 4 texture corners)
const DRAW_2D_POLYGON: u64 = 0x4a4c18; // CSprite2d::Draw2DPolygon(4 points, colour)
// part of the weapon picture inside its baked frame (left, right, top, bottom as texture coordinates)
const ICON: [f32; 4] = [0.05, 0.935, 0.095, 0.665];
// how far the clock moves down, in weapon picture heights
const CLOCK_DROP: f32 = 0.66;

static MODERN: AtomicBool = AtomicBool::new(false);
static ICON_HEIGHT: AtomicU32 = AtomicU32::new(0);

/// Host setting (lcs.ini `hud_style`).
#[no_mangle]
pub extern "C" fn lcs_set_hud_style(modern: i32) {
    MODERN.store(modern != 0, Ordering::Relaxed);
}

#[inline]
fn modern() -> bool {
    MODERN.load(Ordering::Relaxed) && !verify::enabled()
}

/// Health and armour of the player in focus as 0..1.
unsafe fn player_shares() -> Option<(f32, f32)> {
    let index = r8(r64(GOT_PLAYER_INDEX)) as u64;
    let info = r64(GOT_PLAYERS) + index * 0x1a0;
    let ped = r64(info);
    if ped == 0 {
        return None;
    }
    let share = |value: f32, max: u8| if max == 0 { 0.0 } else { (value / max as f32).clamp(0.0, 1.0) };
    Some((share(rf32(ped + 0x5a0), r8(info + 0x18f)), share(rf32(ped + 0x5a4), r8(info + 0x190))))
}

/// sin and cos of `turn` half turns (0..=1), close enough for drawing (no_std has no libm here).
fn half_turn(turn: f32) -> (f32, f32) {
    // Bhaskara's approximation of sin(pi x) on 0..=1, error below 0.002
    let sine = |x: f32| {
        let p = x * (1.0 - x);
        16.0 * p / (5.0 - 4.0 * p)
    };
    let cos = if turn <= 0.5 { sine(0.5 - turn) } else { -sine(turn - 0.5) };
    (sine(turn), cos)
}

/// A piece of ring: from the bottom of the radar upward, on the left (side -1) or right (side 1), up to `share` of
/// the half circle. `inner` and `outer` are radii in radar radii.
unsafe fn arc(c: *mut Cpu, at: [f32; 4], side: f32, share: f32, inner: f32, outer: f32, colour: [u8; 4]) {
    const STEPS: u32 = 36;
    const GAP: f32 = 0.012; // half turns left open at the bottom and the top
    let [cx, cy, rx, ry] = at;
    let span = (1.0 - 2.0 * GAP) * share;
    if !(span > 0.0) {
        return;
    }
    let point = |turn: f32, r: f32| {
        let (s, co) = half_turn(turn);
        [cx + side * s * rx * r, cy + co * ry * r]
    };
    for i in 0..STEPS {
        let a = GAP + span * i as f32 / STEPS as f32;
        let b = GAP + span * (i + 1) as f32 / STEPS as f32;
        let (p3, p4, p2, p1) = (point(a, inner), point(a, outer), point(b, outer), point(b, inner));
        call_f(c, DRAW_2D_POLYGON, &[colour.as_ptr() as u64], &[p1[0], p1[1], p2[0], p2[1], p3[0], p3[1], p4[0], p4[1]]);
    }
}

/// The ring around the radar; `rect` is where the original ring sprite would go (same centre as the radar).
unsafe fn draw_ring(c: *mut Cpu, rect: u64) {
    let (left, bottom, right, top) = (rf32(rect), rf32(rect + 4), rf32(rect + 8), rf32(rect + 0xc));
    let rx = call(c, GET_RADAR_SIZE_X, &[]).1 * 0.5;
    let ry = call(c, GET_RADAR_SIZE_Y, &[]).1 * 0.5;
    let at = [(left + right) * 0.5, (top + bottom) * 0.5, rx, ry];
    let hidden = HIDDEN.load(Ordering::Relaxed);
    let (health, armour) = player_shares().unwrap_or((0.0, 0.0));
    for (side, bit, share, full, empty) in [
        (-1.0f32, 8, health, [104u8, 170, 80, 255], [38u8, 62, 32, 200]),
        (1.0, 16, armour, [84, 156, 216, 255], [30, 52, 74, 200]),
    ] {
        arc(c, at, side, 1.0, 0.98, 1.17, [12, 12, 14, 210]); // backing
        if hidden & bit == 0 {
            arc(c, at, side, 1.0, 1.02, 1.13, empty);
            arc(c, at, side, share, 1.02, 1.13, full);
        }
    }
}

/// The sprite draw with four colours (0x4a4408): when modern, the health and armour ring takes the place of the radar
/// ring sprite of CHud::Draw.
#[no_mangle]
pub unsafe extern "C" fn G_4a4408(c: *mut Cpu) {
    if modern() && (*c).x[30] == RING_CALL {
        draw_ring(c, (*c).x[1]);
        return;
    }
    G_4a4408_orig(c);
}

/// void CSprite2d::Draw(CRect const &, CRGBA const &): when modern, the weapon picture is cut to the inside of the
/// frame that is part of its texture.
#[no_mangle]
pub unsafe extern "C" fn G_4a3c40(c: *mut Cpu) {
    if !(modern() && WEAPON_CALLS.contains(&(*c).x[30])) {
        G_4a3c40_orig(c);
        return;
    }
    let (sprite, rect, colour) = ((*c).x[0], (*c).x[1], (*c).x[2]);
    let (left, bottom, right, top) = (rf32(rect), rf32(rect + 4), rf32(rect + 8), rf32(rect + 0xc));
    let [u0, u1, v0, v1] = ICON;
    let (w, h) = (right - left, bottom - top);
    ICON_HEIGHT.store(h.to_bits(), Ordering::Relaxed);
    // CRect in memory: left, bottom, right, top
    let cut = [left + u0 * w, top + v1 * h, left + u1 * w, top + v0 * h];
    call_f(c, SPRITE_DRAW_UV, &[sprite, cut.as_ptr() as u64, colour], &[u0, v0, u1, v0, u0, v1, u1, v1]);
}

/// void CHud::DrawTimeAndCashNumbers(char *, float x, float y, short, bool): the clock sits lower when modern, in the
/// row the health bar used, directly above the money.
#[no_mangle]
pub unsafe extern "C" fn G_483a98(c: *mut Cpu) {
    if modern() && (*c).x[30] == CLOCK_CALL {
        let y = (*c).s(1) + CLOCK_DROP * f32::from_bits(ICON_HEIGHT.load(Ordering::Relaxed));
        (*c).set_s(1, y);
    }
    G_483a98_orig(c);
}
