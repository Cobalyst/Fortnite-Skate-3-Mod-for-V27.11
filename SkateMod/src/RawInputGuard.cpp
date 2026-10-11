#include "RawInputGuard.h"
#include "Common.h"
#include "MinHook.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace
{
    HHOOK Hook = nullptr;
    std::map<UINT, long long> Counts;          // message id -> count since the last log
    long long RawByType[3] = {};               // WM_INPUT by device: mouse, keyboard, HID
    ULONGLONG LastLog = 0;
    std::vector<RAWINPUTDEVICE> Removed;       // registrations taken out while skating
    long long Dropped = 0;                     // Xbox HID messages kept from Fortnite since the last log

    const char* MessageName(UINT M)
    {
        switch (M)
        {
        case WM_INPUT: return "WM_INPUT";
        case WM_INPUT_DEVICE_CHANGE: return "WM_INPUT_DEVICE_CHANGE";
        case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
        case WM_TIMER: return "WM_TIMER";
        case WM_PAINT: return "WM_PAINT";
        case WM_KEYDOWN: return "WM_KEYDOWN";
        case WM_KEYUP: return "WM_KEYUP";
        case WM_CHAR: return "WM_CHAR";
        case WM_SETCURSOR: return "WM_SETCURSOR";
        case WM_NCHITTEST: return "WM_NCHITTEST";
        case WM_DEVICECHANGE: return "WM_DEVICECHANGE";
        case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN";
        case WM_USER: return "WM_USER";
        default: return nullptr;
        }
    }

    void LogCounts()
    {
        std::vector<std::pair<long long, UINT>> V;
        long long Total = 0;
        for (auto& [M, N] : Counts) { V.push_back({ N, M }); Total += N; }
        std::sort(V.rbegin(), V.rend());
        std::string Line;
        char Buf[96];
        for (size_t i = 0; i < V.size() && i < 10; i++)
        {
            const char* Name = MessageName(V[i].second);
            if (Name) snprintf(Buf, sizeof(Buf), "%s %lld, ", Name, V[i].first);
            else snprintf(Buf, sizeof(Buf), "0x%04X %lld, ", V[i].second, V[i].first);
            Line += Buf;
        }
        Log("[messages] last minute: %lld messages (%.0f a second) | raw input: mouse %lld, keyboard %lld, controller/HID %lld (%lld kept from Fortnite) | %s",
            Total, double(Total) / 60.0, RawByType[0], RawByType[1], RawByType[2], Dropped, Line.c_str());
        Dropped = 0;
        Counts.clear();
        RawByType[0] = RawByType[1] = RawByType[2] = 0;
    }

    bool bSkatingNow = false;
    std::map<HANDLE, bool> XboxDevice;   // raw input device handle -> a Microsoft (Xbox) HID controller

    bool IsXbox(HANDLE Device)
    {
        auto It = XboxDevice.find(Device);
        if (It != XboxDevice.end()) return It->second;
        RID_DEVICE_INFO Info{};
        Info.cbSize = sizeof(Info);
        UINT Size = sizeof(Info);
        bool bXbox = false;
        if (GetRawInputDeviceInfoW(Device, RIDI_DEVICEINFO, &Info, &Size) != UINT(-1) && Info.dwType == RIM_TYPEHID)
            bXbox = Info.hid.dwVendorId == 0x045E;
        if (XboxDevice.size() > 64) XboxDevice.clear();
        XboxDevice[Device] = bXbox;
        Log("[messages] controller raw input from device %p: vendor 0x%04lX product 0x%04lX%s", Device, Info.hid.dwVendorId, Info.hid.dwProductId,
            bXbox ? " (Xbox: kept from Fortnite while skating)" : "");
        return bXbox;
    }

    LRESULT CALLBACK OnMessage(int Code, WPARAM W, LPARAM L)
    {
        if (Code == HC_ACTION && L && W == PM_REMOVE)
        {
            MSG* M = reinterpret_cast<MSG*>(L);
            Counts[M->message]++;
            if (M->message == WM_INPUT)
            {
                RAWINPUTHEADER H{};
                UINT Size = sizeof(H);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(M->lParam), RID_HEADER, &H, &Size, sizeof(RAWINPUTHEADER)) == sizeof(H) && H.dwType <= 2)
                {
                    RawByType[H.dwType]++;
                    // While skating, the Xbox pad's raw HID messages never reach Fortnite's window: its
                    // generic-HID controller path handled each one slowly (game thread stuck in the
                    // message pump while the pad was used: 12-28 fps lows, fine on keyboard). The skate
                    // engine and Fortnite read the pad through XInput. Windows still gets the message
                    // for its own clean-up (DefWindowProc), then the window sees WM_NULL.
                    if (bSkatingNow && H.dwType == RIM_TYPEHID && IsXbox(H.hDevice))
                    {
                        DefWindowProcW(M->hwnd, WM_INPUT, M->wParam, M->lParam);
                        M->message = WM_NULL;
                        M->wParam = 0;
                        M->lParam = 0;
                        Dropped++;
                    }
                }
            }
            const ULONGLONG Now = GetTickCount64();
            if (!LastLog) LastLog = Now;
            if (Now - LastLog >= 60000) { LastLog = Now; LogCounts(); }
        }
        return CallNextHookEx(Hook, Code, W, L);
    }

    std::vector<RAWINPUTDEVICE> Registered()
    {
        UINT N = 0;
        GetRegisteredRawInputDevices(nullptr, &N, sizeof(RAWINPUTDEVICE));
        std::vector<RAWINPUTDEVICE> List(N);
        if (N && GetRegisteredRawInputDevices(List.data(), &N, sizeof(RAWINPUTDEVICE)) == UINT(-1)) List.clear();
        List.resize(std::min<size_t>(List.size(), N));
        return List;
    }

    std::string Describe(const std::vector<RAWINPUTDEVICE>& List)
    {
        std::string S;
        char Buf[96];
        for (auto& D : List)
        {
            snprintf(Buf, sizeof(Buf), "page 0x%02X usage 0x%02X flags 0x%X window %p; ", D.usUsagePage, D.usUsage, D.dwFlags, D.hwndTarget);
            S += Buf;
        }
        return S.empty() ? "none" : S;
    }

    // Generic desktop joystick (4), gamepad (5), multi-axis controller (8).
    bool IsController(const RAWINPUTDEVICE& D) { return D.usUsagePage == 0x01 && (D.usUsage == 0x04 || D.usUsage == 0x05 || D.usUsage == 0x08); }
}

namespace
{
    // Fortnite never gets controller raw input (by design: Fortnite is played on keyboard and
    // mouse, the controller is for skating, read by PadReader.exe). Its RegisterRawInputDevices calls
    // go through with the joystick / gamepad / multi-axis entries taken out, so Windows never queues
    // the pad's HID reports to the game window: no message to fetch and clean up on the game thread.
    using RegisterFn = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
    RegisterFn OrigRegister = nullptr;
    int StripLogs = 0;

    BOOL WINAPI Register(PCRAWINPUTDEVICE Devices, UINT Count, UINT Size)
    {
        if (!Devices || Size != sizeof(RAWINPUTDEVICE)) return OrigRegister(Devices, Count, Size);
        std::vector<RAWINPUTDEVICE> Keep;
        Keep.reserve(Count);
        for (UINT i = 0; i < Count; i++)
        {
            const RAWINPUTDEVICE& D = Devices[i];
            if (IsController(D) && !(D.dwFlags & RIDEV_REMOVE))
            {
                if (StripLogs++ < 20)
                    Log("[rawinput] Fortnite asked for controller raw input (page 0x%02X usage 0x%02X, flags 0x%X): left out", D.usUsagePage, D.usUsage, D.dwFlags);
                continue;
            }
            Keep.push_back(D);
        }
        if (Keep.size() == Count) return OrigRegister(Devices, Count, Size);
        if (Keep.empty()) return TRUE;
        return OrigRegister(Keep.data(), UINT(Keep.size()), Size);
    }
}

void RawInputGuard::InstallFilter()
{
    if (OrigRegister) return;
    void* Target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "RegisterRawInputDevices"));
    const bool bOk = Target && MH_CreateHook(Target, reinterpret_cast<void*>(&Register), reinterpret_cast<void**>(&OrigRegister)) == MH_OK &&
                     MH_EnableHook(Target) == MH_OK;
    Log("[rawinput] controller raw input kept out of Fortnite: %s", bOk ? "on" : "FAILED");
}

void RawInputGuard::Install()
{
    if (Hook) return;
    Hook = SetWindowsHookExW(WH_GETMESSAGE, OnMessage, nullptr, GetCurrentThreadId());
    Log("[messages] counting the game thread's window messages: %s", Hook ? "on" : "FAILED");
}

namespace
{
    bool bSkating = false;

    // Takes out every controller registration found (remembering the first copy of each).
    void RemoveControllers(bool bRemember)
    {
        for (const RAWINPUTDEVICE& D : Registered())
        {
            if (!IsController(D)) continue;
            RAWINPUTDEVICE R{ D.usUsagePage, D.usUsage, RIDEV_REMOVE, nullptr };
            if (!RegisterRawInputDevices(&R, 1, sizeof(R)))
            {
                Log("[rawinput] could not switch off page 0x%02X usage 0x%02X (error %lu)", D.usUsagePage, D.usUsage, GetLastError());
                continue;
            }
            bool bKnown = false;
            for (auto& K : Removed) bKnown |= K.usUsagePage == D.usUsagePage && K.usUsage == D.usUsage;
            if (bRemember && !bKnown) Removed.push_back(D);
            Log("[rawinput] controller raw input (page 0x%02X usage 0x%02X, flags 0x%X) switched off%s", D.usUsagePage, D.usUsage, D.dwFlags,
                bRemember ? " while skating; the pad is read through XInput" : "");
        }
    }
}

void RawInputGuard::Tick(bool bServer)
{
    static ULONGLONG Last = 0;
    const ULONGLONG Now = GetTickCount64();
    if (Now - Last < 2000) return;
    Last = Now;
    if (bServer)
    {
        static bool bListed = false;
        if (!bListed) { bListed = true; Log("[rawinput] server process registered: %s", Describe(Registered()).c_str()); }
        RemoveControllers(false);   // the server never needs controller input (XInputFix blinds it too)
    }
    else
    {
        // Registered before the filter was in place: taken out once (the filter keeps it out).
        static int Checks = 0;
        if (Checks < 30) { Checks++; RemoveControllers(false); }
    }
}

void RawInputGuard::BeginSkating()
{
    bSkating = true;
    bSkatingNow = true;
    Log("[rawinput] registered for this process: %s", Describe(Registered()).c_str());
}

void RawInputGuard::EndSkating()
{
    bSkating = false;
    bSkatingNow = false;
    // Registrations removed by an earlier build are put back (none are removed any more).
    for (const RAWINPUTDEVICE& D : Removed) RegisterRawInputDevices(&D, 1, sizeof(D));
    Removed.clear();
}
