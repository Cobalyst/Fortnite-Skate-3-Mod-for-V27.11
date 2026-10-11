#pragma once

// The game window's raw-input controller messages (2026-10-10 profiler: with the Bluetooth pad in
// use, the game thread's message pump (PeekMessageW) and other window calls slowed down and skate
// mode fell to 12 fps; with the pad idle it ran at 60-70). Fortnite and the skate engine read the
// Xbox pad through XInput, so while skating the window's registrations for joystick / gamepad /
// multi-axis HID raw input are removed, and put back exactly as they were when skating ends.
// Also counts the messages the game thread takes off its queue, logged once a minute as "[messages]".
namespace RawInputGuard
{
    void Install();       // game thread, once: the message counter
    void InstallFilter(); // at load, after MH_Initialize: Fortnite's controller raw-input requests are left out
    void BeginSkating();  // game thread
    void EndSkating();    // game thread
    // Game thread, every frame: while skating, takes out controller registrations made again; in the
    // server process, keeps them out for good.
    void Tick(bool bServer);
}
