#pragma once

// Port of Skate 3's skater/board audio game layer (TU3 default.xex, audio component methods
// 0x824A46B8..0x824B8528), driven by the engine's physics-side audio publication (SkAudio).
// Ported so far: the material collision sound system (0x82474D30 message -> 0x824BFE08 handler,
// 0x82484410 / 0x82484638 slot pick, 0x82484A98 intensity, 0x82484EC8 force class, 0x824C01B8
// voice volume/pitch), the board dispatcher 0x824A6F70 (landing 0x824A84C8, pop 0x824A7B60,
// grind contact 0x824A8F78) and the body region impacts 0x824AA020.
// Data: skate-data/sounds/aud_materials.txt (tools/export_aud_materials.py), trick_audio.txt
// (tools/export_trick_audio.py); tuning values are the game's own (see SkaterAudio.cpp).

#include "../../SkateEngine/skate_engine.h"

namespace SkaterAudio
{
    struct Backend
    {
        // Plays one SPLC id (event or group) of the collision bank of a material kind
        // (0 Skate_Collisions, 1 Skate_Metal, 2 HOM_Set_1) at a volume and pitch ratio.
        void (*PlaySplc)(int Kind, int Id, float Volume, float Pitch) = nullptr;
    };

    bool Load();                      // material + trick tables; false when missing
    void Reset();                     // entering skate mode
    // Ground material every Fortnite surface reports (Fortnite has no Skate 3 surface tags).
    void SetGroundMaterial(int Material);
    // bBodyHits: play body impact sounds this frame (the mod allows them only during a bail:
    // Fortnite's terrain gave the skeleton's contact regions stray hits while walking).
    void Update(const SkAudio& A, float Dt, const Backend& B, bool bBodyHits = true);
    // The trick table's audio fields of a trick (SkAudio::trick_id): first = flip sound type
    // (eSk8AudioTricks, Class_Flips / cloth_trick), second = the trick-end cloth type. -1 = none.
    int TrickAudio(int TrickId, bool bSecond);
    // Grind loop tuning (class 049861E8F9A8D16B): the grind surface (0..13) of the ground material
    // (AudioSurfaceMap word 4, 0x82482C58), its per-kind gain (0x824B0CA0) and emitter scale
    // (0x824B0B58), and the speed divisor (0x824B0708).
    int GrindSurface();
    float GrindGain(int Surface, int Kind);
    float GrindScale(int Surface, int Kind);
    float GrindSpeedDivisor();
    // Wheel skid surface (0..4) of the ground material: AudioSurfaceMap word 3 (0x824B5178).
    int SkidSurface();
    // Rolling surface (1..13) of the ground material: AudioSurfaceMap word 1 (0x82482B18).
    int RollingSurface();
    // Footstep surface class (AudioSurfaceMap word 6, 0x82482D98) of the ground material.
    int FootSurface();
    // Seam surface of the ground material (0x824B02C0): AudioSurfaceMap word 8 for a seam-pattern
    // hit, word 9 + 5 for a change of surface under a wheel.
    int SeamSurface(bool bSurfaceChange);
    // Foot drag surface (0x824A8228): AudioSurfaceMap word 5 (0x82482CF8); with State54 (the
    // back wheels' material) a result of 1 becomes 0.
    int FootDragSurface(bool bState54);
}
