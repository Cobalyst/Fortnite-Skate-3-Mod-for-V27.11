//! Cylinder generic primitive (RenderWare VolumeType 5) for the object-dropper DMOs.
//!
//! NOT a recovered native routine: the TU3 cylinder GP callbacks (dispatch slot 5 of the
//! generic-primitive tables) were not located in this executable (Fortnite skate mod, 2026-10-09).
//! The cylinder is exact geometry inside the recovered GP framework: the projection interval is
//! the cylinder's true support, and the maximum feature uses the same record shapes the native
//! box and capsule callbacks produce (a side segment, a cap face, or a rim point), with the cap
//! as the largest face the record holds (8 edges). GP layout: origin +0, axis as normal +16 and
//! as edge +64, half height word 28, radius word 29, counts 1 normal / 1 edge.
use super::{MaximumFeature, initialize_feature_segment};
use crate::physics::reciprocal_sqrt::estimate;

type V = [f32; 4];

/// The box callback's "small projection" threshold (0x3e4ccccd = 0.2) decides side / cap / rim.
const SMALL: f32 = 0.2;

pub fn cylinder_maximum_feature(gp: &[u32; 48], mode: u32, direction: [u32; 4], output: &mut MaximumFeature) {
    let center = load(gp, 0);
    let axis = load(gp, 16);
    let half = f32::from_bits(gp[28]);
    let radius = f32::from_bits(gp[29]);
    let d = direction.map(f32::from_bits);
    let t = dot(axis, d);
    // the direction's part across the axis, and its length (how far the rim faces d)
    let across = sub(d, scale(axis, t));
    let across_len = length(across);
    if t.abs() < SMALL && across_len > f32::from_bits(0x3400_0000) {
        // side: the line of the wall facing d, from cap to cap
        let rim = madd(across, radius / across_len, center);
        let negative = sub(rim, scale(axis, half));
        let positive = madd(axis, half, rim);
        let mut segment = [0; 16];
        initialize_feature_segment(&mut segment, negative.map(f32::to_bits), positive.map(f32::to_bits));
        output[4..20].copy_from_slice(&segment);
        output[140] = 1;
    } else if across_len < SMALL {
        cap(center, axis, half, radius, if t > 0.0 { 1.0 } else { -1.0 }, mode, d, output);
    } else {
        // rim point furthest along d
        let sign = if t > 0.0 { 1.0 } else { -1.0 };
        let rim = madd(across, radius / across_len, madd(axis, half * sign, center));
        store(output, 136, rim);
        output[140] = 0;
    }
    output[0] = 0;
}

/// The cap facing d as an 8-sided face, in the box callback's record layout and winding rule.
fn cap(center: V, axis: V, half: f32, radius: f32, sign: f32, mode: u32, direction: V, output: &mut MaximumFeature) {
    let face_center = madd(axis, half * sign, center);
    // u, v across the axis with u x v = axis
    let helper: V = if axis[0].abs() < 0.9 { [1.0, 0.0, 0.0, 0.0] } else { [0.0, 1.0, 0.0, 0.0] };
    let u = normalize(cross(helper, axis));
    let v = cross(axis, u);
    const N: usize = 8;
    // clockwise about u x v (= axis): the box face's [0,1,2,3] order
    let clockwise: [V; N] = std::array::from_fn(|k| {
        let a = -(k as f32) * std::f32::consts::TAU / N as f32;
        add(face_center, add(scale(u, radius * a.cos()), scale(v, radius * a.sin())))
    });
    let points: [V; N] = if u32::from(sign < 0.0) == mode {
        clockwise
    } else {
        std::array::from_fn(|k| clockwise[(N - k) % N])
    };
    for i in 0..N {
        let edge = sub(points[(i + 1) % N], points[i]);
        let len = length(edge);
        let dir = if len > 0.0 { scale(edge, 1.0 / len) } else { [0.0; 4] };
        let offset = 4 + i * 16;
        store(output, offset, points[i]);
        store(output, offset + 4, dir);
        store(output, offset + 8, plane(dir, direction, mode));
        output[offset + 12..offset + 16].fill(len.to_bits());
    }
    store(output, 132, scale(axis, -sign));
    output[140] = N as u32;
}

fn plane(axis: V, direction: V, mode: u32) -> V {
    let result = if mode != 0 { cross(axis, direction) } else { cross(direction, axis) };
    let squared = dot(result, result);
    if squared > f32::from_bits(0x3400_0000) {
        let mut r = estimate(squared);
        for _ in 0..2 {
            r = (r * 0.5).mul_add((-squared).mul_add(r * r, 1.0), r);
        }
        scale(result, r)
    } else {
        result
    }
}

/// Support of the cylinder along a unit direction (without radius/fatness, which is zero).
pub fn cylinder_extent(gp: &[u32; 48], direction: V) -> f32 {
    let axis = load(gp, 16);
    let t = dot(axis, direction);
    let half = f32::from_bits(gp[28]);
    let radius = f32::from_bits(gp[29]);
    t.abs() * half + radius * (1.0 - t * t).max(0.0).sqrt()
}

fn load(words: &[u32], offset: usize) -> V {
    std::array::from_fn(|i| f32::from_bits(words[offset + i]))
}
fn store(words: &mut [u32], offset: usize, v: V) {
    words[offset..offset + 4].copy_from_slice(&v.map(f32::to_bits));
}
fn dot(a: V, b: V) -> f32 {
    super::arithmetic::dot(a[..3].try_into().unwrap(), b[..3].try_into().unwrap())
}
fn length(a: V) -> f32 {
    dot(a, a).sqrt()
}
fn normalize(a: V) -> V {
    let l = length(a);
    if l > 0.0 { scale(a, 1.0 / l) } else { a }
}
fn scale(a: V, b: f32) -> V {
    a.map(|x| x * b)
}
fn sub(a: V, b: V) -> V {
    std::array::from_fn(|i| a[i] - b[i])
}
fn add(a: V, b: V) -> V {
    std::array::from_fn(|i| a[i] + b[i])
}
fn madd(a: V, b: f32, c: V) -> V {
    std::array::from_fn(|i| a[i].mul_add(b, c[i]))
}
fn cross(a: V, b: V) -> V {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 0.0]
}
