//! Animation attributes become owned physical inputs through the actual
//! Skeleton dispatcher. This adapter loads stock settings and retains caches;
//! it does not choose animations or manufacture a push from controller input.
use skate_core::{
    animation::{
        output::{
            NativeMatrix,
            attributes::{AnimationAttribute, AttributeName},
        },
        skeleton_input::{
            attribute_finalization::{FinalizationInput, JumpAttributeState},
            contact_events::{ContactEventPose, ContactEventState},
            extended_attributes::ExtendedAttributes,
            name::encode,
            process_attributes,
            scalar_attributes::{AnimationControlOutput, ScalarAttributeInputs},
        },
    },
    input::controller::ActionMap,
};
use skate_data::{animation_frames::AnimationFrames, collections::Collections};

pub(crate) struct AnimationInput {
    pub fields: ScalarAttributeInputs,
    pub extra: ExtendedAttributes,
    /// (Fortnite mod) strongest AudibleFootStepStrength since the bridge last read it.
    pub footstep_latch: f32,
    /// (Fortnite mod) push foot touched down (push_contact strength 1) since the bridge last read it.
    pub push_latch: f32,
    /// (Fortnite mod) brake foot touched down (brake_contact strength 1).
    pub brake_latch: bool,
    pub contacts: ContactEventState,
    pub output: AnimationControlOutput,
    cached_jump: JumpAttributeState,
    bone_names: Vec<AttributeName>,
    right_toe: usize,
    settings: FinalizationInput,
    height_overrides: [bool; 5],
}

impl AnimationInput {
    pub fn load(data: &Collections, frames: &AnimationFrames, mode: &str) -> Result<Self, String> {
        let right_toe = frames
            .bone_names
            .iter()
            .position(|n| n.eq_ignore_ascii_case("RightToeBase"))
            .ok_or("Stock skeleton is missing RightToeBase")?;
        Ok(Self {
            height_overrides: crate::difficulty::NATIVE_MODES.map(|key|
                data.boolean("physics_mode", key, "JumpHeightOverrideEnabled"))
                .into_iter().collect::<Result<Vec<_>, _>>()?.try_into().unwrap(),
            fields: ScalarAttributeInputs::reset(0, 0),
            extra: ExtendedAttributes::reset(0.0),
            footstep_latch: 0.0,
            push_latch: 0.0,
            brake_latch: false,
            contacts: ContactEventState {
                bone: 0,
                push_speed: 0.0,
            },
            cached_jump: JumpAttributeState::new(),
            // AnimOut construction82DB1E88..1EFC initializes the null name,
            // float24=0 and clears all represented high flag bits.
            output: AnimationControlOutput {
                grind_name: encode(b""),
                flags: 0,
            },
            bone_names: frames
                .bone_names
                .iter()
                .map(|n| encode(n.as_bytes()))
                .collect(),
            right_toe,
            settings: FinalizationInput {
                //TU3 global constructor8289F8B4 binds292 to anim_motion
                //collectionAC260B44FA3CA0A4 (jumping), not a default profile.
                select_jump_extremes: data.boolean("anim_motion", "jumping", "clamp_jump")?,
                low_jump_threshold: data.float("anim_motion", "jumping", "clamp_low_inclusive")?,
                high_jump_threshold: data.float(
                    "anim_motion",
                    "jumping",
                    "clamp_high_inclusive",
                )?,
                allow_height_override: data.boolean(
                    "physics_mode",
                    mode,
                    "JumpHeightOverrideEnabled",
                )?,
                use_prepared_controls: data.boolean(
                    "physics_jump",
                    "default",
                    "AdjustOnPrepare",
                )?,
                external_impulse_active: false,
                animation_flags: 0,
            },
        })
    }

    pub fn select_physics_mode(&mut self, mode: u32) -> Result<(), String> {
        self.settings.allow_height_override = *self.height_overrides.get(mode as usize)
            .ok_or_else(|| format!("Invalid animation physics mode {mode}"))?;
        Ok(())
    }

    /// ProcessedPhysIn Reset precedes physical/packet flag publication. The
    /// caller then publishes those flags before dispatching the current pose.
    pub fn reset_processed(&mut self) {
        self.fields = ScalarAttributeInputs::reset(self.fields.flags2468, self.fields.flags2488);
        self.extra = ExtendedAttributes::reset(self.extra.footstep_strength);
        self.contacts = ContactEventState {
            bone: 0,
            push_speed: 0.0,
        };
    }

    ///82DB7DCC..7E0C runs AFTER publishing AnimOut into the physical output.
    /// It is distinct from ProcessedPhysIn reset at the start of input.
    pub fn finish_output_publication(&mut self) {
        self.output.grind_name = encode(b"");
        self.output.flags &= 0x003fffff;
        self.footstep_latch = self.footstep_latch.max(self.extra.footstep_strength);
        self.extra.footstep_strength = 0.0;
    }

    pub fn process(
        &mut self,
        attributes: &[AnimationAttribute],
        hierarchy: &[NativeMatrix],
        timestep: f32,
        animation_flags: u32,
        external_impulse_active: bool,
        map: &mut dyn ActionMap,
    ) -> Result<(), String> {
        let pose = ContactEventPose {
            bone_names: &self.bone_names,
            hierarchy,
            trajectory_bone: 0,
            right_toe_bone: self.right_toe as i32,
            timestep,
        };
        let settings = FinalizationInput {
            animation_flags,
            external_impulse_active,
            ..self.settings
        };
        let result = process_attributes::process(
            attributes,
            &pose,
            &mut self.fields,
            &mut self.extra,
            &mut self.contacts,
            &mut self.cached_jump,
            &mut self.output,
            settings,
            Some(map),
        );
        // push_contact sets 2468 bit 27 with bit 25 = foot down; brake_contact bit 28 with bit 30.
        let f = self.fields.flags2468;
        if f & (1 << 27) != 0 && f & (1 << 25) != 0 {
            self.push_latch = self.push_latch.max(self.contacts.push_speed.max(0.01));
        }
        if f & (1 << 28) != 0 && f & (1 << 30) != 0 {
            self.brake_latch = true;
        }
        result
    }
}
