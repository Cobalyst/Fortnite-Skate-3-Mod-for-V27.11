#include <unordered_set>
#include "Avatar.h"
#include "EngineApi.h"
#include "DropperWorld.h"

#include "SDK/ProceduralMeshComponent_classes.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace SDK;

namespace
{
    // Fortnite bone -> Skate 3 bone, with the child of each used to line up bone directions.
    struct BonePair { const char* Fn; const char* Skate; const char* FnChild; const char* SkateChild; };
    constexpr BonePair Pairs[] =
    {
        { "pelvis",     "HIPS",          "spine_01",   "SPINE" },
        { "spine_01",   "SPINE",         "spine_02",   "SPINE1" },
        { "spine_02",   "SPINE1",        "spine_03",   "SPINE2" },
        { "spine_03",   "SPINE2",        "spine_04",   "SPINE3" },
        { "spine_04",   "SPINE3",        "neck_01",    "NECK" },
        { "neck_01",    "NECK",          "head",       "HEAD" },
        { "head",       "HEAD",          nullptr,      nullptr },
        { "clavicle_l", "LEFTSHOULDER",  "upperarm_l", "LEFTARM" },
        { "upperarm_l", "LEFTARM",       "lowerarm_l", "LEFTFOREARM" },
        { "lowerarm_l", "LEFTFOREARM",   "hand_l",     "LEFTHAND" },
        { "hand_l",     "LEFTHAND",      nullptr,      nullptr },
        { "clavicle_r", "RIGHTSHOULDER", "upperarm_r", "RIGHTARM" },
        { "upperarm_r", "RIGHTARM",      "lowerarm_r", "RIGHTFOREARM" },
        { "lowerarm_r", "RIGHTFOREARM",  "hand_r",     "RIGHTHAND" },
        { "hand_r",     "RIGHTHAND",     nullptr,      nullptr },
        { "thigh_l",    "LEFTUPLEG",     "calf_l",     "LEFTLEG" },
        { "calf_l",     "LEFTLEG",       "foot_l",     "LEFTFOOT" },
        { "foot_l",     "LEFTFOOT",      "ball_l",     "LEFTTOEBASE" },
        { "ball_l",     "LEFTTOEBASE",   nullptr,      nullptr },
        { "thigh_r",    "RIGHTUPLEG",    "calf_r",     "RIGHTLEG" },
        { "calf_r",     "RIGHTLEG",      "foot_r",     "RIGHTFOOT" },
        { "foot_r",     "RIGHTFOOT",     "ball_r",     "RIGHTTOEBASE" },
        { "ball_r",     "RIGHTTOEBASE",  nullptr,      nullptr },
    };

    const std::wstring BoardTexture[] =
    {
        ModPathW("skate-data\\board\\tex0.png"),
        ModPathW("skate-data\\board\\tex1.png"),
        ModPathW("skate-data\\board\\tex2.png"),
    };

    struct MappedBone
    {
        int   FnIndex;
        FName FnName;
        int   SkateIndex;
        Mat4  Offset;  // skate world -> Fortnite bone world
    };

    struct Follower
    {
        USkinnedMeshComponent* Component;
        USkinnedMeshComponent* OriginalLeader;
        int32 Index;   // GObjects slot: Fortnite can destroy and rebuild outfit parts at any time

        bool Alive() const { return Component && UObject::GObjects->GetByIndex(Index) == Component; }
    };

    struct BoardPiece
    {
        UProceduralMeshComponent* Mesh;
        uint32_t Bone;
        uint32_t Texture;   // Skate 3 board texture (0 = the deck graphic)
    };
    USkeletalMeshComponent* Body = nullptr;
    UPoseableMeshComponent* Poser = nullptr;
    std::vector<MappedBone> Mapped;
    std::vector<Follower> Followers;
    std::vector<BoardPiece> Board;
    bool bBoardWrap = true;                        // mod_settings.txt board_wrap
    UMaterialInstanceDynamic* WrapDeck = nullptr;  // the wrap's base colour on the pass-through material

    UMaterialInstanceDynamic* PlainBoardMaterial(ACharacter* Character, uint32_t Texture);
    UMaterialInstanceDynamic* BoardMaterial(ACharacter* Character, uint32_t Texture);

    void ApplyBoardMaterials(ACharacter* Character)
    {
        for (BoardPiece& P : Board)
        {
            UMaterialInterface* Use = (P.Texture == 0 && bBoardWrap && WrapDeck) ? static_cast<UMaterialInterface*>(WrapDeck) : nullptr;
            // Skate 3's own board textures (skate-data/board/tex0-2.png) on the same pass-through
            // material the wrap renders with; flat colours only if a texture cannot be loaded.
            if (!Use) Use = BoardMaterial(Character, P.Texture);
            if (!Use) Use = PlainBoardMaterial(Character, P.Texture);
            if (Use && P.Mesh) P.Mesh->SetMaterial(0, Use);
        }
    }

    std::vector<float> SkateBones;
    bool bLeading = false;
    bool bLoggedSkeleton = false;
    std::string LastMontage;
    UMaterialInstanceDynamic* BoardMaterials[3] = {};

    FName Name(const char* Text)
    {
        std::wstring Wide(Text, Text + strlen(Text));
        return UKismetStringLibrary::Conv_StringToName(FString(Wide.c_str()));
    }

    FTransform Identity()
    {
        FTransform T{};
        T.Rotation.W = 1;
        T.Scale3D = FVector{ 1, 1, 1 };
        return T;
    }

    template<typename T>
    TArray<T> View(std::vector<T>& V) { return TArray<T>(V.data(), int32(V.size()), int32(V.size())); }

    int SkateBone(const char* Wanted)
    {
        const uint32_t N = Api.BoneCount(Engine);
        for (uint32_t i = 0; i < N; i++)
            if (strcmp(Api.BoneName(Engine, i), Wanted) == 0)
                return int(i);
        return -1;
    }

    Mat4 SkateBind(int Index)
    {
        float M[16];
        return Index >= 0 && Api.BoneBind(Engine, uint32_t(Index), M) ? Mat4::FromArray(M) : Mat4{};
    }

    // Upright frame of a skeleton from hips, head and the two upper arms.
    Mat4 BodyFrame(const Vec3& Hips, const Vec3& Head, const Vec3& LeftArm, const Vec3& RightArm)
    {
        const Vec3 Up = (Head - Hips).Normal();
        Vec3 Left = LeftArm - RightArm;
        Left = (Left - Up * Left.Dot(Up)).Normal();
        return Mat4::FromBasis(Left.Cross(Up), Left, Up);
    }

    void SetLeading(bool bLead)
    {
        for (const Follower& F : Followers)
        {
            if (!F.Alive()) continue;
            if (bLead)
                F.Component->SetLeaderPoseComponent(Poser, true, true);
            else
                F.Component->SetLeaderPoseComponent(F.OriginalLeader, true, true);
        }
        bLeading = bLead;
    }

    // Fortnite rebuilds or re-links outfit parts during a session (loadout refresh, respawn after a
    // bail, weapon swaps). A part that went back to following the base Mesh shows the A-pose: the
    // base only copies the poser, so it has no pose of its own to give. Once a second, every part
    // with the character skeleton is put back on the poser and new parts are added; parts that
    // were destroyed are dropped. Logged when anything changes, with the part count (a growing
    // count would mean parts pile up on the character).
    void RelinkParts(ACharacter* Character)
    {
        static ULONGLONG Last = 0;
        const ULONGLONG Now = GetTickCount64();
        if (!bLeading || !Poser || !Body || Now - Last < 1000) return;
        Last = Now;
        Followers.erase(std::remove_if(Followers.begin(), Followers.end(), [](const Follower& F) { return !F.Alive(); }), Followers.end());
        const FName PelvisName = Name("pelvis");
        TArray<UActorComponent*> Components = Character->K2_GetComponentsByClass(USkinnedMeshComponent::StaticClass());
        int Relinked = 0, Added = 0;
        for (int i = 0; i < Components.Num(); i++)
        {
            auto* Skinned = static_cast<USkinnedMeshComponent*>(Components[i]);
            if (!Skinned || Skinned == Poser || !Skinned->GetSkinnedAsset()) continue;
            if (Skinned != Body && Skinned->GetBoneIndex(PelvisName) < 0) continue;
            auto It = std::find_if(Followers.begin(), Followers.end(), [&](const Follower& F) { return F.Component == Skinned; });
            if (It == Followers.end())
            {
                USkinnedMeshComponent* Leader = Skinned->LeaderPoseComponent.Get();
                Followers.push_back({ Skinned, Leader == Poser ? (Skinned == Body ? nullptr : Body) : Leader, Skinned->Index });
                Added++;
            }
            if (Skinned->LeaderPoseComponent.Get() != Poser)
            {
                Skinned->SetLeaderPoseComponent(Poser, true, true);
                Relinked++;
            }
        }
        static int LastCount = -1;
        if (Relinked || Added || Components.Num() != LastCount)
            Log("[avatar] %d skinned parts on the character, %zu follow the skater pose; re-linked %d, new %d",
                Components.Num(), Followers.size(), Relinked, Added);
        LastCount = Components.Num();
    }

    bool BuildRetarget()
    {
        Mapped.clear();
        const int Count = Body->GetNumBones();

        // Reference pose of the Fortnite skeleton in component space.
        std::vector<Mat4> Ref(Count);
        std::vector<int> Parent(Count, -1);
        std::vector<std::string> Names(Count);
        for (int i = 0; i < Count; i++)
        {
            const FName BoneName = Body->GetBoneName(i);
            Names[i] = BoneName.ToString();
            Parent[i] = Body->GetBoneIndex(Body->GetParentBone(BoneName));
            const Mat4 Local = FromTransform(Body->GetRefPoseTransform(i));
            Ref[i] = Parent[i] >= 0 && Parent[i] < i ? Ref[Parent[i]] * Local : Local;
        }
        if (!bLoggedSkeleton)
        {
            bLoggedSkeleton = true;
            std::string List;
            for (int i = 0; i < Count; i++)
                List += Names[i] + "(" + std::to_string(Parent[i]) + ") ";
            Log("Fortnite skeleton: %d bones: %s", Count, List.c_str());
        }
        auto FnBone = [&](const char* Wanted) -> int
        {
            for (int i = 0; i < Count; i++)
                if (Names[i] == Wanted) return i;
            return -1;
        };

        const int Pelvis = FnBone("pelvis"), Head = FnBone("head"), ArmL = FnBone("upperarm_l"), ArmR = FnBone("upperarm_r");
        const int SHips = SkateBone("HIPS"), SHead = SkateBone("HEAD"), SArmL = SkateBone("LEFTARM"), SArmR = SkateBone("RIGHTARM");
        if (Pelvis < 0 || Head < 0 || ArmL < 0 || ArmR < 0 || SHips < 0 || SHead < 0 || SArmL < 0 || SArmR < 0)
        {
            Log("Retarget: missing core bones (fortnite %d %d %d %d, skate %d %d %d %d)", Pelvis, Head, ArmL, ArmR, SHips, SHead, SArmL, SArmR);
            return false;
        }

        // Rotation taking the Fortnite reference body onto the skate reference body.
        const Mat4 FnFrame = BodyFrame(Ref[Pelvis].Translation(), Ref[Head].Translation(), Ref[ArmL].Translation(), Ref[ArmR].Translation());
        const Mat4 SkFrame = BodyFrame(SkateBind(SHips).Translation(), SkateBind(SHead).Translation(),
            SkateBind(SArmL).Translation(), SkateBind(SArmR).Translation());
        const Mat4 Align = SkFrame * FnFrame.RigidInverse();

        std::vector<Mat4> Fits(Count, Align);
        std::string Missing;
        for (const BonePair& P : Pairs)
        {
            const int F = FnBone(P.Fn), S = SkateBone(P.Skate);
            if (F < 0 || S < 0) { Missing += std::string(P.Fn) + " "; continue; }

            // Line up this bone's direction (towards its child) with the skate bone's.
            Mat4 Fit = Parent[F] >= 0 ? Fits[Parent[F]] : Align;
            const int FC = P.FnChild ? FnBone(P.FnChild) : -1, SC = P.SkateChild ? SkateBone(P.SkateChild) : -1;
            if (FC >= 0 && SC >= 0)
            {
                const Vec3 DirF = Align.TransformVector(Ref[FC].Translation() - Ref[F].Translation()).Normal();
                const Vec3 DirS = (SkateBind(SC).Translation() - SkateBind(S).Translation()).Normal();
                Fit = Mat4::Arc(DirF, DirS) * Align;
            }
            Fit.SetTranslation(Vec3{});
            Fit.SetTranslation(SkateBind(S).Translation() - Fit.TransformVector(Ref[F].Translation()));
            Fits[F] = Fit;

            MappedBone M;
            M.FnIndex = F;
            M.FnName = Body->GetBoneName(F);
            M.SkateIndex = S;
            M.Offset = SkateBind(S).RigidInverse() * Fit * Ref[F];
            Mapped.push_back(M);
        }
        // Parents before children, so each bone is set relative to an up-to-date parent.
        std::sort(Mapped.begin(), Mapped.end(), [](const MappedBone& A, const MappedBone& B) { return A.FnIndex < B.FnIndex; });
        Log("Retarget: %zu bones mapped%s%s", Mapped.size(), Missing.empty() ? "" : ", missing: ", Missing.c_str());
        return !Mapped.empty();
    }

    UMaterialInstanceDynamic* BoardMaterial(ACharacter* Character, uint32_t Texture)
    {
        if (Texture >= 3) Texture = 0;
        if (BoardMaterials[Texture])
            return BoardMaterials[Texture];

        static UMaterialInterface* Parent = nullptr;
        if (!Parent)
            Parent = FindMaterialByName("Widget3DPassThrough_Opaque");
        UTexture2D* Tex = UKismetRenderingLibrary::ImportFileAsTexture2D(Character, FString(BoardTexture[Texture].c_str()));
        // Lit (shaded by Fortnite's lights, like the placed objects); the unlit pass-through only
        // when no lit material takes the texture.
        if (Tex)
            if (UMaterialInstanceDynamic* Lit = DropperWorld::LitMaterial(Character, Tex))
            {
                KeepAlive(Tex);
                Log("Board texture %u: lit material", Texture);
                BoardMaterials[Texture] = Lit;
                return Lit;
            }
        if (!Parent || !Tex)
        {
            Log("Board material: parent %p texture %p (texture %u)", Parent, Tex, Texture);
            return nullptr;
        }
        UMaterialInstanceDynamic* M = UKismetMaterialLibrary::CreateDynamicMaterialInstance(Character, Parent, FName(), EMIDCreationFlags::None);
        if (M)
        {
            M->SetTextureParameterValue(Name("SlateUI"), Tex);
            KeepAlive(M);     // cached here while the wrap is shown instead (key 7)
            KeepAlive(Tex);
        }
        Log("Board texture %u: %dx%d %p, material %p reads back texture %p", Texture,
            Tex->Blueprint_GetSizeX(), Tex->Blueprint_GetSizeY(), Tex, M, M ? M->K2_GetTextureParameterValue(Name("SlateUI")) : nullptr);
        BoardMaterials[Texture] = M;
        return M;
    }

    // The player's equipped Fortnite wrap (locker "Wraps"), for the deck. AFortPlayerPawn::
    // CosmeticLoadout (FFortAthenaLoadout) at 0x30F0: ItemWrapOverride +0xB8, ItemWraps +0xC0;
    // UAthenaItemWrapDefinition::ItemWrapMaterial (soft pointer) at 0x7B0. Weapon-wrap materials
    // need weapon mesh data (they render black on the board), so the deck takes the wrap's
    // colour texture through the same pass-through material as the Skate 3 textures.
    UMaterialInterface* WrapMaterial = nullptr;  // the equipped wrap's own material (set by WrapTexture)
    // mod_settings board_wrap_material 1 (test): the deck uses the wrap's own material, its customization mask
    // (which parts of an item get the wrap; all black = none, on the board it has no item mask of its own) set
    // to all white, so the whole deck gets the full wrap look (layers, colours, glow) as on a weapon.
    bool bWrapMaterialMode = false;

    // A plain opaque lit material that renders on procedural meshes in the cooked game. (The UI
    // pass-through material used before has no shaders for this mesh type: Unreal drew its grey
    // checker "default material" instead.)
    UMaterialInterface* PlainMaterial()
    {
        static UMaterialInterface* Plain = nullptr;
        static bool bTried = false;
        if (!Plain && !bTried)
        {
            bTried = true;
            Plain = FindMaterialByName("BasicShapeMaterial");   // (FindObjectFast found its package)
            if (!Plain)
            {
                const FSoftObjectPath Path = UKismetSystemLibrary::MakeSoftObjectPath(FString(L"/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
                UObject* O = UKismetSystemLibrary::LoadAsset_Blocking(UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(Path));
                if (O && O->IsA(UMaterialInterface::StaticClass())) Plain = static_cast<UMaterialInterface*>(O);
            }
            Log("Board: plain material %s", Plain ? Plain->GetFullName().c_str() : "NOT FOUND");
            if (Plain) KeepAlive(Plain);
        }
        return Plain;
    }

    // Flat colours of the Skate 3 board textures (their average colour; tex0 = deck, 1 = trucks, 2 = wheels).
    UMaterialInstanceDynamic* PlainBoardMaterial(ACharacter* Character, uint32_t Texture)
    {
        static UMaterialInstanceDynamic* Made[3] = {};
        if (Texture >= 3) Texture = 0;
        if (Made[Texture]) return Made[Texture];
        UMaterialInterface* Plain = PlainMaterial();
        if (!Plain) return nullptr;
        static const FLinearColor Colors[3] = {
            { 74 / 255.f, 81 / 255.f, 93 / 255.f, 1 }, { 139 / 255.f, 140 / 255.f, 146 / 255.f, 1 }, { 153 / 255.f, 150 / 255.f, 145 / 255.f, 1 } };
        UMaterialInstanceDynamic* M = UKismetMaterialLibrary::CreateDynamicMaterialInstance(Character, Plain, FName(), EMIDCreationFlags::None);
        if (M)
        {
            auto Lin = [](float c) { return std::pow(c, 2.2f); };
            const FLinearColor& C = Colors[Texture];
            M->SetVectorParameterValue(Name("Color"), FLinearColor{ Lin(C.R), Lin(C.G), Lin(C.B), 1 });
            KeepAlive(M);
        }
        Made[Texture] = M;
        return M;
    }

    // The material on the item the player is holding: Fortnite applies the equipped wrap to it, so
    // it is the wrap exactly as it looks on the weapon (the wrap material alone renders black).
    // Static mesh parts first (their materials have shaders for the board's mesh type).
    UMaterialInterface* HeldItemMaterial(ACharacter* Character)
    {
        static UClass* FortPawn = UObject::FindClassFast("FortPawn");
        if (!FortPawn || !Character->IsA(FortPawn)) return nullptr;
        AActor* Weapon = *reinterpret_cast<AActor**>(reinterpret_cast<uint8*>(Character) + 0x9F8);
        if (!Weapon || !Weapon->IsA(AActor::StaticClass())) return nullptr;
        UMaterialInterface* Best = nullptr;
        int BestScore = -1;
        std::string List;
        TArray<UActorComponent*> Comps = Weapon->K2_GetComponentsByClass(UMeshComponent::StaticClass());
        for (int i = 0; i < Comps.Num(); i++)
        {
            UMeshComponent* M = static_cast<UMeshComponent*>(Comps[i]);
            if (!M) continue;
            UMaterialInterface* Mat = M->GetMaterial(0);
            if (!Mat) continue;
            const bool bStatic = M->IsA(UStaticMeshComponent::StaticClass());
            List += M->GetName() + (bStatic ? "[static]=" : "[skeletal]=") + Mat->GetName() + " ";
            const int Score = (bStatic ? 2 : 1) + (M->IsVisible() ? 1 : 0);
            if (Score > BestScore) { BestScore = Score; Best = Mat; }
        }
        Log("Held item %s: meshes %s-> deck uses %s", Weapon->GetName().c_str(), List.empty() ? "(none) " : List.c_str(),
            Best ? Best->GetName().c_str() : "(none)");
        return Best;
    }

    bool bDumpWrap = false;   // set with the wrap survey: the deck's wrap texture is saved as Logs\wrap_<name>.png

    // Draws the texture into a render target and saves it, so what the deck gets can be looked at.
    void DumpWrapTexture(UObject* Context, UTexture* T)
    {
        if (!bDumpWrap || !T) return;
        UTextureRenderTarget2D* Rt = UKismetRenderingLibrary::CreateRenderTarget2D(Context, 512, 512, ETextureRenderTargetFormat::RTF_RGBA8,
                                                                                   FLinearColor{ 0, 0, 0, 1 }, false, false);
        if (!Rt) { Log("[wrap survey] could not make a render target"); return; }
        UCanvas* Canvas = nullptr;
        FVector2D Size{};
        FDrawToRenderTargetContext Ctx{};
        UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(Context, Rt, &Canvas, &Size, &Ctx);
        if (Canvas)
            Canvas->K2_DrawTexture(T, FVector2D{ 0, 0 }, FVector2D{ 512, 512 }, FVector2D{ 0, 0 }, FVector2D{ 1, 1 },
                                   FLinearColor{ 1, 1, 1, 1 }, EBlendMode::BLEND_Opaque, 0.f, FVector2D{ 0.5, 0.5 });
        UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(Context, Ctx);
        const std::wstring Dir = ModPathW("Logs");
        const std::string N = "wrap_" + T->GetName();
        UKismetRenderingLibrary::ExportRenderTarget(Context, Rt, FString(Dir.c_str()), FString(std::wstring(N.begin(), N.end()).c_str()));
        Log("[wrap survey] saved the deck texture as Logs\\%s", N.c_str());
    }

    // Test diagnostic (board_wrap_material): a material's parent chain with every parameter, so the way wraps
    // render can be worked out ("[wrap mat]" lines).
    void LogMaterialChain(const char* What, UMaterialInterface* M)
    {
        for (int Depth = 0; M && Depth < 12; Depth++)
        {
            std::string Line = std::string(What) + " " + std::to_string(Depth) + ": " + M->GetName() + " [" + M->Class->GetName() + "]";
            if (M->IsA(UMaterialInstance::StaticClass()))
            {
                auto* Mi = static_cast<UMaterialInstance*>(M);
                char B[160];
                for (const FScalarParameterValue& P : Mi->ScalarParameterValues)
                {
                    snprintf(B, sizeof(B), " s:%s=%g", P.ParameterInfo.Name.ToString().c_str(), P.ParameterValue);
                    Line += B;
                }
                for (const FVectorParameterValue& P : Mi->VectorParameterValues)
                {
                    snprintf(B, sizeof(B), " v:%s=(%.2f %.2f %.2f %.2f)", P.ParameterInfo.Name.ToString().c_str(),
                             P.ParameterValue.R, P.ParameterValue.G, P.ParameterValue.B, P.ParameterValue.A);
                    Line += B;
                }
                for (const FTextureParameterValue& P : Mi->TextureParameterValues)
                    Line += " t:" + P.ParameterInfo.Name.ToString() + "=" + (P.ParameterValue ? P.ParameterValue->GetName() : "null");
                Log("[wrap mat] %s", Line.c_str());
                M = Mi->Parent;
            }
            else
            {
                if (M->IsA(UMaterial::StaticClass()))
                {
                    auto* Base = static_cast<UMaterial*>(M);
                    char B[160];
                    snprintf(B, sizeof(B), " (base: domain %d, blend %d, shading model %d)", int(Base->MaterialDomain), int(Base->BlendMode),
                             int(Base->MaterialDomain));
                    Line += B;
                }
                Log("[wrap mat] %s", Line.c_str());
                break;
            }
        }
    }

    // The wrap texture the board deck shows: of the wrap material's texture parameters (and its parents'), the
    // main colour / pattern layer. List gets every parameter (for the log).
    UTexture* PickWrapTexture(UMaterialInterface* Material, std::string& List)
    {
        // Wraps are layered material instances: the wrap's own layer on top of shared parents whose texture slots
        // hold generic defaults (black / white fillers, a zebra or cactus pattern...). The wrap's art is in its
        // own layer, so the first layer (from the wrap down) that has a real picture wins; within a layer the
        // main pattern slot beats second layers, decals and glow pictures. 2026-10-11 survey of all 679 wraps:
        // over half of them gave a black filler or a parent's default with the earlier rule.
        auto Has = [](const std::string& S, const char* W) { return S.find(W) != std::string::npos; };
        auto Ends = [](const std::string& S, const char* W)
        {
            const size_t L = strlen(W);
            return S.size() >= L && S.compare(S.size() - L, L, W) == 0;
        };
        auto EndsDigits = [&](const std::string& S, const char* W)   // "_bc", "_bc2", "_d", "_d1"...
        {
            size_t E = S.size();
            while (E > 0 && isdigit(static_cast<unsigned char>(S[E - 1]))) E--;
            return Ends(S.substr(0, E), W);
        };
        std::string Own = Material ? Material->GetName() : std::string();
        for (char& C : Own) C = char(tolower(C));
        auto Filler = [&](const std::string& T)
        {
            // Template textures copied into many wraps' own layers (their real art is in other slots), unless
            // the wrap really is that pattern (Zebra, the Cactus wraps).
            if ((T == "t_wrap_zebra_d" || T == "t_wrap_zebra_n") && !Has(Own, "zebra")) return true;
            if (T == "t_wrap_cactusrocker_bc" && !Has(Own, "cactus")) return true;
            static const char* Exact[] = { "t_blacksrgb", "t_whitesrgb", "t_greysrgb", "uvcheaker", "defaultdiffuse", "3pxrgbimage",
                                           "t_blackmask", "flatnormal", "flatnormalblue", "defaultnormal", "t_black", "t_white", "t_grey",
                                           "t_gray", "whitesquaretexture", "blacksquaretexture" };
            for (const char* E : Exact) if (T == E) return true;
            return T.rfind("t_thinfilm", 0) == 0 || T.rfind("t_vertgradient", 0) == 0 || T.rfind("placeholder", 0) == 0 ||
                   T.rfind("t_default", 0) == 0;
        };
        auto Score = [&](const std::string& N, const std::string& T) -> int
        {
            if (Filler(T)) return 0;
            if (Has(N, "emissive") && !Has(N, "grad") && !Has(N, "mask") && !Has(N, "msk") && !Has(T, "grad") && !Has(T, "mask") &&
                (EndsDigits(T, "_e") || Has(T, "emissive")))
                return 2;   // a glow picture: the art of neon-style wraps
            if (Has(N, "logo") && !Has(N, "mask") && !Has(N, "msk") && !Has(N, "normal") && !Has(T, "mask") && !Has(T, "msk") &&
                !Ends(T, "_n") && !Ends(T, "_m"))
                return 2;   // a logo picture (Neon Graffiti Lava)
            if (Has(T, "normal") || Ends(T, "_n") || Ends(T, "_nrm") || Has(T, "mask") || Has(T, "grad") || EndsDigits(T, "_m"))
                return 0;
            static const char* NotColour[] = { "normal", "mask", "msk", "grad", "film", "emissive", "spec", "rough", "metal", "fresnel",
                                               "warpuv", "grunge", "edgewear", "distortion" };
            for (const char* W : NotColour) if (Has(N, W)) return 0;
            if (Ends(N, "_m")) return 0;
            const bool Colour = Has(N, "diffuse") || Has(N, "basecolor") || Has(N, "base color") || Has(N, "albedo") || Has(N, "_bc") ||
                                Has(N, "basecolour") || EndsDigits(T, "_bc") || EndsDigits(T, "_d") || Has(T, "_diffuse");
            if (!Colour) return 1;
            if (Has(N, "triplaner_01_diffuse") || Has(N, "tops_01__tex_basecolor") || Has(N, "ts_basecolor_01")) return 6;
            if (N == "wrap_diffuse" || Has(N, "sideproejction 01 diffuse") || Has(N, "carocker__text_bc")) return 5;
            if (Has(N, "decal")) return 4;
            if (Has(N, "drip") || Has(N, "overlay") || Has(N, "detail") || Has(N, "triplaner_02") || Has(N, "tops_02") ||
                Has(N, "ts_basecolor_02") || Has(N, "secondary") || Has(N, "animated"))
                return 3;
            return 4;
        };
        std::unordered_set<std::string> Seen;   // a parameter set on a layer above overrides the parents' value
        int Depth = 0;
        for (UMaterialInterface* M = Material; M && M->IsA(UMaterialInstance::StaticClass());
             M = static_cast<UMaterialInstance*>(M)->Parent, Depth++)
        {
            UTexture* Best = nullptr;
            int BestScore = 1;   // only real pictures (2 and up)
            List += "[d" + std::to_string(Depth) + "] ";
            for (const FTextureParameterValue& P : static_cast<UMaterialInstance*>(M)->TextureParameterValues)
            {
                if (!P.ParameterValue) continue;
                std::string N = P.ParameterInfo.Name.ToString();
                std::string T = P.ParameterValue->GetName();
                List += N + "=" + T + " ";
                for (char& C : N) C = char(tolower(C));
                for (char& C : T) C = char(tolower(C));
                if (!Seen.insert(N).second) continue;
                int S = Score(N, T);
                // Virtual textures can't be sampled by the plain material the board uses.
                if ((reinterpret_cast<const uint8*>(P.ParameterValue)[0xFE] & 0x04) != 0) { List += "(virtual) "; S = 0; }
                if (S > BestScore) { BestScore = S; Best = P.ParameterValue; }
            }
            if (Best) return Best;
        }
        return nullptr;
    }

    UTexture* WrapTexture(ACharacter* Character)
    {
        constexpr uintptr_t LoadoutOffset = 0x30F0, OverrideOffset = 0xB8, WrapsOffset = 0xC0, MaterialOffset = 0x7B0;
        static UClass* PlayerPawnClass = UObject::FindClassFast("FortPlayerPawn");
        if (!PlayerPawnClass || !Character->IsA(PlayerPawnClass))
            return nullptr;
        uint8* Loadout = reinterpret_cast<uint8*>(Character) + LoadoutOffset;
        UObject* Wrap = *reinterpret_cast<UObject**>(Loadout + OverrideOffset);
        const auto& Wraps = *reinterpret_cast<TArray<UObject*>*>(Loadout + WrapsOffset);
        for (int i = 0; i < Wraps.Num() && !Wrap; i++)
            Wrap = Wraps[i];  // slot 0 is the vehicle wrap
        if (!Wrap)
        {
            Log("Wrap: none equipped");
            return nullptr;
        }
        const auto& Soft = *reinterpret_cast<TSoftObjectPtr<UObject>*>(reinterpret_cast<uint8*>(Wrap) + MaterialOffset);
        UObject* Material = UKismetSystemLibrary::LoadAsset_Blocking(Soft);
        if (!Material || !Material->IsA(UMaterialInstance::StaticClass()))
        {
            Log("Wrap %s: material %s is not a material instance", Wrap->GetName().c_str(), Material ? Material->GetFullName().c_str() : "(did not load)");
            return nullptr;
        }

        WrapMaterial = static_cast<UMaterialInterface*>(Material);
        std::string List;
        UTexture* Best = PickWrapTexture(static_cast<UMaterialInterface*>(Material), List);
        Log("Wrap %s (%s): textures %s-> deck uses %s", Wrap->GetName().c_str(), Material->GetName().c_str(),
            List.empty() ? "(none) " : List.c_str(), Best ? Best->GetName().c_str() : "(none)");
        return Best;
    }

    void BuildBoard(ACharacter* Character)
    {
        // Deck pieces (Skate 3 texture 0) wear the player's wrap when one is equipped.
        UMaterialInstanceDynamic* Wrap = nullptr;
        WrapMaterial = nullptr;
        WrapDeck = nullptr;
        UMaterialInterface* PassThrough = FindMaterialByName("Widget3DPassThrough_Opaque");
        if (UTexture* T = WrapTexture(Character))
        {
            // Lit first (Fortnite's light and shadows on the deck, as on the placed objects); the
            // unlit pass-through only if no lit material takes the texture.
            if (T->IsA(UTexture2D::StaticClass()))
                if (UMaterialInstanceDynamic* Lit = DropperWorld::LitMaterial(Character, static_cast<UTexture2D*>(T)))
                {
                    static_cast<UTexture2D*>(T)->SetForceMipLevelsToBeResident(3600.f, 0);
                    Lit->SetForceMipLevelsToBeResident(true, true, 3600.f, 0, true);
                    Log("Wrap texture %s on the lit deck material", T->GetName().c_str());
                    DumpWrapTexture(Character, T);
                    Wrap = Lit;
                    WrapDeck = Lit;
                }
            if (WrapDeck) {}   // lit
            else if (!PassThrough) Log("Wrap: pass-through material not found");
            else if (!(Wrap = UKismetMaterialLibrary::CreateDynamicMaterialInstance(Character, PassThrough, FName(), EMIDCreationFlags::None)))
                Log("Wrap: could not create the deck material");
            else
                {
                    Wrap->SetTextureParameterValue(Name("SlateUI"), T);
                    // Nothing else in the world uses this texture, so the streamer may keep only its
                    // smallest mips loaded: keep every mip resident while the board exists.
                    if (T->IsA(UTexture2D::StaticClass()))
                        static_cast<UTexture2D*>(T)->SetForceMipLevelsToBeResident(3600.f, 0);
                    Wrap->SetForceMipLevelsToBeResident(true, true, 3600.f, 0, true);
                    const uint8* Raw = reinterpret_cast<const uint8*>(T);
                    UTexture* Back = Wrap->K2_GetTextureParameterValue(Name("SlateUI"));
                    Log("Wrap texture %s: class %s, virtual %d, never-stream %d, LOD group %d, material reads back %s",
                        T->GetName().c_str(), T->Class->GetName().c_str(), (Raw[0xFE] & 0x04) != 0, (Raw[0xBC] & 1) != 0,
                        int(Raw[0xF5]), Back ? Back->GetName().c_str() : "nothing");
                    KeepAlive(Wrap);
                    WrapDeck = Wrap;
                }
        }

        if (bWrapMaterialMode && WrapMaterial)
        {
            static UTexture* White = nullptr;
            if (!White)
                for (int32 i = 0; i < UObject::GObjects->Num() && !White; i++)
                {
                    UObject* O = UObject::GObjects->GetByIndex(i);
                    if (O && !O->IsDefaultObject() && O->IsA(UTexture::StaticClass()) && O->GetName() == "T_WhitesRGB") White = static_cast<UTexture*>(O);
                }
            UMaterialInstanceDynamic* Own = UKismetMaterialLibrary::CreateDynamicMaterialInstance(Character, WrapMaterial, FName(), EMIDCreationFlags::None);
            if (Own)
            {
                if (White) Own->SetTextureParameterValue(Name("CustomizationMask (_CM)"), White);
                UTexture* Back = Own->K2_GetTextureParameterValue(Name("CustomizationMask (_CM)"));
                KeepAlive(Own);
                WrapDeck = Own;
                Log("Wrap material %s on the deck (customization mask %s)", WrapMaterial->GetName().c_str(),
                    Back ? Back->GetName().c_str() : "none");
                LogMaterialChain("wrap", WrapMaterial);
                // the held item's materials (Fortnite puts wraps on weapons and vehicles)
                constexpr uintptr_t CurrentWeaponOffset = 0x9F8;
                if (AActor* Weapon = *reinterpret_cast<AActor**>(reinterpret_cast<uint8*>(Character) + CurrentWeaponOffset))
                {
                    TArray<UActorComponent*> Comps = Weapon->K2_GetComponentsByClass(UMeshComponent::StaticClass());
                    for (int i = 0; i < Comps.Num(); i++)
                        if (auto* Mc = static_cast<UMeshComponent*>(Comps[i]))
                            for (int k = 0; k < 4; k++)
                                if (UMaterialInterface* Mm = Mc->GetMaterial(k))
                                    LogMaterialChain(("held " + Weapon->GetName() + "/" + Mc->GetName() + "#" + std::to_string(k)).c_str(), Mm);
                }
            }
            else Log("Wrap material %s: no dynamic instance", WrapMaterial->GetName().c_str());
        }

        Board.clear();
        const uint32_t Parts = Api.BoardPartCount(Engine);
        for (uint32_t i = 0; i < Parts; i++)
        {
            SkBoardPart Part;
            if (!Api.BoardPart(Engine, i, &Part))
                continue;

            auto* Mesh = static_cast<UProceduralMeshComponent*>(
                Character->AddComponentByClass(UProceduralMeshComponent::StaticClass(), true, Identity(), false));
            if (!Mesh)
            {
                Log("Board: could not create a mesh component");
                return;
            }

            std::vector<FVector> Vertices(Part.vertex_count), Normals(Part.vertex_count);
            std::vector<FVector2D> UV(Part.vertex_count);
            std::vector<int32> Triangles(Part.indices, Part.indices + Part.index_count);
            for (uint32_t v = 0; v < Part.vertex_count; v++)
            {
                Vertices[v] = FVector{ Part.positions[v * 3], Part.positions[v * 3 + 1], Part.positions[v * 3 + 2] };
                Normals[v] = FVector{ Part.normals[v * 3], Part.normals[v * 3 + 1], Part.normals[v * 3 + 2] };
                UV[v] = FVector2D{ Part.uvs[v * 2], Part.uvs[v * 2 + 1] };
            }
            // Everything Fortnite's materials may read: white vertex colours (weapon materials use
            // them as masks; missing colours turned the wrap black), every UV channel, tangents.
            std::vector<FLinearColor> Colors(Part.vertex_count, FLinearColor{ 1, 1, 1, 1 });
            std::vector<FProcMeshTangent> Tangents(Part.vertex_count);
            for (uint32_t v = 0; v < Part.vertex_count; v++)
            {
                const FVector& N = Normals[v];
                FVector T = std::fabs(N.Z) < 0.9 ? FVector{ -N.Y, N.X, 0 } : FVector{ 1, 0, 0 };
                const double L = std::sqrt(T.X * T.X + T.Y * T.Y + T.Z * T.Z);
                if (L > 1e-6) T = FVector{ T.X / L, T.Y / L, T.Z / L };
                Tangents[v].TangentX = T;
                Tangents[v].bFlipTangentY = false;
            }
            Mesh->CreateMeshSection_LinearColor(0, View(Vertices), View(Triangles), View(Normals), View(UV),
                View(UV), View(UV), View(UV), View(Colors), View(Tangents), false, false);
            // (The held item's material and the wrap's own material used to go on the deck: the
            // first showed whatever the player held, the second renders black on this mesh.)
            if (Part.texture == 0)
            {
                Mesh->bForceMipStreaming = 1;
                Mesh->PrestreamTextures(3600.f, true, 0);
            }
            Board.push_back({ Mesh, Part.bone, Part.texture });
        }
        ApplyBoardMaterials(Character);
        Log("Board: %zu pieces, deck %s", Board.size(), bBoardWrap && WrapDeck ? "Fortnite wrap" : "Skate 3 graphic");
    }

    void Clear()
    {
        Body = nullptr;
        Poser = nullptr;
        Mapped.clear();
        Followers.clear();
        Board.clear();
        bLeading = false;
        // BoardMaterials stay: they are kept alive (KeepAlive) and reused by the next board.
    }
}

bool Avatar::Begin(ACharacter* Character)
{
    Clear();
    Body = Character->Mesh;
    if (!Body || !Body->GetSkinnedAsset())
    {
        Log("Avatar: character has no body mesh");
        return false;
    }

    if (!BuildRetarget())
    {
        Clear();
        return false;
    }

    Poser = static_cast<UPoseableMeshComponent*>(
        Character->AddComponentByClass(UPoseableMeshComponent::StaticClass(), true, Identity(), false));
    if (!Poser)
    {
        Log("Avatar: could not create the pose component");
        Clear();
        return false;
    }
    Poser->K2_AttachToComponent(Body, FName(), EAttachmentRule::SnapToTarget, EAttachmentRule::SnapToTarget, EAttachmentRule::SnapToTarget, false);
    Poser->SetSkinnedAssetAndUpdate(Body->GetSkinnedAsset(), true);
    Poser->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    Poser->SetHiddenInGame(true, false);
    // A component created at runtime may start inactive with its tick off; the poser only
    // applies new bone transforms (and the body only copies them) when it ticks.
    Poser->Activate(true);
    Poser->SetComponentTickEnabled(true);
    Log("Avatar: poser active %d tick %d", Poser->IsActive(), Poser->IsComponentTickEnabled());

    // Fortnite characters are layered: Mesh is an invisible base skeleton and the visible outfit
    // parts (body, head, ...) copy its pose with their own animation. Every part that has the
    // character skeleton (a pelvis bone) follows the poser directly instead.
    const FName PelvisName = Name("pelvis");
    TArray<UActorComponent*> Components = Character->K2_GetComponentsByClass(USkinnedMeshComponent::StaticClass());
    std::string Parts;
    for (int i = 0; i < Components.Num(); i++)
    {
        auto* Skinned = static_cast<USkinnedMeshComponent*>(Components[i]);
        if (!Skinned || Skinned == Poser || !Skinned->GetSkinnedAsset())
            continue;
        if (Skinned != Body && Skinned->GetBoneIndex(PelvisName) < 0)
            continue;  // props with their own skeleton (back bling straps, capes) keep animating
        Followers.push_back({ Skinned, Skinned->LeaderPoseComponent.Get(), Skinned->Index });
        Parts += Skinned->GetName() + " ";
    }
    Log("Avatar: %zu mesh parts follow the skater pose: %s", Followers.size(), Parts.c_str());

    SkateBones.resize(size_t(Api.BoneCount(Engine)) * 16);
    Update(Character);
    SetLeading(true);
    BuildBoard(Character);
    return true;
}

void Avatar::TraceTrick(const char* State, const float* Velocity, float Yaw)
{
    static int BoardRoot = -2, Hips = -2;
    static int Frames = -1;  // frames left to log after the Boneless phase
    if (BoardRoot == -2) { BoardRoot = SkateBone("SKATEBOARD_ROOT"); Hips = SkateBone("HIPS"); }
    if (BoardRoot < 0 || Hips < 0 || SkateBones.empty()) return;
    const bool bBoneless = std::strcmp(State, "Boneless") == 0;
    if (bBoneless) Frames = 36;
    else if (Frames > 0) Frames--;
    if (Frames <= 0) return;
    auto At = [&](int i) { return Mat4::FromArray(&SkateBones[size_t(i) * 16]).Translation(); };
    const Vec3 B = At(BoardRoot), P = At(Hips);
    const float R = Yaw * 3.14159265f / 180.f, Fx = std::cos(R), Fy = std::sin(R);
    const float Dx = B.X - P.X, Dy = B.Y - P.Y;
    Log("[nocomply] %-16s board vs hips: forward %.0f right %.0f up %.0f cm | board speed %.0f cm/s",
        State, Dx * Fx + Dy * Fy, Dx * Fy - Dy * Fx, B.Z - P.Z,
        std::sqrt(Velocity[0] * Velocity[0] + Velocity[1] * Velocity[1] + Velocity[2] * Velocity[2]));
}

void Avatar::Update(ACharacter* Character)
{
    if (!Poser || !Body)
        return;

    Api.GetBones(Engine, SkateBones.data(), uint32_t(SkateBones.size() / 16));
    auto Bone = [&](uint32_t i) { return Mat4::FromArray(&SkateBones[size_t(i) * 16]); };

    for (const MappedBone& M : Mapped)
        Poser->SetBoneTransformByName(M.FnName, ToTransform(Bone(M.SkateIndex) * M.Offset), EBoneSpaces::WorldSpace);

    for (const BoardPiece& Piece : Board)
        Piece.Mesh->K2_SetWorldTransform(ToTransform(Bone(Piece.Bone)), false, nullptr, true);

    // Diagnostics: the board far from the carrying hand (reported "holding the board" glitch).
    {
        static int HandR = -2, BoardRoot = -2, Hips = -2;
        if (HandR == -2) { HandR = SkateBone("RIGHTHAND"); BoardRoot = SkateBone("SKATEBOARD_ROOT"); Hips = SkateBone("HIPS"); }
        static ULONGLONG LastFar = 0;
        if (HandR >= 0 && BoardRoot >= 0 && Hips >= 0)
        {
            const Vec3 H = Bone(uint32_t(HandR)).Translation(), B = Bone(uint32_t(BoardRoot)).Translation(), P = Bone(uint32_t(Hips)).Translation();
            const float Far = (H - B).Length();
            const ULONGLONG Now = GetTickCount64();
            // Only while the board is up at hip height (carried or grabbed), not under the feet.
            if (Far > 60.f && std::fabs(B.Z - P.Z) < 50.f && Now - LastFar > 500)
            {
                LastFar = Now;
                Log("[board] %.0f cm from right hand: hand (%.0f %.0f %.0f) board (%.0f %.0f %.0f) hips (%.0f %.0f %.0f)",
                    Far, H.X, H.Y, H.Z, B.X, B.Y, B.Z, P.X, P.Y, P.Z);
            }
        }
    }

    // Emotes: hand the body back to Fortnite's animation while an emote montage plays.
    UAnimInstance* Anim = Body->GetAnimInstance();
    UAnimMontage* Montage = Anim ? Anim->GetCurrentActiveMontage() : nullptr;
    std::string MontageName = Montage ? Montage->GetName() : "";
    if (MontageName != LastMontage)
    {
        Log("Montage now: %s", MontageName.empty() ? "(none)" : MontageName.c_str());
        LastMontage = MontageName;
    }
    std::string Lower = MontageName;
    for (char& C : Lower) C = char(tolower(C));
    const bool bEmote = Lower.find("emote") != std::string::npos || Lower.find("dance") != std::string::npos;
    if (bEmote && bLeading)
    {
        SetLeading(false);
        Log("Emote playing: showing Fortnite animation while still skating");
    }
    else if (!bEmote && !bLeading)
    {
        SetLeading(true);
    }
    RelinkParts(Character);

    // Once a second: where the pelvis should be, where the poser has it, where the body shows it.
    static float Last = 0;
    const float Now = float(GetTickCount64() % 100000000) / 1000.f;
    if (Now - Last >= 1.f && !Mapped.empty())
    {
        Last = Now;
        const MappedBone& Root = Mapped.front();
        const Vec3 Want = (Bone(Root.SkateIndex) * Root.Offset).Translation();
        const FTransform Got = Poser->GetBoneTransformByName(Root.FnName, EBoneSpaces::WorldSpace);
        const FVector Shown = Body->GetSocketLocation(Root.FnName);
        Log("[avatar] %s want (%.0f %.0f %.0f) poser (%.0f %.0f %.0f) body (%.0f %.0f %.0f) leading %d tick %d",
            Root.FnName.ToString().c_str(), Want.X, Want.Y, Want.Z, Got.Translation.X, Got.Translation.Y, Got.Translation.Z,
            Shown.X, Shown.Y, Shown.Z, bLeading, Poser->IsComponentTickEnabled());
        if (!Board.empty())
        {
            UProceduralMeshComponent* Deck = Board.front().Mesh;
            const FVector At = Deck->K2_GetComponentLocation();
            const Vec3 Bw = Bone(Board.front().Bone).Translation();
            Log("[avatar] board piece 0 at (%.0f %.0f %.0f), bone (%.0f %.0f %.0f), visible %d, sections %d",
                At.X, At.Y, At.Z, Bw.X, Bw.Y, Bw.Z, Deck->IsVisible(), Deck->GetNumSections());
        }
    }
}

void Avatar::End(ACharacter* Character)
{
    if (Character && Body)
    {
        if (bLeading)
            SetLeading(false);
        for (const BoardPiece& Piece : Board)
            Piece.Mesh->K2_DestroyComponent(Character);
        if (Poser)
            Poser->K2_DestroyComponent(Character);
    }
    Clear();
}

void Avatar::SetBoardWrap(ACharacter* Character, bool bWrap)
{
    bBoardWrap = bWrap;
    if (Character && !Board.empty()) ApplyBoardMaterials(Character);
}

bool Avatar::BoardWrap() { return bBoardWrap; }
void Avatar::SetWrapMaterialMode(bool bOn) { bWrapMaterialMode = bOn; }

// Wrap survey (mod_settings wrap_survey 1): every weapon wrap in the game (asset registry), loaded a few per
// frame, with the texture the board would show, logged as "[wrap survey]". For checking the choice works for
// all wraps, not just the equipped one.
namespace
{
    bool bSurveyWanted = false, bSurveyListed = false;
    size_t SurveyNext = 0;
    struct GetAssetsParams { FTopLevelAssetPath ClassPathName; TArray<FAssetData> OutAssetData; bool bSearchSubClasses; bool ReturnValue; uint8 Pad[6]; };
    GetAssetsParams* SurveyList = nullptr;   // kept (the engine owns its array)
    int SurveyLoaded = 0, SurveyChosen = 0, SurveyNone = 0;

    UObject* RegistryHelpers()
    {
        static UClass* C = UObject::FindClassFast("AssetRegistryHelpers");
        return C ? C->ClassDefaultObject : nullptr;
    }

    UObject* CallGetAsset(const FAssetData& Data)
    {
        static UFunction* F = nullptr;
        UObject* H = RegistryHelpers();
        if (!F && H) F = H->Class->GetFunction("AssetRegistryHelpers", "GetAsset");
        if (!F || !H) return nullptr;
        struct { FAssetData In; UObject* Ret; } P{};
        memcpy(&P.In, &Data, sizeof(FAssetData));
        const auto Flags = F->FunctionFlags;
        F->FunctionFlags |= 0x400;
        H->ProcessEvent(F, &P);
        F->FunctionFlags = Flags;
        return P.Ret;
    }
}

void Avatar::RequestWrapSurvey() { bSurveyWanted = true; bDumpWrap = true; }

bool Avatar::WrapSurveyStep(int Budget)
{
    if (!bSurveyWanted) return false;
    if (!bSurveyListed)
    {
        bSurveyListed = true;
        UObject* H = RegistryHelpers();
        UFunction* GetReg = H ? H->Class->GetFunction("AssetRegistryHelpers", "GetAssetRegistry") : nullptr;
        UFunction* GetByClass = UObject::FindObject<UFunction>("Function AssetRegistry.AssetRegistry.GetAssetsByClass");
        if (!GetReg || !GetByClass) { Log("[wrap survey] asset registry functions not found"); bSurveyWanted = false; return false; }
        struct { FScriptInterface Ret; } R{};
        auto F0 = GetReg->FunctionFlags; GetReg->FunctionFlags |= 0x400; H->ProcessEvent(GetReg, &R); GetReg->FunctionFlags = F0;
        UObject* Registry = R.Ret.GetObjectRef();
        if (!Registry) { Log("[wrap survey] no asset registry"); bSurveyWanted = false; return false; }
        SurveyList = new GetAssetsParams{};
        SurveyList->ClassPathName.PackageName = Name("/Script/FortniteGame");
        SurveyList->ClassPathName.AssetName = Name("AthenaItemWrapDefinition");
        SurveyList->bSearchSubClasses = true;
        auto F1 = GetByClass->FunctionFlags; GetByClass->FunctionFlags |= 0x400; Registry->ProcessEvent(GetByClass, SurveyList); GetByClass->FunctionFlags = F1;
        Log("[wrap survey] %d weapon wraps in the asset registry", SurveyList->OutAssetData.Num());
        return true;
    }
    constexpr uintptr_t MaterialOffset = 0x7B0;
    static UClass* WrapClass = UObject::FindClassFast("AthenaItemWrapDefinition");
    for (int n = 0; n < Budget && SurveyNext < size_t(SurveyList->OutAssetData.Num()); n++, SurveyNext++)
    {
        UObject* Wrap = CallGetAsset(SurveyList->OutAssetData[int(SurveyNext)]);
        if (!Wrap || (WrapClass && !Wrap->IsA(WrapClass))) { Log("[wrap survey] #%zu did not load", SurveyNext); continue; }
        SurveyLoaded++;
        const auto& Soft = *reinterpret_cast<TSoftObjectPtr<UObject>*>(reinterpret_cast<uint8*>(Wrap) + MaterialOffset);
        UObject* Material = UKismetSystemLibrary::LoadAsset_Blocking(Soft);
        if (!Material || !Material->IsA(UMaterialInstance::StaticClass()))
        {
            Log("[wrap survey] %s: material %s", Wrap->GetName().c_str(), Material ? "is not a material instance" : "did not load");
            SurveyNone++;
            continue;
        }
        std::string List;
        UTexture* T = PickWrapTexture(static_cast<UMaterialInterface*>(Material), List);
        if (T) SurveyChosen++; else SurveyNone++;
        Log("[wrap survey] %s (%s): %s| %s", Wrap->GetName().c_str(), Material->GetName().c_str(), T ? T->GetName().c_str() : "NONE",
            List.c_str());
    }
    if (SurveyNext >= size_t(SurveyList->OutAssetData.Num()))
    {
        Log("[wrap survey] done: %d wraps loaded, %d with a deck texture, %d without", SurveyLoaded, SurveyChosen, SurveyNone);
        bSurveyWanted = false;
    }
    return true;
}
