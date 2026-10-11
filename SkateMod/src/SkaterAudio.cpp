#include "SkaterAudio.h"
#include "Common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Ported from Skate 3 TU3 (default.xex); retail addresses next to each piece.
namespace
{
    const std::string MaterialsPath = ModPath("skate-data\\sounds\\aud_materials.txt");
    const std::string TricksPath = ModPath("skate-data\\sounds\\trick_audio.txt");
    const std::string GrindPath = ModPath("skate-data\\sounds\\grind_surfaces.txt");
    constexpr int None = 0x8f;  // "no material"

    // ---- audio material table (0x82FD1930 + aud_material records) ----
    struct Material
    {
        int Kind = -1;               // 0 Skate_Collisions, 1 Skate_Metal, 2 HOM_Set_1, -1 none
        std::string Name;
        int Words[19] = {};          // layout words 0x30..0x78
        uint8_t Bytes[4] = {};       // 0x38, 0x39, 0x7c, 0x7d
        float Thr[4] = {};           // heavy, top, medium, min (class 7DAFF70B3A91CD5D)
        float Rng[15] = {};          // intensity ranges (class 13E20D398E385A56), layout order
        bool bAltThr = false, bAltRng = false;
        float AltThr[4] = {}, AltRng[15] = {};
        int Hom[7] = {};             // HOM_Set_1 slots (kind 2)
        int AltPitch = 4096;
        int W(int Off) const { return Words[(Off - 0x30) / 4]; }
    };
    std::vector<Material> Mats(None);
    std::vector<std::array<int, 18>> SurfaceMap;  // Sk8::AudioSurfaceMap, 0x5f entries

    struct Trick { int First = -1, Second = -1, Flag = 0, Tier = 0; };
    std::vector<Trick> Tricks;

    struct GrindSurfaceTuning { float Gain[4] = { 1, 1, 1, 1 }, Scale[4] = { 1, 1, 1, 1 }; };
    GrindSurfaceTuning GrindTuning[14];
    float GrindSpeedDiv = 45.f;

    int GroundMaterial = 2;  // concrete_polished: Fortnite reports no Skate 3 surface tags

    bool Valid(int M) { return M >= 0 && M < None; }

    // 0x82485750
    int SurfaceClass(int M)
    {
        switch (M - 0x5f)
        {
        case 0: case 2: case 4: case 5: case 6: case 7: case 9: case 10: case 0xb: case 0x12: return 1;
        case 1: return 2;
        case 3: case 8: case 0xc: case 0xd: case 0xe: return 0;
        }
        const int E = (M >= 0 && M < 0x5e) ? M : 0x5e;
        return E < int(SurfaceMap.size()) ? SurfaceMap[size_t(E)][7] : 0;
    }

    // 0x82482BB8
    int LandingClass(int M)
    {
        const int E = (M >= 0 && M < 0x5e) ? M : 0x5e;
        return E < int(SurfaceMap.size()) ? SurfaceMap[size_t(E)][2] : 0;
    }

    // 0x82484638
    int SlotPick(int M, int Surface, int Impact)
    {
        const Material& T = Mats[size_t(M)];
        if (T.Kind == 1)
        {
            if (Impact == 2) return T.W(0x5c);
            if (Impact == 0) return Surface == 0 ? T.W(0x50) : Surface == 1 ? T.W(0x58) : Surface == 2 ? T.W(0x64) : 0;
            return Surface == 0 ? T.W(0x4c) : Surface == 1 ? T.W(0x54) : Surface == 2 ? T.W(0x60) : 0;
        }
        if (T.Kind == 2)
        {
            if (Impact == 2) return T.Hom[0];
            if (Impact == 0) return Surface == 0 ? T.Hom[1] : Surface == 1 ? T.Hom[2] : Surface == 2 ? T.Hom[3] : 0;
            return Surface == 0 ? T.Hom[4] : Surface == 1 ? T.Hom[5] : Surface == 2 ? T.Hom[6] : 0;
        }
        if (Impact == 2) return T.W(0x70);
        if (Impact == 0) return Surface == 0 ? T.W(0x40) : Surface == 1 ? T.W(0x6c) : Surface == 2 ? T.W(0x78) : 0;
        return Surface == 0 ? T.W(0x3c) : Surface == 1 ? T.W(0x68) : Surface == 2 ? T.W(0x74) : 0;
    }

    // 0x82484EC8: force class (0 light, 1 medium, 2 heavy, 3 none) and its force range.
    int ForceClass(float Force, int M, float& Lo, float& Hi, bool bAlt = false)
    {
        if (!Valid(M)) return 3;
        const Material& T = Mats[size_t(M)];
        const float* R = (bAlt && T.Bytes[1] && T.bAltThr) ? T.AltThr : T.Thr;
        const float Heavy = R[0], Top = R[1], Medium = R[2], Min = R[3];
        if (Force < Min) return 3;
        Lo = Min; Hi = Medium;
        if (Force <= Heavy)
        {
            if (Force > Medium) { Lo = Medium; Hi = Heavy; return 1; }
            return 0;
        }
        Lo = Heavy; Hi = Top;
        return 2;
    }

    // 0x82484A98: intensity 0..32767 across the class's force range.
    int Intensity(float Lo, float Hi, float Force, int M, int Other, int Class, bool bAlt = false)
    {
        if (M < 0 || Class == 3 || !Valid(M)) return 0;
        const Material& T = Mats[size_t(M)];
        const float* R = (bAlt && T.Bytes[1] && T.bAltRng) ? T.AltRng : T.Rng;
        if (Hi <= Force) Force = Hi;
        const int Surface = (M < 0x61 && Other != None) ? SurfaceClass(Other) : 0;
        int A = 0, B = 0x7fff;
        if (Class == 0)
        {
            if (Surface == 0) { A = int(R[2]); B = int(R[3]); }
            else if (Surface == 1) { A = int(R[6]); B = int(R[7]); }
            else { A = int(R[13]); B = int(R[14]); }
        }
        else if (Class == 1)
        {
            if (Surface == 0) { A = int(R[0]); B = int(R[1]); }
            else if (Surface == 1) { A = int(R[4]); B = int(R[5]); }
            else { A = int(R[11]); B = int(R[12]); }
        }
        else if (Class < 3) { A = int(R[8]); B = int(R[9]); }
        if (Lo < Hi) return int(float(B - A) / (Hi - Lo) * (Force - Lo) + float(A));
        return B;
    }

    // 0x82484D90: ranges record float @0x28 (1.0 for no material).
    float RangeScale(int M) { return Valid(M) ? Mats[size_t(M)].Rng[10] : 1.f; }

    // 0x82474D30 message -> 0x824BFE08 (82484410 per side) -> 0x824C01B8 voice parameters. The
    // local skater's emitter values (category gain, pitch) are neutral: 32767 and 4096.
    void Collide(const SkaterAudio::Backend& B, int MatA, int MatB, int ClassA, int ClassB, int IntA, int IntB, bool bFlag)
    {
        for (int Side = 0; Side < 2; Side++)
        {
            const int M = Side ? MatB : MatA, Other = Side ? MatA : MatB, Class = Side ? ClassB : ClassA;
            if (M == None || Class == 3 || !Valid(M)) continue;
            const Material& T = Mats[size_t(M)];
            if (T.Kind == -1) continue;
            const int Surface = Other != None ? SurfaceClass(Other) : 0;
            const int Slot = SlotPick(M, Surface, Class);
            if (Slot == -1) continue;
            const int Vol = std::clamp(T.W(0x34), 0, 0x7fff);
            const int Pitch = (bFlag && T.Bytes[0]) ? T.AltPitch : T.W(0x44);
            const int Inten = Side ? IntB : IntA;
            const int V0 = int(float(Inten) * (1.f / 32767.f) * 32767.f);
            const int V1 = int(float(V0) * (float(Vol) * (1.f / 32767.f)));
            const int P1 = int(4096.f * (float(Pitch) * (1.f / 4096.f)));
            if (B.PlaySplc) B.PlaySplc(T.Kind, Slot, float(V1) * (1.f / 32767.f), float(P1) * (1.f / 4096.f));
        }
    }

    // ---- board audio settings (C26949FCB638A2CA "default", AttribSys slot 0x18) ----
    constexpr float LandScale = 0.4f;          // 6D68BC2D1A23C29A
    constexpr int LandOther = 0x5f;            // 85FDC8BF696BCA5C: board
    constexpr float LandMedium = 0.1f;         // 3462CBB16DCA696E
    constexpr float LandTop = 0.3f;            // 590495E420B399E5
    constexpr float LandGainA = 0.65f;         // 31DEEF8FA219950F
    constexpr float LandGainB = 1.0f;          // 0EC6EEF5366FEA85
    constexpr float LandLayerSplit = 0.75f;    // 224B06562D9D5E0E
    constexpr int LandLayer[4] = { 860, 861, 862, 863 };  // F262, 3A2F, 1E86, BF22
    constexpr int LandLocal = 1095;            // 633FA94E39C1AE8F
    constexpr float PopHeavy = 0.42f;          // F2A1E273ABB8E9AB
    constexpr float PopMedium = 0.25f;         // D7758385CDB8DC26
    constexpr int PopA[3] = { 1097, 1098, 1099 };  // 3C1E3B965A93594A
    constexpr int PopB[3] = { 1103, 1104, 1105 };  // 84DFABF76D821DEC
    constexpr float PopSpeed[3] = { 12.f, 8.f, 4.f };  // 58523180, A73073D3, FB71DF2C
    constexpr int PopSpeedSlot = 1111;         // 7A745D81 / 537C97E4 / 9C4CDCF0 (all 0x457)
    constexpr int PopLocal = 1096;             // C3C25A37D00712A8
    // Wheel touchdown sounds (0x824A8288): SPLC ids by [touchdown kind * 3 + class], kind 0 = all
    // wheels at once, 1 = first pair, 2 = second pair, 3 = one wheel. Array by landing surface:
    // 5A93802D11B00173 for hard ground (AudioSurfaceMap landing class 0; the reference footage's
    // concrete landings match it), 797EC34502499EC3 for the soft class. (The second pair of
    // arrays, A0F86FEE / AB0D9205, is picked by Motion+0xC8 < 0.5, which the engine does not
    // publish; Skate 3's own concrete landings come from the first.)
    constexpr int TouchHard[12] = { 1051, 1052, 1053, 1054, 1055, 1056, 1057, 1058, 1059, 1060, 1061, 1127 };
    constexpr int TouchSoft[12] = { 1062, 1063, 1064, 1065, 1066, 1067, 1068, 1069, 1070, 1071, 1131, 1132 };
    // Wheel landing class (0x824A0160): the stored air factor >= 0.5 -> 2, >= 0.31 -> 1, else 0.
    constexpr float WheelHeavy = 0.5f;         // 6D3D91A9BA7ADCDC
    constexpr float WheelMedium = 0.31f;       // 2A70BB8A382574E4
    // grind tuning (049861E8F9A8D16B "default", slot 0x28)
    constexpr float GrindMedium = 0.25f;       // 086B66C3D4FFEE8F
    constexpr float GrindTop = 0.5f;           // B2ACAFDBCD963C93
    // body (6EBA5BCD3E38A98A "default", slot 0x24)
    constexpr int BodyCooldownFrames = 15;     // 6DD85F43C1B6E6AA

    // The local human skater: AudioPlayer+0x48 set, +0x40 zero.
    constexpr bool bLocal = true;

    // ---- per-skater state (audio object / components) ----
    struct State
    {
        bool PrevAir = false, PrevGrind = false;   // board component +0x78 / +0x7A
        float LandStrength = 0;                    // +0x154 (air time while airborne)
        int FramesSinceLand = 0;                   // +0x1A8
        float GrindCooldown = 0;                   // +0x7C
        int LayerLatch = 0, LayerFrames = 0;       // audio obj +0x308 / +0x304
        float BodyCooldown[6] = {};                // skater component +0x104 + 4 * part
        float WheelAir[4] = {};                    // audio obj +0xF4 + 4w (air factor per wheel)
        int WheelClass[4] = {};                    // audio obj +0x1C0 + 4w
        bool Latched[4] = {};                      // skater component +0x84..0x87
        bool PairSingle = false;                   // +0x88
        uint64_t LastTick = ~0ull;                 // engine tick of the last processed audio frame
        float PendingDt = 0;
    } S;

    // 0x824A7B60 / 0x824A7970 / 0x824A81A8
    void Pop(const SkAudio& A, int Type, const SkaterAudio::Backend& B)
    {
        if (S.FramesSinceLand < 2) return;
        int Class = A.pop_strength > PopHeavy ? 2 : A.pop_strength > PopMedium ? 1 : 0;
        if (Type == 0x21 || Type == 0x22) Class = 0;
        const int Wheel0 = GroundMaterial;  // audio obj +0x26C
        const int MatClass = (LandingClass(Wheel0) != 0 ? 2 : 0);  // + emitter state bit (no effect on the pick)
        const int Slot = MatClass < 2 ? PopA[Class] : PopB[Class];
        if (B.PlaySplc) B.PlaySplc(0, Slot, 1.f, 1.f);
        // Speed layer (+0x60 handle; replayed per pop here).
        int Speed = -1;
        const float Spd = A.ground_speed;
        if (Spd > PopSpeed[0]) Speed = PopSpeedSlot;
        else if (Spd > PopSpeed[1]) Speed = PopSpeedSlot;
        else
        {
            if (Spd > PopSpeed[2]) Speed = PopSpeedSlot;
            const Trick* T = (A.trick_id >= 0 && A.trick_id < int(Tricks.size())) ? &Tricks[size_t(A.trick_id)] : nullptr;
            if (T && T->Flag) Speed = PopSpeedSlot;
        }
        if (Speed >= 0 && B.PlaySplc) B.PlaySplc(0, Speed, 1.f, 1.f);
        if (bLocal && B.PlaySplc) B.PlaySplc(0, PopLocal, 1.f, 1.f);
    }

    // 0x824A84C8
    void Land(const SkAudio& A, const SkaterAudio::Backend& B)
    {
        S.FramesSinceLand = 0;
        if (bLocal && B.PlaySplc) B.PlaySplc(0, LandLocal, 1.f, 1.f);
        int Mat = None;
        for (int w = 0; w < 4; w++)
            if (A.wheel_flag[w]) { Mat = GroundMaterial; break; }
        const float Strength = std::clamp(S.LandStrength / LandScale, 0.f, 1.f);
        if (Valid(Mat) && Mats[size_t(Mat)].Bytes[2])
        {
            int Class = 0;
            float Lo = 0.f, Hi = LandMedium;
            if (LandMedium <= Strength) { Class = 1; Lo = LandMedium; Hi = LandTop; }
            const int IA = Intensity(Lo, Hi, Strength, Mat, LandOther, Class);
            const int IB = Intensity(Lo, Hi, Strength, LandOther, Mat, Class);
            if (IA && IB)
                Collide(B, LandOther, Mat, Class, Class, int(float(IA) * LandGainA), int(float(IB) * LandGainB), bLocal);
        }
        if (S.LayerLatch || A.state_flag_80)
        {
            const int Class = LandingClass(0);  // audio obj +0x266 (landing on a marked surface) is never set here
            int Slot = LandLayer[0];
            // (raw air time +0x154, not the clamped strength)
            if (Class == 0) { if (LandLayerSplit <= S.LandStrength) Slot = LandLayer[1]; }
            else Slot = S.LandStrength < LandLayerSplit ? LandLayer[2] : LandLayer[3];
            if (B.PlaySplc) B.PlaySplc(0, Slot, 1.f, 1.f);
        }
    }

    // 0x824A0248 / 0x824A0160 (audio update): per wheel, while airborne, touching or grinding,
    // the class of the stored air factor, then store the new factor.
    void WheelClasses(const SkAudio& A)
    {
        const float X = A.air ? std::clamp(A.airtime * 0.5f, 0.f, 1.f) : 0.f;
        for (int w = 0; w < 4; w++)
            if (X > 0.f || A.wheel_flag[w] || A.grinding)
            {
                const float Stored = S.WheelAir[w];
                S.WheelClass[w] = Stored >= WheelHeavy ? 2 : Stored >= WheelMedium ? 1 : 0;
                S.WheelAir[w] = X;
            }
    }

    // 0x824A6BE0 + 0x824A8288: one touchdown sound; a heavy landing (class 2) plays the medium
    // sound plus the heavy one (unless this is the second half of a split landing).
    void Touch(int Kind, int Class, bool bSecondHalf, const SkaterAudio::Backend& B)
    {
        const int* Ids = LandingClass(GroundMaterial) != 0 ? TouchSoft : TouchHard;
        const bool bHeavy = Class > 1 && (!bSecondHalf || !S.PairSingle);
        if (Class > 1) Class = 1;
        if (!B.PlaySplc) return;
        B.PlaySplc(0, Ids[Kind * 3 + Class], 1.f, 1.f);
        if (bHeavy) B.PlaySplc(0, Ids[Kind * 3 + 2], 1.f, 1.f);
    }

    // 0x824A6578: which wheels just touched down (all four, a pair, one) and how hard.
    void Touchdown(const SkAudio& A, const SkaterAudio::Backend& B)
    {
        int New = 0, Class = 0;
        bool bNew[4] = {};
        for (int w = 0; w < 4; w++)
        {
            if (!A.wheel_flag[w]) S.Latched[w] = false;
            else if (!S.Latched[w])
            {
                bNew[w] = true;
                New++;
                Class = std::max(Class, S.WheelClass[w]);
            }
        }
        int Down = 0;
        for (bool L : S.Latched) Down += L ? 1 : 0;
        auto LatchNew = [&] { for (int w = 0; w < 4; w++) if (bNew[w]) S.Latched[w] = true; };
        auto LatchAll = [&] { for (bool& L : S.Latched) L = true; };
        const bool bLight = Class < 2;
        if (New == 0) return;
        if (New == 4) { Touch(0, Class, false, B); LatchAll(); S.PairSingle = false; return; }
        if (New == 2)
        {
            if (Down == 0) { S.PairSingle = bLight; Touch(1, Class, false, B); LatchNew(); return; }
            if (Down == 2) { Touch(2, Class, true, B); LatchNew(); S.PairSingle = false; return; }
            Touch(0, Class, true, B); LatchAll(); S.PairSingle = false;
            return;
        }
        if (New == 3) { Touch(0, Class, true, B); LatchAll(); S.PairSingle = false; return; }
        // one wheel
        if (Down == 3) { LatchAll(); return; }
        if (Down == 2) { Touch(2, Class, true, B); LatchAll(); S.PairSingle = false; return; }
        if (Down == 1) { Touch(3, Class, S.PairSingle, B); S.PairSingle = bLight; LatchNew(); return; }
        S.PairSingle = bLight;
        Touch(3, Class, false, B);
        LatchNew();
    }

    // 0x824A8F78
    void GrindHit(const SkAudio& A, const SkaterAudio::Backend& B)
    {
        if (S.GrindCooldown > 0.f) return;
        // The grind surface's material (Audio+0x28 - 1, metal_solid_round_1 when untagged); Fortnite
        // geometry carries no Skate 3 surface tags, so every surface is the ground material for now.
        const int Other = GroundMaterial;
        const int Fam = A.grind_family;
        const int Mine = (Fam == 1 || Fam == 2 || Fam == 5) ? 0x5f : 0x60;
        const float Force = A.grind_force;
        int Class = 0;
        float Lo = 0.f, Hi = GrindMedium;
        if (GrindMedium < Force) { Class = 1; Lo = GrindMedium; Hi = GrindTop; }
        const int IA = Mine < None ? Intensity(Lo, Hi, Force, Mine, Other, Class) : 0;
        const int IB = Other < None ? Intensity(Lo, Hi, Force, Other, Mine, Class) : 0;
        Collide(B, Mine, Other, Class, Class, IA, IB, bLocal);
        S.GrindCooldown = 0.5f;
    }

    // 0x824AA020 (body region impacts; the stats, Hall of Meat and foley side-paths are separate)
    void Body(const SkAudio& A, float Dt, const SkaterAudio::Backend& B, bool bAllowed)
    {
        static const int PartMaterial[6] = { 0x61, 0x62, 0x64, 0x64, 0x63, 0x63 };  // 0x824AAA38
        for (int p = 0; p < 6; p++)
        {
            float& Cool = S.BodyCooldown[p];
            const float Force = A.region_force[p];
            if (bAllowed && Force > 0.f && Cool <= 0.f)
            {
                const int Body = PartMaterial[p];
                const uint32_t Tag = A.region_material[p];
                int Ground = Tag == 0 ? None : int(Tag) - 1;
                if (Ground > None || Ground < 0) Ground = None;
                if (Ground == None) Ground = GroundMaterial;  // every Fortnite surface: concrete
                float LoA = 0, HiA = 0, LoB = 0, HiB = 0;
                const int ClassA = Body < None ? ForceClass(Force, Body, LoA, HiA) : 3;
                const int ClassB = Ground < None ? ForceClass(Force, Ground, LoB, HiB) : 3;
                if (!(ClassA == 3 && ClassB == 3))
                {
                    Cool = float(BodyCooldownFrames);
                    const int IA = Body < None ? Intensity(LoA, HiA, Force, Body, Ground, ClassA) : 0;
                    const int IB = Ground < None ? Intensity(LoB, HiB, Force, Ground, Body, ClassB) : 0;
                    Collide(B, Body, Ground, ClassA, ClassB, IA, IB, bLocal);
                    // Heavy hits add a medium layer scaled by each material's range factor.
                    const int A2 = ClassA == 2 ? 1 : 3, B2 = ClassB == 2 ? 1 : 3;
                    if (!(A2 == 3 && B2 == 3))
                        Collide(B, Body, Ground, A2, B2, int(RangeScale(Body) * float(IA)), int(RangeScale(Ground) * float(IB)), bLocal);
                }
            }
            // Cooldown counts game frames (30 fps); at most one frame per update.
            if (Cool > 0.f) Cool -= std::fmin(Dt * 30.f, 1.f);
        }
    }

    bool LoadMaterials()
    {
        std::ifstream In(MaterialsPath);
        if (!In) { Log("SkaterAudio: %s missing", MaterialsPath.c_str()); return false; }
        std::string Line;
        int Count = 0;
        SurfaceMap.clear();
        while (std::getline(In, Line))
        {
            std::istringstream L(Line);
            std::string Key;
            L >> Key;
            if (Key == "surf")
            {
                int Idx;
                std::array<int, 18> E{};
                L >> Idx;
                for (int& V : E) L >> V;
                SurfaceMap.push_back(E);
                continue;
            }
            if (Key != "mat") continue;
            int Idx, Kind;
            std::string Name, Tag;
            L >> Idx >> Kind >> Name;
            if (Idx < 0 || Idx >= None) continue;
            Material& M = Mats[size_t(Idx)];
            M.Kind = Kind;
            M.Name = Name;
            if (Kind == -1) continue;
            auto Floats = [&](float* Out, int N, bool& bPresent) {
                std::string First;
                L >> First;
                if (First == "-") { bPresent = false; return; }
                bPresent = true;
                Out[0] = std::stof(First);
                for (int i = 1; i < N; i++) L >> Out[i];
            };
            bool bDummy = true;
            L >> Tag; for (int& W : M.Words) L >> W;                      // words
            L >> Tag; for (uint8_t& B : M.Bytes) { int V; L >> V; B = uint8_t(V); }  // bytes
            L >> Tag; Floats(M.Thr, 4, bDummy);                             // thr
            L >> Tag; Floats(M.Rng, 15, bDummy);                            // rng
            L >> Tag; Floats(M.AltThr, 4, M.bAltThr);                       // althr
            L >> Tag; Floats(M.AltRng, 15, M.bAltRng);                      // altrng
            L >> Tag; for (int& H : M.Hom) L >> H;                          // hom
            int Cartoon;
            L >> Tag >> Cartoon >> Tag >> M.AltPitch;                       // cartoon, altpitch
            Count++;
        }
        Log("SkaterAudio: %d audio materials, %zu surface entries", Count, SurfaceMap.size());
        return Count > 0 && SurfaceMap.size() >= 0x5f;
    }

    bool LoadTricks()
    {
        std::ifstream In(TricksPath);
        if (!In) { Log("SkaterAudio: %s missing", TricksPath.c_str()); return false; }
        std::string Line;
        while (std::getline(In, Line))
        {
            std::istringstream L(Line);
            std::string Key, Name;
            int Id;
            Trick T;
            if (!(L >> Key) || Key != "trick") continue;
            L >> Id >> Name >> T.First >> T.Second >> T.Flag >> T.Tier;
            if (Id >= 0 && Id < 2000)
            {
                if (Id >= int(Tricks.size())) Tricks.resize(size_t(Id) + 1);
                Tricks[size_t(Id)] = T;
            }
        }
        Log("SkaterAudio: %zu trick audio entries", Tricks.size());
        return !Tricks.empty();
    }
}

bool SkaterAudio::Load()
{
    const bool bMats = LoadMaterials();
    const bool bTricks = LoadTricks();
    std::ifstream In(GrindPath);
    std::string Line;
    int Count = 0;
    while (std::getline(In, Line))
    {
        std::istringstream L(Line);
        std::string Key;
        L >> Key;
        if (Key == "grindspeed") L >> GrindSpeedDiv;
        if (Key != "gsurf") continue;
        int Idx = -1;
        L >> Idx;
        if (Idx < 0 || Idx >= 14) continue;
        for (float& G : GrindTuning[Idx].Gain) L >> G;
        for (float& G : GrindTuning[Idx].Scale) L >> G;
        Count++;
    }
    Log("SkaterAudio: %d grind surfaces", Count);
    return bMats && bTricks;
}

void SkaterAudio::Reset()
{
    S = State{};
}

void SkaterAudio::SetGroundMaterial(int Material)
{
    if (Valid(Material)) GroundMaterial = Material;
}

void SkaterAudio::Update(const SkAudio& A, float Dt, const Backend& B, bool bBodyHits)
{
    if (SurfaceMap.empty()) return;
    // One pass per engine tick (the game runs its audio update once per simulation frame).
    S.PendingDt += Dt;
    if (A.tick == S.LastTick) return;
    S.LastTick = A.tick;
    Dt = S.PendingDt;
    S.PendingDt = 0;
    WheelClasses(A);
    Touchdown(A, B);  // 0x824A6578 runs before the board dispatcher
    // 0x824A6F70 (board dispatcher)
    S.FramesSinceLand++;
    const bool bAir = A.air != 0, bGrind = A.grinding != 0;
    if (!S.PrevAir)
    {
        if (bAir && A.trick_active)
        {
            const int Type = (A.trick_id >= 0 && A.trick_id < int(Tricks.size())) ? Tricks[size_t(A.trick_id)].First : -1;
            const bool bPopType = Type != -1 && Type != 0x20 && Type != 0x1f && Type != 0x23 && Type != 0x24;
            if (bPopType && !S.PrevGrind) Pop(A, Type, B);
        }
    }
    else if (!bAir && !bGrind)
        Land(A, B);
    if (bAir) S.LandStrength = A.airtime;
    S.PrevAir = bAir;
    if (bGrind && !S.PrevGrind) GrindHit(A, B);
    S.PrevGrind = bGrind;
    if (S.GrindCooldown > 0.f) S.GrindCooldown -= Dt;

    // Landing layer latch (audio update 0x8249ECA0: obj+0x301 / +0x304 / +0x308).
    if (A.state_flag_80) { S.LayerLatch = 1; S.LayerFrames = 0; }
    else if (S.LayerFrames < 5) S.LayerFrames++;
    else S.LayerLatch = 0;

    Body(A, Dt, B, bBodyHits);
}

int SkaterAudio::TrickAudio(int TrickId, bool bSecond)
{
    if (TrickId < 0 || TrickId >= int(Tricks.size())) return -1;
    return bSecond ? Tricks[size_t(TrickId)].Second : Tricks[size_t(TrickId)].First;
}

int SkaterAudio::GrindSurface()
{
    if (GroundMaterial < 0 || GroundMaterial >= None) return 4;  // no grind material: surface 4
    const int E = GroundMaterial < 0x5e ? GroundMaterial : 0x5e;
    return E < int(SurfaceMap.size()) ? SurfaceMap[size_t(E)][4] : 4;
}

float SkaterAudio::GrindGain(int Surface, int Kind)
{
    if (Surface > 13 || Surface < 0) Surface = 4;
    return (Kind >= 0 && Kind < 4) ? GrindTuning[Surface].Gain[Kind] : 1.f;
}

float SkaterAudio::GrindScale(int Surface, int Kind)
{
    if (Surface > 13 || Surface < 0) Surface = 4;
    return (Kind >= 0 && Kind < 4) ? GrindTuning[Surface].Scale[Kind] : 1.f;
}

float SkaterAudio::GrindSpeedDivisor() { return GrindSpeedDiv; }

int SkaterAudio::SkidSurface()
{
    if (GroundMaterial < 0 || GroundMaterial >= None) return 0;
    const int E = GroundMaterial < 0x5e ? GroundMaterial : 0x5e;
    return E < int(SurfaceMap.size()) ? std::clamp(SurfaceMap[size_t(E)][3], 0, 4) : 0;
}

int SkaterAudio::RollingSurface()
{
    if (GroundMaterial < 0 || GroundMaterial >= None) return 3;
    const int E = GroundMaterial < 0x5e ? GroundMaterial : 0x5e;
    return E < int(SurfaceMap.size()) ? SurfaceMap[size_t(E)][1] : 3;
}

int SkaterAudio::FootSurface()
{
    if (GroundMaterial < 0 || GroundMaterial >= None) return 1;
    const int E = GroundMaterial < 0x5e ? GroundMaterial : 0x5e;
    return E < int(SurfaceMap.size()) ? SurfaceMap[size_t(E)][6] : 1;
}

int SkaterAudio::SeamSurface(bool bSurfaceChange)
{
    if (GroundMaterial < 0 || GroundMaterial >= None) return 0;
    const int E = GroundMaterial < 0x5e ? GroundMaterial : 0x5e;
    if (E >= int(SurfaceMap.size())) return 0;
    return bSurfaceChange ? SurfaceMap[size_t(E)][9] + 5 : SurfaceMap[size_t(E)][8];
}

int SkaterAudio::FootDragSurface(bool bState54)
{
    if (GroundMaterial < 0 || GroundMaterial >= None) return 0;
    const int E = GroundMaterial < 0x5e ? GroundMaterial : 0x5e;
    const int S = E < int(SurfaceMap.size()) ? SurfaceMap[size_t(E)][5] : 0;
    return bState54 && S == 1 ? 0 : S;
}
