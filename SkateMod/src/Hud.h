#pragma once

#include "Common.h"

// Skate 3 trickline HUD: Skate 3's own trickdisplay2 movie (art, animation and ActionScript)
// played by the APT player and fed from the skate engine's ScoreModule (SkateHud.cpp).
namespace Hud
{
    void Start();               // entering skate mode
    void Stop();                // leaving skate mode (the HUD fades out)
    void Poll(float Dt);        // every frame on the game thread: drain engine HUD events
    void Draw(SDK::UCanvas* Canvas);  // every frame, on the game viewport's canvas

    // Trickline sound cues (filled by Poll, consumed by Audio).
    struct Cues
    {
        int Tricks = 0; int Lines = 0; int Bails = 0;
        char Trick[64] = {};         // name of the newest trick (this frame)
        float Spin[3] = {};          // deck angular velocity in its own frame (rad/s)
        float Multiplier = 1.f;
        int Steps = 0;               // audible footsteps (walking) this frame
        int Pushes = 0;              // push-foot ground contact events this frame
        float PushSpeed = 0.f;
        // Hall of Meat front-end sounds this frame (Skate 3 'fe' entries: hom_transition and
        // hom_score_red from the HUD movie, hom_<part> when the x-ray skeleton first shows).
        const char* Fe[4] = {};
        int FeCount = 0;
        // Strongest contact force this frame of the skeleton contact regions Skate 3's skater audio
        // listens to: head, torso, left arm, right arm, left leg, right leg (0 = no contact).
        float BodyForce[6] = {};
    };
    Cues TakeCues();
    // A Skate 3 'fe' sound cue (sounds.txt name; the string must outlive the frame).
    void QueueFeSound(const char* Name);
    // The LB session marker menu is up (LB held while skating).
    bool MarkerMenuOpen();
    // The object dropper may be offered in the marker menu.
    void SetDropperAvailable(bool bAvailable);

    // Session marker counters from the engine: markers placed and gone to so far, and whether one
    // is set now (the prop list follows them).
    void MarkerCounts(uint32_t& Placed, uint32_t& Returned, bool& bSet);
    // Skate 3's action overlay (actionoverlay2) while the skater holds a prop: whether it shows and
    // its five DMO_GetParams flags (see ActionOverlay.h).
    void SetActionOverlay(bool bShow, const bool Params[5]);

    // A short line of text in the middle of the screen (mode toggles), shown for Seconds.
    void Notice(const wchar_t* Text, float Seconds = 2.5f);

    // Hall of Meat x-ray skeleton on / off (mod_settings.txt: xray).
    void SetXray(bool bOn);
}
