#pragma once

#include "EngineApi.h"

// Skate 3 board sounds, played with miniaudio on the default output device.
// Which decoded Skate 3 sound plays for each event is listed in skate-data/sounds/sounds.txt
// (written by Tools/pick_sounds.py, editable with the sound picker page).
namespace Audio
{
    struct Cues
    {
        int Tricks = 0;      // tricks announced this frame
        int Lines = 0;       // lines banked
        int Bails = 0;
        const char* Trick = "";  // newest trick's name
        float Spin[3] = {};      // deck angular velocity in its own frame, rad/s (x side, y up, z along)
        float Multiplier = 1.f;
        int Steps = 0;           // audible footsteps (walking)
        int Pushes = 0;          // push-foot contact events
        float PushSpeed = 0.f;
        const char* const* Fe = nullptr;  // Hall of Meat front-end sounds (sounds.txt cue names)
        int FeCount = 0;
        const SkAudio* Skater = nullptr;  // engine audio frame for the Skate 3 skater audio port (SkaterAudio)
    };

    void Preload();          // start loading the sound banks in the background
    void Start();            // entering skate mode
    void Stop();             // leaving skate mode (loops fade out)
    void Update(const SkState& Skater, float Dt, const Cues& Hud);

    // Diagnostics for the once-a-minute health log: what the sound system holds right now.
    struct Stats { int Voices = 0, Modules = 0, Classes = 0, OneShots = 0; };
    Stats GetStats();
}
