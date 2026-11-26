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
use crate::guest::{fcvtzu, fma, fmax, screen_size, sqrt};
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
    let wide = if new_style { ASPECT } else { 1.0 };
    let (width, height) = (ORIGINAL[1] * scale * wide, ORIGINAL[2] * scale);
    let room = if new_style { BAR_ROOM } else { 0.0 };
    // HUD space is 448 lines tall: position = y * scaleY, size = h * scaleSize
    let y = if settings[0] != 0 || new_style {
        (448.0 - ORIGINAL[0] * sy - height * ss - f32::from_bits(settings[1]) - room) / sy
    } else {
        ORIGINAL[0]
    };
    // the modern rectangle keeps a gap to the left edge
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
        let mut out_y = fma(-cs, dx, sn * dy);
        if modern_radar() {
            out_y *= ASPECT;
        }
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
        let reach = if modern_radar() { fmax(x.abs(), y.abs()) } else { len };
        if frontend_map_flag() == 0 && reach > 1.0 {
            let k = 1.0 / reach;
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
            // health (3) and armour (4) are drawn as bars by the host in the modern style
            let replaced = $bit >= 3 && modern();
            if verify::enabled() || (HIDDEN.load(Ordering::Relaxed) & (1 << $bit) == 0 && !replaced) {
                $orig(c);
            }
        }
    };
}
extern "C" {
    fn G_358c84_orig(c: *mut Cpu);
    fn G_35b4f4_orig(c: *mut Cpu);
    fn G_35c90c_orig(c: *mut Cpu);
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
// SETTINGS > HUD > STYLE = MODERN (lcs.ini hud_style=modern): a rectangular, slightly tilted radar in the bottom-left
// corner in the manner of GTA V, with health and armour as flat bars under it (drawn by rt/present.c). The radar
// widget is widened; radar space is stretched vertically by the same factor so the map keeps its scale and the
// engine's clip to the unit square becomes the rectangle; the circle mask and the ring sprite are not drawn. The
// pause map is not changed.

const ASPECT: f32 = 1.45; // width / height of the modern radar
const TILT: f32 = 0.12; // how much narrower the far edge is
const BAR_ROOM: f32 = 14.0; // HUD lines kept free under the radar for the bars
const LEFT_GAP: f32 = 12.0; // layout units added to the radar x
const RING_CALL: u64 = G + 0x481330; // return address of the radar ring sprite in CHud::Draw
const GOT_PLAYER_INDEX: u64 = G + 0x7f9118;
const GOT_PLAYERS: u64 = G + 0x7f9120;

static MODERN: AtomicBool = AtomicBool::new(false);
static DRAWN: AtomicBool = AtomicBool::new(false);
static CORNERS: [AtomicU32; 4] = [AtomicU32::new(0), AtomicU32::new(0), AtomicU32::new(0), AtomicU32::new(0)];

/// Host setting (lcs.ini `hud_style`).
#[no_mangle]
pub extern "C" fn lcs_set_hud_style(modern: i32) {
    MODERN.store(modern != 0, Ordering::Relaxed);
}

#[inline]
fn modern() -> bool {
    MODERN.load(Ordering::Relaxed) && !verify::enabled()
}

/// Modern style applies to the in-game radar only, not to the pause map.
#[inline]
unsafe fn modern_radar() -> bool {
    if !modern() {
        return false;
    }
    let map = r64(G + 0x7f92c0);
    r8(map + 0x11) == 0 && r8(map + 0x14) == 0
}

/// What the host draws over the radar this frame: out = left x, right x, bottom y (fractions of the screen, y from the
/// top), health 0..1, armour 0..1, parts hidden (HUD bit set). Returns 0 when the modern radar was not drawn.
#[no_mangle]
pub unsafe extern "C" fn lcs_modern_overlay(out: *mut f32) -> i32 {
    if !DRAWN.swap(false, Ordering::Relaxed) || !modern() {
        return 0;
    }
    for i in 0..3 {
        *out.add(i) = f32::from_bits(CORNERS[i].load(Ordering::Relaxed));
    }
    let index = r8(r64(GOT_PLAYER_INDEX)) as u64;
    let info = r64(GOT_PLAYERS) + index * 0x1a0;
    let ped = r64(info);
    if ped == 0 {
        return 0;
    }
    let share = |value: f32, max: u8| if max == 0 { 0.0 } else { (value / max as f32).clamp(0.0, 1.0) };
    *out.add(3) = share(rf32(ped + 0x5a0), r8(info + 0x18f));
    *out.add(4) = share(rf32(ped + 0x5a4), r8(info + 0x190));
    *out.add(5) = HIDDEN.load(Ordering::Relaxed) as f32;
    1
}

/// void CRadar::DrawRadarMask(): the circle's corners. The modern radar keeps them and notes where it is instead.
#[no_mangle]
pub unsafe extern "C" fn G_35c90c(c: *mut Cpu) {
    if !modern_radar() {
        G_35c90c_orig(c);
        return;
    }
    let mut at = [[0.0f32; 2]; 2];
    for (i, x) in [-1.0f32, 1.0].iter().enumerate() {
        let point = [*x, -1.0f32];
        let mut screen = [0.0f32; 2];
        crate::guest::call(c, 0x358c84, &[screen.as_mut_ptr() as u64, point.as_ptr() as u64, 0]);
        at[i] = screen;
    }
    let values = [at[0][0] / 640.0, at[1][0] / 640.0, at[0][1] / 448.0]; // HUD space is 640 x 448
    for (slot, v) in CORNERS.iter().zip(values) {
        slot.store(v.to_bits(), Ordering::Relaxed);
    }
    DRAWN.store(true, Ordering::Relaxed);
}

/// void CRadar::TransformRadarPointToScreenSpace(CVector2D &out, CVector2D const &in, bool): tilted when modern.
#[no_mangle]
pub unsafe extern "C" fn G_358c84(c: *mut Cpu) {
    if !modern_radar() {
        G_358c84_orig(c);
        return;
    }
    let (out, input, flag) = ((*c).x[0], (*c).x[1], (*c).x[2]);
    let y = rf32(input + 4);
    let middle = [0.0f32, y];
    let mut centre = [0.0f32; 2];
    let saved: Cpu = *c;
    (*c).x[0] = centre.as_mut_ptr() as u64;
    (*c).x[1] = middle.as_ptr() as u64;
    G_358c84_orig(c);
    *c = saved;
    (*c).x[2] = flag;
    G_358c84_orig(c);
    let k = 1.0 - TILT * y.clamp(-1.0, 1.0);
    w32f(out, centre[0] + (rf32(out) - centre[0]) * k);
}

/// void CRadar::TransformRadarPointToRealWorldSpace(CVector2D &out, CVector2D const &in): the inverse of the vertical
/// stretch of the modern radar.
#[no_mangle]
pub unsafe extern "C" fn G_35b4f4(c: *mut Cpu) {
    if !modern_radar() {
        G_35b4f4_orig(c);
        return;
    }
    let input = (*c).x[1];
    let point = [rf32(input), rf32(input + 4) / ASPECT];
    (*c).x[1] = point.as_ptr() as u64;
    G_35b4f4_orig(c);
}

/// The sprite draw with four colours (0x4a4408): the radar ring from CHud::Draw is left out when modern.
#[no_mangle]
pub unsafe extern "C" fn G_4a4408(c: *mut Cpu) {
    if modern() && (*c).x[30] == RING_CALL {
        return;
    }
    G_4a4408_orig(c);
}
