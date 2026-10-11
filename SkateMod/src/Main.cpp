#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <algorithm>
// SkateMod: Skate 3 skating for Fortnite 27.11 (ATLAS). Inject into both the server and the client.
// Hooks UObject::ProcessEvent to get a per-frame callback on the game thread, then hands the
// frame to Client:: or Server:: depending on what this process is.

#include "Common.h"

#include <TlHelp32.h>
#include <timeapi.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <share.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "XInputFix.h"
#include "Profiler.h"
#include "RawInputGuard.h"
#include "NetGuard.h"

using namespace SDK;

static FILE* g_Log = nullptr;

// Log lines are formatted on the calling thread into memory and written to the file by a
// background thread, so a slow disk never stops the game (2026-10-10: one log line on the game
// thread took 365 ms while the disk was busy, as did lines in other sections before). The writer
// flushes within ~100 ms; if lines pile up past 8 MB (disk gone) newer ones are dropped.
static std::mutex g_LogLock;
static std::condition_variable g_LogWake;
static std::string g_LogPending;

static DWORD WINAPI LogWriter(LPVOID)
{
    std::string Batch;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> L(g_LogLock);
            g_LogWake.wait_for(L, std::chrono::milliseconds(100), [] { return !g_LogPending.empty(); });
            Batch.swap(g_LogPending);
        }
        if (!Batch.empty() && g_Log)
        {
            fwrite(Batch.data(), 1, Batch.size(), g_Log);
            fflush(g_Log);
            Batch.clear();
        }
    }
}

// Writes whatever is pending now, on this thread (crash paths: the process may end next).
static void FlushLogNow()
{
    std::lock_guard<std::mutex> L(g_LogLock);
    if (g_Log && !g_LogPending.empty())
    {
        fwrite(g_LogPending.data(), 1, g_LogPending.size(), g_Log);
        fflush(g_Log);
        g_LogPending.clear();
    }
}

void Log(const char* Fmt, ...)
{
    if (!g_Log) return;
    char Line[4096];
    SYSTEMTIME T;
    GetLocalTime(&T);
    int N = snprintf(Line, sizeof(Line), "[%02d:%02d:%02d.%03d] ", T.wHour, T.wMinute, T.wSecond, T.wMilliseconds);
    va_list Args;
    va_start(Args, Fmt);
    const int M = vsnprintf(Line + N, sizeof(Line) - size_t(N) - 1, Fmt, Args);
    va_end(Args);
    N = std::min(int(sizeof(Line)) - 2, N + std::max(M, 0));
    Line[N++] = '\n';
    {
        std::lock_guard<std::mutex> L(g_LogLock);
        if (g_LogPending.size() < (8u << 20)) g_LogPending.append(Line, size_t(N));
    }
    g_LogWake.notify_one();
}

const std::string& ModRoot()
{
    // bin\SkateMod.dll -> the folder above bin (or the DLL's own folder if it is not in one named bin)
    static const std::string Root = [] {
        HMODULE Self = nullptr;
        char Path[MAX_PATH] = {};
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&ModRoot), &Self) || !GetModuleFileNameA(Self, Path, MAX_PATH))
            return std::string(".");
        std::string Dir = Path;
        Dir = Dir.substr(0, Dir.find_last_of("\\/"));
        const size_t Slash = Dir.find_last_of("\\/");
        if (Slash != std::string::npos && _stricmp(Dir.c_str() + Slash + 1, "bin") == 0) Dir = Dir.substr(0, Slash);
        return Dir;
    }();
    return Root;
}

std::string ModPath(const char* Rel) { return ModRoot() + "\\" + Rel; }

std::wstring ModPathW(const char* Rel)
{
    const std::string S = ModPath(Rel);
    const int N = MultiByteToWideChar(CP_ACP, 0, S.c_str(), -1, nullptr, 0);
    std::wstring W(size_t(N > 0 ? N - 1 : 0), L'\0');
    if (N > 1) MultiByteToWideChar(CP_ACP, 0, S.c_str(), -1, W.data(), N);
    return W;
}

static void OpenLog()
{
    CreateDirectoryA(ModPath("Logs").c_str(), nullptr);

    char Path[MAX_PATH];
    time_t Now = time(nullptr);
    tm Local;
    localtime_s(&Local, &Now);
    snprintf(Path, sizeof(Path), "%s\\SkateMod_%04d-%02d-%02d_%02d-%02d-%02d_pid%lu.log", ModPath("Logs").c_str(),
        Local.tm_year + 1900, Local.tm_mon + 1, Local.tm_mday, Local.tm_hour, Local.tm_min, Local.tm_sec,
        GetCurrentProcessId());
    g_Log = _fsopen(Path, "w", _SH_DENYNO);
    if (g_Log) CreateThread(nullptr, 0, LogWriter, nullptr, 0, nullptr);
}

// FUObjectItem (0x18 bytes): UObject* Object; int32 Flags; ... RootSet is bit 30 of Flags in UE 5.x.
static long* ObjectItemFlags(UObject* Object)
{
    if (!Object) return nullptr;
    const int32 Index = Object->Index;
    FUObjectItem** Chunks = UObject::GObjects->GetDecrytedObjPtr();
    FUObjectItem* Chunk = Chunks ? Chunks[Index / TUObjectArray::ElementsPerChunk] : nullptr;
    if (!Chunk) return nullptr;
    FUObjectItem* Item = &Chunk[Index % TUObjectArray::ElementsPerChunk];
    return Item->Object == Object ? reinterpret_cast<long*>(reinterpret_cast<uint8*>(Item) + 8) : nullptr;
}

void KeepAlive(UObject* Object)
{
    static bool bChecked = false;
    if (!bChecked)
    {
        // The engine object is always in the root set: confirms the flag position.
        bChecked = true;
        UObject* EngineObject = UEngine::GetEngine();
        long* Flags = ObjectItemFlags(EngineObject);
        Log("KeepAlive: engine object item flags 0x%08lX (root set bit 30 expected)", Flags ? *Flags : -1);
    }
    if (long* Flags = ObjectItemFlags(Object))
        _InterlockedOr(Flags, 1L << 30);
}

bool GameWindowFocused()
{
    // Asked by several threads many times a frame: Windows is queried at most every 50 ms.
    static std::atomic<ULONGLONG> CheckedAt{ 0 };
    static std::atomic<bool> bFocused{ false };
    const ULONGLONG Now = GetTickCount64();
    if (Now - CheckedAt.load(std::memory_order_relaxed) >= 50)
    {
        DWORD Pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &Pid);
        bFocused.store(Pid == GetCurrentProcessId(), std::memory_order_relaxed);
        CheckedAt.store(Now, std::memory_order_relaxed);
    }
    return bFocused.load(std::memory_order_relaxed);
}

APlayerController* GetLocalController(UWorld* World)
{
    UGameInstance* GI = World ? World->OwningGameInstance : nullptr;
    if (!GI || GI->LocalPlayers.Num() <= 0 || !GI->LocalPlayers[0])
        return nullptr;
    return GI->LocalPlayers[0]->PlayerController;
}

ACharacter* GetLocalCharacter(UWorld* World)
{
    APlayerController* PC = GetLocalController(World);
    APawn* Pawn = PC ? PC->AcknowledgedPawn : nullptr;
    if (!Pawn || !Pawn->IsA(ACharacter::StaticClass()))
        return nullptr;
    return static_cast<ACharacter*>(Pawn);
}

enum class ERole { Menu, Server, Client };

static ERole GetRole(UWorld* World)
{
    UNetDriver* Net = World->NetDriver;
    if (!Net)                   return ERole::Menu;
    if (Net->ServerConnection)  return ERole::Client;
    return ERole::Server;
}

static ERole g_Role = ERole::Menu;

// From ProcessEvent (many times per frame, rate-limited): role tracking and the server's checks.
// The client's skating runs once per rendered frame from the viewport draw instead (ClientFrame).
static void ModTick(float DeltaSeconds)
{
    UWorld* World = UWorld::GetWorld();
    if (!World) return;

    const ERole Role = GetRole(World);
    if (Role != g_Role)
    {
        Log("Role is now %s", Role == ERole::Server ? "SERVER" : Role == ERole::Client ? "CLIENT" : "MENU");
        g_Role = Role;
        XInputFix::SetServer(Role == ERole::Server);
        if (Role == ERole::Client) Profiler::Start(GetCurrentThreadId());   // ModTick runs on the game thread
    }

    if (Role == ERole::Server) Server::Tick(World, DeltaSeconds);
}

static void SafeTick(float DeltaSeconds)
{
    __try
    {
        ModTick(DeltaSeconds);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int Reported = 0;
        if (Reported++ < 20)
            Log("Exception 0x%08X inside the mod tick (ignored)", GetExceptionCode());
    }
}

// ---- ProcessEvent hook: a once-per-frame callback on the game thread ----

using ProcessEventFn = void(*)(const UObject*, UFunction*, void*);
static ProcessEventFn g_OriginalProcessEvent = nullptr;
static DWORD g_GameThread = 0;
static thread_local bool t_InTick = false;
static LARGE_INTEGER g_LastTick{}, g_Frequency{};

// The game viewport's per-frame draw (UGameViewportClient::DrawTransition, called from PostRender
// with the frame's canvas; exe+0x248643C, shared by every viewport class's vtable). The HUD is drawn
// right after it, on the game thread, on top of the game.
using ViewportDrawFn = void(*)(UObject* Viewport, UCanvas* Canvas);
static ViewportDrawFn g_OriginalViewportDraw = nullptr;
static constexpr uintptr_t ViewportDrawOffset = 0x248643C;

static LARGE_INTEGER g_LastFrame{};

static ModFrameCost g_FrameCost;
static long long g_GameCalls = 0;

// Game-thread ProcessEvent calls by function (diagnostics: which game functions run more and more).
static std::unordered_map<int32, uint32_t> g_CallsByFunction;

std::string TakeTopGameCalls(int Count, long long Frames)
{
    std::vector<std::pair<uint32_t, int32>> All;
    All.reserve(g_CallsByFunction.size());
    for (auto& [Name, N] : g_CallsByFunction) All.push_back({ N, Name });
    g_CallsByFunction.clear();
    std::sort(All.begin(), All.end(), [](auto& A, auto& B) { return A.first > B.first; });
    std::string Out;
    char Buf[160];
    for (int i = 0; i < Count && i < int(All.size()); i++)
    {
        snprintf(Buf, sizeof(Buf), "%s %.1f, ", FName(All[i].second).ToString().c_str(), Frames ? double(All[i].first) / double(Frames) : 0.0);
        Out += Buf;
    }
    return Out;
}

ModFrameCost TakeModFrameCost()
{
    ModFrameCost C = g_FrameCost;
    C.GameCalls = g_GameCalls;
    g_FrameCost = {};
    g_GameCalls = 0;
    return C;
}

// Once per rendered frame on the game thread: step the skater, then draw the HUD.
static void SafeClientFrame(UCanvas* Canvas)
{
    __try
    {
        static bool bLogged = false;
        if (!bLogged) { bLogged = true; Log("Frame hook is live (canvas %dx%d)", Canvas->SizeX, Canvas->SizeY); }

        LARGE_INTEGER Now;
        QueryPerformanceCounter(&Now);
        double Dt = g_LastFrame.QuadPart ? double(Now.QuadPart - g_LastFrame.QuadPart) / double(g_Frequency.QuadPart) : 0.0;
        g_LastFrame = Now;
        if (Dt > 0.1) Dt = 0.1;

        // Fortnite never sees a controller, in menus and in matches: its XInput
        // reads get a neutral pad and the Xbox pad's raw HID messages never reach its window. The
        // skate engine (its own thread) and the mod's own reads keep the real pad. Not the server.
        static bool bPadHidden = false;
        if (!bPadHidden && g_Role != ERole::Server)
        {
            bPadHidden = true;
            XInputFix::SetSkating(true, GetCurrentThreadId());
            RawInputGuard::Install();
            RawInputGuard::BeginSkating();
        }
        if (g_Role == ERole::Client)
            if (UWorld* World = UWorld::GetWorld())
                Client::Tick(World, float(Dt));
        LARGE_INTEGER Ticked;
        QueryPerformanceCounter(&Ticked);
        Client::DrawHud(Canvas);
        LARGE_INTEGER Done;
        QueryPerformanceCounter(&Done);
        g_FrameCost.HudMs += double(Done.QuadPart - Ticked.QuadPart) * 1000.0 / double(g_Frequency.QuadPart);
        const double Ms = double(Done.QuadPart - Now.QuadPart) * 1000.0 / double(g_Frequency.QuadPart);
        g_FrameCost.TotalMs += Ms;
        g_FrameCost.WorstMs = Ms > g_FrameCost.WorstMs ? Ms : g_FrameCost.WorstMs;
        g_FrameCost.Frames++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int Reported = 0;
        if (Reported++ < 20)
            Log("Exception 0x%08X while drawing the HUD (ignored)", GetExceptionCode());
    }
}

static void HookedViewportDraw(UObject* Viewport, UCanvas* Canvas)
{
    g_OriginalViewportDraw(Viewport, Canvas);
    if (Canvas && !t_InTick && GetCurrentThreadId() == g_GameThread)
    {
        t_InTick = true;
        SafeClientFrame(Canvas);
        t_InTick = false;
    }
}

// While skating on the client: count the server->client RPCs that arrive, logged once a second,
// so anything that still moves the skater can be named.
static std::unordered_map<int32, std::pair<UFunction*, int>> g_ClientRpcs;
static ULONGLONG g_LastRpcLog = 0;

static void CountClientRpc(UFunction* Function)
{
    if (!Function || !(uint32(Function->FunctionFlags) & 0x01000000 /* FUNC_NetClient */) || !Client::IsSkating())
        return;
    auto& Entry = g_ClientRpcs[Function->Name.ComparisonIndex];
    Entry.first = Function;
    Entry.second++;
    const ULONGLONG Now = GetTickCount64();
    if (Now - g_LastRpcLog >= 1000)
    {
        g_LastRpcLog = Now;
        std::string Line;
        for (auto& [Name, Value] : g_ClientRpcs)
            Line += Value.first->GetName() + " x" + std::to_string(Value.second) + "  ";
        Log("[rpc] server->client while skating: %s", Line.c_str());
        g_ClientRpcs.clear();
    }
}

// While skating, anything else that moves the character is undone at once (ProcessEvent runs
// thousands of times a frame, so before the frame is drawn), and the game functions running
// when it happened are counted and logged once a second.
static std::unordered_map<uint64_t, int> g_Movers;
static int32 g_PrevFunctionName = -1;
static ULONGLONG g_LastMoverLog = 0;

static void PinSkater(const UObject* Object, UFunction* Function)
{
    ACharacter* Character = nullptr;
    FVector Pin{};
    if (!Client::GetPin(&Character, &Pin))
        return;
    // The character may have been destroyed since the last frame: only touch it while it is
    // still the live object in its GUObjectArray slot.
    if (UObject::GObjects->GetByIndex(Character->Index) != Character || !Character->RootComponent)
        return;
    const FVector& At = Character->RootComponent->RelativeLocation;  // root: relative == world
    const double DX = At.X - Pin.X, DY = At.Y - Pin.Y, DZ = At.Z - Pin.Z;
    if (DX * DX + DY * DY + DZ * DZ > 0.25)
    {
        const int32 Current = Function ? Function->Name.ComparisonIndex : -1;
        g_Movers[(uint64_t(uint32_t(g_PrevFunctionName)) << 32) | uint32_t(Current)]++;
        t_InTick = true;
        Character->K2_SetActorLocation(Pin, false, nullptr, true);
        t_InTick = false;
    }
    const ULONGLONG Now = GetTickCount64();
    if (!g_Movers.empty() && Now - g_LastMoverLog >= 1000)
    {
        g_LastMoverLog = Now;
        std::string Line;
        int Shown = 0;
        for (auto& [Key, Count] : g_Movers)
        {
            if (Shown++ >= 8) break;
            Line += FName(int32(Key >> 32)).ToString() + " -> " + FName(int32(Key & 0xFFFFFFFF)).ToString() + " x" + std::to_string(Count) + "  ";
        }
        Log("[pin] skater moved by something else and put back, during: %s", Line.c_str());
        g_Movers.clear();
    }
}

// Server: while someone skates, log emote / ability-ending calls (why an emote stops).
static void LogServerEmoteCalls(const UObject* Object, UFunction* Function)
{
    static int Logged = 0;
    if (!Function || Logged > 400 || !Server::AnySkating())
        return;
    // Decided once per function name (this runs for every game event on the server).
    static std::unordered_map<int32, bool> Interesting;
    auto It = Interesting.find(Function->Name.ComparisonIndex);
    if (It == Interesting.end())
    {
        const std::string N = Function->GetName();
        const bool bYes = (N.find("Emote") != std::string::npos || N.find("CancelAbilit") != std::string::npos ||
                           N.find("EndAbility") != std::string::npos || N.find("Cancelled") != std::string::npos) &&
                          N.find("Tick") == std::string::npos && N.find("Update") == std::string::npos;
        It = Interesting.emplace(Function->Name.ComparisonIndex, bYes).first;
    }
    if (!It->second)
        return;
    const std::string Name = Function->GetName();
    Logged++;
    Log("[emote] server: %s on %s", Name.c_str(), Object ? Object->GetName().c_str() : "?");
}

// Client: from an emote request until its cancel, log every distinct game call (why it stops).
static void LogClientEmoteWindow(const UObject* Object, UFunction* Function)
{
    static ULONGLONG WindowEnd = 0;
    static std::unordered_map<int32, int> Seen;
    static int Windows = 0;
    if (!Function || !Client::IsSkating())
        return;
    // Names are looked up once per function (this runs for every game event while skating; a
    // text lookup per call cost frame time in busy areas). Done for good after 6 windows.
    static int32 EmoteRequest = -1;
    const ULONGLONG Now = GetTickCount64();
    if (Windows >= 6 && Now > WindowEnd)
        return;
    // Each function's name is read once (this used to read the name of every game call while
    // skating until the first emote: thousands of string lookups a frame).
    static std::unordered_set<int32> Checked;
    if (EmoteRequest < 0 && Checked.insert(Function->Name.ComparisonIndex).second && Function->GetName() == "ServerPlayEmoteItem")
        EmoteRequest = Function->Name.ComparisonIndex;
    if (Function->Name.ComparisonIndex == EmoteRequest && Windows < 6)
    {
        Windows++;
        WindowEnd = Now + 400;
        Seen.clear();
        Log("[emote] client: emote requested, logging calls for 0.4 s");
    }
    if (Now > WindowEnd || Seen[Function->Name.ComparisonIndex]++)
        return;
    Log("[emote] client: %s on %s", Function->GetName().c_str(), Object ? Object->GetName().c_str() : "?");
}

// If a game call crashes, note which UFunction / object it was (each nested ProcessEvent adds a
// line, innermost first), then let the crash continue exactly as before.
static void LogCrashingCall(DWORD Code, void* Where, const UObject* Object, UFunction* Function)
{
    Log("[crash] exception 0x%08X at %p during ProcessEvent %s on %s", Code, Where,
        Function ? Function->GetFullName().c_str() : "?", Object ? Object->GetFullName().c_str() : "?");
}

static LONG NoteCrashingCall(EXCEPTION_POINTERS* Info, const UObject* Object, UFunction* Function)
{
    static int Noted = 0;
    if (Noted < 12)
    {
        Noted++;
        const DWORD Code = Info->ExceptionRecord->ExceptionCode;
        __try { LogCrashingCall(Code, Info->ExceptionRecord->ExceptionAddress, Object, Function); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] exception 0x%08X during ProcessEvent (names unreadable)", Code); }
        FlushLogNow();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void CallOriginalProcessEvent(const UObject* Object, UFunction* Function, void* Parms)
{
    __try { g_OriginalProcessEvent(Object, Function, Parms); }
    __except (NoteCrashingCall(GetExceptionInformation(), Object, Function)) {}
}

// ATLAS cheat commands (teleport / fly / waypoints, Remix.dll) vs this mod: log every command,
// teleport and movement-mode change on both sides, and whether ProcessEvent was already hooked
// by someone else when we hooked it (or gets re-hooked later). Diagnostics only.
// ServerCheat's FString Msg (data pointer + length), copied without C++ objects so it can be guarded.
static bool ReadCheatText(void* Parms, wchar_t* Out, int Max)
{
    __try
    {
        const wchar_t* Data = *reinterpret_cast<wchar_t* const*>(Parms);
        const int Num = *reinterpret_cast<const int*>(static_cast<unsigned char*>(Parms) + 8);
        if (!Data || Num <= 0) return false;
        int i = 0;
        for (; i < Num - 1 && i < Max && Data[i]; i++) Out[i] = Data[i];
        Out[i] = 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void LogCheatCalls(const UObject* Object, UFunction* Function, void* Parms)
{
    static std::unordered_map<int32, bool> Interesting;
    static int Logged = 0;
    if (!Function || Logged > 600)
        return;
    const int32 Key = Function->Name.ComparisonIndex;
    auto It = Interesting.find(Key);
    if (It == Interesting.end())
    {
        const std::string Name = Function->GetName();
        const bool bYes = Name.find("Cheat") != std::string::npos || Name.find("ConsoleCommand") != std::string::npos ||
                          Name.find("Teleport") != std::string::npos || Name.find("SetMovementMode") != std::string::npos ||
                          Name.find("Waypoint") != std::string::npos || Name.find("Fly") != std::string::npos ||
                          Name.find("ClientAdjust") != std::string::npos || Name == "ClientSetLocation" ||
                          ((Name.find("SafeZone") != std::string::npos || Name.find("Storm") != std::string::npos || Name.find("Pause") != std::string::npos) &&
                           Name.find("Tick") == std::string::npos && Name.find("Update") == std::string::npos);
        It = Interesting.emplace(Key, bYes).first;
    }
    if (!It->second)
        return;
    // ServerCheat(FString Msg): the command text. ATLAS's menu re-sends one about every 1.2 s;
    // log each distinct text once, other calls as before.
    std::string Text;
    const std::string FnName = Function->GetName();
    if (FnName == "ServerCheat" && Parms)
    {
        wchar_t Buf[256] = {};
        Text = ReadCheatText(Parms, Buf, 255) ? std::string(Buf, Buf + wcslen(Buf)) : std::string("?");
        static std::unordered_map<std::string, int> Seen;
        if (Seen[Text]++ > 0) return;
    }
    Logged++;
    Log("[atlas] %s: %s on %s%s%s", g_Role == ERole::Server ? "server" : "client", FnName.c_str(),
        Object ? Object->GetName().c_str() : "?", Text.empty() ? "" : " : ", Text.c_str());
}

static void LogProcessEventBytes(const char* When, const void* Target)
{
    const unsigned char* B = static_cast<const unsigned char*>(Target);
    Log("[atlas] ProcessEvent first bytes %s: %02X %02X %02X %02X %02X %02X %02X %02X%s", When,
        B[0], B[1], B[2], B[3], B[4], B[5], B[6], B[7], (B[0] == 0xE9 || B[0] == 0xFF) ? " (already a jump: hooked)" : "");
}

static void HookedProcessEvent(const UObject* Object, UFunction* Function, void* Parms)
{
    g_GameCalls++;
    if (Function && GetCurrentThreadId() == g_GameThread && g_CallsByFunction.size() < 20000)
        g_CallsByFunction[Function->Name.ComparisonIndex]++;
    if (g_Role == ERole::Client && GetCurrentThreadId() == g_GameThread)
        LogClientEmoteWindow(Object, Function);
    if (g_Role == ERole::Server && GetCurrentThreadId() == g_GameThread)
        LogServerEmoteCalls(Object, Function);
    if (g_Role != ERole::Menu && GetCurrentThreadId() == g_GameThread)
        LogCheatCalls(Object, Function, Parms);
    if (g_Role == ERole::Client && GetCurrentThreadId() == g_GameThread)
    {
        CountClientRpc(Function);
        if (!t_InTick)
            PinSkater(Object, Function);
        if (Function)
            g_PrevFunctionName = Function->Name.ComparisonIndex;
    }

    if (!t_InTick && GetCurrentThreadId() == g_GameThread)
    {
        LARGE_INTEGER Now;
        QueryPerformanceCounter(&Now);
        const double Elapsed = double(Now.QuadPart - g_LastTick.QuadPart) / double(g_Frequency.QuadPart);

        // ProcessEvent runs thousands of times per frame; the server checks need ~30 Hz.
        if (Elapsed >= 1.0 / 30.0)
        {
            g_LastTick = Now;
            t_InTick = true;
            SafeTick(float(Elapsed > 0.1 ? 0.1 : Elapsed));
            t_InTick = false;
        }
    }
    // BlueprintCanInteract called without its parameter buffer writes its answer through a null
    // pointer: the hosted server crashed this way at 17:04:47 on 2026-10-10 (campfire) and on
    // 2026-10-07 (gnome); the caller was ATLAS's Magnesium. Only that function is guarded: other
    // functions are called without a buffer on purpose (a first, wider guard broke the sky).
    if (Function && !Parms)
    {
        static int32 CanInteract = -1;
        static std::unordered_set<int32> Checked;
        const int32 Index = Function->Name.ComparisonIndex;
        if (CanInteract < 0 && Checked.insert(Index).second && Function->GetName() == "BlueprintCanInteract") CanInteract = Index;
        if (Index == CanInteract)
        {
            static int Noted = 0;
            if (Noted++ < 10)
                Log("[netguard] skipped BlueprintCanInteract on %s: called without its parameter buffer (would crash the game)",
                    Object ? Object->GetName().c_str() : "?");
            return;
        }
    }
    CallOriginalProcessEvent(Object, Function, Parms);
}

// Unreal's game thread is the process's main (oldest) thread on Windows.
static DWORD FindMainThread()
{
    DWORD Best = 0;
    ULONGLONG BestTime = ~0ull;
    HANDLE Snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 Entry{ sizeof(Entry) };
    for (BOOL Ok = Thread32First(Snap, &Entry); Ok; Ok = Thread32Next(Snap, &Entry))
    {
        if (Entry.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE Thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, Entry.th32ThreadID);
        if (!Thread) continue;
        FILETIME Created, Exit, Kernel, User;
        if (GetThreadTimes(Thread, &Created, &Exit, &Kernel, &User))
        {
            const ULONGLONG T = (ULONGLONG(Created.dwHighDateTime) << 32) | Created.dwLowDateTime;
            if (T < BestTime) { BestTime = T; Best = Entry.th32ThreadID; }
        }
        CloseHandle(Thread);
    }
    CloseHandle(Snap);
    return Best;
}

static DWORD WINAPI Init(LPVOID)
{
    OpenLog();
    Log("SkateMod loaded into pid %lu", GetCurrentProcessId());
    {
        const DWORD A = GetFileAttributesA(ModPath("skate-data").c_str());
        Log("Mod folder: %s (%s)", ModRoot().c_str(), A != INVALID_FILE_ATTRIBUTES && (A & FILE_ATTRIBUTE_DIRECTORY)
            ? "skate-data found" : "skate-data MISSING: SkateMod.dll must be injected from the bin folder inside the mod folder");
    }

    // 1 ms timer resolution so the skate simulation thread wakes on time (most games set this too).
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&g_Frequency);
    QueryPerformanceCounter(&g_LastTick);
    g_GameThread = FindMainThread();

    void* Target = reinterpret_cast<void*>(InSDKUtils::GetImageBase() + Offsets::ProcessEvent);
    LogProcessEventBytes("before our hook", Target);
    if (MH_Initialize() != MH_OK ||
        MH_CreateHook(Target, reinterpret_cast<void*>(&HookedProcessEvent), reinterpret_cast<void**>(&g_OriginalProcessEvent)) != MH_OK ||
        MH_EnableHook(Target) != MH_OK)
    {
        Log("Failed to hook ProcessEvent at %p", Target);
        return 1;
    }
    Log("Hooked ProcessEvent at %p, game thread %lu. Press J in a match to skate.", Target, g_GameThread);
    // Who else hooks ProcessEvent later (ATLAS loads its DLLs on its own schedule).
    CreateThread(nullptr, 0, [](LPVOID P) -> DWORD
    {
        unsigned char Last[8] = {};
        memcpy(Last, P, 8);
        LogProcessEventBytes("after our hook", P);
        for (int i = 0; i < 60; i++)
        {
            Sleep(5000);
            if (memcmp(Last, P, 8) != 0) { memcpy(Last, P, 8); LogProcessEventBytes("changed by someone else", P); }
        }
        return 0;
    }, Target, 0, nullptr);
    XInputFix::Install();
    RawInputGuard::InstallFilter();
    {
        // which ATLAS process this is (the client window or the background match server)
        const char* Cmd = GetCommandLineA();
        Log("Command line: %.900s", Cmd ? Cmd : "?");
    }
    NetGuard::Install();

    void* Draw = reinterpret_cast<void*>(InSDKUtils::GetImageBase() + ViewportDrawOffset);
    if (MH_CreateHook(Draw, reinterpret_cast<void*>(&HookedViewportDraw), reinterpret_cast<void**>(&g_OriginalViewportDraw)) != MH_OK ||
        MH_EnableHook(Draw) != MH_OK)
        Log("Failed to hook the viewport draw at %p: no HUD", Draw);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE Module, DWORD Reason, LPVOID)
{
    if (Reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(Module);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
