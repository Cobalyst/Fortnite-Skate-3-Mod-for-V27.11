#pragma once

// Skate 3's input layer for the object dropper, from the game itself:
//  - the pad: FUN_82945db0 (XInput state -> 24 "Button" values: D-pad, Start, Back, stick clicks,
//    bumpers, triggers/255, A B X Y, then the four stick directions per stick with Skate 3's radial
//    dead zone: squared length r2 < 0.001 -> 0, else scale = clamp((r2 - 0.25) / 0.7, 0, 1) / r2);
//  - the actions: miscboot.big config/input.cfg ("GodHand_*" for the editor, "Apt*" for its menus),
//    evaluated as written there (e.g. GodHand_Cursor_MoveLeft = LStickL * LStickL * LStickL).
// The keyboard drives the same 24 buttons through the mod's key binds (additive: the controller
// mapping is Skate 3's and is not changed).

#include <cstdint>

namespace DropperInput
{
    // config/input.cfg "XBOX control scheme 1" button numbering.
    enum Button : int
    {
        DPadU, DPadD, DPadL, DPadR, Start, Back, LStick, RStick, LBumper, RBumper, LTrigger, RTrigger,
        A, B, X, Y, LStickR, LStickL, LStickU, LStickD, RStickR, RStickL, RStickU, RStickD, Count
    };

    struct State
    {
        float Value[Count] = {};
        bool Held[Count] = {}, Pressed[Count] = {}, Released[Count] = {}, Repeat[Count] = {};
    };

    // Once per frame: reads the controller (slot 0) and the keyboard, updates the button records.
    void Update(float Dt);
    const State& Get();
    void Reset();

    // ---- config/input.cfg actions used by the dropper ----
    // GodHand (editor core, read by slot in FUN_82864470.. via FUN_82944b08)
    float CursorMoveLeft();    float CursorMoveRight();  float CursorMoveForward();  float CursorMoveBack();
    float CursorElevateUp();   float CursorElevateDown();
    bool SimpleMoveLeft();     bool SimpleMoveRight();   bool SimpleMoveForward();   bool SimpleMoveBack();
    bool SimpleElevateUp();    bool SimpleElevateDown();
    float CameraRotateLeft();  float CameraRotateRight(); float CameraRotateUp();    float CameraRotateDown();
    float CameraZoomIn();      float CameraZoomOut();
    float RotateLeft();        float RotateRight();      float RotateUp();           float RotateDown();
    bool SimpleRotateLeft();   bool SimpleRotateRight(); bool SimpleRotateUp();      bool SimpleRotateDown();
    bool AdvancedModifier();   // LTrigger.held

    // Other actions the dropper code tests (FUN_82548008 ids)
    bool TopLeftShoulderHeld();  // 0x1d: LBumper.held
    bool SpectatorNext();        // 0x3d: RTrigger.pressed
    bool SpectatorPrev();        // 0x3e: LTrigger.pressed

    // FE screen events in the order the game numbers them (the Apt* actions of input.cfg):
    // the dropper screen's input handlers (825b3718, 825b2820, ...) switch on these codes.
    enum AptEvent : int
    {
        AptStart, AptSelect, AptNext, AptPrevious, AptOption, AptMisc, AptRShift1, AptRShift2, AptRShift3,
        AptLShift1, AptLShift2, AptLShift3, AptUp, AptDown, AptLeft, AptRight, AptDUp, AptDDown, AptDLeft, AptDRight,
        AptRStickUp, AptRStickDown, AptRStickLeft, AptRStickRight, AptLStickUp, AptLStickDown, AptLStickLeft, AptLStickRight,
        AptUpReleased, AptDownReleased, AptLeftReleased, AptRightReleased, AptDUpReleased, AptDDownReleased, AptDLeftReleased,
        AptDRightReleased, AptLStickUReleased, AptLStickDReleased, AptLStickLReleased, AptLStickRReleased, AptRStickUReleased,
        AptRStickDReleased, AptRStickLReleased, AptRStickRReleased, AptStartReleased, AptSelectReleased, AptNextReleased,
        AptPreviousReleased, AptOptionReleased, AptMiscReleased, AptRShift1Released, AptRShift2Released, AptRShift3Released,
        AptLShift1Released, AptLShift2Released, AptLShift3Released, AptEventCount
    };
    // Events that fired this frame, in code order.
    int Events(int* Out, int Max);
}
