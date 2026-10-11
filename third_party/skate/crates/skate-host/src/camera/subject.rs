//! Typed physics/animation publication boundary for normal subject82DF69C0.
//! The simulation supplies current physical outputs; camera history stays here.
use skate_core::camera::{
    AnchorInputs, Anchors, CameraMan, Compass, CompassPoseInputs, CompassSettings,
    ManagerSubject, ReferencePointInputs, SubjectPoseInputs, SubjectPosePublisher,
};

#[derive(Clone, Copy, Debug)]
pub(crate) struct CameraSubjectSnapshot {
    /// Completed physics tick that produced every field in this snapshot.
    pub tick: u64,
    /// Physical flags, trajectory, steering, root and other direct outputs.
    /// The camera overwrites this value's transform, reference_positions,
    /// anchors and compass with the source publishers below before use.
    pub subject: ManagerSubject,
    pub pose: SubjectPoseInputs,
    pub anchors: AnchorInputs,
    /// Bone positions and raw COM; tracked_anchor, incline_normal and damped
    /// COM are filled by their camera owners immediately before publication.
    pub reference_points: ReferencePointInputs,
    pub compass: CompassPoseInputs,
    pub graph: super::graph_subject::CameraGraphSubject,
}

pub(super) struct SubjectPublisher {
    pose: SubjectPosePublisher,
    anchors: Anchors,
    compass: Compass,
    last_tick: Option<u64>,
    /// No-Comply / Boneless camera hold: deck offset from the centre of mass when the trick
    /// began, and the remaining blend back onto the deck (1 = fully held) after it ends.
    trick_hold: Option<[f32; 3]>,
    trick_blend: f32,
    /// Board velocity and spin of the last tick before the trick (the kick throws them to tens of
    /// m/s, which swung the camera's look-ahead and heading), held through it and blended back.
    pre_trick_motion: ([f32; 4], [f32; 4]),
    held_motion: Option<([f32; 4], [f32; 4])>,
}
impl SubjectPublisher {
    pub fn new() -> Self {
        Self {
            pose: SubjectPosePublisher::new(),
            anchors: Anchors::new(),
            compass: Compass::new(),
            last_tick: None,
            trick_hold: None,
            trick_blend: 0.0,
            pre_trick_motion: ([0.0; 4], [0.0; 4]),
            held_motion: None,
        }
    }

    pub fn publish(&mut self, mut input: CameraSubjectSnapshot, manager: &CameraMan,
        settings: CompassSettings) -> Result<ManagerSubject, String> {
        if let Some(last_tick) = self.last_tick && input.tick <= last_tick {
            return Err(format!(
                "Camera subject publication is not monotonic: previous={}, current={}",
                last_tick, input.tick,
            ));
        }
        self.last_tick = Some(input.tick);
        self.hold_through_trick(&mut input);
        let pose = self.pose.publish(input.pose);
        input.subject.rig.transform = pose.transform;
        input.subject.rig.skeleton_root = input.pose.skeleton_root;
        input.anchors.damped_center_of_mass = pose.damped_center_of_mass;
        self.anchors.update(input.anchors);
        input.subject.anchors = self.anchors.entries;
        input.reference_points.damped_centre_of_mass = pose.damped_center_of_mass;
        input.reference_points.tracked_anchor = manager.rig.anchor.position;
        input.reference_points.incline_normal = manager.state.incline_normal;
        input.subject.rig.reference_positions = input.reference_points.positions();
        let selected = if manager.shots.current().name.is_empty() { 5 }
            else { manager.shots.current().shot.compass_north };
        let compass_input = input.compass.bind(&input.subject, manager.frame.position, selected);
        input.subject.compass = self.compass.update(
            if input.subject.reset != 0 { 0.0 } else { f32::from_bits(0x3c88_8889) },
            compass_input, settings);
        Ok(input.subject)
    }

    /// While a No-Comply / Boneless is under way (the graph's SkaterIsPerformingBoneless flag),
    /// the deck is thrown up and back under the planted foot, and the camera, which tracks the
    /// deck's position, read that as a sharp turn and swung sideways. During the trick the camera
    /// follows the skater's centre of mass instead (keeping the deck's offset from when the trick
    /// began), then eases back onto the deck over a quarter second. Nothing changes otherwise.
    fn hold_through_trick(&mut self, input: &mut CameraSubjectSnapshot) {
        const BLEND_PER_TICK: f32 = 1.0 / 15.0; // 0.25 s at the 60 Hz physics tick
        let deck = input.pose.physical_transform[3];
        let com = input.pose.center_of_mass;
        let live = (input.anchors.board_velocity, input.anchors.board_acceleration);
        let motion = if input.graph.boneless {
            *self.held_motion.get_or_insert(self.pre_trick_motion)
        } else if let Some(held) = self.held_motion {
            let t = (self.trick_blend - BLEND_PER_TICK).max(0.0);
            if t <= 0.0 {
                self.held_motion = None;
            }
            let mix = |a: [f32; 4], b: [f32; 4]| core::array::from_fn(|i| (a[i] - b[i]).mul_add(t, b[i]));
            (mix(held.0, live.0), mix(held.1, live.1))
        } else {
            self.pre_trick_motion = live;
            live
        };
        input.anchors.board_velocity = motion.0;
        input.anchors.board_acceleration = motion.1;
        input.compass.board_velocity = motion.0;
        if input.graph.boneless {
            let offset = *self.trick_hold.get_or_insert([deck[0] - com[0], deck[1] - com[1], deck[2] - com[2]]);
            self.trick_blend = 1.0;
            input.pose.physical_transform[3] = [com[0] + offset[0], com[1] + offset[1], com[2] + offset[2], deck[3]];
        } else if let Some(offset) = self.trick_hold {
            self.trick_blend -= BLEND_PER_TICK;
            if self.trick_blend <= 0.0 {
                self.trick_hold = None;
                self.trick_blend = 0.0;
            } else {
                let t = self.trick_blend;
                input.pose.physical_transform[3] = [
                    (com[0] + offset[0] - deck[0]).mul_add(t, deck[0]),
                    (com[1] + offset[1] - deck[1]).mul_add(t, deck[1]),
                    (com[2] + offset[2] - deck[2]).mul_add(t, deck[2]),
                    deck[3],
                ];
            }
        }
    }
}
