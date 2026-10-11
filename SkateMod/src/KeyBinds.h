#pragma once

#include "Common.h"

// Keyboard bindings for every Skate 3 pad input plus the mod's own keys, remappable in game with a
// small menu (the - key: arrow keys pick, Enter rebinds, Backspace resets, Esc / - closes; F8 is ATLAS's). Saved in
// skate-data/settings/keybinds.txt. Controller input is not touched: these only feed the keyboard
// pad the engine uses when no controller is connected.
namespace KeyBinds
{
    enum Action : int
    {
        SteerLeft, SteerRight, Forward, Backward,  // left stick (on the board: steer, push, brake)
        Ollie,                                      // a quick right-stick ollie flick
        StickUp, StickDown, StickLeft, StickRight,  // right stick held (flick tricks, bail poses)
        Manual, NoseManual,                         // right stick half back / forward
        GrabLeft, GrabRight,                        // left / right trigger
        BumperLeft, BumperRight,                    // LB / RB
        ButtonA, ButtonB, ButtonX, ButtonY,
        DpadUp, DpadDown, DpadLeft, DpadRight,
        StickClickLeft, StickClickRight,            // L3 / R3
        Start, Back,
        ToggleSkate, BailNow, BoardSwing, HallOfMeat, MultiBodyFlips, BoardGraphic,  // mod keys
        Count
    };

    bool Down(Action A);              // false while the menu is open or the game is not focused
    int  KeyOf(Action A);             // virtual-key code (0 = unbound)

    void Tick();                      // every frame on the game thread (- menu input)
    bool MenuOpen();
    void DrawMenu(SDK::UCanvas* Canvas);
}
