//! Host camera query adapter. The six source fat lines are preserved, and each
//! candidate uses the shared recovered TU3 thin/fat triangle dispatcher.
use std::sync::atomic::{AtomicU32, Ordering::Relaxed};
use skate_core::{
    camera::{DropCollisionProvider, FatLine, FatLineResult, PositionerCollisionProvider},
    physics::board_world::BoardWorld,
};

pub(super) struct CameraCollision<'a> {
    world: &'a BoardWorld,
    results: [FatLineResult; 6],
    pub error: Option<String>,
}

impl DropCollisionProvider for CameraCollision<'_> {
    fn query(
        &mut self,
        lines: [FatLine; 10],
        _context: u32,
    ) -> Result<[FatLineResult; 10], String> {
        let mut output = [no_hit(); 10];
        // The host world is a streamed collision patch around the skater (12 m), not the whole
        // map. A probe past its edge found "no ground" and the predictor saw a drop that is not
        // there: at speed the camera pitched down until the next patch arrived, then back up
        // (bobbing on long straight streets). Such probes now see the ground continue at the
        // height of the last probe that did land inside the patch; real drops inside are unchanged.
        let covered = patch_bounds(self.world);
        if let Some((x0, z0, x1, z1)) = covered {
            CAMERA_DEBUG.patch_size_cm.store(((x1 - x0).max(z1 - z0) * 100.0) as u32, Relaxed);
        }
        // Seeded with the probe origin (the subject on the ground: level ground ahead).
        let mut ground: Option<f32> = Some(lines[0].start[1]);
        for (index, (result, line)) in output.iter_mut().zip(lines).enumerate() {
            // Odd lines drop straight down from a point ahead; even lines run forward to it.
            let down = index % 2 == 1;
            let inside = covered.is_some_and(|(x0, z0, x1, z1)| {
                const MARGIN: f32 = 0.5;
                let (x, z) = (line.start[0], line.start[2]);
                x > x0 + MARGIN && x < x1 - MARGIN && z > z0 + MARGIN && z < z1 - MARGIN
            });
            if inside || covered.is_none() {
                *result = super::world_query::line(self.world, line.start, line.end, line.radius)?;
                if !down { continue; }
                if result.hit != 0 { ground = Some(result.position[1]); continue; }
                // A downward line that found nothing inside the patch fell through a gap in the
                // mod's 50 cm ground grid (2026-10-08 log: every second the camera dived on a flat
                // Tilted street had such misses, steady seconds had none). Ground continues; a
                // real drop up to MaxDropDistance (6 m) still hits the lower ground and counts.
                CAMERA_DEBUG.drop_miss_inside.fetch_add(1, Relaxed);
            } else if down {
                CAMERA_DEBUG.drop_beyond_patch.fetch_add(1, Relaxed);
            } else {
                continue;
            }
            if let Some(y) = ground {
                let span = line.start[1] - line.end[1];
                if span > 1e-3 && y <= line.start[1] && y >= line.end[1] {
                    let fraction = (line.start[1] - y) / span;
                    *result = FatLineResult {
                        position: [line.start[0] + (line.end[0] - line.start[0]) * fraction, y,
                                   line.start[2] + (line.end[2] - line.start[2]) * fraction, 0.0],
                        normal: [0.0, 1.0, 0.0, 0.0], fraction, hit: 1, surface: 0 };
                }
            }
        }
        Ok(output)
    }
}

impl<'a> CameraCollision<'a> {
    pub fn new(world: &'a BoardWorld) -> Self {
        Self {
            world,
            results: [no_hit(); 6],
            error: None,
        }
    }
}

/// Diagnostics for the mod's log: drop probes that found no ground inside the patch, probes
/// past the patch edge (ground assumed to continue), positioner probes that hit something,
/// and the patch's horizontal size.
pub struct CameraDebug {
    pub drop_miss_inside: AtomicU32,
    pub drop_beyond_patch: AtomicU32,
    pub positioner_hits: AtomicU32,
    pub patch_size_cm: AtomicU32,
}
pub static CAMERA_DEBUG: CameraDebug = CameraDebug {
    drop_miss_inside: AtomicU32::new(0), drop_beyond_patch: AtomicU32::new(0),
    positioner_hits: AtomicU32::new(0), patch_size_cm: AtomicU32::new(0),
};

impl PositionerCollisionProvider for CameraCollision<'_> {
    fn submit(&mut self, lines: [FatLine; 6], _context: u32) {
        // Context excludes the subject actor from native world queries. The
        // clone's BoardWorld contains world triangles only, never actor bodies.
        self.results = [no_hit(); 6];
        for (result, line) in self.results.iter_mut().zip(lines) {
            match super::world_query::line(self.world, line.start, line.end, line.radius) {
                Ok(hit) => {
                    if hit.hit != 0 { CAMERA_DEBUG.positioner_hits.fetch_add(1, Relaxed); }
                    *result = hit
                }
                Err(error) => {
                    self.error = Some(error.into());
                }
            }
        }
    }
    fn results(&mut self) -> [FatLineResult; 6] {
        self.results
    }
}

// 82DF3290 initializes both vectors, fraction and surface to zero on a miss.
fn no_hit() -> FatLineResult {
    FatLineResult {
        position: [0.0; 4],
        normal: [0.0; 4],
        fraction: 0.0,
        hit: 0,
        surface: 0,
    }
}

/// Horizontal extent (x, z) of the world triangles: the streamed collision patch.
fn patch_bounds(world: &BoardWorld) -> Option<(f32, f32, f32, f32)> {
    let mut b: Option<(f32, f32, f32, f32)> = None;
    for t in world.triangles() {
        for v in t.triangle.vertices {
            let (x, z) = (v.x, v.z);
            b = Some(match b {
                None => (x, z, x, z),
                Some((x0, z0, x1, z1)) => (x0.min(x), z0.min(z), x1.max(x), z1.max(z)),
            });
        }
    }
    b
}
