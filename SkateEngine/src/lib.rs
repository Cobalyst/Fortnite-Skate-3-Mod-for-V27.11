//! Skate 3 engine behind a small C ABI, for the Fortnite 27.11 skate mod DLL.
//!
//! Units at this boundary are Unreal's: centimetres, X forward, Y right, Z up,
//! yaw in degrees (see convert.rs).
//!
//! The simulation runs on its own thread at the engine's fixed rate (sim.rs), never
//! on the game thread: `sk_step` only hands over the latest input and reads back the
//! newest two ticks, blended for the current moment. Collision is built on a second
//! thread and swapped in between ticks. If the physics ever fails, the simulation
//! thread rebuilds its session so skating can start again.
//!
//! Pointers returned stay valid until the next call of the same function (bones,
//! HUD) or for the engine's lifetime (bone names, board parts).

mod assets;
mod convert;
mod sim;

pub(crate) use convert::ue_to_skate;
use skate_host::bridge::HudEvent;
use std::collections::{HashMap, VecDeque};
use std::ffi::{CStr, CString, c_char};
use std::io::Write;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, mpsc};

const STACK: usize = 64 * 1024 * 1024;
// The mod passes its log folder in SKATEMOD_LOG_DIR (the mod can live in any folder); without it the
// log goes to the system temp folder.
fn log_path() -> PathBuf {
    std::env::var_os("SKATEMOD_LOG_DIR").map(PathBuf::from).unwrap_or_else(std::env::temp_dir).join("skate_engine.log")
}

#[repr(C)]
#[derive(Default)]
struct SystemTime {
    year: u16,
    month: u16,
    day_of_week: u16,
    day: u16,
    hour: u16,
    minute: u16,
    second: u16,
    milliseconds: u16,
}

unsafe extern "system" {
    fn GetLocalTime(time: *mut SystemTime);
}

/// Appends a line to the engine log, stamped with the local time like the mod's own log. The
/// file is written by a background thread: the simulation thread never waits on the disk (a busy
/// disk used to stall a tick, which shows as the skater hitching).
pub(crate) fn log(msg: &str) {
    static WRITER: std::sync::OnceLock<Mutex<mpsc::Sender<String>>> = std::sync::OnceLock::new();
    let mut t = SystemTime::default();
    unsafe { GetLocalTime(&mut t) };
    let line = format!("[{:02}:{:02}:{:02}.{:03}] {msg}", t.hour, t.minute, t.second, t.milliseconds);
    let writer = WRITER.get_or_init(|| {
        let (tx, rx) = mpsc::channel::<String>();
        std::thread::spawn(move || {
            let mut file = std::fs::OpenOptions::new().create(true).append(true).open(log_path()).ok();
            for line in rx {
                if let Some(f) = file.as_mut() {
                    let _ = writeln!(f, "{line}");
                }
            }
        });
        Mutex::new(tx)
    });
    if let Ok(tx) = writer.lock() {
        let _ = tx.send(line);
    }
}

static LAST_ERROR: Mutex<Option<CString>> = Mutex::new(None);

fn set_error(e: impl Into<String>) {
    let e = e.into();
    log(&format!("error: {}", &e[..e.len().min(600)]));
    *LAST_ERROR.lock().unwrap() = CString::new(e.replace('\0', " ")).ok();
}

/// One virtual controller frame (XInput layout), used when no real pad is connected.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SkPad {
    pub buttons: u16,
    pub triggers: [u8; 2],
    pub left: [i16; 2],
    pub right: [i16; 2],
}

/// The skater for the current moment, in Unreal space.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SkState {
    /// Skater root position (cm).
    pub position: [f32; 3],
    /// Board velocity (cm/s): what the character keeps when leaving skate mode.
    pub velocity: [f32; 3],
    /// Direction the skater faces (degrees, Unreal yaw).
    pub yaw: f32,
    /// Skater root transform, Unreal space, column-major 4x4 (translation in cm).
    pub root: [f32; 16],
    pub tick: u64,
    /// 1 while a real XInput controller drives the skater.
    pub controller: u8,
    /// Skate 3 player state name, e.g. "PhysicsGround", "KnownAir" (nul-terminated).
    pub state: [u8; 48],
    /// 1 when the Skate 3 camera below is valid.
    pub camera_valid: u8,
    /// Skate 3 camera, Unreal space: position (cm), look direction, up, field of view (degrees).
    pub camera_position: [f32; 3],
    pub camera_forward: [f32; 3],
    pub camera_up: [f32; 3],
    pub camera_fov: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SkHud {
    pub sequence: f32,
    pub line: f32,
    pub line_timer: f32,
    pub multiplier: f32,
    pub total: f32,
    pub line_seconds: f32,
    pub clean: u8,
    pub sketchy: u8,
    pub switch: u8,
    pub fakie: u8,
    pub sequence_active: u8,
    pub board_spin: [f32; 3],
    pub hom_active: u8,
    pub hom_shown: u8,
    pub hom_score: f32,
    pub hom_metrics: [f32; 5],
    pub hom_points: [f32; 5],
    pub hom_levels: [i8; 25],
    pub marker_visible: u8,
    pub marker_can_place: u8,
    pub marker_can_return: u8,
    pub marker_set: u8,
    pub marker_progress: f32,
    pub marker_placed: u32,
    pub marker_refused: u32,
    pub marker_returned: u32,
}

/// Skate 3's physics-side audio data (retail per-skater audio record fields), for the mod's
/// port of the skater/board audio layer.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct SkAudio {
    pub tick: u64,
    pub air: u8,
    pub grinding: u8,
    pub trick_active: u8,
    pub state_flag_80: u8,
    pub trick_id: i32,
    pub airtime: f32,
    pub pop_strength: f32,
    pub ground_speed: f32,
    pub wheel_flag: [u8; 4],
    pub wheel_impact: [f32; 4],
    pub grind_family: i32,
    pub grind_force: f32,
    pub grind_surface: i32,
    pub side_slip: f32,
    pub side_slip_rise: f32,
    pub region_force: [f32; 8],
    pub region_tangent: [f32; 8],
    pub region_material: [u32; 8],
    pub skater_force: f32,
    pub group_8_force: f32,
    pub state_flags: u8,
    pub wheel_count: u8,
    pub com_speed: f32,
    pub carve_turn: f32,
    pub deck_tilt: f32,
    pub deck_roll_rate: f32,
    pub feet_on_deck: u8,
    pub wheel_position: [[f32; 3]; 4],
    pub board_slide: u8,
}

#[repr(C)]
pub struct SkHudEvent {
    /// 1 new trick, 2 current trick changed, 3 bail, 4 line banked
    pub kind: u8,
    pub value: f32,
    pub text: [u8; 64],
}

#[repr(C)]
pub struct SkBoardPart {
    /// Bone the part follows (index into the bone list).
    pub bone: u32,
    /// Board texture index: skate-data/board/tex{texture}.png
    pub texture: u32,
    pub vertex_count: u32,
    pub index_count: u32,
    /// vertex_count * 3 floats, cm, in the bone's local space.
    pub positions: *const f32,
    pub normals: *const f32,
    /// vertex_count * 2 floats.
    pub uvs: *const f32,
    pub indices: *const i32,
}

/// A collision patch for the builder thread: Fortnite-sampled triangles, grind rails, and native
/// Skate 3 triangles (placed dropper objects) with their own edge codes and surfaces.
type CollisionWork = (Vec<[[f32; 3]; 3]>, Vec<Vec<[f32; 3]>>, Vec<skate_host::bridge::NativeTriangle>);

pub struct Engine {
    shared: Arc<Mutex<sim::Shared>>,
    jobs: mpsc::Sender<sim::Job>,
    /// Number of collision patches installed so far.
    installed: Arc<AtomicU64>,
    collision: mpsc::Sender<CollisionWork>,
    period: f32,
    bone_names: Vec<CString>,
    assets: assets::Assets,
    bones: Vec<[f32; 16]>,
    hud: SkHud,
    audio: SkAudio,
    events: VecDeque<HudEvent>,
    /// Skate 3 string keys -> English (skate-data/strings_en.txt).
    strings: HashMap<String, String>,
}

/// Loads KEY<TAB>text lines written by Tools/skate3_strings.py.
fn load_strings(root: &std::path::Path) -> HashMap<String, String> {
    let path = root.parent().unwrap_or(root).join("strings_en.txt");
    // Skate 3's strings are single-byte (Latin-1) text, not UTF-8 (the skate logo is 0xAB).
    let Ok(bytes) = std::fs::read(&path) else {
        log(&format!("no {} - trick names stay as Skate 3 keys", path.display()));
        return HashMap::new();
    };
    let text: String = bytes.iter().map(|&b| b as char).collect();
    text.lines()
        .filter_map(|l| l.split_once('\t'))
        .map(|(k, v)| (k.to_string(), v.to_string()))
        .collect()
}

/// "Switch ID_TRICK_FLIP_KICKFLIP" -> "Switch Kickflip".
/// Translates every localization key in a trick text ("Switch ID_TRICK_X 360 ID_TRICK_Y"),
/// leaving plain words and numbers as they are.
fn translate(strings: &HashMap<String, String>, text: &str) -> String {
    text.split(' ')
        .map(|word| strings.get(word).map_or(word, |v| v.as_str()))
        .collect::<Vec<_>>()
        .join(" ")
}

fn guard<T>(default: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or_else(|_| {
        set_error("panic at the C boundary");
        default
    })
}

/// Loads the skate session from `assets_root` (the converted skate-data/assets folder).
/// Takes several seconds. Returns null on failure (see sk_last_error).
#[unsafe(no_mangle)]
pub extern "C" fn sk_create(assets_root: *const c_char) -> *mut Engine {
    guard(std::ptr::null_mut(), || {
        if assets_root.is_null() {
            set_error("sk_create: null assets_root");
            return std::ptr::null_mut();
        }
        let root = PathBuf::from(unsafe { CStr::from_ptr(assets_root) }.to_string_lossy().into_owned());
        log(&format!("sk_create {}", root.display()));
        // Every panic (caught at the job / C boundary) is logged with its message and source line.
        std::panic::set_hook(Box::new(|info| log(&format!("PANIC: {info}"))));
        let strings = load_strings(&root);

        let (job_tx, job_rx) = mpsc::channel();
        let (ready_tx, ready_rx) = mpsc::channel();
        let installed = Arc::new(AtomicU64::new(0));
        let counter = Arc::clone(&installed);
        let sim_root = root.clone();
        if let Err(e) = std::thread::Builder::new()
            .name("skate-sim".into())
            .stack_size(STACK)
            .spawn(move || sim::run(sim_root, counter, job_rx, ready_tx))
        {
            set_error(format!("spawn simulation thread: {e}"));
            return std::ptr::null_mut();
        }
        let ready = match ready_rx.recv() {
            Ok(Ok(r)) => r,
            Ok(Err(e)) => {
                set_error(format!("skate session: {e}"));
                return std::ptr::null_mut();
            }
            Err(_) => {
                set_error("skate simulation thread died while loading");
                return std::ptr::null_mut();
            }
        };
        let assets = match assets::load(&root, &ready.bone_names) {
            Ok(a) => a,
            Err(e) => {
                set_error(e);
                return std::ptr::null_mut();
            }
        };
        log(&format!(
            "{} bones ({} with a bind pose), {} board parts",
            ready.bone_names.len(),
            assets.binds.iter().filter(|b| b.is_some()).count(),
            assets.board.len()
        ));

        // Collision builds run here, then go to the simulation as Install jobs.
        let builder = ready.builder;
        let (col_tx, col_rx) = mpsc::channel::<CollisionWork>();
        let install = job_tx.clone();
        let _ = std::thread::Builder::new()
            .name("skate-collision".into())
            .stack_size(STACK)
            .spawn(move || {
                while let Ok(mut work) = col_rx.recv() {
                    while let Ok(newer) = col_rx.try_recv() {
                        work = newer;
                    }
                    let (tris, rails, native) = work;
                    let n = tris.len() + native.len();
                    match catch_unwind(AssertUnwindSafe(|| builder.build_with_native(tris, rails, native))) {
                        Ok(Ok(prepared)) => {
                            if install.send(sim::Job::Install(prepared)).is_err() {
                                break;
                            }
                        }
                        Ok(Err(e)) => log(&format!("collision build ({n} triangles) failed: {e}")),
                        Err(_) => log(&format!("collision build ({n} triangles) panicked")),
                    }
                }
            });

        Box::into_raw(Box::new(Engine {
            shared: ready.shared,
            jobs: job_tx,
            installed,
            collision: col_tx,
            period: ready.period,
            bone_names: ready
                .bone_names
                .iter()
                .map(|n| CString::new(n.as_str()).unwrap_or_default())
                .collect(),
            assets,
            bones: Vec::new(),
            hud: SkHud::default(),
            audio: SkAudio::default(),
            events: VecDeque::new(),
            strings,
        }))
    })
}

/// Replaces the world the skater rides. `triangles` holds `count` triangles as
/// 9 floats each (Unreal cm), wound so that (b-a)x(c-a) points out of the surface.
/// Rails are `rail_count` polylines; `rail_lengths[i]` points (3 floats each) are
/// read in order from `rail_points`. Built off-thread.
#[unsafe(no_mangle)]
pub extern "C" fn sk_set_collision(
    engine: *mut Engine,
    triangles: *const f32,
    count: u32,
    rail_points: *const f32,
    rail_lengths: *const u32,
    rail_count: u32,
) -> bool {
    sk_set_collision2(engine, triangles, count, rail_points, rail_lengths, rail_count, std::ptr::null(), std::ptr::null(), std::ptr::null(), 0)
}

/// Native Skate 3 triangles (Unreal cm, 9 floats each, same winding rule as `triangles`) with their
/// RW ClusteredMesh edge codes (3 bytes each, edge i = corner i to corner i+1) and 16-bit surface ids.
fn native_triangles(points: *const f32, edges: *const u8, surfaces: *const u16, count: u32) -> Vec<skate_host::bridge::NativeTriangle> {
    if count == 0 || points.is_null() || edges.is_null() || surfaces.is_null() {
        return Vec::new();
    }
    let p = unsafe { std::slice::from_raw_parts(points, count as usize * 9) };
    let e = unsafe { std::slice::from_raw_parts(edges, count as usize * 3) };
    let s = unsafe { std::slice::from_raw_parts(surfaces, count as usize) };
    (0..count as usize)
        .filter(|&i| p[i * 9..i * 9 + 9].iter().all(|v| v.is_finite()))
        .map(|i| {
            let t = &p[i * 9..i * 9 + 9];
            // Corners b and c swap with the handedness change, so the edges become
            // (a,c) = old edge 2, (c,b) = old edge 1, (b,a) = old edge 0.
            skate_host::bridge::NativeTriangle {
                points: [ue_to_skate([t[0], t[1], t[2]]), ue_to_skate([t[6], t[7], t[8]]), ue_to_skate([t[3], t[4], t[5]])],
                edges: [e[i * 3 + 2], e[i * 3 + 1], e[i * 3]],
                surface: s[i],
            }
        })
        .collect()
}

/// sk_set_collision plus native Skate 3 triangles (see native_triangles). Built off-thread.
#[unsafe(no_mangle)]
pub extern "C" fn sk_set_collision2(
    engine: *mut Engine,
    triangles: *const f32,
    count: u32,
    rail_points: *const f32,
    rail_lengths: *const u32,
    rail_count: u32,
    native_triangles_ptr: *const f32,
    native_edges: *const u8,
    native_surfaces: *const u16,
    native_count: u32,
) -> bool {
    guard(false, || {
        let Some(engine) = (unsafe { engine.as_ref() }) else {
            return false;
        };
        let raw = unsafe { std::slice::from_raw_parts(triangles, count as usize * 9) };
        let tris: Vec<[[f32; 3]; 3]> = raw
            .chunks_exact(9)
            .filter(|t| t.iter().all(|v| v.is_finite()))
            .map(|t| {
                // Unreal is left-handed and skate space right-handed: swapping two
                // corners keeps the face pointing the same way after conversion.
                [
                    ue_to_skate([t[0], t[1], t[2]]),
                    ue_to_skate([t[6], t[7], t[8]]),
                    ue_to_skate([t[3], t[4], t[5]]),
                ]
            })
            .filter(|[a, b, c]| {
                // Skate rejects the whole patch if one triangle has no normal; slivers
                // (under 1 cm^2) are where non-finite contact normals come from.
                let u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
                let v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
                let n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]];
                let l = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
                l.is_finite() && l > 4e-8
            })
            .collect();
        let mut rails = Vec::new();
        if rail_count > 0 && !rail_points.is_null() && !rail_lengths.is_null() {
            let lengths = unsafe { std::slice::from_raw_parts(rail_lengths, rail_count as usize) };
            let total: usize = lengths.iter().map(|&n| n as usize).sum();
            let pts = unsafe { std::slice::from_raw_parts(rail_points, total * 3) };
            let mut at = 0;
            for &n in lengths {
                let rail: Vec<[f32; 3]> = (0..n as usize)
                    .map(|i| ue_to_skate([pts[(at + i) * 3], pts[(at + i) * 3 + 1], pts[(at + i) * 3 + 2]]))
                    .collect();
                at += n as usize;
                if rail.len() >= 2 {
                    rails.push(rail);
                }
            }
        }
        if tris.is_empty() && native_count == 0 {
            set_error("sk_set_collision: no usable triangles");
            return false;
        }
        engine.collision.send((tris, rails, native_triangles(native_triangles_ptr, native_edges, native_surfaces, native_count))).is_ok()
    })
}

/// An object-dropper DMO for sk_set_dmos. The pose is Unreal space (cm, rotation quaternion); all
/// model data is Skate 3's own (model space: metres, Y up), straight from the object's files.
#[repr(C)]
pub struct SkDmo {
    pub id: u32,
    pub kinematic: u32,
    pub position: [f32; 3],
    pub rotation: [f32; 4],
    /// PartDefinition +0x40 (rw::physics::Inertia).
    pub inverse_tensor: [f32; 3],
    pub inverse_mass: f32,
    pub spherical: f32,
    pub maximum_linear_velocity: f32,
    pub maximum_angular_velocity: f32,
    pub linear_drag: f32,
    pub angular_drag: f32,
    /// PartDefinition +0x70: inverse body frame rows and translation.
    pub inverse_body_rows: [f32; 9],
    pub inverse_body_translation: [f32; 3],
    /// PartDefinition +0xb0: static friction, dynamic friction, restitution.
    pub material: [f32; 3],
    pub first_volume: u32,
    pub volume_count: u32,
    pub first_triangle: u32,
    pub triangle_count: u32,
    pub first_spline: u32,
    pub spline_count: u32,
}

/// A grab spline of a DMO (its RenderWare 0xEB001F spline: points, optional approach vector,
/// header word +0x3C), model space. Points index the point array (three floats each).
#[repr(C)]
pub struct SkDmoSpline {
    pub first_point: u32,
    pub point_count: u32,
    pub approach: [f32; 3],
    pub approach_count: u32,
    pub word_60: u32,
}

#[repr(C)]
pub struct SkDmoVolume {
    pub kind: u32,
    pub rows: [f32; 9],
    pub position: [f32; 3],
    pub params: [f32; 4],
}

#[repr(C)]
pub struct SkDmoTriangle {
    /// Skate model space, Skate 3's own winding.
    pub points: [f32; 9],
    pub edges: [u8; 3],
    pub pad: u8,
    pub surface: u16,
    pub pad2: u16,
}

/// Replaces the object dropper's DMOs: rigid bodies in the board's island, colliding with the
/// world, each other, the board and the rider (see skate-host physics/dmo.rs).
#[unsafe(no_mangle)]
pub extern "C" fn sk_set_dmos(
    engine: *mut Engine,
    dmos: *const SkDmo,
    count: u32,
    volumes: *const SkDmoVolume,
    volume_count: u32,
    triangles: *const SkDmoTriangle,
    triangle_count: u32,
    splines: *const SkDmoSpline,
    spline_count: u32,
    points: *const f32,
    point_count: u32,
) -> bool {
    guard(false, || {
        let Some(engine) = (unsafe { engine.as_ref() }) else {
            return false;
        };
        let dmos = if count == 0 || dmos.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(dmos, count as usize) } };
        let volumes = if volume_count == 0 || volumes.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(volumes, volume_count as usize) } };
        let triangles = if triangle_count == 0 || triangles.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(triangles, triangle_count as usize) } };
        let splines = if spline_count == 0 || splines.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(splines, spline_count as usize) } };
        let points = if point_count == 0 || points.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(points, point_count as usize * 3) } };
        let mut specs = Vec::with_capacity(dmos.len());
        for d in dmos {
            // Unreal pose -> skate model frame: R_s = P * R_u * P (P swaps Y and Z), origin swapped.
            let q = glam::Quat::from_xyzw(d.rotation[0], d.rotation[1], d.rotation[2], d.rotation[3]).normalize();
            let r = glam::Mat3::from_quat(q);
            let swap = |v: glam::Vec3| [v.x, v.z, v.y];
            let cols = [swap(r.x_axis), swap(r.z_axis), swap(r.y_axis)];
            let rows = |a: [f32; 9]| [[a[0], a[1], a[2]], [a[3], a[4], a[5]], [a[6], a[7], a[8]]];
            let vol = |v: &SkDmoVolume| skate_host::bridge::DmoVolume { kind: v.kind, rows: rows(v.rows), position: v.position, params: v.params };
            let fv = d.first_volume as usize;
            let ft = d.first_triangle as usize;
            let vs = volumes.get(fv..fv + d.volume_count as usize).unwrap_or(&[]);
            let ts = triangles.get(ft..ft + d.triangle_count as usize).unwrap_or(&[]);
            specs.push(skate_host::bridge::DmoSpec {
                id: d.id,
                rotation: cols,
                position: ue_to_skate(d.position),
                kinematic: d.kinematic != 0,
                inverse_tensor: d.inverse_tensor,
                inverse_mass: d.inverse_mass,
                spherical: d.spherical,
                maximum_linear_velocity: d.maximum_linear_velocity,
                maximum_angular_velocity: d.maximum_angular_velocity,
                linear_drag: d.linear_drag,
                angular_drag: d.angular_drag,
                inverse_body_rows: rows(d.inverse_body_rows),
                inverse_body_translation: d.inverse_body_translation,
                material: d.material,
                volumes: vs.iter().map(vol).collect(),
                triangles: ts
                    .iter()
                    .map(|t| skate_host::bridge::NativeTriangle {
                        points: [[t.points[0], t.points[1], t.points[2]], [t.points[3], t.points[4], t.points[5]], [t.points[6], t.points[7], t.points[8]]],
                        edges: t.edges,
                        surface: t.surface,
                    })
                    .collect(),
                grab_splines: splines
                    .get(d.first_spline as usize..(d.first_spline + d.spline_count) as usize)
                    .unwrap_or(&[])
                    .iter()
                    .map(|g| skate_host::bridge::DmoGrabSpline {
                        points: (g.first_point..g.first_point + g.point_count)
                            .filter_map(|k| points.get(k as usize * 3..k as usize * 3 + 3))
                            .map(|p| [p[0], p[1], p[2]])
                            .collect(),
                        approach: (g.approach_count != 0).then_some(g.approach),
                        word_60: g.word_60,
                    })
                    .collect(),
            });
        }
        engine.jobs.send(sim::Job::Dmos(specs)).is_ok()
    })
}

/// Skate 3's action overlay "Upright Object" (D-pad up while holding a prop): the DMO turns its
/// up axis toward world up for up to 2 s, unless it is already within 10 degrees.
#[unsafe(no_mangle)]
pub extern "C" fn sk_upright_dmo(engine: *mut Engine, id: u32) -> bool {
    guard(false, || {
        let Some(engine) = (unsafe { engine.as_ref() }) else {
            set_error("null engine");
            return false;
        };
        engine.jobs.send(sim::Job::UprightDmo(id)).is_ok()
    })
}

/// Copies the DMOs' poses after the last tick (Unreal space); returns how many there are.
#[unsafe(no_mangle)]
pub extern "C" fn sk_get_dmo_poses(engine: *const Engine, out: *mut sim::SkDmoPose, max: u32) -> u32 {
    guard(0, || {
        let Some(engine) = (unsafe { engine.as_ref() }) else {
            return 0;
        };
        let s = engine.shared.lock().unwrap();
        let n = s.dmo_poses.len().min(max as usize);
        if !out.is_null() {
            for (i, p) in s.dmo_poses.iter().take(n).enumerate() {
                unsafe { *out.add(i) = *p };
            }
        }
        s.dmo_poses.len() as u32
    })
}

/// How many collision patches have been installed; poll after sk_set_collision.
#[unsafe(no_mangle)]
pub extern "C" fn sk_collision_generation(engine: *const Engine) -> u64 {
    unsafe { engine.as_ref() }.map_or(0, |e| e.installed.load(Ordering::SeqCst))
}

/// Puts the skater at `position` (cm) facing `yaw` (degrees) and starts simulating.
#[unsafe(no_mangle)]
pub extern "C" fn sk_activate(engine: *mut Engine, position: *const f32, yaw: f32, velocity: *const f32, out: *mut SkState) -> bool {
    guard(false, || {
        let Some(e) = (unsafe { engine.as_mut() }) else {
            return false;
        };
        if position.is_null() {
            return false;
        }
        if e.shared.lock().unwrap().restarting {
            set_error("the skate engine is restarting after an error; try again in a few seconds");
            return false;
        }
        let p = unsafe { [*position, *position.add(1), *position.add(2)] };
        let (reply_tx, reply_rx) = mpsc::channel();
        let v = if velocity.is_null() { [0.; 3] } else { unsafe { [*velocity, *velocity.add(1), *velocity.add(2)] } };
        if e.jobs.send(sim::Job::Activate(p, yaw, v, reply_tx)).is_err() {
            set_error("skate simulation thread is gone");
            return false;
        }
        match reply_rx.recv() {
            Ok(Ok(())) => {
                e.events.clear();
                read_frame(e, out);
                true
            }
            Ok(Err(err)) => {
                set_error(err);
                false
            }
            Err(_) => {
                set_error("skate simulation thread is gone");
                false
            }
        }
    })
}

/// Copies the current blended frame into the engine's caches and `out`.
fn read_frame(e: &mut Engine, out: *mut SkState) {
    let frame = {
        let mut shared = e.shared.lock().unwrap();
        let alpha = (shared.cur_at.elapsed().as_secs_f32() / e.period).clamp(0., 1.);
        sim::blended(&mut shared, alpha)
    };
    if let Some(out) = unsafe { out.as_mut() } {
        *out = frame.state;
    }
    e.bones = frame.bones;
    let h = frame.hud;
    e.hud = SkHud { sequence: h.sequence, line: h.line, line_timer: h.line_timer, multiplier: h.multiplier, total: h.total,
        line_seconds: h.line_seconds, clean: h.clean as u8, sketchy: h.sketchy as u8, switch: h.switch as u8, fakie: h.fakie as u8, sequence_active: h.sequence_active as u8, board_spin: h.board_spin,
        hom_active: h.hom_active as u8, hom_shown: h.hom_shown as u8, hom_score: h.hom_score,
        hom_metrics: h.hom_metrics, hom_points: h.hom_points, hom_levels: h.hom_levels,
        marker_visible: h.marker.visible as u8, marker_can_place: h.marker.can_place as u8, marker_can_return: h.marker.can_return as u8,
        marker_set: h.marker.has_marker as u8, marker_progress: h.marker.progress,
        marker_placed: h.marker.placed, marker_refused: h.marker.refused, marker_returned: h.marker.returned };
    let a = frame.audio;
    e.audio = SkAudio { tick: a.tick, air: a.air as u8, grinding: a.grinding as u8, trick_active: a.trick_active as u8,
        state_flag_80: a.state_flag_80 as u8, trick_id: a.trick_id, airtime: a.airtime, pop_strength: a.pop_strength,
        ground_speed: a.ground_speed, wheel_flag: a.wheel_flag.map(|f| f as u8), wheel_impact: a.wheel_impact,
        grind_family: a.grind_family, grind_force: a.grind_force, grind_surface: a.grind_surface,
        side_slip: a.side_slip, side_slip_rise: a.side_slip_rise, region_force: a.region_force,
        region_tangent: a.region_tangent, region_material: a.region_material, skater_force: a.skater_force,
        group_8_force: a.group_8_force, state_flags: a.state_flags, wheel_count: a.wheel_count,
        com_speed: a.com_speed, carve_turn: a.carve_turn, deck_tilt: a.deck_tilt, deck_roll_rate: a.deck_roll_rate,
        feet_on_deck: a.feet_on_deck, wheel_position: a.wheel_position, board_slide: a.board_slide };
    for mut ev in frame.events {
        ev.text = translate(&e.strings, &ev.text);
        e.events.push_back(ev);
    }
    while e.events.len() > 64 {
        e.events.pop_front();
    }
}

/// Hands the simulation the latest pad (used only when no XInput controller is connected)
/// and returns the skater for this moment. Never waits for the simulation. `dt` is unused:
/// the simulation keeps its own fixed-rate clock.
#[unsafe(no_mangle)]
pub extern "C" fn sk_step(engine: *mut Engine, _dt: f32, pad: *const SkPad, out: *mut SkState) -> bool {
    guard(false, || {
        let Some(e) = (unsafe { engine.as_mut() }) else {
            set_error("null engine");
            return false;
        };
        {
            let mut shared = e.shared.lock().unwrap();
            if let Some(err) = shared.error.take() {
                drop(shared);
                set_error(err);
                return false;
            }
            shared.pad = unsafe { pad.as_ref() }.copied().unwrap_or_default();
        }
        read_frame(e, out);
        true
    })
}

/// Stops simulating and drops held input (leaving skate mode). The session stays loaded.
#[unsafe(no_mangle)]
pub extern "C" fn sk_suspend(engine: *mut Engine) -> bool {
    guard(false, || {
        let Some(e) = (unsafe { engine.as_ref() }) else {
            return false;
        };
        e.jobs.send(sim::Job::Suspend).is_ok()
    })
}

/// Freezes (paused = true) or resumes the active skater in place; the pose and momentum are kept.
#[unsafe(no_mangle)]
pub extern "C" fn sk_set_paused(engine: *mut Engine, paused: bool) -> bool {
    guard(false, || {
        let Some(e) = (unsafe { engine.as_ref() }) else {
            return false;
        };
        e.jobs.send(sim::Job::Pause(paused)).is_ok()
    })
}

/// Number of skater bones (pose order; same for every frame).
#[unsafe(no_mangle)]
pub extern "C" fn sk_bone_count(engine: *const Engine) -> u32 {
    unsafe { engine.as_ref() }.map_or(0, |e| e.bone_names.len() as u32)
}

/// Bone name (e.g. "HIPS", "LEFTFOOT", "SKATEBOARD_ROOT"); valid for the engine's lifetime.
#[unsafe(no_mangle)]
pub extern "C" fn sk_bone_name(engine: *const Engine, index: u32) -> *const c_char {
    unsafe { engine.as_ref() }
        .and_then(|e| e.bone_names.get(index as usize))
        .map_or(std::ptr::null(), |n| n.as_ptr())
}

/// Bind (reference) pose of a bone, Unreal space, column-major. False if the rig lacks it.
#[unsafe(no_mangle)]
pub extern "C" fn sk_bone_bind(engine: *const Engine, index: u32, out: *mut f32) -> bool {
    let Some(bind) = unsafe { engine.as_ref() }.and_then(|e| e.assets.binds.get(index as usize).copied().flatten()) else {
        return false;
    };
    unsafe { std::ptr::copy_nonoverlapping(bind.as_ptr(), out, 16) };
    true
}

/// World transforms of every bone for the last sk_step/sk_activate (Unreal space, column-major,
/// 16 floats each). Writes at most `max` bones; returns how many there are.
#[unsafe(no_mangle)]
pub extern "C" fn sk_get_bones(engine: *const Engine, out: *mut f32, max: u32) -> u32 {
    let Some(e) = (unsafe { engine.as_ref() }) else {
        return 0;
    };
    let n = e.bones.len().min(max as usize);
    if !out.is_null() {
        for (i, b) in e.bones.iter().take(n).enumerate() {
            unsafe { std::ptr::copy_nonoverlapping(b.as_ptr(), out.add(i * 16), 16) };
        }
    }
    e.bones.len() as u32
}

/// Trickline HUD state for the last sk_step.
#[unsafe(no_mangle)]
pub extern "C" fn sk_get_hud(engine: *const Engine, out: *mut SkHud) -> bool {
    let (Some(e), Some(out)) = (unsafe { engine.as_ref() }, unsafe { out.as_mut() }) else {
        return false;
    };
    *out = e.hud;
    true
}

/// Skate 3's physics-side audio data after the latest simulation tick.
#[unsafe(no_mangle)]
pub extern "C" fn sk_get_audio(engine: *const Engine, out: *mut SkAudio) -> bool {
    let (Some(e), Some(out)) = (unsafe { engine.as_ref() }, unsafe { out.as_mut() }) else {
        return false;
    };
    *out = e.audio;
    true
}

/// Scoring identifier name for a trick id from SkAudio (empty for an unknown id).
#[unsafe(no_mangle)]
pub extern "C" fn sk_trick_identifier(id: i32) -> *const std::ffi::c_char {
    static NAMES: std::sync::OnceLock<Vec<std::ffi::CString>> = std::sync::OnceLock::new();
    let names = NAMES.get_or_init(|| {
        (0..400).map(|i| std::ffi::CString::new(skate_host::audio_output::trick_identifier(i)).unwrap()).collect()
    });
    names.get(id as usize).map_or(c"".as_ptr(), |n| n.as_ptr())
}

/// Next trickline event (oldest first); false when there are none.
#[unsafe(no_mangle)]
pub extern "C" fn sk_pop_hud_event(engine: *mut Engine, out: *mut SkHudEvent) -> bool {
    let (Some(e), Some(out)) = (unsafe { engine.as_mut() }, unsafe { out.as_mut() }) else {
        return false;
    };
    let Some(ev) = e.events.pop_front() else {
        return false;
    };
    out.kind = ev.kind;
    out.value = ev.value;
    out.text = [0; 64];
    // single-byte text for the HUD's Skate 3 fonts (Latin-1 characters stay one byte)
    let bytes: Vec<u8> = ev.text.chars().map(|c| if (c as u32) < 256 { c as u32 as u8 } else { b'?' }).collect();
    let n = bytes.len().min(63);
    out.text[..n].copy_from_slice(&bytes[..n]);
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn sk_board_part_count(engine: *const Engine) -> u32 {
    unsafe { engine.as_ref() }.map_or(0, |e| e.assets.board.len() as u32)
}

/// A rigid piece of the Skate 3 board; pointers stay valid for the engine's lifetime.
#[unsafe(no_mangle)]
pub extern "C" fn sk_board_part(engine: *const Engine, index: u32, out: *mut SkBoardPart) -> bool {
    let (Some(e), Some(out)) = (unsafe { engine.as_ref() }, unsafe { out.as_mut() }) else {
        return false;
    };
    let Some(p) = e.assets.board.get(index as usize) else {
        return false;
    };
    *out = SkBoardPart {
        bone: p.bone,
        texture: p.texture,
        vertex_count: (p.positions.len() / 3) as u32,
        index_count: p.indices.len() as u32,
        positions: p.positions.as_ptr(),
        normals: p.normals.as_ptr(),
        uvs: p.uvs.as_ptr(),
        indices: p.indices.as_ptr(),
    };
    true
}

/// Last error message, valid until the next failing call.
#[unsafe(no_mangle)]
pub extern "C" fn sk_last_error() -> *const c_char {
    LAST_ERROR.lock().unwrap().as_ref().map_or(c"".as_ptr(), |e| e.as_ptr())
}

/// Gameplay option by name: "hall_of_meat" (0/1). Returns false for an unknown name.
/// Diagnostics: the last controller poll (12 ints: slot or -1, buttons, triggers, sticks, then
/// each slot's status 0 ready / 1 disconnected / 2 state error / 3 capabilities error).
/// Camera diagnostics since the last call (skate_host::camera_debug): drop probes without
/// ground inside the patch, drop probes past the patch edge, positioner hits, patch size cm.
#[unsafe(no_mangle)]
pub extern "C" fn sk_get_camera_debug(out: *mut u32) -> bool {
    guard(false, || {
        if out.is_null() { return false; }
        let v = skate_host::camera_debug();
        unsafe { std::ptr::copy_nonoverlapping(v.as_ptr(), out, 4) };
        true
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn sk_get_pad_debug(engine: *const Engine, out: *mut i32) -> bool {
    guard(false, || {
        let Some(engine) = (unsafe { engine.as_ref() }) else { return false };
        if out.is_null() { return false; }
        let values = engine.shared.lock().map(|s| s.pad_debug).unwrap_or([0; 12]);
        unsafe { std::ptr::copy_nonoverlapping(values.as_ptr(), out, 12) };
        true
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn sk_set_option(name: *const c_char, value: i32) -> bool {
    if name.is_null() {
        return false;
    }
    let name = unsafe { std::ffi::CStr::from_ptr(name) }.to_string_lossy();
    match name.as_ref() {
        "hall_of_meat" => {
            skate_host::options::HALL_OF_MEAT.store(value != 0, std::sync::atomic::Ordering::Relaxed);
            true
        }
        "board_swing" => {
            skate_host::options::BOARD_SWING.store(value != 0, std::sync::atomic::Ordering::Relaxed);
            true
        }
        "gesture_up" | "gesture_down" | "gesture_left" | "gesture_right" => {
            let slot = match name.as_ref() { "gesture_up" => 0, "gesture_down" => 1, "gesture_left" => 2, _ => 3 };
            if !(0..37).contains(&value) {
                return false;
            }
            skate_host::options::GESTURES[slot].store(value as u32, std::sync::atomic::Ordering::Relaxed);
            true
        }
        "multi_body_flips" => {
            skate_host::options::MULTI_BODY_FLIPS.store(value != 0, std::sync::atomic::Ordering::Relaxed);
            true
        }
        _ => false,
    }
}
