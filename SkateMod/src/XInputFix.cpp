#include "XInputFix.h"
#include "Common.h"
#include "MinHook.h"
#include "PadShare.h"

#include <atomic>
#include <cstring>
#include <string>

namespace
{
    constexpr DWORD NotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED
    constexpr ULONGLONG RecheckMs = 3000; // an empty slot is asked again at most this often (no pad connected)
    constexpr long long FreshMs = 12;     // a reading younger than this is shared instead of read again
    constexpr ULONGLONG CapsMs = 2000;    // capabilities are re-read this often
    constexpr ULONGLONG IdleMs = 2000;    // a DLL nobody asked for this long is not read

    using GetStateFn = DWORD(WINAPI*)(DWORD, void*);
    using GetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, void*);
    using EnableFn = void(WINAPI*)(BOOL);

    // What the reader thread last got for one slot of one XInput DLL.
    struct Cached
    {
        DWORD Result = NotConnected;
        unsigned char State[16] = {};   // XINPUT_STATE
        DWORD CapsResult = NotConnected;
        unsigned char Caps[20] = {};    // XINPUT_CAPABILITIES
        ULONGLONG CapsAt = 0;
    };

    struct Module
    {
        const char* Name;
        GetStateFn OrigGetState = nullptr;
        GetCapabilitiesFn OrigGetCapabilities = nullptr;
        EnableFn Enable = nullptr;      // the real XInputEnable
        EnableFn OrigEnable = nullptr;  // the hook's trampoline
        bool bGameEnabled = true;       // what the game last asked for (it turns XInput off when unfocused)
        std::atomic<ULONGLONG> AskedAt{ 0 };   // last time anyone asked this DLL for a pad
        std::atomic<bool> bFilled{ false };    // some slot of this DLL has been read for real
        std::atomic<long long> ReadAtUs[4] = {}; // when each slot was last read for real (0 = never)
        Cached Slots[4];
    };
    Module Modules[2] = { { "xinput1_3.dll" }, { "xinput1_4.dll" } };

    // Real XInput calls are shared (GetState below): with the Bluetooth pad in use each one takes
    // milliseconds. Readings are passed through unchanged (no mapping or dead-zone changes).
    // Lock: the real calls, one at a time (never waited for once a slot has a reading);
    // CacheLock: the copies. (A dedicated reader thread polling at 125 Hz was tried on 2026-10-10:
    // more real reads, and the game fell to 10 fps the moment the pad was used.)
    CRITICAL_SECTION Lock;
    SRWLOCK CacheLock = SRWLOCK_INIT;
    thread_local int Depth = 0;
    bool Connected[4] = {};
    ULONGLONG EmptyAt[4] = {};    // tick of the last "not connected" answer (0 = never asked)

    // Frozen pad: after an AMD Instant Replay save (Ctrl+Shift+S) the Bluetooth pad kept
    // answering with the same packet forever (2026-10-07 20:53 and 00:05 logs) while the
    // keyboard worked. When a connected pad's packet number has not changed for 10 s, XInput is
    // switched off and on again (XInputEnable), which makes it read the device afresh: at most
    // once per freeze (at 3 s and every 3 s a still pad was re-enabled hundreds of times).
    constexpr ULONGLONG StaleMs = 10000;
    DWORD LastPacket[4] = {};
    ULONGLONG LastChange[4] = {}, LastKick[4] = {};
    bool KickedThisFreeze[4] = {};
    std::atomic<ULONGLONG> LastPadInput{ 0 };   // last time any pad sent new data (XInputFix::PadInUse)
    int Kicks = 0;

    // Server process (XInputFix::SetServer): neutral pad while its window is in the background.
    volatile bool bServerProcess = false;
    DWORD BlindPacket[4] = {};
    int BlindLogs = 0;

    // Diagnostics (health log): slowest real XInputGetState, longest wait for the lock.
    std::atomic<long long> WorstCallUs{ 0 }, WorstWaitUs{ 0 };
    long long Us()
    {
        static LARGE_INTEGER F{};
        if (!F.QuadPart) QueryPerformanceFrequency(&F);
        LARGE_INTEGER C; QueryPerformanceCounter(&C);
        return C.QuadPart * 1000000 / F.QuadPart;
    }
    void Worst(std::atomic<long long>& W, long long V) { long long O = W.load(); while (V > O && !W.compare_exchange_weak(O, V)) {} }

    std::atomic<bool> bSkating{ false };
    std::atomic<DWORD> GameThreadId{ 0 };
    thread_local bool tModRead = false;
    DWORD NeutralPacket[4] = {};

    // Nothing pressed, sticks centred, and a packet number that stays the same (no new input).
    void Neutral(DWORD Slot, void* State)
    {
        unsigned char* P = static_cast<unsigned char*>(State);
        memset(P + 4, 0, 12);
        *reinterpret_cast<DWORD*>(P) = NeutralPacket[Slot];
    }

    void Blind(DWORD Slot, void* State)
    {
        // XINPUT_STATE: dwPacketNumber, then XINPUT_GAMEPAD (12 bytes: buttons, triggers, sticks)
        unsigned char* P = static_cast<unsigned char*>(State);
        const WORD Buttons = *reinterpret_cast<const WORD*>(P + 4);
        if (Buttons && BlindLogs < 20)
        {
            BlindLogs++;
            Log("[xinput] server window in the background: controller buttons %04X kept from it", Buttons);
        }
        memset(P + 4, 0, 12);
        *reinterpret_cast<DWORD*>(P) = BlindPacket[Slot];
    }

    bool AnyConnected()
    {
        for (bool C : Connected) if (C) return true;
        return false;
    }

    // True when the real call should be skipped for this slot (empty, and asking costs ~40 ms).
    bool SkipEmpty(DWORD Slot)
    {
        if (!EmptyAt[Slot]) return false;
        if (AnyConnected()) return true;  // the slow case: a pad is connected elsewhere
        return GetTickCount64() - EmptyAt[Slot] < RecheckMs;
    }

    void Record(DWORD Slot, DWORD Result)
    {
        if (Result == NotConnected)
        {
            if (Connected[Slot]) Log("[xinput] controller in slot %lu disconnected", Slot + 1);
            Connected[Slot] = false;
            EmptyAt[Slot] = GetTickCount64();
        }
        else if (Result == ERROR_SUCCESS)
        {
            if (!Connected[Slot]) Log("[xinput] controller in slot %lu connected", Slot + 1);
            Connected[Slot] = true;
            EmptyAt[Slot] = 0;
        }
    }

    template <int M>
    void CheckStale(DWORD Slot, const void* State)
    {
        const DWORD Packet = *static_cast<const DWORD*>(State);  // XINPUT_STATE::dwPacketNumber
        const ULONGLONG Now = GetTickCount64();
        if (Packet != LastPacket[Slot] || !LastChange[Slot])
        {
            static int Recovered = 0;
            if (KickedThisFreeze[Slot] && Recovered++ < 40)
                Log("[xinput] slot %lu: new controller data again after re-enabling XInput", Slot + 1);
            LastPacket[Slot] = Packet;
            LastChange[Slot] = Now;
            KickedThisFreeze[Slot] = false;
            LastPadInput = Now;
            return;
        }
        if (KickedThisFreeze[Slot] || Now - LastChange[Slot] < StaleMs || !Modules[M].Enable) return;
        // Unchanging data is also what XInput gives while the game itself has switched it off (an
        // unfocused window, e.g. the ATLAS server running in the background). Switching it back on
        // there let controller presses reach that process: the Y button resumed a paused storm on
        // the server (2026-10-09). Only a focused game that wants input gets the kick.
        if (!Modules[M].bGameEnabled || !GameWindowFocused()) return;
        LastKick[Slot] = Now;
        KickedThisFreeze[Slot] = true;
        if (Kicks++ < 40)
            Log("[xinput] slot %lu: no new controller data for %.0f s (packet %lu): re-enabling XInput", Slot + 1,
                double(Now - LastChange[Slot]) / 1000.0, Packet);
        EnableFn Real = Modules[M].OrigEnable ? Modules[M].OrigEnable : Modules[M].Enable;
        Real(FALSE);
        Real(TRUE);
    }

    // PadReader.exe (PadShare.h): a separate program does every real read; this process only copies
    // its readings. Without it (missing, not started yet, or stopped) the in-process path below runs.
    PadShare::Block* Share = nullptr;
    HANDLE ShareMap = nullptr, HelperJob = nullptr;
    std::wstring HelperPath;
    std::atomic<ULONGLONG> HelperStartedAt{ 0 };
    std::atomic<int> HelperStarts{ 0 };
    std::atomic<bool> bHelperWasLive{ false };
    std::atomic<int> HelperConnected[4] = {};   // -1 unknown, 0 empty, 1 connected (logs only)
    std::atomic<DWORD> HelperPacket[4] = {};
    LONG SeenKicks = 0;
    SRWLOCK StartLock = SRWLOCK_INIT;

    void StartHelper()
    {
        if (!Share || HelperPath.empty() || HelperStarts.load() >= 5) return;
        if (!TryAcquireSRWLockExclusive(&StartLock)) return;
        const ULONGLONG Now = GetTickCount64();
        if (HelperStartedAt.load() && Now - HelperStartedAt.load() < 10000) { ReleaseSRWLockExclusive(&StartLock); return; }
        HelperStartedAt = Now;
        HelperStarts++;
        std::wstring Cmd = L"\"" + HelperPath + L"\" " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW Si{}; Si.cb = sizeof(Si);
        PROCESS_INFORMATION Pi{};
        if (CreateProcessW(HelperPath.c_str(), Cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &Si, &Pi))
        {
            const bool bJob = HelperJob && AssignProcessToJobObject(HelperJob, Pi.hProcess);
            ResumeThread(Pi.hThread);
            CloseHandle(Pi.hThread);
            CloseHandle(Pi.hProcess);
            Log("[xinput] controller reader started (PadReader.exe, pid %lu%s): Fortnite's process never reads the pad itself", Pi.dwProcessId,
                bJob ? "" : ", not in the game's job: it ends when the game does");
        }
        else Log("[xinput] could not start PadReader.exe (error %lu): pad read in-process as before", GetLastError());
        ReleaseSRWLockExclusive(&StartLock);
    }

    bool HelperLive()
    {
        if (!Share) return false;
        const bool bLive = Share->HelperPid && GetTickCount64() - ULONGLONG(Share->HeartbeatMs) < 1000;
        if (bLive != bHelperWasLive.load())
        {
            bHelperWasLive = bLive;
            Log("[xinput] controller reader %s", bLive ? "running: pad readings come from PadReader.exe" : "stopped answering: pad read in-process until it is back");
        }
        if (!bLive)
        {
            if (Share->HelperPid) Share->HelperPid = 0;   // it was running and stopped
            if (HelperStartedAt.load() && GetTickCount64() - HelperStartedAt.load() > 10000) StartHelper();
        }
        return bLive;
    }

    // Bookkeeping for a reading from the helper (logs, the server's neutral packet, PadInUse).
    void NoteHelper(DWORD Slot, const PadShare::Slot& S);

    // One real read of a slot (caller holds Lock), its bookkeeping, and the cached copy.
    template <int M>
    void ReadSlot(DWORD Slot)
    {
        Module& Mod = Modules[M];
        unsigned char Buf[16] = {};
        DWORD Result = NotConnected;
        if (!SkipEmpty(Slot))
        {
            Depth++;
            const long long C0 = Us();
            Result = Mod.OrigGetState(Slot, Buf);
            Worst(WorstCallUs, Us() - C0);
            Depth--;
            Record(Slot, Result);
            if (Result == ERROR_SUCCESS)
            {
                BlindPacket[Slot] = *reinterpret_cast<const DWORD*>(Buf);
                CheckStale<M>(Slot, Buf);
            }
        }
        Cached& C = Mod.Slots[Slot];
        const ULONGLONG Now = GetTickCount64();
        unsigned char Caps[20] = {};
        DWORD CapsResult = C.CapsResult;
        const bool bCaps = Result == ERROR_SUCCESS && Mod.OrigGetCapabilities && (C.CapsResult != ERROR_SUCCESS || Now - C.CapsAt >= CapsMs);
        if (bCaps)
        {
            Depth++;
            CapsResult = Mod.OrigGetCapabilities(Slot, 1 /* XINPUT_FLAG_GAMEPAD */, Caps);
            Depth--;
        }
        AcquireSRWLockExclusive(&CacheLock);
        C.Result = Result;
        if (Result == ERROR_SUCCESS) memcpy(C.State, Buf, sizeof(Buf));
        if (bCaps) { C.CapsResult = CapsResult; memcpy(C.Caps, Caps, sizeof(Caps)); C.CapsAt = Now; }
        if (Result != ERROR_SUCCESS) C.CapsResult = Result;
        ReleaseSRWLockExclusive(&CacheLock);
    }

    template <int M>
    DWORD WINAPI GetState(DWORD Slot, void* State)
    {
        Module& Mod = Modules[M];
        if (Slot >= 4 || Depth || !State) return Mod.OrigGetState(Slot, State);
        Mod.AskedAt = GetTickCount64();
        if (HelperLive())
        {
            PadShare::Slot S;
            if (PadShare::Read(Share, int(Slot), S))
            {
                Share->GameWantsInput = (Mod.bGameEnabled && GameWindowFocused()) ? 1 : 0;
                NoteHelper(Slot, S);
                if (S.Result != ERROR_SUCCESS) return S.Result;
                memcpy(State, S.State, sizeof(S.State));
                // The game switched XInput off (its window lost focus): XInput then reports a neutral pad.
                if (!Mod.bGameEnabled) memset(static_cast<unsigned char*>(State) + 4, 0, 12);
                if (bServerProcess && !GameWindowFocused()) Blind(Slot, State);
                else if (bSkating && !tModRead && GetCurrentThreadId() == GameThreadId) Neutral(Slot, State);
                return ERROR_SUCCESS;
            }
        }
        // A real read only when the last one is older than FreshMs and no other thread is in the
        // middle of one; otherwise the last reading at once. Fortnite reads the pad every frame
        // on the game thread, so the skate engine's and the dropper's reads ride on those instead
        // of adding their own slow ones (2026-10-10: with the Bluetooth pad in use a real read
        // takes 2-6 ms, up to 30+; serialised behind the skate engine's reads, Fortnite's own read
        // each frame waited, and skate mode fell to 10-20 fps while Fortnite mode was fine).
        const long long Now = Us();
        if (!Mod.ReadAtUs[Slot].load() || Now - Mod.ReadAtUs[Slot].load() >= FreshMs * 1000)
        {
            if (TryEnterCriticalSection(&Lock))
            {
                if (!Mod.ReadAtUs[Slot].load() || Now - Mod.ReadAtUs[Slot].load() >= FreshMs * 1000)
                {
                    ReadSlot<M>(Slot);
                    Mod.ReadAtUs[Slot] = Us();
                    Mod.bFilled = true;
                }
                LeaveCriticalSection(&Lock);
            }
            else if (!Mod.ReadAtUs[Slot].load())
            {
                const long long W0 = Us();
                EnterCriticalSection(&Lock);   // this slot's very first reading: wait for it once
                Worst(WorstWaitUs, Us() - W0);
                if (!Mod.ReadAtUs[Slot].load()) { ReadSlot<M>(Slot); Mod.ReadAtUs[Slot] = Us(); Mod.bFilled = true; }
                LeaveCriticalSection(&Lock);
            }
        }
        AcquireSRWLockShared(&CacheLock);
        const Cached& C = Mod.Slots[Slot];
        const DWORD Result = C.Result;
        if (Result == ERROR_SUCCESS) memcpy(State, C.State, sizeof(C.State));
        ReleaseSRWLockShared(&CacheLock);
        if (Result == ERROR_SUCCESS && bServerProcess && !GameWindowFocused()) Blind(Slot, State);
        // While skating, Fortnite's own reads (its game thread) get a neutral pad: the skate controls
        // also drove Fortnite's actions on the hidden character (triggers = pickaxe swings / fire /
        // aim-down-sights with aim assist, sticks and buttons = sprint, jump, crouch...), which
        // piled up the longer the pad was used (2026-10-10: 12-18 fps with the pad, 60-70 with the
        // keyboard). The skate engine (its own thread) and the mod's own reads keep the real pad.
        else if (Result == ERROR_SUCCESS && bSkating && !tModRead && GetCurrentThreadId() == GameThreadId) Neutral(Slot, State);
        return Result;
    }

    template <int M>
    DWORD WINAPI GetCapabilities(DWORD Slot, DWORD Flags, void* Caps)
    {
        Module& Mod = Modules[M];
        if (Slot >= 4 || Depth || !Caps) return Mod.OrigGetCapabilities(Slot, Flags, Caps);
        Mod.AskedAt = GetTickCount64();
        if (HelperLive())
        {
            PadShare::Slot S;
            if (PadShare::Read(Share, int(Slot), S))
            {
                if (S.Result != ERROR_SUCCESS) return S.Result;
                if (S.CapsResult == ERROR_SUCCESS) memcpy(Caps, S.Caps, sizeof(S.Caps));
                return S.CapsResult;
            }
        }
        AcquireSRWLockShared(&CacheLock);
        const Cached& C = Mod.Slots[Slot];
        const DWORD Result = C.CapsResult;
        const bool bHave = Result == ERROR_SUCCESS || (Mod.bFilled && C.Result == NotConnected);
        if (Result == ERROR_SUCCESS) memcpy(Caps, C.Caps, sizeof(C.Caps));
        ReleaseSRWLockShared(&CacheLock);
        if (bHave) return Result;
        // Not read yet: one real call, as before.
        const long long W0 = Us();
        EnterCriticalSection(&Lock);
        Worst(WorstWaitUs, Us() - W0);
        DWORD Real = NotConnected;
        if (!SkipEmpty(Slot))
        {
            Depth++;
            Real = Mod.OrigGetCapabilities(Slot, Flags, Caps);
            Depth--;
            Record(Slot, Real);
        }
        LeaveCriticalSection(&Lock);
        return Real;
    }

    template <int M>
    void WINAPI Enable(BOOL bEnable)
    {
        Modules[M].bGameEnabled = bEnable != FALSE;
        Modules[M].OrigEnable(bEnable);
    }

    template <int M>
    void HookModule()
    {
        Module& Mod = Modules[M];
        HMODULE H = GetModuleHandleA(Mod.Name);
        if (!H && M == 0) H = LoadLibraryA(Mod.Name);  // Fortnite imports xinput1_3; the skate engine uses it too
        if (!H) return;
        void* State = reinterpret_cast<void*>(GetProcAddress(H, "XInputGetState"));
        void* Caps = reinterpret_cast<void*>(GetProcAddress(H, "XInputGetCapabilities"));
        Mod.Enable = reinterpret_cast<EnableFn>(GetProcAddress(H, "XInputEnable"));
        if (Mod.Enable && (MH_CreateHook(reinterpret_cast<void*>(Mod.Enable), reinterpret_cast<void*>(&Enable<M>), reinterpret_cast<void**>(&Mod.OrigEnable)) != MH_OK ||
                           MH_EnableHook(reinterpret_cast<void*>(Mod.Enable)) != MH_OK))
        {
            Log("[xinput] %s: could not watch XInputEnable: frozen-pad recovery off", Mod.Name);
            Mod.Enable = nullptr;   // never re-enable XInput without knowing the game's choice
        }
        const bool bState = State && MH_CreateHook(State, reinterpret_cast<void*>(&GetState<M>), reinterpret_cast<void**>(&Mod.OrigGetState)) == MH_OK &&
                            MH_EnableHook(State) == MH_OK;
        const bool bCaps = Caps && MH_CreateHook(Caps, reinterpret_cast<void*>(&GetCapabilities<M>), reinterpret_cast<void**>(&Mod.OrigGetCapabilities)) == MH_OK &&
                           MH_EnableHook(Caps) == MH_OK;
        if (!bState) Mod.OrigGetState = nullptr;
        Log("[xinput] %s: shared readings, fast empty slots %s", Mod.Name, bState && bCaps ? "on" : "FAILED");
    }
}

namespace
{
    void NoteHelper(DWORD Slot, const PadShare::Slot& S)
    {
        const int Now = S.Result == ERROR_SUCCESS ? 1 : 0;
        const int Was = HelperConnected[Slot].exchange(Now);
        if (Was != Now && (Was == 1 || Now == 1))
            Log("[xinput] controller in slot %lu %s", Slot + 1, Now ? "connected" : "disconnected");
        if (!Now) return;
        const DWORD Packet = *reinterpret_cast<const DWORD*>(S.State);
        BlindPacket[Slot] = Packet;
        if (HelperPacket[Slot].exchange(Packet) != Packet) LastPadInput = GetTickCount64();
    }
}

void XInputFix::SetServer(bool bServer)
{
    if (bServerProcess != bServer)
        Log("[xinput] %s", bServer ? "server process: no controller input while its window is in the background" : "not the server any more: controller input as usual");
    bServerProcess = bServer;
}

void XInputFix::Install()
{
    InitializeCriticalSection(&Lock);
    HookModule<0>();
    HookModule<1>();
    // The controller reader: PadReader.exe next to SkateMod.dll.
    HMODULE Self = nullptr;
    wchar_t Path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&XInputFix::Install), &Self) && GetModuleFileNameW(Self, Path, MAX_PATH))
    {
        std::wstring P = Path;
        P = P.substr(0, P.find_last_of(L"\\/") + 1) + L"PadReader.exe";
        if (GetFileAttributesW(P.c_str()) != INVALID_FILE_ATTRIBUTES) HelperPath = P;
    }
    if (HelperPath.empty()) { Log("[xinput] PadReader.exe not found next to SkateMod.dll: pad read in-process"); return; }
    wchar_t Name[64];
    PadShare::Name(Name, 64, GetCurrentProcessId());
    ShareMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(PadShare::Block), Name);
    if (ShareMap) Share = static_cast<PadShare::Block*>(MapViewOfFile(ShareMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PadShare::Block)));
    if (!Share) { Log("[xinput] no shared memory for the controller reader (error %lu): pad read in-process", GetLastError()); return; }
    memset(Share, 0, sizeof(PadShare::Block));
    for (int i = 0; i < 4; i++) { Share->Slots[i].Result = NotConnected; Share->Slots[i].CapsResult = NotConnected; HelperConnected[i] = -1; }
    Share->Version = PadShare::Version;
    MemoryBarrier();
    Share->Magic = PadShare::Magic;
    HelperJob = CreateJobObjectW(nullptr, nullptr);
    if (HelperJob)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION L{};
        L.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(HelperJob, JobObjectExtendedLimitInformation, &L, sizeof(L));
    }
    StartHelper();
}

void XInputFix::TakeStats(double& WorstCallMs, double& WorstWaitMs)
{
    WorstCallMs = double(WorstCallUs.exchange(0)) / 1000.0;   // real reads in this process (none while the reader runs)
    WorstWaitMs = double(WorstWaitUs.exchange(0)) / 1000.0;
    if (Share)
    {
        const double HelperMs = double(InterlockedExchange64(&Share->WorstReadUs, 0)) / 1000.0;
        if (HelperLive()) Log("[xinput] controller reader: slowest read %.1f ms (in PadReader.exe, not in the game)", HelperMs);
        const LONG K = Share->Kicks;
        if (K != SeenKicks) { Log("[xinput] controller reader re-enabled XInput for a frozen pad (%ld so far)", K); SeenKicks = K; }
    }
}

bool XInputFix::PadInUse()
{
    return GetTickCount64() - LastPadInput.load() < 500;
}

void XInputFix::SetSkating(bool bOn, unsigned long GameThread)
{
    GameThreadId = GameThread;
    if (bOn && !bSkating)
        for (int i = 0; i < 4; i++) NeutralPacket[i] = BlindPacket[i] + 1;   // one last "new" packet: all released
    bSkating = bOn;
    Log("[xinput] %s", bOn ? "Fortnite gets a neutral pad (the skate engine reads the real one)" : "not skating: Fortnite reads the pad again");
}

XInputFix::ModRead::ModRead() { tModRead = true; }
XInputFix::ModRead::~ModRead() { tModRead = false; }
