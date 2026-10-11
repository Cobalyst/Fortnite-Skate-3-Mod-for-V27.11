#include "Hud.h"
#include "EngineApi.h"
#include "SkateHud.h"
#include "HomHud.h"
#include "PhoneHud.h"
#include "ActionOverlay.h"
#include "DropperScreen.h"
#include "Xray.h"
#include "Math.h"
#include "SkateCamera.h"
#include <chrono>

#include <cmath>
#include <cwctype>
#include <algorithm>
#include <string>
#include <cstring>
#include <vector>

using namespace SDK;

namespace
{
    std::wstring NoticeText;
    ULONGLONG NoticeUntil = 0;

    std::wstring Widen(const char* S)
    {
        std::wstring W;
        for (; S && *S; S++) W += wchar_t(static_cast<unsigned char>(*S));
        return W;
    }
    std::wstring Upper(std::wstring W)
    {
        for (auto& C : W) C = wchar_t(towupper(C));
        return W;
    }
}


// Skate 3's own trickline HUD (trickdisplay2.apt) played by the APT player (SkateHud.cpp),
// drawn on the game viewport's canvas as translucent textured quads.
namespace
{
    SkateHud::Driver Movie;
    bool bTriedLoad = false;
    bool bSkating = false;
    SkHud State{};
    Hud::Cues PendingCues;
    void QueueFe(const char* Name)
    {
        if (PendingCues.FeCount < 4) PendingCues.Fe[PendingCues.FeCount++] = Name;
    }
    bool bDropperAvailable = false;
    std::vector<UTexture2D*> DropperTextures;   // the dropper movie's textures, then its button glyphs
    std::vector<Apt::DrawCmd> DropperCmds;
    // The screens over the dropper (properties menu, popup): textures per layer, then button glyphs.
    std::vector<UTexture2D*> LayerTextures[DropperScreen::LayerCount];
    std::vector<UTexture2D*> Textures;
    bool bTexturesLoaded = false;
    std::vector<Apt::DrawCmd> Cmds;
    std::vector<Apt::Quad> Quads;

    // Skate 3's Hall of Meat HUD (homscoring.apt), fed the engine's live HoM numbers.
    HomHud::Driver HomMovie;
    bool bHomTried = false;
    std::vector<UTexture2D*> HomTextures;
    bool bHomTexturesLoaded = false;
    std::vector<Apt::DrawCmd> HomCmds;

    void EnsureHomLoaded()
    {
        if (bHomTried) return;
        bHomTried = true;
        HomMovie.SetLog([](const std::string& S) { Log("%s", S.c_str()); });
        HomMovie.PlaySound = [](const std::string& Sound) { QueueFe(Sound == "score_red" ? "hom_score_red" : "hom_transition"); };
        std::string Error;
        if (HomMovie.Load(ModPath("skate-data").c_str(), Error))
            Log("HUD: Skate 3 homscoring loaded (%zu textures)", HomMovie.Player().Textures().size());
        else
            Log("HUD: cannot load homscoring: %s", Error.c_str());
    }

    bool EnsureLoaded()
    {
        if (!bTriedLoad)
        {
            bTriedLoad = true;
            Movie.SetLog([](const std::string& S) { Log("%s", S.c_str()); });
            std::string Error;
            if (Movie.Load(ModPath("skate-data").c_str(), Error))
                Log("HUD: Skate 3 trickdisplay2 loaded (%zu textures)", Movie.Player().Textures().size());
            else
                Log("HUD: cannot load trickdisplay2: %s", Error.c_str());
        }
        return Movie.Loaded();
    }

    void LoadTexturesOf(UObject* WorldContext, const Apt::Player& Player, std::vector<UTexture2D*>& Textures)
    {
        for (auto& T : Player.Textures())
        {
            const std::wstring Path(T.Path.begin(), T.Path.end());
            UTexture2D* Tex = UKismetRenderingLibrary::ImportFileAsTexture2D(WorldContext, FString(Path.c_str()));
            KeepAlive(Tex);  // nothing in the game references it
            Textures.push_back(Tex);
            if (!Tex) Log("HUD: texture %s did not load", T.Path.c_str());
        }
    }

    void LoadTextures(UObject* WorldContext)
    {
        if (!bTexturesLoaded && Movie.Loaded()) { bTexturesLoaded = true; LoadTexturesOf(WorldContext, Movie.Player(), Textures); }
        if (!bHomTexturesLoaded && HomMovie.Loaded()) { bHomTexturesLoaded = true; LoadTexturesOf(WorldContext, HomMovie.Player(), HomTextures); }
    }

    // Hall of Meat x-ray skeleton (Xray.h): Skate 3's dem_bones meshes skinned to the skater.
    Xray::Skeleton Skeleton;
    bool bXrayTried = false;
    UTexture2D* XrayAtlas = nullptr;
    std::vector<int> XrayBone;           // skeleton bone -> engine bone
    std::vector<Mat4> XrayInvBind;       // per skeleton bone
    std::vector<float> EngineBones, XraySkin;
    std::vector<Xray::Tri> XrayTris;
    std::vector<FCanvasUVTri> XrayCanvasTris;
    bool bXrayShown = false;             // retail HOMSkaterPresEntity +0x35: first reveal of this bail
    bool bXrayEnabled = true;            // mod_settings xray
    // Hall of Meat screen effects, as Skate 3 layers them:
    //  - the HoM colour grade (post_fx colour_matrix_hall_of_meat) for the whole bail, while the
    //    HoM HUD is up;
    //  - at the major injury (the skeleton's first reveal, HOMSkaterPresEntity 0x82763C44) the
    //    bail effect (state 0x1C) for 40 frames: hom vignette at full strength (postfx_uberPS
    //    f_Hom_Params.w), which then fades by 0.02 per frame (0x827D20B0), frames at 30 fps.
    float HomFx = 0.f;           // vignette strength
    float HomMoment = 0.f;       // seconds left of the major-injury effect
    bool bHomMomentDone = false; // this bail's major injury has happened
    UTexture2D* VignetteTex = nullptr;
    bool bVignetteTried = false;
    std::chrono::steady_clock::time_point LastDraw{};

    // Front-end sound per HoM bone when the skeleton first shows (retail FUN_82763CB0).
    const char* const RevealSound[25] = {
        "hom_neck", "hom_neck", "hom_lefthand", "hom_leftforearm", "hom_leftarm", "hom_leftshoulder",
        "hom_righthand", "hom_rightforearm", "hom_rightarm", "hom_rightshoulder", "hom_upperbody",
        "hom_upperbody", "hom_upperbody", "hom_upperbody", "hom_lefttoe", "hom_leftfoot", "hom_leftcalf",
        "hom_leftupleg", "hom_righttoe", "hom_rightfoot", "hom_rightcalf", "hom_rightupleg", "hom_hipsleft",
        "hom_skull", nullptr /* hom_groin: no sound */,
    };

    bool EnsureXray(UObject* WorldContext)
    {
        if (bXrayTried) return XrayAtlas && Skeleton.Loaded() && !XrayBone.empty();
        bXrayTried = true;
        const std::string Dir = ModPath("skate-data\\hud\\xray\\");
        std::string Error;
        if (!Skeleton.Load(Dir + "skeleton.bin", Error)) { Log("x-ray: %s", Error.c_str()); return false; }
        XrayAtlas = UKismetRenderingLibrary::ImportFileAsTexture2D(WorldContext, FString(ModPathW("skate-data\\hud\\xray\\bones_atlas.png").c_str()));
        if (!XrayAtlas) { Log("x-ray: bones_atlas.png did not load"); return false; }
        KeepAlive(XrayAtlas);
        const uint32_t N = Api.BoneCount(Engine);
        for (const std::string& Name : Skeleton.BoneNames())
        {
            int Index = -1;
            for (uint32_t i = 0; i < N; i++)
                if (Name == Api.BoneName(Engine, i)) Index = int(i);
            float M[16];
            if (Index < 0 || !Api.BoneBind(Engine, uint32_t(Index), M))
            {
                Log("x-ray: skater rig has no bone %s", Name.c_str());
                XrayBone.clear();
                return false;
            }
            XrayBone.push_back(Index);
            XrayInvBind.push_back(Mat4::FromArray(M).RigidInverse());
        }
        EngineBones.resize(size_t(N) * 16);
        XraySkin.resize(XrayBone.size() * 16);
        Log("x-ray: Skate 3 skeleton loaded (%zu bones)", XrayBone.size());
        return true;
    }

    void DrawXray(UCanvas* Canvas)
    {
        if (!bXrayEnabled || !HomMovie.Visible() || !bSkating || !Engine) { bXrayShown = false; return; }
        if (!EnsureXray(Canvas)) return;
        if (!Skeleton.SetLevels(State.hom_levels)) return;
        if (!bXrayShown)
        {
            // The retail reveal plays the sound of the last showing bone in HoM bone order.
            bXrayShown = true;
            for (int b = 24; b >= 0; b--)
                if (State.hom_levels[b] > 1) { if (RevealSound[b]) QueueFe(RevealSound[b]); break; }
        }

        APlayerController* PC = GetLocalController(UWorld::GetWorld());
        if (!PC || !PC->PlayerCameraManager) return;
        const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
        const FRotator Rot = PC->PlayerCameraManager->GetCameraRotation();
        const FVector Fwd = UKismetMathLibrary::GetForwardVector(Rot);
        const FVector Right = UKismetMathLibrary::GetRightVector(Rot);
        const FVector Up = UKismetMathLibrary::GetUpVector(Rot);
        // Calibrate the projection against the canvas's own (FOV, aspect and DPI as the game uses).
        auto Add = [](const FVector& A, const FVector& B, double S) { return FVector{ A.X + B.X * S, A.Y + B.Y * S, A.Z + B.Z * S }; };
        const FVector Centre = Canvas->K2_Project(Add(Eye, Fwd, 100.0));
        const FVector Rx = Canvas->K2_Project(Add(Add(Eye, Fwd, 100.0), Right, 10.0));
        const FVector Uy = Canvas->K2_Project(Add(Add(Eye, Fwd, 100.0), Up, 10.0));
        Xray::Camera Cam{};
        Cam.Pos[0] = float(Eye.X); Cam.Pos[1] = float(Eye.Y); Cam.Pos[2] = float(Eye.Z);
        Cam.Fwd[0] = float(Fwd.X); Cam.Fwd[1] = float(Fwd.Y); Cam.Fwd[2] = float(Fwd.Z);
        Cam.Right[0] = float(Right.X); Cam.Right[1] = float(Right.Y); Cam.Right[2] = float(Right.Z);
        Cam.Up[0] = float(Up.X); Cam.Up[1] = float(Up.Y); Cam.Up[2] = float(Up.Z);
        Cam.Cx = float(Centre.X); Cam.Cy = float(Centre.Y);
        Cam.Fx = float(Rx.X - Centre.X) * 10.f;   // pixels per unit at depth 1
        Cam.Fy = float(Centre.Y - Uy.Y) * 10.f;
        if (!(Cam.Fx > 1.f) || !(Cam.Fy > 1.f)) return;

        Api.GetBones(Engine, EngineBones.data(), uint32_t(EngineBones.size() / 16));
        for (size_t i = 0; i < XrayBone.size(); i++)
        {
            const Mat4 S = Mat4::FromArray(&EngineBones[size_t(XrayBone[i]) * 16]) * XrayInvBind[i];
            memcpy(&XraySkin[i * 16], S.M, sizeof(float) * 16);
        }
        Skeleton.Build(XraySkin.data(), Cam, XrayTris);
        if (XrayTris.empty()) return;
        XrayCanvasTris.resize(XrayTris.size());
        const FLinearColor White{ 1.f, 1.f, 1.f, 1.f };
        for (size_t t = 0; t < XrayTris.size(); t++)
        {
            const Xray::Tri& T = XrayTris[t];
            FCanvasUVTri& C = XrayCanvasTris[t];
            C.V0_Pos = FVector2D{ T.X[0], T.Y[0] }; C.V0_UV = FVector2D{ T.U[0], T.V[0] }; C.V0_Color = White;
            C.V1_Pos = FVector2D{ T.X[1], T.Y[1] }; C.V1_UV = FVector2D{ T.U[1], T.V[1] }; C.V1_Color = White;
            C.V2_Pos = FVector2D{ T.X[2], T.Y[2] }; C.V2_UV = FVector2D{ T.U[2], T.V[2] }; C.V2_Color = White;
        }
        Canvas->K2_DrawTriangle(XrayAtlas, TArray<FCanvasUVTri>(XrayCanvasTris.data(), int32(XrayCanvasTris.size()), int32(XrayCanvasTris.size())));
    }

    void DrawHomEffect(UCanvas* Canvas)
    {
        const auto Now = std::chrono::steady_clock::now();
        float Dt = LastDraw.time_since_epoch().count() ? std::chrono::duration<float>(Now - LastDraw).count() : 0.f;
        LastDraw = Now;
        if (Dt > 0.25f) Dt = 0.25f;
        const bool bOn = bSkating && HomMovie.Loaded() && HomMovie.Visible();
        if (!bOn) bHomMomentDone = false;
        else if (!bHomMomentDone)
        {
            // The first bone shown this bail (a sprain or worse) is the major injury.
            for (int b = 0; b < 25; b++)
                if (State.hom_levels[b] > 1) { bHomMomentDone = true; HomMoment = 40.f / 30.f; break; }
        }
        if (HomMoment > 0.f) { HomMoment -= Dt; HomFx = 1.f; }
        else HomFx = std::max(0.f, HomFx - 0.02f * 30.f * Dt);
        if (!bOn) HomFx = 0.f;  // the HUD, skeleton and effects all go when the bail ends
        SkateCamera::SetHomGrade(bOn ? 1.f : 0.f);
        if (HomFx <= 0.f) return;
        if (!bVignetteTried)
        {
            bVignetteTried = true;
            VignetteTex = UKismetRenderingLibrary::ImportFileAsTexture2D(Canvas, FString(ModPathW("skate-data\\hud\\xray\\hom_vignette.png").c_str()));
            if (VignetteTex) KeepAlive(VignetteTex); else Log("HUD: hom_vignette.png did not load");
        }
        if (!VignetteTex) return;
        // 4 x 4 tiles = 16 strengths (tools/hom_vignette.py); the whole screen is multiplied by it.
        const int Tile = std::clamp(int(std::lround(HomFx * 15.f)), 0, 15);
        const float U = float(Tile % 4) * 0.25f + 0.5f / 1024.f, V = float(Tile / 4) * 0.25f + 0.5f / 1024.f;
        const float S = 0.25f - 1.f / 1024.f;
        Canvas->K2_DrawTexture(VignetteTex, FVector2D{ 0, 0 }, FVector2D{ float(Canvas->SizeX), float(Canvas->SizeY) },
            FVector2D{ U, V }, FVector2D{ S, S }, FLinearColor{ 1, 1, 1, 1 }, EBlendMode::BLEND_Modulate, 0.f, FVector2D{ 0.5, 0.5 });
    }

    // The canvas multiplies texture by vertex colour in linear space; Skate 3's colours are sRGB.
    float Linear(float C) { return C <= 0.f ? 0.f : std::pow(C > 1.f ? 1.f : C, 2.2f); }
}

// Session marker HUD as Skate 3 shows it: while LB is held, the quick menu movie (hudphonelist:
// D-pad strip with Go To Marker / Place Marker / Object Dropper, unavailable options at 40%); while
// the go-to is held, TV static fades in over the screen with the hold progress until the skater is
// moved (reference/skate3_gameplay.mp4 0:48.6). Skate 3 draws its static with the post effect
// postfx_basictex_fisheye_noisePS from a noise texture made at run time; marker_static.png is a
// random monochrome noise texture made the same way, scrolled each frame (f_NoiseScroll).
namespace
{
    PhoneHud::Driver PhoneMovie;
    bool bPhoneTried = false;
    std::vector<UTexture2D*> PhoneTextures;
    bool bPhoneTexturesLoaded = false;
    std::vector<Apt::DrawCmd> PhoneCmds;
    UTexture2D* StaticTex = nullptr;
    bool bStaticTried = false;

    // Skate 3's action overlay in its DMO grab state (ActionOverlay.h).
    ActionOverlay::Driver OverlayMovie;
    bool bOverlayTried = false;
    std::vector<UTexture2D*> OverlayTextures;
    bool bOverlayTexturesLoaded = false;
    std::vector<Apt::DrawCmd> OverlayCmds;
    ActionOverlay::Input OverlayInput;

    void EnsureOverlayLoaded()
    {
        if (bOverlayTried) return;
        bOverlayTried = true;
        OverlayMovie.SetLog([](const std::string& S) { Log("%s", S.c_str()); });
        std::string Error;
        if (OverlayMovie.Load(ModPath("skate-data").c_str(), Error))
            Log("HUD: Skate 3 actionoverlay2 loaded (%zu textures)", OverlayMovie.Player().Textures().size());
        else
            Log("HUD: cannot load actionoverlay2: %s", Error.c_str());
    }

    void EnsurePhoneLoaded()
    {
        if (bPhoneTried) return;
        bPhoneTried = true;
        PhoneMovie.SetLog([](const std::string& S) { Log("%s", S.c_str()); });
        std::string Error;
        if (PhoneMovie.Load(ModPath("skate-data").c_str(), Error))
            Log("HUD: Skate 3 hudphonelist loaded (%zu textures)", PhoneMovie.Player().Textures().size());
        else
            Log("HUD: cannot load hudphonelist: %s", Error.c_str());
    }
}

void Hud::Start()
{
    bSkating = true;
    if (EnsureLoaded()) Movie.Start();
}

void Hud::Stop()
{
    bSkating = false;
    if (Movie.Loaded()) Movie.Stop();
    HomMovie.Hide();
    PhoneMovie.Hide();
    OverlayMovie.Hide();
    OverlayInput = {};
}

Hud::Cues Hud::TakeCues()
{
    Cues C = PendingCues;
    PendingCues = {};
    return C;
}

void Hud::Poll(float Dt)
{
    if (!Engine || !Api.PopHudEvent || !EnsureLoaded())
        return;
    if (bSkating)
        Api.GetHud(Engine, &State);

    SkHudEvent Ev;
    while (Api.PopHudEvent(Engine, &Ev))
    {
        if (Ev.kind == 5) { PendingCues.Steps++; continue; }
        if (Ev.kind == 7) continue;  // Hall of Meat injury "Bone|Level" (Skate 3 plays no sound for it)
        if (Ev.kind == 8) continue;  // Hall of Meat result lines (the HUD reads the live numbers)
        if (Ev.kind == 9) { Log("[hom] bail scored: total %d", int(Ev.value)); continue; }
        if (Ev.kind == 10)  // body contact: text = region 0-5, value = contact force
        {
            const int Region = Ev.text[0] - '0';
            if (Region >= 0 && Region < 6 && Ev.value > PendingCues.BodyForce[Region]) PendingCues.BodyForce[Region] = Ev.value;
            continue;
        }
        if (Ev.kind == 6)
        {
            if (Ev.text[0] == 'p') { PendingCues.Pushes++; PendingCues.PushSpeed = Ev.value; }
            continue;
        }
        Log("[hud] event %d '%s' %.0f", Ev.kind, Ev.text, Ev.value);
        switch (Ev.kind)
        {
        case SK_HUD_TRICK: PendingCues.Tricks++; strncpy_s(PendingCues.Trick, Ev.text, _TRUNCATE); break;
        case SK_HUD_BAIL: PendingCues.Bails++; break;
        case SK_HUD_LINE: if (Ev.value >= 1.f) PendingCues.Lines++; break;
        }
        if (bSkating) Movie.Event(Ev.kind, Ev.text);
    }

    if (bSkating)
    {
        for (int i = 0; i < 3; i++) PendingCues.Spin[i] = State.board_spin[i];
        PendingCues.Multiplier = State.multiplier < 1.f ? 1.f : State.multiplier;
    }

    SkateHud::Input In;
    if (bSkating)
    {
        In.Sequence = State.sequence;
        In.SequenceActive = State.sequence_active != 0;
        In.Clean = State.clean != 0;
        In.Sketchy = State.sketchy != 0;
        In.LineSeconds = State.line_seconds;
        In.LineScore = State.line;
        In.Multiplier = State.multiplier < 1.f ? 1.f : State.multiplier;
        In.Switch = State.switch_stance != 0;
        In.Fakie = State.fakie != 0;
    }
    Movie.Update(In, Dt);

    EnsureHomLoaded();
    HomHud::Input Hom;
    if (bSkating)
    {
        Hom.Active = State.hom_active != 0;
        Hom.Shown = State.hom_shown != 0;
        Hom.Score = State.hom_score;
        for (int i = 0; i < 5; i++) { Hom.Metrics[i] = State.hom_metrics[i]; Hom.Points[i] = State.hom_points[i]; }
    }
    HomMovie.Update(Hom, Dt);

    EnsurePhoneLoaded();
    PhoneHud::Input Phone;
    if (bSkating)
    {
        Phone.Open = State.marker_visible != 0 && !DropperScreen::IsOpen();
        Phone.CanGoTo = State.marker_can_return != 0;
        Phone.CanPlace = State.marker_can_place != 0;
        Phone.CanDrop = bDropperAvailable;
    }
    PhoneMovie.Update(Phone, Dt);

    EnsureOverlayLoaded();
    ActionOverlay::Input Overlay;
    if (bSkating && !DropperScreen::IsOpen() && State.marker_visible == 0) Overlay = OverlayInput;
    OverlayMovie.Update(Overlay, Dt);
}

void Hud::MarkerCounts(uint32_t& Placed, uint32_t& Returned, bool& bSet)
{
    Placed = State.marker_placed;
    Returned = State.marker_returned;
    bSet = State.marker_set != 0;
}

void Hud::SetActionOverlay(bool bShow, const bool Params[5])
{
    OverlayInput.Show = bShow;
    for (int i = 0; i < 5; i++) OverlayInput.Params[i] = bShow && Params[i];
}

void Hud::QueueFeSound(const char* Name) { QueueFe(Name); }
bool Hud::MarkerMenuOpen() { return bSkating && State.marker_visible != 0; }
void Hud::SetDropperAvailable(bool bAvailable) { bDropperAvailable = bAvailable; }

void Hud::Notice(const wchar_t* Text, float Seconds)
{
    NoticeText = Text ? Text : L"";
    NoticeUntil = GetTickCount64() + ULONGLONG(Seconds * 1000.f);
}

static void DrawNotice(UCanvas* Canvas)
{
    if (NoticeText.empty() || GetTickCount64() > NoticeUntil)
        return;
    UEngine* Eng = UEngine::GetEngine();
    UFont* Font = Eng ? (Eng->LargeFont ? Eng->LargeFont : Eng->MediumFont) : nullptr;
    if (!Font)
        return;
    const float W = float(Canvas->SizeX), H = float(Canvas->SizeY);
    const float Scale = std::fmax(1.f, H / 720.f);
    Canvas->K2_DrawText(Font, FString(NoticeText.c_str()), FVector2D{ W * 0.5f, H * 0.3f }, FVector2D{ Scale, Scale },
        FLinearColor{ 1.f, 1.f, 1.f, 1.f }, 0.f, FLinearColor{ 0.f, 0.f, 0.f, 1.f }, FVector2D{ 2.f, 2.f }, true, true, true,
        FLinearColor{ 0.f, 0.f, 0.f, 1.f });
}

static void DrawMovie(UCanvas* Canvas, const Apt::Player& Player, const std::vector<UTexture2D*>& Textures,
                      const std::vector<Apt::DrawCmd>& Cmds)
{
    // Stage (1280x720) to screen, fitted to the height and centred like Skate 3's 16:9 frame.
    const float W = float(Canvas->SizeX), H = float(Canvas->SizeY);
    const float S = H / Player.StageHeight();
    const float X0 = (W - Player.StageWidth() * S) * 0.5f;

    for (auto& C : Cmds)
    {
        // The canvas has no additive colour term; tex*mul + add is exact for white texels
        // (glyphs, glows) when folded into the multiplier.
        FLinearColor Col;
        Col.R = Linear(C.Mul[0] + C.Add[0]);
        Col.G = Linear(C.Mul[1] + C.Add[1]);
        Col.B = Linear(C.Mul[2] + C.Add[2]);
        Col.A = std::fmin(1.f, C.Mul[3] + C.Add[3]);
        if (Col.A <= 0.004f) continue;
        UTexture2D* Tex = C.Texture >= 0 && C.Texture < int(Textures.size()) ? Textures[size_t(C.Texture)] : nullptr;
        if (C.Texture >= 0 && !Tex) continue;

        // K2_DrawTexture blends translucently (K2_DrawTriangle draws opaque boxes).
        Quads.clear();
        Apt::ToQuads(C, Quads);
        for (const Apt::Quad& Q : Quads)
            Canvas->K2_DrawTexture(Tex, FVector2D{ X0 + Q.x * S, Q.y * S }, FVector2D{ Q.w * S, Q.h * S },
                FVector2D{ Q.u, Q.v }, FVector2D{ Q.du, Q.dv }, Col, EBlendMode::BLEND_Translucent,
                Q.Rotation, FVector2D{ 0.5, 0.5 });
    }
}

static void DrawMarkerStatic(UCanvas* Canvas)
{
    const float P = bSkating ? std::fmin(1.f, State.marker_progress) : 0.f;
    if (P <= 0.f)
        return;
    if (!bStaticTried)
    {
        bStaticTried = true;
        StaticTex = UKismetRenderingLibrary::ImportFileAsTexture2D(Canvas, FString(ModPathW("skate-data\\hud\\marker_static.png").c_str()));
        KeepAlive(StaticTex);
        if (!StaticTex) Log("HUD: marker_static.png did not load");
    }
    if (!StaticTex)
        return;
    const float W = float(Canvas->SizeX), H = float(Canvas->SizeY);
    // About two screen pixels per noise texel at 720p, a new random offset every frame.
    const float Texel = 2.f * std::fmax(1.f, H / 720.f);
    const float U = float(rand() % 256) / 256.f, V = float(rand() % 256) / 256.f;
    Canvas->K2_DrawTexture(StaticTex, FVector2D{ 0, 0 }, FVector2D{ W, H }, FVector2D{ U, V },
        FVector2D{ W / (256.f * Texel), H / (256.f * Texel) }, FLinearColor{ 1, 1, 1, P }, EBlendMode::BLEND_Translucent,
        0.f, FVector2D{ 0.5, 0.5 });
}

void Hud::Draw(UCanvas* Canvas)
{
    if (!Canvas)
        return;
    DrawNotice(Canvas);
    DrawMarkerStatic(Canvas);
    LoadTextures(Canvas);
    if (!bPhoneTexturesLoaded && PhoneMovie.Loaded()) { bPhoneTexturesLoaded = true; LoadTexturesOf(Canvas, PhoneMovie.Player(), PhoneTextures); }
    // Button icons follow the movie's own textures (PhoneHud::ButtonIcons), added as they are first used.
    while (bPhoneTexturesLoaded && PhoneTextures.size() < PhoneMovie.Player().Textures().size() + PhoneMovie.ButtonIcons().size())
    {
        const std::string& P = PhoneMovie.ButtonIcons()[PhoneTextures.size() - PhoneMovie.Player().Textures().size()];
        UTexture2D* Tex = UKismetRenderingLibrary::ImportFileAsTexture2D(Canvas, FString(std::wstring(P.begin(), P.end()).c_str()));
        KeepAlive(Tex);
        if (!Tex) Log("HUD: button icon %s did not load", P.c_str());
        PhoneTextures.push_back(Tex);
    }
    if (PhoneMovie.Loaded())
    {
        PhoneMovie.Render(PhoneCmds);
        if (!PhoneCmds.empty()) DrawMovie(Canvas, PhoneMovie.Player(), PhoneTextures, PhoneCmds);
    }
    if (!bOverlayTexturesLoaded && OverlayMovie.Loaded()) { bOverlayTexturesLoaded = true; LoadTexturesOf(Canvas, OverlayMovie.Player(), OverlayTextures); }
    while (bOverlayTexturesLoaded && OverlayTextures.size() < OverlayMovie.Player().Textures().size() + OverlayMovie.ButtonIcons().size())
    {
        const std::string& P = OverlayMovie.ButtonIcons()[OverlayTextures.size() - OverlayMovie.Player().Textures().size()];
        UTexture2D* Tex = UKismetRenderingLibrary::ImportFileAsTexture2D(Canvas, FString(std::wstring(P.begin(), P.end()).c_str()));
        KeepAlive(Tex);
        if (!Tex) Log("HUD: overlay button icon %s did not load", P.c_str());
        OverlayTextures.push_back(Tex);
    }
    if (OverlayMovie.Loaded() && !DropperScreen::Player())
    {
        OverlayMovie.Render(OverlayCmds);
        if (!OverlayCmds.empty()) DrawMovie(Canvas, OverlayMovie.Player(), OverlayTextures, OverlayCmds);
    }
    // Skate 3's object dropper screen replaces the skating HUD while it is open.
    if (const Apt::Player* Dp = DropperScreen::Player())
    {
        // Same texture order every time the movie loads; button glyphs are added as first used.
        if (DropperTextures.empty()) LoadTexturesOf(Canvas, *Dp, DropperTextures);
        const auto& Icons = DropperScreen::ButtonIcons();
        while (DropperTextures.size() >= Dp->Textures().size() && DropperTextures.size() < Dp->Textures().size() + Icons.size())
        {
            const std::string& P = Icons[DropperTextures.size() - Dp->Textures().size()];
            UTexture2D* Tex = UKismetRenderingLibrary::ImportFileAsTexture2D(Canvas, FString(std::wstring(P.begin(), P.end()).c_str()));
            KeepAlive(Tex);
            if (!Tex) Log("HUD: dropper button icon %s did not load", P.c_str());
            DropperTextures.push_back(Tex);
        }
        DropperScreen::Render(DropperCmds);
        if (!DropperCmds.empty()) DrawMovie(Canvas, *Dp, DropperTextures, DropperCmds);
        for (int L = 1; L < DropperScreen::LayerCount; L++)
        {
            const Apt::Player* Lp = DropperScreen::LayerPlayer(L);
            if (!Lp) continue;
            std::vector<UTexture2D*>& Tx = LayerTextures[L];
            if (Tx.empty()) LoadTexturesOf(Canvas, *Lp, Tx);
            while (Tx.size() >= Lp->Textures().size() && Tx.size() < Lp->Textures().size() + Icons.size())
            {
                const std::string& P = Icons[Tx.size() - Lp->Textures().size()];
                UTexture2D* Tex = UKismetRenderingLibrary::ImportFileAsTexture2D(Canvas, FString(std::wstring(P.begin(), P.end()).c_str()));
                KeepAlive(Tex);
                if (!Tex) Log("HUD: dropper button icon %s did not load", P.c_str());
                Tx.push_back(Tex);
            }
            DropperScreen::RenderLayer(L, DropperCmds);
            if (!DropperCmds.empty()) DrawMovie(Canvas, *Lp, Tx, DropperCmds);
        }
        return;
    }
    DrawXray(Canvas);
    DrawHomEffect(Canvas);
    if (HomMovie.Loaded() && HomMovie.Visible())
    {
        HomMovie.Render(HomCmds);
        DrawMovie(Canvas, HomMovie.Player(), HomTextures, HomCmds);
        return;  // Skate 3 shows the Hall of Meat HUD instead of the trickline HUD
    }
    if (!Movie.Loaded())
        return;
    Movie.Render(Cmds);
    if (Cmds.empty())
        return;
    DrawMovie(Canvas, Movie.Player(), Textures, Cmds);
}

void Hud::SetXray(bool bOn)
{
    bXrayEnabled = bOn;
}
