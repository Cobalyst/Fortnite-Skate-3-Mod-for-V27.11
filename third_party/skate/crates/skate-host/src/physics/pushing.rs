//! PHYSICS_STATE_OFFBOARD_PUSHING (502): the off-board skater holding RB on a prop's grab spline,
//! pushing / pulling / turning it (Fortnite skate mod, ported 2026-10-09 from the Skate 3 image).
//!
//! Ours (the shipped executable; TU3 addresses are 0x2AD40 higher here): vtable 82321884,
//! constructor 82D18CB8, reset 82D18F28, Enter 82D19488, Update 82D19BC8, Exit 82D19B70,
//! Fill 82D1C188, Post 82D1C120. Helpers: attach 82D19658, grab frame 82D1AEE8, facing 82D19A90
//! (+ grab normal 82DDE818), frame blend 82D1B3D0 / 82D1B590, hand / root targets 82D1B7C8,
//! force and torque 82D1A4D0, skeleton root 82BB6B90 / 82BB8980.
//! Tuning: physics_state_offboard_pushing/default (hashed fields), the biped grab settings
//! (physics_state_offboard/default) and the record's assembly data (the prop's PartDefinition).
//!
//! The prop moves by the force / torque this state computes (the grab-spline manager queues them;
//! the DMO provider adds the force to the body's force accumulator and replaces its torque): see
//! dmo.rs `push`. Everything else here is the skater side.
use super::{GamePhysics, SkaterRuntime};
use skate_core::{
    animation::foot_ik::interpolate_affine,
    physics::{
        skeleton_animation_record::{AnimationPartTransform as Frame, IDENTITY, compose_affine, transform_point},
        skeleton_landing_on_board::world_up_correction,
        skeleton_root::{inverse_rigid, orthonormalize},
    },
    player::offboard::{
        grab_scene::{self, Descriptor, Query, Record, polyline},
        ground_query::QueryContext,
        ground_sync::{BoardLimits, Bounds, board_bounds},
    },
    point_graph::PointGraph,
};
use skate_data::collections::Collections;

type V = [f32; 4];

const DT: f32 = f32::from_bits(0x3c88_8889);
const ZERO: V = [0.0; 4];
const UP: V = [0.0, 1.0, 0.0, 0.0];
/// 830BD350 / 83060A70: safe-normalize length threshold (82F57A68 splats 8217FB28).
const UNIT_THRESHOLD: f32 = f32::from_bits(0x3586_37bd);
const DEG: f32 = f32::from_bits(0x3c8e_fa35);
const PI: f32 = f32::from_bits(0x4049_0fdb);
const TAU: f32 = f32::from_bits(0x40c9_0fdb);
const INV_TAU: f32 = f32::from_bits(0x3e22_f983);

// ---- vector arithmetic in the native order --------------------------------------------------

fn dot(a: V, b: V) -> f32 {
    a[0].mul_add(b[0], a[1].mul_add(b[1], a[2] * b[2]))
}
fn add(a: V, b: V) -> V {
    std::array::from_fn(|i| a[i] + b[i])
}
fn sub(a: V, b: V) -> V {
    std::array::from_fn(|i| a[i] - b[i])
}
fn scale(a: V, k: f32) -> V {
    a.map(|v| v * k)
}
/// a * k + b (vmaddfp with a splat).
fn madd(a: V, k: f32, b: V) -> V {
    std::array::from_fn(|i| a[i].mul_add(k, b[i]))
}
fn neg(a: V) -> V {
    a.map(|v| f32::from_bits(v.to_bits() ^ 0x8000_0000))
}
fn flat(mut a: V) -> V {
    a[1] = 0.0;
    a
}
/// (a * b.yzx - a.yzx * b).yzx
fn cross(a: V, b: V) -> V {
    let r = [a[1], a[2], a[0], a[3]];
    let s = [b[1], b[2], b[0], b[3]];
    let t: V = std::array::from_fn(|i| (-r[i]).mul_add(b[i], a[i] * s[i]));
    [t[1], t[2], t[0], t[3]]
}
/// vrsqrtefp followed by the two native refinements.
fn rsqrt(d: f32) -> f32 {
    let mut r = 1.0 / d.sqrt();
    for _ in 0..2 {
        r = (r * 0.5).mul_add((-d).mul_add(r * r, 1.0), r);
    }
    r
}
/// vrefp followed by the two native refinements.
fn recip(d: f32) -> f32 {
    let mut r = 1.0 / d;
    for _ in 0..2 {
        r = r.mul_add((-r).mul_add(d, 1.0), r);
    }
    r
}
/// Native length: d * rsqrt(d), exactly zero for a zero vector.
fn length(a: V) -> f32 {
    let d = dot(a, a);
    if d == 0.0 { 0.0 } else { d * rsqrt(d) }
}
/// Native safe normalize: zero unless the length exceeds 1e-6.
fn unit(a: V) -> V {
    let d = dot(a, a);
    let len = if d == 0.0 { 0.0 } else { d * rsqrt(d) };
    if len > UNIT_THRESHOLD { scale(a, rsqrt(d)) } else { ZERO }
}
/// 82BAB6B8: length limited to `limit` (vectors shorter than 2^-16 are returned unchanged).
fn clamp_length(a: V, limit: f32) -> V {
    let len = length(a);
    if !(len >= f32::from_bits(0x3780_0000)) {
        return a;
    }
    let capped = if limit - len >= 0.0 { len } else { limit };
    scale(a, capped * recip(len))
}
/// 82BABAD8: `target`, unless it is farther than `limit` from `current`.
fn move_towards(target: V, current: V, limit: f32) -> V {
    let delta = sub(target, current);
    let len = length(delta);
    if limit >= len {
        return target;
    }
    madd(delta, limit * recip(len), current)
}
/// 82947118: unsigned angle between two vectors (zero when either is shorter than 0.01).
fn angle(a: V, b: V) -> f32 {
    let (da, db) = (dot(a, a), dot(b, b));
    if !(da > f32::from_bits(0x38d1_b717)) || !(db > f32::from_bits(0x38d1_b717)) {
        return 0.0;
    }
    let c = dot(scale(a, rsqrt(da)), scale(b, rsqrt(db)));
    skate_core::trigonometry::acos(c.max(-1.0).min(1.0))
}
/// 82947200: angle from a to b in [0, 2pi) around `axis`.
fn signed_angle(a: V, b: V, axis: V) -> f32 {
    let (da, db) = (dot(a, a), dot(b, b));
    if !(da > f32::from_bits(0x38d1_b717)) || !(db > f32::from_bits(0x38d1_b717)) {
        return 0.0;
    }
    let (ua, ub) = (scale(a, rsqrt(da)), scale(b, rsqrt(db)));
    let value = skate_core::trigonometry::acos(dot(ua, ub).max(-1.0).min(1.0));
    if 0.0 > dot(cross(ua, ub), axis) { TAU - value } else { value }
}
/// 82847D08: signed angle of a to b measured in the plane normal to `axis`.
fn plane_angle(a: V, b: V, axis: V) -> f32 {
    let pa = sub(a, scale(axis, dot(axis, a)));
    let pb = sub(b, scale(axis, dot(axis, b)));
    if !(dot(a, a) * dot(b, b) > f32::from_bits(0x3780_0000)) {
        return 0.0;
    }
    signed_angle(pa, pb, axis)
}
/// The native (t / 2pi, fraction, minus one above one half, times 2pi) wrap into [-pi, pi].
fn wrap(angle: f32) -> f32 {
    let t = angle * INV_TAU;
    let f = t - t.floor();
    (if f > 0.5 { f - 1.0 } else { f }) * TAU
}

// ---- tuning -------------------------------------------------------------------------------

/// physics_state_offboard_pushing/default and the biped grab settings it reads.
#[derive(Clone, Debug)]
pub(crate) struct Settings {
    /// physics_state_offboard/default layout (BoardSettings) plus layout448.
    pub board: skate_core::player::offboard::ground_sync::BoardSettings,
    pub blend_duration_448: f32,
    pub pid_vector_3f0: [f32; 4],
    pub pid_scalar_440: [f32; 4],
    pub reach_96ec: f32,
    pub reset_out_4a4: f32,
    pub pose_error_out_f081: f32,
    pub tune_49c: f32,
    pub tune_4a0: f32,
    pub forward_force_2258: f32,
    pub backward_force_f1c0: f32,
    pub side_force_096a: f32,
    pub torque_limit_ad32: f32,
    pub force_limit_7914: f32,
    pub turn_threshold_557f: f32,
    pub turn_rate_42438: f32,
    pub vertical_fde8: f32,
    pub mass_curve_57d3: PointGraph<8>,
    pub inertia_curve_1cec: PointGraph<8>,
    pub inertia_curve_eabf: PointGraph<8>,
    pub rotation_distance_e4ff: PointGraph<8>,
    pub rotation_scale_bfb3: PointGraph<8>,
    pub lift_curve_702f: PointGraph<8>,
    pub lift_blend_1348: PointGraph<8>,
    pub lift_x_range: f32,
    pub yaw_gain_023d: PointGraph<8>,
    pub step_gain_4e91: PointGraph<8>,
}

impl Settings {
    pub(crate) fn load(data: &Collections, board: skate_core::player::offboard::ground_sync::BoardSettings) -> Result<Self, String> {
        const C: &str = "physics_state_offboard_pushing";
        let f = |name: &str| data.float(C, "default", name);
        let v4 = |name: &str| -> Result<[f32; 4], String> { Ok(data.words::<4>(C, "default", name)?.map(f32::from_bits)) };
        // Graph fields: 4 header words, 8 x, 8 y (82D19758 passes +0x10 and +0x30 to 8246FD40).
        let g = |name: &str| -> Result<(PointGraph<8>, f32), String> {
            let w = data.words::<20>(C, "default", name)?.map(f32::from_bits);
            Ok((PointGraph { x: w[4..12].try_into().unwrap(), y: w[12..20].try_into().unwrap() }, w[2]))
        };
        let (lift_curve_702f, range_702f) = g("Hash_702F25BA3A5AAA56")?;
        let (lift_blend_1348, range_1348) = g("Hash_1348E9A1F213B42D")?;
        Ok(Self {
            board,
            blend_duration_448: data.float("physics_state_offboard", "default", "Hash_1B8F17A8DA7B75AA")?,
            pid_vector_3f0: v4("Hash_DF79539DBDA006EE")?,
            pid_scalar_440: v4("Hash_B46764285AD1DC5F")?,
            reach_96ec: f("Hash_96ECC98838ECCC11")?,
            reset_out_4a4: f("Hash_B6730894F9DFE0B3")?,
            pose_error_out_f081: f("Hash_F081CA3EC9E51BFB")?,
            tune_49c: f("Hash_5E35DB02BE697A58")?,
            tune_4a0: f("Hash_02F03DA51050C8B3")?,
            forward_force_2258: f("Hash_2258076B612569A9")?,
            backward_force_f1c0: f("Hash_F1C038722EC7D0C6")?,
            side_force_096a: f("Hash_096A4FA6489E5541")?,
            torque_limit_ad32: f("Hash_AD327350D151B1E3")?,
            force_limit_7914: f("Hash_791421DDAF54C2D5")?,
            turn_threshold_557f: f("Hash_557FA142008FD7CE")?,
            turn_rate_42438: f("Hash_42438D4C6B209C09")?,
            vertical_fde8: f("Hash_FDE807D9B85A6AC2")?,
            mass_curve_57d3: g("Hash_57D37D696363167E")?.0,
            inertia_curve_1cec: g("Hash_1CECE1CC07C5CD94")?.0,
            inertia_curve_eabf: g("Hash_EABFCC79873A2859")?.0,
            rotation_distance_e4ff: g("Hash_E4FF0185DA44CDBD")?.0,
            rotation_scale_bfb3: g("Hash_BFB3BEF0BB2661C0")?.0,
            lift_x_range: if range_1348 - range_702f >= 0.0 { range_1348 } else { range_702f },
            lift_curve_702f,
            lift_blend_1348,
            yaw_gain_023d: g("Hash_023D73966B8669A5")?.0,
            step_gain_4e91: g("Hash_4E91B4A605E54CC2")?.0,
        })
    }
}

// ---- state --------------------------------------------------------------------------------

/// 82D232D0: per-lane PID with a low-pass filtered integral term.
#[derive(Clone, Copy, Debug, Default)]
struct Pid {
    gains: [f32; 4],
    accum: V,
    prev: V,
    filt: V,
    delta: V,
}
impl Pid {
    fn step(&mut self, input: V) -> V {
        let alpha = self.gains[3];
        self.filt = std::array::from_fn(|i| self.filt[i].mul_add(1.0 - alpha, input[i] * alpha));
        self.delta = sub(input, self.prev);
        self.prev = input;
        let term: V = std::array::from_fn(|i| {
            let t = input[i].mul_add(self.gains[0], self.filt[i] * self.gains[1]);
            self.delta[i].mul_add(self.gains[2], t)
        });
        self.accum = add(self.accum, term);
        self.accum
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct ScalarPid {
    gains: [f32; 4],
    accum: f32,
    prev: f32,
    filt: f32,
    delta: f32,
}

pub(crate) struct Pushing {
    pub settings: Settings,
    /// Frames (x, y, z axes, position): blend source 0x50, target 0x90, current 0xD0.
    frame_from_50: Frame,
    frame_target_90: Frame,
    frame_current_d0: Frame,
    hand_110: V,
    step_120: V,
    direction_140: V,
    back_170: V,
    back_180: V,
    back_190: V,
    root_1a0: V,
    left_hand_1b0: V,
    right_hand_1c0: V,
    left_from_1d0: V,
    left_to_1e0: V,
    left_1f0: V,
    right_from_200: V,
    right_to_210: V,
    right_220: V,
    ik_left_230: V,
    ik_right_240: V,
    frame_x_250: V,
    point_260: V,
    velocity_270: V,
    filtered_force_280: V,
    displacement_290: V,
    force_2a0: V,
    zero_2b0: V,
    descriptor_2c0: [u32; 2],
    record_2d0: Option<Record>,
    pid_3f0: Pid,
    pid_440: ScalarPid,
    length_460: f32,
    yaw_464: f32,
    distance_468: f32,
    ik_blend_46c: f32,
    effort_470: f32,
    grab_time_474: f32,
    mass_478: f32,
    inertia_47c: f32,
    mass_gain_480: f32,
    inertia_gain_484: f32,
    inertia_gain_488: f32,
    blend_t_48c: f32,
    blend_rate_490: f32,
    idle_494: f32,
    flag_time_498: f32,
    tune_49c: f32,
    tune_4a0: f32,
    out_4a4: f32,
    turn_rate_4a8: f32,
    length_4ac: f32,
    /// bit0 widened force (record word68), bit1 pose error, bit2 spline reversed, bit3 blending,
    /// bit4 displaced, bit5 grabbing, bit6 hand IK on, bit7 hand order fixed.
    flags_4b0: u8,
}

impl Pushing {
    /// Constructor 82D18CB8: the two PID gain blocks, then reset.
    pub(crate) fn new(settings: Settings) -> Self {
        let mut p = Self {
            frame_from_50: IDENTITY,
            frame_target_90: IDENTITY,
            frame_current_d0: IDENTITY,
            hand_110: ZERO,
            step_120: ZERO,
            direction_140: ZERO,
            back_170: ZERO,
            back_180: ZERO,
            back_190: ZERO,
            root_1a0: ZERO,
            left_hand_1b0: ZERO,
            right_hand_1c0: ZERO,
            left_from_1d0: ZERO,
            left_to_1e0: ZERO,
            left_1f0: ZERO,
            right_from_200: ZERO,
            right_to_210: ZERO,
            right_220: ZERO,
            ik_left_230: ZERO,
            ik_right_240: ZERO,
            frame_x_250: ZERO,
            point_260: ZERO,
            velocity_270: ZERO,
            filtered_force_280: ZERO,
            displacement_290: ZERO,
            force_2a0: ZERO,
            zero_2b0: ZERO,
            descriptor_2c0: [0; 2],
            record_2d0: None,
            pid_3f0: Pid { gains: settings.pid_vector_3f0, ..Default::default() },
            pid_440: ScalarPid { gains: settings.pid_scalar_440, ..Default::default() },
            length_460: 0.0,
            yaw_464: 0.0,
            distance_468: 0.0,
            ik_blend_46c: 0.0,
            effort_470: 0.0,
            grab_time_474: 0.0,
            mass_478: 0.0,
            inertia_47c: 0.0,
            mass_gain_480: 0.0,
            inertia_gain_484: 0.0,
            inertia_gain_488: 0.0,
            blend_t_48c: 0.0,
            blend_rate_490: 0.0,
            idle_494: 0.0,
            flag_time_498: 0.0,
            tune_49c: 0.0,
            tune_4a0: 0.0,
            out_4a4: 0.0,
            turn_rate_4a8: 0.0,
            length_4ac: 0.0,
            flags_4b0: 0,
            settings,
        };
        p.reset();
        p
    }

    /// 82D18F28.
    fn reset(&mut self) {
        let identity = [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], ZERO];
        self.frame_from_50 = identity;
        self.frame_target_90 = identity;
        self.frame_current_d0 = identity;
        self.descriptor_2c0 = [0; 2];
        self.record_2d0 = None;
        for v in [
            &mut self.hand_110, &mut self.step_120, &mut self.direction_140, &mut self.back_170,
            &mut self.root_1a0, &mut self.left_hand_1b0, &mut self.right_hand_1c0, &mut self.left_from_1d0,
            &mut self.left_to_1e0, &mut self.left_1f0, &mut self.right_from_200, &mut self.right_to_210,
            &mut self.right_220, &mut self.ik_left_230, &mut self.ik_right_240, &mut self.frame_x_250,
            &mut self.point_260, &mut self.velocity_270, &mut self.filtered_force_280,
            &mut self.displacement_290, &mut self.force_2a0, &mut self.zero_2b0,
        ] {
            *v = ZERO;
        }
        self.back_190 = [1.0, 0.0, 0.0, 0.0];
        self.pid_3f0 = Pid { gains: self.pid_3f0.gains, ..Default::default() };
        self.pid_440 = ScalarPid { gains: self.pid_440.gains, ..Default::default() };
        self.length_460 = 0.0;
        self.yaw_464 = 0.0;
        self.distance_468 = 0.0;
        self.ik_blend_46c = 0.0;
        self.effort_470 = 0.5;
        self.grab_time_474 = 0.0;
        self.mass_478 = 1.0;
        self.inertia_47c = 1.0;
        self.mass_gain_480 = 1.0;
        self.inertia_gain_484 = 1.0;
        self.inertia_gain_488 = 0.0;
        self.blend_t_48c = -1.0;
        self.blend_rate_490 = -1.0;
        self.idle_494 = 0.0;
        self.flag_time_498 = 0.0;
        self.tune_49c = 0.0;
        self.tune_4a0 = 0.0;
        self.out_4a4 = self.settings.reset_out_4a4;
        self.turn_rate_4a8 = 0.0;
        self.length_4ac = 0.0;
        self.flags_4b0 = 0;
    }
}

// ---- skater-side helpers ------------------------------------------------------------------

/// 82BBAB48: world frame of an animated bone (record pose under the animation root).
fn bone_world(skater: &SkaterRuntime, bone: usize) -> Frame {
    compose_affine(&skater.animated_skeleton.roots.animation_to_world, &skater.animated_skeleton.record.pose[bone])
}

fn processed_frame(skater: &SkaterRuntime) -> Frame {
    skater.player_input.processed.effective_anim_transform_192.map(|v| v.map(f32::from_bits))
}

/// The record's vector at a byte offset, and its setter (the native code edits its copy).
fn rv(r: &Record, offset: usize) -> V {
    r.vector(offset)
}
fn set_rv(r: &mut Record, offset: usize, v: V) {
    for i in 0..4 {
        r.0[offset / 4 + i] = v[i].to_bits();
    }
}

/// 82DDE818: the grab normal of segment a -> b: horizontal (perpendicular to the segment, on the
/// approach's side) unless the segment is nearly vertical, blending to the approach between the
/// two cosines (820ED620 = 0.999, 822F3C48 = 0.9999).
fn grab_normal(a: V, b: V, approach: V) -> V {
    let lo = f32::from_bits(0x3f7f_be77);
    let hi = f32::from_bits(0x3f7f_f972);
    let direction = unit(sub(b, a));
    let c = dot(direction, UP).abs();
    if c > lo {
        if c > hi {
            return approach;
        }
        let t = (recip(hi - lo) * (hi - c)).max(0.0).min(1.0);
        let mut n = unit(cross(direction, UP));
        if 0.0 > dot(approach, n) {
            n = neg(n);
        }
        return madd(n, t, scale(approach, 1.0 - t));
    }
    let n = unit(cross(direction, UP));
    if 0.0 > dot(approach, n) { neg(n) } else { n }
}

impl Pushing {
    /// Enter 82D19488.
    pub(crate) fn enter(&mut self, physics: &mut GamePhysics, skater: &mut SkaterRuntime) -> Result<(), String> {
        self.reset();
        self.root_1a0 = skater.animated_skeleton.board_frames.com_frame[3];
        super::biped_ground::services::enter_feet(skater);
        // SkateboardController (+0x4C, state448): a held, aligning or idle board is let go, a
        // board being fetched is hidden; a released or hidden board stays as it is.
        let state = skater.skateboard_controller.fields.state_448;
        if state <= 5 && !matches!(state, 2 | 3) {
            let observation = super::offboard::board_manager::runtime::observe(physics, skater);
            let mut effects = skater.board_possession_live.effects(
                physics,
                &mut skater.ground_lifecycle.board_animated_290,
                skater.player_input.processed.timestep_2604,
            );
            skater.skateboard_controller.fields.word_444 = 0;
            if state == 4 {
                skater.board_possession.hide(&observation, &mut effects);
                skater.skateboard_controller.fields.state_448 = 3;
            } else {
                skater.board_possession.let_go(&mut skater.skateboard_controller.fields, &observation, &mut effects);
                skater.skateboard_controller.fields.state_448 = 2;
            }
            skater.board_possession_live.publish_volumes(physics);
        }
        skater.ground_lifecycle.skeleton_controller.request(4, &mut skater.skeleton_collision)?;
        let p = &skater.player_input.processed;
        let first = &p.grab_records_1888_2176[0];
        if p.flags_2480 & 0x0040_0000 != 0 && first[49] != 0 && first[54] != 0 {
            self.record_2d0 = skater.offboard_grab.published_record(0);
            self.attach(skater, true);
            self.flags_4b0 |= 0x20;
            self.grab_frame(skater);
        }
        self.back_170 = neg(self.frame_target_90[2]);
        self.back_180 = self.back_170;
        self.back_190 = neg(self.frame_target_90[0]);
        self.tune_49c = self.settings.tune_49c;
        self.tune_4a0 = self.settings.tune_4a0;
        Ok(())
    }

    /// Exit 82D19B70: the skeleton leaves its partial-ragdoll collision set.
    pub(crate) fn exit(&mut self, skater: &mut SkaterRuntime) {
        let c = &mut skater.skeleton_collision;
        c.partial_ragdoll = false;
        c.parts[0].volume_group = 4;
        c.parts[0].enabled = false;
        c.disable_count[0] = 0;
        c.parts[24].enabled = false;
        c.parts[25].enabled = false;
    }

    /// 82D19658: bind the record; `new` re-derives its length, mass and inertia terms.
    fn attach(&mut self, skater: &SkaterRuntime, new: bool) {
        let Some(mut r) = self.record_2d0.take() else { return };
        self.descriptor_2c0 = [r.0[47], r.0[48]];
        self.hand_110 = bone_world(skater, 23)[3];
        if new {
            self.flags_4b0 = (self.flags_4b0 & !4) | if r.byte(200) & 0x20 != 0 { 4 } else { 0 };
            self.length_460 = r.scalar(176);
            self.mass_478 = 1.0 / r.scalar(240);
            self.mass_gain_480 = self.settings.mass_curve_57d3.evaluate(self.mass_478);
            self.inertia_47c = 1.0 / rv(&r, 224)[1];
            self.inertia_gain_484 = self.settings.inertia_curve_1cec.evaluate(self.inertia_47c);
            self.inertia_gain_488 = self.settings.inertia_curve_eabf.evaluate(self.inertia_47c);
            let (point, distance) = polyline::nearest_point(&r, self.hand_110);
            self.point_260 = flat(point);
            let mut margin = self.settings.board.margin_444;
            let half = self.length_460 * 0.5;
            if margin > half {
                margin = half;
            }
            self.distance_468 = (self.length_460 - margin).min(margin.max(distance));
            self.flags_4b0 &= 0x3f;
        }
        // The spline runs left to right as seen from the hand: otherwise swap its ends.
        let c = cross(sub(rv(&r, 64), self.hand_110), sub(rv(&r, 80), self.hand_110));
        if 0.0 > c[1] {
            let (start, end) = (rv(&r, 64), rv(&r, 80));
            set_rv(&mut r, 64, end);
            set_rv(&mut r, 80, start);
            let direction = neg(rv(&r, 112));
            set_rv(&mut r, 112, direction);
            let byte = r.byte(200) ^ 0x20;
            r.0[50] = (r.0[50] & 0x00ff_ffff) | (u32::from(byte) << 24);
        }
        let reversed = r.byte(200) & 0x20 != 0;
        if reversed != (self.flags_4b0 & 4 != 0) {
            self.distance_468 = self.length_460 - self.distance_468;
            self.flags_4b0 = (self.flags_4b0 & !4) | if reversed { 4 } else { 0 };
        }
        self.record_2d0 = Some(r);
        if new {
            self.grab_frame(skater);
            self.point_260 = flat(madd(self.back_170, self.settings.reach_96ec, self.point_260));
        }
    }

    /// 82D1AEE8: hands, the grab segment around the retained distance and the target frame.
    fn grab_frame(&mut self, skater: &SkaterRuntime) {
        let Some(r) = self.record_2d0.as_ref() else { return };
        self.left_hand_1b0 = bone_world(skater, 3)[3];
        self.right_hand_1c0 = bone_world(skater, 7)[3];
        let half = 0.5 * length(sub(self.left_hand_1b0, self.right_hand_1c0));
        let a = (self.distance_468 + half).max(0.0).min(self.length_460);
        let b = (self.distance_468 - half).max(0.0).min(self.length_460);
        let mut pa = polyline::at_distance(r, a);
        let mut pb = polyline::at_distance(r, b);
        let mut across = sub(pa, pb);
        if !(dot(across, across) >= f32::from_bits(0x38d1_b717)) {
            let to_hand = sub(pa, self.hand_110);
            across = if !(dot(to_hand, to_hand) >= f32::from_bits(0x38d1_b717)) {
                processed_frame(skater)[0]
            } else {
                cross(UP, to_hand)
            };
        }
        let mut right = unit(across);
        let side = cross(sub(pb, self.hand_110), sub(pa, self.hand_110));
        if 0.0 > side[1] {
            if self.flags_4b0 & 0x80 != 0 {
                std::mem::swap(&mut pa, &mut pb);
                right = neg(right);
            }
        } else {
            self.flags_4b0 |= 0x80;
        }
        let forward = neg(self.facing(pa, pb, rv(r, 96)));
        let centre = polyline::at_distance(r, self.distance_468);
        let up = cross(forward, right);
        self.frame_target_90 = [right, up, forward, centre];
        let inverse = inverse_rigid(&self.frame_target_90);
        self.left_to_1e0 = transform_point(&inverse, pa);
        self.right_to_210 = transform_point(&inverse, pb);
        self.direction_140 = unit(sub(flat(pa), flat(pb)));
    }

    /// 82D19A90: the grab normal, flipped to face away from the hand.
    fn facing(&self, pa: V, pb: V, approach: V) -> V {
        let n = grab_normal(pa, pb, approach);
        let mid = flat(sub(scale(add(pa, pb), 0.5), self.hand_110));
        if dot(n, mid) > 0.0 { neg(n) } else { n }
    }

    /// 82D1B3D0: start blending the current frame and hand points to the new target.
    fn blend_start(&mut self) {
        self.frame_from_50 = self.frame_current_d0;
        self.left_from_1d0 = self.left_1f0;
        self.right_from_200 = self.right_220;
        self.blend_t_48c = 1.0;
        let frames = self.settings.blend_duration_448 * f32::from_bits(0x426f_ffff);
        let rate = recip(frames) * length(sub(self.frame_target_90[3], self.frame_current_d0[3]));
        if rate >= 1.0 {
            self.blend_t_48c = 1.0;
            self.blend_rate_490 = -1.0 / rate;
            self.flags_4b0 |= 8;
        } else {
            self.blend_t_48c = -1.0;
            self.blend_rate_490 = -1.0;
            self.flags_4b0 &= !8;
        }
        self.blend_step();
    }

    /// 82D1B590.
    fn blend_step(&mut self) {
        if self.flags_4b0 & 8 != 0 {
            let t = (self.blend_t_48c + self.blend_rate_490).max(0.0).min(1.0);
            self.blend_t_48c = t;
            if t < self.blend_rate_490 * f32::from_bits(0x3a83_126f) {
                self.flags_4b0 &= !8;
                self.blend_t_48c = -1.0;
            }
        }
        if self.flags_4b0 & 8 != 0 {
            let t = self.blend_t_48c;
            self.frame_current_d0 = interpolate_affine(&self.frame_from_50, &self.frame_target_90, 1.0 - t);
            self.left_1f0 = madd(self.left_from_1d0, t, scale(self.left_to_1e0, 1.0 - t));
            self.right_220 = madd(self.right_from_200, t, scale(self.right_to_210, 1.0 - t));
        } else {
            self.frame_current_d0 = self.frame_target_90;
            self.left_1f0 = self.left_to_1e0;
            self.right_220 = self.right_to_210;
        }
    }
}

impl Pushing {
    /// 82D1B7C8: the hand IK targets, the ground offset, the push point and its velocity, the
    /// effort, the yaw and the skeleton step.
    fn targets(&mut self, skater: &SkaterRuntime) {
        let p = &skater.player_input.processed;
        self.ik_left_230 = transform_point(&self.frame_current_d0, self.left_1f0);
        let feet = skater.offboard_feet.scalars_288_to_296[2];
        let timer = p.state_timer_2664;
        self.ik_right_240 = transform_point(&self.frame_current_d0, self.right_220);
        let hands_flat = scale(add(flat(self.left_hand_1b0), flat(self.right_hand_1c0)), 0.5);
        let frame = processed_frame(skater);
        self.frame_x_250 = frame[0];
        let low = f32::from_bits(0xbccc_cccd);
        let high = f32::from_bits(0x3ccc_cccd);
        let lift = feet.max(low).min(high);
        let reach = self.settings.reach_96ec;
        let point = flat(madd(self.back_170, reach, self.frame_current_d0[3]));
        let moved = scale(sub(point, self.point_260), f32::from_bits(0x3e19_999a));
        self.velocity_270 = madd(self.velocity_270, f32::from_bits(0x3f59_999a), scale(moved, recip(DT)));
        self.point_260 = point;
        let hands = scale(add(self.left_hand_1b0, self.right_hand_1c0), 0.5);
        let body = sub(self.frame_current_d0[3], hands);
        let anchor = sub(self.point_260, scale(self.back_170, reach));
        let mut step = sub(anchor, hands_flat);
        let mut effort = body[1] * f32::from_bits(0xbdcc_cccd);
        if self.tune_49c > 0.0 && !(timer > self.settings.lift_x_range) {
            let blend = self.settings.lift_blend_1348.evaluate(timer);
            let rest = 1.0 - blend;
            let curve = self.settings.lift_curve_702f.evaluate(timer);
            let offset = [0.0, self.tune_4a0, self.tune_49c + f32::from_bits(0x3e00_0000), 0.0];
            let target = transform_point(&frame, offset);
            self.frame_x_250 = madd(self.frame_x_250, rest, scale(frame[0], blend));
            let weight = 1.0 - curve;
            let carried = weight * effort;
            let toward = sub(anchor, target);
            let lift_by = sub(target, hands);
            step = madd(step, rest, scale(toward, blend));
            effort = ((-lift_by[1]) * curve).mul_add(0.5, carried);
            step = flat(step);
            if weight > f32::from_bits(0x3dcc_cccd) {
                self.ik_left_230 = madd(self.ik_left_230, weight, scale(self.left_hand_1b0, curve));
                self.ik_right_240 = madd(self.ik_right_240, weight, scale(self.right_hand_1c0, curve));
                self.flags_4b0 |= 0x40;
            }
        }
        if timer < f32::from_bits(0x3dcc_cccd) {
            self.velocity_270 = ZERO;
        }
        self.effort_470 = (self.effort_470 + effort).max(0.0).min(1.0);
        let yaw_gain = self.settings.yaw_gain_023d.evaluate(self.grab_time_474);
        self.yaw_464 = wrap(plane_angle(self.frame_x_250, self.direction_140, UP)) * yaw_gain;
        let step_gain = self.settings.step_gain_4e91.evaluate(self.grab_time_474);
        let limited = clamp_length(scale(step, step_gain), self.settings.turn_rate_42438 * DT);
        let mut out = add(limited, [0.0, -lift, 0.0, 0.0]);
        if f32::from_bits(0x38d1_b717) > dot(out, out) {
            out = scale(out, f32::from_bits(0x3dcc_cccd));
        }
        self.step_120 = out;
    }

    /// 82D1A4D0: the push force (a PID on the wanted push velocity minus the prop's velocity) and
    /// the turning torque (a PID on the turn rate), from the move-object intents. Returns
    /// (force, torque) for the grabbed DMO.
    fn force_torque(&mut self, skater: &SkaterRuntime) -> Option<(V, V)> {
        let r = self.record_2d0.as_ref()?;
        let s = &self.settings;
        let extra = &skater.animation_input.extra;
        let forward_gain = s.forward_force_2258 * self.mass_gain_480;
        let backward_gain = s.backward_force_f1c0 * self.mass_gain_480;
        let side_gain = self.mass_gain_480 * s.side_force_096a;
        let object = rv(r, 160);
        let along = dot(neg(self.direction_140), sub(object, self.frame_current_d0[3]));
        let sign = if along >= 0.0 { 1.0 } else { -1.0 };
        let distance = along * sign;
        let push = extra.object_move_z;
        let side = extra.object_move_x;
        let rotation = (s.rotation_distance_e4ff.evaluate(distance) * sign).mul_add(push, extra.object_move_rotation);
        let rotation = if 1.0 - rotation >= 0.0 { rotation } else { 1.0 };
        let turn = -(s.rotation_scale_bfb3.evaluate(distance) * rotation * self.inertia_gain_488);
        let lever = along * turn;
        let forward = if push > 0.0 { push * forward_gain } else { push * backward_gain };
        let sideways = side * side_gain;
        let forward = forward + lever * 0.25;
        let torque_limit = s.torque_limit_ad32;
        let force_limit = s.force_limit_7914;
        let mut turning = true;
        if turn.abs() < f32::from_bits(0x3dcc_cccd) {
            let off = wrap(angle(neg(self.frame_target_90[2]), self.back_170));
            if off.abs() > s.turn_threshold_557f {
                self.back_170 = neg(self.frame_target_90[2]);
                self.back_190 = neg(self.frame_target_90[0]);
            }
            turning = false;
        } else {
            self.back_170 = neg(self.frame_target_90[2]);
            self.back_190 = neg(self.frame_target_90[0]);
        }
        self.length_4ac = 0.0;
        self.flags_4b0 &= !1;
        self.zero_2b0 = ZERO;
        let mut wanted = madd(neg(self.back_170), forward, scale(self.back_190, sideways));
        if r.0[68] != 0 {
            self.flags_4b0 |= 1;
            wanted = scale(wanted, 2.0);
            self.length_4ac = length(wanted);
        }
        let small = f32::from_bits(0x38d1_b717) > dot(wanted, wanted);
        self.idle_494 = if turning || !small { 0.0 } else { self.idle_494 + DT };
        if self.flags_4b0 & 0x10 != 0 {
            let d = scale(self.displacement_290, 0.5);
            let n = unit(d);
            let pv = dot(wanted, n);
            let pd = dot(d, n);
            wanted = flat(madd(n, pv.max(pd), sub(wanted, scale(n, pv))));
        }
        let error = sub(wanted, rv(r, 128));
        let pid = flat(self.pid_3f0.step(error));
        let limited = clamp_length(pid, force_limit);
        let delta = clamp_length(sub(limited, self.force_2a0), f32::from_bits(0x4080_0000));
        let force = add(self.force_2a0, delta);
        self.force_2a0 = force;
        self.filtered_force_280 = madd(self.filtered_force_280, f32::from_bits(0x3f66_6666), scale(force, f32::from_bits(0x3dcc_cccd)));
        self.pid_3f0.accum = force;
        let measured = signed_angle(self.back_180, neg(self.frame_target_90[2]), UP);
        self.back_180 = neg(self.frame_target_90[2]);
        let rate = wrap(measured) * f32::from_bits(0x426f_ffff);
        self.turn_rate_4a8 = rate.abs();
        let error = turn - rate;
        let pid = &mut self.pid_440;
        let [kp, ki, kd, alpha] = pid.gains;
        pid.delta = error - pid.prev;
        pid.filt = (1.0 - alpha).mul_add(pid.filt, alpha * error);
        pid.prev = error;
        let accum = kp.mul_add(error, ki.mul_add(pid.filt, kd * pid.delta)) + pid.accum;
        pid.accum = accum.max(-torque_limit).min(torque_limit);
        let torque = clamp_length([0.0, pid.accum, 0.0, 0.0], torque_limit);
        Some((force, torque))
    }
}

/// 82BB1050 / 82BB10F8 (TU3 82BD9728 / 97D0): a hand's external IK target, moved toward `target`
/// from its authored reparented target by at most `limit`, with the given blend.
fn hand_ik(skater: &mut SkaterRuntime, limb: usize, target: V, limit: f32, weight: f32) {
    let s = &skater.animated_skeleton;
    let original = transform_point(&s.roots.animation_to_world, s.targets[limb][3]);
    let hand = move_towards(target, original, limit);
    skater.foot_ik.state.external_targets[limb].world_position = hand;
    skater.foot_ik.state.limbs[limb].external_target_set = true;
    skater.foot_ik.state.limbs[limb].target_blend = weight;
}

impl Pushing {
    /// Update (slot 2) 82D19BC8.
    pub(crate) fn update(&mut self, physics: &mut GamePhysics, skater: &mut SkaterRuntime) -> Result<(), String> {
        let p = skater.player_input.processed;
        let pose_error = p.collision_pose_error_736.map(f32::from_bits);
        if dot(pose_error, pose_error) > f32::from_bits(0x3d23_d70b) {
            self.out_4a4 = self.settings.pose_error_out_f081;
            self.flags_4b0 |= 2;
        }
        // Our build tests 2484 bit 27, which its publication fills from collision byte 215
        // (82D895A0); this port's 2484 layout (TU3) carries that byte in bit 26.
        self.flag_time_498 = if p.flags_2484 & 0x0400_0000 != 0 { self.flag_time_498 + DT } else { 0.0 };
        super::biped_ground::services::update_feet(skater);
        // 82D66370(5) plus the legs: arm and leg collision volumes off for five frames.
        let collision = &mut skater.skeleton_collision;
        collision.disable_handplant_contacts(5);
        for part in [19, 15, 20, 16, 17, 21] {
            collision.pending_reenable = true;
            collision.disable_count[part] = 5;
            collision.parts[part].enabled = false;
        }
        let board = self.settings.board;
        let bounds = board_bounds(processed_frame(skater), board.offset_32, board.extent_0);
        let first = &p.grab_records_1888_2176[0];
        let ready = p.flags_2480 & 0x0040_0000 != 0 && first[49] != 0 && first[54] != 0;
        let grab = p.flags_2476 & 0x0040_0000 != 0;
        let angle_a = board.angle_452 * DEG;
        let angle_b = board.angle_436 * DEG;
        let valid = ready
            && self
                .record_2d0
                .as_ref()
                .is_some_and(|r| grab_scene::qualify_at(r, self.hand_110, self.distance_468, bounds, angle_a, angle_b));
        let error = skater.collision_extra_errors[1];
        self.displacement_290 = madd(self.displacement_290, f32::from_bits(0x3f73_3333), scale(error, 0.5));
        let displaced = dot(self.displacement_290, self.displacement_290) > f32::from_bits(0x37d1_b717);
        self.flags_4b0 = (self.flags_4b0 & !0x10) | if displaced { 0x10 } else { 0 };
        let mut grabbing = false;
        if grab && !(self.flag_time_498 > f32::from_bits(0x3ecc_cccd)) && self.flags_4b0 & 2 == 0 {
            if ready && valid {
                self.record_2d0 = skater.offboard_grab.published_record(0);
                self.attach(skater, false);
                self.grab_frame(skater);
                self.blend_step();
                grabbing = true;
            } else {
                self.grab_time_474 = 0.0;
                if skater.offboard_grab.flags_12836 & 0x40 != 0 {
                    let current = Descriptor { kind: self.descriptor_2c0[0], id: self.descriptor_2c0[1] };
                    let limits = BoardLimits { margin: board.margin_444, angle_a, angle_b };
                    if let Some(candidate) = skater.offboard_grab.best_other(self.hand_110, current) {
                        if grab_scene::qualify(&candidate, self.hand_110, bounds, limits) && candidate.0[49] != 0 && candidate.0[54] != 0 {
                            self.record_2d0 = Some(candidate);
                            self.attach(skater, true);
                            self.velocity_270 = ZERO;
                            self.grab_frame(skater);
                            if self.flags_4b0 & 0x20 != 0 {
                                self.blend_start();
                            }
                            grabbing = true;
                        }
                    }
                }
            }
        }
        if std::env::var_os("PUSH_TRACE").is_some() {
            eprintln!(
                "PUSH grab {grab} ready {ready} valid {valid} grabbing {grabbing} t498 {:.3} f4b0 {:#x} rec {} w49 {} w54 {} hand {:?}",
                self.flag_time_498, self.flags_4b0, self.record_2d0.is_some(), first[49], first[54], self.hand_110
            );
        }
        self.flags_4b0 = (self.flags_4b0 & !0x20) | if grabbing { 0x20 } else { 0 };
        if self.flags_4b0 & 0x20 != 0 {
            self.targets(skater);
            let pushed = self.force_torque(skater);
            let step = f32::from_bits(0x3e4c_cccd);
            self.ik_blend_46c = if self.flags_4b0 & 0x40 != 0 {
                (self.ik_blend_46c + step).min(1.0)
            } else {
                (self.ik_blend_46c - step).max(0.0)
            };
            let reach = f32::from_bits(0x3f26_6666);
            hand_ik(skater, 2, self.ik_left_230, reach, self.ik_blend_46c);
            hand_ik(skater, 3, self.ik_right_240, reach, self.ik_blend_46c);
            let mut target = madd(self.back_170, reach, self.frame_target_90[3]);
            target[1] = processed_frame(skater)[3][1] + f32::from_bits(0x3f38_51ec);
            let moved = move_towards(sub(target, self.root_1a0), scale(self.velocity_270, DT), f32::from_bits(0x3dcc_cccd));
            self.root_1a0 = add(self.root_1a0, moved);
            root_step(physics, skater, self.step_120, self.yaw_464, self.root_1a0)?;
            if let Some((force, torque)) = pushed {
                physics.dmos.push(self.descriptor_id(), force, torque);
            }
            // Vtable+0x2C / +0x28: keep the bound spline's data flowing (processed record 0).
            if let Some(r) = &self.record_2d0 {
                skater.offboard_grab.request_primary(r.descriptor());
            }
            self.grab_time_474 += DT;
        } else {
            root_step(physics, skater, ZERO, 0.0, self.root_1a0)?;
        }
        let p = &skater.player_input.processed;
        skater.offboard_grab.query(Query {
            position: self.hand_110,
            sort_position: self.hand_110,
            bounds,
            limits: BoardLimits { margin: board.margin_444, angle_a, angle_b },
            mode: 4,
            capacity: 5,
            context: QueryContext { selection_flags_2948: p.actor_query_2948, matching_id_2952: p.actor_query_2952 as i32 },
        });
        Ok(())
    }

    fn descriptor_id(&self) -> u32 {
        self.record_2d0.as_ref().map_or(self.descriptor_2c0[1], |r| r.0[48])
    }

    /// Fill 82D1C188 (+ shared Feet Fill 82D4D720).
    pub(crate) fn fill(&self, skater: &mut SkaterRuntime) -> super::biped_ground::lifecycle::StatePublication {
        let feet = &skater.offboard_feet;
        let hand_flags = feet.hands.map(|f| (f.flags_104_to_107[1] && !f.flags_104_to_107[2]) || f.flags_104_to_107[3]);
        let physical = &mut skater.player_input.physical;
        let off = &mut physical.off_board;
        off.locomotion_state_84 = 0;
        off.flag_304 = (self.flags_4b0 >> 5) & 1;
        let forward = self.frame_current_d0[2];
        let left = neg(self.frame_current_d0[0]);
        off.moving_object_0_12 = [
            dot(self.velocity_270, forward),
            dot(self.velocity_270, left),
            dot(self.filtered_force_280, forward),
            dot(self.filtered_force_280, left),
        ];
        off.flag_332 = self.flags_4b0 & 1;
        off.scalar_20 = self.length_4ac;
        off.scalar_16 = self.turn_rate_4a8;
        off.scalar_28 = self.effort_470;
        off.scalar_24 = self.mass_478;
        off.flags_306_307 = hand_flags.map(u8::from);
        physical.ground.scalar_296 = self.out_4a4;
        // State+0x24 / +0x28 (descriptor kind / id): applied after the shared state reset.
        let (kind, id) = self.record_2d0.as_ref().map_or((0, 0), |r| (r.0[47], r.0[48]));
        super::biped_ground::lifecycle::StatePublication { word_36: kind, word_40: id, flag_86: false }
    }

    /// Post 82D1C120: the shared off-board collision wipeout check (82D65220), then request 25
    /// while the feet manager's flag305 is set.
    pub(crate) fn post(&self, skater: &mut SkaterRuntime, collision: skate_core::player::offboard::ground_lifecycle::CollisionInput<'_>) {
        skate_core::player::offboard::ground_lifecycle::check_collision(
            &mut skater.wipeout.state,
            skater.biped_ground.collision_settings(),
            &collision,
        );
        if skater.offboard_feet.flags_304_to_307[1] {
            skater.wipeout.state.request(25, 0.0);
        }
    }
}

/// 82BB6B90: the animated skeleton root while pushing: the skate root from the old root, the COM
/// toward the root target (at most 0.3 per update), then 82BB8980's root (the world-up corrected
/// root, turned by `yaw` and moved by `delta` in animation space, limited against the collision
/// pose error), then the general update.
fn root_step(physics: &mut GamePhysics, skater: &mut SkaterRuntime, delta: V, yaw: f32, root: V) -> Result<(), String> {
    let s = &mut skater.animated_skeleton;
    s.board_frames.skate_root = compose_affine(&s.roots.animation_to_world, &s.record.pose[0]);
    let current = add(s.board_frames.com_frame[3], delta);
    let com = move_towards(root, current, f32::from_bits(0x3e99_999a));
    let old = s.roots.animation_to_world;
    s.board_frames.update_com_lift(&old, com, 0.0);
    // 82BB8980
    let w2a = s.roots.world_to_animation;
    let t = madd(w2a[2], delta[2], madd(w2a[1], delta[1], scale(w2a[0], delta[0])));
    let corrected = world_up_correction(s.roots.animation_to_world);
    let (sin, cos) = skate_core::trigonometry::sin_cos(yaw);
    let turn: Frame = [[cos, 0.0, -sin, cos], [0.0, 1.0, 0.0, 0.0], [sin, 0.0, cos, sin], [t[0], t[1], t[2], t[3]]];
    let mut frame = orthonormalize(compose_affine(&corrected, &turn));
    let error = skater.collision_pose_error;
    let limit = length(error);
    if limit > f32::from_bits(0x3c23_d70a) {
        let direction = scale(error, recip(limit));
        let moved = sub(frame[3], old[3]);
        let projection = dot(direction, moved);
        if limit > projection {
            let (base, extra) = if 0.0 > projection { (limit, -projection) } else { (limit - projection, 0.0) };
            frame[3] = add(old[3], madd(direction, base.mul_add(f32::from_bits(0x3e4c_cccd), extra), moved));
        }
    }
    s.motion.trajectory = IDENTITY;
    let packet = &mut skater.animation.packet;
    if let Some(first) = packet.hierarchy.first_mut() {
        *first = IDENTITY;
    }
    if let Some(first) = packet.local.first_mut() {
        *first = IDENTITY;
    }
    s.roots.reset_initial_alignment(frame);
    let p = &mut skater.player_input.processed;
    p.flags_2468 |= 0x80000;
    let target = compose_affine(&s.roots.animation_to_world, &skater.skeleton_input.drive_frames[0]);
    s.board_frames.animation_target = target;
    if p.flags_2480 & 0x0001_0000 != 0 {
        s.board_frames.physical_board = skater.skeleton_air.apply_board(&mut physics.board, &target, true);
    }
    let collision = super::input_phase::collision(skater);
    let mut owners = super::skeleton_input_runtime::SkeletonOwners {
        animated: &mut skater.animated_skeleton,
        body: &mut skater.skeleton,
        drives: &mut skater.skeleton_drives,
        ik: &mut skater.foot_ik,
        animation_input: &mut skater.animation_input,
        correction: &mut skater.skeleton_output.correction,
        pose_errors: &mut skater.pose_errors,
    };
    skater.skeleton_input.general_update(
        &skater.player_input.processed,
        &mut owners,
        &skater.animation.packet.hierarchy,
        &collision,
        physics.settings.step.simulation,
    )?;
    owners.animated.finish_ground();
    owners.animation_input.fields.flags2468 = skater.player_input.processed.flags_2468;
    Ok(())
}

// ---- state entry points (the owner lives in SkaterRuntime; it is moved out for each call) ----

fn with<R>(skater: &mut SkaterRuntime, f: impl FnOnce(&mut Pushing, &mut SkaterRuntime) -> R) -> Result<R, String> {
    let mut owner = skater.pushing.take().ok_or("OffBoardPushing state owner is busy")?;
    let result = f(&mut owner, skater);
    skater.pushing = Some(owner);
    Ok(result)
}
pub(crate) fn enter(physics: &mut GamePhysics, skater: &mut SkaterRuntime) -> Result<(), String> {
    with(skater, |p, s| p.enter(physics, s))?
}
pub(crate) fn exit(skater: &mut SkaterRuntime) {
    let _ = with(skater, |p, s| p.exit(s));
}
pub(crate) fn update(physics: &mut GamePhysics, skater: &mut SkaterRuntime) -> Result<(), String> {
    with(skater, |p, s| p.update(physics, s))?
}
pub(crate) fn fill(skater: &mut SkaterRuntime) -> Result<super::biped_ground::lifecycle::StatePublication, String> {
    with(skater, |p, s| p.fill(s))
}
pub(crate) fn post(skater: &mut SkaterRuntime, collision: skate_core::player::offboard::ground_lifecycle::CollisionInput<'_>) -> Result<(), String> {
    with(skater, |p, s| p.post(s, collision))
}
