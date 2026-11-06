pub(super) fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

pub(super) fn linear_to_srgb(c: f32) -> f32 {
    if c <= 0.003_130_8 {
        12.92 * c
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    }
}

pub(super) fn clamp01(value: f32) -> f32 {
    value.clamp(0.0, 1.0)
}

pub(super) fn rgb8_to_hsv(r8: u8, g8: u8, b8: u8) -> (f32, f32, f32) {
    let r = f32::from(r8) / 255.0;
    let g = f32::from(g8) / 255.0;
    let b = f32::from(b8) / 255.0;
    let max = r.max(g).max(b);
    let min = r.min(g).min(b);
    let delta = max - min;
    let hue = if delta <= f32::EPSILON {
        0.0
    } else if max == r {
        60.0 * (((g - b) / delta) % 6.0)
    } else if max == g {
        60.0 * (((b - r) / delta) + 2.0)
    } else {
        60.0 * (((r - g) / delta) + 4.0)
    };
    let hue = (hue + 360.0) % 360.0;
    let sat = if max <= f32::EPSILON {
        0.0
    } else {
        delta / max
    };
    (hue, sat, max)
}

pub(super) fn hsv_to_rgb8(hue: f32, sat: f32, value: f32) -> (u8, u8, u8) {
    let h = hue.rem_euclid(360.0) / 60.0;
    let c = value * sat;
    let x = c * (1.0 - (h % 2.0 - 1.0).abs());
    let (r, g, b) = match h as u32 {
        0 => (c, x, 0.0),
        1 => (x, c, 0.0),
        2 => (0.0, c, x),
        3 => (0.0, x, c),
        4 => (x, 0.0, c),
        _ => (c, 0.0, x),
    };
    let m = value - c;
    let to_u8 = |v: f32| ((v + m) * 255.0 + 0.5) as u8;
    (to_u8(r), to_u8(g), to_u8(b))
}

pub(super) fn rgb8_to_xy(r8: u8, g8: u8, b8: u8) -> (f32, f32) {
    let r = srgb_to_linear(f32::from(r8) / 255.0);
    let g = srgb_to_linear(f32::from(g8) / 255.0);
    let b = srgb_to_linear(f32::from(b8) / 255.0);
    let x = r * 0.4124 + g * 0.3576 + b * 0.1805;
    let y = r * 0.2126 + g * 0.7152 + b * 0.0722;
    let z = r * 0.0193 + g * 0.1192 + b * 0.9505;
    let sum = x + y + z;
    if sum <= 0.0 {
        return (0.0, 0.0);
    }
    (x / sum, y / sum)
}

pub(super) fn xy_to_rgb8(x: f32, y: f32) -> (u8, u8, u8) {
    if y <= 0.0 {
        return (0, 0, 0);
    }
    let big_y = 1.0;
    let big_x = (big_y / y) * x;
    let big_z = (big_y / y) * (1.0 - x - y);
    let mut r = big_x * 3.2406 - big_y * 1.5372 - big_z * 0.4986;
    let mut g = -big_x * 0.9689 + 1.8758 + big_z * 0.0415;
    let mut b = big_x * 0.0557 - 0.204 + big_z * 1.057;
    let m = r.max(g).max(b).max(1.0);
    r /= m;
    g /= m;
    b /= m;
    let to_u8 = |v: f32| (clamp01(linear_to_srgb(v)) * 255.0 + 0.5) as u8;
    (to_u8(r), to_u8(g), to_u8(b))
}

pub(super) fn rgb_hex(r: u8, g: u8, b: u8) -> u32 {
    (u32::from(r) << 16) | (u32::from(g) << 8) | u32::from(b)
}
