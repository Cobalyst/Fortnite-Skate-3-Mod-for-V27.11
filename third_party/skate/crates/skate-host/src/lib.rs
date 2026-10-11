#![allow(dead_code, unused_imports)]
mod physics;
mod graph_host;
mod graph_runtime;
mod skater_animation;
mod animation_pose;
mod camera;
mod difficulty;
mod grind_world;
mod input;
mod scoring_runtime;
mod skate_world;
mod animation;
mod crash_context;
mod tuning;

pub use physics::bridge;
pub use physics::audio_output;

/// Host gameplay options set by the Fortnite mod.
pub mod options {
    use std::sync::atomic::AtomicBool;
    /// Skate 3's Hall of Meat mode (game settings +184 == 1, UI option 29).
    pub static HALL_OF_MEAT: AtomicBool = AtomicBool::new(false);
    /// One board swing (Skate 3's Shove) requested by the mod's swing key, consumed by the next
    /// animation tick.
    pub static BOARD_SWING: AtomicBool = AtomicBool::new(false);
    /// The mod's multi body flip key: body flips (front / back flips of the whole skater) run as
    /// on Normal / Hardcore (PerfectBodyFlips off: keep rotating while held) instead of Easy's
    /// single timed rotation. Only that one physics_mode field changes.
    pub static MULTI_BODY_FLIPS: AtomicBool = AtomicBool::new(false);
    /// The on-foot D-pad gestures (Skate 3 lets the player pick four of its 37): catalog indices
    /// for Up, Down, Left, Right, set by the mod from mod_settings.txt.
    pub static GESTURES: [std::sync::atomic::AtomicU32; 4] = [
        std::sync::atomic::AtomicU32::new(21),
        std::sync::atomic::AtomicU32::new(7),
        std::sync::atomic::AtomicU32::new(0),
        std::sync::atomic::AtomicU32::new(24),
    ];
    pub fn gestures() -> [u32; 4] {
        std::array::from_fn(|i| GESTURES[i].load(std::sync::atomic::Ordering::Relaxed).min(36))
    }
}

pub mod session_marker;

/// Notices for the engine log (the host has no log of its own): pushed where something odd was
/// survived, drained by the simulation thread after each tick. Bounded so a repeating notice
/// cannot grow memory.
pub mod notices {
    use std::sync::Mutex;
    static LIST: Mutex<Vec<String>> = Mutex::new(Vec::new());
    pub fn push(text: String) {
        if let Ok(mut l) = LIST.lock() {
            if l.len() < 64 {
                l.push(text);
            }
        }
    }
    pub fn take() -> Vec<String> {
        LIST.lock().map(|mut l| std::mem::take(&mut *l)).unwrap_or_default()
    }
}

/// Camera diagnostics since the last call: [drop probes without ground inside the patch,
/// drop probes past the patch edge, positioner probe hits, patch size in cm].
pub fn camera_debug() -> [u32; 4] {
    use std::sync::atomic::Ordering::Relaxed;
    let d = &camera::collision_debug();
    [d.drop_miss_inside.swap(0, Relaxed), d.drop_beyond_patch.swap(0, Relaxed),
     d.positioner_hits.swap(0, Relaxed), d.patch_size_cm.load(Relaxed)]
}
