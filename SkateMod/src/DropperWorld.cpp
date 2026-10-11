#include "DropperWorld.h"
#include "Share.h"
#include "DropperSeams.h"

#include "SDK/ProceduralMeshComponent_classes.hpp"

#include <algorithm>
#include <sstream>
#include <fstream>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

using namespace SDK;

namespace DropperWorld
{
    namespace
    {
        constexpr double Deg = 3.14159265358979323846 / 180.0;
        const std::string RootS = ModPath("skate-data\\dropper");
        const char* const Root = RootS.c_str();

        DropperData::Library Lib;
        bool bLoaded = false;
        std::vector<Placed> Objects;
        int NextHandle = 1;
        uint64_t Gen = 1;

        // Live Unreal objects of a placed object, remembered by GObjects slot (the world may
        // destroy them: a new match, a level change).
        struct Visual
        {
            AActor* Actor = nullptr;
            int32 Index = -1;
            AActor* Get() const
            {
                if (!Actor || Index < 0 || Index >= UObject::GObjects->Num()) return nullptr;
                return UObject::GObjects->GetByIndex(Index) == Actor ? Actor : nullptr;
            }
        };
        std::map<int, Visual> Visuals;

        template <typename T>
        TArray<T> View(std::vector<T>& V) { return TArray<T>(V.data(), int32(V.size()), int32(V.size())); }

        FName Name(const char* Text)
        {
            std::wstring Wide(Text, Text + strlen(Text));
            return UKismetStringLibrary::Conv_StringToName(FString(Wide.c_str()));
        }

        // ---- Materials ----------------------------------------------------------------------
        // Fortnite's cooked materials only: a lit opaque (or masked, for Skate 3's alpha-tested
        // materials) surface material whose instances take a "Diffuse" texture. The best match
        // is chosen once and logged; each Skate 3 material becomes a dynamic instance of it.
        UMaterialInterface* Parents[2] = {};   // [opaque, masked]
        UMaterialInterface* PassThrough[2] = {};
        bool bPicked = false;
        FName DiffuseParam, NormalParam;
        bool bHasNormal = false;
        std::map<std::string, UTexture2D*> Textures;
        std::map<std::string, UMaterialInstanceDynamic*> Mids;

        UMaterial* BaseOf(UMaterialInterface* M)
        {
            for (int Depth = 0; M && Depth < 16; Depth++)
            {
                if (M->IsA(UMaterial::StaticClass())) return static_cast<UMaterial*>(M);
                if (!M->IsA(UMaterialInstance::StaticClass())) return nullptr;
                M = static_cast<UMaterialInstance*>(M)->Parent;
            }
            return nullptr;
        }

        int Score(const std::string& Name)
        {
            static const char* Bad[] = { "foliage", "tree", "grass", "water", "glass", "holo", "decal", "damage", "destroy", "character",
                                         "hair", "skin", "eye", "_ui", "ui_", "weapon", "vehicle", "sky", "cloud", "vfx", "fx_", "_fx",
                                         "particle", "transl", "emiss", "glow", "screen", "light", "landscape", "terrain", "blend", "wpo",
                                         "wind", "pickup", "outline", "preview", "ghost", "edit", "build", "trap", "mask", "cable" };
            static const char* Good[] = { "prop", "concrete", "metal", "wood", "env", "master", "standard", "basic", "simple", "opaque" };
            std::string L = Name;
            for (char& C : L) C = char(tolower(C));
            int S = 0;
            for (const char* B : Bad) if (L.find(B) != std::string::npos) S -= 10;
            for (const char* G : Good) if (L.find(G) != std::string::npos) S += 3;
            return S;
        }

        // A texture streamed as a virtual texture: such sampler slots refuse an ordinary texture
        // (the earlier lit attempt kept Fortnite's own wood because of that). UTexture +0xFE bit 2.
        bool IsVirtual(const UTexture* T)
        {
            return T && (reinterpret_cast<const uint8*>(T)[0xFE] & 0x04) != 0;
        }

        std::vector<UMaterialInstance*> Lit[2];   // ranked lit candidates [opaque, masked]
        int LitChoice[2] = { -1, -1 };              // the first candidate that took a texture (-2 = none did)
        UTexture* FlatNormal = nullptr;             // Fortnite's FlatNormalBlue, for candidates with a normal map slot

        void PickParents()
        {
            if (bPicked) return;
            bPicked = true;
            struct Candidate { UMaterialInstance* Mi; int Score; std::string Info; };
            std::vector<Candidate> Found[2];
            UClass* MicClass = UObject::FindClassFast("MaterialInstanceConstant");
            const int32 N = UObject::GObjects->Num();
            for (int32 i = 0; i < N; i++)
            {
                UObject* O = UObject::GObjects->GetByIndex(i);
                if (!O || O->IsDefaultObject()) continue;
                if (!FlatNormal && O->IsA(UTexture::StaticClass()) && O->GetName() == "FlatNormalBlue") FlatNormal = static_cast<UTexture*>(O);
                if (!MicClass || !O->IsA(MicClass)) continue;
                auto* Mi = static_cast<UMaterialInstance*>(O);
                bool bDiffuse = false, bNormals = false, bOther = false, bVirtual = false;
                for (const FTextureParameterValue& P : Mi->TextureParameterValues)
                {
                    const std::string Pn = P.ParameterInfo.Name.ToString();
                    if (Pn == "Diffuse") { bDiffuse = true; bVirtual = IsVirtual(P.ParameterValue); }
                    else if (Pn == "Normals") bNormals = true;
                    else bOther = true;   // SpecularMasks and the rest would keep Fortnite's own detail
                }
                if (!bDiffuse || bVirtual) continue;
                UMaterial* Base = BaseOf(Mi);
                if (!Base || uint8(Base->MaterialDomain) != 0) continue;            // surface materials only
                const uint8 Blend = uint8(Base->BlendMode);                         // 0 opaque, 1 masked
                if (Blend > 1) continue;
                const std::string Name = Mi->GetName(), BaseName = Base->GetName();
                int S = Score(Name) + Score(BaseName) - (bNormals ? 2 : 0) - (bOther ? 8 : 0);
                // The one known to show any texture as given (placed objects, the wraps tested): first when
                // loaded. Otherwise equal scores came out in load order, so a session could pick a wooden door
                // material that kept its own wood (2026-10-11: the board showed wood instead of the wrap).
                if (Name == "MI_FORT_Concrete_05") S += 100;
                Found[Blend].push_back({ Mi, S, Name + " (base " + BaseName + (bNormals ? ", normals" : "") + (bOther ? ", more textures" : "") + ")" });
            }
            for (int B = 0; B < 2; B++)
            {
                std::sort(Found[B].begin(), Found[B].end(), [](const Candidate& A, const Candidate& C)
                          { return A.Score != C.Score ? A.Score > C.Score : A.Info < C.Info; });   // same order every session
                Log("[dropper] %s lit material candidates (non-virtual Diffuse): %zu", B ? "masked" : "opaque", Found[B].size());
                for (size_t k = 0; k < Found[B].size() && k < 24; k++)
                {
                    if (k < 8) Log("[dropper]   %d %s", Found[B][k].Score, Found[B][k].Info.c_str());
                    Lit[B].push_back(Found[B][k].Mi);
                    KeepAlive(Found[B][k].Mi);
                }
            }
            DiffuseParam = Name("Diffuse");
            NormalParam = Name("Normals");
            Log("[dropper] flat normal texture %s", FlatNormal ? "found" : "NOT found");
            // Fallback when no lit candidate takes the texture: the unlit pass-through (clamped, no shadows).
            PassThrough[0] = FindMaterialByName("Widget3DPassThrough_Opaque");
            PassThrough[1] = FindMaterialByName("Widget3DPassThrough_Masked");
            if (!PassThrough[1]) PassThrough[1] = PassThrough[0];
        }

        // A lit instance of candidate C with texture D, or nullptr when the material refuses it.
        UMaterialInstanceDynamic* TryLit(UObject* Context, UMaterialInstance* C, UTexture2D* D, UTexture2D* Nrm)
        {
            UMaterialInstanceDynamic* Mid = UKismetMaterialLibrary::CreateDynamicMaterialInstance(Context, C, FName(), EMIDCreationFlags::None);
            if (!Mid) return nullptr;
            Mid->SetTextureParameterValue(DiffuseParam, D);
            if (Mid->K2_GetTextureParameterValue(DiffuseParam) != D) return nullptr;
            bool bHasNormals = false;
            for (const FTextureParameterValue& P : C->TextureParameterValues)
                if (P.ParameterInfo.Name.ToString() == "Normals") bHasNormals = true;
            if (bHasNormals)
            {
                // Skate 3's own normal map when the material has one, else flat (never Fortnite's).
                UTexture* Use = Nrm ? static_cast<UTexture*>(Nrm) : FlatNormal;
                if (Use) Mid->SetTextureParameterValue(NormalParam, Use);
            }
            return Mid;
        }

        UTexture2D* Texture(UObject* Context, const std::string& Id)
        {
            if (Id.empty()) return nullptr;
            auto It = Textures.find(Id);
            if (It != Textures.end()) return It->second;
            const std::string Path = Lib.TexturePath(Id);
            std::wstring Wide(Path.begin(), Path.end());
            UTexture2D* T = UKismetRenderingLibrary::ImportFileAsTexture2D(Context, FString(Wide.c_str()));
            if (T) KeepAlive(T);
            else Log("[dropper] texture %s did not load", Path.c_str());
            Textures[Id] = T;
            return T;
        }

        UMaterialInterface* MaterialFor(UObject* Context, const DropperData::Material& M)
        {
            PickParents();
            const int Kind = (M.Flags & 1) ? 1 : 0;
            const std::string Key = std::to_string(Kind) + "|" + M.Diffuse + "|" + M.Normal;
            auto It = Mids.find(Key);
            if (It != Mids.end()) return It->second;
            UTexture2D* D = Texture(Context, M.Diffuse);
            UMaterialInstanceDynamic* Mid = nullptr;
            // Lit: Fortnite's light and shadows on the piece, the texture tiling as in Skate 3 (most
            // pieces' UVs run past 0..1). The first candidate that keeps the texture is used for all.
            if (D && LitChoice[Kind] != -2)
            {
                UTexture2D* Nrm = M.Normal.empty() ? nullptr : Texture(Context, M.Normal);
                if (LitChoice[Kind] >= 0) Mid = TryLit(Context, Lit[Kind][size_t(LitChoice[Kind])], D, Nrm);
                else
                {
                    for (size_t c = 0; c < Lit[Kind].size() && !Mid; c++)
                        if ((Mid = TryLit(Context, Lit[Kind][c], D, Nrm)))
                        {
                            LitChoice[Kind] = int(c);
                            Log("[dropper] lit %s material: %s", Kind ? "masked" : "opaque", Lit[Kind][c]->GetFullName().c_str());
                        }
                    if (!Mid)
                    {
                        LitChoice[Kind] = -2;
                        Log("[dropper] no lit %s material took a Skate 3 texture: unlit pass-through instead", Kind ? "masked" : "opaque");
                    }
                }
            }
            if (!Mid && PassThrough[Kind])
            {
                Mid = UKismetMaterialLibrary::CreateDynamicMaterialInstance(Context, PassThrough[Kind], FName(), EMIDCreationFlags::None);
                if (Mid && D) Mid->SetTextureParameterValue(Name("SlateUI"), D);
            }
            if (Mid) KeepAlive(Mid);
            Mids[Key] = Mid;
            return Mid;
        }

        // ---- Meshes -------------------------------------------------------------------------
        // Unreal-space mesh sections of a model, built once per model and shared by every copy.
        struct Section
        {
            uint32_t Material;
            std::vector<FVector> Vertices, Normals;
            std::vector<FVector2D> Uv0, Uv1;
            std::vector<int32> Triangles;
            std::vector<FLinearColor> Colors;
            std::vector<FProcMeshTangent> Tangents;
        };
        std::map<std::string, std::shared_ptr<std::vector<Section>>> Sections;

        std::shared_ptr<std::vector<Section>> SectionsOf(const std::string& Id, const DropperData::Object& O)
        {
            auto It = Sections.find(Id);
            if (It != Sections.end()) return It->second;
            auto Out = std::make_shared<std::vector<Section>>();
            for (const DropperData::Mesh& M : O.Meshes)
            {
                Section S;
                S.Material = M.Material;
                const uint32_t N = M.VertexCount();
                for (uint32_t v = 0; v < N; v++)
                {
                    const float* V = &M.Vertices[size_t(v) * 13];
                    S.Vertices.push_back(FromSkate(V[0], V[1], V[2]));
                    // Directions swap axes too (no scale).
                    S.Normals.push_back(FVector{ V[3], V[5], V[4] });
                    FProcMeshTangent T;
                    T.TangentX = FVector{ V[6], V[8], V[7] };
                    T.bFlipTangentY = true;   // the axis swap mirrors the tangent frame
                    S.Tangents.push_back(T);
                    S.Uv0.push_back(FVector2D{ V[9], V[10] });
                    S.Uv1.push_back(FVector2D{ V[11], V[12] });
                    S.Colors.push_back(FLinearColor{ 1, 1, 1, 1 });
                }
                // Skate 3's (Xbox 360, clockwise front faces) index order already faces outward after
                // the axis swap: reversing it turned every object inside out (seen 2026-10-09).
                for (uint32_t Ix : M.Indices) S.Triangles.push_back(int32(Ix));
                Out->push_back(std::move(S));
            }
            Sections[Id] = Out;
            return Out;
        }

        // ---- Decals: environmentParkDecal materials' 'decal' texture, drawn over the piece
        // (Tools/dropper_decals.py -> decals.txt: object, material, texture, uv set) ----
        struct DecalLayer { uint32_t Material = 0; std::string Texture; int UvSet = 1; };
        std::map<std::string, std::vector<DecalLayer>> Decals;
        bool bDecalsLoaded = false;
        const std::vector<DecalLayer>* DecalsOf(const std::string& Id)
        {
            if (!bDecalsLoaded)
            {
                bDecalsLoaded = true;
                std::ifstream In(std::string(Root) + "\\decals.txt");
                std::string Line;
                while (std::getline(In, Line))
                {
                    if (Line.empty() || Line[0] == '#') continue;
                    std::istringstream S(Line);
                    std::string Obj;
                    DecalLayer L;
                    if (S >> Obj >> L.Material >> L.Texture >> L.UvSet) Decals[Obj].push_back(L);
                }
            }
            auto It = Decals.find(Id);
            return It == Decals.end() ? nullptr : &It->second;
        }

        // The decal over its section: the same triangles a hair (1 mm) out along the normals, the
        // decal's uv set as uv 0, alpha tested.
        void AddDecalSections(AActor* A, UProceduralMeshComponent* Mesh, const std::string& Id, std::vector<Section>& Secs, int32 First)
        {
            const std::vector<DecalLayer>* Layers = DecalsOf(Id);
            if (!Layers) return;
            int32 Index = First;
            for (const DecalLayer& L : *Layers)
            {
                DropperData::Material M;
                M.Flags = 1;
                M.Diffuse = L.Texture;
                UMaterialInterface* Mi = MaterialFor(A, M);
                for (Section& S : Secs)
                {
                    if (S.Material != L.Material) continue;
                    std::vector<FVector> Out(S.Vertices.size());
                    for (size_t v = 0; v < Out.size(); v++)
                        Out[v] = FVector{ S.Vertices[v].X + S.Normals[v].X * 0.1, S.Vertices[v].Y + S.Normals[v].Y * 0.1, S.Vertices[v].Z + S.Normals[v].Z * 0.1 };
                    std::vector<FVector2D>& Uv = L.UvSet ? S.Uv1 : S.Uv0;
                    Mesh->CreateMeshSection_LinearColor(Index, View(Out), View(S.Triangles), View(S.Normals), View(Uv), View(Uv),
                                                        View(Uv), View(Uv), View(S.Colors), View(S.Tangents), false, false);
                    if (Mi) Mesh->SetMaterial(Index, Mi);
                    Index++;
                }
            }
        }

        FTransform Identity()
        {
            FTransform T{};
            T.Rotation.W = 1;
            T.Scale3D = FVector{ 1, 1, 1 };
            return T;
        }

        // Skate 3's collision triangles as an invisible section with Unreal collision, so the
        // object blocks Fortnite movement (walking, the server's copy) where the skate engine
        // collides with it.
        struct CollisionMesh { std::vector<FVector> Vertices; std::vector<int32> Triangles; };
        std::map<std::string, std::shared_ptr<CollisionMesh>> CollisionMeshes;

        void AddCollisionSection(UProceduralMeshComponent* Mesh, const std::string& Id, const DropperData::Object& O, int32 Index)
        {
            auto& C = CollisionMeshes[Id];
            if (!C)
            {
                C = std::make_shared<CollisionMesh>();
                for (const DropperData::CollisionTriangle& T : O.Collision)
                {
                    for (int k = 0; k < 3; k++) C->Vertices.push_back(FromSkate(T.V[k * 3], T.V[k * 3 + 1], T.V[k * 3 + 2]));
                    const int32 B = int32(C->Vertices.size()) - 3;
                    C->Triangles.insert(C->Triangles.end(), { B, B + 1, B + 2 });
                }
            }
            if (C->Triangles.empty()) return;
            std::vector<FVector> Normals(C->Vertices.size(), FVector{ 0, 0, 1 });
            std::vector<FVector2D> Uv(C->Vertices.size(), FVector2D{});
            std::vector<FLinearColor> Colors(C->Vertices.size(), FLinearColor{ 1, 1, 1, 1 });
            std::vector<FProcMeshTangent> Tangents(C->Vertices.size());
            Mesh->CreateMeshSection_LinearColor(Index, View(C->Vertices), View(C->Triangles), View(Normals), View(Uv), View(Uv),
                                                View(Uv), View(Uv), View(Colors), View(Tangents), true, false);
            Mesh->SetMeshSectionVisible(Index, false);
        }

        void SetSolid(UProceduralMeshComponent* Mesh, bool bSolid)
        {
            if (!Mesh) return;
            Mesh->SetCollisionProfileName(Name(bSolid ? "BlockAll" : "NoCollision"), true);
            Mesh->SetCollisionEnabled(bSolid ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
        }

        UProceduralMeshComponent* MeshOf(AActor* A)
        {
            return A ? static_cast<UProceduralMeshComponent*>(A->GetComponentByClass(UProceduralMeshComponent::StaticClass())) : nullptr;
        }

        // A character standing on a Movable component sends the server its position relative to
        // that component (a movement base). The dropped pieces are this process's own actors the
        // other side cannot resolve, so the relative offset was taken as a map position: leaving
        // skate mode on top of a piece put the pawn near the map origin, under the ground. At rest
        // the pieces are Static (positions then go as world positions); Movable only while they move.
        void SetStill(AActor* A, bool bStill)
        {
            if (UProceduralMeshComponent* M = MeshOf(A))
            {
                const EComponentMobility Want = bStill ? EComponentMobility::Static : EComponentMobility::Movable;
                if (M->Mobility != Want) M->SetMobility(Want);
            }
        }
        // Moves an actor whichever its mobility (a Static one is made Movable for the move and back).
        void PlaceActor(AActor* A, const FVector& Position, const FRotator& Rotation)
        {
            UProceduralMeshComponent* M = MeshOf(A);
            const bool bStatic = M && M->Mobility != EComponentMobility::Movable;
            if (bStatic) M->SetMobility(EComponentMobility::Movable);
            A->K2_SetActorLocationAndRotation(Position, Rotation, false, nullptr, true);
            if (bStatic) M->SetMobility(EComponentMobility::Static);
        }

        // ---- Display modes (82841ae0) ---------------------------------------------------------
        std::map<int, int> Modes;   // handle -> mode (0 = normal, not stored)
        struct Rest { bool bMoving = false; };
        std::map<int, Rest> Resting;   // last pose read back per physical object
        struct Saved { FVector Position; Quat Rotation; };
        std::map<int, Saved> MarkerSet;   // session marker DMOs -> their frame when added
        const size_t MarkerMax = 50;      // 828745A8: count < 0x32

        // The visual indicator colour drawn over a piece: Skate 3's own texture on the translucent
        // pass-through material, as the mesh's overlay.
        UMaterialInterface* Indicator(UObject* Context, const char* Colour)
        {
            static std::map<std::string, UMaterialInterface*> Made;
            auto It = Made.find(Colour);
            if (It != Made.end()) return It->second;
            UMaterialInstanceDynamic* Mid = nullptr;
            static UMaterialInterface* Translucent = FindMaterialByName("Widget3DPassThrough_Translucent");
            const std::string Path = ModPath("skate-data\\hud\\vitextures\\visual_indicator_") + Colour + ".png";
            std::wstring Wide(Path.begin(), Path.end());
            UTexture2D* T = UKismetRenderingLibrary::ImportFileAsTexture2D(Context, FString(Wide.c_str()));
            if (Translucent && T && (Mid = UKismetMaterialLibrary::CreateDynamicMaterialInstance(Context, Translucent, FName(), EMIDCreationFlags::None)))
            {
                Mid->SetTextureParameterValue(Name("SlateUI"), T);
                KeepAlive(T);
                KeepAlive(Mid);
            }
            Log("[dropper] %s indicator: material %s, texture %s", Colour, Translucent ? "found" : "NOT found", T ? "loaded" : "NOT loaded");
            Made[Colour] = Mid;
            return Mid;
        }

        void ApplyMode(AActor* A, int Mode)
        {
            UProceduralMeshComponent* Mesh = MeshOf(A);
            if (!Mesh) return;
            const char* Colour = Mode == 1 ? "green" : Mode == 5 ? "red" : nullptr;
            Mesh->SetOverlayMaterial(Colour ? Indicator(A, Colour) : nullptr);
        }

        AActor* Spawn(UWorld* World, const Placed& P)
        {
            auto O = Lib.Get(P.Id);
            if (!O || !World) return nullptr;
            FTransform T = Identity();
            T.Translation = P.Position;
            T.Rotation = FQuat{ P.Rotation.X, P.Rotation.Y, P.Rotation.Z, P.Rotation.W };
            AActor* A = UGameplayStatics::BeginDeferredActorSpawnFromClass(World, AActor::StaticClass(), T,
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, ESpawnActorScaleMethod::MultiplyWithRoot);
            if (A) A = UGameplayStatics::FinishSpawningActor(A, T, ESpawnActorScaleMethod::MultiplyWithRoot);
            if (!A) { Log("[dropper] could not spawn an actor for %s", P.Id.c_str()); return nullptr; }
            auto* Mesh = static_cast<UProceduralMeshComponent*>(A->AddComponentByClass(UProceduralMeshComponent::StaticClass(), false, Identity(), false));
            if (!Mesh) { Log("[dropper] could not add a mesh to %s", P.Id.c_str()); return A; }
            // Cook the collision on a worker thread: cooked here on the game thread, a big piece
            // (mega ramps) froze the frame for 100-800 ms each time it spawned (2026-10-10 log).
            // The server's copies (MirrorOnServer) stay synchronous: a player may stand on them.
            Mesh->bUseAsyncCooking = true;
            PlaceActor(A, P.Position, ToRotator(P.Rotation));
            auto Secs = SectionsOf(P.Id, *O);
            for (size_t i = 0; i < Secs->size(); i++)
            {
                Section& S = (*Secs)[i];
                std::vector<FVector2D> Empty;
                Mesh->CreateMeshSection_LinearColor(int32(i), View(S.Vertices), View(S.Triangles), View(S.Normals), View(S.Uv0), View(S.Uv1),
                                                    View(S.Uv0), View(S.Uv0), View(S.Colors), View(S.Tangents), false, false);
                if (S.Material < O->Materials.size())
                    if (UMaterialInterface* M = MaterialFor(A, O->Materials[S.Material]))
                        Mesh->SetMaterial(int32(i), M);
            }
            AddCollisionSection(Mesh, P.Id, *O, int32(Secs->size()));
            AddDecalSections(A, Mesh, P.Id, *Secs, int32(Secs->size()) + 1);
            SetSolid(Mesh, !P.bGhost);
            Mesh->SetCastShadow(true);
            if (auto M = Modes.find(P.Handle); M != Modes.end()) ApplyMode(A, M->second);
            SetStill(A, !P.bGhost);
            return A;
        }

        void Destroy(int Handle)
        {
            auto It = Visuals.find(Handle);
            if (It == Visuals.end()) return;
            if (AActor* A = It->second.Get()) A->K2_DestroyActor();
            Visuals.erase(It);
        }

        void Show(UWorld* World, const Placed& P)
        {
            Destroy(P.Handle);
            if (AActor* A = Spawn(World, P)) Visuals[P.Handle] = { A, A->Index };
        }
    }

    UMaterialInstanceDynamic* LitMaterial(UObject* Context, UTexture2D* Diffuse)
    {
        if (!Diffuse) return nullptr;
        PickParents();
        if (LitChoice[0] == -2) return nullptr;
        UMaterialInstanceDynamic* Mid = nullptr;
        if (LitChoice[0] >= 0) Mid = TryLit(Context, Lit[0][size_t(LitChoice[0])], Diffuse, nullptr);
        else
            for (size_t c = 0; c < Lit[0].size() && !Mid; c++)
                if ((Mid = TryLit(Context, Lit[0][c], Diffuse, nullptr)))
                {
                    LitChoice[0] = int(c);
                    Log("[dropper] lit opaque material: %s", Lit[0][c]->GetFullName().c_str());
                }
        if (Mid) KeepAlive(Mid);
        return Mid;
    }

    Quat FromRotator(double Pitch, double Yaw, double Roll)
    {
        // FRotator::Quaternion
        const double SP = std::sin(Pitch * Deg * 0.5), CP = std::cos(Pitch * Deg * 0.5);
        const double SY = std::sin(Yaw * Deg * 0.5), CY = std::cos(Yaw * Deg * 0.5);
        const double SR = std::sin(Roll * Deg * 0.5), CR = std::cos(Roll * Deg * 0.5);
        Quat Q;
        Q.X = CR * SP * SY - SR * CP * CY;
        Q.Y = -CR * SP * CY - SR * CP * SY;
        Q.Z = CR * CP * SY - SR * SP * CY;
        Q.W = CR * CP * CY + SR * SP * SY;
        return Q;
    }

    FRotator ToRotator(const Quat& Q)
    {
        // FQuat::Rotator
        const double SingularityTest = Q.Z * Q.X - Q.W * Q.Y;
        const double YawY = 2.0 * (Q.W * Q.Z + Q.X * Q.Y);
        const double YawX = 1.0 - 2.0 * (Q.Y * Q.Y + Q.Z * Q.Z);
        const double Threshold = 0.4999995;
        FRotator R{};
        if (SingularityTest < -Threshold)
        {
            R.Pitch = -90.0;
            R.Yaw = std::atan2(YawY, YawX) / Deg;
            R.Roll = -R.Yaw - 2.0 * std::atan2(Q.X, Q.W) / Deg;
        }
        else if (SingularityTest > Threshold)
        {
            R.Pitch = 90.0;
            R.Yaw = std::atan2(YawY, YawX) / Deg;
            R.Roll = R.Yaw - 2.0 * std::atan2(Q.X, Q.W) / Deg;
        }
        else
        {
            R.Pitch = std::asin(2.0 * SingularityTest) / Deg;
            R.Yaw = std::atan2(YawY, YawX) / Deg;
            R.Roll = std::atan2(-2.0 * (Q.W * Q.X + Q.Y * Q.Z), 1.0 - 2.0 * (Q.X * Q.X + Q.Y * Q.Y)) / Deg;
        }
        return R;
    }

    FVector Rotate(const Quat& Q, const FVector& V)
    {
        // FQuat::RotateVector: v + 2w(q x v) + 2 q x (q x v)
        const double TX = 2.0 * (Q.Y * V.Z - Q.Z * V.Y), TY = 2.0 * (Q.Z * V.X - Q.X * V.Z), TZ = 2.0 * (Q.X * V.Y - Q.Y * V.X);
        return FVector{ V.X + Q.W * TX + (Q.Y * TZ - Q.Z * TY), V.Y + Q.W * TY + (Q.Z * TX - Q.X * TZ), V.Z + Q.W * TZ + (Q.X * TY - Q.Y * TX) };
    }

    Quat Mul(const Quat& A, const Quat& B)
    {
        Quat R;
        R.W = A.W * B.W - A.X * B.X - A.Y * B.Y - A.Z * B.Z;
        R.X = A.W * B.X + A.X * B.W + A.Y * B.Z - A.Z * B.Y;
        R.Y = A.W * B.Y - A.X * B.Z + A.Y * B.W + A.Z * B.X;
        R.Z = A.W * B.Z + A.X * B.Y - A.Y * B.X + A.Z * B.W;
        return R;
    }

    bool Load(std::string& Error)
    {
        if (bLoaded) return true;
        if (!Lib.Load(Root, Error)) return false;
        bLoaded = true;
        Log("[dropper] catalog: %zu objects", Lib.Catalog().size());
        return true;
    }

    DropperData::Library& Library() { return Lib; }

    int Add(UWorld* World, const std::string& Id, const FVector& Position, const Quat& Rotation, bool bGhost)
    {
        std::string E;
        if (!Load(E)) { Log("[dropper] %s", E.c_str()); return 0; }
        if (!Lib.Get(Id, &E)) { Log("[dropper] object %s: %s", Id.c_str(), E.c_str()); return 0; }
        Placed P;
        P.Handle = NextHandle++;
        P.Id = Id;
        P.Position = Position;
        P.Rotation = Rotation;
        P.HomePosition = Position;
        P.HomeRotation = Rotation;
        P.bGhost = bGhost;
        Objects.push_back(P);
        Show(World, Objects.back());
        if (!bGhost) Gen++;
        return P.Handle;
    }

    void Move(int Handle, const FVector& Position, const Quat& Rotation)
    {
        for (Placed& P : Objects)
        {
            if (P.Handle != Handle) continue;
            P.Position = Position;
            P.Rotation = Rotation;
            P.HomePosition = Position;   // the dropper moved it: its new placement
            P.HomeRotation = Rotation;
            auto V = Visuals.find(Handle);
            if (V != Visuals.end())
                if (AActor* A = V->second.Get())
                    PlaceActor(A, Position, ToRotator(Rotation));
            if (!P.bGhost) Gen++;
        }
    }

    void ShowAt(int Handle, const FVector& Position, const Quat& Rotation)
    {
        auto V = Visuals.find(Handle);
        if (V != Visuals.end())
            if (AActor* A = V->second.Get())
                PlaceActor(A, Position, ToRotator(Rotation));
    }

    void SetGhost(int Handle, bool bGhost)
    {
        for (Placed& P : Objects)
            if (P.Handle == Handle && P.bGhost != bGhost)
            {
                P.bGhost = bGhost;
                Gen++;
                auto V = Visuals.find(Handle);
                if (V != Visuals.end())
                {
                    SetSolid(MeshOf(V->second.Get()), !bGhost);
                    if (AActor* A = V->second.Get()) SetStill(A, !bGhost);   // carried: Movable; put down: Static
                }
            }
    }

    void SetDisplayMode(int Handle, int Mode)
    {
        auto Cur = Modes.find(Handle);
        if ((Cur == Modes.end() ? 0 : Cur->second) == Mode) return;
        if (Mode) Modes[Handle] = Mode;
        else Modes.erase(Handle);
        auto V = Visuals.find(Handle);
        if (V != Visuals.end())
            if (AActor* A = V->second.Get()) ApplyMode(A, Mode);
    }

    void Remove(int Handle)
    {
        Modes.erase(Handle);
        Resting.erase(Handle);
        MarkerSet.erase(Handle);
        Destroy(Handle);
        const size_t Before = Objects.size();
        Objects.erase(std::remove_if(Objects.begin(), Objects.end(), [&](const Placed& P) { return P.Handle == Handle; }), Objects.end());
        if (Objects.size() != Before) Gen++;
    }

    bool OverlapsOthers(int Handle, const std::vector<int>& Ignore, double InsetCm)
    {
        struct Box { FVector C, A[3]; double E[3]; };
        auto BoxOf = [&](const Placed& P, Box& B) -> bool
        {
            auto O = Lib.Get(P.Id);
            if (!O) return false;
            const FVector Lo = FromSkate(O->Min[0], O->Min[1], O->Min[2]), Hi = FromSkate(O->Max[0], O->Max[1], O->Max[2]);
            const FVector Mid{ (Lo.X + Hi.X) * 0.5, (Lo.Y + Hi.Y) * 0.5, (Lo.Z + Hi.Z) * 0.5 };
            const FVector R = Rotate(P.Rotation, Mid);
            B.C = FVector{ P.Position.X + R.X, P.Position.Y + R.Y, P.Position.Z + R.Z };
            B.A[0] = Rotate(P.Rotation, FVector{ 1, 0, 0 });
            B.A[1] = Rotate(P.Rotation, FVector{ 0, 1, 0 });
            B.A[2] = Rotate(P.Rotation, FVector{ 0, 0, 1 });
            B.E[0] = std::max(0.0, std::fabs(Hi.X - Lo.X) * 0.5 - InsetCm);
            B.E[1] = std::max(0.0, std::fabs(Hi.Y - Lo.Y) * 0.5 - InsetCm);
            B.E[2] = std::max(0.0, std::fabs(Hi.Z - Lo.Z) * 0.5 - InsetCm);
            return true;
        };
        auto Dt = [](const FVector& a, const FVector& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
        auto Cr = [](const FVector& a, const FVector& b) { return FVector{ a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X }; };
        // separating axis test of two oriented boxes
        auto Overlap = [&](const Box& P, const Box& Q)
        {
            const FVector D{ Q.C.X - P.C.X, Q.C.Y - P.C.Y, Q.C.Z - P.C.Z };
            auto Separated = [&](const FVector& Ax)
            {
                const double L = Dt(Ax, Ax);
                if (L < 1e-10) return false;
                double Rp = 0, Rq = 0;
                for (int i = 0; i < 3; i++) { Rp += P.E[i] * std::fabs(Dt(P.A[i], Ax)); Rq += Q.E[i] * std::fabs(Dt(Q.A[i], Ax)); }
                return std::fabs(Dt(D, Ax)) > Rp + Rq;
            };
            for (int i = 0; i < 3; i++) if (Separated(P.A[i]) || Separated(Q.A[i])) return false;
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if (Separated(Cr(P.A[i], Q.A[j]))) return false;
            return true;
        };
        // the box against a triangle (separating axes: the box's 3, the normal, the 9 edge crosses)
        auto BoxTri = [&](const Box& Bx, const FVector V[3])
        {
            FVector L[3];
            for (int k = 0; k < 3; k++)
            {
                const FVector D{ V[k].X - Bx.C.X, V[k].Y - Bx.C.Y, V[k].Z - Bx.C.Z };
                L[k] = FVector{ Dt(D, Bx.A[0]), Dt(D, Bx.A[1]), Dt(D, Bx.A[2]) };
            }
            auto Sep = [&](const FVector& Ax)
            {
                const double Ln = Dt(Ax, Ax);
                if (Ln < 1e-12) return false;
                const double P0 = Dt(L[0], Ax), P1 = Dt(L[1], Ax), P2 = Dt(L[2], Ax);
                const double R = Bx.E[0] * std::fabs(Ax.X) + Bx.E[1] * std::fabs(Ax.Y) + Bx.E[2] * std::fabs(Ax.Z);
                return std::min({ P0, P1, P2 }) > R || std::max({ P0, P1, P2 }) < -R;
            };
            const FVector E[3] = { FVector{ L[1].X - L[0].X, L[1].Y - L[0].Y, L[1].Z - L[0].Z },
                                   FVector{ L[2].X - L[1].X, L[2].Y - L[1].Y, L[2].Z - L[1].Z },
                                   FVector{ L[0].X - L[2].X, L[0].Y - L[2].Y, L[0].Z - L[2].Z } };
            const FVector U[3] = { FVector{ 1, 0, 0 }, FVector{ 0, 1, 0 }, FVector{ 0, 0, 1 } };
            for (const FVector& Ax : U) if (Sep(Ax)) return false;
            if (Sep(Cr(E[0], E[1]))) return false;
            for (const FVector& Ua : U) for (const FVector& Ed : E) if (Sep(Cr(Ua, Ed))) return false;
            return true;
        };
        const Placed* Me = Get(Handle);
        Box Mb;
        if (!Me || !BoxOf(*Me, Mb)) return false;
        const double Reach = Mb.E[0] + Mb.E[1] + Mb.E[2];
        for (const Placed& P : Objects)
        {
            if (P.Handle == Handle || P.bGhost || std::find(Ignore.begin(), Ignore.end(), P.Handle) != Ignore.end()) continue;
            Box Pb;
            if (!BoxOf(P, Pb)) continue;
            const double Dx = Pb.C.X - Mb.C.X, Dy = Pb.C.Y - Mb.C.Y, Dz = Pb.C.Z - Mb.C.Z;
            const double Far = Reach + Pb.E[0] + Pb.E[1] + Pb.E[2] + 2 * InsetCm;
            if (Dx * Dx + Dy * Dy + Dz * Dz > Far * Far) continue;
            auto O = Lib.Get(P.Id);
            // the other object's own collision triangles (a ramp's slope, not its box); its box if it has none
            if (!O || O->Collision.empty()) { if (Overlap(Mb, Pb)) return true; continue; }
            if (!Overlap(Mb, Pb)) continue;
            // the box in the other object's own space (its triangles need no transform), and that
            // box's bounds there to skip the triangles nowhere near it
            const Quat Inv{ -P.Rotation.X, -P.Rotation.Y, -P.Rotation.Z, P.Rotation.W };
            Box Lb;
            Lb.C = Rotate(Inv, FVector{ Mb.C.X - P.Position.X, Mb.C.Y - P.Position.Y, Mb.C.Z - P.Position.Z });
            for (int i = 0; i < 3; i++) { Lb.A[i] = Rotate(Inv, Mb.A[i]); Lb.E[i] = Mb.E[i]; }
            double Ext[3] = { 0, 0, 0 };
            for (int i = 0; i < 3; i++)
            {
                Ext[0] += std::fabs(Lb.A[i].X) * Lb.E[i]; Ext[1] += std::fabs(Lb.A[i].Y) * Lb.E[i]; Ext[2] += std::fabs(Lb.A[i].Z) * Lb.E[i];
            }
            const double Lo[3] = { Lb.C.X - Ext[0], Lb.C.Y - Ext[1], Lb.C.Z - Ext[2] }, Hi[3] = { Lb.C.X + Ext[0], Lb.C.Y + Ext[1], Lb.C.Z + Ext[2] };
            for (const DropperData::CollisionTriangle& T : O->Collision)
            {
                FVector V[3];
                for (int k = 0; k < 3; k++) V[k] = FromSkate(T.V[3 * k], T.V[3 * k + 1], T.V[3 * k + 2]);
                if (std::max({ V[0].X, V[1].X, V[2].X }) < Lo[0] || std::min({ V[0].X, V[1].X, V[2].X }) > Hi[0] ||
                    std::max({ V[0].Y, V[1].Y, V[2].Y }) < Lo[1] || std::min({ V[0].Y, V[1].Y, V[2].Y }) > Hi[1] ||
                    std::max({ V[0].Z, V[1].Z, V[2].Z }) < Lo[2] || std::min({ V[0].Z, V[1].Z, V[2].Z }) > Hi[2])
                    continue;
                if (BoxTri(Lb, V)) return true;
            }
        }
        return false;
    }

    bool Replace(UWorld* World, int Handle, const std::string& NewId)
    {
        if (!Lib.Get(NewId)) return false;
        for (Placed& P : Objects)
            if (P.Handle == Handle)
            {
                if (P.Id == NewId) return true;
                P.Id = NewId;
                Resting.erase(Handle);
                Show(World, P);
                auto M = Modes.find(Handle);
                auto V = Visuals.find(Handle);
                if (M != Modes.end() && V != Visuals.end())
                    if (AActor* A = V->second.Get()) ApplyMode(A, M->second);
                Gen++;
                return true;
            }
        return false;
    }

    void RemoveAll()
    {
        for (const Placed& P : Objects) Destroy(P.Handle);
        Objects.clear();
        Modes.clear();
        Resting.clear();
        MarkerSet.clear();
        Gen++;
    }

    const Placed* Get(int Handle)
    {
        for (const Placed& P : Objects) if (P.Handle == Handle) return &P;
        return nullptr;
    }

    const std::vector<Placed>& All() { return Objects; }
    uint64_t Generation() { return Gen; }

    void Bounds(int Handle, FVector& Min, FVector& Max)
    {
        Min = Max = FVector{};
        const Placed* P = Get(Handle);
        if (!P) return;
        auto O = Lib.Get(P->Id);
        if (!O) return;
        bool bFirst = true;
        for (int c = 0; c < 8; c++)
        {
            const FVector L = FromSkate((c & 1) ? O->Max[0] : O->Min[0], (c & 2) ? O->Max[1] : O->Min[1], (c & 4) ? O->Max[2] : O->Min[2]);
            const FVector R = Rotate(P->Rotation, L);
            const FVector W{ R.X + P->Position.X, R.Y + P->Position.Y, R.Z + P->Position.Z };
            if (bFirst) { Min = Max = W; bFirst = false; continue; }
            Min = FVector{ std::min(Min.X, W.X), std::min(Min.Y, W.Y), std::min(Min.Z, W.Z) };
            Max = FVector{ std::max(Max.X, W.X), std::max(Max.Y, W.Y), std::max(Max.Z, W.Z) };
        }
    }

    namespace
    {
        // PartDefinition (RenderWare 0x40007, 212 bytes, big-endian as stored).
        float BigFloat(const std::vector<uint8_t>& B, size_t Offset)
        {
            const uint32_t U = (uint32_t(B[Offset]) << 24) | (uint32_t(B[Offset + 1]) << 16) | (uint32_t(B[Offset + 2]) << 8) | uint32_t(B[Offset + 3]);
            float F;
            memcpy(&F, &U, 4);
            return F;
        }

        bool HasBody(const DropperData::Object& O)
        {
            if (O.RigidBody.size() < 0xbc) return false;
            for (const DropperData::Volume& V : O.Volumes)
                if (V.Type == 1 || V.Type == 2 || V.Type == 4 || V.Type == 5) return true;
            return false;
        }

        const std::vector<DropperData::CollisionTriangle> NoTriangles;
    }

    bool IsPhysical(const std::string& Id)
    {
        const DropperData::CatalogEntry* E = Lib.Find(Id);
        if (!E || E->Db != "dynamic") return false;
        auto O = Lib.Get(Id);
        return O && HasBody(*O);
    }

    void BuildPhysics(std::vector<SkDmo>& Dmos, std::vector<SkDmoVolume>& Volumes, std::vector<SkDmoTriangle>& Triangles,
                      std::vector<SkDmoSpline>& Splines, std::vector<float>& Points)
    {
        Dmos.clear(); Volumes.clear(); Triangles.clear(); Splines.clear(); Points.clear();
        int GroundJoined = 0;
        struct LogJoined { int& N; ~LogJoined() { static int Logged = 0; if (N && Logged++ < 20) Log("[dropper] %d physics-piece edge(s) joined to the ground", N); } } LogAtEnd{ GroundJoined };
        for (const Placed& P : Objects)
        {
            if (!IsPhysical(P.Id)) continue;
            auto O = Lib.Get(P.Id);
            const std::vector<uint8_t>& B = O->RigidBody;
            SkDmo D{};
            D.id = uint32_t(P.Handle);
            D.kinematic = P.bGhost ? 1u : 0u;
            D.position[0] = float(P.Position.X); D.position[1] = float(P.Position.Y); D.position[2] = float(P.Position.Z);
            D.rotation[0] = float(P.Rotation.X); D.rotation[1] = float(P.Rotation.Y); D.rotation[2] = float(P.Rotation.Z); D.rotation[3] = float(P.Rotation.W);
            for (int i = 0; i < 3; i++) D.inverse_tensor[i] = BigFloat(B, 0x40 + 4 * i);
            D.inverse_mass = BigFloat(B, 0x50);
            D.spherical = BigFloat(B, 0x54);
            D.maximum_linear_velocity = BigFloat(B, 0x58);
            D.maximum_angular_velocity = BigFloat(B, 0x5c);
            D.linear_drag = BigFloat(B, 0x60);
            D.angular_drag = BigFloat(B, 0x64);
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) D.inverse_body_rows[r * 3 + c] = BigFloat(B, 0x70 + 0x10 * r + 4 * c);
            for (int i = 0; i < 3; i++) D.inverse_body_translation[i] = BigFloat(B, 0xa0 + 4 * i);
            for (int i = 0; i < 3; i++) D.material[i] = BigFloat(B, 0xb0 + 4 * i);
            D.first_volume = uint32_t(Volumes.size());
            for (const DropperData::Volume& V : O->Volumes)
            {
                if (V.Type != 1 && V.Type != 2 && V.Type != 4 && V.Type != 5) continue;
                SkDmoVolume W{};
                W.kind = V.Type;
                memcpy(W.rows, V.Rot, sizeof W.rows);
                memcpy(W.position, V.Pos, sizeof W.position);
                memcpy(W.params, V.Params, sizeof W.params);
                Volumes.push_back(W);
            }
            D.volume_count = uint32_t(Volumes.size()) - D.first_volume;
            D.first_triangle = uint32_t(Triangles.size());
            for (const DropperData::CollisionTriangle& T : O->Collision)
            {
                SkDmoTriangle W{};
                memcpy(W.points, T.V, sizeof W.points);
                memcpy(W.edges, T.Edges, sizeof W.edges);
                GroundJoined += DropperSeams::JoinModelGround(W.points, W.edges);   // its edges on the ground: no lip
                W.surface = T.Surface;
                Triangles.push_back(W);
            }
            D.triangle_count = uint32_t(Triangles.size()) - D.first_triangle;
            D.first_spline = uint32_t(Splines.size());
            for (const DropperData::GrindSpline& G : O->Grinds)
            {
                // Header +0x44 flags: byte 0 point count, byte 2 approach-vector count.
                SkDmoSpline W{};
                W.first_point = uint32_t(Points.size() / 3);
                W.point_count = uint32_t(G.Segments.size() / 3);
                Points.insert(Points.end(), G.Segments.begin(), G.Segments.end());
                W.approach_count = (G.Flags >> 8) & 0xFF;
                memcpy(W.approach, G.Normal, sizeof W.approach);
                W.word_60 = G.Surface;
                Splines.push_back(W);
            }
            D.spline_count = uint32_t(Splines.size()) - D.first_spline;
            Dmos.push_back(D);
        }
    }

    bool ApplyPhysicsPose(int Handle, const FVector& Position, const Quat& Rotation, bool bAwake)
    {
        for (Placed& P : Objects)
        {
            if (P.Handle != Handle || P.bGhost) continue;
            Rest& R = Resting[Handle];
            const double D = std::fabs(P.Position.X - Position.X) + std::fabs(P.Position.Y - Position.Y) + std::fabs(P.Position.Z - Position.Z)
                + 100.0 * (std::fabs(P.Rotation.X - Rotation.X) + std::fabs(P.Rotation.Y - Rotation.Y) + std::fabs(P.Rotation.Z - Rotation.Z) + std::fabs(P.Rotation.W - Rotation.W));
            if (D > 0.01)
            {
                P.Position = Position;
                P.Rotation = Rotation;
                auto V = Visuals.find(Handle);
                if (V != Visuals.end())
                    if (AActor* A = V->second.Get())
                    {
                        SetStill(A, false);   // simulating: Movable until it rests
                        PlaceActor(A, Position, ToRotator(Rotation));
                    }
                R.bMoving = true;
            }
            if (!bAwake && R.bMoving)
            {
                R.bMoving = false;
                auto V = Visuals.find(Handle);
                if (V != Visuals.end())
                    if (AActor* A = V->second.Get()) SetStill(A, true);
                return true;
            }
            return false;
        }
        return false;
    }

    // Props settling after physics only move: the server moves its copies, the patch follows its
    // rails at a slower pace (Client). Bumping Gen here respawned everything every few seconds.
    uint64_t PoseGen = 0;
    void Touch() { PoseGen++; }
    uint64_t PoseGeneration() { return PoseGen; }

    Snapshot Save()
    {
        Snapshot S;
        for (const Placed& P : Objects)
            if (!P.bGhost) S.Objects.push_back(P);
        return S;
    }

    void Load(UWorld* World, const Snapshot& S)
    {
        std::vector<int> Gone;
        for (const Placed& P : Objects)
        {
            bool bKept = false;
            for (const Placed& Q : S.Objects) if (Q.Handle == P.Handle && Q.Id == P.Id) { bKept = true; break; }
            if (!bKept) Gone.push_back(P.Handle);
        }
        for (int H : Gone) Remove(H);
        for (const Placed& Q : S.Objects)
        {
            Placed* P = nullptr;
            for (Placed& O : Objects) if (O.Handle == Q.Handle) { P = &O; break; }
            if (P)
            {
                *P = Q;
                auto V = Visuals.find(Q.Handle);
                if (V != Visuals.end())
                    if (AActor* A = V->second.Get())
                        PlaceActor(A, Q.Position, ToRotator(Q.Rotation));
                continue;
            }
            Objects.push_back(Q);
            NextHandle = std::max(NextHandle, Q.Handle + 1);
            Show(World, Objects.back());
        }
        Gen++;
    }

    // ---- the action overlay's actions ----
    namespace
    {
        Placed* Find(int Handle)
        {
            for (Placed& P : Objects) if (P.Handle == Handle) return &P;
            return nullptr;
        }

        // Bounds of a placed object at a given frame (cm), from its model bounds.
        void BoundsAt(const Placed& P, const FVector& Position, const Quat& Rotation, FVector& Min, FVector& Max)
        {
            Min = Max = Position;
            auto O = Lib.Get(P.Id);
            if (!O) return;
            for (int c = 0; c < 8; c++)
            {
                const FVector L = FromSkate((c & 1) ? O->Max[0] : O->Min[0], (c & 2) ? O->Max[1] : O->Min[1], (c & 4) ? O->Max[2] : O->Min[2]);
                const FVector R = Rotate(Rotation, L);
                const FVector W{ R.X + Position.X, R.Y + Position.Y, R.Z + Position.Z };
                if (c == 0) { Min = Max = W; continue; }
                Min = FVector{ std::min(Min.X, W.X), std::min(Min.Y, W.Y), std::min(Min.Z, W.Z) };
                Max = FVector{ std::max(Max.X, W.X), std::max(Max.Y, W.Y), std::max(Max.Z, W.Z) };
            }
        }

        bool Overlap(const FVector& AMin, const FVector& AMax, const FVector& BMin, const FVector& BMax, double Pad)
        {
            return AMin.X - Pad <= BMax.X && BMin.X <= AMax.X + Pad && AMin.Y - Pad <= BMax.Y && BMin.Y <= AMax.Y + Pad
                && AMin.Z - Pad <= BMax.Z && BMin.Z <= AMax.Z + Pad;
        }

        bool Movable(const Placed& P) { return !P.bGhost && IsPhysical(P.Id); }

        // 82C21948: the props touching Handle (its bounds grown by 0.05 m), and theirs, recursively;
        // props already in Out are skipped.
        void Touching(int Handle, std::vector<int>& Out)
        {
            const Placed* P = Find(Handle);
            if (!P) return;
            FVector Lo, Hi;
            BoundsAt(*P, P->Position, P->Rotation, Lo, Hi);
            std::vector<int> Found;
            for (const Placed& Q : Objects)
            {
                if (Q.Handle == Handle || !Movable(Q) || std::find(Out.begin(), Out.end(), Q.Handle) != Out.end()) continue;
                FVector QLo, QHi;
                BoundsAt(Q, Q.Position, Q.Rotation, QLo, QHi);
                if (Overlap(Lo, Hi, QLo, QHi, 5.0)) { Out.push_back(Q.Handle); Found.push_back(Q.Handle); }
            }
            for (int H : Found) Touching(H, Out);
        }

        void SetPose(Placed& P, const FVector& Position, const Quat& Rotation)
        {
            P.Position = Position;
            P.Rotation = Rotation;
            auto V = Visuals.find(P.Handle);
            if (V != Visuals.end())
                if (AActor* A = V->second.Get())
                    PlaceActor(A, Position, ToRotator(Rotation));
        }
    }

    bool MarkerHas(int Handle) { return MarkerSet.count(Handle) != 0; }
    bool MarkerFull() { return MarkerSet.size() >= MarkerMax; }

    void MarkerAdd(int Handle)
    {
        if (MarkerFull()) return;
        Placed* P = Find(Handle);
        if (!P || !Movable(*P)) return;
        std::vector<int> Group;
        for (auto& [H, S] : MarkerSet) Group.push_back(H);   // those already in are not gathered again
        const size_t Before = Group.size();
        Group.push_back(Handle);
        Touching(Handle, Group);
        for (size_t i = Before; i < Group.size(); i++)
            if (Placed* Q = Find(Group[i])) MarkerSet[Q->Handle] = Saved{ Q->Position, Q->Rotation };
        Log("[dropper] session marker: %zu props (added %zu with %d)", MarkerSet.size(), Group.size() - Before, Handle);
    }

    void MarkerRemove(int Handle) { MarkerSet.erase(Handle); }
    void MarkerClear() { MarkerSet.clear(); }

    void MarkerRestore()
    {
        if (MarkerSet.empty()) return;
        for (auto& [H, S] : MarkerSet)
            if (Placed* P = Find(H)) SetPose(*P, S.Position, S.Rotation);
        Gen++;   // the engine re-places the bodies there, at rest
        Log("[dropper] session marker: %zu props put back", MarkerSet.size());
    }

    bool IsUpright(int Handle)
    {
        const Placed* P = Find(Handle);
        if (!P) return true;
        const FVector Up = Rotate(P->Rotation, FVector{ 0, 0, 1 });   // the model's up (Skate Y)
        const double C = std::clamp(Up.Z / std::max(1e-9, std::sqrt(Up.X * Up.X + Up.Y * Up.Y + Up.Z * Up.Z)), -1.0, 1.0);
        return std::acos(C) * 57.29577951308232 < 10.0;
    }

    namespace
    {
        // 82C21368 + the gather: the prop and the props on it (82C21948 at the current frames).
        std::vector<int> ResetGroup(int Handle)
        {
            std::vector<int> Group{ Handle };
            Touching(Handle, Group);
            return Group;
        }
    }

    bool CanReset(int Handle)
    {
        const Placed* P = Find(Handle);
        if (!P || !Movable(*P)) return false;
        const std::vector<int> Group = ResetGroup(Handle);
        // 82C21D00 -> 82DDFB80 with no margin: another prop where the group goes back to blocks it.
        for (int H : Group)
        {
            const Placed* G = Find(H);
            if (!G) continue;
            FVector Lo, Hi;
            BoundsAt(*G, G->HomePosition, G->HomeRotation, Lo, Hi);
            for (const Placed& Q : Objects)
            {
                if (Q.bGhost || std::find(Group.begin(), Group.end(), Q.Handle) != Group.end()) continue;
                FVector QLo, QHi;
                BoundsAt(Q, Q.Position, Q.Rotation, QLo, QHi);
                if (Overlap(Lo, Hi, QLo, QHi, 0.0)) return false;
            }
        }
        return true;
    }

    void Reset(int Handle)
    {
        if (!CanReset(Handle)) return;
        for (int H : ResetGroup(Handle))
            if (Placed* P = Find(H)) SetPose(*P, P->HomePosition, P->HomeRotation);
        Gen++;
    }

    namespace
    {
        // The static pieces' collision, joined (buried faces out, seams and piece-on-piece edges
        // joined), kept until a static piece is placed, moved or removed: rebuilding it for every
        // collision patch took 50-60 ms each time (stutters near placed objects, 2026-10-11).
        struct JoinedPieces
        {
            std::string Key;
            std::vector<float> Tri;
            std::vector<uint8_t> Edges;
            std::vector<uint16_t> Surfaces;
            std::vector<size_t> Starts;       // per piece, plus the end
            std::vector<FVector> Lo, Hi;      // per piece bounds
            std::vector<std::vector<std::vector<DropperSeams::V3>>> Rails;   // per piece: grind rails from its own edges
        };
        JoinedPieces Joined;

        std::string StaticKey()
        {
            std::string K;
            char Buf[160];
            for (const Placed& P : Objects)
            {
                if (P.bGhost || IsPhysical(P.Id)) continue;
                snprintf(Buf, sizeof(Buf), "%s %.2f %.2f %.2f %.5f %.5f %.5f %.5f|", P.Id.c_str(), P.Position.X, P.Position.Y, P.Position.Z,
                         P.Rotation.X, P.Rotation.Y, P.Rotation.Z, P.Rotation.W);
                K += Buf;
            }
            return K;
        }

        void RebuildJoined(const std::string& Key)
        {
            LARGE_INTEGER F, T0, T1; QueryPerformanceFrequency(&F); QueryPerformanceCounter(&T0);
            JoinedPieces J;
            J.Key = Key;
            for (const Placed& P : Objects)
            {
                if (P.bGhost || IsPhysical(P.Id)) continue;
                auto O = Lib.Get(P.Id);
                if (!O) continue;
                FVector Lo, Hi;
                Bounds(P.Handle, Lo, Hi);
                J.Lo.push_back(Lo); J.Hi.push_back(Hi);
                J.Starts.push_back(J.Edges.size() / 3);
                for (const DropperData::CollisionTriangle& T : O->Collision)
                {
                    // Skate (a, b, c) -> Unreal (a, c, b): the engine swaps b and c back when it converts
                    // to skate space, and the edge codes follow the corners (see sk_set_collision2).
                    auto World = [&](const float* V)
                    {
                        const FVector R = Rotate(P.Rotation, FromSkate(V[0], V[1], V[2]));
                        return FVector{ R.X + P.Position.X, R.Y + P.Position.Y, R.Z + P.Position.Z };
                    };
                    const FVector A = World(&T.V[0]), B = World(&T.V[3]), C = World(&T.V[6]);
                    for (const FVector& V : { A, C, B })
                    {
                        J.Tri.push_back(float(V.X)); J.Tri.push_back(float(V.Y)); J.Tri.push_back(float(V.Z));
                    }
                    J.Edges.push_back(T.Edges[2]); J.Edges.push_back(T.Edges[1]); J.Edges.push_back(T.Edges[0]);
                    J.Surfaces.push_back(T.Surface);
                }
            }
            J.Starts.push_back(J.Edges.size() / 3);
            const size_t Gone = DropperSeams::DropCoveredFaces(J.Tri, J.Edges, J.Surfaces, J.Starts);
            DropperSeams::Join(J.Tri, J.Edges, J.Starts);
            const int OnPieces = DropperSeams::JoinSupport(J.Tri, J.Edges, J.Starts, [](float, float) { return std::nanf(""); });
            size_t RailCount = 0;
            J.Rails.resize(J.Lo.size());
            for (size_t o = 0; o + 1 < J.Starts.size(); o++) { DropperSeams::GrindRails(J.Tri, J.Edges, J.Starts[o], J.Starts[o + 1], J.Rails[o]); RailCount += J.Rails[o].size(); }
            QueryPerformanceCounter(&T1);
            static int Logged = 0;
            if (Logged++ < 40)
                Log("[dropper] joined %zu static piece(s) in %.1f ms: %zu buried face(s) left out, %d edge(s) joined to the piece under them, %zu grind rail(s) from their edges",
                    J.Lo.size(), double(T1.QuadPart - T0.QuadPart) * 1000.0 / double(F.QuadPart), Gone, OnPieces, RailCount);
            Joined = std::move(J);
        }
    }

    void AppendCollision(const FVector& Centre, double Radius, std::vector<float>& Triangles, std::vector<uint8_t>& Edges,
                         std::vector<uint16_t>& Surfaces, std::vector<float>& RailPoints, std::vector<uint32_t>& RailLengths,
                         const std::function<float(float, float)>& GroundAt)
    {
        auto Near = [&](const FVector& Lo, const FVector& Hi)
        {
            const double DX = std::max({ Lo.X - Centre.X, 0.0, Centre.X - Hi.X }), DY = std::max({ Lo.Y - Centre.Y, 0.0, Centre.Y - Hi.Y });
            return DX * DX + DY * DY <= Radius * Radius;
        };
        // Static pieces: their joined collision, rebuilt only when one of them changed.
        const std::string Key = StaticKey();
        if (Key != Joined.Key) RebuildJoined(Key);
        std::vector<size_t> Starts;
        for (size_t o = 0; o + 1 < Joined.Starts.size(); o++)
        {
            if (!Near(Joined.Lo[o], Joined.Hi[o])) continue;
            Starts.push_back(Edges.size() / 3);
            const size_t A = Joined.Starts[o], B = Joined.Starts[o + 1];
            Triangles.insert(Triangles.end(), Joined.Tri.begin() + long long(A * 9), Joined.Tri.begin() + long long(B * 9));
            Edges.insert(Edges.end(), Joined.Edges.begin() + long long(A * 3), Joined.Edges.begin() + long long(B * 3));
            Surfaces.insert(Surfaces.end(), Joined.Surfaces.begin() + long long(A), Joined.Surfaces.begin() + long long(B));
            for (const auto& R : Joined.Rails[o])
            {
                // a rail along the ground is the ramp's foot (joined to the ground below): not a grind
                bool bOnGround = bool(GroundAt);
                for (size_t i = 0; i < R.size() && bOnGround; i++)
                {
                    const float G = GroundAt(float(R[i].X), float(R[i].Y));
                    bOnGround = std::isfinite(G) && std::fabs(float(R[i].Z) - G) <= 6.f;
                }
                if (bOnGround) continue;
                for (const auto& V : R) { RailPoints.push_back(float(V.X)); RailPoints.push_back(float(V.Y)); RailPoints.push_back(float(V.Z)); }
                RailLengths.push_back(uint32_t(R.size()));
            }
        }
        Starts.push_back(Edges.size() / 3);
        // The scanned ground changes with every patch: only that (cheap) join runs here.
        if (GroundAt && Starts.size() > 1)
        {
            const int N = DropperSeams::JoinSupport(Triangles, Edges, Starts, GroundAt, false);
            static int Logged = 0;
            if (N && Logged++ < 20) Log("[dropper] %d edge(s) joined to the ground", N);
        }
        // Grind splines (every solid piece, props too): the game's grind contact is each cubic segment's chord (P0 -> P3).
        for (const Placed& P : Objects)
        {
            if (P.bGhost) continue;
            auto O = Lib.Get(P.Id);
            if (!O || O->Grinds.empty()) continue;
            FVector Lo, Hi;
            Bounds(P.Handle, Lo, Hi);
            if (!Near(Lo, Hi)) continue;
            auto World = [&](const float* V)
            {
                const FVector R = Rotate(P.Rotation, FromSkate(V[0], V[1], V[2]));
                return FVector{ R.X + P.Position.X, R.Y + P.Position.Y, R.Z + P.Position.Z };
            };
            for (const DropperData::GrindSpline& G : O->Grinds)
            {
                const uint32_t N = G.SegmentCount();
                if (!N) continue;
                uint32_t Count = 0;
                for (uint32_t s = 0; s < N; s++)
                {
                    const float* Seg = &G.Segments[size_t(s) * 12];
                    if (s == 0)
                    {
                        const FVector W = World(&Seg[0]);
                        RailPoints.push_back(float(W.X)); RailPoints.push_back(float(W.Y)); RailPoints.push_back(float(W.Z));
                        Count++;
                    }
                    const FVector W = World(&Seg[9]);
                    RailPoints.push_back(float(W.X)); RailPoints.push_back(float(W.Y)); RailPoints.push_back(float(W.Z));
                    Count++;
                }
                RailLengths.push_back(Count);
            }
        }
    }

    FVector LocalCentre(const std::string& Id)
    {
        auto O = Lib.Get(Id);
        if (!O) return FVector{};
        const FVector Lo = FromSkate(O->Min[0], O->Min[1], O->Min[2]), Hi = FromSkate(O->Max[0], O->Max[1], O->Max[2]);
        return FVector{ (Lo.X + Hi.X) * 0.5, (Lo.Y + Hi.Y) * 0.5, 0.0 };
    }

    FVector LocalSize(const std::string& Id)
    {
        auto O = Lib.Get(Id);
        if (!O) return FVector{};
        const FVector Lo = FromSkate(O->Min[0], O->Min[1], O->Min[2]), Hi = FromSkate(O->Max[0], O->Max[1], O->Max[2]);
        return FVector{ std::fabs(Hi.X - Lo.X), std::fabs(Hi.Y - Lo.Y), std::fabs(Hi.Z - Lo.Z) };
    }

    void PublishToServer()
    {
        static uint64_t Published = ~0ull, PublishedPose = ~0ull;
        if (Published == Gen && PublishedPose == PoseGen) return;
        DropperShareHeader* H = OpenDropperShare();
        if (!H) return;
        Published = Gen;
        PublishedPose = PoseGen;
        InterlockedIncrement(&H->Sequence);
        uint32_t N = 0;
        DropperShareEntry* E = DropperShareEntries(H);
        for (const Placed& P : Objects)
        {
            if (P.bGhost || N >= DropperShareCapacity) continue;
            DropperShareEntry& D = E[N++];
            strncpy_s(D.Id, P.Id.c_str(), _TRUNCATE);
            D.X = P.Position.X; D.Y = P.Position.Y; D.Z = P.Position.Z;
            D.Qx = P.Rotation.X; D.Qy = P.Rotation.Y; D.Qz = P.Rotation.Z; D.Qw = P.Rotation.W;
        }
        H->Count = N;
        H->Generation = (Gen << 24) ^ PoseGen;   // any change; the server sees whether the set changed
        InterlockedIncrement(&H->Sequence);
    }

    void MirrorOnServer(UWorld* World)
    {
        static uint64_t Mirrored = 0;
        static bool bAny = false;
        static std::vector<Visual> Copies;
        DropperShareHeader* H = OpenDropperShare();
        if (!H || !World) return;
        const LONG Seq = H->Sequence;
        if ((Seq & 1) || (bAny && H->Generation == Mirrored)) return;
        std::vector<DropperShareEntry> Want(DropperShareEntries(H), DropperShareEntries(H) + std::min(H->Count, DropperShareCapacity));
        const uint64_t G = H->Generation;
        if (H->Sequence != Seq) return;   // the client was writing: next frame
        if (!bAny && G == 0 && Want.empty()) return;
        std::string Err;
        if (!Load(Err)) { Log("[dropper] server: %s", Err.c_str()); Mirrored = G; bAny = true; return; }
        // the same objects (only moved, e.g. props settling): the copies move, nothing is respawned
        static std::vector<std::string> CopyIds;
        bool bSame = CopyIds.size() == Want.size() && Copies.size() == Want.size();
        for (size_t i = 0; bSame && i < Want.size(); i++) bSame = CopyIds[i] == Want[i].Id;
        if (bSame)
        {
            for (size_t i = 0; i < Want.size(); i++)
                if (AActor* A = Copies[i].Get())
                    PlaceActor(A, FVector{ Want[i].X, Want[i].Y, Want[i].Z },
                        ToRotator(Quat{ Want[i].Qx, Want[i].Qy, Want[i].Qz, Want[i].Qw }));
            Mirrored = G;
            bAny = true;
            return;
        }
        for (Visual& V : Copies) if (AActor* A = V.Get()) A->K2_DestroyActor();
        Copies.clear();
        CopyIds.clear();
        for (const DropperShareEntry& D : Want)
        {
            auto O = Lib.Get(D.Id);
            Copies.push_back({});   // one slot per entry (moves match by index)
            CopyIds.push_back(D.Id);
            if (!O) continue;
            FTransform T = Identity();
            T.Translation = FVector{ D.X, D.Y, D.Z };
            T.Rotation = FQuat{ D.Qx, D.Qy, D.Qz, D.Qw };
            AActor* A = UGameplayStatics::BeginDeferredActorSpawnFromClass(World, AActor::StaticClass(), T,
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, ESpawnActorScaleMethod::MultiplyWithRoot);
            if (A) A = UGameplayStatics::FinishSpawningActor(A, T, ESpawnActorScaleMethod::MultiplyWithRoot);
            if (!A) continue;
            auto* Mesh = static_cast<UProceduralMeshComponent*>(A->AddComponentByClass(UProceduralMeshComponent::StaticClass(), false, Identity(), false));
            if (!Mesh) { A->K2_DestroyActor(); continue; }
            PlaceActor(A, T.Translation, ToRotator(Quat{ D.Qx, D.Qy, D.Qz, D.Qw }));
            AddCollisionSection(Mesh, D.Id, *O, 0);
            SetSolid(Mesh, true);
            Mesh->SetMobility(EComponentMobility::Static);   // never a movement base the client cannot resolve
            Copies.back() = { A, A->Index };
        }
        Mirrored = G;
        bAny = true;
        Log("[dropper] server: %zu solid copies of the client's objects", Copies.size());
    }

    const std::vector<AActor*>& Actors()
    {
        // traced thousands of times a frame: rebuilt only when objects come or go
        static std::vector<AActor*> List;
        static size_t Built = ~size_t(0);
        static uint64_t BuiltGen = ~0ull;
        if (Built != Visuals.size() || BuiltGen != Gen)
        {
            List.clear();
            for (auto& [Handle, V] : Visuals)
                if (AActor* A = V.Get()) List.push_back(A);
            Built = Visuals.size();
            BuiltGen = Gen;
        }
        return List;
    }
}
