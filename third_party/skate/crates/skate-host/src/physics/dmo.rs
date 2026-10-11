//! Object-dropper DMOs (Skate 3 "dynamic objects": props such as barriers, bins, benches) as rigid
//! bodies in the board's island (Fortnite skate mod, 2026-10-09).
//!
//! Each body comes from the object's own RenderWare data: its PartDefinition (0x40007: inertia,
//! inverse body frame, contact material) and its collision volumes (0x80002: sphere, capsule, box,
//! cylinder). The bodies join the shared board/skater solve as attached bodies, after the skater,
//! its drive targets and network proxies, so the board, the rider and the props push each other
//! through the same contact solver and the same retail integrator. Their RenderWare collision
//! meshes stay in the board's world (riding / grinding / foot queries) at the bodies' current poses;
//! contacts against those triangles name the body (BoardWorld dynamic ranges).
//! Sleep: bodies start asleep where they are placed; a contact wakes one, and a body whose retail
//! cool-down counter reached the simulation's maximum sleeps again.
use bevy::math::{Mat3, Mat4, Quat, Vec3};
use skate_core::{
    math::{Basis3, Vector3},
    physics::{
        assembly::BodySnapshot,
        board_step::CollisionBody,
        board_world::BoardWorldVolume,
        collision::Sphere,
        contact::RetailContactMaterial,
        rigid_body::{RetailBodyRates, RetailInertiaDynamics, RetailQuaternion, world_inverse_inertia},
        world_contact::ContactPrimitive,
    },
};

/// One collision volume in model space (RenderWare row-vector rows: local axis i = row i).
#[derive(Clone, Copy, Debug)]
pub struct DmoVolume {
    /// RenderWare VolumeType: 1 sphere, 2 capsule, 4 box, 5 cylinder.
    pub kind: u32,
    pub rows: [[f32; 3]; 3],
    pub position: [f32; 3],
    /// sphere: [.., .., .., radius]; capsule: [half length, .., .., radius]; box: [half x, y, z,
    /// fatness]; cylinder: [half height, radius, .., ..].
    pub params: [f32; 4],
}

/// One authored grab spline (the prop's RenderWare 0xEB001F spline block, also its grind spline):
/// header +0x30 points (byte +0x44 count), +0x38 approach vectors (byte +0x46 count), +0x3C word.
/// Model space. The DMO grab provider (ours 82570F38 -> 8256E798) records it as descriptor kind 2.
#[derive(Clone, Debug)]
pub struct DmoGrabSpline {
    pub points: Vec<[f32; 3]>,
    pub approach: Option<[f32; 3]>,
    pub word_60: u32,
}

/// A DMO as the host places it (skate space: metres, Y up).
#[derive(Clone, Debug)]
pub struct DmoSpec {
    pub id: u32,
    /// Model frame: rotation (columns) and origin.
    pub rotation: [[f32; 3]; 3],
    pub position: [f32; 3],
    /// Held by the object dropper: not simulated, no collision.
    pub kinematic: bool,
    /// PartDefinition +0x40: rw::physics::Inertia.
    pub inverse_tensor: [f32; 3],
    pub inverse_mass: f32,
    pub spherical: f32,
    pub maximum_linear_velocity: f32,
    pub maximum_angular_velocity: f32,
    pub linear_drag: f32,
    pub angular_drag: f32,
    /// PartDefinition +0x70: inverse body frame (rows) and translation, model -> body.
    pub inverse_body_rows: [[f32; 3]; 3],
    pub inverse_body_translation: [f32; 3],
    /// PartDefinition +0xb0: static friction, dynamic friction, restitution.
    pub material: [f32; 3],
    pub volumes: Vec<DmoVolume>,
    /// Model-space collision triangles (RW ClusteredMesh) for the board's world.
    pub triangles: Vec<crate::physics::bridge::NativeTriangle>,
    /// Grab splines for the off-board grab scene (state 502 pushes the body through them).
    pub grab_splines: Vec<DmoGrabSpline>,
}

/// Collision bodies of the BoardWorld dynamic ranges: Attached(BODY_BASE + i) for DMO i, remapped
/// to the real attached index (after skater, targets and network proxies) by the solve.
pub(crate) const BODY_BASE: usize = 1 << 20;

/// Identities this host gives a DMO in the grab scene (the native ones are guest pointers; these
/// only need to be distinct and nonzero): query mesh geometry, assembly, first part, rates, spline.
pub(crate) const MESH_BASE: u32 = 0x0C00_0000;
const ASSEMBLY_BASE: u32 = 0x0D00_0000;
const PART_BASE: u32 = 0x0E00_0000;
const RATES_BASE: u32 = 0x0F00_0000;
const GEOMETRY_BASE: u32 = 0x1000_0000;

/// The world simulation the game's DMOs live in (8273B170 -> Simulation 82D97918, parameter
/// words 8 and 9): minimum energy 1e-5 (82199150) and a cool-down of 2 steps. The counter follows
/// the integrator's rule (82ABE120): below the minimum energy and not rising, it counts up, capped
/// at the cool-down; at or above it, it resets.
const MINIMUM_ENERGY: f32 = f32::from_bits(0x3727_C5AC);
const COOL_DOWN: u32 = 2;

pub(crate) struct Dmo {
    pub spec: DmoSpec,
    pub body: BodySnapshot,
    /// Model -> body (the inverse of the inverse body frame).
    body_from_model: Mat4,
    pub awake: bool,
    /// Pose the world triangles were last built at.
    pub built_at: Mat4,
    /// Its query mesh in the board's world (None: kinematic or no triangles).
    pub mesh: Option<usize>,
    /// Sleep counter and the energy of the previous step (MINIMUM_ENERGY / COOL_DOWN).
    counter: u32,
    previous_energy: f32,
    /// The grab splines as grab-scene geometry (built once per spec).
    grab_geometry: Vec<std::sync::Arc<skate_core::player::offboard::grab_scene::Geometry>>,
    /// Upright Object in progress (DMO flags +0x1170 bit 0x40) and its timer (+0x1118).
    upright: Option<f32>,
}

#[derive(Default)]
pub(crate) struct Dmos {
    pub items: Vec<Dmo>,
    /// The world must be rebuilt (one was added / removed / held, or a new static world).
    pub dirty: bool,
    /// A body moved from where its triangles are: they move in place.
    pub moved: bool,
}

fn rows_to_mat3(rows: [[f32; 3]; 3]) -> Mat3 {
    // row-vector rows: local axis i maps to rows[i], i.e. the column-vector matrix has them as columns
    Mat3::from_cols(Vec3::from_array(rows[0]), Vec3::from_array(rows[1]), Vec3::from_array(rows[2]))
}

fn v(a: Vec3) -> Vector3 {
    Vector3::new(a.x, a.y, a.z)
}

/// 82947118: the angle between two directions.
fn angle_between(a: Vec3, b: Vec3) -> f32 {
    let l = (a.length_squared() * b.length_squared()).sqrt();
    if l == 0.0 {
        return 0.0;
    }
    (a.dot(b) / l).clamp(-1.0, 1.0).acos()
}

impl Dmo {
    fn new(spec: DmoSpec, template: &BodySnapshot, gravity: Vector3) -> Self {
        let inverse_body = Mat4::from_mat3(rows_to_mat3(spec.inverse_body_rows))
            * Mat4::IDENTITY;
        let inverse_body = Mat4::from_cols(
            inverse_body.x_axis,
            inverse_body.y_axis,
            inverse_body.z_axis,
            Vec3::from_array(spec.inverse_body_translation).extend(1.0),
        );
        let body_from_model = inverse_body.inverse();
        let mut dmo = Dmo {
            body: *template,
            body_from_model,
            awake: true,
            built_at: Mat4::ZERO,
            mesh: None,
            counter: 0,
            previous_energy: 0.0,
            grab_geometry: Vec::new(),
            upright: None,
            spec,
        };
        dmo.grab_geometry = dmo
            .spec
            .grab_splines
            .iter()
            .enumerate()
            .map(|(k, g)| {
                std::sync::Arc::new(skate_core::player::offboard::grab_scene::Geometry {
                    // A real, nonzero identity per spline (the record's word 49 and validity).
                    id: GEOMETRY_BASE | ((dmo.spec.id & 0xFFFF) << 8) | (k as u32 & 0xFF),
                    points: g.points.iter().map(|p| [p[0], p[1], p[2], 1.0]).collect(),
                    approach_vectors: g.approach.iter().map(|a| [a[0], a[1], a[2], 0.0]).collect(),
                    word_60: g.word_60,
                })
            })
            .collect();
        dmo.body.inertia = RetailInertiaDynamics {
            inverse_tensor: Vector3::new(dmo.spec.inverse_tensor[0], dmo.spec.inverse_tensor[1], dmo.spec.inverse_tensor[2]),
            inverse_mass: dmo.spec.inverse_mass,
            spherical: dmo.spec.spherical,
            maximum_linear_velocity: dmo.spec.maximum_linear_velocity,
            maximum_angular_velocity: dmo.spec.maximum_angular_velocity,
            linear_drag: dmo.spec.linear_drag,
            angular_drag: dmo.spec.angular_drag,
        };
        dmo.place(dmo.model(), gravity);
        dmo
    }

    /// The host's model frame as a matrix.
    fn model(&self) -> Mat4 {
        let r = Mat3::from_cols_array_2d(&self.spec.rotation);
        Mat4::from_cols(r.x_axis.extend(0.0), r.y_axis.extend(0.0), r.z_axis.extend(0.0), Vec3::from_array(self.spec.position).extend(1.0))
    }

    /// Puts the body where the model frame says, at rest.
    fn place(&mut self, model: Mat4, gravity: Vector3) {
        let body = model * self.body_from_model;
        let (_, q, t) = body.to_scale_rotation_translation();
        let q = q.normalize();
        let rates = &mut self.body.rates;
        rates.orientation = RetailQuaternion { x: q.x, y: q.y, z: q.z, w: q.w };
        rates.basis = Basis3 { columns: Mat3::from_quat(q).to_cols_array_2d() };
        rates.world_inverse_inertia = world_inverse_inertia(rates.basis, self.body.inertia.inverse_tensor);
        rates.position = v(t);
        rates.linear_velocity = Vector3::ZERO;
        rates.angular_velocity = Vector3::ZERO;
        // DynamicUpdate leaves gravity in the force accumulator for the next step; a fresh body starts so.
        rates.force_acceleration = gravity;
        rates.torque_acceleration = Vector3::ZERO;
        rates.kinetic_energy = 0.0;
        rates.cool_down = 0;
        // Placed awake: a prop dropped in the air falls and settles, then sleeps through the
        // cool-down like any other body (it used to start asleep and hang until touched).
        self.body.state_flags = 4;
        self.awake = !self.spec.kinematic;
        self.counter = 0;
        self.previous_energy = f32::MAX;
    }

    /// The body's current model frame (world).
    pub fn model_pose(&self) -> Mat4 {
        let r = &self.body.rates;
        let q = Quat::from_xyzw(r.orientation.x, r.orientation.y, r.orientation.z, r.orientation.w).normalize();
        Mat4::from_rotation_translation(q, Vec3::new(r.position.x, r.position.y, r.position.z)) * self.body_from_model.inverse()
    }

    pub fn material(&self) -> RetailContactMaterial {
        RetailContactMaterial {
            static_friction: self.spec.material[0],
            dynamic_friction: self.spec.material[1],
            restitution: self.spec.material[2],
        }
    }

    /// The volumes in world space for `body` (the attached index).
    pub fn world_volumes(&self, body: CollisionBody, out: &mut Vec<BoardWorldVolume>) {
        let model = self.model_pose();
        let rot = Mat3::from_mat4(model);
        let material = self.material();
        let velocity = self.body.rates.linear_velocity;
        for vol in &self.spec.volumes {
            let local = rows_to_mat3(vol.rows);
            let axes = rot * local;
            let center = v(model.transform_point3(Vec3::from_array(vol.position)));
            let p = vol.params;
            let primitive = match vol.kind {
                1 => ContactPrimitive::Sphere(Sphere { center, radius: p[3] }),
                2 => ContactPrimitive::Capsule { center, axis: v(axes.z_axis.normalize()), half_length: p[0], radius: p[3] },
                4 => ContactPrimitive::RoundedBox {
                    center,
                    basis: Basis3 { columns: axes.to_cols_array_2d() },
                    half_extents: Vector3::new(p[0], p[1], p[2]),
                    radius: p[3],
                },
                5 => ContactPrimitive::Cylinder { center, axis: v(axes.z_axis.normalize()), half_height: p[0], radius: p[1] },
                _ => continue,
            };
            out.push(BoardWorldVolume { body, primitive, linear_velocity: velocity, material });
        }
    }

    /// Conservative world bounds (centre, radius) of the volumes.
    pub fn bounds(&self) -> (Vec3, f32) {
        let model = self.model_pose();
        let mut lo = Vec3::splat(f32::INFINITY);
        let mut hi = Vec3::splat(f32::NEG_INFINITY);
        for t in &self.spec.triangles {
            for p in t.points {
                let w = model.transform_point3(Vec3::from_array(p));
                lo = lo.min(w);
                hi = hi.max(w);
            }
        }
        for vol in &self.spec.volumes {
            let w = model.transform_point3(Vec3::from_array(vol.position));
            let r = vol.params.iter().fold(0.0f32, |a, b| a + b.abs());
            lo = lo.min(w - Vec3::splat(r));
            hi = hi.max(w + Vec3::splat(r));
        }
        if !lo.is_finite() || !hi.is_finite() {
            let p = model.w_axis.truncate();
            return (p, 0.0);
        }
        ((lo + hi) * 0.5, (hi - lo).length() * 0.5)
    }
}

impl Dmos {
    /// Replaces the set. A body whose id stays and whose model frame did not change keeps its
    /// motion (the host resends the whole set after every edit).
    pub fn set(&mut self, specs: Vec<DmoSpec>, template: &BodySnapshot, gravity: Vector3) {
        let mut old: Vec<Dmo> = std::mem::take(&mut self.items);
        for spec in specs {
            if let Some(i) = old.iter().position(|d| d.spec.id == spec.id) {
                let mut d = old.swap_remove(i);
                // The host resends the pose it last read back (sk_get_dmo_poses): a body within a
                // millimetre / a thousandth of its current model frame keeps its motion.
                let model = Mat4::from_cols(
                    Vec3::from_array(spec.rotation[0]).extend(0.0),
                    Vec3::from_array(spec.rotation[1]).extend(0.0),
                    Vec3::from_array(spec.rotation[2]).extend(0.0),
                    Vec3::from_array(spec.position).extend(1.0),
                );
                let now = d.model_pose();
                let moved = d.spec.kinematic != spec.kinematic
                    || (model.w_axis - now.w_axis).length() > 0.001
                    || (model.x_axis - now.x_axis).length() > 0.001
                    || (model.y_axis - now.y_axis).length() > 0.001;
                let triangles_changed = d.spec.triangles.len() != spec.triangles.len();
                if !moved && !triangles_changed {
                    // keep the simulated pose: the host's frame is where it last read it
                    d.spec.triangles = spec.triangles;
                    self.items.push(d);
                    continue;
                }
            }
            self.items.push(Dmo::new(spec, template, gravity));
        }
        self.dirty = true;
    }

    /// The grab-scene objects (DMO provider, ours 82570F38: descriptor kind 2, id = object << 8 |
    /// spline; record frame = the body's model frame; object vector128 = the body's linear
    /// velocity, 82D718F0 rates+0x20; assembly first part = the PartDefinition inertia block, whose
    /// words 1 and 4 state 502 reads as 1/Iyy and 1/mass; rates = angular velocity and position).
    /// Gates: selection 0 and matching -1 pass the player's actor query (2948/2952 are 0).
    pub fn grab_objects(&self) -> Vec<skate_core::player::offboard::grab_scene::Object> {
        use skate_core::player::offboard::grab_scene::{AssemblyData, Descriptor, Object, Provider, Spline, record::{PartData, RatesData}};
        let mut out = Vec::new();
        for (i, d) in self.items.iter().enumerate() {
            if d.spec.kinematic || d.grab_geometry.is_empty() {
                continue;
            }
            let n = i as u32 + 1;
            let m = d.model_pose();
            let r = &d.body.rates;
            let i = &d.body.inertia;
            let frame = [m.x_axis.to_array(), m.y_axis.to_array(), m.z_axis.to_array(), m.w_axis.to_array()];
            let velocity = [r.linear_velocity.x, r.linear_velocity.y, r.linear_velocity.z, 0.0];
            out.push(Object {
                id: n,
                provider: Provider::Dmo { selection_variant: 0, matching_group: -1, record_enabled: true },
                disabled: false,
                assembly_ready: true,
                assembly: Some(AssemblyData {
                    identity: ASSEMBLY_BASE | n,
                    first_part: Some(PartData {
                        identity: PART_BASE | n,
                        rates: Some(RatesData {
                            identity: RATES_BASE | n,
                            vector_48: [r.angular_velocity.x, r.angular_velocity.y, r.angular_velocity.z, 0.0],
                            transform_position_48: [r.position.x, r.position.y, r.position.z, 1.0],
                        }),
                        coefficients_0_to_36: [
                            i.inverse_tensor.x,
                            i.inverse_tensor.y,
                            i.inverse_tensor.z,
                            0.0,
                            i.inverse_mass,
                            i.spherical,
                            i.maximum_linear_velocity,
                            i.maximum_angular_velocity,
                            i.linear_drag,
                            i.angular_drag,
                        ],
                    }),
                }),
                frame,
                object_vector_128: velocity,
                splines: d
                    .grab_geometry
                    .iter()
                    .enumerate()
                    .map(|(k, g)| Spline {
                        descriptor: Descriptor { kind: 2, id: (n << 8) | k as u32 },
                        geometry: g.clone(),
                        word_272: 0,
                    })
                    .collect(),
            });
        }
        out
    }

    /// Query mesh -> assembly bindings of the DMO meshes (the interactable probe's hits).
    pub fn mesh_assemblies(&self) -> Vec<crate::physics::offboard::grab_scene::MeshAssembly> {
        self.items
            .iter()
            .enumerate()
            .filter(|(_, d)| d.mesh.is_some() && !d.spec.kinematic)
            .map(|(i, _)| crate::physics::offboard::grab_scene::MeshAssembly {
                mesh: MESH_BASE | (i as u32 + 1),
                assembly: ASSEMBLY_BASE | (i as u32 + 1),
            })
            .collect()
    }

    /// The pushing state's force and torque on a grabbed DMO (descriptor kind 2): the provider
    /// (ours 82571108 / 82571188) wakes the body, ADDS the force to its force accumulator
    /// (82D71D50, rates+0x90) and REPLACES its torque (82D71DC8, rates+0xA0).
    pub fn push(&mut self, descriptor_id: u32, force: [f32; 4], torque: [f32; 4]) {
        let n = (descriptor_id >> 8) as usize;
        let Some(d) = n.checked_sub(1).and_then(|i| self.items.get_mut(i)) else { return };
        if d.spec.kinematic {
            return;
        }
        if !d.awake {
            d.awake = true;
            d.body.state_flags = 4;
            d.body.rates.cool_down = 0;
            d.counter = 0;
            d.previous_energy = f32::MAX;
        }
        let r = &mut d.body.rates;
        r.force_acceleration = Vector3::new(r.force_acceleration.x + force[0], r.force_acceleration.y + force[1], r.force_acceleration.z + force[2]);
        r.torque_acceleration = Vector3::new(torque[0], torque[1], torque[2]);
    }

    /// The action overlay's Upright Object (world vt+0x28, 82C224A0): unless the DMO is already
    /// upright (82C2D330: its up axis within 10 degrees of world up), set flag 0x40 and zero the
    /// timer. Returns whether the DMO exists.
    pub fn start_upright(&mut self, id: u32) -> bool {
        let Some(d) = self.items.iter_mut().find(|d| d.spec.id == id) else { return false };
        if d.spec.kinematic {
            return true;
        }
        let up = d.model_pose().y_axis.truncate();
        if !(angle_between(up, Vec3::Y) * f32::from_bits(0x4265_2ee1) < 10.0) {
            d.upright = Some(0.0);
        }
        true
    }

    /// 82C2D3E0 (from the DMO update 82C2CCD8), once per tick before the solve: while flag 0x40
    /// is set (at most 2 s), turn the body's up axis toward world up.
    pub fn update_upright(&mut self) {
        let dt = f32::from_bits(0x3c88_8889);
        let deg = f32::from_bits(0x3c8e_fa35);
        for d in &mut self.items {
            let Some(timer) = d.upright.as_mut() else { continue };
            *timer += dt;
            if *timer > 2.0 {
                // cleared now; this tick still runs
                d.upright = None;
            }
            if d.spec.kinematic {
                d.upright = None;
                continue;
            }
            let model = d.model_pose();
            let up = model.y_axis.truncate();
            let angle = angle_between(up, Vec3::Y);
            if !(angle * f32::from_bits(0x4265_2ee1) > 10.0) {
                d.upright = None;
                continue;
            }
            let limit = 70.0 * deg;
            let clamped = if angle - limit >= 0.0 { limit } else { angle };
            // the axis that turns up onto world up; a vanishing one (or past 120 degrees) uses the
            // model X or Z axis, whichever has the larger inverse inertia
            let unit = |a: Vec3| {
                let l = a.length();
                if l > f32::from_bits(0x3586_37bd) { a / l } else { Vec3::ZERO }
            };
            let mut axis = unit(up.cross(Vec3::Y));
            let eps = f32::from_bits(0x3400_0000);
            if !(axis.x.abs() > eps || axis.y.abs() > eps || axis.z.abs() > eps) || angle * f32::from_bits(0x4265_2ee1) > 120.0 {
                let t = d.spec.inverse_tensor;
                let local = if t[0] > t[2] { model.x_axis.truncate() } else { model.z_axis.truncate() };
                axis = unit(local);
            }
            let t = Vec3::from_array(d.spec.inverse_tensor);
            let size = {
                let s = t.length() - f32::from_bits(0x3dcc_cccd) - 1.0;
                let s = if -s >= 0.0 { 0.0 } else { s };
                if 1.0 - s >= 0.0 { s } else { 1.0 }
            };
            let gain = (1.0 - size) * 3.0 + size * 5.0;
            let excess = clamped - 5.0 * deg;
            let speed = gain * if -excess >= 0.0 { 0.0 } else { excess };
            let r = &d.body.rates;
            let omega = Vec3::new(r.angular_velocity.x, r.angular_velocity.y, r.angular_velocity.z);
            let along = axis * axis.dot(omega);
            let keep = (omega - along) * f32::from_bits(0x3dcc_cccd) + along;
            let torque = (axis * speed - keep) * 60.0;
            // 82C29B40 -> 82D71DC8: wakes the island and replaces the torque accumulator
            if !d.awake {
                d.awake = true;
                d.body.state_flags = 4;
                d.body.rates.cool_down = 0;
                d.counter = 0;
                d.previous_energy = f32::MAX;
            }
            d.body.rates.torque_acceleration = v(torque);
            if std::env::var_os("UPRIGHT_TRACE").is_some() {
                eprintln!("UPRIGHT angle {:.1} axis {axis:?} speed {speed:.3} omega {omega:?} torque {torque:?}", angle.to_degrees());
            }
        }
    }

    /// Wakes every sleeping DMO a contact names (as B, or as A for a DMO-DMO pair).
    pub fn wake_touched(&mut self, contacts: &[skate_core::physics::board_step::BoardCollision]) {
        for c in contacts {
            for b in [c.body_a, c.body_b] {
                if let CollisionBody::Attached(i) = b {
                    if i >= BODY_BASE {
                        if let Some(d) = self.items.get_mut(i - BODY_BASE) {
                            if !d.awake && !d.spec.kinematic {
                                d.awake = true;
                                d.body.state_flags = 4;
                                d.body.rates.cool_down = 0;
                                d.counter = 0;
                                d.previous_energy = f32::MAX;
                            }
                        }
                    }
                }
            }
        }
    }

    /// After the solve: sleep bodies whose counter reached the cool-down, and mark the world for a
    /// rebuild when a body moved from where its triangles were built.
    pub fn after_step(&mut self) {
        for d in &mut self.items {
            if d.spec.kinematic {
                continue;
            }
            if d.awake {
                let energy = d.body.rates.kinetic_energy;
                if !(energy < MINIMUM_ENERGY) {
                    d.counter = 0;
                } else {
                    if !(energy > d.previous_energy) {
                        d.counter += 1;
                    }
                    d.counter = d.counter.min(COOL_DOWN);
                }
                d.previous_energy = energy;
                if d.counter >= COOL_DOWN {
                    d.awake = false;
                    d.body.state_flags = 0;
                    d.body.rates.linear_velocity = Vector3::ZERO;
                    d.body.rates.angular_velocity = Vector3::ZERO;
                }
            }
            let pose = d.model_pose();
            let moved = (pose.w_axis - d.built_at.w_axis).length() > 0.002
                || (pose.x_axis - d.built_at.x_axis).length() > 0.002
                || (pose.y_axis - d.built_at.y_axis).length() > 0.002;
            if moved {
                self.moved = true;
            }
        }
    }
}
