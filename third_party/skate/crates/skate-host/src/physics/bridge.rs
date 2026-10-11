use super::{GamePhysics, PlayerControls, SkaterRuntime};
use crate::{camera::CameraRuntime, graph_runtime::StockGraphs, input::ControllerInput};
use bevy::prelude::*;
use skate_data::skate_map::{Collision, Geometry, Rail, SkateMap};
use std::path::Path;

#[derive(Clone, Copy, Default)]
pub struct Controls {
    pub buttons: u16,
    pub triggers: [u8; 2],
    pub left: [i16; 2],
    pub right: [i16; 2],
}
pub struct Session {
    physics: GamePhysics,
    skater: SkaterRuntime,
    controls: PlayerControls,
    graphs: StockGraphs,
    input: ControllerInput,
    camera: CameraRuntime,
    markers: crate::session_marker::Runtime,
    hud_events: Vec<HudEvent>,
    last_completed: f32,
    /// Hall of Meat scorer (runs while the hall_of_meat option is on).
    hom: Option<super::hall_of_meat::HallOfMeat>,
    /// Skate 3's physics-side audio publication (the mod's audio port reads it).
    audio: Option<super::audio_output::AudioOutput>,
    audio_tick: u64,
}
pub use super::dmo::{DmoGrabSpline, DmoSpec, DmoVolume};

/// An object-dropper DMO's pose after the last tick (skate space): model frame rotation columns,
/// origin, and whether it is simulating.
#[derive(Clone, Copy, Debug)]
pub struct DmoPose {
    pub id: u32,
    pub rotation: [[f32; 3]; 3],
    pub position: [f32; 3],
    pub awake: bool,
    /// The skater holds it (OffBoardPushing 502 with the pushing state grabbing: Fill
    /// 82D1C188 publishes OffBoard+304 and the bound spline's descriptor id in State+40).
    pub grabbed: bool,
}

impl Session {
    /// Replaces the object-dropper DMOs (rigid bodies in the board's island, see dmo.rs).
    pub fn set_dmos(&mut self, specs: Vec<DmoSpec>) -> Result<(), String> {
        self.physics.set_dmos(specs)
    }
    /// The action overlay's Upright Object on DMO `id` (see Dmos::start_upright).
    pub fn upright_dmo(&mut self, id: u32) -> bool {
        self.physics.dmos.start_upright(id)
    }
    /// Where every DMO is now.
    pub fn dmo_poses(&self) -> Vec<DmoPose> {
        let physical = &self.skater.player_input.physical;
        // Descriptor kind 2 ids are (DMO index + 1) << 8 | spline (dmo.rs grab_objects).
        let held = (physical.state.state_16 == 502 && physical.off_board.flag_304 != 0)
            .then(|| (physical.state.skitch_value_40 >> 8) as usize)
            .and_then(|n| n.checked_sub(1));
        self.physics
            .dmos
            .items
            .iter()
            .enumerate()
            .map(|(i, d)| {
                let m = d.model_pose();
                DmoPose {
                    id: d.spec.id,
                    rotation: [m.x_axis.truncate().to_array(), m.y_axis.truncate().to_array(), m.z_axis.truncate().to_array()],
                    position: m.w_axis.truncate().to_array(),
                    awake: d.awake,
                    grabbed: held == Some(i),
                }
            })
            .collect()
    }
}

/// Trickline HUD event, recorded after every physics tick so none are missed.
#[derive(Clone, Debug)]
pub struct HudEvent {
    /// 1 new trick, 2 current trick changed, 3 bail, 4 line banked (value = points),
    /// 5 footstep (value = strength), 6 foot contact (text push/brake, value = push speed)
    pub kind: u8,
    pub text: String,
    pub value: f32,
}
/// Trickline HUD state after the last tick.
#[derive(Clone, Copy, Debug, Default)]
pub struct Hud {
    /// Points of the trick sequence in progress (already multiplied).
    pub sequence: f32,
    /// Points banked into the current line so far.
    pub line: f32,
    /// Line timer, 0..1 (the line ends when it runs out).
    pub line_timer: f32,
    pub multiplier: f32,
    /// Every line completed this session.
    pub total: f32,
    /// Seconds left on the line timer.
    pub line_seconds: f32,
    /// Landing quality of the last sequence.
    pub clean: bool,
    pub sketchy: bool,
    /// Stance of the current trick.
    pub switch: bool,
    pub fakie: bool,
    /// A trick sequence is in progress (its score still counting).
    pub sequence_active: bool,
    /// Deck angular velocity in the deck's own frame (rad/s): x side, y up, z along the board.
    pub board_spin: [f32; 3],
    /// Hall of Meat: a bail is being scored / there is one to show, running total, and the
    /// metrics (speed m/s, drop m, air s, bail time s, rotation degrees) with their points.
    pub hom_active: bool,
    pub hom_shown: bool,
    pub hom_score: f32,
    pub hom_metrics: [f32; 5],
    pub hom_points: [f32; 5],
    /// Hall of Meat bone injury levels (-1 unhurt, 0 scraped .. 5 broken) for the x-ray.
    pub hom_levels: [i8; 25],
    /// Session marker prompt (LB held): shown, can set here, can go to, a marker exists,
    /// go-to hold progress 0..1, and counters of placements / refusals / returns.
    pub marker: crate::session_marker::HudState,
}
pub struct Pose {
    pub root: Mat4,
    pub bones: Vec<Mat4>,
    pub names: Vec<String>,
    pub camera: Option<(Vec3, Mat3, f32)>,
    pub velocity: Vec3,
    pub tick: u64,
    pub state: String,
}
impl Session {
    pub fn new(
        root: &Path,
        triangles: Vec<[[f32; 3]; 3]>,
        rails: Vec<Vec<[f32; 3]>>,
        spawn: [f32; 3],
        heading: f32,
    ) -> Result<Self, String> {
        let started = std::time::Instant::now();
        eprintln!("IW4L_SKATE_LOAD begin");
        skate_data::input_config::StockGameplayConfig::load(root).map_err(|e| e.to_string())?;
        let assets = skate_data::GameAssets::load(root).map_err(|e| e.to_string())?;
        let graphs = StockGraphs::load(root, &assets)?;
        let map = collision_map(triangles, rails, spawn, heading);
        eprintln!("IW4L_SKATE_LOAD graphs {}ms", started.elapsed().as_millis());
        let physics = GamePhysics::load_with_map(root, Some(&map))?;
        eprintln!(
            "IW4L_SKATE_LOAD physics {}ms",
            started.elapsed().as_millis()
        );
        let skater = SkaterRuntime::load(root, &graphs, &physics, super::test_difficulty().key())?;
        eprintln!("IW4L_SKATE_LOAD skater {}ms", started.elapsed().as_millis());
        Ok(Self {
            physics,
            skater,
            controls: PlayerControls::load(root)?,
            graphs,
            input: ControllerInput::default(),
            camera: CameraRuntime::load(root)?,
            markers: crate::session_marker::Runtime::load(root)?,
            hud_events: Vec::new(),
            last_completed: 0.,
            audio: skate_data::collections::Collections::load(root)
                .ok()
                .map(|data| super::audio_output::AudioOutput::load(&data)),
            audio_tick: 0,
            hom: match skate_data::collections::Collections::load(root)
                .and_then(|data| super::hall_of_meat::Tuning::load(&data))
            {
                Ok(tuning) => Some(super::hall_of_meat::HallOfMeat::new(tuning)),
                Err(error) => {
                    eprintln!("Hall of Meat tuning unavailable: {error}");
                    None
                }
            },
        })
    }
    /// A builder for collision to swap in later, usable on another thread.
    pub fn collision_builder(&self) -> CollisionBuilder {
        CollisionBuilder {
            material: self.physics.floor_material(),
        }
    }
    /// Swaps in collision built by `collision_builder`: the world the skater
    /// rides, climbs and grinds from the next tick.
    pub fn install_collision(&mut self, prepared: PreparedCollision) -> Result<(), String> {
        self.physics
            .install_world(prepared.world, std::sync::Arc::clone(&prepared.grind))?;
        self.skater.trajectory.bind_grind_world(prepared.grind);
        Ok(())
    }
    pub fn period(&self) -> f32 {
        self.physics.period().as_secs_f32()
    }
    pub fn set_aspect_ratio(&mut self, aspect_ratio: f32) {
        if aspect_ratio.is_finite() && aspect_ratio > 0. {
            self.camera.set_aspect_ratio(aspect_ratio);
        }
    }
    /// Eagerly decode immutable animation banks before a map is ready.
    pub fn preload(root: &Path) -> Result<(), String> {
        crate::skater_animation::AnimationSource::load(root).map(|_| ())
    }
    /// Reuse the complete world and animation session. The original teleport path
    /// resets physical bodies and animation state at the new MW2 position.
    pub fn activate(&mut self, spawn: [f32; 3], heading: f32) -> Result<Pose, String> {
        self.input = ControllerInput::default();
        self.markers.suspend();
        if self.physics.ticks == 0 {
            self.tick(Controls::default())?;
        }
        let mut transform = Mat4::from_rotation_translation(
            Quat::from_rotation_y(heading),
            Vec3::from_array(spawn),
        )
        .to_cols_array_2d();
        transform[3][3] = 0.;
        self.skater.travel_to(transform)?;
        for _ in 0..4 {
            self.tick(Controls::default())?;
        }
        self.input = ControllerInput::default();
        Ok(self.pose())
    }
    /// Gives the board and the skater's body a velocity (skate space, m/s), e.g. the
    /// running speed of a player stepping onto the board.
    pub fn add_velocity(&mut self, velocity: [f32; 3]) {
        let add = |v: &mut skate_core::math::Vector3| {
            v.x += velocity[0];
            v.y += velocity[1];
            v.z += velocity[2];
        };
        for body in self.physics.board.bodies_mut().iter_mut() {
            add(&mut body.rates.linear_velocity);
        }
        add(&mut self.physics.board.hook_mut().body.rates.linear_velocity);
        for body in self.skater.skeleton.bodies_mut().iter_mut() {
            add(&mut body.rates.linear_velocity);
        }
    }
    pub fn collect(&mut self, frame: InputFrame, dt: f32) {
        self.input.collect(frame.samples);
        self.markers.collect_time(f64::from(dt));
    }
    pub fn suspend_input(&mut self) {
        self.input = ControllerInput::default();
        self.markers.suspend();
    }
    pub fn advance(&mut self) -> Result<(), String> {
        self.input.publish_actions();
        self.advance_published()?;
        self.record_hud();
        self.audio_tick += 1;
        if let Some(audio) = self.audio.as_mut() {
            audio.update(self.audio_tick, &self.physics, &self.skater);
        }
        Ok(())
    }
    /// Skate 3's physics-side audio data after the last tick.
    pub fn audio(&self) -> super::audio_output::AudioFrame {
        self.audio.as_ref().map(|a| a.frame).unwrap_or_default()
    }
    /// Hall of Meat: feed the scorer this tick's skeleton contact forces and publish its
    /// injuries (kind 7: text "bone|level", value points) and results (kind 8: text lines,
    /// value total).
    fn record_hall_of_meat(&mut self) {
        let enabled = crate::options::HALL_OF_MEAT.load(std::sync::atomic::Ordering::Relaxed);
        let Some(hom) = self.hom.as_mut() else { return };
        let state = format!("{:?}", self.skater.player_state.current());
        let feedback = &self.skater.collision_feedback;
        let mut forces = [0.0f32; 24];
        let mut specific = [0.0f32; 24];
        for part in 0..24 {
            forces[part] = feedback.bones[part].force;
            specific[part] = feedback.bones[part].specific_force;
        }
        let hips = &self.skater.skeleton.bodies()[23].rates;
        let input = super::hall_of_meat::Input {
            wiping_out: enabled && state.contains("Wipeout"),
            forces,
            specific_forces: specific,
            any_contact: feedback.current[1..].iter().any(|&c| c),
            hips_position: [hips.position.x, hips.position.y, hips.position.z],
            hips_velocity: [hips.linear_velocity.x, hips.linear_velocity.y, hips.linear_velocity.z],
            hips_spin: [hips.angular_velocity.x, hips.angular_velocity.y, hips.angular_velocity.z],
            _marker: std::marker::PhantomData,
        };
        hom.update(&input);
        for event in hom.events.drain(..) {
            match event {
                super::hall_of_meat::Event::Injury { bone, level, points } => self.hud_events.push(HudEvent {
                    kind: 7,
                    text: format!(
                        "{}|{}",
                        super::hall_of_meat::BONE_NAMES[bone],
                        super::hall_of_meat::LEVEL_NAMES[level]
                    ),
                    value: points as f32,
                }),
                super::hall_of_meat::Event::Results { lines, total } => {
                    for (label, points) in lines {
                        self.hud_events.push(HudEvent { kind: 8, text: label, value: points as f32 });
                    }
                    self.hud_events.push(HudEvent { kind: 9, text: "Total".into(), value: total as f32 });
                }
            }
        }
    }
    /// Body impacts (kind 10: text = contact region 0-5, value = that region's contact force this
    /// tick). Skate 3's skater audio (0x824AA020) reads the strongest contact of the first six
    /// skeleton contact regions every frame (head, torso, left/right arm, left/right leg) and plays
    /// the region's body-material collision sound; classification, cooldown and volume are done on
    /// the sound side with the game's own material records.
    fn record_bail_impacts(&mut self) {
        let feedback = &self.skater.collision_feedback;
        for (region, contact) in feedback.regions.iter().take(6).enumerate() {
            if contact.force > 0.0 {
                self.hud_events.push(HudEvent { kind: 10, text: region.to_string(), value: contact.force });
            }
        }
    }
    fn record_hud(&mut self) {
        self.record_hall_of_meat();
        self.record_bail_impacts();
        let scoring = &self.skater.scoring;
        if scoring.new_trick || scoring.modified_trick {
            let [switch, fakie, nollie, _] = scoring.stance;
            let prefix = if switch {
                "Switch "
            } else if nollie {
                "Nollie "
            } else if fakie {
                "Fakie "
            } else {
                ""
            };
            self.hud_events.push(HudEvent {
                kind: if scoring.new_trick { 1 } else { 2 },
                text: format!("{prefix}{}", scoring.trick_name),
                value: scoring.sequence_score,
            });
        }
        if scoring.close_tricks {
            let reasons = self.skater.wipeout.last_reasons.lock().map(|mut l| std::mem::take(&mut *l)).unwrap_or_default();
            self.hud_events.push(HudEvent { kind: 3, text: reasons, value: 0. });
        }
        // Audible footsteps / push strokes (Skate 3's AudibleFootStepStrength animation attribute).
        let step = std::mem::take(&mut self.skater.animation_input.footstep_latch);
        if step > 0.0 {
            self.hud_events.push(HudEvent { kind: 5, text: String::new(), value: step });
        }
        let push = std::mem::take(&mut self.skater.animation_input.push_latch);
        if push > 0.0 {
            self.hud_events.push(HudEvent { kind: 6, text: "push".into(), value: push });
        }
        if std::mem::take(&mut self.skater.animation_input.brake_latch) {
            self.hud_events.push(HudEvent { kind: 6, text: "brake".into(), value: 0.0 });
        }
        let completed = self.skater.scoring.session.holder.snapshot.completed_lines;
        if completed > self.last_completed + 0.5 {
            self.hud_events.push(HudEvent {
                kind: 4,
                text: String::new(),
                value: completed - self.last_completed,
            });
        }
        self.last_completed = completed;
        // Bounded in case nobody drains them.
        if self.hud_events.len() > 256 {
            self.hud_events.drain(..128);
        }
    }
    /// Trickline HUD state after the last tick.
    pub fn hud(&self) -> Hud {
        let scoring = &self.skater.scoring;
        let capacity = scoring.data.line_capacity.max(1.);
        Hud {
            sequence: scoring.sequence_score,
            line: scoring.session.holder.snapshot.line,
            line_timer: (scoring.session.line.points / capacity).clamp(0., 1.),
            multiplier: scoring.session.combo.multiplier,
            total: scoring.session.holder.snapshot.completed_lines,
            line_seconds: scoring.session.line.seconds(scoring.data.line_drain.max(1e-6)),
            clean: scoring.clean,
            sketchy: scoring.sketchy,
            switch: scoring.stance[0],
            fakie: scoring.stance[1],
            sequence_active: scoring.sequence_active,
            board_spin: {
                let deck = &self.physics.board.bodies()[6].rates;
                let w = deck.angular_velocity;
                let c = &deck.basis.columns;
                let dot = |a: &[f32; 3]| a[0] * w.x + a[1] * w.y + a[2] * w.z;
                [dot(&c[0]), dot(&c[1]), dot(&c[2])]
            },
            ..{
                let live = self.hom.as_ref().map(|h| h.live()).unwrap_or_default();
                Hud {
                    hom_active: live.active,
                    hom_shown: live.shown,
                    hom_score: live.score,
                    hom_metrics: live.metrics,
                    hom_points: live.points,
                    hom_levels: self.hom.as_ref().map(|h| h.levels().map(|l| l as i8)).unwrap_or([-1; 25]),
                    marker: self.markers.hud(),
                    ..Default::default()
                }
            }
        }
    }
    /// HUD events since the last call.
    pub fn take_hud_events(&mut self) -> Vec<HudEvent> {
        std::mem::take(&mut self.hud_events)
    }
    /// Every trick label the scoring data knows (diagnostics).
    pub fn trick_labels(&self) -> Vec<(String, String)> {
        self.skater
            .scoring
            .data
            .definitions
            .iter()
            .map(|d| (d.identifier.to_string(), d.label.clone()))
            .collect()
    }
    fn advance_published(&mut self) -> Result<(), String> {
        let published = self.input.tick_input();
        self.markers
            .advance(&self.input, &self.physics, &mut self.skater);
        let mut actions = published.actions();
        self.controls.update_for_physics(
            &mut actions,
            &self.physics,
            &self.skater,
            &self.camera,
        )?;
        self.controls.publish_gestures(
            self.physics.animation_profile.physics_mode,
            self.skater.player_input.physical.state.state_16,
        );
        super::frame::advance(
            &mut self.physics,
            &mut self.skater,
            &mut self.controls,
            &self.graphs,
            &mut actions,
            published.controller_available(),
            &mut self.camera,
        )
    }
    /// Deterministic raw-packet entry point for playback/diagnostics.
    pub fn tick(&mut self, input: Controls) -> Result<(), String> {
        crate::input::sample(
            &mut self.input,
            skate_core::input::xbox::XboxState {
                buttons: input.buttons,
                triggers: input.triggers,
                left: input.left,
                right: input.right,
            },
        );
        self.advance_published()
    }
    pub fn pose(&self) -> Pose {
        let v = self.physics.board.bodies()[skate_core::physics::board::BodyId::Deck.index()]
            .rates
            .linear_velocity;
        Pose {
            root: crate::animation::native_matrix(
                self.skater.animated_skeleton.roots.animation_to_world,
            ),
            bones: self
                .skater
                .render_pose
                .iter()
                .map(|m| crate::animation::native_matrix(*m))
                .collect(),
            names: self.skater.animation.evaluator.frames.bone_names.clone(),
            camera: self.camera.frame.as_ref().map(|f| {
                (
                    Vec3::new(f.position[0], f.position[1], f.position[2]),
                    Mat3::from_cols_array_2d(&f.basis.columns),
                    f.field_of_view_degrees,
                )
            }),
            velocity: Vec3::new(v.x, v.y, v.z),
            tick: self.physics.ticks,
            state: format!("{:?}", self.skater.player_state.current()),
        }
    }
}

/// Collision for `Session::install_collision`, built off the simulation.
pub struct PreparedCollision {
    world: skate_core::physics::board_world::BoardWorld,
    grind: std::sync::Arc<crate::grind_world::StaticProvider>,
}

#[derive(Clone, Copy)]
pub struct CollisionBuilder {
    material: skate_core::physics::contact::RetailContactMaterial,
}

/// A Skate 3 collision triangle carried unchanged from the game's own data (a placed object-dropper
/// object): its RW ClusteredMesh edge codes and 16-bit surface id (audio | physics << 7 | pattern << 12).
#[derive(Clone, Copy, Debug)]
pub struct NativeTriangle {
    pub points: [[f32; 3]; 3],
    pub edges: [u8; 3],
    pub surface: u16,
}

impl CollisionBuilder {
    pub fn build(
        &self,
        triangles: Vec<[[f32; 3]; 3]>,
        rails: Vec<Vec<[f32; 3]>>,
    ) -> Result<PreparedCollision, String> {
        self.build_with_native(triangles, rails, Vec::new())
    }

    /// As `build`, plus native triangles: their edge codes go through the game's own decoder
    /// (skate_world::decode_native_edges) and their surface id is packed exactly as authored.
    pub fn build_with_native(
        &self,
        triangles: Vec<[[f32; 3]; 3]>,
        rails: Vec<Vec<[f32; 3]>>,
        native: Vec<NativeTriangle>,
    ) -> Result<PreparedCollision, String> {
        let mut map = collision_map(triangles, rails, [0.; 3], 0.);
        if !native.is_empty() {
            // One material per distinct surface id, so the packed surface (audio | physics << 7 |
            // pattern << 12) reproduces the id itself.
            let mut index = std::collections::HashMap::new();
            for t in &native {
                let next = map.materials.len() as u32 + 1;
                let m = *index.entry(t.surface).or_insert_with(|| {
                    let mut mat = map.materials[0].clone();
                    mat.name = format!("skate3 surface {:#06x}", t.surface);
                    mat.audio = u32::from(t.surface & 0x7f);
                    mat.physics = u32::from((t.surface >> 7) & 0x1f);
                    mat.pattern = u32::from(t.surface >> 12);
                    map.materials.push(mat);
                    next
                });
                map.geometry.collision.push(Collision {
                    points: t.points,
                    surface: u32::from(t.surface),
                    material: m,
                    native_edges: Some(t.edges),
                });
            }
        }
        Ok(PreparedCollision {
            world: crate::skate_world::collision_world(&map, self.material)?,
            grind: std::sync::Arc::new(crate::grind_world::StaticProvider::new(Some(&map))?),
        })
    }
}

/// IW4L's collision as a Skate map: one material, the triangles and rails.
fn collision_map(
    triangles: Vec<[[f32; 3]; 3]>,
    rails: Vec<Vec<[f32; 3]>>,
    spawn: [f32; 3],
    heading: f32,
) -> SkateMap {
    SkateMap {
            version: 14,
            name: "IW4L collision".into(),
            spawn,
            heading,
            environment: vec![],
            materials: vec![skate_data::skate_map::Material {
                name: "MW2".into(),
                flags: 0,
                friction: 0.8,
                restitution: 0.,
                color: [1.; 3],
                roughness: 1.,
                emissive: 0.,
                textures: [0; 5],
                indirect_strength: 1.,
                alpha_mode: 0,
                alpha_cutoff: 0.5,
                audio: 0,
                physics: 0,
                pattern: 0,
                depth_layer: None,
                retail_definition: None,
            }],
            textures: vec![],
            geometry: Geometry {
                vertices: vec![],
                indices: vec![],
                collision: triangles
                    .into_iter()
                    .map(|points| Collision {
                        points,
                        surface: 0,
                        material: 1,
                        native_edges: None,
                    })
                    .collect(),
            },
            rails: rails
                .into_iter()
                .enumerate()
                .map(|(i, p)| Rail {
                    name: format!("iw4_edge_{i}"),
                    closed: false,
                    points: p,
                    native: None,
                })
                .collect(),
            doors: vec![],
            lights: vec![],
            routes: vec![],
            extensions: vec![],
    }
}

/// The source engine's raw XInput transport. No Bevy deadzones, button remaps,
/// trigger reconstruction or rounding are inserted ahead of its native Pad.
#[derive(Default)]
pub struct ControllerTransport {
    capabilities: [crate::input::platform::CapabilityCache; 4],
    /// Empty slots are not asked again until this time. With a controller plugged in, Windows
    /// takes ~7 ms to report each empty XInput slot; three of them every tick (21 ms) made the
    /// 60 Hz simulation run in slow motion. A slot that answers is still read every tick.
    empty_until: [Option<std::time::Instant>; 4],
    /// Which empty slot may be re-checked next (one per tick at most).
    next_empty_check: usize,
    /// Slots polled at least once (a slot not yet polled is not "connected").
    seen: [bool; 4],
}
pub struct InputFrame {
    samples: [Result<crate::input::platform::DevicePacket, crate::input::platform::DeviceError>; 4],
}
impl ControllerTransport {
    pub fn poll(&mut self) -> InputFrame {
        const RECHECK: std::time::Duration = std::time::Duration::from_secs(3);
        let now = std::time::Instant::now();
        // At most one empty slot is re-checked per tick, in turn, and none while a pad answers:
        // with a pad plugged in an empty slot takes ~25 ms to report, and that call holds the
        // XInput lock the game's own pad reading waits on (a ~55 ms frame hitch every second).
        let connected = (0..4).any(|i| self.empty_until[i].is_none() && self.seen[i]);
        let recheck = if connected {
            None
        } else {
            (0..4)
                .map(|k| (self.next_empty_check + k) % 4)
                .find(|&i| self.empty_until[i].is_some_and(|t| now >= t))
        };
        if let Some(i) = recheck {
            self.next_empty_check = (i + 1) % 4;
        }
        InputFrame {
            samples: std::array::from_fn(|i| {
                if self.empty_until[i].is_some() && recheck != Some(i) {
                    return Err(crate::input::platform::DeviceError::Disconnected);
                }
                if connected && self.empty_until[i].is_none() && !self.seen[i] {
                    // Never polled yet while another pad answers: leave it for later.
                    return Err(crate::input::platform::DeviceError::Disconnected);
                }
                let sample = crate::input::platform::poll_cached(i, &mut self.capabilities[i]);
                self.seen[i] = true;
                self.empty_until[i] = match &sample {
                    Err(crate::input::platform::DeviceError::Disconnected) => Some(now + RECHECK),
                    _ => None,
                };
                sample
            }),
        }
    }
}
impl InputFrame {
    pub fn neutral() -> Self {
        Self {
            samples: std::array::from_fn(|_|Err(crate::input::platform::DeviceError::Disconnected)),
        }
    }
    /// A gamepad that is not a device: pad 0 reads `controls` (keyboard and
    /// mouse, mapped by the host), the other three are disconnected.
    pub fn virtual_pad(controls: Controls) -> Self {
        static PACKET: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(1);
        let number = PACKET.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let mut samples: [Result<crate::input::platform::DevicePacket, crate::input::platform::DeviceError>; 4] =
            std::array::from_fn(|_| Err(crate::input::platform::DeviceError::Disconnected));
        samples[0] = Ok(crate::input::platform::DevicePacket {
            number,
            state: skate_core::input::xbox::XboxState {
                buttons: controls.buttons,
                triggers: controls.triggers,
                left: controls.left,
                right: controls.right,
            },
            // XINPUT_DEVSUBTYPE_GAMEPAD.
            subtype: 1,
        });
        Self { samples }
    }
    /// Diagnostics for the mod log: [first ready slot or -1, buttons, left trigger, right
    /// trigger, left x, left y, right x, right y, then per slot 0 ready / 1 disconnected /
    /// 2 state error / 3 capabilities error].
    pub fn debug(&self) -> [i32; 12] {
        let mut out = [0i32; 12];
        out[0] = -1;
        for (i, sample) in self.samples.iter().enumerate() {
            out[8 + i] = match sample {
                Ok(_) => 0,
                Err(crate::input::platform::DeviceError::Disconnected) => 1,
                Err(crate::input::platform::DeviceError::State(_)) => 2,
                Err(_) => 3,
            };
            if let (Ok(p), -1) = (sample, out[0]) {
                let s = &p.state;
                out[..8].copy_from_slice(&[i as i32, i32::from(s.buttons), i32::from(s.triggers[0]), i32::from(s.triggers[1]),
                                          i32::from(s.left[0]), i32::from(s.left[1]), i32::from(s.right[0]), i32::from(s.right[1])]);
            }
        }
        out
    }
    pub fn controller(&self) -> Option<usize> {
        self.samples.iter().position(Result::is_ok)
    }
    /// Keyboard on top of a connected controller (the mod's keyboard binds): buttons from either,
    /// the harder trigger press, and a stick from the keyboard only while its keys are held. With
    /// no keyboard input the controller's packet is left exactly as read.
    pub fn merge_keyboard(&mut self, keys: Controls) {
        if keys.buttons == 0 && keys.triggers == [0; 2] && keys.left == [0; 2] && keys.right == [0; 2] {
            return;
        }
        static PACKET: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(1);
        if let Some(Ok(p)) = self.samples.iter_mut().find(|s| s.is_ok()) {
            let s = &mut p.state;
            s.buttons |= keys.buttons;
            s.triggers = [s.triggers[0].max(keys.triggers[0]), s.triggers[1].max(keys.triggers[1])];
            if keys.left != [0; 2] {
                s.left = keys.left;
            }
            if keys.right != [0; 2] {
                s.right = keys.right;
            }
            // a changed state is a new packet
            p.number = p.number.wrapping_add(PACKET.fetch_add(1, std::sync::atomic::Ordering::Relaxed) | 0x8000_0000);
        }
    }
    pub fn buttons(&self) -> u16 {
        self.samples
            .iter()
            .find_map(|s| s.as_ref().ok().map(|s| s.state.buttons))
            .unwrap_or(0)
    }
}
