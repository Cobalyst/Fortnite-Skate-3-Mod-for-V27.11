// Client side: J toggles skate mode. While skating, the Skate 3 engine drives the local
// character; Fortnite's own movement component is deactivated (in MOVE_Walking, so emotes are allowed).
// Leaving skate mode hands the board's exact velocity to the character (mid-air included).

#include "Common.h"
#include "SDK/EnhancedInput_classes.hpp"
#include "WorldSampler.h"
#include "EngineApi.h"
#include "Avatar.h"
#include "Hud.h"
#include "Audio.h"
#include "SkateCamera.h"
#include "Share.h"
#include "EmoteHold.h"
#include "KeyBinds.h"
#include "DropperWorld.h"
#include "DropperScreen.h"
#include "DropperEditor.h"
#include "DropperInput.h"
#include "XInputFix.h"
#include "RawInputGuard.h"

#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string_view>
#include <sstream>
#include <fstream>
#include <vector>

using namespace SDK;

EngineApi Api;
SkEngine* Engine = nullptr;

namespace
{
    void BlockBuildInput(APlayerController* PC, bool bForce);
    void RestoreBuildInput(APlayerController* PC);

    const std::string EngineDll = ModPath("bin\\skate_engine.dll");
    const std::string AssetsRoot = ModPath("skate-data\\assets");

    // Traces per frame while streaming collision (2 line traces each column).
    constexpr int StreamBudget = 200;

    // ---- engine DLL ----
    std::atomic<int> EngineStatus{ 0 };  // 0 idle, 1 loading, 2 ready, -1 failed

    DWORD WINAPI LoadEngine(LPVOID)
    {
        // Loading runs while a match starts (the battle bus): keep it from competing with the game.
        // Background mode lowers this thread's disk and memory priority too, not only its CPU
        // priority: at normal disk priority the ~13 s load starved the sounds Fortnite streams
        // from disk at that moment (battle bus engine, the bus jump's skydive wind; 2026-10-09).
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        SetEnvironmentVariableA("SKATEMOD_LOG_DIR", ModPath("Logs").c_str());   // the engine writes skate_engine.log there
        HMODULE Dll = LoadLibraryA(EngineDll.c_str());
        if (!Dll) { Log("Could not load %s (error %lu)", EngineDll.c_str(), GetLastError()); EngineStatus = -1; return 1; }

        Api.Create              = (sk_create_fn)GetProcAddress(Dll, "sk_create");
        Api.SetCollision        = (sk_set_collision_fn)GetProcAddress(Dll, "sk_set_collision");
        Api.SetCollision2       = (sk_set_collision2_fn)GetProcAddress(Dll, "sk_set_collision2");
        Api.CollisionGeneration = (sk_collision_generation_fn)GetProcAddress(Dll, "sk_collision_generation");
        Api.Activate            = (sk_activate_fn)GetProcAddress(Dll, "sk_activate");
        Api.Step                = (sk_step_fn)GetProcAddress(Dll, "sk_step");
        Api.Suspend             = (sk_suspend_fn)GetProcAddress(Dll, "sk_suspend");
        Api.SetPaused           = (sk_set_paused_fn)GetProcAddress(Dll, "sk_set_paused");
        Api.LastError           = (sk_last_error_fn)GetProcAddress(Dll, "sk_last_error");
        Api.SetOption           = (sk_set_option_fn)GetProcAddress(Dll, "sk_set_option");
        Api.GetPadDebug         = (sk_get_pad_debug_fn)GetProcAddress(Dll, "sk_get_pad_debug");
        Api.GetCameraDebug      = (sk_get_camera_debug_fn)GetProcAddress(Dll, "sk_get_camera_debug");
        Api.GetAudio            = (sk_get_audio_fn)GetProcAddress(Dll, "sk_get_audio");
        Api.BoneCount           = (sk_bone_count_fn)GetProcAddress(Dll, "sk_bone_count");
        Api.BoneName            = (sk_bone_name_fn)GetProcAddress(Dll, "sk_bone_name");
        Api.BoneBind            = (sk_bone_bind_fn)GetProcAddress(Dll, "sk_bone_bind");
        Api.GetBones            = (sk_get_bones_fn)GetProcAddress(Dll, "sk_get_bones");
        Api.GetHud              = (sk_get_hud_fn)GetProcAddress(Dll, "sk_get_hud");
        Api.PopHudEvent         = (sk_pop_hud_event_fn)GetProcAddress(Dll, "sk_pop_hud_event");
        Api.BoardPartCount      = (sk_board_part_count_fn)GetProcAddress(Dll, "sk_board_part_count");
        Api.BoardPart           = (sk_board_part_fn)GetProcAddress(Dll, "sk_board_part");
        Api.SetDmos             = (sk_set_dmos_fn)GetProcAddress(Dll, "sk_set_dmos");
        Api.GetDmoPoses         = (sk_get_dmo_poses_fn)GetProcAddress(Dll, "sk_get_dmo_poses");
        Api.UprightDmo          = (sk_upright_dmo_fn)GetProcAddress(Dll, "sk_upright_dmo");
        if (!Api.Create || !Api.SetCollision || !Api.CollisionGeneration || !Api.Activate || !Api.Step || !Api.Suspend || !Api.LastError ||
            !Api.BoneCount || !Api.BoneName || !Api.BoneBind || !Api.GetBones || !Api.GetHud || !Api.PopHudEvent ||
            !Api.BoardPartCount || !Api.BoardPart)
        {
            Log("skate_engine.dll is missing exports");
            EngineStatus = -1;
            return 1;
        }

        Log("Loading Skate 3 engine (takes ~10 s the first time)...");
        const DWORD Start = GetTickCount();
        Engine = Api.Create(AssetsRoot.c_str());
        if (!Engine) { Log("Skate engine failed to load: %s", Api.LastError()); EngineStatus = -1; return 1; }
        Log("Skate engine ready in %lu ms", GetTickCount() - Start);
        EngineStatus = 2;
        Audio::Preload();  // sound banks next (one heavy load at a time), long before J is pressed
        return 0;
    }

    // ---- keyboard + mouse standing in for a controller (as in the IW4L skate mode) ----
    struct KeyboardPad
    {
        float FlickX = 0, FlickY = 0;  // mouse-driven right stick
        float Ollie = 0;               // seconds left of the scripted ollie flick
        bool  SpaceWasDown = false;
        float Bail = 0;                // seconds left of the held manual-bail combo
        bool  BailWasDown = false, HomWasDown = false, SwingWasDown = false, FlipsWasDown = false, BoardWasDown = false;
    } Keyboard;

    // Hall of Meat (Skate 3's bail mode, without its slow motion): H in skate mode, or
    // hall_of_meat in mod_settings.txt. Applied to the engine whenever it changes.
    bool bHallOfMeat = false;
    bool bBailControls = true;   // mod_settings bail_controls: arrow keys pick the bail pose
    bool bShowRails = false;     // mod_settings show_rails: draw the grind spots found around the skater
    void ApplyHallOfMeat()
    {
        if (Api.SetOption) Api.SetOption("hall_of_meat", bHallOfMeat ? 1 : 0);
    }
    // Multi body flips (N in skate mode, or multi_body_flips in mod_settings.txt): body flips
    // (front / back flips of the whole skater) keep rotating like on Normal / Hardcore instead of
    // Easy's single timed flip. Nothing else about Easy changes.
    bool bMultiBodyFlips = false;
    // On-foot D-pad gestures (Skate 3's gesture catalog 0..36), mod_settings gesture_up/down/left/right.
    int Gestures[4] = { 21, 7, 0, 24 };
    void ApplyMultiBodyFlips()
    {
        if (Api.SetOption) Api.SetOption("multi_body_flips", bMultiBodyFlips ? 1 : 0);
    }
    void SaveSetting(const char* Key, int Value);

    constexpr float MouseFlickGain = 0.02f;
    constexpr float MouseFlickReturn = 14.f;
    constexpr float OllieSeconds = 0.24f;
    // Camera movement also moves the mouse: only a quick, deliberate flick (the virtual stick
    // past this) reaches the skater as a trick flick; slower mouse movement just turns the camera.
    constexpr float MouseFlickThreshold = 0.65f;
    // Mouse speed (units/s) above which movement counts as a trick flick, not camera look.
    constexpr float MouseLookMaxSpeed = 900.f;

    // What the skate inputs did during the current jump (logged when a landing turns into a bail).
    struct AirInputs
    {
        float Seconds = 0, MaxFlick = 0, LastFlickAgo = 99.f;
        float GrabL = 0, GrabR = 0, Lb = 0, Rb = 0;  // seconds held
        void Reset() { *this = AirInputs{}; }
    } Air;

    int16_t Stick(float V) { V = V < -1 ? -1 : V > 1 ? 1 : V; return int16_t(V * 32767.f); }

    SkPad ReadKeyboard(APlayerController* PC, float Dt, const char* State)
    {
        SkPad Pad{};
        if (!GameWindowFocused() || KeyBinds::MenuOpen())
        {
            Keyboard = {};
            return Pad;
        }
        using KB = KeyBinds::Action;
        auto Key = [](KB A) { return KeyBinds::Down(A); };

        float MouseX = 0, MouseY = 0;
        if (PC) PC->GetInputMouseDelta(&MouseX, &MouseY);

        const float Decay = std::exp(-Dt * MouseFlickReturn);
        Keyboard.FlickX = Keyboard.FlickX * Decay + MouseX * MouseFlickGain;
        Keyboard.FlickY = Keyboard.FlickY * Decay + MouseY * MouseFlickGain;  // mouse up = stick up
        const float Len = std::sqrt(Keyboard.FlickX * Keyboard.FlickX + Keyboard.FlickY * Keyboard.FlickY);
        if (Len > 1.f) { Keyboard.FlickX /= Len; Keyboard.FlickY /= Len; }

        static int AppliedHom = -1;  // the engine DLL may load after the settings were read
        if (Api.SetOption && AppliedHom != int(bHallOfMeat)) { ApplyHallOfMeat(); AppliedHom = int(bHallOfMeat); }
        static int AppliedGestures[4] = { -1, -1, -1, -1 };
        if (Api.SetOption)
        {
            const char* Names[4] = { "gesture_up", "gesture_down", "gesture_left", "gesture_right" };
            for (int i = 0; i < 4; i++)
                if (AppliedGestures[i] != Gestures[i]) { Api.SetOption(Names[i], Gestures[i]); AppliedGestures[i] = Gestures[i]; }
        }
        static int AppliedFlips = -1;
        if (Api.SetOption && AppliedFlips != int(bMultiBodyFlips)) { ApplyMultiBodyFlips(); AppliedFlips = int(bMultiBodyFlips); }
        // Bails (Skate 3 wipeout controls). Any state while wiping out is a "Wipeout..." state.
        const bool bBailing = std::string_view(State).find("Wipeout") != std::string_view::npos;
        const bool Space = Key(KB::Ollie);
        if (Space && !Keyboard.SpaceWasDown && Keyboard.Ollie <= 0.f)
            Keyboard.Ollie = OllieSeconds;
        Keyboard.SpaceWasDown = Space;
        // K: bail on command - Skate 3's manual bail is both triggers fully in + both sticks clicked.
        const bool BailKey = Key(KB::BailNow);
        if (BailKey && !Keyboard.BailWasDown && !bBailing)
            Keyboard.Bail = 0.1f;
        Keyboard.BailWasDown = BailKey;
        // H: Hall of Meat on / off.
        const bool HomKey = Key(KB::HallOfMeat);
        if (HomKey && !Keyboard.HomWasDown)
        {
            bHallOfMeat = !bHallOfMeat;
            ApplyHallOfMeat();
            SaveSetting("hall_of_meat", bHallOfMeat ? 1 : 0);
            Hud::Notice(bHallOfMeat ? L"HALL OF MEAT: ON" : L"HALL OF MEAT: OFF");
            Log("Hall of Meat %s", bHallOfMeat ? "on" : "off");
        }
        Keyboard.HomWasDown = HomKey;
        // N: multi body flips on / off.
        const bool FlipsKey = Key(KB::MultiBodyFlips);
        if (FlipsKey && !Keyboard.FlipsWasDown)
        {
            bMultiBodyFlips = !bMultiBodyFlips;
            ApplyMultiBodyFlips();
            SaveSetting("multi_body_flips", bMultiBodyFlips ? 1 : 0);
            Hud::Notice(bMultiBodyFlips ? L"MULTI BODY FLIPS: ON" : L"MULTI BODY FLIPS: OFF");
            Log("Multi body flips %s", bMultiBodyFlips ? "on" : "off");
        }
        Keyboard.FlipsWasDown = FlipsKey;
        // 7: the deck graphic, Skate 3's board texture or the equipped Fortnite vehicle wrap.
        const bool BoardKey = Key(KB::BoardGraphic);
        if (BoardKey && !Keyboard.BoardWasDown)
        {
            const bool bWrap = !Avatar::BoardWrap();
            Avatar::SetBoardWrap(GetLocalCharacter(UWorld::GetWorld()), bWrap);
            SaveSetting("board_wrap", bWrap ? 1 : 0);
            Hud::Notice(bWrap ? L"BOARD: FORTNITE WRAP" : L"BOARD: SKATE 3 GRAPHIC");
            Log("Board graphic: %s", bWrap ? "Fortnite wrap" : "Skate 3");
        }
        Keyboard.BoardWasDown = BoardKey;
        // G: smack with the board (Skate 3 Shove, board-in-hand version when carrying it).
        const bool SwingKey = Key(KB::BoardSwing);
        if (SwingKey && !Keyboard.SwingWasDown && Api.SetOption)
            Api.SetOption("board_swing", 1);
        Keyboard.SwingWasDown = SwingKey;

        float RightX = Keyboard.FlickX, RightY = Keyboard.FlickY;
        const float FlickLen = std::sqrt(RightX * RightX + RightY * RightY);
        // Mouse look: smooth movement turns the camera; a quick flick (fast, or already past the
        // trick threshold) is a trick and leaves the camera alone.
        {
            const float Speed = Dt > 0 ? std::sqrt(MouseX * MouseX + MouseY * MouseY) / Dt : 0.f;
            if (FlickLen < MouseFlickThreshold && Speed < MouseLookMaxSpeed)
                SkateCamera::AddLook(MouseX, MouseY);
        }
        if (FlickLen < MouseFlickThreshold) RightX = RightY = 0;
        // Manuals: Skate 3 holds the right stick part-way back (manual) or forward (nose manual).
        // A mouse can't hold a stick, so X / V hold it (engine test: -0.4 -> Manual, +0.5 -> Nose Manual).
        // They link tricks: the combo meter drains at 20 pts/s in a manual instead of 100 while rolling.
        if (RightX == 0 && RightY == 0)
        {
            if (Key(KB::Manual)) RightY = -0.45f;
            else if (Key(KB::NoseManual)) RightY = 0.5f;
        }
        Air.Seconds += Dt;
        Air.LastFlickAgo += Dt;
        if (FlickLen >= MouseFlickThreshold) { Air.LastFlickAgo = 0; Air.MaxFlick = std::max(Air.MaxFlick, FlickLen); }
        if (Key(KB::GrabLeft)) Air.GrabL += Dt;
        if (Key(KB::GrabRight)) Air.GrabR += Dt;
        if (Key(KB::BumperLeft)) Air.Lb += Dt;
        if (Key(KB::BumperRight)) Air.Rb += Dt;
        if (Keyboard.Ollie > 0.f)
        {
            RightX = 0;
            RightY = Keyboard.Ollie > OllieSeconds * 0.5f ? -1.f : 1.f;  // stick back, then forward
            Keyboard.Ollie -= Dt;
        }

        const float Steer = float(Key(KB::SteerRight)) - float(Key(KB::SteerLeft));
        Pad.left[0] = Stick(Steer);
        Pad.right[0] = Stick(RightX);
        Pad.right[1] = Stick(RightY);
        // Off the board (Skate 3's biped states) the left stick walks: W forward, S back.
        const std::string_view Now = State;
        const bool bOnFoot = Now.rfind("Biped", 0) == 0;  // not OffBoardPushing: that's a push stroke
        // Body flips (Skate 3: "start grabbing the board and pull down just as you take off" = back
        // flip, up = front flip): while a grab key is held, or in the air, W / S are the left stick's
        // up / down instead of push / brake. Hold Z or C + W (front flip) / S (back flip) and pop.
        const bool bGrabKey = Key(KB::GrabLeft) || Key(KB::GrabRight);
        const bool bAirborne = Now.find("Air") != std::string_view::npos;
        const float Fwd = float(Key(KB::Forward)) - float(Key(KB::Backward));
        if (bOnFoot)
            Pad.left[1] = Stick(Fwd);
        else if (bGrabKey || bAirborne)
            Pad.left[1] = Stick(Fwd);
        else
        {
            if (Key(KB::Forward))  Pad.buttons |= SK_PAD_A;   // push
            if (Key(KB::Backward)) Pad.buttons |= SK_PAD_X;   // brake
        }
        // Every pad button has its own key too (KeyBinds.h; remap in game with F8).
        if (Key(KB::ButtonA))     Pad.buttons |= SK_PAD_A;
        if (Key(KB::ButtonX))     Pad.buttons |= SK_PAD_X;
        if (Key(KB::ButtonB))     Pad.buttons |= SK_PAD_B;
        if (Key(KB::ButtonY))     Pad.buttons |= SK_PAD_Y;
        if (Key(KB::BumperLeft))  Pad.buttons |= SK_PAD_LB;
        if (Key(KB::BumperRight)) Pad.buttons |= SK_PAD_RB;
        if (Key(KB::DpadUp))      Pad.buttons |= 0x0001;    // XInput D-pad up
        if (Key(KB::DpadDown))    Pad.buttons |= 0x0002;
        if (Key(KB::DpadLeft))    Pad.buttons |= 0x0004;
        if (Key(KB::DpadRight))   Pad.buttons |= 0x0008;
        if (Key(KB::Start))       Pad.buttons |= SK_PAD_START;
        if (Key(KB::Back))        Pad.buttons |= 0x0020;
        if (Key(KB::StickClickLeft))  Pad.buttons |= 0x0040;
        if (Key(KB::StickClickRight)) Pad.buttons |= 0x0080;
        Pad.triggers[0] = Key(KB::GrabLeft) ? 255 : 0;      // left grab
        Pad.triggers[1] = Key(KB::GrabRight) ? 255 : 0;     // right grab
        // Trick stick keys hold the right stick in a direction (flick tricks without a mouse:
        // e.g. down then up = ollie). In a bail they pick the bail pose, as before.
        if (!bBailing || bBailControls)
        {
            const float Kx = float(Key(KB::StickRight)) - float(Key(KB::StickLeft));
            const float Ky = float(Key(KB::StickUp)) - float(Key(KB::StickDown));
            if (Kx != 0.f || Ky != 0.f)
            {
                Pad.right[0] = Stick(Kx);
                Pad.right[1] = Stick(Ky);
            }
        }
        // During a bail the pad is exactly what it is while riding (Skate 3's clipping glitches
        // depend on it). The only addition: an arrow key, while held, picks the bail pose
        // (right stick: Skate 3's wipeout profiles up / down / left / right).

        if (Keyboard.Bail > 0.f)
        {
            Pad.triggers[0] = Pad.triggers[1] = 255;
            Pad.buttons |= 0x0040 | 0x0080;                 // both stick clicks
            Keyboard.Bail -= Dt;
        }
        return Pad;
    }

    // ---- skate mode state ----
    enum class EState { Off, WaitingEngine, WaitingCollision, Skating };
    EState State = EState::Off;

    ACharacter* Skater = nullptr;
    WorldSampler Sampler;
    std::vector<float> Triangles;

    // Reads skate-data/settings/mod_settings.txt ("name value" lines, # comments). Read on each
    // skate-mode start, so a change applies after leaving and re-entering skate mode.
    void LoadModSettings()
    {
        std::ifstream In(ModPath("skate-data\\settings\\mod_settings.txt"));
        std::string Line;
        while (std::getline(In, Line))
        {
            if (Line.empty() || Line[0] == '#') continue;
            std::istringstream L(Line);
            std::string Key;
            int Value = 0;
            if (!(L >> Key >> Value)) continue;
            if (Key == "solid_walls") Sampler.bSolidWalls = Value != 0;
            if (Key == "hall_of_meat") bHallOfMeat = Value != 0;
            if (Key == "multi_body_flips") bMultiBodyFlips = Value != 0;
            if (Key == "gesture_up" && Value >= 0 && Value < 37) Gestures[0] = Value;
            if (Key == "gesture_down" && Value >= 0 && Value < 37) Gestures[1] = Value;
            if (Key == "gesture_left" && Value >= 0 && Value < 37) Gestures[2] = Value;
            if (Key == "gesture_right" && Value >= 0 && Value < 37) Gestures[3] = Value;
            if (Key == "bail_controls") bBailControls = Value != 0;
            if (Key == "show_rails") bShowRails = Value != 0;
            if (Key == "xray") Hud::SetXray(Value != 0);
            if (Key == "board_wrap") Avatar::SetBoardWrap(nullptr, Value != 0);
            if (Key == "board_wrap_material") Avatar::SetWrapMaterialMode(Value != 0);
            if (Key == "wrap_survey" && Value) Avatar::RequestWrapSurvey();
        }
        Log("Settings: solid walls %s, hall of meat %s", Sampler.bSolidWalls ? "on" : "off", bHallOfMeat ? "on" : "off");
        ApplyHallOfMeat();
    }

    // Rewrites one "key value" line of mod_settings.txt (appends it when missing).
    void SaveSetting(const char* Key, int Value)
    {
        const std::string PathS = ModPath("skate-data\\settings\\mod_settings.txt");
        const char* Path = PathS.c_str();
        std::vector<std::string> Lines;
        {
            std::ifstream In(Path);
            for (std::string L; std::getline(In, L);) Lines.push_back(L);
        }
        bool bFound = false;
        for (auto& L : Lines)
        {
            std::istringstream S(L);
            std::string K;
            if (L.empty() || L[0] == '#' || !(S >> K) || K != Key) continue;
            L = std::string(Key) + " " + std::to_string(Value);
            bFound = true;
        }
        if (!bFound) Lines.push_back(std::string(Key) + " " + std::to_string(Value));
        std::ofstream Out(Path, std::ios::trunc);
        for (auto& L : Lines) Out << L << "\n";
    }
    std::vector<float> RailPoints;       // grind rails along ledges (WorldSampler::BuildRails)
    std::vector<uint32_t> RailLengths;
    uint64_t WaitingForGeneration = 0;
    float WaitSeconds = 0;
    float HalfHeight = 90.f;
    SkState Sk{};
    bool JWasDown = false;
    bool bSkatingNow = false;
    FVector Placed{};      // where we put the character last frame
    bool bPlaced = false;
    float FacingYaw = 0;  // smoothed travel direction the character faces while skating
    bool bBlockedMoveInput = false;
    float StatusTimer = 0;

    FVector Feet(ACharacter* Character)
    {
        FVector P = Character->K2_GetActorLocation();
        P.Z -= HalfHeight;
        return P;
    }

    std::vector<float> DropperTris;
    std::vector<uint8_t> DropperEdges;
    std::vector<uint16_t> DropperSurfaces;
    uint64_t SentDropperGeneration = 0;
    uint64_t SentDropperPose = 0;        // DropperWorld::PoseGeneration() in the last patch
    ULONGLONG SentDropperPoseAt = 0;
    int DropperLogged = 0;

    // Object-dropper DMOs as rigid bodies in the skate engine (DropperWorld::BuildPhysics).
    uint64_t SentDmoGeneration = 0;
    std::vector<SkDmo> DmoBodies;
    std::vector<SkDmoVolume> DmoVolumes;
    std::vector<SkDmoTriangle> DmoTriangles;
    std::vector<SkDmoPose> DmoPoses;
    std::vector<SkDmoSpline> DmoSplines;
    std::vector<float> DmoPoints;
    int HeldDmo = 0;   // the prop the skater holds (OffBoardPushing), from the poses read back

    void SendDmos()
    {
        if (!Api.SetDmos || SentDmoGeneration == DropperWorld::Generation()) return;
        SentDmoGeneration = DropperWorld::Generation();
        DropperWorld::BuildPhysics(DmoBodies, DmoVolumes, DmoTriangles, DmoSplines, DmoPoints);
        if (!Api.SetDmos(Engine, DmoBodies.data(), uint32_t(DmoBodies.size()), DmoVolumes.data(), uint32_t(DmoVolumes.size()),
                         DmoTriangles.data(), uint32_t(DmoTriangles.size()), DmoSplines.data(), uint32_t(DmoSplines.size()),
                         DmoPoints.data(), uint32_t(DmoPoints.size() / 3)))
            Log("[dropper] SetDmos failed: %s", Api.LastError());
        static size_t Logged = ~size_t(0);
        if (Logged != DmoBodies.size())
        {
            Logged = DmoBodies.size();
            Log("[dropper] %zu props are rigid bodies (%zu volumes, %zu triangles)", DmoBodies.size(), DmoVolumes.size(), DmoTriangles.size());
        }
    }

    void ReadDmoPoses()
    {
        if (!Api.GetDmoPoses) return;
        const uint32_t N = Api.GetDmoPoses(Engine, nullptr, 0);
        if (!N) return;
        DmoPoses.resize(N);
        const uint32_t Got = std::min(N, Api.GetDmoPoses(Engine, DmoPoses.data(), N));
        bool bSettled = false;
        HeldDmo = 0;
        for (uint32_t i = 0; i < Got; i++)
        {
            const SkDmoPose& P = DmoPoses[i];
            const DropperWorld::Quat Q{ P.rotation[0], P.rotation[1], P.rotation[2], P.rotation[3] };
            bSettled |= DropperWorld::ApplyPhysicsPose(int(P.id), FVector{ P.position[0], P.position[1], P.position[2] }, Q, (P.awake & 1) != 0);
            if (P.awake & 2) HeldDmo = int(P.id);
        }
        // A prop came to rest somewhere new: its grind rails, the server's copy and the engine's
        // body set follow (the bodies keep their state: the poses sent are the ones just read).
        if (bSettled)
        {
            DropperWorld::Touch();
            SentDmoGeneration = DropperWorld::Generation();
        }
    }

    // ---- Holding a prop on foot: Skate 3's action overlay (HUD state 82648B48 / 82648800) ----
    uint32_t SeenMarkerPlaced = 0, SeenMarkerReturned = 0;
    bool bMarkerCountsSeen = false, bMarkerWasSet = false;

    void TickPropActions(float Dt)
    {
        // The session marker's prop list follows the marker: placing (or clearing) one forgets
        // the props (SessionMarker vt+0x20 / +0x28 -> +0x3C), going to it puts them back (+0x38).
        uint32_t Placed = 0, Returned = 0;
        bool bSet = false;
        Hud::MarkerCounts(Placed, Returned, bSet);
        if (!bMarkerCountsSeen) { SeenMarkerPlaced = Placed; SeenMarkerReturned = Returned; bMarkerWasSet = bSet; bMarkerCountsSeen = true; }
        if (Placed != SeenMarkerPlaced || (bMarkerWasSet && !bSet)) DropperWorld::MarkerClear();
        if (Returned != SeenMarkerReturned) DropperWorld::MarkerRestore();
        SeenMarkerPlaced = Placed;
        SeenMarkerReturned = Returned;
        bMarkerWasSet = bSet;

        bool Params[5] = {};
        const bool bShow = HeldDmo != 0 && !DropperScreen::IsOpen() && DropperWorld::Get(HeldDmo);
        if (bShow)
        {
            Params[0] = DropperWorld::IsUpright(HeldDmo);
            Params[1] = !DropperWorld::CanReset(HeldDmo);
            Params[2] = bSet;
            Params[3] = DropperWorld::MarkerHas(HeldDmo);
            Params[4] = DropperWorld::MarkerFull();
        }
        Hud::SetActionOverlay(bShow, Params);
        if (!bShow || Hud::MarkerMenuOpen()) return;   // LB + D-pad belongs to the marker menu
        DropperInput::Update(Dt);
        const DropperInput::State& In = DropperInput::Get();
        if (In.Pressed[DropperInput::DPadU] && !Params[0])
        {
            Hud::QueueFeSound("sp_upright_object");
            if (Api.UprightDmo) Api.UprightDmo(Engine, uint32_t(HeldDmo));
        }
        if (In.Pressed[DropperInput::DPadD] && !Params[1])
        {
            Hud::QueueFeSound("sp_reset_object");
            DropperWorld::Reset(HeldDmo);
        }
        if (In.Pressed[DropperInput::DPadR])
        {
            Hud::QueueFeSound("sp_add_to_marker");
            if (bSet) DropperWorld::MarkerAdd(HeldDmo);   // the manager refuses when full
        }
        if (In.Pressed[DropperInput::DPadL])
        {
            Hud::QueueFeSound("sp_remove_from_marker");
            if (bSet) DropperWorld::MarkerRemove(HeldDmo);
        }
    }

    void SendPatch(const char* Why)
    {
        Sampler.BuildTriangles(Triangles);
        if (Triangles.empty())
        {
            Log("Collision patch (%s) is empty: nothing below the skater?", Why);
            return;
        }
        Sampler.BuildRails(RailPoints, RailLengths);
        static int Patches = 0;
        if (Patches++ % 10 == 0 || std::string_view(Why) == "enter")
        {
            Log("Collision patch (%s): %zu triangles, %zu grind rails", Why, Triangles.size() / 9, RailLengths.size());
            Log("[grind] world meshes: %s", Sampler.ShapeSummary().c_str());
        }
        // Placed object-dropper objects: their own Skate 3 collision (native edge codes and surface
        // ids) and authored grind splines, within the patch.
        DropperTris.clear(); DropperEdges.clear(); DropperSurfaces.clear();
        DropperWorld::AppendCollision(Sampler.PatchCentre(), Sampler.PatchRadius() + 500.0, DropperTris, DropperEdges, DropperSurfaces,
                                      RailPoints, RailLengths, [](float X, float Y) { return Sampler.GroundAt(X, Y); });
        SentDropperGeneration = DropperWorld::Generation();
        SentDropperPose = DropperWorld::PoseGeneration();
        SentDropperPoseAt = GetTickCount64();
        const bool bOk = Api.SetCollision2
            ? Api.SetCollision2(Engine, Triangles.data(), uint32_t(Triangles.size() / 9),
                                RailPoints.empty() ? nullptr : RailPoints.data(), RailLengths.empty() ? nullptr : RailLengths.data(),
                                uint32_t(RailLengths.size()), DropperTris.empty() ? nullptr : DropperTris.data(),
                                DropperEdges.empty() ? nullptr : DropperEdges.data(), DropperSurfaces.empty() ? nullptr : DropperSurfaces.data(),
                                uint32_t(DropperSurfaces.size()))
            : Api.SetCollision(Engine, Triangles.data(), uint32_t(Triangles.size() / 9),
                               RailPoints.empty() ? nullptr : RailPoints.data(), RailLengths.empty() ? nullptr : RailLengths.data(),
                               uint32_t(RailLengths.size()));
        if (!bOk)
            Log("SetCollision failed: %s", Api.LastError());
        else if (!DropperSurfaces.empty() && DropperLogged++ < 20)
            Log("[dropper] %zu object triangles in the collision patch", DropperSurfaces.size());
    }

    // The held item (pickaxe, gun...) stays equipped and usable while skating, just invisible.
    // AFortPawn::CurrentWeapon (AFortWeapon*) at 0x9F8; Fortnite can re-show it, so this runs per frame.
    // Remembered by its GObjects slot: a weapon can be destroyed (dropped, used up, swapped by the
    // game) while hidden, and touching a freed actor crashes. Valid only while the slot still holds it.
    struct WeakActor
    {
        AActor* Ptr = nullptr;
        int32 Index = -1;
        void Set(AActor* A) { Ptr = A; Index = A ? A->Index : -1; }
        AActor* Get() const
        {
            if (!Ptr || Index < 0 || Index >= UObject::GObjects->Num()) return nullptr;
            return UObject::GObjects->GetByIndex(Index) == Ptr ? Ptr : nullptr;
        }
    } HiddenWeapon;

    AActor* CurrentWeapon(ACharacter* Character)
    {
        static UClass* FortPawn = UObject::FindClassFast("FortPawn");
        if (!Character || !FortPawn || !Character->IsA(FortPawn)) return nullptr;
        AActor* W = *reinterpret_cast<AActor**>(reinterpret_cast<uint8*>(Character) + 0x9F8);
        return W && W->IsA(AActor::StaticClass()) ? W : nullptr;
    }

    void HideHeldItem(ACharacter* Character)
    {
        AActor* W = CurrentWeapon(Character);
        AActor* Old = HiddenWeapon.Get();
        if (Old && Old != W)
            Old->SetActorHiddenInGame(false);  // switched items: the old one goes back to normal
        if (W && !W->bHidden)
            W->SetActorHiddenInGame(true);
        HiddenWeapon.Set(W);
    }

    void ShowHeldItem(bool bCharacterAlive)
    {
        if (AActor* W = HiddenWeapon.Get(); W && bCharacterAlive)
            W->SetActorHiddenInGame(false);
        HiddenWeapon.Set(nullptr);
    }

    EmoteHold::Hold ClientEmotes;  // non-traversal emotes while skating (EmoteHold.h)

    // Skate 3's object dropper (LB + B in the session marker menu while skating).
    int DropperLoad = 0;   // 0 not tried, 1 loaded, -1 failed

    void EnsureDropper()
    {
        if (DropperLoad) return;
        DropperScreen::SetLog([](const std::string& S) { Log("%s", S.c_str()); });
        DropperScreen::SetSoundSink([](const char* Cue) { Hud::QueueFeSound(Cue); });
        std::string Error;
        DropperLoad = DropperScreen::Load(ModPath("skate-data").c_str(), Error) ? 1 : -1;
        if (DropperLoad < 0) Log("Object dropper unavailable: %s", Error.c_str());
        else if (!Api.SetPaused) Log("Object dropper: this skate_engine.dll cannot pause the skater (rebuild it)");
        Hud::SetDropperAvailable(DropperLoad > 0 && Api.SetPaused);
    }

    void Stop(bool bKeepMomentum, const char* Why)
    {
        if (DropperScreen::IsOpen())
        {
            DropperScreen::Cancel();
            DropperScreen::TakeClosed();
        }
        if (State == EState::Skating && Skater)
        {
            UCharacterMovementComponent* Move = Skater->CharacterMovement;
            const FVector Velocity{ Sk.velocity[0], Sk.velocity[1], Sk.velocity[2] };
            Skater->SetReplicateMovement(true);
            if (Move)
            {
                Move->Activate(true);
                Move->SetComponentTickEnabled(true);
                // Falling with the board's velocity: if we are on the ground the character
                // lands next tick and keeps sliding; if airborne it keeps flying the same arc.
                Move->SetMovementMode(EMovementMode::MOVE_Falling, 0);
                if (bKeepMomentum)
                {
                    Move->Velocity = Velocity;
                    Skater->LaunchCharacter(Velocity, true, true);
                }
            }
            Log("Skate mode OFF (%s). Exit velocity (%.0f, %.0f, %.0f) cm/s", Why,
                Velocity.X, Velocity.Y, Velocity.Z);
        }
        else if (State != EState::Off)
        {
            Log("Skate mode cancelled (%s)", Why);
        }
        if (State == EState::Skating)
        {
            Avatar::End(Skater);
            Hud::Stop();
            Audio::Stop();
            SkateCamera::End(GetLocalController(UWorld::GetWorld()), Skater);
            EmoteHold::Release(GetLocalController(UWorld::GetWorld()), ClientEmotes, "client");
            ShowHeldItem(Skater != nullptr);
            const FVector At = Skater ? Skater->K2_GetActorLocation() : Placed;
            WriteSkateShare(false, At.X, At.Y, At.Z, FacingYaw, Sk.velocity);
        }
        bSkatingNow = false;
        if (bBlockedMoveInput)
        {
            // Always undo, even if the character is gone: the controller outlives it.
            if (APlayerController* PC = GetLocalController(UWorld::GetWorld()))
            {
                PC->ResetIgnoreMoveInput();
                RestoreBuildInput(PC);
            }
            bBlockedMoveInput = false;
        }
        if (Engine && State == EState::Skating)
            Api.Suspend(Engine);
        State = EState::Off;
        Skater = nullptr;
    }

    // Fortnite's sprint is about 550 cm/s on the ground (running about 410).
    constexpr float SprintSpeed = 500.f;

    void BeginEnter(UWorld* World, ACharacter* Character)
    {
        Skater = Character;
        HalfHeight = Character->CapsuleComponent ? Character->CapsuleComponent->GetScaledCapsuleHalfHeight() : 90.f;

        // One synchronous patch so the ground exists before the skater is placed on it.
        LARGE_INTEGER A, B, F;
        QueryPerformanceFrequency(&F);
        QueryPerformanceCounter(&A);
        Sampler.Begin(Feet(Character));
        Sampler.Step(Character, 1 << 30);
        QueryPerformanceCounter(&B);
        Log("Sampled %d collision columns around the skater in %.1f ms",
            Sampler.Traced(), double(B.QuadPart - A.QuadPart) * 1000.0 / double(F.QuadPart));

        WaitingForGeneration = Api.CollisionGeneration(Engine);
        SendPatch("enter");
        WaitSeconds = 0;
        State = EState::WaitingCollision;
    }

    void FinishEnter(ACharacter* Character)
    {
        APlayerController* PC = Character->Controller && Character->Controller->IsA(APlayerController::StaticClass())
            ? static_cast<APlayerController*>(Character->Controller) : nullptr;
        const float Yaw = PC ? float(PC->GetControlRotation().Yaw) : float(Character->K2_GetActorRotation().Yaw);

        const FVector At = Feet(Character);
        const float Spawn[3] = { float(At.X), float(At.Y), float(At.Z) };

        // Stepping onto the board at sprint speed or faster keeps that momentum (like leaving
        // skate mode does): the board starts with the player's velocity, facing where they move.
        float Velocity[3] = { 0, 0, 0 };
        float StartYaw = Yaw;
        if (UCharacterMovementComponent* Move = Character->CharacterMovement)
        {
            const FVector V = Move->Velocity;
            const float Ground = float(std::sqrt(V.X * V.X + V.Y * V.Y));
            if (Ground >= SprintSpeed)
            {
                Velocity[0] = float(V.X);
                Velocity[1] = float(V.Y);
                Velocity[2] = float(V.Z);
                StartYaw = float(std::atan2(V.Y, V.X) * 57.29577951308232);
                Log("Entering skate mode at %.0f cm/s, keeping that speed", Ground);
            }
        }
        if (!Api.Activate(Engine, Spawn, StartYaw, Velocity, &Sk))
        {
            Log("Skate activate failed: %s", Api.LastError());
            State = EState::Off;
            Skater = nullptr;
            return;
        }

        if (UCharacterMovementComponent* Move = Character->CharacterMovement)
        {
            // Walking (not None): Fortnite cancels emotes unless the character is "on the ground".
            // The component is deactivated below, so it never actually moves the character.
            Move->SetMovementMode(EMovementMode::MOVE_Walking, 0);
            Move->Velocity = FVector{};
            // Pause Fortnite's movement entirely: no moves go to the server, so the server never
            // answers with corrections that drag the skater back. The server follows the skater
            // through the shared-memory link instead (Share.h).
            Move->SetComponentTickEnabled(false);
            Move->Deactivate();  // Fortnite ticks movement through its own subsystem; inactive stops that too
        }
        // Don't send movement to the server while skating (it answers with corrections).
        Character->SetReplicateMovement(false);
        State = EState::Skating;
        bSkatingNow = true;
        FacingYaw = StartYaw;
        bPlaced = false;
        // Fortnite's own movement input (WASD) would cancel emotes; skate controls read the keys directly.
        if (PC)
        {
            PC->SetIgnoreMoveInput(true);
            bBlockedMoveInput = true;
            BlockBuildInput(PC, true);
        }
        LoadModSettings();
        Hud::Start();
        Audio::Start();
        SkateCamera::Begin(UWorld::GetWorld(), PC);
        if (!Avatar::Begin(Character))
            Log("Skater animation unavailable; skating with Fortnite's own animation");
        Log("Skate mode ON at (%.0f, %.0f, %.0f) yaw %.0f", At.X, At.Y, At.Z, StartYaw);

    }

    // Per-second performance stats while skating (see [perf] lines in the log).
    struct Perf
    {
        int Frames = 0;
        int Moved = 0;          // frames where something else moved the character since we placed it
        double MaxMoved = 0;    // biggest such move (cm): server corrections show up here
        double MaxDt = 0, MaxStep = 0, MaxAvatar = 0, MaxAudio = 0, MaxSample = 0, MaxTotal = 0, MaxJump = 0;
        double MaxEngine = 0, MaxMove = 0, MaxCam = 0;  // parts of "step": engine call, moving the character, camera
        double MaxKeys = 0, MaxDropperOpen = 0, MaxProps = 0, MaxShare = 0;   // the rest of "step"
        DWORD WorstFaults = 0;  // page faults during the slowest skate update (a stall with many = Windows paging)
        uint64_t FirstTick = 0;
        uint64_t FirstGeneration = 0;
        float Timer = 0;
    } Stats;

    double Ms(const LARGE_INTEGER& A, const LARGE_INTEGER& B)
    {
        static LARGE_INTEGER F{};
        if (!F.QuadPart) QueryPerformanceFrequency(&F);
        return double(B.QuadPart - A.QuadPart) * 1000.0 / double(F.QuadPart);
    }

    // Building keys share keys with the skate controls: while skating, Fortnite's building
    // input mapping contexts (Enhanced Input) are taken out, and put back afterwards.
    std::vector<std::pair<UInputMappingContext*, int32>> RemovedContexts;
    ULONGLONG LastContextScan = 0;

    // The Enhanced Input subsystem's mapping-context functions belong to the interface
    // EnhancedInputSubsystemInterface. (The SDK wrappers look them up on the subsystem's own class,
    // where they don't exist, and crash on the null function.) Called through ProcessEvent directly.
    UObject* InputSubsystem(APlayerController* PC)
    {
        if (!PC) return nullptr;
        return USubsystemBlueprintLibrary::GetLocalPlayerSubSystemFromPlayerController(PC, UEnhancedInputLocalPlayerSubsystem::StaticClass());
    }

    UFunction* InputFunction(const char* Name)
    {
        static UClass* Interface = nullptr;
        if (!Interface)
        {
            Interface = UObject::FindClassFast("EnhancedInputSubsystemInterface");
            if (!Interface) return nullptr;
        }
        return Interface->GetFunction("EnhancedInputSubsystemInterface", Name);
    }

    void CallInput(UObject* Sub, UFunction* Func, void* Parms)
    {
        const auto Flags = Func->FunctionFlags;
        Func->FunctionFlags |= 0x400;
        Sub->ProcessEvent(Func, Parms);
        Func->FunctionFlags = Flags;
    }

    bool HasContext(UObject* Sub, UInputMappingContext* Context, int32* Priority)
    {
        static UFunction* Func = InputFunction("HasMappingContext");
        if (!Func) return false;
        struct { const UInputMappingContext* Context; int32 Priority; bool Return; uint8 Pad[3]; } P{ Context, 0, false, {} };
        CallInput(Sub, Func, &P);
        if (Priority) *Priority = P.Priority;
        return P.Return;
    }

    void RemoveContext(UObject* Sub, UInputMappingContext* Context)
    {
        static UFunction* Func = InputFunction("RemoveMappingContext");
        if (!Func) return;
        struct { const UInputMappingContext* Context; uint8 Options; uint8 Pad[7]; } P{ Context, 0, {} };
        CallInput(Sub, Func, &P);
    }

    void AddContext(UObject* Sub, UInputMappingContext* Context, int32 Priority)
    {
        static UFunction* Func = InputFunction("AddMappingContext");
        if (!Func) return;
        struct { const UInputMappingContext* Context; int32 Priority; uint8 Options; uint8 Pad[3]; } P{ Context, Priority, 0, {} };
        CallInput(Sub, Func, &P);
    }

    void BlockBuildInputUnsafe(APlayerController* PC, bool bForce)
    {
        const ULONGLONG Now = GetTickCount64();
        if (!bForce && Now - LastContextScan < 500)
            return;  // Fortnite re-adds contexts when it swaps modes; check twice a second
        LastContextScan = Now;
        UObject* Sub = InputSubsystem(PC);
        if (!Sub) return;
        if (!InputFunction("HasMappingContext"))
        {
            static bool Logged = false;
            if (!Logged) { Logged = true; Log("Building-key blocker: Enhanced Input functions not found"); }
            return;
        }
        std::string Active, Removed;
        for (int i = 0; i < UObject::GObjects->Num(); i++)
        {
            UObject* O = UObject::GObjects->GetByIndex(i);
            if (!O || !O->IsA(UInputMappingContext::StaticClass()))
                continue;
            std::string Name = O->GetName();
            if (Name.rfind("Default__", 0) == 0)
                continue;
            int32 Priority = 0;
            if (!HasContext(Sub, static_cast<UInputMappingContext*>(O), &Priority))
                continue;
            Active += Name + "(" + std::to_string(Priority) + ") ";
            std::string Lower = Name;
            for (char& C : Lower) C = char(tolower(C));
            if (Lower.find("build") == std::string::npos)
                continue;
            RemoveContext(Sub, static_cast<UInputMappingContext*>(O));
            bool Known = false;
            for (auto& R : RemovedContexts) Known |= R.first == O;
            if (!Known) RemovedContexts.push_back({ static_cast<UInputMappingContext*>(O), Priority });
            Removed += Name + " ";
        }
        if (bForce || !Removed.empty())
            Log("Input contexts while skating: %s| removed (building): %s", Active.c_str(), Removed.empty() ? "none" : Removed.c_str());
    }

    void RestoreBuildInputUnsafe(APlayerController* PC)
    {
        UObject* Sub = InputSubsystem(PC);
        if (Sub)
            for (auto& [Context, Priority] : RemovedContexts)
                if (!HasContext(Sub, Context, nullptr))
                    AddContext(Sub, Context, Priority);
        if (!RemovedContexts.empty()) Log("Building input restored (%zu contexts)", RemovedContexts.size());
        RemovedContexts.clear();
    }

    // A failure in the building-key blocker must never take skating, the HUD or sound down with it.
    bool bBuildBlockBroken = false;

    void BlockBuildInput(APlayerController* PC, bool bForce)
    {
        if (bBuildBlockBroken) return;
        __try { BlockBuildInputUnsafe(PC, bForce); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            bBuildBlockBroken = true;
            Log("Building-key blocker failed (exception 0x%08X); turned off for this session", GetExceptionCode());
        }
    }

    void RestoreBuildInput(APlayerController* PC)
    {
        __try { RestoreBuildInputUnsafe(PC); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("Restoring building input failed (exception 0x%08X)", GetExceptionCode());
        }
    }

    // Once a minute while skating: what the game holds, to find what piles up over a long session
    // (reported: FPS falls steadily the longer skate mode runs). One pass over GObjects
    // (pointer compares only), a few milliseconds once a minute.
    void LogHealth(UWorld* World, float Dt, bool bSkating)
    {
        static float Timer = 0, FrameTime = 0;
        static int Frames = 0;
        Timer += Dt; FrameTime += Dt; Frames++;
        // The game's own frames, counted by its real-time clock (it advances once per world tick),
        // independent of how often the HUD hook runs; and the slowest second of the minute.
        static double LastReal = -1, SecondStart = -1;
        static int WorldFrames = 0, SecondFrames = 0, SlowestSecond = 1 << 30;
        const double Real = UGameplayStatics::GetRealTimeSeconds(World);
        if (Real != LastReal)
        {
            LastReal = Real;
            WorldFrames++;
            SecondFrames++;
            if (SecondStart < 0) SecondStart = Real;
            if (Real - SecondStart >= 1.0)
            {
                SlowestSecond = std::min(SlowestSecond, SecondFrames);
                SecondFrames = 0;
                SecondStart = Real;
            }
        }
        if (Timer < 60.f) return;
        static UClass* Mid = UObject::FindClassFast("MaterialInstanceDynamic");
        static UClass* Tex = UObject::FindClassFast("Texture2D");
        static UClass* Proc = UObject::FindClassFast("ProceduralMeshComponent");
        static UClass* Skin = UObject::FindClassFast("SkeletalMeshComponent");
        static UClass* Pose = UObject::FindClassFast("PoseableMeshComponent");
        static UClass* Sound = UObject::FindClassFast("AudioComponent");
        static UClass* Niagara = UObject::FindClassFast("NiagaraComponent");
        static UClass* Decal = UObject::FindClassFast("DecalComponent");
        LARGE_INTEGER A, B;
        QueryPerformanceCounter(&A);
        int Live = 0, NMid = 0, NTex = 0, NProc = 0, NSkin = 0, NPose = 0, NSound = 0, NNiagara = 0, NDecal = 0;
        const int32 N = UObject::GObjects->Num();
        for (int32 i = 0; i < N; i++)
        {
            UObject* O = UObject::GObjects->GetByIndex(i);
            if (!O) continue;
            Live++;
            const UClass* C = O->Class;
            if (C == Mid) NMid++;
            else if (C == Tex) NTex++;
            else if (C == Proc) NProc++;
            else if (C == Pose) NPose++;
            else if (C == Sound) NSound++;
            else if (C == Niagara) NNiagara++;
            else if (C == Decal) NDecal++;
            else if (Skin && O->IsA(Skin)) NSkin++;
        }
        int Actors = 0;
        if (World && World->PersistentLevel) Actors = World->PersistentLevel->Actors.Num();
        QueryPerformanceCounter(&B);
        PROCESS_MEMORY_COUNTERS Mem{};
        K32GetProcessMemoryInfo(GetCurrentProcess(), &Mem, sizeof(Mem));
        MEMORYSTATUSEX Ram{ sizeof(Ram) };
        GlobalMemoryStatusEx(&Ram);
        const Audio::Stats Snd = Audio::GetStats();
        const ModFrameCost Mod = TakeModFrameCost();
        const std::string TopCalls = TakeTopGameCalls(12, Mod.Frames);
        // A fixed amount of arithmetic, timed: if it takes longer as the session goes on, the CPU
        // itself is slower (heat / power throttling, or other programs), not the mod's work.
        LARGE_INTEGER P0, P1;
        QueryPerformanceCounter(&P0);
        volatile double Acc = 1.0;
        for (int i = 1; i < 2000000; i++) Acc = Acc * 1.0000001 + 1.0 / double(i);
        QueryPerformanceCounter(&P1);
        Log("[health] %s: %.0f fps over the last minute (game clock: %.0f, slowest second %d), the mod's share %.2f ms a frame (HUD %.2f, worst %.1f), %.0f game calls a frame | %d live objects (%d slots), %d actors in the level | dynamic materials %d, textures %d, "
            "procedural meshes %d, skeletal meshes %d, pose meshes %d, audio %d, niagara %d, decals %d | skate sound: %d voices, %d modules, %d classes, %d one-shots | "
            "game %.1f GB, system RAM %lu%% | scan %.1f ms",
            bSkating ? "skating" : "not skating", Frames / FrameTime, WorldFrames / FrameTime, SlowestSecond == (1 << 30) ? 0 : SlowestSecond, Mod.Frames ? Mod.TotalMs / double(Mod.Frames) : 0.0, Mod.Frames ? Mod.HudMs / double(Mod.Frames) : 0.0, Mod.WorstMs,
            Mod.Frames ? double(Mod.GameCalls) / double(Mod.Frames) : 0.0, Live, N, Actors, NMid, NTex, NProc, NSkin, NPose, NSound, NNiagara, NDecal, Snd.Voices, Snd.Modules, Snd.Classes, Snd.OneShots,
            double(Mem.WorkingSetSize) / 1073741824.0, Ram.dwMemoryLoad, Ms(A, B));
        double PadCall = 0, PadWait = 0;
        XInputFix::TakeStats(PadCall, PadWait);
        Log("[health] cpu check %.2f ms (same work every minute: higher = slower CPU) | controller: slowest read %.1f ms, longest wait %.1f ms | most called game functions per frame: %s",
            Ms(P0, P1), PadCall, PadWait, TopCalls.c_str());
        Timer = 0; FrameTime = 0; Frames = 0; WorldFrames = 0; SlowestSecond = 1 << 30;
    }

    void TickSkating(UWorld* World, ACharacter* Character, float Dt)
    {
        LARGE_INTEGER T0, T1, TA, T2, T3;
        QueryPerformanceCounter(&T0);
        PROCESS_MEMORY_COUNTERS Mem0{};
        K32GetProcessMemoryInfo(GetCurrentProcess(), &Mem0, sizeof(Mem0));
        APlayerController* PC = GetLocalController(World);
        const SkPad Pad = ReadKeyboard(PC, Dt, Sk.state);
        LARGE_INTEGER K1; QueryPerformanceCounter(&K1);

        // Object dropper: LB + B in the session marker menu opens it; the skater waits frozen.
        EnsureDropper();
        if (!DropperScreen::IsOpen() && DropperLoad > 0 && Api.SetPaused && Hud::MarkerMenuOpen())
        {
            DropperInput::Update(Dt);
            if (DropperInput::Get().Pressed[DropperInput::B])
            {
                const float Fx = Sk.camera_forward[0], Fy = Sk.camera_forward[1];
                const double Yaw = (Fx * Fx + Fy * Fy > 1e-6f) ? std::atan2(Fy, Fx) * 57.29577951308232 : double(FacingYaw);
                if (DropperScreen::Open(World, FVector{ Sk.position[0], Sk.position[1], Sk.position[2] }, Yaw))
                {
                    Api.SetPaused(Engine, true);
                    Log("[dropper] opened at %.0f %.0f %.0f", Sk.position[0], Sk.position[1], Sk.position[2]);
                }
            }
        }

        // Did anything else move the character since we placed it (server correction, push)?
        if (bPlaced)
        {
            const FVector Now = Character->K2_GetActorLocation();
            const double Off = std::sqrt((Now.X - Placed.X) * (Now.X - Placed.X) + (Now.Y - Placed.Y) * (Now.Y - Placed.Y) + (Now.Z - Placed.Z) * (Now.Z - Placed.Z));
            if (Off > 1.0)
            {
                Stats.Moved++;
                Stats.MaxMoved = std::max(Stats.MaxMoved, Off);
            }
        }

        const float Before[3] = { Sk.position[0], Sk.position[1], Sk.position[2] };
        const float SpeedBefore = std::sqrt(Sk.velocity[0] * Sk.velocity[0] + Sk.velocity[1] * Sk.velocity[1] + Sk.velocity[2] * Sk.velocity[2]);
        LARGE_INTEGER S0, S1, S2, S3;
        QueryPerformanceCounter(&S0);
        SendDmos();
        const bool bStepped = Api.Step(Engine, Dt, &Pad, &Sk);
        QueryPerformanceCounter(&S1);
        if (bStepped) { ReadDmoPoses(); TickPropActions(Dt); }
        LARGE_INTEGER P1; QueryPerformanceCounter(&P1);
        if (!bStepped)
        {
            Log("Skate step failed (the engine restarts itself; J works again in a few seconds): %.300s", Api.LastError());
            // The rebuilt engine starts with no props: send them again on the next skate step.
            SentDmoGeneration = ~0ull;
            Stop(true, "engine error");
            return;
        }

        // Bail diagnostics: what the skate inputs did during the jump that ended in a wipeout.
        {
            static std::string LastState;
            const std::string Now = Sk.state;
            if (Now != LastState)
            {
                const bool InAir = Now.find("Air") != std::string::npos, WasAir = LastState.find("Air") != std::string::npos;
                if (InAir && !WasAir) Air.Reset();
                if (Now == "WipeoutGround")
                    Log("[bail] from %s: air %.2f s, last trick flick %.2f s before (max %.2f), grab Z %.2f s / C %.2f s, Shift %.2f s, Ctrl %.2f s, speed %.0f cm/s",
                        LastState.c_str(), Air.Seconds, Air.LastFlickAgo, Air.MaxFlick, Air.GrabL, Air.GrabR, Air.Lb, Air.Rb,
                        std::sqrt(Sk.velocity[0] * Sk.velocity[0] + Sk.velocity[1] * Sk.velocity[1] + Sk.velocity[2] * Sk.velocity[2]));
                LastState = Now;
            }
        }

        // Face the direction of travel. (The skater's own facing swings sideways with its stance,
        // which would whip anything not posed by the skate animation around.)
        const float Speed2D = std::sqrt(Sk.velocity[0] * Sk.velocity[0] + Sk.velocity[1] * Sk.velocity[1]);
        if (Speed2D > 60.f)
        {
            const float Target = std::atan2(Sk.velocity[1], Sk.velocity[0]) * 57.29578f;
            float Delta = std::fmod(Target - FacingYaw + 540.f, 360.f) - 180.f;
            FacingYaw += Delta * (1.f - std::exp(-Dt * 10.f));
        }
        const FVector NewLocation{ Sk.position[0], Sk.position[1], Sk.position[2] + HalfHeight };
        const FRotator NewRotation{ 0.0, FacingYaw, 0.0 };
        QueryPerformanceCounter(&S2);
        Character->K2_SetActorLocationAndRotation(NewLocation, NewRotation, false, nullptr, true);
        QueryPerformanceCounter(&S3);
        Placed = NewLocation;
        bPlaced = true;
        WriteSkateShare(true, NewLocation.X, NewLocation.Y, NewLocation.Z, FacingYaw, Sk.velocity);
        LARGE_INTEGER W1; QueryPerformanceCounter(&W1);
        Stats.MaxKeys = std::max(Stats.MaxKeys, Ms(T0, K1));
        Stats.MaxDropperOpen = std::max(Stats.MaxDropperOpen, Ms(K1, S0));
        Stats.MaxProps = std::max(Stats.MaxProps, Ms(S1, P1));
        Stats.MaxShare = std::max(Stats.MaxShare, Ms(S3, W1));

        if (UCharacterMovementComponent* Move = Character->CharacterMovement)
        {
            if (Move->MovementMode != EMovementMode::MOVE_Walking)
                Move->SetMovementMode(EMovementMode::MOVE_Walking, 0);
            if (Move->IsComponentTickEnabled())
                Move->SetComponentTickEnabled(false);
            // Fortnite refuses emotes while the character "moves"; the skate engine owns
            // the real velocity (Sk.velocity), which is handed back when leaving skate mode.
            Move->Velocity = FVector{};
        }
        LARGE_INTEGER C0, C1;
        QueryPerformanceCounter(&C0);
        if (DropperScreen::IsOpen())
        {
            DropperEditor::SetViewer(Character);
            DropperScreen::Update(World, Dt);
            FVector CamAt;
            FRotator CamRot;
            float CamFov = 0.f;
            if (DropperScreen::IsOpen() && DropperEditor::View(CamAt, CamRot, CamFov)) SkateCamera::UpdateFixed(PC, CamAt, CamRot);
            else SkateCamera::Update(PC, Sk);
        }
        else SkateCamera::Update(PC, Sk);
        if (DropperScreen::TakeClosed())
        {
            Api.SetPaused(Engine, false);
            Log("[dropper] closed, %zu objects placed", DropperWorld::All().size());
        }
        QueryPerformanceCounter(&C1);
        HideHeldItem(Character);
        EmoteHold::WhileSkating(PC, NewLocation, ClientEmotes, "client");
        QueryPerformanceCounter(&T1);
        Avatar::Update(Character);
        Avatar::TraceTrick(Sk.state, Sk.velocity, Sk.yaw);
        QueryPerformanceCounter(&TA);
        {
            const Hud::Cues C = Hud::TakeCues();
            SkAudio Skater{};
            const bool bSkater = Api.GetAudio && Api.GetAudio(Engine, &Skater);
            Audio::Cues A{ C.Tricks, C.Lines, C.Bails, C.Trick, { C.Spin[0], C.Spin[1], C.Spin[2] }, C.Multiplier,
                           C.Steps, C.Pushes, C.PushSpeed, C.Fe, C.FeCount, bSkater ? &Skater : nullptr };
            Audio::Update(Sk, Dt, A);
        }
        QueryPerformanceCounter(&T2);

        // Stream collision: start a new patch when the skater drifts from the last one's centre.
        const FVector At{ Sk.position[0], Sk.position[1], Sk.position[2] };
        // Session marker return (or any other teleport): the skater lands where the streamed
        // patch has no ground yet and sank in (bail) or fell through. Build the ground at the new
        // spot at once, in one go like entering skate mode, instead of streaming it in.
        {
            const float Tx = Sk.position[0] - Before[0], Ty = Sk.position[1] - Before[1], Tz = Sk.position[2] - Before[2];
            const float Moved = std::sqrt(Tx * Tx + Ty * Ty + Tz * Tz);
            if (Moved > 300.f && Moved > SpeedBefore * Dt * 3.f + 200.f)
            {
                LARGE_INTEGER A, B, F;
                QueryPerformanceFrequency(&F);
                QueryPerformanceCounter(&A);
                Sampler.Begin(At);
                Sampler.Step(Character, 1 << 30);
                SendPatch("teleport");
                QueryPerformanceCounter(&B);
                Log("[marker] skater moved %.0f m in one frame (%s): ground rebuilt there in %.1f ms", Moved / 100.f, Sk.state,
                    double(B.QuadPart - A.QuadPart) * 1000.0 / double(F.QuadPart));
            }
        }
        // Climbing a slope on the ground (45-degree terrain, stairs, ramps) outran the patch: the
        // next one was scanned from far below and its upper part only came right once the skater
        // stood near it for a while. While climbing, patches are taken sooner, from where the
        // skater is heading, and scanned faster. The scan itself is unchanged, and nothing
        // changes going down, in the air, in a bail or on flat ground.
        const bool bClimbing = Sk.velocity[2] > 100.f && std::strstr(Sk.state, "Ground") && !std::strstr(Sk.state, "Wipeout");
        if (!Sampler.Busy() && Sampler.NeedsRecentre(At, bClimbing ? 150.0 : 300.0))
        {
            // Lead the patch a little in the direction of travel (at most 6 m, so the skater stays
            // well inside the 12 m patch even at very high speed).
            double LeadX = Sk.velocity[0] * 0.3, LeadY = Sk.velocity[1] * 0.3;
            const double Lead = std::sqrt(LeadX * LeadX + LeadY * LeadY);
            if (Lead > 600.0) { LeadX *= 600.0 / Lead; LeadY *= 600.0 / Lead; }
            // At most 1 m up: indoors the low trace must still start under the ceiling.
            const double LeadZ = bClimbing ? std::min(double(Sk.velocity[2]) * 0.3, 100.0) : 0.0;
            const FVector Ahead{ At.X + LeadX, At.Y + LeadY, At.Z + LeadZ };
            Sampler.Begin(Ahead, &At);
        }
        // Faster than ~15 m/s the patch is scanned in proportionally fewer frames.
        const float Speed3 = std::sqrt(Sk.velocity[0] * Sk.velocity[0] + Sk.velocity[1] * Sk.velocity[1] + Sk.velocity[2] * Sk.velocity[2]);
        const int Budget = int(float(StreamBudget) * std::clamp(Speed3 / 1500.f, bClimbing ? 2.f : 1.f, 3.f));
        if (Sampler.Busy() && Sampler.Step(Character, Budget))
            SendPatch("stream");
        else if (Sampler.HasPatch() && DropperWorld::Generation() != SentDropperGeneration)
            SendPatch("objects");   // a dropper object was placed, moved or removed
        else if (Sampler.HasPatch() && DropperWorld::PoseGeneration() != SentDropperPose && GetTickCount64() - SentDropperPoseAt > 2000)
            SendPatch("settled props");   // props came to rest elsewhere: their grind rails follow, at most every 2 s
        QueryPerformanceCounter(&T3);

        // How far the skater moved this frame beyond what its speed allows (a hitch or snap).
        const float Dx = Sk.position[0] - Before[0], Dy = Sk.position[1] - Before[1], Dz = Sk.position[2] - Before[2];
        const double Jump = std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz) - double(SpeedBefore) * Dt;
        if (Stats.Frames == 0)
        {
            Stats.FirstTick = Sk.tick;
            Stats.FirstGeneration = Api.CollisionGeneration(Engine);
        }
        Stats.Frames++;
        Stats.MaxDt = std::max(Stats.MaxDt, double(Dt) * 1000.0);
        Stats.MaxStep = std::max(Stats.MaxStep, Ms(T0, T1));
        Stats.MaxAvatar = std::max(Stats.MaxAvatar, Ms(T1, TA));
        Stats.MaxAudio = std::max(Stats.MaxAudio, Ms(TA, T2));
        Stats.MaxSample = std::max(Stats.MaxSample, Ms(T2, T3));
        Stats.MaxEngine = std::max(Stats.MaxEngine, Ms(S0, S1));
        Stats.MaxMove = std::max(Stats.MaxMove, Ms(S2, S3));
        Stats.MaxCam = std::max(Stats.MaxCam, Ms(C0, C1));
        if (Ms(T0, T3) > Stats.MaxTotal)
        {
            PROCESS_MEMORY_COUNTERS Mem1{};
            K32GetProcessMemoryInfo(GetCurrentProcess(), &Mem1, sizeof(Mem1));
            Stats.WorstFaults = Mem1.PageFaultCount - Mem0.PageFaultCount;
        }
        Stats.MaxTotal = std::max(Stats.MaxTotal, Ms(T0, T3));
        Stats.MaxJump = std::max(Stats.MaxJump, Jump);
        Stats.Timer += Dt;
        if (Stats.Timer >= 1.f)
        {
            Log("[perf] %d frames, worst frame %.1f ms | skate update worst %.2f ms (step %.2f [engine %.2f, move %.2f, camera %.2f], pose %.2f, sound %.2f, ground scan %.2f) | %llu physics ticks, %llu collision swaps | worst position jump %.0f cm | moved by something else on %d frames (worst %.0f cm)",
                Stats.Frames, Stats.MaxDt, Stats.MaxTotal, Stats.MaxStep, Stats.MaxEngine, Stats.MaxMove, Stats.MaxCam, Stats.MaxAvatar, Stats.MaxAudio, Stats.MaxSample,
                (unsigned long long)(Sk.tick - Stats.FirstTick), (unsigned long long)(Api.CollisionGeneration(Engine) - Stats.FirstGeneration),
                Stats.MaxJump, Stats.Moved, Stats.MaxMoved);
            Log("[perf] rest of step (worst): keys %.2f ms, dropper/marker %.2f, prop poses/actions %.2f, server hand-off %.2f",
                Stats.MaxKeys, Stats.MaxDropperOpen, Stats.MaxProps, Stats.MaxShare);
            if (Stats.MaxTotal > 50.0)
            {
                // A long stall: was it Windows paging memory back in (RAM full)?
                MEMORYSTATUSEX Ram{ sizeof(Ram) };
                GlobalMemoryStatusEx(&Ram);
                PROCESS_MEMORY_COUNTERS Mem{};
                K32GetProcessMemoryInfo(GetCurrentProcess(), &Mem, sizeof(Mem));
                Log("[perf] stall %.0f ms: %lu page faults in it | system RAM %lu%% used (%.1f of %.1f GB free) | game %.1f GB in RAM",
                    Stats.MaxTotal, Stats.WorstFaults, Ram.dwMemoryLoad, double(Ram.ullAvailPhys) / 1073741824.0, double(Ram.ullTotalPhys) / 1073741824.0,
                    double(Mem.WorkingSetSize) / 1073741824.0);
            }
            Stats = {};
        }

        // Player-built stairs diagnostics ("still choppy"): 10 times a second while their ramps
        // are in the patch, the skater's height, vertical speed and state (300 lines a session).
        {
            static float RideTimer = 0;
            static int RideLines = 0;
            RideTimer += Dt;
            if (Sampler.HasBuildStairs() && RideTimer >= 0.1f && RideLines < 300)
            {
                RideTimer = 0;
                RideLines++;
                Log("[stairs ride] z %.0f vz %.0f speed %.0f %s", Sk.position[2], Sk.velocity[2],
                    std::sqrt(Sk.velocity[0] * Sk.velocity[0] + Sk.velocity[1] * Sk.velocity[1]), Sk.state);
            }
        }

        // Camera diagnostics (bobbing on straight streets): how far the camera's up/down look
        // swung this second, with the engine's camera probe counts.
        static float LookMin = 1e9f, LookMax = -1e9f;
        if (Sk.camera_valid) { LookMin = std::min(LookMin, Sk.camera_forward[2]); LookMax = std::max(LookMax, Sk.camera_forward[2]); }
        StatusTimer += Dt;
        if (StatusTimer >= 1.f)
        {
            StatusTimer = 0;
            uint32_t Cam[4] = {};
            if (Api.GetCameraDebug && Api.GetCameraDebug(Cam) && LookMax >= LookMin)
                Log("[camera] look up/down %.3f..%.3f (swing %.3f) | drop probes: no ground in patch %u, past patch edge %u | positioner hits %u | patch %u cm",
                    LookMin, LookMax, LookMax - LookMin, Cam[0], Cam[1], Cam[2], Cam[3]);
            LookMin = 1e9f; LookMax = -1e9f;
            // What the engine read from the controller this tick: slot, buttons, triggers,
            // sticks, and each slot's status (R ready, - none, E state error, C capabilities error).
            char PadInfo[160] = "";
            int32_t P[12] = {};
            if (Api.GetPadDebug && Api.GetPadDebug(Engine, P))
            {
                const char Code[4] = { 'R', '-', 'E', 'C' };
                snprintf(PadInfo, sizeof(PadInfo), " pad slot %d btn %04X trig %d/%d L(%d,%d) R(%d,%d) slots %c%c%c%c",
                         P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7],
                         Code[P[8] & 3], Code[P[9] & 3], Code[P[10] & 3], Code[P[11] & 3]);
            }
            Log("[skate] %s pos=(%.0f, %.0f, %.0f) speed=%.0f%s%s", Sk.state, Sk.position[0], Sk.position[1], Sk.position[2],
                std::sqrt(Sk.velocity[0] * Sk.velocity[0] + Sk.velocity[1] * Sk.velocity[1]),
                Sk.controller ? " (controller)" : "", PadInfo);
        }
    }
}

bool Client::IsSkating()
{
    return bSkatingNow;
}

bool Client::GetPin(ACharacter** Character, FVector* Location)
{
    if (!bSkatingNow || !Skater || !bPlaced)
        return false;
    *Character = Skater;
    *Location = Placed;
    return true;
}

void Client::DrawHud(UCanvas* Canvas)
{
    KeyBinds::DrawMenu(Canvas);
    if (bShowRails && bSkatingNow && Canvas && Sampler.HasPatch())
    {
        // Grind spots as yellow lines (segments with an end behind the camera are skipped).
        APlayerController* PC = GetLocalController(UWorld::GetWorld());
        if (PC && PC->PlayerCameraManager)
        {
            const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
            const FVector Fwd = UKismetMathLibrary::GetForwardVector(PC->PlayerCameraManager->GetCameraRotation());
            auto InFront = [&](const FVector& P) { return (P.X - Eye.X) * Fwd.X + (P.Y - Eye.Y) * Fwd.Y + (P.Z - Eye.Z) * Fwd.Z > 10.0; };
            const FLinearColor Yellow{ 1.f, 0.85f, 0.f, 1.f };
            for (const auto& Rail : Sampler.Rails())
                for (size_t k = 0; k + 1 < Rail.size(); k++)
                {
                    if (!InFront(Rail[k]) || !InFront(Rail[k + 1])) continue;
                    const FVector A = Canvas->K2_Project(Rail[k]), B = Canvas->K2_Project(Rail[k + 1]);
                    Canvas->K2_DrawLine(FVector2D{ A.X, A.Y }, FVector2D{ B.X, B.Y }, 3.f, Yellow);
                }
        }
    }
    Hud::Draw(Canvas);
}

void Client::Tick(UWorld* World, float Dt)
{
    RawInputGuard::Tick(false);
    Avatar::WrapSurveyStep(8);
    LogHealth(World, Dt, State == EState::Skating);
    Hud::Poll(Dt);
    DropperWorld::PublishToServer();   // the server keeps solid copies of placed objects

    // Start loading the engine as soon as we know this is the client.
    if (EngineStatus == 0)
    {
        EngineStatus = 1;
        CreateThread(nullptr, 0, LoadEngine, nullptr, 0, nullptr);
    }

    ACharacter* Character = GetLocalCharacter(World);

    // Leave skate mode if the character went away (death, vehicle, match end).
    if (State != EState::Off && Character != Skater)
    {
        Skater = nullptr;  // may already be destroyed: don't touch it
        Stop(false, "character changed");
    }

    KeyBinds::Tick();
    const bool J = KeyBinds::Down(KeyBinds::ToggleSkate);
    const bool JPressed = J && !JWasDown;
    JWasDown = J;

    if (JPressed)
    {
        if (State != EState::Off)
            Stop(true, "J pressed");
        else if (!Character)
            Log("J pressed but there is no character to skate with");
        else if (EngineStatus == -1)
            Log("J pressed but the skate engine failed to load (see above)");
        else
        {
            Skater = Character;
            State = EState::WaitingEngine;
            if (EngineStatus != 2)
                Log("Skate engine still loading; skate mode will start when it is ready");
        }
    }

    switch (State)
    {
    case EState::Off:
        Audio::Update(Sk, Dt, Audio::Cues{});  // fades the loops out after skating
        break;

    case EState::WaitingEngine:
        if (EngineStatus == 2)
            BeginEnter(World, Character);
        else if (EngineStatus == -1)
            Stop(false, "engine failed");
        break;

    case EState::WaitingCollision:
        WaitSeconds += Dt;
        if (Api.CollisionGeneration(Engine) > WaitingForGeneration)
            FinishEnter(Character);
        else if (WaitSeconds > 5.f)
            Stop(false, "collision never arrived");
        break;

    case EState::Skating:
        TickSkating(World, Character, Dt);
        break;
    }
}

UMaterialInterface* FindMaterialByName(const char* Name)
{
    for (int i = 0; i < UObject::GObjects->Num(); i++)
    {
        UObject* O = UObject::GObjects->GetByIndex(i);
        if (!O || O->IsDefaultObject() || !O->IsA(UMaterialInterface::StaticClass())) continue;
        if (O->GetName() == Name) return static_cast<UMaterialInterface*>(O);
    }
    return nullptr;
}
