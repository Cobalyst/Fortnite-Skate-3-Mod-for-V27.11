#pragma once

// Skate 3's object-dropper editor core ("GodHand", 0x82830000-0x82880000 in the retail executable),
// ported function by function: the orbit camera (828603c0 / 82866498), the cursor (82861ad0 and
// helpers) and the manipulation of a held object (82865968), with the tuning of the attribute
// records editor_camera/objectdropper, editor_cursor/objectdropper and objectdropper/<mode>.
// Unreal space (cm, Z up); Skate 3's metre values are scaled by 100 where they are distances.
// Updates run at Skate 3's fixed rate: the original steps are per update, not per second.

#include "Common.h"
#include "DropperWorld.h"

namespace DropperEditor
{
    // FUN_82864ba8 modes (+0x868)
    enum Mode : int { Off = 0, FreeCam = 1, Rotate = 2, LockAxis = 3 };

    // objectdropper/<key> record (FUN_82864d68 picks it by key)
    enum Tuning : int { Default, FreeCamTuning, Manipulate, ObjectSpawner };

    void Enter(const SDK::FVector& Start, double FacingYaw);   // 82864dd0: cursor and camera at the start
    void Exit();
    bool Active();

    void SetMode(Mode M);           // FUN_82864ba8
    Mode GetMode();
    void SetTuning(Tuning T);       // FUN_82864d68

    // Once per frame; runs as many fixed updates as the time allows (reads DropperInput itself).
    void Tick(SDK::UWorld* World, float Dt);
    // One fixed update with the input DropperInput already read (DropperScreen runs the updates).
    void UpdateStep(SDK::UWorld* World);
    // The player's character: the camera's collision sweep (8285fbf8) passes through it.
    void SetViewer(SDK::AActor* Character);

    // After the frame's updates: Alpha (0..1) = how far the frame is into the next update. The
    // camera and the carried object are drawn between the last two updates (the updates themselves
    // keep Skate 3's fixed rate and steps).
    void Present(double Alpha);

    // The camera for the view (Unreal): location and rotation.
    bool View(SDK::FVector& Location, SDK::FRotator& Rotation, float& FovDegrees);

    // Cursor and held object
    SDK::FVector CursorPosition();
    void SetCursor(const SDK::FVector& At);   // moves the cursor (and its leash centre) there
    SDK::FVector ViewLocation();
    SDK::FVector ViewForward();
    int Held();                     // DropperWorld handle of the carried object, 0 = none
    void Hold(int Handle, bool bFrame = true);   // carry this object (it follows the cursor); bFrame = 82866a70's zoom to it
    int Release();                  // stop carrying; returns the handle (a group's others stay where they are)
    // A group select's objects (the cursor's group, cursor +4): Handle is carried and the others
    // follow it, keeping where they are relative to it.
    void HoldGroup(int Handle, const std::vector<int>& Others, bool bFrame = true);
    const std::vector<int>& HeldGroup();   // the others carried with Held()
    bool HeldValid();                       // it can be put down here (not red)
    void FinishSteps();                     // once per frame after UpdateStep: snapping, placement check
    int Hovered();                  // object under the cursor (82861270 picker), 0 = none
    void ResetRotation();           // 828624a0

    // The catalog (825b91d0 / 82860270)
    void SetCatalogCamera(bool bOn);           // +0x7e8: camera stays, looks at the cursor
    SDK::FVector SpawnCursor();                // cursor to eye + forward * distance; returns it
    DropperWorld::Quat SpawnRotation();        // cursor rotation (+0xe0) a spawned object gets
    void CursorToCamera();                     // the carried object went: cursor back to the eye
    void Placed(int Handle);                   // 828668d8: placed; its rotation kept, cursor to the eye

    // The properties menu's options (editor +0x8ac bits 7 snap / 6 collision / 5 invert X /
    // 4 invert Y, +0x8b0 cursor speed; 828649b0 starts with snap and collision on, speed 1).
    struct Options { bool Snap = true; bool Collision = true; bool InvertX = false; bool InvertY = false; float CursorSpeed = 1.f; };
    Options GetOptions();
    void SetOptions(const Options& O);   // invert X / Y: +0x874 / +0x878 = -1 or 1 (825b63c0)
}
