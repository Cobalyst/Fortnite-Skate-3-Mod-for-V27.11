//! Stock MotionGraph moving-object (on-foot prop pushing) behaviours, from our build:
//! InitMovingObjects (factory 82B9FFF8, Begin 82B8DD08) and MovingObject (factory
//! 82BA0098, ctor 82B8DDE8, instance 82B8F1D8, Begin 82B8E190, Update 82B8E350), plus the
//! state they share through ISkaterAnim +0x140 (get v88 82B6EAD0, set v92 82B6EAD8) that
//! AngleAdjustByAnimationEndLessThan (82B7EE98) reads.
use skate_core::point_graph::PointGraph;

const PI: f32 = f32::from_bits(0x4049_0fdb);
const HALF_PI: f32 = f32::from_bits(0x3fc9_0fdb);
const TAU: f32 = f32::from_bits(0x40c9_0fdb);
const INV_TAU: f32 = f32::from_bits(0x3e22_f983);
/// 0x822F32AC: 60 frames per second, stored as 0x426FFFFF.
const FRAMES_PER_SECOND: f32 = f32::from_bits(0x426f_ffff);

/// The ten words ISkaterAnim keeps for the moving-object behaviours.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct State {
    /// Facing of the push relative to the object (+0 / instance +8).
    pub angle: f32,
    /// The "angle" animation attribute, 0..1 across the active quadrant (+4).
    pub normalized_angle: f32,
    /// The "Speed" attribute, smoothed (+8).
    pub speed: f32,
    /// The "strength" attribute, smoothed (+12).
    pub strength: f32,
    /// +16 is copied but never written by these behaviours.
    pub word_16: u32,
    /// The "crouch" attribute, smoothed (+20).
    pub crouch: f32,
    /// Transition target angle: 0 for push, pi for pull (+24).
    pub target: f32,
    /// Direction of the newest MovingObject begin, and the one before it (+28 / +32).
    pub current: u32,
    pub previous: u32,
    /// +36 high byte: bit 0x80 set once the push/pull flip has turned `angle` half a turn.
    pub flags: u8,
}

/// Direction enum written by the ctor (82B8DF5C..E180); unknown text is 0 (still).
pub fn direction(text: &str) -> u32 {
    match text {
        "PUSHLEFT" => 1,
        "PUSHRIGHT" => 2,
        "PUSHLEFTToRIGHT" => 3,
        "PUSHRIGHTToLEFT" => 4,
        "PUSHToPULLLEFT" => 5,
        "PUSHToPULLRIGHT" => 6,
        "PULLLEFT" => 7,
        "PULLRIGHT" => 8,
        "PULLLEFTToRIGHT" => 9,
        "PULLRIGHTToLEFT" => 10,
        "PULLToPUSHLEFT" => 11,
        "PULLToPUSHRIGHT" => 12,
        _ => 0,
    }
}

fn push(d: u32) -> bool {
    (1..=6).contains(&d)
}
fn pull(d: u32) -> bool {
    (7..=12).contains(&d)
}
fn transition(d: u32) -> bool {
    matches!(d, 3..=6 | 9..=12)
}
/// The left-hand variants (1, 3, 5, 7, 9, 11).
fn left(d: u32) -> bool {
    d != 0 && d % 2 == 1
}
fn right(d: u32) -> bool {
    d != 0 && d % 2 == 0
}

/// The native wrap into [-pi, pi]: t / 2pi, fraction, minus one above one half, times 2pi.
fn wrap(angle: f32) -> f32 {
    let t = angle * INV_TAU;
    let f = t - t.floor();
    (if f > 0.5 { f - 1.0 } else { f }) * TAU
}

/// vrsqrtefp with its two Newton steps, times the input (zero stays zero).
fn vector_sqrt(x: f32) -> f32 {
    if x == 0.0 {
        return 0.0;
    }
    let mut r = 1.0 / x.sqrt();
    for _ in 0..2 {
        r = (r * 0.5).mul_add((-x).mul_add(r * r, 1.0), r);
    }
    x * r
}

/// 82B8F308: `angle` limited to the arc from `start` to `end` (wrapping when start >= end);
/// outside it, the nearer endpoint by wrapped distance (the end on a tie).
fn clamp_arc(angle: f32, start: f32, end: f32) -> f32 {
    let inside = if !(start >= end) {
        !(angle < start) && angle <= end
    } else {
        angle >= start || angle <= end
    };
    if inside {
        return angle;
    }
    let to_start = wrap(start - angle).abs();
    let to_end = wrap(angle - end).abs();
    if to_start >= to_end { end } else { start }
}

/// fsel: `a >= 0` selects `b`, anything else (NaN included) `c`.
fn fsel(a: f32, b: f32, c: f32) -> f32 {
    if a >= 0.0 { b } else { c }
}
/// The native clamp pair: at least `low`, then at most `high`.
fn clamp(value: f32, low: f32, high: f32) -> f32 {
    let at_least = fsel(low - value, low, value);
    fsel(high - at_least, at_least, high)
}

/// Per-instance copy (82B8F1D8 allocates it; Begin fills it from the shared state).
#[derive(Clone, Copy, Debug, Default)]
pub struct Instance {
    pub data: State,
}

/// InitMovingObjects Begin 82B8DD08: angles, attributes, target and both directions to
/// zero, the flip bit cleared; word16 and the low flag bits survive.
pub fn init(state: &mut State) {
    *state = State {
        word_16: state.word_16,
        flags: state.flags & 0x7f,
        ..State::default()
    };
}

/// MovingObject Begin 82B8E190. `stance_match` is ISkaterAnim v32 (82B6EC88: natural and
/// relative stance equal); the result is the v40 (82B70088) request for SkaterAnim flags
/// bits 22 / 21 (one extra mirror node on the next animation tree).
pub fn begin(state: &State, direction: u32, stance_match: bool) -> (Instance, bool) {
    let mut data = *state;
    data.previous = data.current;
    data.current = direction;
    data.flags &= 0x7f;
    let mirror = (right(direction) && !stance_match) || (left(direction) && stance_match);
    (Instance { data }, mirror)
}

/// Completed OffBoard publication (OffBoardPushing Fill 82D1C188): push-point velocity
/// forward / left, filtered force forward / left, then +16 .. +28; +28 is the effort.
#[derive(Clone, Copy, Debug, Default)]
pub struct Physical {
    pub words: [f32; 8],
}

/// Animation attributes written by Update, in its call order.
pub struct Attributes {
    pub angle: f32,
    pub strength: f32,
    pub crouch: f32,
    /// Global "Crouch": the raw effort.
    pub crouch_global: f32,
    /// Global "Speed".
    pub speed: f32,
}

/// MovingObject Update 82B8E350. `time` / `length` are the current animation tree's
/// (v24 / v20), needed only for the transition directions.
pub fn update(
    instance: &mut Instance,
    state: &mut State,
    direction: u32,
    blend_out_over_next_cycle: bool,
    physical: &Physical,
    strength_graph: &PointGraph<8>,
    time: impl FnOnce() -> Result<(f32, f32), String>,
) -> Result<Attributes, String> {
    let [forward, side, force_forward, force_side, ..] = physical.words;
    let effort = physical.words[7];
    let i = &mut instance.data;
    let squared = forward * forward + side * side;
    let mut speed = 0.0;
    let mut force = 0.0;
    if squared > f32::from_bits(0x38d1_b717) {
        speed = vector_sqrt(squared);
        let along = force_side * side + force_forward * forward;
        let k = along / squared;
        if !(along < 0.0) {
            let (pf, ps) = (forward * k, side * k);
            force = vector_sqrt(ps * ps + pf * pf);
        }
    }
    let curve = strength_graph.evaluate(force);
    let limit = |value: f32, around: f32, step: f32| clamp(value, around - step, around + step);
    let strength = limit(curve, i.strength, f32::from_bits(0x3c23_d70a));
    i.strength = strength;
    let crouch = limit(effort, i.crouch, f32::from_bits(0x3cf5_c28f));
    i.crouch = crouch;
    let walk = clamp(speed * f32::from_bits(0x3ecc_cccd), 0.0, 1.0);
    let smoothed_speed = limit(walk, i.speed, f32::from_bits(0x3cf5_c28f));
    let mut angle = i.angle;
    let flipped = (pull(i.previous) && push(i.current)) || (push(i.previous) && pull(i.current));
    if flipped && i.flags & 0x80 == 0 {
        i.flags |= 0x80;
        angle = wrap(angle + PI);
    }
    let step = if transition(direction) {
        let (time, length) = time()?;
        let extra = if blend_out_over_next_cycle { length } else { 0.0 };
        let frames = (length - time + extra) * FRAMES_PER_SECOND;
        let frames = if 1.0 - frames >= 0.0 { 1.0 } else { frames };
        i.target = 0.0;
        if pull(direction) {
            i.target = PI;
        }
        wrap(i.target - angle) / frames
    } else {
        // atan2(-side, forward) through the native reciprocal and vector atan.
        let estimate = 1.0 / forward;
        let reciprocal = estimate.mul_add((-estimate).mul_add(forward, 1.0), estimate);
        let base = skate_core::input::angle::atan(-side * reciprocal);
        let toward = if forward == 0.0 {
            HALF_PI.copysign(-side)
        } else if 0.0 > forward {
            PI.copysign(-side) + base
        } else {
            base
        };
        let max = f32::from_bits(0x3d0e_fa35);
        clamp(wrap(toward - angle), -max, max)
    };
    let mut angle = wrap(angle + step);
    if push(direction) {
        angle = if left(direction) { clamp_arc(angle, -HALF_PI, 0.0) } else { clamp_arc(angle, 0.0, HALF_PI) };
    } else if pull(direction) {
        angle = if left(direction) { clamp_arc(angle, PI, -HALF_PI) } else { clamp_arc(angle, HALF_PI, PI) };
    }
    i.angle = angle;
    let normalized = if push(direction) {
        if left(direction) { -angle / HALF_PI } else { angle / HALF_PI }
    } else if pull(direction) {
        if left(direction) { wrap(angle + PI) / HALF_PI } else { wrap(PI - angle) / HALF_PI }
    } else {
        i.normalized_angle
    };
    i.normalized_angle = normalized;
    i.speed = smoothed_speed;
    *state = *i;
    Ok(Attributes { angle: normalized, strength, crouch, crouch_global: effort, speed: smoothed_speed })
}

/// AngleAdjustByAnimationEndLessThan 82B7EE98 (factory 82B9DBF0, MaxDeltaAngle default 2):
/// the turn still owed to the transition target, spread over the frames left in the
/// current animation, is below MaxDeltaAngle degrees per frame.
pub fn angle_adjust_less_than(state: &State, max_delta_degrees: f32, time: f32, length: f32) -> bool {
    let owed = wrap(state.target - state.angle).abs();
    let frames = (length - time) * FRAMES_PER_SECOND;
    let frames = if 1.0 - frames >= 0.0 { 1.0 } else { frames };
    (1.0 / frames) * owed < max_delta_degrees * f32::from_bits(0x3c8e_fa35)
}

/// anim_moveobjects/default layout +160 (Hash_C3A15E5DE438066F): push strength from force.
pub fn load_strength_graph(data: &skate_data::collections::Collections) -> Result<PointGraph<8>, String> {
    let w = data
        .words::<20>("anim_moveobjects", "default", "Hash_C3A15E5DE438066F")?
        .map(f32::from_bits);
    Ok(PointGraph { x: w[4..12].try_into().unwrap(), y: w[12..20].try_into().unwrap() })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arc_clamp_picks_the_nearer_end() {
        assert_eq!(clamp_arc(0.3, -HALF_PI, 0.0), 0.0);
        assert_eq!(clamp_arc(-2.0, -HALF_PI, 0.0), -HALF_PI);
        assert_eq!(clamp_arc(-3.0, PI, -HALF_PI), -3.0);
        assert_eq!(clamp_arc(0.1, PI, -HALF_PI), -HALF_PI);
    }

    #[test]
    fn direction_names() {
        assert_eq!(direction("PULLToPUSHRIGHT"), 12);
        assert_eq!(direction("still"), 0);
        assert!(left(7) && right(8) && !left(0));
    }
}
