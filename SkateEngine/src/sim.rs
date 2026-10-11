//! The skate simulation thread: advances the session at its fixed rate in real time,
//! independent of the game's frame rate, and publishes the last two ticks so the game
//! thread can show any moment in between without waiting.

use crate::convert::{self, skate_matrix_to_ue, skate_to_ue};
use crate::{SkPad, SkState, log};
use skate_host::audio_output::AudioFrame;
use skate_host::bridge::{CollisionBuilder, ControllerTransport, Controls, Hud, HudEvent, InputFrame, PreparedCollision, Session};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::time::{Duration, Instant};

pub enum Job {
    Activate([f32; 3], f32, [f32; 3], mpsc::Sender<Result<(), String>>),
    Suspend,
    /// Freeze (true) / resume (false) an active skater where it is (Skate 3's object dropper
    /// stops the skater while the editor is open).
    Pause(bool),
    Install(PreparedCollision),
    /// The object dropper's DMOs (rigid bodies in the board's island).
    Dmos(Vec<skate_host::bridge::DmoSpec>),
    /// Upright Object on one DMO (by id).
    UprightDmo(u32),
}

/// A DMO pose for the game thread (Unreal space): position cm, rotation quaternion (x, y, z, w).
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct SkDmoPose {
    pub id: u32,
    /// Bit 0: simulating. Bit 1: the skater holds it.
    pub awake: u32,
    pub position: [f32; 3],
    pub rotation: [f32; 4],
}

/// Skate model frame (columns, skate space) -> Unreal rotation quaternion and position.
fn dmo_pose_to_ue(p: &skate_host::bridge::DmoPose) -> SkDmoPose {
    // Unreal = P * R * P with P swapping Y and Z (a reflection on both sides: a proper rotation).
    let swap = |v: [f32; 3]| [v[0], v[2], v[1]];
    let c = [swap(p.rotation[0]), swap(p.rotation[2]), swap(p.rotation[1])];
    let m = glam::Mat3::from_cols(glam::Vec3::from_array(c[0]), glam::Vec3::from_array(c[1]), glam::Vec3::from_array(c[2]));
    let q = glam::Quat::from_mat3(&m).normalize();
    SkDmoPose {
        id: p.id,
        awake: p.awake as u32 | (p.grabbed as u32) << 1,
        position: convert::skate_to_ue(p.position),
        rotation: [q.x, q.y, q.z, q.w],
    }
}

pub struct Ready {
    pub builder: CollisionBuilder,
    pub bone_names: Vec<String>,
    pub shared: Arc<Mutex<Shared>>,
    pub period: f32,
}

/// One simulation tick's pose in Unreal space.
#[derive(Clone)]
pub struct Snap {
    root: [f32; 16],
    /// World transforms: the engine's bones are relative to the skater root.
    bones: Vec<[f32; 16]>,
    velocity: [f32; 3],
    tick: u64,
    state: String,
    /// Skate 3 camera (Unreal space): position (cm), look direction, up, field of view (degrees).
    camera: Option<([f32; 3], [f32; 3], [f32; 3], f32)>,
}

impl Snap {
    fn finite(&self) -> bool {
        self.root.iter().all(|v| v.is_finite())
            && self.velocity.iter().all(|v| v.is_finite())
            && self.bones.iter().all(|b| b.iter().all(|v| v.is_finite()))
    }
}

/// What the game thread reads and writes; held only briefly by either side.
pub struct Shared {
    pub pad: SkPad,
    pub prev: Snap,
    pub cur: Snap,
    /// When `cur` was published.
    pub cur_at: Instant,
    pub hud: Hud,
    pub audio: AudioFrame,
    pub events: Vec<HudEvent>,
    pub controller: bool,
    /// The last controller poll (InputFrame::debug), for the mod log.
    pub pad_debug: [i32; 12],
    /// Set when the physics failed; the next sk_step reports it.
    pub error: Option<String>,
    /// True while the session is being rebuilt after a failure.
    pub restarting: bool,
    /// The DMOs after the last tick (or the last Dmos job).
    pub dmo_poses: Vec<SkDmoPose>,
}

pub struct Frame {
    pub state: SkState,
    pub bones: Vec<[f32; 16]>,
    pub hud: Hud,
    pub audio: AudioFrame,
    pub events: Vec<HudEvent>,
}

fn snapshot(session: &Session) -> Snap {
    let p = session.pose();
    let root = p.root.to_cols_array();
    Snap {
        root: skate_matrix_to_ue(&root),
        bones: p.bones.iter().map(|b| skate_matrix_to_ue(&convert::mul(&root, &b.to_cols_array()))).collect(),
        velocity: skate_to_ue(p.velocity.to_array()),
        tick: p.tick,
        state: p.state,
        // The camera looks along its basis Z with Y up (as the stock viewer presents it).
        camera: p.camera.map(|(position, basis, fov)| {
            (skate_to_ue(position.to_array()), convert::skate_dir_to_ue(basis.z_axis.to_array()),
             convert::skate_dir_to_ue(basis.y_axis.to_array()), fov)
        }),
    }
}

/// The skater `alpha` of the way from the previous tick to the latest one.
pub fn blended(shared: &mut Shared, alpha: f32) -> Frame {
    let (prev, cur) = (&shared.prev, &shared.cur);
    let root = convert::blend(&prev.root, &cur.root, alpha);
    // The skater's local +Z is forward; after conversion to Unreal that is column 1.
    let mut state = SkState {
        position: [root[12], root[13], root[14]],
        velocity: cur.velocity,
        yaw: root[5].atan2(root[4]).to_degrees(),
        root,
        tick: cur.tick,
        controller: shared.controller as u8,
        state: [0; 48],
        camera_valid: 0,
        camera_position: [0.; 3],
        camera_forward: [1., 0., 0.],
        camera_up: [0., 0., 1.],
        camera_fov: 0.,
    };
    if let (Some(a), Some(b)) = (&prev.camera, &cur.camera) {
        let lerp = |x: &[f32; 3], y: &[f32; 3]| [x[0] + (y[0] - x[0]) * alpha, x[1] + (y[1] - x[1]) * alpha, x[2] + (y[2] - x[2]) * alpha];
        state.camera_valid = 1;
        state.camera_position = lerp(&a.0, &b.0);
        state.camera_forward = lerp(&a.1, &b.1);
        state.camera_up = lerp(&a.2, &b.2);
        state.camera_fov = a.3 + (b.3 - a.3) * alpha;
    } else if let Some(b) = &cur.camera {
        state.camera_valid = 1;
        (state.camera_position, state.camera_forward, state.camera_up, state.camera_fov) = (b.0, b.1, b.2, b.3);
    }
    let n = cur.state.len().min(state.state.len() - 1);
    state.state[..n].copy_from_slice(&cur.state.as_bytes()[..n]);
    let bones = if prev.bones.len() == cur.bones.len() {
        prev.bones.iter().zip(&cur.bones).map(|(a, b)| convert::blend(a, b, alpha)).collect()
    } else {
        cur.bones.clone()
    };
    Frame { state, bones, hud: shared.hud, audio: shared.audio, events: std::mem::take(&mut shared.events) }
}

fn new_session(root: &Path) -> Result<Session, String> {
    // Placeholder floor far below the world until real collision is installed.
    let floor = vec![
        [[-1e4, -500., -1e4], [1e4, -500., -1e4], [1e4, -500., 1e4]],
        [[-1e4, -500., -1e4], [1e4, -500., 1e4], [-1e4, -500., 1e4]],
    ];
    let started = Instant::now();
    let s = Session::new(root, floor, Vec::new(), [0., 0., 0.], 0.)?;
    log(&format!("session loaded in {}ms", started.elapsed().as_millis()));
    Ok(s)
}

unsafe extern "system" {
    fn CreateWaitableTimerExW(attributes: *mut std::ffi::c_void, name: *const u16, flags: u32, access: u32) -> *mut std::ffi::c_void;
    fn SetWaitableTimer(timer: *mut std::ffi::c_void, due: *const i64, period: i32, routine: *mut std::ffi::c_void,
                        arg: *mut std::ffi::c_void, resume: i32) -> i32;
    fn WaitForSingleObject(handle: *mut std::ffi::c_void, milliseconds: u32) -> u32;
}

/// Sleeps `d` on a high-resolution waitable timer (accurate to ~0.5 ms, independent of the
/// system timer period); falls back to thread::sleep if the timer cannot be made.
fn precise_sleep(d: Duration) {
    thread_local! {
        static TIMER: *mut std::ffi::c_void = unsafe {
            // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS
            CreateWaitableTimerExW(std::ptr::null_mut(), std::ptr::null(), 0x2, 0x1F_0003)
        };
    }
    let ok = TIMER.with(|t| {
        if t.is_null() {
            return false;
        }
        let due = -((d.as_nanos() / 100) as i64).max(1);   // relative, 100 ns units
        unsafe { SetWaitableTimer(*t, &due, 0, std::ptr::null_mut(), std::ptr::null_mut(), 0) != 0 && WaitForSingleObject(*t, 1000) == 0 }
    });
    if !ok {
        std::thread::sleep(d);
    }
}

pub fn run(root: PathBuf, installed: Arc<AtomicU64>, jobs: mpsc::Receiver<Job>, ready: mpsc::Sender<Result<Ready, String>>) {
    let mut session = match new_session(&root) {
        Ok(s) => s,
        Err(e) => {
            let _ = ready.send(Err(e));
            return;
        }
    };
    let first = snapshot(&session);
    let shared = Arc::new(Mutex::new(Shared {
        pad: SkPad::default(),
        prev: first.clone(),
        cur: first,
        cur_at: Instant::now(),
        hud: Hud::default(),
        audio: AudioFrame::default(),
        events: Vec::new(),
        controller: false,
        pad_debug: [0; 12],
        error: None,
        restarting: false,
        dmo_poses: Vec::new(),
    }));
    let period_secs = session.period();
    let _ = ready.send(Ok(Ready {
        builder: session.collision_builder(),
        bone_names: session.pose().names,
        shared: Arc::clone(&shared),
        period: period_secs,
    }));
    log(&format!("simulation thread running at {:.1} Hz", 1. / period_secs));

    let period = Duration::from_secs_f32(period_secs);
    let mut transport = ControllerTransport::default();
    let mut active = false;
    let mut paused = false;
    let mut next = Instant::now() + period;
    let mut slow_ticks = 0u32;
    let mut late_ticks = 0u32;
    let ignore_pad = std::env::var_os("SKATE_IGNORE_PAD").is_some();
    // SKATE_FAST_SIM (offline soak tests only): tick as fast as possible instead of in real time.
    let fast = std::env::var_os("SKATE_FAST_SIM").is_some();
    // Per-minute tick cost (simulated minutes), to catch work that grows the longer a session runs.
    let (mut minute_ticks, mut minute_total, mut minute_worst, mut minutes) = (0u32, 0f64, 0f64, 0u32);

    loop {
        // Handle jobs until the next tick is due.
        // Jobs first; otherwise sleep in short precise steps until the tick is due. (recv_timeout
        // rides the coarse Windows timer: about half the ticks woke 10-15 ms late, and a late tick
        // holds the skater still for a moment, which shows as stutter.)
        let job = match jobs.try_recv() {
            Ok(job) => Ok(job),
            Err(mpsc::TryRecvError::Disconnected) => Err(mpsc::RecvTimeoutError::Disconnected),
            Err(mpsc::TryRecvError::Empty) => {
                let wait = next.saturating_duration_since(Instant::now());
                if !wait.is_zero() {
                    precise_sleep(wait.min(Duration::from_millis(2)));
                    continue;
                }
                Err(mpsc::RecvTimeoutError::Timeout)
            }
        };
        match job {
            Ok(Job::Install(prepared)) => {
                let t = Instant::now();
                match catch_unwind(AssertUnwindSafe(|| session.install_collision(prepared))) {
                    Ok(Ok(())) => {
                        installed.fetch_add(1, Ordering::SeqCst);
                    }
                    Ok(Err(e)) => log(&format!("install collision: {e}")),
                    Err(_) => log("install collision panicked"),
                }
                let ms = t.elapsed().as_secs_f32() * 1000.;
                if ms > 4. {
                    log(&format!("slow collision install: {ms:.1} ms"));
                }
                continue;
            }
            Ok(Job::Dmos(specs)) => {
                let count = specs.len();
                match catch_unwind(AssertUnwindSafe(|| session.set_dmos(specs))) {
                    Ok(Ok(())) => {}
                    Ok(Err(e)) => log(&format!("set dmos ({count}): {e}")),
                    Err(_) => log("set dmos panicked"),
                }
                let poses = session.dmo_poses().iter().map(dmo_pose_to_ue).collect();
                shared.lock().unwrap().dmo_poses = poses;
                continue;
            }
            Ok(Job::UprightDmo(id)) => {
                if !session.upright_dmo(id) {
                    log(&format!("upright: no DMO {id}"));
                }
                continue;
            }
            Ok(Job::Suspend) => {
                active = false;
                paused = false;
                session.suspend_input();
                continue;
            }
            Ok(Job::Pause(pause)) => {
                if pause && !paused {
                    paused = true;
                    // hold the pose: the game thread's blend stays on the current tick
                    let mut s = shared.lock().unwrap();
                    s.prev = s.cur.clone();
                    s.cur_at = Instant::now();
                } else if !pause && paused {
                    paused = false;
                    // buttons pressed in the editor must not reach the skater
                    session.suspend_input();
                    next = Instant::now() + period;
                }
                continue;
            }
            Ok(Job::Activate(position, yaw, velocity, reply)) => {
                // Unreal yaw -> heading about skate +Y (skater local +Z faces forward).
                let heading = std::f32::consts::FRAC_PI_2 - yaw.to_radians();
                let result = catch_unwind(AssertUnwindSafe(|| session.activate(crate::ue_to_skate(position), heading)))
                    .unwrap_or_else(|_| Err("skate engine panicked while activating".into()));
                if result.is_ok() {
                    // Velocity in Unreal cm/s -> skate m/s, same axis swap as positions.
                    if velocity.iter().any(|v| *v != 0.) {
                        session.add_velocity([velocity[0] / 100., velocity[2] / 100., velocity[1] / 100.]);
                    }
                    session.take_hud_events();
                    let snap = snapshot(&session);
                    let mut s = shared.lock().unwrap();
                    s.prev = snap.clone();
                    s.cur = snap;
                    s.cur_at = Instant::now();
                    s.events.clear();
                    s.error = None;
                    drop(s);
                    active = true;
                    next = Instant::now() + period;
                }
                let _ = reply.send(result.map(|_| ()));
                continue;
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }

        // A tick is due. Its scheduled time is what the game thread blends against, so a late
        // wake-up (Windows timers) does not show as a stutter. If we fell far behind
        // (debugger, system stall), don't try to catch up.
        let mut scheduled = next;
        next += period;
        if fast {
            next = Instant::now();
        }
        let now = Instant::now();
        if now > next + period * 4 {
            scheduled = now;
            next = now + period;
        }
        let late_ms = now.saturating_duration_since(scheduled).as_secs_f32() * 1000.;
        if late_ms > period_secs * 1000. * 0.5 {
            late_ticks += 1;
            if late_ticks % 120 == 1 {
                log(&format!("simulation tick woke {late_ms:.1} ms late ({late_ticks} late ticks so far)"));
            }
        }
        if !active || paused {
            continue;
        }

        let started = Instant::now();
        let pad = shared.lock().unwrap().pad;
        let result = catch_unwind(AssertUnwindSafe(|| -> Result<(Snap, bool), String> {
            // SKATE_IGNORE_PAD (offline tests only): scripted input even with a real pad plugged in.
            let mut input = if ignore_pad { InputFrame::neutral() } else { transport.poll() };
            let controller = input.controller().is_some();
            shared.lock().unwrap().pad_debug = input.debug();
            let keys = Controls { buttons: pad.buttons, triggers: pad.triggers, left: pad.left, right: pad.right };
            if !controller {
                input = InputFrame::virtual_pad(keys);
            } else {
                // keyboard binds work alongside a connected controller
                input.merge_keyboard(keys);
            }
            session.collect(input, period_secs);
            session.advance()?;
            let snap = snapshot(&session);
            if !snap.finite() {
                return Err("the skater's physics produced non-finite values".into());
            }
            Ok((snap, controller))
        }))
        .unwrap_or_else(|_| Err("skate engine panicked".into()));

        match result {
            Ok((snap, controller)) => {
                let hud = session.hud();
                let audio = session.audio();
                let events = session.take_hud_events();
                let dmo_poses: Vec<SkDmoPose> = session.dmo_poses().iter().map(dmo_pose_to_ue).collect();
                let mut s = shared.lock().unwrap();
                s.dmo_poses = dmo_poses;
                s.prev = std::mem::replace(&mut s.cur, snap);
                s.cur_at = scheduled;
                s.hud = hud;
                s.audio = audio;
                s.events.extend(events);
                if s.events.len() > 256 {
                    s.events.drain(..128);
                }
                s.controller = controller;
            }
            Err(e) => {
                // The session is poisoned (NaN bodies never recover): report it and rebuild.
                log(&format!("physics failed, rebuilding the session: {}", &e[..e.len().min(400)]));
                active = false;
                {
                    let mut s = shared.lock().unwrap();
                    s.error = Some(e);
                    s.restarting = true;
                }
                match new_session(&root) {
                    Ok(fresh) => session = fresh,
                    Err(e) => log(&format!("could not rebuild the session: {e}")),
                }
                shared.lock().unwrap().restarting = false;
                next = Instant::now() + period;
                continue;
            }
        }

        for notice in skate_host::notices::take() {
            log(&notice);
        }
        let ms = started.elapsed().as_secs_f32() * 1000.;
        minute_ticks += 1;
        minute_total += ms as f64;
        minute_worst = minute_worst.max(ms as f64);
        if minute_ticks as f32 * period_secs >= 60. {
            minutes += 1;
            log(&format!("skating minute {minutes}: tick avg {:.2} ms, worst {:.1} ms ({minute_ticks} ticks)", minute_total / minute_ticks as f64, minute_worst));
            (minute_ticks, minute_total, minute_worst) = (0, 0., 0.);
        }
        if ms > period_secs * 1000. {
            slow_ticks += 1;
            if slow_ticks % 60 == 1 {
                log(&format!("slow tick: {ms:.1} ms ({slow_ticks} so far)"));
            }
        }
    }
}
