#include "DropperInput.h"
#include "XInputFix.h"

#include "Common.h"
#include "KeyBinds.h"

#include <Xinput.h>
#include <algorithm>
#include <cmath>

namespace DropperInput
{
    namespace
    {
        State Cur;
        float HeldFor[Count] = {};      // seconds each button has been held (for .repeat)
        float NextRepeat[Count] = {};

        // TODO(exact): Skate 3's .held threshold and .repeat timing live in the input VM / pad cache
        // (82944b08 reads per-button flag bytes +0xc..+0xf); not yet located. Until then: held = value
        // at or above one half, repeat = the press, then every RepeatRate seconds after RepeatDelay.
        constexpr float HeldThreshold = 0.5f;
        constexpr float RepeatDelay = 0.5f, RepeatRate = 0.1f;

        using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        XInputGetStateFn GetStateFn()
        {
            static XInputGetStateFn Fn = nullptr;
            static bool bTried = false;
            if (!bTried)
            {
                bTried = true;
                HMODULE H = GetModuleHandleA("xinput1_3.dll");
                if (!H) H = LoadLibraryA("xinput1_3.dll");
                if (H) Fn = reinterpret_cast<XInputGetStateFn>(GetProcAddress(H, "XInputGetState"));
            }
            return Fn;
        }

        // FUN_82945db0: one stick -> four direction buttons with the radial dead zone.
        void Stick(float X, float Y, float* Out /* R, L, U, D */)
        {
            const float R2 = X * X + Y * Y;
            float S = 0.f;
            if (R2 >= 0.001f)
            {
                float T = (R2 - 0.25f) * 1.4285714f;   // 0x822f3b90 = 1 / 0.7
                T = T < 0.f ? 0.f : (T > 1.f ? 1.f : T);
                S = T / R2;
            }
            const float Sx = X * S, Sy = Y * S;
            Out[0] = std::max(Sx, 0.f);
            Out[1] = std::max(-Sx, 0.f);
            Out[2] = std::max(Sy, 0.f);
            Out[3] = std::max(-Sy, 0.f);
        }
    }

    // Buttons already down when the dropper opens (the B of LB + B) must not count as presses.
    bool bAbsorb = false;

    void Reset()
    {
        Cur = State{};
        bAbsorb = true;
        for (int i = 0; i < Count; i++) { HeldFor[i] = 0; NextRepeat[i] = 0; }
    }

    void Update(float Dt)
    {
        float V[Count] = {};
        // Controller, exactly as FUN_82945db0 reads XINPUT_STATE.
        XINPUT_STATE Xs{};
        XInputFix::ModRead Mine;   // the real pad, also while Fortnite gets a neutral one
        if (auto Fn = GetStateFn(); Fn && Fn(0, &Xs) == ERROR_SUCCESS)
        {
            const WORD W = Xs.Gamepad.wButtons;
            const WORD Bits[10] = { XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
                                    XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
                                    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER };
            for (int i = 0; i < 10; i++) V[i] = (W & Bits[i]) ? 1.f : 0.f;
            V[LTrigger] = float(Xs.Gamepad.bLeftTrigger) * 0.00392157f;
            V[RTrigger] = float(Xs.Gamepad.bRightTrigger) * 0.00392157f;
            V[A] = (W & XINPUT_GAMEPAD_A) ? 1.f : 0.f;
            V[B] = (W & XINPUT_GAMEPAD_B) ? 1.f : 0.f;
            V[X] = (W & XINPUT_GAMEPAD_X) ? 1.f : 0.f;
            V[Y] = (W & XINPUT_GAMEPAD_Y) ? 1.f : 0.f;
            constexpr float Scale = 3.05176e-05f;   // 1 / 32768
            Stick(float(Xs.Gamepad.sThumbLX) * Scale, float(Xs.Gamepad.sThumbLY) * Scale, &V[LStickR]);
            Stick(float(Xs.Gamepad.sThumbRX) * Scale, float(Xs.Gamepad.sThumbRY) * Scale, &V[RStickR]);
        }
        // Keyboard: the same buttons from the mod's key binds; sticks at full tilt on the unit circle.
        if (GameWindowFocused() && !KeyBinds::MenuOpen())
        {
            using KB = KeyBinds::Action;
            auto K = [](KB Act) { return KeyBinds::Down(Act) ? 1.f : 0.f; };
            V[DPadU] = std::max(V[DPadU], K(KB::DpadUp));
            V[DPadD] = std::max(V[DPadD], K(KB::DpadDown));
            V[DPadL] = std::max(V[DPadL], K(KB::DpadLeft));
            V[DPadR] = std::max(V[DPadR], K(KB::DpadRight));
            V[Start] = std::max(V[Start], K(KB::Start));
            V[Back] = std::max(V[Back], K(KB::Back));
            V[LStick] = std::max(V[LStick], K(KB::StickClickLeft));
            V[RStick] = std::max(V[RStick], K(KB::StickClickRight));
            V[LBumper] = std::max(V[LBumper], K(KB::BumperLeft));
            V[RBumper] = std::max(V[RBumper], K(KB::BumperRight));
            V[LTrigger] = std::max(V[LTrigger], K(KB::GrabLeft));
            V[RTrigger] = std::max(V[RTrigger], K(KB::GrabRight));
            V[A] = std::max(V[A], K(KB::ButtonA));
            V[B] = std::max(V[B], K(KB::ButtonB));
            V[X] = std::max(V[X], K(KB::ButtonX));
            V[Y] = std::max(V[Y], K(KB::ButtonY));
            auto KeyStick = [&](float Xk, float Yk, int First)
            {
                if (Xk == 0.f && Yk == 0.f) return;
                const float L = std::sqrt(Xk * Xk + Yk * Yk);
                float S[4];
                Stick(Xk / L, Yk / L, S);
                for (int i = 0; i < 4; i++) V[First + i] = std::max(V[First + i], S[i]);
            };
            KeyStick(K(KB::SteerRight) - K(KB::SteerLeft), K(KB::Forward) - K(KB::Backward), LStickR);
            KeyStick(K(KB::StickRight) - K(KB::StickLeft), K(KB::StickUp) - K(KB::StickDown), RStickR);
        }
        for (int i = 0; i < Count; i++)
        {
            const bool bWas = bAbsorb ? V[i] >= HeldThreshold : Cur.Held[i];
            Cur.Value[i] = V[i];
            Cur.Held[i] = V[i] >= HeldThreshold;
            Cur.Pressed[i] = Cur.Held[i] && !bWas;
            Cur.Released[i] = !Cur.Held[i] && bWas;
            Cur.Repeat[i] = false;
            if (Cur.Pressed[i]) { Cur.Repeat[i] = true; HeldFor[i] = 0; NextRepeat[i] = RepeatDelay; }
            else if (bAbsorb) { HeldFor[i] = 0; NextRepeat[i] = RepeatDelay; }
            else if (Cur.Held[i])
            {
                HeldFor[i] += Dt;
                if (HeldFor[i] >= NextRepeat[i]) { Cur.Repeat[i] = true; NextRepeat[i] += RepeatRate; }
            }
        }
        bAbsorb = false;
    }

    const State& Get() { return Cur; }

    namespace
    {
        float Cube(Button B) { const float V = Cur.Value[B]; return V * V * V; }
        float HeldF(Button B) { return Cur.Held[B] ? 1.f : 0.f; }
    }

    // config/input.cfg "Object Dropper" section
    float CursorMoveLeft() { return Cube(LStickL); }
    float CursorMoveRight() { return Cube(LStickR); }
    float CursorMoveForward() { return Cube(LStickU); }
    float CursorMoveBack() { return Cube(LStickD); }
    float CursorElevateUp() { return HeldF(DPadU); }
    float CursorElevateDown() { return HeldF(DPadD); }
    bool SimpleMoveLeft() { return Cur.Repeat[LStickL]; }
    bool SimpleMoveRight() { return Cur.Repeat[LStickR]; }
    bool SimpleMoveForward() { return Cur.Repeat[LStickU]; }
    bool SimpleMoveBack() { return Cur.Repeat[LStickD]; }
    bool SimpleElevateUp() { return Cur.Repeat[DPadU]; }
    bool SimpleElevateDown() { return Cur.Repeat[DPadD]; }
    float CameraRotateLeft() { return Cube(RStickL); }
    float CameraRotateRight() { return Cube(RStickR); }
    float CameraRotateUp() { return Cube(RStickU); }
    float CameraRotateDown() { return Cube(RStickD); }
    float CameraZoomIn() { return HeldF(DPadR); }
    float CameraZoomOut() { return HeldF(DPadL); }
    float RotateLeft() { return Cube(LStickL); }
    float RotateRight() { return Cube(LStickR); }
    float RotateUp() { return Cube(LStickU); }
    float RotateDown() { return Cube(LStickD); }
    bool SimpleRotateLeft() { return Cur.Repeat[LStickL]; }
    bool SimpleRotateRight() { return Cur.Repeat[LStickR]; }
    bool SimpleRotateUp() { return Cur.Repeat[LStickU]; }
    bool SimpleRotateDown() { return Cur.Repeat[LStickD]; }
    bool AdvancedModifier() { return Cur.Held[LTrigger]; }

    bool TopLeftShoulderHeld() { return Cur.Held[LBumper]; }
    bool SpectatorNext() { return Cur.Pressed[RTrigger]; }
    bool SpectatorPrev() { return Cur.Pressed[LTrigger]; }

    int Events(int* Out, int Max)
    {
        const State& S = Cur;
        bool E[AptEventCount] = {};
        E[AptStart] = S.Pressed[Start];
        E[AptSelect] = S.Pressed[Back];
        E[AptNext] = S.Pressed[A];
        E[AptPrevious] = S.Pressed[B];
        E[AptOption] = S.Pressed[Y];
        E[AptMisc] = S.Pressed[X];
        E[AptRShift1] = S.Pressed[RBumper];
        E[AptRShift2] = S.Pressed[RTrigger];
        E[AptRShift3] = S.Pressed[RStick];
        E[AptLShift1] = S.Pressed[LBumper];
        E[AptLShift2] = S.Pressed[LTrigger];
        E[AptLShift3] = S.Pressed[LStick];
        E[AptUp] = S.Repeat[LStickU] || S.Repeat[DPadU];
        E[AptDown] = S.Repeat[LStickD] || S.Repeat[DPadD];
        E[AptLeft] = S.Repeat[LStickL] || S.Repeat[DPadL];
        E[AptRight] = S.Repeat[LStickR] || S.Repeat[DPadR];
        E[AptDUp] = S.Repeat[DPadU];
        E[AptDDown] = S.Repeat[DPadD];
        E[AptDLeft] = S.Repeat[DPadL];
        E[AptDRight] = S.Repeat[DPadR];
        E[AptRStickUp] = S.Repeat[RStickU];
        E[AptRStickDown] = S.Repeat[RStickD];
        E[AptRStickLeft] = S.Repeat[RStickL];
        E[AptRStickRight] = S.Repeat[RStickR];
        E[AptLStickUp] = S.Repeat[LStickU];
        E[AptLStickDown] = S.Repeat[LStickD];
        E[AptLStickLeft] = S.Repeat[LStickL];
        E[AptLStickRight] = S.Repeat[LStickR];
        E[AptUpReleased] = S.Released[LStickU] || S.Released[DPadU];
        E[AptDownReleased] = S.Released[LStickD] || S.Released[DPadD];
        E[AptLeftReleased] = S.Released[LStickL] || S.Released[DPadL];
        E[AptRightReleased] = S.Released[LStickR] || S.Released[DPadR];
        E[AptDUpReleased] = S.Released[DPadU];
        E[AptDDownReleased] = S.Released[DPadD];
        E[AptDLeftReleased] = S.Released[DPadL];
        E[AptDRightReleased] = S.Released[DPadR];
        E[AptLStickUReleased] = S.Released[LStickU];
        E[AptLStickDReleased] = S.Released[LStickD];
        E[AptLStickLReleased] = S.Released[LStickL];
        E[AptLStickRReleased] = S.Released[LStickR];
        E[AptRStickUReleased] = S.Released[RStickU];
        E[AptRStickDReleased] = S.Released[RStickD];
        E[AptRStickLReleased] = S.Released[RStickL];
        E[AptRStickRReleased] = S.Released[RStickR];
        E[AptStartReleased] = S.Released[Start];
        E[AptSelectReleased] = S.Released[Back];
        E[AptNextReleased] = S.Released[A];
        E[AptPreviousReleased] = S.Released[B];
        E[AptOptionReleased] = S.Released[Y];
        E[AptMiscReleased] = S.Released[X];
        E[AptRShift1Released] = S.Released[RBumper];
        E[AptRShift2Released] = S.Released[RTrigger];
        E[AptRShift3Released] = S.Released[RStick];
        E[AptLShift1Released] = S.Released[LBumper];
        E[AptLShift2Released] = S.Released[LTrigger];
        E[AptLShift3Released] = S.Released[LStick];
        int N = 0;
        for (int i = 0; i < AptEventCount && N < Max; i++)
            if (E[i]) Out[N++] = i;
        return N;
    }
}
