#include "KeyBinds.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace SDK;

namespace
{
    struct Def { const char* Name; const wchar_t* Label; int Default; };
    // Defaults keep every key the mod already used; the rest fill in what had no key.
    const Def Defs[KeyBinds::Count] = {
        { "steer_left",   L"Steer / walk left (left stick)",        'A' },
        { "steer_right",  L"Steer / walk right (left stick)",       'D' },
        { "forward",      L"Push / forward (left stick up)",        'W' },
        { "backward",     L"Brake / back (left stick down)",        'S' },
        { "ollie",        L"Ollie (quick flick)",                   VK_SPACE },
        { "stick_up",     L"Trick stick up (right stick)",          VK_UP },
        { "stick_down",   L"Trick stick down (right stick)",        VK_DOWN },
        { "stick_left",   L"Trick stick left (right stick)",        VK_LEFT },
        { "stick_right",  L"Trick stick right (right stick)",       VK_RIGHT },
        { "manual",       L"Manual (right stick half back)",        'X' },
        { "nose_manual",  L"Nose manual (right stick half up)",     'V' },
        { "grab_left",    L"Left grab (LT)",                        'Z' },
        { "grab_right",   L"Right grab (RT)",                       'C' },
        { "bumper_left",  L"LB (session marker modifier)",          VK_LSHIFT },
        { "bumper_right", L"RB",                                    VK_LCONTROL },
        { "button_a",     L"A button",                              'Q' },
        { "button_b",     L"B button",                              'R' },
        { "button_x",     L"X button",                              'E' },
        { "button_y",     L"Y button",                              'F' },
        { "dpad_up",      L"D-pad up",                              '1' },
        { "dpad_down",    L"D-pad down",                            '2' },
        { "dpad_left",    L"D-pad left",                            '3' },
        { "dpad_right",   L"D-pad right",                           '4' },
        { "stick_click_left",  L"Left stick click (L3)",            '5' },
        { "stick_click_right", L"Right stick click (R3)",           '6' },
        { "start",        L"Start",                                 'P' },
        { "back",         L"Back",                                  'O' },
        { "toggle_skate", L"Skate mode on / off",                   'J' },
        { "bail_now",     L"Bail now",                              'K' },
        { "board_swing",  L"Swing the board",                       'G' },
        { "hall_of_meat", L"Hall of Meat on / off",                 'H' },
        { "multi_body_flips", L"Multi body flips on / off",         'N' },
        { "board_graphic", L"Board graphic: Skate 3 / Fortnite wrap", '7' },
    };
    // The '-' key: not a letter (letters are typed into ATLAS commands) and F8 is ATLAS's command menu.
    constexpr int MenuKey = VK_OEM_MINUS;
    const std::string PathS = ModPath("skate-data\\settings\\keybinds.txt");
    const char* const Path = PathS.c_str();

    int Keys[KeyBinds::Count] = {};
    bool bLoaded = false;
    bool bOpen = false;
    int Selected = 0, Top = 0;
    bool bCapturing = false;
    bool WasDown[256] = {};      // previous frame's key state (edges)
    bool State[256] = {};        // this frame's key state, read once in Tick (Down() reads it)
    bool bTicked = false;
    bool HeldAtCapture[256] = {};

    void Load()
    {
        bLoaded = true;
        for (int i = 0; i < KeyBinds::Count; i++) Keys[i] = Defs[i].Default;
        std::ifstream In(Path);
        std::string Line;
        while (std::getline(In, Line))
        {
            if (Line.empty() || Line[0] == '#') continue;
            std::istringstream L(Line);
            std::string Name;
            int Vk = 0;
            if (!(L >> Name >> Vk) || Vk < 0 || Vk > 255) continue;
            for (int i = 0; i < KeyBinds::Count; i++)
                if (Name == Defs[i].Name) Keys[i] = Vk;
        }
    }

    void Save()
    {
        std::ofstream Out(Path, std::ios::trunc);
        Out << "# Skate mod keyboard binds (Windows virtual-key codes). Change them in game with the - key.\n";
        for (int i = 0; i < KeyBinds::Count; i++) Out << Defs[i].Name << ' ' << Keys[i] << '\n';
    }

    std::wstring KeyName(int Vk)
    {
        switch (Vk)
        {
        case 0: return L"(none)";
        case VK_SPACE: return L"Space";
        case VK_UP: return L"Up arrow";
        case VK_DOWN: return L"Down arrow";
        case VK_LEFT: return L"Left arrow";
        case VK_RIGHT: return L"Right arrow";
        case VK_LSHIFT: return L"Left Shift";
        case VK_RSHIFT: return L"Right Shift";
        case VK_LCONTROL: return L"Left Ctrl";
        case VK_RCONTROL: return L"Right Ctrl";
        case VK_LMENU: return L"Left Alt";
        case VK_RMENU: return L"Right Alt";
        case VK_RETURN: return L"Enter";
        case VK_TAB: return L"Tab";
        case VK_BACK: return L"Backspace";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"Page Up";
        case VK_NEXT: return L"Page Down";
        }
        if (Vk >= VK_F1 && Vk <= VK_F24) return L"F" + std::to_wstring(Vk - VK_F1 + 1);
        if (Vk >= VK_NUMPAD0 && Vk <= VK_NUMPAD9) return L"Numpad " + std::to_wstring(Vk - VK_NUMPAD0);
        wchar_t Buf[64] = {};
        const UINT Scan = MapVirtualKeyW(UINT(Vk), MAPVK_VK_TO_VSC);
        if (Scan && GetKeyNameTextW(LONG(Scan << 16), Buf, 64) > 0) return Buf;
        return L"Key " + std::to_wstring(Vk);
    }

    bool Pressed(int Vk) { return (GetAsyncKeyState(Vk) & 0x8000) != 0; }
}

bool KeyBinds::Down(Action A)
{
    if (!bLoaded) Load();
    if (bOpen || A < 0 || A >= Count || !Keys[A]) return false;
    // The state Tick read this frame: the skate code asks ~40 times a frame, and each live Windows
    // query (key + foreground window) was the step that grew to 15 ms a frame over a session.
    if (bTicked) return State[Keys[A]];
    return GameWindowFocused() && Pressed(Keys[A]);
}

int KeyBinds::KeyOf(Action A)
{
    if (!bLoaded) Load();
    return A >= 0 && A < Count ? Keys[A] : 0;
}

bool KeyBinds::MenuOpen() { return bOpen; }

void KeyBinds::Tick()
{
    if (!bLoaded) Load();
    bool Now[256] = {};
    const bool bFocused = GameWindowFocused();
    if (bFocused)
    {
        // Every key only while the menu is open (rebinding takes any key); otherwise the bound
        // keys and the menu key, once each.
        if (bOpen)
            for (int Vk = 1; Vk < 256; Vk++) Now[Vk] = Pressed(Vk);
        else
        {
            for (int A = 0; A < Count; A++) if (Keys[A] > 0 && Keys[A] < 256) Now[Keys[A]] = Pressed(Keys[A]);
            Now[MenuKey] = Pressed(MenuKey);
        }
    }
    Now[0] = false;
    for (int Vk = 0; Vk < 256; Vk++) State[Vk] = Now[Vk];
    bTicked = true;
    auto Hit = [&](int Vk) { return Now[Vk] && !WasDown[Vk]; };

    if (!bOpen)
    {
        if (Hit(MenuKey)) { bOpen = true; bCapturing = false; }
    }
    else if (bCapturing)
    {
        if (Hit(VK_ESCAPE)) bCapturing = false;
        else
        {
            for (int Vk = 8; Vk < 256; Vk++)
            {
                // Generic Shift / Ctrl / Alt come with their left / right versions: take those.
                if (Vk == MenuKey || Vk == VK_ESCAPE || Vk == VK_SHIFT || Vk == VK_CONTROL || Vk == VK_MENU) continue;
                if (Now[Vk] && !HeldAtCapture[Vk])
                {
                    Keys[Selected] = Vk;
                    bCapturing = false;
                    Save();
                    Log("[keys] %s bound to virtual key %d", Defs[Selected].Name, Vk);
                    break;
                }
            }
            for (int Vk = 0; Vk < 256; Vk++) if (!Now[Vk]) HeldAtCapture[Vk] = false;
        }
    }
    else
    {
        if (Hit(MenuKey) || Hit(VK_ESCAPE)) bOpen = false;
        if (Hit(VK_UP)) Selected = (Selected + Count - 1) % Count;
        if (Hit(VK_DOWN)) Selected = (Selected + 1) % Count;
        if (Hit(VK_BACK)) { Keys[Selected] = Defs[Selected].Default; Save(); }
        if (Hit(VK_DELETE)) { Keys[Selected] = 0; Save(); }
        if (Hit(VK_RETURN))
        {
            bCapturing = true;
            for (int Vk = 0; Vk < 256; Vk++) HeldAtCapture[Vk] = Now[Vk];
        }
    }
    for (int Vk = 0; Vk < 256; Vk++) WasDown[Vk] = Now[Vk];
}

void KeyBinds::DrawMenu(UCanvas* Canvas)
{
    if (!bOpen || !Canvas) return;
    UEngine* Eng = UEngine::GetEngine();
    UFont* Font = Eng ? (Eng->MediumFont ? Eng->MediumFont : Eng->SmallFont) : nullptr;
    if (!Font) return;
    const float W = float(Canvas->SizeX), H = float(Canvas->SizeY);
    const float S = std::fmax(1.f, H / 1080.f) * 1.1f;
    const float Row = 26.f * S, BoxW = 760.f * S;
    const int Visible = 20;
    if (Selected < Top) Top = Selected;
    if (Selected >= Top + Visible) Top = Selected - Visible + 1;
    const int Rows = Count < Visible ? Count : Visible;
    const float BoxH = Row * (Rows + 4.5f);
    const float X0 = (W - BoxW) * 0.5f, Y0 = (H - BoxH) * 0.5f;

    Canvas->K2_DrawTexture(nullptr, FVector2D{ X0, Y0 }, FVector2D{ BoxW, BoxH }, FVector2D{ 0, 0 }, FVector2D{ 1, 1 },
        FLinearColor{ 0.f, 0.f, 0.f, 0.78f }, EBlendMode::BLEND_Translucent, 0.f, FVector2D{ 0.5, 0.5 });
    const FLinearColor White{ 1, 1, 1, 1 }, Grey{ 0.6f, 0.6f, 0.6f, 1 }, Gold{ 1.f, 0.8f, 0.2f, 1 }, Black{ 0, 0, 0, 1 };
    auto Text = [&](const std::wstring& T, float X, float Y, const FLinearColor& C)
    {
        Canvas->K2_DrawText(Font, FString(T.c_str()), FVector2D{ X, Y }, FVector2D{ S, S }, C, 0.f, Black, FVector2D{ 1.f, 1.f },
            false, false, true, Black);
    };
    Text(L"SKATE KEYBOARD CONTROLS", X0 + 20 * S, Y0 + 12 * S, Gold);
    for (int r = 0; r < Rows; r++)
    {
        const int i = Top + r;
        const float Y = Y0 + Row * (r + 1.6f);
        const bool bSel = i == Selected;
        if (bSel)
            Canvas->K2_DrawTexture(nullptr, FVector2D{ X0 + 10 * S, Y - 2 * S }, FVector2D{ BoxW - 20 * S, Row }, FVector2D{ 0, 0 }, FVector2D{ 1, 1 },
                FLinearColor{ 0.25f, 0.25f, 0.25f, 0.9f }, EBlendMode::BLEND_Translucent, 0.f, FVector2D{ 0.5, 0.5 });
        int Uses = 0;
        for (int j = 0; j < Count; j++) if (Keys[j] && Keys[j] == Keys[i]) Uses++;
        Text(Defs[i].Label, X0 + 24 * S, Y, bSel ? White : Grey);
        const std::wstring Key = (bSel && bCapturing) ? L"press a key..." : KeyName(Keys[i]) + (Uses > 1 ? L"  (shared)" : L"");
        Text(Key, X0 + BoxW * 0.62f, Y, bSel ? Gold : White);
    }
    const float Yh = Y0 + Row * (Rows + 2.0f);
    Text(bCapturing ? L"Press the new key (Esc cancels)"
                    : L"Up/Down: choose   Enter: rebind   Backspace: default   Delete: unbind   Esc/-: close",
         X0 + 20 * S, Yh, Grey);
    Text(std::to_wstring(Selected + 1) + L" / " + std::to_wstring(Count), X0 + BoxW - 110 * S, Y0 + 12 * S, Grey);
}
