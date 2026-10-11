#pragma once
#include <windows.h>

// Built-in sampling profiler (diagnostics for the long-session FPS loss, 2026-10-10): a background
// thread pauses the game thread and the render thread ~100 times a second for a few microseconds,
// notes which module each is running (or, when it is waiting in Windows, which module it waits
// for), and logs the split once a minute as "[profile]". Nothing is called while a thread is
// paused except copying its registers and the top of its stack.
namespace Profiler
{
    void Start(DWORD GameThread);   // once, in the client process
}
