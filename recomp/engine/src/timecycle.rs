//! CTimeCycle: the time-of-day and weather colour cycle (ambient, directional, sky, sun, sprites, fog,
//! water, blur, ...). Rewritten from libGame.so 2.4.379; every function keeps the original ABI and
//! matches the original bit for bit (checked with `LCS_RS_VERIFY=1`).
//!
//! Layout of the CTimeCycle object (`this`):
//!   +0x000  16 sun direction vectors (x, y, z, w), one per frame in a ring indexed by +0x2bd8
//!   +0x100  six arrays of 16 floats: CShadows::CalcPedShadowValues outputs for each ring slot
//!   +0x280  52 packed tables [24 hours][8 weathers] of u8, i8 or i16 (see [`FIELDS`]), filled from
//!           the float tables timecyc.dat is parsed into (G+0x868154, 0x300 bytes each)
//!   +0x2b00 the current values, one per table ([`FIELDS`]), plus the derived values below
//!   +0x2b54 three averaged ambient values, +0x2bd8 ring slot, +0x2bdc fog reduction
//!   +0x2be0 extra colour index, +0x2be4 extra colour on, +0x2be8 extra colour strength
//! Weather 6 of a packed table holds the "extra colours" (interiors, cutscenes), indexed by hour.

use crate::cpu::Cpu;
use crate::guest::{call, call_f, fma, fmax, fmin, sqrt};
use crate::mem::*;
use crate::verify::{self, Ret};

/// Element type of a packed table.
#[derive(Clone, Copy, PartialEq)]
enum Kind {
    U8,
    I8,
    I16,
}

/// How the interpolated value is stored.
#[derive(Clone, Copy, PartialEq)]
enum Out {
    Int,     // int32, truncated
    Float,   // float
    Tenth,   // float, table value / 10
    Short,   // int16, truncated and saturated
}

/// How the extra colour blends into the value.
#[derive(Clone, Copy, PartialEq)]
enum Extra {
    /// Only when the extra colour has a non-zero ambient (tables 0xdc0/0xe80/0xf40).
    IfAmbient,
    /// Always, then scaled by the float at `*GOT[slot] + 0x4c`.
    Scaled(u64),
    /// Always, then scaled by `*GOT[slot] + 0x4c` and by the global colour scale (`*GOT[0x7fa710] + 0x4c`).
    Colour(u64),
}

struct Field {
    table: u64,
    kind: Kind,
    out: u64,
    store: Out,
    extra: Extra,
}

const fn f(table: u64, kind: Kind, out: u64, store: Out, extra: Extra) -> Field {
    Field { table, kind, out, store, extra }
}

use Extra::*;
use Kind::*;
use Out::*;

/// The 52 cycled values, in table order.
static FIELDS: [Field; 52] = [
    // 0x2b00..0x2b38: 15 colour channels kept as 0..1 floats (ambient, directional, sky, sun ...)
    f(0x280, U8, 0x2b00, Float, Colour(0x7fa708)),
    f(0x340, U8, 0x2b04, Float, Colour(0x7fa708)),
    f(0x400, U8, 0x2b08, Float, Colour(0x7fa708)),
    f(0x4c0, U8, 0x2b0c, Float, Colour(0x7fa718)),
    f(0x580, U8, 0x2b10, Float, Colour(0x7fa718)),
    f(0x640, U8, 0x2b14, Float, Colour(0x7fa718)),
    f(0x700, U8, 0x2b18, Float, Colour(0x7fa720)),
    f(0x7c0, U8, 0x2b1c, Float, Colour(0x7fa720)),
    f(0x880, U8, 0x2b20, Float, Colour(0x7fa720)),
    f(0x940, U8, 0x2b24, Float, Colour(0x7fa728)),
    f(0xa00, U8, 0x2b28, Float, Colour(0x7fa728)),
    f(0xac0, U8, 0x2b2c, Float, Colour(0x7fa728)),
    f(0xb80, U8, 0x2b30, Float, Colour(0x7fa730)),
    f(0xc40, U8, 0x2b34, Float, Colour(0x7fa730)),
    f(0xd00, U8, 0x2b38, Float, Colour(0x7fa730)),
    // 0x2b3c..0x2b50: sky top / bottom colours (int)
    f(0xdc0, U8, 0x2b3c, Int, IfAmbient),
    f(0xe80, U8, 0x2b40, Int, IfAmbient),
    f(0xf40, U8, 0x2b44, Int, IfAmbient),
    f(0x1000, U8, 0x2b48, Int, IfAmbient),
    f(0x10c0, U8, 0x2b4c, Int, IfAmbient),
    f(0x1180, U8, 0x2b50, Int, IfAmbient),
    // 0x2b60..0x2b74: sun core and corona colours (int)
    f(0x1240, U8, 0x2b60, Int, IfAmbient),
    f(0x1300, U8, 0x2b64, Int, IfAmbient),
    f(0x13c0, U8, 0x2b68, Int, IfAmbient),
    f(0x1480, U8, 0x2b6c, Int, IfAmbient),
    f(0x1540, U8, 0x2b70, Int, IfAmbient),
    f(0x1600, U8, 0x2b74, Int, IfAmbient),
    // sun size, sprite size, sprite brightness (tenths)
    f(0x16c0, I8, 0x2b78, Tenth, IfAmbient),
    f(0x1780, I8, 0x2b7c, Tenth, Scaled(0x7fa738)),
    f(0x1840, I8, 0x2b80, Tenth, Scaled(0x7fa748)),
    // shadow strength, light shadow strength, pole shadow strength (int16)
    f(0x1900, U8, 0x2b84, Short, Scaled(0x7fa740)),
    f(0x19c0, U8, 0x2b86, Short, Scaled(0x7fa700)),
    f(0x1a80, U8, 0x2b88, Short, Scaled(0x7fa750)),
    // far clip, fog start
    f(0x1b40, I16, 0x2b8c, Float, Scaled(0x7fa758)),
    f(0x1cc0, I16, 0x2b90, Float, Scaled(0x7fa770)),
    // lights on ground brightness (tenths)
    f(0x1e40, U8, 0x2b94, Tenth, Scaled(0x7fa760)),
    // low clouds, fluffy clouds top / bottom, water colour (int)
    f(0x1f00, U8, 0x2b98, Int, IfAmbient),
    f(0x1fc0, U8, 0x2b9c, Int, IfAmbient),
    f(0x2080, U8, 0x2ba0, Int, IfAmbient),
    f(0x2140, U8, 0x2ba4, Int, IfAmbient),
    f(0x2200, U8, 0x2ba8, Int, IfAmbient),
    f(0x22c0, U8, 0x2bac, Int, IfAmbient),
    f(0x2380, U8, 0x2bb0, Int, IfAmbient),
    f(0x2440, U8, 0x2bb4, Int, IfAmbient),
    f(0x2500, U8, 0x2bb8, Int, IfAmbient),
    // motion blur colour (float, also given to TheCamera)
    f(0x25c0, U8, 0x2bbc, Float, Scaled(0x7fa768)),
    f(0x2680, U8, 0x2bc0, Float, Scaled(0x7fa768)),
    f(0x2740, U8, 0x2bc4, Float, Scaled(0x7fa768)),
    // water RGBA
    f(0x2800, U8, 0x2bc8, Float, IfAmbient),
    f(0x28c0, U8, 0x2bcc, Float, IfAmbient),
    f(0x2980, U8, 0x2bd0, Float, IfAmbient),
    f(0x2a40, U8, 0x2bd4, Float, IfAmbient),
];

/// Float source tables (timecyc.dat values, -1 = not given): one per field, 24 x 8 floats each.
const SOURCE: u64 = G + 0x868154;

// globals (GOT slots)
const GOT_SECONDS: u64 = 0x7f9790; // u8 game clock seconds
const GOT_MINUTES: u64 = 0x7f9788; // u8 game clock minutes
const GOT_HOURS: u64 = 0x7f9770; // u8 game clock hours
const GOT_OLD_WEATHER: u64 = 0x7fa6f0; // i16 CWeather::OldWeatherType
const GOT_NEW_WEATHER: u64 = 0x7fa6e8; // i16 CWeather::NewWeatherType
const GOT_WEATHER_BLEND: u64 = 0x7fa6f8; // float CWeather::InterpolationValue
const GOT_TIMESTEP: u64 = 0x7f91e8; // float CTimer::ms_fTimeStep
const GOT_UPDATE_FRAME: u64 = 0x7f96c0; // u8 frame counter
const GOT_THE_CAMERA: u64 = 0x7f9190; // TheCamera (CCamera)
const GOT_SUN_VALID: u64 = 0x7fa778; // u8
const GOT_SUN_DIR: u64 = 0x7fa780; // CVector4 sun direction for the renderer
const GOT_SCRIPTS_FORCE_RAIN: u64 = 0x7fa788; // u8 CWeather::bScriptsForceRain
const GOT_CUTSCENE_RUNNING: u64 = 0x7f9010; // u8 CCutsceneMgr::ms_running
const GOT_COLOUR_SCALE: u64 = 0x7fa710;
const GOT_EXTRA_RESET: u64 = 0x7fa790;

/// The four interpolation weights, also kept as globals for CTimeCycle::Interpolate.
const WEIGHTS: u64 = G + 0x871d60; // [old hour old weather, next hour old weather, old hour new weather, next hour new weather]

// called functions (PLT entries, so hooks apply)
const MINUTES_SINCE_MIDNIGHT: u64 = 0x7cca00; // CClock::GetMinutesSinceMidnight
const COSF: u64 = 0x7c92c0;
const SINF: u64 = 0x7c92a0;
const SINCOSF: u64 = 0x7c9270;
const CALC_PED_SHADOW_VALUES: u64 = 0x7d01a0; // CShadows::CalcPedShadowValues(CVector, float*, ...)
const PLAYER_NO_RAIN: u64 = 0x7ca7f0; // CCullZones::PlayerNoRain
const CAM_NO_RAIN: u64 = 0x7ca7e0; // CCullZones::CamNoRain
const FORCE_UPDATE: u64 = 0x7d0160;
const FILL_GAPS_U8: u64 = 0x7d0170;
const FILL_GAPS_I8: u64 = 0x7d0180;
const FILL_GAPS_I16: u64 = 0x7d0190;

impl Kind {
    const fn size(self) -> u64 {
        match self {
            I16 => 2,
            _ => 1,
        }
    }
}

/// Element of a packed table as a float.
#[inline]
unsafe fn elem(kind: Kind, a: u64) -> f32 {
    match kind {
        U8 => r8(a) as f32,
        I8 => ri8(a) as f32,
        I16 => ri16(a) as f32,
    }
}

/// fcvtzs to 32 bits (truncate, saturate, NaN -> 0).
#[inline]
fn to_int(v: f32) -> i32 {
    v as i32
}

#[inline]
fn to_short(v: f32) -> u16 {
    (v as i32).clamp(-0x8000, 0x7fff) as u16
}

#[derive(Clone, Copy)]
struct Weights {
    cc: f32, // current hour, old weather
    nc: f32, // next hour, old weather
    cn: f32, // current hour, new weather
    nn: f32, // next hour, new weather
}

/// Value of a packed table between the two hours and the two weathers.
#[inline]
unsafe fn blend(w: Weights, at: impl Fn(u64, i64) -> f32, hc: u64, hn: u64, ow: i64, nw: i64) -> f32 {
    fma(w.nn, at(hn, nw), fma(w.cn, at(hc, nw), fma(w.cc, at(hc, ow), w.nc * at(hn, ow))))
}

#[inline]
unsafe fn weights_now() -> Weights {
    Weights { cc: rf32(WEIGHTS), nc: rf32(WEIGHTS + 4), cn: rf32(WEIGHTS + 8), nn: rf32(WEIGHTS + 12) }
}

#[inline]
unsafe fn weathers() -> (i64, i64) {
    (ri16(got(GOT_OLD_WEATHER)) as i64, ri16(got(GOT_NEW_WEATHER)) as i64)
}

/// void CTimeCycle::ForceUpdate()
#[no_mangle]
pub unsafe extern "C" fn G_36d570(c: *mut Cpu) {
    let this = (*c).x[0];
    let cam = got(GOT_THE_CAMERA);
    let regions = [
        (this, 0x280),
        (this + 0x2b00, 0xf0),
        (WEIGHTS, 16),
        (got(GOT_SUN_VALID), 1),
        (got(GOT_SUN_DIR), 16),
        (if cam != 0 { cam + 0x1acc } else { 0 }, 12),
    ];
    verify::port_regions(c, G_36d570_orig, Ret::None, &regions, |c| force_update(c, this));
}

static FAR_SCALE: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0x3f80_0000); // 1.0
static FAR_SEEN: [core::sync::atomic::AtomicU32; 2] = [core::sync::atomic::AtomicU32::new(0), core::sync::atomic::AtomicU32::new(0)];

/// Host setting (lcs.ini `draw_distance`): factor on the time cycle's far clip and fog start.
#[no_mangle]
pub extern "C" fn lcs_set_far_clip_scale(scale: f32) {
    let s = if scale.is_finite() && scale >= 1.0 && scale <= 3.0 { scale } else { 1.0 };
    FAR_SCALE.store(s.to_bits(), core::sync::atomic::Ordering::Relaxed);
}

/// Last far clip and fog start before scaling (diagnostics).
#[no_mangle]
pub extern "C" fn lcs_far_clip_seen(out: *mut f32) {
    for i in 0..2 {
        unsafe { *out.add(i) = f32::from_bits(FAR_SEEN[i].load(core::sync::atomic::Ordering::Relaxed)) };
    }
}

/// New behaviour (off while verifying): the weather's far clip (+0x2b8c) and fog start (+0x2b90) are multiplied, so
/// the world is drawn and fogged farther out.
unsafe fn scale_far_clip(this: u64) {
    if verify::enabled() {
        return;
    }
    let k = f32::from_bits(FAR_SCALE.load(core::sync::atomic::Ordering::Relaxed));
    for (i, off) in [0x2b8cu64, 0x2b90].iter().enumerate() {
        let v = rf32(this + off);
        FAR_SEEN[i].store(v.to_bits(), core::sync::atomic::Ordering::Relaxed);
        w32f(this + off, v * k);
    }
}

static LOOK: [core::sync::atomic::AtomicU32; 3] = [
    core::sync::atomic::AtomicU32::new(0x3f80_0000),
    core::sync::atomic::AtomicU32::new(0x3f80_0000),
    core::sync::atomic::AtomicU32::new(0x3f80_0000),
];

/// Host colour preset (lcs.ini `look`): share of the colour filter tint that is kept (1 = original, 0 = neutral),
/// factor on the fog start distance, factor on the ambient light.
#[no_mangle]
pub extern "C" fn lcs_set_look(tint: f32, fog: f32, ambient: f32) {
    for (slot, v) in LOOK.iter().zip([tint, fog, ambient]) {
        let v = if v.is_finite() && v >= 0.0 && v <= 8.0 { v } else { 1.0 };
        slot.store(v.to_bits(), core::sync::atomic::Ordering::Relaxed);
    }
}

/// New behaviour (off while verifying): the preset applied to this update, before the colour channels are brought to
/// 0..1 and before the camera takes its copy of the colour filter (+0x2bbc..0x2bc4).
unsafe fn apply_look(this: u64) {
    if verify::enabled() {
        return;
    }
    let get = |i: usize| f32::from_bits(LOOK[i].load(core::sync::atomic::Ordering::Relaxed));
    let (tint, fog, ambient) = (get(0), get(1), get(2));
    if tint != 1.0 {
        let c = [rf32(this + 0x2bbc), rf32(this + 0x2bc0), rf32(this + 0x2bc4)];
        let grey = (c[0] + c[1] + c[2]) / 3.0;
        for (i, v) in c.iter().enumerate() {
            w32f(this + 0x2bbc + 4 * i as u64, grey + (v - grey) * tint);
        }
    }
    if fog != 1.0 {
        w32f(this + 0x2b90, rf32(this + 0x2b90) * fog);
    }
    if ambient != 1.0 {
        for i in 0..6u64 {
            let a = this + 0x2b00 + 4 * i;
            w32f(a, rf32(a) * ambient);
        }
    }
}

unsafe fn force_update(c: &mut Cpu, this: u64) {
    // position within the hour, and within the weather change
    let seconds = r8(got(GOT_SECONDS)) as f32;
    let minutes = r8(got(GOT_MINUTES)) as f32;
    let t = (seconds / 60.0 + minutes) / 60.0;
    let wb = rf32(got(GOT_WEATHER_BLEND));
    let hc = r8(got(GOT_HOURS)) as u64;
    let hn = (hc + 1) % 24;
    let (ow, nw) = weathers();
    let w = Weights { nc: (1.0 - wb) * t, nn: wb * t, cc: (1.0 - wb) * (1.0 - t), cn: wb * (1.0 - t) };
    w32f(WEIGHTS + 4, w.nc);
    w32f(WEIGHTS + 12, w.nn);
    w32f(WEIGHTS, w.cc);
    w32f(WEIGHTS + 8, w.cn);

    for fd in FIELDS.iter() {
        let (size, base) = (fd.kind.size(), this + fd.table);
        let at = |h: u64, wt: i64| elem(fd.kind, base.wrapping_add(h * 8 * size).wrapping_add((wt * size as i64) as u64));
        let v = blend(w, at, hc, hn, ow, nw);
        let o = this + fd.out;
        match fd.store {
            Int => w32(o, to_int(v) as u32),
            Float => w32f(o, v),
            Tenth => w32f(o, v / 10.0),
            Short => w16(o, to_short(v)),
        }
    }

    // fog reduction raises the far clip (64 frames -> 650 units)
    let no_rain = ri32(this + 0x2bdc);
    if no_rain != 0 {
        let min = no_rain as f32 * 10.15625;
        let far = rf32(this + 0x2b8c);
        w32f(this + 0x2b8c, if far > min { far } else { min });
    }

    sun_direction(c, this);
    extra_colour(this);
    scale_far_clip(this);
    apply_look(this);

    // motion blur colour of TheCamera (inverted), for the default blur types
    let cam = got(GOT_THE_CAMERA);
    let mode = r32(cam + 0x1adc);
    if mode | 2 == 2 {
        let (r, g, b) = (to_int(rf32(this + 0x2bbc)), to_int(rf32(this + 0x2bc4)), to_int(rf32(this + 0x2bc0)));
        w32(cam + 0x1acc, 255i32.wrapping_sub(r) as u32);
        w32(cam + 0x1ad4, 255i32.wrapping_sub(g) as u32);
        w32(cam + 0x1ad0, 255i32.wrapping_sub(b) as u32);
    }

    // colour channels to 0..1
    for i in 0..15 {
        let a = this + 0x2b00 + 4 * i;
        w32f(a, rf32(a) / 255.0);
    }
    // ambient used by peds and objects: (sky top + 2 * sky bottom) / 3 per channel
    for i in 0..3 {
        let top = ri32(this + 0x2b3c + 4 * i);
        let bottom = ri32(this + 0x2b48 + 4 * i);
        w32(this + 0x2b54 + 4 * i, (top.wrapping_add(bottom.wrapping_mul(2)) / 3) as u32);
    }

    // ped shadows for this frame's sun direction
    let slot = r32(this + 0x2bd8) as u64;
    let sun: [u32; 4] = [r32(this + slot * 16), r32(this + slot * 16 + 4), r32(this + slot * 16 + 8), r32(this + slot * 16 + 12)];
    let s = this + slot * 4;
    call(c, CALC_PED_SHADOW_VALUES, &[sun.as_ptr() as u64, s + 0x100, s + 0x140, s + 0x180, s + 0x1c0, s + 0x200, s + 0x240]);

    // the renderer's copy of the sun direction, z made positive
    let slot = r32(this + 0x2bd8) as u64;
    let p = this + slot * 16;
    let cam_forward_z = rf32(cam + 0x18);
    w8(got(GOT_SUN_VALID), 1);
    let dir = got(GOT_SUN_DIR);
    w64(dir, r64(p));
    w32(dir + 0xc, r32(p + 0xc));
    w32(dir + 8, r32(p + 8) & 0x7fff_ffff);

    // fog reduction (+0x2bdc): counts up to 64 while looking down or where it cannot rain
    let n = ri32(this + 0x2bdc);
    let reduce = if cam_forward_z < f32::from_bits(0xbf66_6666) {
        true
    } else if r8(got(GOT_SCRIPTS_FORCE_RAIN)) != 0 {
        false
    } else {
        call(c, PLAYER_NO_RAIN, &[]).0 & 1 != 0 || call(c, CAM_NO_RAIN, &[]).0 & 1 != 0 || r8(got(GOT_CUTSCENE_RUNNING)) != 0
    };
    let n = if reduce {
        if n.wrapping_add(1) >= 64 { 64 } else { n + 1 }
    } else {
        let m = n.wrapping_sub(1);
        if m < 0 { 0 } else { m }
    };
    w32(this + 0x2bdc, n as u32);
}

/// Sun direction from the time of day, into the next ring slot (+0x2bd8), normalised.
unsafe fn sun_direction(c: &mut Cpu, this: u64) {
    let slot = (r32(this + 0x2bd8).wrapping_add(1)) & 15;
    w32(this + 0x2bd8, slot);
    const PI: f32 = f32::from_bits(0x4049_0fdb);
    let mut m = call(c, MINUTES_SINCE_MIDNIGHT, &[]).1;
    // the morning and the evening are stretched along a quarter circle
    if m >= 360.0 && m < 720.0 {
        let x = (m + -360.0) * 0.25 * PI / 180.0;
        let cos = call_f(c, COSF, &[], &[x]).1;
        m = (1.0 - cos) * 360.0 + 360.0;
    } else if m >= 720.0 && m < 1080.0 {
        let x = (m + -720.0) * 0.25 * PI / 180.0;
        let sin = call_f(c, SINF, &[], &[x]).1;
        m = sin * 360.0 + 720.0;
    }
    let angle = m * f32::from_bits(0x3b8e_fa35); // pi / 720
    let mut sin = 0f32;
    let mut cos = 0f32;
    call_f(c, SINCOSF, &[&mut sin as *mut f32 as u64, &mut cos as *mut f32 as u64], &[angle]);
    let p = this + slot as u64 * 16;
    w32f(p + 4, 1.0);
    w32f(p + 8, f32::from_bits(0x3e4c_cccd) - cos);
    w32f(p, sin);
    w32f(p + 12, 1.0);
    // CVector::Normalise
    let (x, y, z) = (rf32(p), rf32(p + 4), rf32(p + 8));
    let len2 = fma(z, z, fma(x, x, y * y));
    if len2 > 0.0 {
        let k = 1.0 / sqrt(len2);
        w32f(p, x * k);
        w32f(p + 4, y * k);
        w32f(p + 8, z * k);
    } else {
        w32f(p, 1.0);
    }
}

/// Fade the extra colour (+0x2be0 index, +0x2be4 on) in or out and blend it over the values.
unsafe fn extra_colour(this: u64) {
    let step = rf32(got(GOT_TIMESTEP)) / 120.0;
    let cur = rf32(this + 0x2be8);
    let a = if r32(this + 0x2be4) != 0 { fmin(cur + step, 1.0) } else { fmax(cur - step, 0.0) };
    w32f(this + 0x2be8, a);
    if !(a > 0.0) {
        return;
    }
    let idx = ri32(this + 0x2be0) as i64 as u64;
    let ia = 1.0 - a;
    let row = this.wrapping_add(idx.wrapping_mul(8));
    let ambient = r8(row + 0xdc6) != 0 || r8(row + 0xe86) != 0 || r8(row + 0xf46) != 0;
    let colour_scale = rf32(got(GOT_COLOUR_SCALE) + 0x4c);
    for fd in FIELDS.iter() {
        if fd.extra == IfAmbient && !ambient {
            continue;
        }
        // the extra colour sits at weather 6 of the hour row `idx`
        let size = fd.kind.size();
        let e = elem(fd.kind, (this + fd.table).wrapping_add(idx.wrapping_mul(8 * size)) + 6 * size);
        let e = if fd.store == Tenth { e / 10.0 } else { e };
        let o = this + fd.out;
        let old = match fd.store {
            Int => ri32(o) as f32,
            Short => ri16(o) as f32,
            _ => rf32(o),
        };
        let mut v = fma(old, ia, a * e);
        match fd.extra {
            IfAmbient => {}
            Scaled(slot) => v = rf32(got(slot) + 0x4c) * v,
            Colour(slot) => v = v * rf32(got(slot) + 0x4c) * colour_scale,
        }
        match fd.store {
            Int => w32(o, to_int(v) as u32),
            Short => w16(o, to_short(v)),
            _ => w32f(o, v),
        }
    }
}

/// void CTimeCycle::Update(): ForceUpdate on every 16th frame.
#[no_mangle]
pub unsafe extern "C" fn G_3705f0(c: *mut Cpu) {
    // not shadow-verified: running the original would also run ForceUpdate (which has state)
    if r8(got(GOT_UPDATE_FRAME)) & 0xf == 0 {
        call(c, FORCE_UPDATE, &[(*c).x[0]]);
    }
}

/// void CTimeCycle::StartExtraColour(int index, bool fade)
#[no_mangle]
pub unsafe extern "C" fn G_37060c(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_37060c_orig, Ret::None, &[(this + 0x2be0, 12)], |c| {
        w32(this + 0x2be0, c.w(1));
        w32(this + 0x2be4, 1);
        w32f(this + 0x2be8, if c.w(2) & 1 != 0 { 0.0 } else { 1.0 });
    });
}

/// void CTimeCycle::StopExtraColour(bool fade)
#[no_mangle]
pub unsafe extern "C" fn G_370630(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_370630_orig, Ret::None, &[(this + 0x2be0, 12), (got(GOT_EXTRA_RESET), 16)], |c| {
        w32(this + 0x2be4, 0);
        if c.w(1) & 1 == 0 {
            w32(this + 0x2be8, 0);
        }
        let reset = got(GOT_EXTRA_RESET);
        w64(reset, r64(G + 0x235620));
        w64(reset + 8, r64(G + 0x235628));
    });
}

/// void CTimeCycle::UpdateArrays(): pack all float tables into the per-type tables.
#[no_mangle]
pub unsafe extern "C" fn G_36d194(c: *mut Cpu) {
    let this = (*c).x[0];
    verify::port_regions(c, G_36d194_orig, Ret::None, &[(this + 0x280, 0x2880)], |c| {
        for (i, fd) in FIELDS.iter().enumerate() {
            let fill = match fd.kind {
                U8 => FILL_GAPS_U8,
                I8 => FILL_GAPS_I8,
                I16 => FILL_GAPS_I16,
            };
            call(c, fill, &[this + fd.table, SOURCE + i as u64 * 0x300]);
        }
    });
}

/// template<T> void FillGaps(T out[24][8], float in[24][8]): convert every value, then fill the
/// hours timecyc.dat left at -1 by blending linearly between the given hours around them.
unsafe fn fill_gaps(out: u64, src: u64, kind: Kind) {
    let size = kind.size();
    let at = |h: i32, w: u64| src.wrapping_add((h as i64 as u64).wrapping_mul(32)) + w * 4;
    let put = |h: i32, w: u64, v: f32| {
        let a = out.wrapping_add((h as i64 as u64).wrapping_mul(8 * size)) + w * size;
        match kind {
            U8 => w8(a, (v as u32).min(255) as u8),
            I8 => w8(a, (v as i32).clamp(-128, 127) as u8),
            I16 => w16(a, to_short(v)),
        }
    };
    for w in 0..8u64 {
        for h in 0..24 {
            put(h, w, rf32(at(h, w)));
        }
        let first = match (0..24).find(|&h| rf32(at(h, w)) != -1.0) {
            Some(h) => h,
            None => continue,
        };
        // walk once around the day from the first given hour (hour 24 is read past the table
        // when the first given hour is 23, like the original)
        let mut k = first + 1;
        while k != first {
            if rf32(at(k, w)) != -1.0 {
                k = (k + 1) % 24;
                continue;
            }
            let prev = (k + 23) % 24;
            loop {
                k = (k + 1) % 24;
                if rf32(at(k, w)) != -1.0 {
                    break;
                }
            }
            let mut n = (k - prev + 24) % 24;
            if n == 0 {
                n = 24;
            }
            if n >= 2 {
                for i in 1..n {
                    let t = i as f32 / n as f32;
                    let v = fma(rf32(at(prev, w)), 1.0 - t, t * rf32(at(k, w)));
                    put((prev + i) % 24, w, v);
                }
            }
        }
    }
}

/// void FillGaps<unsigned char>(unsigned char (*)[8], float (*)[8])
#[no_mangle]
pub unsafe extern "C" fn G_36f090(c: *mut Cpu) {
    let (out, src) = ((*c).x[0], (*c).x[1]);
    verify::port_regions(c, G_36f090_orig, Ret::None, &[(out, 24 * 8)], |_| fill_gaps(out, src, U8));
}

/// void FillGaps<signed char>(signed char (*)[8], float (*)[8])
#[no_mangle]
pub unsafe extern "C" fn G_36f66c(c: *mut Cpu) {
    let (out, src) = ((*c).x[0], (*c).x[1]);
    verify::port_regions(c, G_36f66c_orig, Ret::None, &[(out, 24 * 8)], |_| fill_gaps(out, src, I8));
}

/// void FillGaps<short>(short (*)[8], float (*)[8])
#[no_mangle]
pub unsafe extern "C" fn G_36fd14(c: *mut Cpu) {
    let (out, src) = ((*c).x[0], (*c).x[1]);
    verify::port_regions(c, G_36fd14_orig, Ret::None, &[(out, 24 * 16)], |_| fill_gaps(out, src, I16));
}

/// float CTimeCycle::Interpolate(T *current_hour, T *next_hour): the four-way blend of one value,
/// with the weights ForceUpdate last stored.
unsafe fn interpolate(c: &mut Cpu, kind: u8) {
    let (a, b) = (c.x[1], c.x[2]);
    let (ow, nw) = weathers();
    let w = weights_now();
    let at = |h: u64, wt: i64| {
        let p = if h == 0 { a } else { b };
        match kind {
            0 => r8(p.wrapping_add(wt as u64)) as f32,
            1 => ri8(p.wrapping_add(wt as u64)) as f32,
            2 => ri16(p.wrapping_add((wt * 2) as u64)) as f32,
            3 => ri32(p.wrapping_add((wt * 4) as u64)) as f32,
            _ => rf32(p.wrapping_add((wt * 4) as u64)),
        }
    };
    c.set_s(0, blend(w, at, 0, 1, ow, nw));
}

/// float CTimeCycle::Interpolate(unsigned char*, unsigned char*)
#[no_mangle]
pub unsafe extern "C" fn G_3703bc(c: *mut Cpu) {
    verify::port(c, G_3703bc_orig, Ret::S0, |c| interpolate(c, 0));
}
/// float CTimeCycle::Interpolate(signed char*, signed char*)
#[no_mangle]
pub unsafe extern "C" fn G_370428(c: *mut Cpu) {
    verify::port(c, G_370428_orig, Ret::S0, |c| interpolate(c, 1));
}
/// float CTimeCycle::Interpolate(short*, short*)
#[no_mangle]
pub unsafe extern "C" fn G_370494(c: *mut Cpu) {
    verify::port(c, G_370494_orig, Ret::S0, |c| interpolate(c, 2));
}
/// float CTimeCycle::Interpolate(int*, int*)
#[no_mangle]
pub unsafe extern "C" fn G_370518(c: *mut Cpu) {
    verify::port(c, G_370518_orig, Ret::S0, |c| interpolate(c, 3));
}
/// float CTimeCycle::Interpolate(float*, float*)
#[no_mangle]
pub unsafe extern "C" fn G_37058c(c: *mut Cpu) {
    verify::port(c, G_37058c_orig, Ret::S0, |c| interpolate(c, 4));
}

extern "C" {
    fn G_36d570_orig(c: *mut Cpu);
    fn G_37060c_orig(c: *mut Cpu);
    fn G_370630_orig(c: *mut Cpu);
    fn G_36d194_orig(c: *mut Cpu);
    fn G_36f090_orig(c: *mut Cpu);
    fn G_36f66c_orig(c: *mut Cpu);
    fn G_36fd14_orig(c: *mut Cpu);
    fn G_3703bc_orig(c: *mut Cpu);
    fn G_370428_orig(c: *mut Cpu);
    fn G_370494_orig(c: *mut Cpu);
    fn G_370518_orig(c: *mut Cpu);
    fn G_37058c_orig(c: *mut Cpu);
}
