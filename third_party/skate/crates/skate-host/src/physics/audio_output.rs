//! Skate 3's physics-side audio publication, for the Fortnite mod's port of the skater/board audio
//! layer. Retail TU3: FillAudio 0x8274F410 (audio PhysOut bundle 10) with helpers 0x8274FAE0 (board
//! side slip), 0x8274FCA0 (wheel touchdowns), 0x8274FE90 (grind contact latch), 0x8274FF60 (body
//! region block, four-frame maximum), the skeleton collision publisher 0x82BAD9F0 (region audio
//! force) and the per-skater record fill 0x8277E688 (flag bits the audio layer reads).
//! Only fields the ported sounds consume are produced; every value follows the retail formula.
use super::{GamePhysics, SkaterRuntime};
use skate_core::physics::board::BodyId;

/// What the mod's audio port reads each frame (mirrors the retail per-skater record fields).
#[derive(Clone, Copy, Debug, Default)]
pub struct AudioFrame {
    pub tick: u64,
    /// Record 0x94 bit31 (Air+438): air category (200..299) and no wheel in contact.
    pub air: bool,
    /// Record 0x94 bit25 (Audio+0xD0): grind category (400), or state 701 with Grinds+323.
    pub grinding: bool,
    /// Record 0x94 bit24: a trick is current (Tricks+0x98 != -1).
    pub trick_active: bool,
    /// Current trick: the scoring identifier index (-1 none). The mod reads the trick table's
    /// audio fields for it.
    pub trick_id: i32,
    /// Record 0x98 bit5: State byte 80 (processed flags2484 bit21).
    pub state_flag_80: bool,
    /// Record 0x78: Air+0xB0, time in the air state.
    pub airtime: f32,
    /// Record 0x84 as the audio layer keeps it (obj+0x1D4): the pop strength (Audio+0x18) of the
    /// last tick with Air+440 (flags2468 bit22, the animation's "jump" moment).
    pub pop_strength: f32,
    /// Record 0x68: Motion+0xA4, board ground speed.
    pub ground_speed: f32,
    /// Audio+0x44..0x47 / +0x34..0x40: wheel touchdown flags and strengths.
    pub wheel_flag: [bool; 4],
    pub wheel_impact: [f32; 4],
    /// Audio+0x1C / +0x20 / +0x28: grind contact family, impact speed, audio surface (latched).
    pub grind_family: i32,
    pub grind_force: f32,
    pub grind_surface: i32,
    /// Audio+0x14 / +0x10: board side slip and its rise.
    pub side_slip: f32,
    pub side_slip_rise: f32,
    /// Audio+0x5C block (record 0x140): region audio force (max of the last four frames),
    /// tangent speed and material (0 none, else material+1), and the group stats.
    pub region_force: [f32; 8],
    pub region_tangent: [f32; 8],
    pub region_material: [u32; 8],
    pub skater_force: f32,
    pub group_8_force: f32,
    /// State bundle bytes the board audio reads (record 0x94 bits 28/27/30/29, 0xA0 bit31, 0x94
    /// bit26): bit0 = State52, bit1 = State54, bit2 = State55 (landing family), bit3 = State56,
    /// bit4 = State57, bit5 = State60 (manual balance).
    pub state_flags: u8,
    /// Collision wheel_count_0 (record 0x98 bits 20..22 -> audio obj +0xC8).
    pub wheel_count: u8,
    /// |Reckoning+0x10| (record 0x20 -> audio obj +0x60): physical centre-of-mass speed.
    pub com_speed: f32,
    /// Ground+0x108 (processed+0xA74, -Turn): the carve weight's steering factor.
    pub carve_turn: f32,
    /// Motion+0xB8 (board+0x100): truck steering deck tilt (squeak trigger).
    pub deck_tilt: f32,
    /// Audio+0 z: deck-local angular velocity about the deck's At axis (Motion+0x40 in deck space).
    pub deck_roll_rate: f32,
    /// Skeleton bytes 600 / 601 (record 0x94 bits 7 / 6 -> audio obj +0x267 / +0x268): each foot
    /// within the deck box. bit0 = 600, bit1 = 601.
    pub feet_on_deck: u8,
    /// Wheel world positions (skate space, metres): the seam grid samples x and z.
    pub wheel_position: [[f32; 3]; 4],
    /// Audio obj +0x30C (record 0x9C bits 5..6, record filler 0x8277F5D0): the deck scraping the
    /// ground in a wipeout or off the board. 1 = upside down (deck up . ground normal < -0.9),
    /// 2 = on its side (|dot| < 0.1), 0 = none.
    pub board_slide: u8,
}

pub(crate) struct AudioOutput {
    /// physics_collision field @0xA4 (Hash_1430BD50F0A33475): region audio force scale.
    region_scale: f32,
    /// board audio (C26949FC "default") 3D399FE04952B425: wheel touchdown speed scale.
    wheel_scale: f32,
    /// side slip (C1831BDB / BA9837A6CF4C26ED): 56D931D202881C2D, E6C3FFE8AA70F944.
    slip_scale: f32,
    slip_offset: f32,
    wheel_prev_contact: [bool; 4],
    wheel_prev_velocity: [[f32; 3]; 4],
    wheel_air_frames: [i32; 4],
    wheel_flag: [bool; 4],
    wheel_impact: [f32; 4],
    slip_ring: [f32; 4],
    slip_index: usize,
    slip_average: f32,
    region_ring: [[f32; 8]; 4],
    region_index: usize,
    grind_family: i32,
    grind_surface: i32,
    grind_force: f32,
    pop_strength: f32,
    pub frame: AudioFrame,
}

impl AudioOutput {
    pub fn load(data: &skate_data::collections::Collections) -> Self {
        let f = |class: &str, key: &str, name: &str, fallback: f32| data.float(class, key, name).unwrap_or(fallback);
        Self {
            region_scale: f("physics_collision", "default", "Hash_1430BD50F0A33475", 10.0),
            wheel_scale: f("Hash_C26949FCB638A2CA", "default", "Hash_3D399FE04952B425", 9.0),
            slip_scale: f("Hash_C1831BDB6CB1B1EA", "Hash_BA9837A6CF4C26ED", "Hash_56D931D202881C2D", 45.0),
            slip_offset: f("Hash_C1831BDB6CB1B1EA", "Hash_BA9837A6CF4C26ED", "Hash_E6C3FFE8AA70F944", -0.75),
            wheel_prev_contact: [false; 4],
            wheel_prev_velocity: [[0.0; 3]; 4],
            wheel_air_frames: [0; 4],
            wheel_flag: [false; 4],
            wheel_impact: [0.0; 4],
            slip_ring: [0.0; 4],
            slip_index: 0,
            slip_average: 0.0,
            region_ring: [[0.0; 8]; 4],
            region_index: 0,
            grind_family: 0,
            grind_surface: 0,
            grind_force: 0.0,
            pop_strength: 0.0,
            frame: AudioFrame::default(),
        }
    }

    pub fn update(&mut self, tick: u64, physics: &GamePhysics, skater: &SkaterRuntime) {
        let p = &skater.player_input.physical;
        let processed = &skater.player_input.processed;
        let ground = &physics.riding.ground;
        let bodies = physics.board.bodies();
        let mut out = AudioFrame { tick, trick_id: -1, ..AudioFrame::default() };

        // Air+438 (0x82D8C934..C96C): air category and no board contact.
        let category = p.state.category_12;
        out.air = (200..300).contains(&category) && ground.wheel_contact_count == 0;
        // Audio+0xD0 (0x8274FE90).
        out.grinding = category == 400 || (p.state.state_16 == 701 && p.grinds.flag_323 != 0);
        out.state_flag_80 = processed.flags_2484 & (1 << 21) != 0;
        out.airtime = p.air.time_in_state_176;
        out.ground_speed = p.skateboard.scalar_164;

        // Pop strength (0x8274F410): |Air+0x70| capped (2.65 -> 1.0) else scaled by 1/2.65.
        let jump = p.air.jump_velocity_delta_112.map(f32::from_bits);
        let speed = (jump[0] * jump[0] + jump[1] * jump[1] + jump[2] * jump[2]).sqrt();
        let pop = if speed > 2.65 { 1.0 } else { speed * f32::from_bits(0x3EC1_3521) };
        if processed.flags_2468 & (1 << 22) != 0 {
            self.pop_strength = pop;
        }
        out.pop_strength = self.pop_strength;

        // Current trick (Tricks+0x98): the animation's trick descriptor, as the scorer resolves it.
        let score = &skater.animation.motion.score_packet;
        if let Some(name) = score.trick_names.first.or_else(|| score.grab.map(|g| g.0))
            && let Some(d) = skater.scoring.data.by_name(name)
        {
            out.trick_id = d.metadata.id as i32;
            out.trick_active = true;
        }

        // Wheel touchdowns (0x8274FCA0): a wheel back in contact after more than five frames off
        // the ground; strength = |contact normal . previous wheel velocity| / scale.
        for w in 0..4 {
            let part = &ground.parts[w];
            let contact = part.in_contact;
            if !self.wheel_prev_contact[w] && self.wheel_air_frames[w] > 5 {
                self.wheel_flag[w] = false;
            }
            if contact && !self.wheel_prev_contact[w] && self.wheel_air_frames[w] > 5 {
                let n = part.normal;
                let v = self.wheel_prev_velocity[w];
                let d = (n.x * v[0] + n.y * v[1] + n.z * v[2]).abs();
                self.wheel_impact[w] = (d / self.wheel_scale).min(1.0);
                self.wheel_flag[w] = true;
            }
            self.wheel_prev_contact[w] = contact;
            let lv = bodies[w].rates.linear_velocity;
            self.wheel_prev_velocity[w] = [lv.x, lv.y, lv.z];
            self.wheel_air_frames[w] = if contact { 0 } else { self.wheel_air_frames[w] + 1 };
        }
        out.wheel_flag = self.wheel_flag;
        out.wheel_impact = self.wheel_impact;

        // Board side slip (0x8274FAE0).
        if ground.wheel_contact_count > 0 {
            let deck = physics.board.part_transforms()[BodyId::Deck.index()].basis;
            let side = deck.columns[0];
            let v = bodies[BodyId::Deck.index()].rates.linear_velocity;
            let lateral = (side[0] * v.x + side[1] * v.y + side[2] * v.z).abs();
            out.side_slip = ((lateral - self.slip_offset).max(0.0) / self.slip_scale).min(1.0);
            self.slip_ring[self.slip_index] = lateral;
            let average = self.slip_ring.iter().sum::<f32>() * 0.25;
            out.side_slip_rise = (average - self.slip_average).max(0.0);
            self.slip_average = average;
            self.slip_index = (self.slip_index + 1) % 4;
        }

        // Grind contact latch (0x8274FE90): on Grinds+316 latch the family and audio surface;
        // the impact speed when positive.
        if p.grinds.grinding_316 != 0 {
            self.grind_family = p.grinds.words_136_140[0] as i32;
            self.grind_surface = p.grinds.audio_surface_216 as i32;
        }
        if p.grinds.impact_speed_128 > 0.0 {
            self.grind_force = p.grinds.impact_speed_128;
        }
        out.grind_family = self.grind_family;
        out.grind_force = self.grind_force;
        out.grind_surface = self.grind_surface;

        // Skeleton region block (0x82BAD9F0 final loop + 0x8274FF60).
        let feedback = &skater.collision_feedback;
        let record = &skater.skeleton.record;
        let weights = &skater.skeleton.definition.animation_masses.part_weights;
        let mut forces = [0.0f32; 8];
        for (i, region) in feedback.regions.iter().enumerate() {
            match region.part {
                Some(part) if part < 24 => {
                    let dv = record.velocity_changes[part];
                    let n = region.normal;
                    let along = (dv[0] * n[0] + dv[1] * n[1] + dv[2] * n[2]).abs();
                    forces[i] = (along * weights[part] * self.region_scale).max(0.001).clamp(0.0, 1.0);
                    out.region_tangent[i] = region.tangent_speed;
                }
                _ => {
                    forces[i] = 0.0;
                    out.region_tangent[i] = 0.0;
                }
            }
            out.region_material[i] = region.material_flags;
        }
        self.region_ring[self.region_index] = forces;
        self.region_index = (self.region_index + 1) % 4;
        for i in 0..8 {
            out.region_force[i] = self.region_ring.iter().map(|r| r[i]).fold(0.0, f32::max);
        }
        let flags = &skater.player_state.state_flags;
        let bit = |offset: usize, b: u8| if flags[offset - 52] { 1u8 << b } else { 0 };
        out.state_flags = bit(52, 0) | bit(54, 1) | bit(55, 2) | bit(56, 3) | bit(57, 4) | bit(60, 5);
        out.wheel_count = ground.wheel_contact_count as u8;
        let com_velocity = p.reckoning.vector_16.map(f32::from_bits);
        out.com_speed = (com_velocity[0] * com_velocity[0] + com_velocity[1] * com_velocity[1]
            + com_velocity[2] * com_velocity[2]).sqrt();
        out.carve_turn = -skater.animation_input.fields.turn;
        out.deck_tilt = skater.ground.steering.deck_tilt;
        let transforms = physics.board.part_transforms();
        let at = transforms[BodyId::Deck.index()].basis.columns[2];
        let w = p.skateboard.vector_64.map(f32::from_bits);
        out.deck_roll_rate = at[0] * w[0] + at[1] * w[1] + at[2] * w[2];
        out.feet_on_deck = u8::from(p.skeleton.flag_600 != 0) | (u8::from(p.skeleton.flag_601 != 0) << 1);
        for w in 0..4 {
            let t = transforms[w].translation;
            out.wheel_position[w] = [t.x, t.y, t.z];
        }
        // Board slide: (State59 wiping, or an off-board state 500..599) with the deck in contact.
        // The record also needs a valid deck material (< 0x5E); Fortnite's ground always is one.
        {
            let wiping = skater.player_state.state_flags[59 - 52];
            let offboard = (500..600).contains(&p.state.state_16);
            if (wiping || offboard) && ground.parts[BodyId::Deck.index()].in_contact {
                let up = transforms[BodyId::Deck.index()].basis.columns[1];
                let n = p.ground.vector_80.map(f32::from_bits);
                let d = up[0] * n[0] + up[1] * n[1] + up[2] * n[2];
                out.board_slide = if d < -0.9 { 1 } else if d > -0.1 && d < 0.1 { 2 } else { 0 };
            }
        }
        out.skater_force = feedback.maximum_skater_force;
        out.group_8_force = feedback.maximum_group_8_force;

        self.frame = out;
    }
}

/// Scoring identifier (trick table key name) of a trick id from `AudioFrame::trick_id`.
pub fn trick_identifier(id: i32) -> &'static str {
    usize::try_from(id)
        .ok()
        .and_then(|i| skate_core::scoring::catalog::IDENTIFIERS.get(i))
        .map_or("", |entry| entry.0)
}
