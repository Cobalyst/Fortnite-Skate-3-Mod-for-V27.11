// PadReader.exe: the only thing that reads the Xbox controller for Fortnite + the skate mod.
// Started by SkateMod.dll (XInputFix) with the game's process id; it writes each slot's XInput state
// to shared memory (PadShare.h) and the mod hands that to everyone in the game who asks, so the
// game's own threads never wait on a real controller read. Readings are passed on unchanged (no
// mapping or dead-zone changes). Ends when the game ends (and through the game's job object).
#include "../src/PadShare.h"
#include <mmsystem.h>
#include <cstdlib>

namespace
{
    using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
    using GetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, void*);
    using EnableFn = void(WINAPI*)(BOOL);

    constexpr DWORD NotConnected = 1167;
    constexpr ULONGLONG RecheckMs = 3000;   // empty slots asked again this often while no pad is connected
    constexpr ULONGLONG CapsMs = 2000;
    constexpr ULONGLONG StaleMs = 10000;    // frozen pad: same packet this long -> XInput off and on (once per freeze)
    constexpr DWORD PeriodMs = 8;           // a pass every 8 ms (125 Hz; the skate engine steps at 60 Hz)

    long long Us()
    {
        static LARGE_INTEGER F{};
        if (!F.QuadPart) QueryPerformanceFrequency(&F);
        LARGE_INTEGER C; QueryPerformanceCounter(&C);
        return C.QuadPart * 1000000 / F.QuadPart;
    }

    void Write(PadShare::Block* B, int S, DWORD Result, const unsigned char* State, DWORD CapsResult, const unsigned char* Caps)
    {
        PadShare::Slot& D = B->Slots[S];
        InterlockedIncrement(&D.Seq);   // odd: being written
        MemoryBarrier();
        D.Result = Result;
        if (State) memcpy(D.State, State, sizeof(D.State));
        D.CapsResult = CapsResult;
        if (Caps) memcpy(D.Caps, Caps, sizeof(D.Caps));
        MemoryBarrier();
        InterlockedIncrement(&D.Seq);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int Argc = 0;
    wchar_t** Argv = CommandLineToArgvW(GetCommandLineW(), &Argc);
    if (!Argv || Argc < 2) return 1;
    const DWORD Owner = DWORD(wcstoul(Argv[1], nullptr, 10));
    HANDLE Game = OpenProcess(SYNCHRONIZE, FALSE, Owner);
    if (!Game) return 2;
    wchar_t MapName[64];
    PadShare::Name(MapName, 64, Owner);
    HANDLE Map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MapName);
    if (!Map) return 3;
    auto* B = static_cast<PadShare::Block*>(MapViewOfFile(Map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PadShare::Block)));
    if (!B || B->Magic != PadShare::Magic || B->Version != PadShare::Version) return 4;

    HMODULE X = LoadLibraryW(L"xinput1_4.dll");
    if (!X) X = LoadLibraryW(L"xinput1_3.dll");
    if (!X) return 5;
    auto GetState = reinterpret_cast<GetStateFn>(GetProcAddress(X, "XInputGetState"));
    auto GetCaps = reinterpret_cast<GetCapabilitiesFn>(GetProcAddress(X, "XInputGetCapabilities"));
    auto Enable = reinterpret_cast<EnableFn>(GetProcAddress(X, "XInputEnable"));
    if (!GetState) return 6;
    B->HelperPid = LONG(GetCurrentProcessId());

    timeBeginPeriod(1);
    HANDLE Timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!Timer) Timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    bool Connected[4] = {};
    ULONGLONG EmptyAt[4] = {}, CapsAt[4] = {}, LastChange[4] = {};
    DWORD LastPacket[4] = {}, CapsResult[4] = { NotConnected, NotConnected, NotConnected, NotConnected };
    unsigned char Caps[4][20] = {};
    bool Kicked[4] = {};

    while (WaitForSingleObject(Game, 0) == WAIT_TIMEOUT)
    {
        const long long PassStart = Us();
        bool bAny = false;
        for (bool C : Connected) bAny |= C;
        for (int S = 0; S < 4; S++)
        {
            const ULONGLONG Now = GetTickCount64();
            // An empty slot answers slowly (~40 ms) while a Bluetooth pad is connected: skipped then.
            if (!Connected[S] && EmptyAt[S] && (bAny || Now - EmptyAt[S] < RecheckMs)) continue;
            unsigned char State[16] = {};
            const long long C0 = Us();
            const DWORD R = GetState(DWORD(S), State);
            const long long Took = Us() - C0;
            for (LONG64 W = B->WorstReadUs; Took > W && InterlockedCompareExchange64(&B->WorstReadUs, Took, W) != W; W = B->WorstReadUs) {}
            if (R != ERROR_SUCCESS)
            {
                Connected[S] = false;
                EmptyAt[S] = GetTickCount64();
                CapsResult[S] = R;
                Write(B, S, R, nullptr, R, nullptr);
                continue;
            }
            Connected[S] = true;
            EmptyAt[S] = 0;
            if (GetCaps && (CapsResult[S] != ERROR_SUCCESS || Now - CapsAt[S] >= CapsMs))
            {
                CapsResult[S] = GetCaps(DWORD(S), 1 /* XINPUT_FLAG_GAMEPAD */, Caps[S]);
                CapsAt[S] = Now;
            }
            Write(B, S, R, State, CapsResult[S], Caps[S]);
            // Frozen pad (after an AMD Instant Replay save the pad kept sending one packet forever):
            // XInput off and on again makes it read the device afresh. Only while the game wants input.
            const DWORD Packet = *reinterpret_cast<const DWORD*>(State);
            if (Packet != LastPacket[S] || !LastChange[S]) { LastPacket[S] = Packet; LastChange[S] = Now; Kicked[S] = false; }
            else if (!Kicked[S] && Enable && B->GameWantsInput && Now - LastChange[S] >= StaleMs)
            {
                Kicked[S] = true;
                Enable(FALSE);
                Enable(TRUE);
                InterlockedIncrement(&B->Kicks);
            }
        }
        B->HeartbeatMs = LONG64(GetTickCount64());
        const long long Left = long long(PeriodMs) * 1000 - (Us() - PassStart);
        if (Left > 500)
        {
            LARGE_INTEGER Due; Due.QuadPart = -Left * 10;   // 100 ns units, relative
            if (Timer && SetWaitableTimer(Timer, &Due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(Timer, 50);
            else Sleep(DWORD(Left / 1000));
        }
    }
    timeEndPeriod(1);
    return 0;
}
