#pragma once

#include "Common.h"
#include "Math.h"
#include <string>
#include <vector>

// Shows the skater: the Skate 3 pose retargeted onto the Fortnite character, and the
// Skate 3 board built from its own mesh and textures.
//
// The character's body mesh (and every part that follows it) is pointed at a hidden
// UPoseableMeshComponent ("leader pose"), whose bones are set each frame from the skate
// engine. Skins, materials and anything attached to body sockets keep working.
// While an emote montage plays the original setup is restored, so emotes play normally
// while the board keeps rolling.
namespace Avatar
{
    // Called once the skater is active (engine loaded, character known).
    bool Begin(SDK::ACharacter* Character);
    // Every frame while skating, after the engine step.
    void Update(SDK::ACharacter* Character);
    // Diagnostics: logs where the board is relative to the hips through a No-Comply / Boneless.
    void TraceTrick(const char* State, const float* Velocity, float Yaw);
    // Leaving skate mode (Character may be null if it was destroyed).
    void End(SDK::ACharacter* Character);
    // The deck graphic: the equipped Fortnite vehicle wrap (true) or Skate 3's own board texture.
    // Applies at once to the current board.
    void SetBoardWrap(SDK::ACharacter* Character, bool bWrap);
    bool BoardWrap();
    void SetWrapMaterialMode(bool bOn);   // mod_settings board_wrap_material (test)
    // Wrap survey (mod_settings wrap_survey 1): logs the deck texture every weapon wrap in the game would give.
    void RequestWrapSurvey();
    bool WrapSurveyStep(int Budget);   // game thread, every frame; false when not running
}
