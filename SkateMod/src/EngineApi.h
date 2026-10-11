#pragma once

// skate_engine.dll, loaded at runtime (see Client.cpp).
#include "../../SkateEngine/skate_engine.h"

struct EngineApi
{
    sk_create_fn               Create = nullptr;
    sk_set_collision_fn        SetCollision = nullptr;
    sk_set_collision2_fn       SetCollision2 = nullptr;  // with native Skate 3 triangles (dropper objects)
    sk_collision_generation_fn CollisionGeneration = nullptr;
    sk_activate_fn             Activate = nullptr;
    sk_step_fn                 Step = nullptr;
    sk_suspend_fn              Suspend = nullptr;
    sk_set_paused_fn           SetPaused = nullptr;  // optional (older engines lack it): freeze the skater
    sk_bone_count_fn           BoneCount = nullptr;
    sk_bone_name_fn            BoneName = nullptr;
    sk_bone_bind_fn            BoneBind = nullptr;
    sk_get_bones_fn            GetBones = nullptr;
    sk_get_hud_fn              GetHud = nullptr;
    sk_pop_hud_event_fn        PopHudEvent = nullptr;
    sk_board_part_count_fn     BoardPartCount = nullptr;
    sk_board_part_fn           BoardPart = nullptr;
    sk_last_error_fn           LastError = nullptr;
    sk_set_option_fn           SetOption = nullptr;
    sk_get_pad_debug_fn GetPadDebug = nullptr;  // optional (older engines lack it)
    sk_get_camera_debug_fn GetCameraDebug = nullptr;  // optional
    sk_get_audio_fn            GetAudio = nullptr;   // optional: skater audio frame (SkaterAudio port)
    sk_set_dmos_fn             SetDmos = nullptr;    // optional: object-dropper props as rigid bodies
    sk_get_dmo_poses_fn        GetDmoPoses = nullptr;
    sk_upright_dmo_fn          UprightDmo = nullptr;   // optional: the action overlay's Upright Object
};

extern EngineApi Api;
extern SkEngine* Engine;
