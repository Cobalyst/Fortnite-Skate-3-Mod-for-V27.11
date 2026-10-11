#pragma once

// Fast, serialized XInput for the whole process (Fortnite's own pad reading and the skate engine's).
//
// With a Bluetooth Xbox controller (xinputhid) connected, Windows takes ~40 ms to report each EMPTY
// XInput slot, for xinput1_3 and xinput1_4 alike (measured 2026-10-07; the
// connected slot answers in ~0.1 ms). Every such query stalls the caller, which showed as heavy
// frame drops in both Fortnite and skate mode whenever the controller was in use. Calls from two
// threads at once (game thread + skate simulation thread) could also leave both reading a frozen pad.
//
// The hook answers "not connected" at once for slots known to be empty while another pad is
// connected, and runs every real call one at a time. Pad readings themselves are passed through
// unchanged (no mapping or dead-zone changes).
namespace XInputFix
{
    void Install();  // after MH_Initialize
    // This process hosts the match (ATLAS's background server window). While its window is not the
    // focused one it reads a neutral, unchanging pad: presses meant for the player's client window
    // reached it and its buttons resumed a paused storm (Y, then A; 2026-10-09).
    void SetServer(bool bServer);
    // Health log: the slowest real XInputGetState and the longest wait for the shared lock (ms)
    // since the last call.
    void TakeStats(double& WorstCallMs, double& WorstWaitMs);
    // A pad sent new data in the last half second (the profiler splits its samples by it).
    bool PadInUse();
    // While skating, Fortnite's own pad reads (on GameThread) get a neutral pad; everything else
    // (the skate engine's thread, reads inside a ModRead scope) gets the real one.
    void SetSkating(bool bOn, unsigned long GameThread);
    struct ModRead { ModRead(); ~ModRead(); };
}
