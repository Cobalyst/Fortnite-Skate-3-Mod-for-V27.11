//! Skate space (metres, Y up, right-handed) <-> Unreal space (cm, Z up, left-handed).
//! A point maps as ue = (s.x, s.z, s.y) * 100: swapping Y and Z is a reflection, so
//! matrices are conjugated by it and triangle winding flips.

pub fn ue_to_skate(p: [f32; 3]) -> [f32; 3] {
    [p[0] / 100., p[2] / 100., p[1] / 100.]
}

pub fn skate_to_ue(p: [f32; 3]) -> [f32; 3] {
    [p[0] * 100., p[2] * 100., p[1] * 100.]
}

/// Direction (no scaling).
pub fn skate_dir_to_ue(v: [f32; 3]) -> [f32; 3] {
    [v[0], v[2], v[1]]
}

/// Column-major 4x4 skate matrix -> Unreal: swap the Y and Z rows and columns, metres -> cm.
pub fn skate_matrix_to_ue(m: &[f32; 16]) -> [f32; 16] {
    let swap = |i: usize| match i {
        1 => 2,
        2 => 1,
        i => i,
    };
    let mut out = [0f32; 16];
    for c in 0..4 {
        for r in 0..4 {
            let v = m[swap(c) * 4 + swap(r)];
            out[c * 4 + r] = if c == 3 && r < 3 { v * 100. } else { v };
        }
    }
    out
}

/// Column-major 4x4 multiply: a * b.
pub fn mul(a: &[f32; 16], b: &[f32; 16]) -> [f32; 16] {
    let mut out = [0f32; 16];
    for c in 0..4 {
        for r in 0..4 {
            out[c * 4 + r] = (0..4).map(|k| a[k * 4 + r] * b[c * 4 + k]).sum();
        }
    }
    out
}

pub fn transform_point(m: &[f32; 16], p: [f32; 3]) -> [f32; 3] {
    [
        m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
        m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
        m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14],
    ]
}

pub fn transform_vector(m: &[f32; 16], v: [f32; 3]) -> [f32; 3] {
    [
        m[0] * v[0] + m[4] * v[1] + m[8] * v[2],
        m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
        m[2] * v[0] + m[6] * v[1] + m[10] * v[2],
    ]
}

/// Blend two rigid transforms: lerp translation, lerp rotation columns and re-orthonormalise.
/// Good for the small differences between consecutive simulation ticks.
pub fn blend(a: &[f32; 16], b: &[f32; 16], t: f32) -> [f32; 16] {
    let lerp = |i: usize| a[i] + (b[i] - a[i]) * t;
    let norm = |v: [f32; 3]| {
        let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt().max(1e-9);
        [v[0] / l, v[1] / l, v[2] / l]
    };
    let cross = |u: [f32; 3], v: [f32; 3]| [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]];
    let x = norm([lerp(0), lerp(1), lerp(2)]);
    let y0 = [lerp(4), lerp(5), lerp(6)];
    let z = norm(cross(x, y0));
    let y = cross(z, x);
    [x[0], x[1], x[2], 0., y[0], y[1], y[2], 0., z[0], z[1], z[2], 0., lerp(12), lerp(13), lerp(14), 1.]
}
