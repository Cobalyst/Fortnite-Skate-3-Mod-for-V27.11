//! Hall of Meat scoring: Skate 3 `Score::HoM::WipeoutScorer` (TU3 ctor 0x82D81FE0, per-frame
//! 0x82D83620), driven by the skeleton's per-part collision forces.
//!
//! Recovered from the retail code:
//! * 25 HoM bones. Bone b < 23 reads skeleton part b+1's contact force; bone 23 (face) reads
//!   part 1's specific force and bone 24 (groin) part 23's (0x82D83AD8, specific contacts of
//!   SkeletonCollisionFeedback). Each frame: amount = force * 2.0 * damage scale; above 0.1 it
//!   is added to the bone (0x82D83820): current += amount, peak = max(peak, current), and it
//!   spreads to up to four linked bones (amount * link factor, again only above 0.1).
//! * Every frame current *= 0.8 (0x82D84308); the bone's level is the highest of its six
//!   (points, threshold) slots whose threshold is between 0 and the peak.
//! * Body damage score = sum of the reached slot's points over all bones (0x82D82578); the
//!   six injury subtotals are the same sum per slot (0x82D82498). Total = damage + bonuses
//!   (0x82D82B60).
//! * Bail start: both feet take damage from the landing graph (0x82D82C78), the toes the same
//!   scaled again.
//! The bonus inputs (rotation in degrees, body speed, air time, drop, bail time) are fed into
//! the stock graphs whose axes match them; the retail layout order of those graphs is not
//! recoverable from the converted vault, so they are chosen by their units.
use skate_data::collections::Collections;

const CLASS: &str = "Hash_9E4C471BC6EAB615";
const KEY: &str = "default";
const DT: f32 = 1.0 / 60.0;
/// 0x820641A0: minimum damage that counts / spreads.
const MIN_DAMAGE: f32 = 0.1;
/// 0x82060C50: contact force to damage.
const FORCE_TO_DAMAGE: f32 = 2.0;
/// 0x8208821C: per-frame decay of the running damage.
const DECAY: f32 = 0.8;

pub const BONE_NAMES: [&str; 25] = [
    "Skull", "Neck", "Left Hand", "Left Forearm", "Left Arm", "Left Shoulder", "Right Hand",
    "Right Forearm", "Right Arm", "Right Shoulder", "Upper Back", "Middle Back", "Lower Back",
    "Tailbone", "Left Toes", "Left Foot", "Left Shin", "Left Thigh", "Right Toes", "Right Foot",
    "Right Shin", "Right Thigh", "Pelvis", "Face", "Groin",
];
/// Injury slot names (ID_HOM_SCRAPED .. ID_HOM_BREAK; slot 5 is the bones' last "break").
pub const LEVEL_NAMES: [&str; 6] = ["Scraped", "Bruised", "Sprained", "Dislocated", "Broken", "Broken"];

#[derive(Clone, Copy, Debug, Default)]
struct BoneDef {
    links: [(i32, f32); 4],
    levels: [(u32, f32); 6], // (points, damage threshold)
}

#[derive(Clone, Copy, Debug)]
struct Graph {
    xs: [f32; 8],
    ys: [f32; 8],
}
impl Graph {
    fn eval(&self, x: f32) -> f32 {
        if x <= self.xs[0] {
            return self.ys[0];
        }
        for i in 1..8 {
            if x <= self.xs[i] {
                let span = self.xs[i] - self.xs[i - 1];
                let t = if span > 0.0 { (x - self.xs[i - 1]) / span } else { 1.0 };
                return self.ys[i - 1] + (self.ys[i] - self.ys[i - 1]) * t;
            }
        }
        self.ys[7]
    }
}

#[derive(Clone, Debug)]
pub struct Tuning {
    bones: [BoneDef; 25],
    damage_scale: f32,
    toe_scale: f32,
    landing: Graph,
    rotation: Graph,
    speed: Graph,
    air: Graph,
    drop: Graph,
    time: Graph,
}

fn hex(data: &str) -> Result<Vec<u8>, String> {
    (0..data.len() / 2)
        .map(|i| u8::from_str_radix(&data[2 * i..2 * i + 2], 16).map_err(|e| e.to_string()))
        .collect()
}
fn be_u32(b: &[u8], o: usize) -> u32 {
    u32::from_be_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
fn be_f32(b: &[u8], o: usize) -> f32 {
    f32::from_bits(be_u32(b, o))
}

impl Tuning {
    pub fn load(data: &Collections) -> Result<Self, String> {
        let raw = |name: &str| -> Result<Vec<u8>, String> { hex(&data.field(CLASS, KEY, name)?.data) };
        let graph = |name: &str| -> Result<Graph, String> {
            let b = raw(name)?;
            if b.len() < 80 {
                return Err(format!("HoM graph {name} too short"));
            }
            let mut g = Graph { xs: [0.0; 8], ys: [0.0; 8] };
            for i in 0..8 {
                g.xs[i] = be_f32(&b, 16 + 4 * i);
                g.ys[i] = be_f32(&b, 48 + 4 * i);
            }
            Ok(g)
        };
        let set = raw("Hash_1F9B5EA147CBEEE9")?;
        if set.len() < 25 * 0x54 {
            return Err("HoM bone set too short".into());
        }
        let mut bones = [BoneDef::default(); 25];
        for (b, def) in bones.iter_mut().enumerate() {
            let r = b * 0x54;
            for k in 0..4 {
                def.links[k] = (be_u32(&set, r + 4 + 8 * k) as i32, be_f32(&set, r + 8 + 8 * k));
            }
            for k in 0..6 {
                def.levels[k] = (be_u32(&set, r + 0x24 + 8 * k), be_f32(&set, r + 0x28 + 8 * k));
            }
        }
        Ok(Self {
            bones,
            damage_scale: data.float(CLASS, KEY, "Hash_9C494E9614FA5ED6")?,
            toe_scale: data.float(CLASS, KEY, "Hash_72A3CCC3C75697CD")?,
            landing: graph("Hash_C2D28EFB70B37684")?,
            rotation: graph("Hash_2E61C1BB9D8A8A95")?,
            speed: graph("Hash_A9B81D5BF44762E4")?,
            air: graph("Hash_96CDCC590988982E")?,
            drop: graph("Hash_B2A997F8579B1998")?,
            time: graph("Hash_95AE883C533FC1EB")?,
        })
    }
}

#[derive(Clone, Copy, Debug)]
struct BoneState {
    current: f32,
    peak: f32,
    level: i32,
}
impl Default for BoneState {
    fn default() -> Self {
        Self { current: 0.0, peak: 0.0, level: -1 }
    }
}

/// What the scorer reports to the HUD / audio.
#[derive(Clone, Debug)]
pub enum Event {
    /// A bone reached a new injury level during the bail.
    Injury { bone: usize, level: usize, points: u32 },
    /// The bail ended: result lines (label, points) and the total.
    Results { lines: Vec<(String, u32)>, total: u32 },
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Live {
    /// A bail is being scored right now.
    pub active: bool,
    /// There is a bail to show (current or the last one).
    pub shown: bool,
    pub score: f32,
    pub metrics: [f32; 5],
    pub points: [f32; 5],
}

pub struct Input<'a> {
    pub wiping_out: bool,
    pub forces: [f32; 24],
    pub specific_forces: [f32; 24],
    pub any_contact: bool,
    /// Hips position (m, y up) and velocity (m/s), angular velocity (rad/s).
    pub hips_position: [f32; 3],
    pub hips_velocity: [f32; 3],
    pub hips_spin: [f32; 3],
    pub _marker: std::marker::PhantomData<&'a ()>,
}

pub struct HallOfMeat {
    tuning: Tuning,
    bones: [BoneState; 25],
    active: bool,
    frames: u32,
    rotation: f32,
    speed: f32,
    air_time: f32,
    air_start_height: f32,
    max_drop: f32,
    airborne: bool,
    pub events: Vec<Event>,
    pub max_force: f32,
    /// Downward hips speed (m/s) over the last three ticks: Skate 3 keeps the feet's landing
    /// value for three frames after touchdown (0x82D83EF8) and applies it if a bail starts then.
    recent_fall: [f32; 3],
    was_in_air: bool,
}

impl HallOfMeat {
    pub fn new(tuning: Tuning) -> Self {
        Self {
            tuning,
            bones: [BoneState::default(); 25],
            active: false,
            frames: 0,
            rotation: 0.0,
            speed: 0.0,
            air_time: 0.0,
            air_start_height: 0.0,
            max_drop: 0.0,
            airborne: false,
            events: Vec::new(),
            max_force: 0.0,
            recent_fall: [0.0; 3],
            was_in_air: false,
        }
    }

    fn add_damage(&mut self, amount: f32, bone: usize, from: usize, depth: u32) {
        let state = &mut self.bones[bone];
        state.current += amount;
        state.peak = state.peak.max(state.current);
        if depth > 6 {
            return;
        }
        let links = self.tuning.bones[bone].links;
        for (link, factor) in links {
            if link < 0 || link as usize >= 25 || link as usize == from {
                continue;
            }
            let spread = factor * amount;
            if spread > MIN_DAMAGE {
                self.add_damage(spread, link as usize, bone, depth + 1);
            }
        }
    }

    fn level_of(&self, bone: usize) -> i32 {
        let peak = self.bones[bone].peak;
        for k in (0..6).rev() {
            let threshold = self.tuning.bones[bone].levels[k].1;
            if threshold > 0.0 && threshold < peak {
                return k as i32;
            }
        }
        -1
    }

    fn begin(&mut self, input: &Input) {
        self.bones = [BoneState::default(); 25];
        self.frames = 0;
        self.rotation = 0.0;
        self.max_drop = 0.0;
        self.air_time = 0.0;
        self.airborne = !input.any_contact;
        self.air_start_height = input.hips_position[1];
        let v = input.hips_velocity;
        self.speed = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
        // Landing impact on the feet (0x82D82C78): only when the bail starts at a touchdown (the
        // feet's landing value lasts three frames), as km/h through the landing graph.
        let landing = if input.any_contact {
            self.recent_fall.iter().copied().fold(0.0, f32::max) * 3.6
        } else {
            0.0
        };
        let foot = self.tuning.landing.eval(landing) * self.tuning.damage_scale;
        if foot > MIN_DAMAGE {
            for (foot_bone, toe_bone) in [(15, 14), (19, 18)] {
                self.add_damage(foot, foot_bone, foot_bone, 0);
                self.add_damage(foot * self.tuning.toe_scale, toe_bone, toe_bone, 0);
            }
        }
        self.max_force = 0.0;
    }

    fn finish(&mut self) {
        let mut subtotal = [0u32; 6];
        let mut count = [0u32; 6];
        for b in 0..25 {
            let level = self.bones[b].level;
            if level >= 0 {
                let points = self.tuning.bones[b].levels[level as usize].0;
                subtotal[level as usize] += points;
                count[level as usize] += 1;
            }
        }
        let damage: u32 = subtotal.iter().sum();
        let t = &self.tuning;
        let bonuses = [
            ("Rotation", t.rotation.eval(self.rotation) as u32),
            ("Speed", t.speed.eval(self.speed) as u32),
            ("Air", t.air.eval(self.air_time) as u32),
            ("Drop", t.drop.eval(self.max_drop) as u32),
            ("Time", t.time.eval(self.frames as f32 * DT) as u32),
        ];
        let bonus: u32 = bonuses.iter().map(|b| b.1).sum();
        let total = damage + bonus;
        let mut lines = Vec::new();
        // Display order as the results screen: Broken, Dislocated, Sprained, Bruised, Scraped.
        let broken = (subtotal[4] + subtotal[5], count[4] + count[5]);
        for (name, (points, n)) in [
            ("Broken", broken),
            ("Dislocated", (subtotal[3], count[3])),
            ("Sprained", (subtotal[2], count[2])),
            ("Bruised", (subtotal[1], count[1])),
            ("Scraped", (subtotal[0], count[0])),
        ] {
            if n > 0 {
                lines.push((format!("{name} (x{n})"), points));
            }
        }
        lines.push(("Body Damage".to_string(), damage));
        for (name, points) in bonuses {
            if points > 0 {
                lines.push((name.to_string(), points));
            }
        }
        self.events.push(Event::Results { lines, total });
    }

    /// One 60 Hz physics tick.
    pub fn update(&mut self, input: &Input) {
        // Touchdown tracking for the landing damage (kept even outside bails).
        let falling = if self.was_in_air { (-input.hips_velocity[1]).max(0.0) } else { 0.0 };
        self.recent_fall = [falling, self.recent_fall[0], self.recent_fall[1]];
        self.was_in_air = !input.any_contact;
        if input.wiping_out && !self.active {
            self.active = true;
            self.begin(input);
        }
        if !input.wiping_out && self.active {
            self.active = false;
            self.finish();
        }
        if !self.active {
            return;
        }
        self.frames += 1;
        // Contact damage per bone (0x82D83AD8).
        for b in 0..25 {
            let force = match b {
                23 => input.specific_forces[1],
                24 => input.specific_forces[23],
                _ => input.forces[b + 1],
            };
            self.max_force = self.max_force.max(force);
            let amount = force * FORCE_TO_DAMAGE * self.tuning.damage_scale;
            if amount > MIN_DAMAGE {
                self.add_damage(amount, b, b, 0);
            }
        }
        // Levels and decay (0x82D84308).
        for b in 0..25 {
            let level = self.level_of(b);
            if level > self.bones[b].level {
                self.bones[b].level = level;
                let points = self.tuning.bones[b].levels[level as usize].0;
                self.events.push(Event::Injury { bone: b, level: level as usize, points });
            }
            self.bones[b].current *= DECAY;
        }
        // Bonus measures.
        let w = input.hips_spin;
        self.rotation += (w[0].abs() + w[1].abs() + w[2].abs()) * DT * 57.29578;
        let v = input.hips_velocity;
        if input.any_contact {
            self.airborne = false;
        } else {
            if !self.airborne {
                self.airborne = true;
                self.air_start_height = input.hips_position[1];
            }
            self.air_time += DT;
            self.max_drop = self.max_drop.max(self.air_start_height - input.hips_position[1]);
        }
    }

    /// Live numbers for the Hall of Meat HUD (homscoring.apt): the running total and the five
    /// metrics in the movie's order (speed m/s, drop m, air s, bail time s, rotation degrees)
    /// with their points. Kept after the bail ends until the next one starts.
    pub fn live(&self) -> Live {
        let t = &self.tuning;
        let mut damage = 0u32;
        for b in 0..25 {
            let level = self.bones[b].level;
            if level >= 0 {
                damage += t.bones[b].levels[level as usize].0;
            }
        }
        let bail_time = self.frames as f32 * DT;
        let metrics = [self.speed, self.max_drop, self.air_time, bail_time, self.rotation];
        let points = [
            t.speed.eval(self.speed),
            t.drop.eval(self.max_drop),
            t.air.eval(self.air_time),
            t.time.eval(bail_time),
            t.rotation.eval(self.rotation),
        ];
        Live {
            active: self.active,
            shown: self.frames > 0,
            score: damage as f32 + points.iter().map(|p| p.floor()).sum::<f32>(),
            metrics,
            points,
        }
    }

    /// Current bone levels (-1 = unhurt), for display.
    pub fn levels(&self) -> [i32; 25] {
        let mut out = [-1; 25];
        for b in 0..25 {
            out[b] = self.bones[b].level;
        }
        out
    }
}
