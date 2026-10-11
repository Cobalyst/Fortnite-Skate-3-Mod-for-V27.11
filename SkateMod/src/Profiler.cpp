#include "Profiler.h"
#include "Common.h"
#include "XInputFix.h"

#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct Mod { uintptr_t Base, End; std::string Name; };
    std::vector<Mod> Mods;   // sorted by Base

    void LoadModules()
    {
        Mods.clear();
        HMODULE List[1024];
        DWORD Bytes = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), List, sizeof(List), &Bytes)) return;
        for (DWORD i = 0; i < Bytes / sizeof(HMODULE); i++)
        {
            MODULEINFO Mi{};
            char Name[MAX_PATH] = {};
            if (!K32GetModuleInformation(GetCurrentProcess(), List[i], &Mi, sizeof(Mi))) continue;
            K32GetModuleBaseNameA(GetCurrentProcess(), List[i], Name, MAX_PATH);
            Mods.push_back({ uintptr_t(Mi.lpBaseOfDll), uintptr_t(Mi.lpBaseOfDll) + Mi.SizeOfImage, Name });
        }
        std::sort(Mods.begin(), Mods.end(), [](const Mod& A, const Mod& B) { return A.Base < B.Base; });
    }

    // The game's own .exe in Mods (game code is attributed by address inside it).
    size_t MainIndex()
    {
        static uintptr_t Base = uintptr_t(GetModuleHandleA(nullptr));
        for (size_t i = 0; i < Mods.size(); i++) if (Mods[i].Base == Base) return i;
        return 0;
    }

    const Mod* Find(uintptr_t A)
    {
        auto It = std::upper_bound(Mods.begin(), Mods.end(), A, [](uintptr_t V, const Mod& M) { return V < M.Base; });
        if (It == Mods.begin()) return nullptr;
        --It;
        return A < It->End ? &*It : nullptr;
    }

    // Windows' own waiting / system modules: a sample there is attributed to the module below it.
    bool IsSystem(const std::string& N)
    {
        static const char* Sys[] = { "ntdll.dll", "KERNELBASE.dll", "KERNEL32.DLL", "win32u.dll", "USER32.dll", "kernel32.dll", "user32.dll", "ucrtbase.dll", "msvcp140.dll", "VCRUNTIME140.dll" };
        for (const char* S : Sys) if (_stricmp(N.c_str(), S) == 0) return true;
        return false;
    }

    struct Target { HANDLE Thread = nullptr; std::string Label; };
    std::vector<Target> Targets;

    void FindRenderThreads(DWORD GameThread)
    {
        using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        auto GetDesc = reinterpret_cast<GetDescFn>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription"));
        HANDLE Snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 E{ sizeof(E) };
        for (BOOL Ok = Thread32First(Snap, &E); Ok; Ok = Thread32Next(Snap, &E))
        {
            if (E.th32OwnerProcessID != GetCurrentProcessId() || E.th32ThreadID == GameThread) continue;
            HANDLE H = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, E.th32ThreadID);
            if (!H) continue;
            PWSTR Desc = nullptr;
            std::string Name;
            if (GetDesc && SUCCEEDED(GetDesc(H, &Desc)) && Desc)
            {
                for (PWSTR P = Desc; *P; P++) Name += char(*P < 128 ? *P : '?');
                LocalFree(Desc);
            }
            if (Name.rfind("RenderThread", 0) == 0 || Name.rfind("RHIThread", 0) == 0) { Targets.push_back({ H, Name }); continue; }
            CloseHandle(H);
        }
        CloseHandle(Snap);
    }

    DWORD WINAPI Run(LPVOID P)
    {
        const DWORD GameThread = DWORD(uintptr_t(P));
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        Sleep(20000);   // let the game settle (render threads exist by then)
        HANDLE G = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, GameThread);
        if (G) Targets.push_back({ G, "game" });
        FindRenderThreads(GameThread);
        std::string Names;
        for (auto& T : Targets) Names += T.Label + " ";
        Log("[profile] sampling %zu threads: %s", Targets.size(), Names.c_str());
        static unsigned char Stack[16384];
        // [thread][0 = controller idle, 1 = controller in use]
        struct Bucket { std::map<std::string, int> Where; std::map<uintptr_t, int> ExeRva; int Samples = 0; };
        std::vector<std::array<Bucket, 2>> B(Targets.size());
        ULONGLONG LastLog = GetTickCount64(), LastMods = 0;
        for (;;)
        {
            Sleep(10);
            if (GetTickCount64() - LastMods > 60000) { LoadModules(); LastMods = GetTickCount64(); }
            const int Mode = XInputFix::PadInUse() ? 1 : 0;
            for (size_t t = 0; t < Targets.size(); t++)
            {
                CONTEXT C{};
                C.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                size_t Copied = 0;
                if (SuspendThread(Targets[t].Thread) == DWORD(-1)) continue;
                if (GetThreadContext(Targets[t].Thread, &C))
                {
                    // The top of its stack, for "waiting in Windows, called from ...".
                    MEMORY_BASIC_INFORMATION Mb{};
                    if (VirtualQuery(reinterpret_cast<void*>(C.Rsp), &Mb, sizeof(Mb)) && Mb.State == MEM_COMMIT)
                    {
                        const uintptr_t End = uintptr_t(Mb.BaseAddress) + Mb.RegionSize;
                        Copied = std::min<size_t>(sizeof(Stack), End - C.Rsp);
                        memcpy(Stack, reinterpret_cast<void*>(C.Rsp), Copied);
                    }
                }
                ResumeThread(Targets[t].Thread);
                if (!C.Rip) continue;
                Bucket& K = B[t][Mode];
                std::string Where;
                uintptr_t ExeAddr = 0;
                const Mod* M = Find(uintptr_t(C.Rip));
                if (M && !IsSystem(M->Name)) { Where = M->Name; if (M == &Mods[MainIndex()]) ExeAddr = uintptr_t(C.Rip) - M->Base; }
                else
                {
                    // In Windows (or unknown): the first return address on the stack in a non-system module.
                    std::string Sys = M ? M->Name : "?";
                    for (size_t o = 0; o + 8 <= Copied && Where.empty(); o += 8)
                    {
                        uintptr_t A;
                        memcpy(&A, Stack + o, 8);
                        if (const Mod* R = Find(A)) if (!IsSystem(R->Name))
                        {
                            Where = "waiting (" + Sys + ") for " + R->Name;
                            if (R == &Mods[MainIndex()]) ExeAddr = (A - R->Base) | (uintptr_t(1) << 63);   // high bit: a wait
                        }
                    }
                    if (Where.empty()) Where = "in " + Sys;
                }
                K.Where[Where]++;
                if (ExeAddr) K.ExeRva[ExeAddr & ~uintptr_t(0x3F) | (ExeAddr & (uintptr_t(1) << 63))]++;
                K.Samples++;
            }
            if (GetTickCount64() - LastLog >= 60000)
            {
                LastLog = GetTickCount64();
                for (size_t t = 0; t < Targets.size(); t++)
                    for (int m = 0; m < 2; m++)
                    {
                        Bucket& K = B[t][m];
                        if (!K.Samples) continue;
                        std::vector<std::pair<int, std::string>> V;
                        for (auto& [Name, N] : K.Where) V.push_back({ N, Name });
                        std::sort(V.rbegin(), V.rend());
                        std::string Line;
                        char Buf[256];
                        for (size_t i = 0; i < V.size() && i < 10; i++)
                        {
                            snprintf(Buf, sizeof(Buf), "%s %.0f%%, ", V[i].second.c_str(), 100.0 * V[i].first / K.Samples);
                            Line += Buf;
                        }
                        std::vector<std::pair<int, uintptr_t>> R;
                        for (auto& [A, N] : K.ExeRva) R.push_back({ N, A });
                        std::sort(R.rbegin(), R.rend());
                        std::string Top;
                        for (size_t i = 0; i < R.size() && i < 12; i++)
                        {
                            snprintf(Buf, sizeof(Buf), "%s%llX %.1f%%, ", (R[i].second >> 63) ? "w" : "", (unsigned long long)(R[i].second & ~(uintptr_t(1) << 63)),
                                     100.0 * R[i].first / K.Samples);
                            Top += Buf;
                        }
                        Log("[profile] %s thread, controller %s (%d samples): %s| game code at: %s", Targets[t].Label.c_str(), m ? "IN USE" : "idle",
                            K.Samples, Line.c_str(), Top.c_str());
                        K = Bucket{};
                    }
            }
        }
    }
}

void Profiler::Start(DWORD GameThread)
{
    static bool bStarted = false;
    if (bStarted) return;
    bStarted = true;
    LoadModules();
    CreateThread(nullptr, 0, Run, reinterpret_cast<LPVOID>(uintptr_t(GameThread)), 0, nullptr);
}
