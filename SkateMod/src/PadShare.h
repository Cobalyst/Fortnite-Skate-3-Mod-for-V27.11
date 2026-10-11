#pragma once
// Shared memory between the mod (inside Fortnite) and PadReader.exe, the separate program that does
// every real controller read. Nothing in Fortnite's process calls the real XInput any more while the
// helper runs: with the pad in use each real read took milliseconds, and Fortnite asks for the pad on
// its game thread every frame (2026-10-11: reads of 5-9 ms there, 50-60 fps with the pad, fine on
// the keyboard). No Unreal dependencies.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace PadShare
{
    constexpr DWORD Magic = 0x50414453;   // 'PADS'
    constexpr DWORD Version = 1;

    struct Slot
    {
        volatile LONG Seq;          // odd while the helper writes this slot
        DWORD Result;               // XInputGetState's answer (0 = connected, 1167 = not connected)
        unsigned char State[16];    // XINPUT_STATE
        DWORD CapsResult;
        unsigned char Caps[20];     // XINPUT_CAPABILITIES (gamepad)
    };

    struct Block
    {
        DWORD Magic;
        DWORD Version;
        volatile LONG64 HeartbeatMs;   // helper: GetTickCount64() of its last pass
        volatile LONG64 WorstReadUs;   // helper: slowest real read since the mod last took it
        volatile LONG GameWantsInput;  // mod: Fortnite has XInput on and its window focused (frozen-pad kick allowed)
        volatile LONG Kicks;           // helper: XInput re-enables done for a frozen pad
        volatile LONG HelperPid;
        Slot Slots[4];
    };

    inline void Name(wchar_t* Out, size_t N, DWORD OwnerPid) { swprintf_s(Out, N, L"Local\\SkateModPad_%lu", OwnerPid); }

    // Reader side: a consistent copy of one slot.
    inline bool Read(const Block* B, int S, Slot& Out)
    {
        for (int Try = 0; Try < 64; Try++)
        {
            const LONG A = B->Slots[S].Seq;
            if (A & 1) { YieldProcessor(); continue; }
            MemoryBarrier();
            Out.Result = B->Slots[S].Result;
            memcpy(Out.State, B->Slots[S].State, sizeof(Out.State));
            Out.CapsResult = B->Slots[S].CapsResult;
            memcpy(Out.Caps, B->Slots[S].Caps, sizeof(Out.Caps));
            MemoryBarrier();
            if (B->Slots[S].Seq == A) return true;
        }
        return false;
    }
}
