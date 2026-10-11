#include "WorldSampler.h"
#include "DropperWorld.h"

#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace SDK;

static constexpr float Miss = std::numeric_limits<float>::quiet_NaN();

// Low trace starts here above the feet: under most ceilings, above curbs and small steps.
static constexpr float LowStart = 150.f;
// High trace starts here above the feet: catches tall ledges, boxes and quarter pipes.
static constexpr float HighStart = 500.f;
// How far below the feet columns are traced.
static constexpr float Depth = 1500.f;
// Open-air patches trace this far down: after a big launch the ground can be tens of metres
// below the skater, and a patch that finds nothing leaves the old one (far behind) in place.
static constexpr float OpenAirDepth = 20000.f;
// A gap smaller than this between the high and low hit is solid, not headroom.
static constexpr float MinHeadroom = 220.f;
// Tall trace (solid walls) starts here above the feet: taller than Fortnite's rocks and cliffs.
static constexpr float TallStart = 4000.f;
// A surface found only from high above is used up to this far above the feet (a 12 m patch edge on
// a 60-degree slope); anything higher is a structure overhead, not ground.
static constexpr float MaxRise = 2200.f;
// The skater counts as on top of the world when the first surface from high above is no higher
// than this above its feet.
static constexpr float OnTopTolerance = 60.f;

void WorldSampler::Begin(const FVector& InFeet, const FVector* Skater)
{
    Feet = InFeet;
    SkaterAt = Skater ? *Skater : InFeet;
    // Snap the grid to its spacing so successive patches line up exactly.
    Centre.X = std::round(InFeet.X / Spacing) * Spacing;
    Centre.Y = std::round(InFeet.Y / Spacing) * Spacing;
    Centre.Z = InFeet.Z;
    Working.assign(size_t(Size) * Size, Miss);
    Next = 0;
    bRefining = false;
    bShapesGathered = false;
    bBusy = true;
}

// Names of what a trace hit: the component's owner, the component and (for static meshes) the mesh.
static std::string HitNames(UPrimitiveComponent* Comp)
{
    std::string Out;
    if (!Comp) return Out;
    // Built once per component (it was rebuilt for most hits: three name lookups per trace, the
    // "GetOwner" calls that doubled over a session in the 2026-10-10 health log).
    static std::unordered_map<const void*, std::string> Cache;
    if (auto It = Cache.find(Comp); It != Cache.end()) return It->second;
    if (Cache.size() > 50000) Cache.clear();
    if (AActor* Owner = Comp->GetOwner()) Out += Owner->GetName() + " ";
    Out += Comp->GetName();
    if (Comp->IsA(UStaticMeshComponent::StaticClass()))
        if (UStaticMesh* Mesh = static_cast<UStaticMeshComponent*>(Comp)->StaticMesh)
            Out += " " + Mesh->GetName();
    Cache.emplace(Comp, Out);
    return Out;
}

// Trees and other foliage: their canopy is high above open ground (and its collision is
// one-sided from below), so it must never turn the ground under it into a solid wall.
static bool IsFoliage(const std::string& Names)
{
    std::string N = Names;
    for (char& C : N) C = char(tolower(static_cast<unsigned char>(C)));
    static const char* Words[] = { "tree", "foliage", "leaf", "leaves", "bush", "shrub", "plant", "pine", "palm",
                                   "oak", "birch", "branch", "canopy", "flower", "grass", "hedge", "vine" };
    for (const char* W : Words)
        if (N.find(W) != std::string::npos) return true;
    return false;
}

void WorldSampler::NoteWall(const std::string& What)
{
    // Strip instance suffixes so one log line covers every copy of an object.
    std::string Key = What.substr(0, What.find(' '));
    while (!Key.empty() && (isdigit(static_cast<unsigned char>(Key.back())) || Key.back() == '_')) Key.pop_back();
    if (WallNames.size() < 200 && WallNames.insert(Key).second)
        Log("[walls] solid wall from: %s", What.c_str());
}

float WorldSampler::TraceDown(UObject* WorldContext, float X, float Y, float Top, float Bottom, UPrimitiveComponent** HitComp, bool* bInside, bool bPawn) const
{
    if (bInside) *bInside = false;
    FHitResult Hit{};
    // Object dropper pieces reach the skate engine with their own Skate 3 collision.
    const std::vector<AActor*>& Dropped = DropperWorld::Actors();
    TArray<AActor*> Ignore(const_cast<AActor**>(Dropped.data()), int32(Dropped.size()), int32(Dropped.size()));
    const FVector Start{ X, Y, Top };
    const FVector End{ X, Y, Bottom };
    const FLinearColor Color{};

    // bPawn: what blocks Fortnite's own character (the "Pawn" collision profile) instead of the
    // Visibility channel. Some terrain pieces (S_Elevation_Ground slopes) block one and not
    // the other over parts of the mesh.
    static const FName PawnProfile = UKismetStringLibrary::Conv_StringToName(FString(L"Pawn"));
    const bool bHit = bPawn
        ? UKismetSystemLibrary::LineTraceSingleByProfile(WorldContext, Start, End, PawnProfile, false, Ignore,
              EDrawDebugTrace::None, &Hit, true, Color, Color, 0.f)
        : UKismetSystemLibrary::LineTraceSingle(WorldContext, Start, End,
              ETraceTypeQuery::TraceTypeQuery1 /* Visibility */, false, Ignore, EDrawDebugTrace::None,
              &Hit, true, Color, Color, 0.f);

    if (!bHit || !Hit.bBlockingHit)
        return Miss;
    if (HitComp) *HitComp = Hit.Component.Get();
    if (bInside) *bInside = Hit.bStartPenetrating || Hit.Distance < 0.5f;
    return float(Hit.ImpactPoint.Z);
}

// Player-built stairs (build pieces named PBW...Stair...): their visible mesh is stepped, their
// player collision one smooth block. On top of the world the column grid looks through them so the
// board rides only that block (added whole by ShapeRails); otherwise the grid's 50 cm samples of
// the steps made the ramp choppy.
static bool IsBuildStair(UPrimitiveComponent* Comp)
{
    static std::unordered_map<const void*, bool> Known;
    auto It = Known.find(Comp);
    if (It != Known.end()) return It->second;
    std::string Name = HitNames(Comp);
    for (char& C : Name) C = char(tolower(static_cast<unsigned char>(C)));
    const bool bStair = Name.find("pbw") != std::string::npos && Name.find("stair") != std::string::npos;
    if (Known.size() > 50000) Known.clear();
    Known.emplace(Comp, bStair);
    return bStair;
}

float WorldSampler::TraceDownPastTrees(UObject* WorldContext, float X, float Y, float Top, float Bottom, bool* bInside) const
{
    if (bInside) *bInside = false;
    static std::unordered_map<const void*, bool> TreeComponents;  // name test once per component
    for (int Try = 0; Try < 4; Try++)
    {
        UPrimitiveComponent* Comp = nullptr;
        const float Z = TraceDown(WorldContext, X, Y, Top, Bottom, &Comp, bInside);
        if (std::isnan(Z) || !Comp) return Z;
        auto It = TreeComponents.find(Comp);
        if (It == TreeComponents.end())
        {
            if (TreeComponents.size() > 50000) TreeComponents.clear();
            It = TreeComponents.emplace(Comp, ShapeRails::IsPassThroughTree(Comp)).first;
        }
        if (!It->second && !(bOnTop && IsBuildStair(Comp))) return Z;
        // Passing through a tree or a player-built stair: starting inside it is not "inside
        // something solid" (the rock-face rule built 5 m walls under stairs from that).
        if (bInside) *bInside = false;
        Top = Z - 1.f;  // continue below the tree (a trace starting inside its trunk passes through)
        if (Top <= Bottom) return Miss;
    }
    if (bInside) *bInside = false;
    return Miss;
}

bool WorldSampler::Step(UObject* WorldContext, int Budget)
{
    if (!bBusy)
        return true;

    const float Half = Radius;
    const int Total = Size * Size;
    if (Next == 0 && !bRefining)
    {
        // On top of the world: standing in open air. Either nothing is above the skater, or
        // what is above has an underside (a roof, a bridge, branches): an upward trace from just
        // above the feet hits it. Clipped under the map (superdude) the skater is inside solid
        // ground, which has no underside from in there, so the patch stays on the plain scan.
        bOnTop = false;
        OnTopWhy.clear();
        if (bHillsWhenOnTop && !bSolidWalls)
        {
            UPrimitiveComponent* Comp = nullptr;
            const float Sx = float(SkaterAt.X), Sy = float(SkaterAt.Y), Sz = float(SkaterAt.Z);
            // From 200 m up: however deep under the map a clip has taken the skater, the map
            // surface above it is found (and keeps the patch on the plain scan).
            const float Top = TraceDown(WorldContext, Sx, Sy, Sz + OpenAirDepth, Sz - OpenAirDepth, &Comp);
            if (!std::isnan(Top) && Top < Sz + OnTopTolerance)
            {
                bOnTop = true;
                OnTopWhy = "open sky";
            }
            else if (!std::isnan(Top))
            {
                const std::string Over = HitNames(Comp);
                UPrimitiveComponent* UnderComp = nullptr;
                const float Ground = TraceDownPastTrees(WorldContext, Sx, Sy, Sz + 100.f, Sz - OpenAirDepth);
                const float Underside = TraceDown(WorldContext, Sx, Sy, Sz + 30.f, Top - 5.f, &UnderComp);
                bOnTop = !std::isnan(Ground) && Ground < Sz + OnTopTolerance && !std::isnan(Underside);
                char Buf[512];
                snprintf(Buf, sizeof(Buf), "above %.0f cm: %.150s; underside %s %.150s", Top - Sz, Over.c_str(),
                         std::isnan(Underside) ? "none" : "at", std::isnan(Underside) ? "" : HitNames(UnderComp).c_str());
                OnTopWhy = Buf;
            }
            else OnTopWhy = "nothing found";
        }
    }
    if (Next == 0 && !bRefining && bHillsWhenOnTop)
    {
        static int LastOnTop = -1;
        // Diagnostics (riding into a Tilted Towers hillside at road level): when the skater goes
        // from on top to inside the ground, log the last patch's column heights around it (cm
        // above the skater's feet, '.' = no ground there) so the wall it passed can be checked.
        static int Dumps = 0;
        if (!bOnTop && LastOnTop == 1 && bHasPatch && Dumps < 6)
        {
            Dumps++;
            const int CI = int(std::lround((SkaterAt.X - FinishedCentre.X) / Spacing)) + Size / 2;
            const int CJ = int(std::lround((SkaterAt.Y - FinishedCentre.Y) / Spacing)) + Size / 2;
            Log("[collision] went inside the ground at (%.0f, %.0f, %.0f): last patch heights around it (rows +X, columns +Y, 50 cm apart):",
                SkaterAt.X, SkaterAt.Y, SkaterAt.Z);
            for (int I = CI - 6; I <= CI + 6; I++)
            {
                std::string Row;
                for (int J = CJ - 6; J <= CJ + 6; J++)
                {
                    char Cell[12];
                    if (I < 0 || J < 0 || I >= Size || J >= Size) snprintf(Cell, sizeof(Cell), "%6s", "-");
                    else
                    {
                        const float H = Finished[size_t(I) * Size + J];
                        if (std::isnan(H)) snprintf(Cell, sizeof(Cell), "%6s", ".");
                        else snprintf(Cell, sizeof(Cell), "%6.0f", H - float(SkaterAt.Z));
                    }
                    Row += Cell;
                }
                Log("[collision]   %s%s", Row.c_str(), I == CI ? "   <- skater row" : "");
            }
        }
        if (int(bOnTop) != LastOnTop || !bOnTop)
            Log("[collision] skater %s (%s): hill/slope meshes %s", bOnTop ? "in open air" : "inside something solid (under the map?)",
                OnTopWhy.c_str(), bOnTop ? "solid" : "scanned plainly");
        LastOnTop = int(bOnTop);
    }
    const bool bSolid = bSolidWalls || bOnTop;
    const float ColumnDepth = bOnTop ? OpenAirDepth : Depth;  // under the map: unchanged
    for (; Next < Total && Budget > 0; Next++, Budget--)
    {
        const int I = Next / Size, J = Next % Size;
        const float X = float(Centre.X) - Half + I * Spacing;
        const float Y = float(Centre.Y) - Half + J * Spacing;
        const float FeetZ = float(Feet.Z);

        bool bLowInside = false, bHighInside = false;
        float Low = TraceDownPastTrees(WorldContext, X, Y, FeetZ + LowStart, FeetZ - ColumnDepth, &bLowInside);
        float High = TraceDownPastTrees(WorldContext, X, Y, FeetZ + HighStart, FeetZ - ColumnDepth, &bHighInside);
        if (bOnTop)
        {
            // A trace that starts inside a rock or slope "hits" at its own start point. Under the
            // open sky that is never ground: the real top comes from the tall trace below.
            // (Under the map the plain scan keeps its old behaviour for the clipping glitches.)
            if (bLowInside) Low = Miss;
            if (bHighInside) High = Miss;
        }

        float Height = Low;
        if (std::isnan(Low))
            Height = High;
        else if (!std::isnan(High) && High > Low + 1.f && High - Low < MinHeadroom)
            Height = High;  // the low trace began inside something solid: use its top
        if (bSolid)
        {
            if (std::isnan(Height))
            {
                // Both traces started under the surface (steep hills, 45-degree slopes, tall
                // builds rising above the feet): take the surface from high above.
                UPrimitiveComponent* Comp = nullptr;
                const float Tall = TraceDown(WorldContext, X, Y, FeetZ + TallStart, FeetZ - ColumnDepth, &Comp);
                if (!std::isnan(Tall) && Tall < FeetZ + MaxRise && !IsFoliage(HitNames(Comp)) && !(bOnTop && Comp && IsBuildStair(Comp)))
                    Height = Tall;
                else if (bOnTop && bLowInside && bHighInside)
                {
                    // Solid at 1.5 m and at 5 m above the feet: a cliff or rock face (Tilted
                    // Towers' rock walls are taller than MaxRise, so their top was rejected and the
                    // column was a hole the skater rode into and fell under the map). A wall up to
                    // the rock's top (at least 5 m). On top of the map only: under it these
                    // "inside" checks are never made, so superdude's clip is untouched.
                    const bool bTop = !std::isnan(Tall) && !IsFoliage(HitNames(Comp)) && !(Comp && IsBuildStair(Comp));
                    Height = bTop ? Tall : FeetZ + HighStart;
                    static int Logged = 0;
                    if (Logged++ < 400 && Logged % 20 == 1) Log("[walls] rock face at (%.0f, %.0f): solid to %.0f cm above the feet", X, Y, Height - FeetZ);
                }
            }
            else
            {
                UPrimitiveComponent* Comp = nullptr;
                const float Tall = TraceDown(WorldContext, X, Y, FeetZ + TallStart, Height + 1.f, &Comp);
                const std::string What = !std::isnan(Tall) && Tall > Height + MinHeadroom ? HitNames(Comp) : std::string();
                if (!std::isnan(Tall) && Tall > Height + MinHeadroom && !IsFoliage(What) && !(bOnTop && Comp && IsBuildStair(Comp)))
                {
                    // Open space above the ground (bridge, roof, tree) has an underside an upward
                    // trace from just above the ground hits; inside a rock there is none. The
                    // trace runs 1 cm past the top: Tilted Towers' bridge walkway is sidewalk and
                    // floor-tile slabs thinner than 5 cm, whose underside an end 5 cm below the
                    // top never reached (an invisible wall under the bridge). This block only runs
                    // on top of the map (bSolid = solid_walls or bOnTop); under the map is unchanged.
                    bool bUnderInside = false;
                    const float Under = TraceDown(WorldContext, X, Y, Height + 30.f, Tall + 1.f, nullptr, &bUnderInside);
                    // A hit at the top itself is that surface seen from below (landscape is solid
                    // from both sides), not an underside: a hill read that way became a 64 m pit
                    // (2026-10-08 18:49 Tilted hillside). A real slab's underside is lower.
                    if (std::isnan(Under) || bUnderInside || Under >= Tall - 0.5f)
                    {
                        Height = Tall;
                        NoteWall(What);
                    }
                }
            }
            if (bOnTop)
            {
                // The ground Fortnite's character stands on (Pawn profile), when it is above what
                // the Visibility traces found and solid below (no underside): slopes whose
                // upper part only blocks pawns. Leaves, trees and invisible blocking volumes
                // do not count.
                UPrimitiveComponent* Comp = nullptr;
                bool bIn = false;
                float Pawn = TraceDown(WorldContext, X, Y, FeetZ + TallStart, FeetZ - ColumnDepth, &Comp, &bIn, true);
                for (int Try = 0; Try < 3 && !std::isnan(Pawn) && Comp; Try++)
                {
                    const std::string Names = HitNames(Comp);
                    if (!IsFoliage(Names) && !ShapeRails::IsPassThroughTree(Comp) && Names.find("Volume") == std::string::npos &&
                        !IsBuildStair(Comp)) break;
                    Pawn = TraceDown(WorldContext, X, Y, Pawn - 1.f, FeetZ - ColumnDepth, &Comp, &bIn, true);
                }
                if (!std::isnan(Pawn) && !bIn && Comp && Pawn < FeetZ + MaxRise && (std::isnan(Height) || Pawn > Height + 1.f))
                {
                    const std::string Names = HitNames(Comp);
                    bool bUnderInside = false;
                    const float Under = std::isnan(Height) ? Miss
                        : TraceDown(WorldContext, X, Y, Height + 30.f, Pawn + 1.f, nullptr, &bUnderInside, true);  // past thin slabs too
                    const bool bTopFromBelow = !std::isnan(Under) && Under >= Pawn - 0.5f;  // the same surface, not an underside
                    if (!IsFoliage(Names) && !ShapeRails::IsPassThroughTree(Comp) && Names.find("Volume") == std::string::npos && !IsBuildStair(Comp) &&
                        (std::isnan(Under) || bUnderInside || bTopFromBelow || Pawn - Height < 30.f))
                    {
                        if (!std::isnan(Height) && Pawn - Height >= MinHeadroom) NoteWall(Names);
                        Height = Pawn;
                    }
                }
            }
        }
        Working[size_t(I) * Size + J] = Height;
    }

    if (Next < Total)
        return false;

    // Ledge refinement: a few short traces per edge point, spread over frames like the columns.
    if (!bRefining)
    {
        // Hairline seams: a column that lands exactly on the crack between two build pieces (two
        // floors, a floor and a stair) traces straight through it to whatever is far below, a
        // 50 cm pit across the floor that the wheels dropped into and fell through (2026-10-08
        // 20:20, sky base floors). One column lower than (or missing between) two neighbours on
        // opposite sides that agree with each other is filled at their height. On top of the map
        // only: under it (superdude) the columns are untouched.
        if (bOnTop)
        {
            const std::vector<float> Col = Working;
            int Filled = 0;
            for (int I = 0; I < Size; I++)
                for (int J = 0; J < Size; J++)
                {
                    const float H = Col[size_t(I) * Size + J];
                    for (int Axis = 0; Axis < 2; Axis++)
                    {
                        const int I1 = Axis ? I : I - 1, J1 = Axis ? J - 1 : J, I2 = Axis ? I : I + 1, J2 = Axis ? J + 1 : J;
                        if (I1 < 0 || J1 < 0 || I2 >= Size || J2 >= Size) continue;
                        const float A = Col[size_t(I1) * Size + J1], B = Col[size_t(I2) * Size + J2];
                        if (std::isnan(A) || std::isnan(B) || std::fabs(A - B) > 20.f) continue;
                        if (std::isnan(H) || H < std::fmin(A, B) - 30.f)
                        {
                            Working[size_t(I) * Size + J] = (A + B) * 0.5f;
                            Filled++;
                            break;
                        }
                    }
                }
            static int Logged = 0;
            if (Filled && Logged++ < 40) Log("[collision] filled %d seam hole column(s) between build pieces", Filled);
        }
        FindCandidates();
        NextCandidate = 0;
        Edges.clear();
        bRefining = true;
    }
    static const int DI[4] = { 1, -1, 0, 0 }, DJ[4] = { 0, 0, 1, -1 };
    for (; NextCandidate < Candidates.size() && Budget > 0; NextCandidate++)
    {
        const Candidate& C = Candidates[NextCandidate];
        const float Top = Working[size_t(C.I) * Size + C.J];
        const float X0 = float(Centre.X) - Half + C.I * Spacing, Y0 = float(Centre.Y) - Half + C.J * Spacing;
        // Binary search between the top column (on the ledge) and the low one (off it).
        float Lo = 0.f, Hi = Spacing;
        for (int k = 0; k < 5; k++)
        {
            const float Mid = (Lo + Hi) * 0.5f;
            const float Z = TraceDown(WorldContext, X0 + DI[C.Dir] * Mid, Y0 + DJ[C.Dir] * Mid, Top + 40.f, Top - 15.f);
            if (!std::isnan(Z) && std::abs(Z - Top) < 12.f) Lo = Mid; else Hi = Mid;
        }
        Budget -= 3;  // the traces are short; count them a little cheaper than full columns
        Edges.push_back({ X0 + DI[C.Dir] * Lo, Y0 + DJ[C.Dir] * Lo, Top, C.Dir, Lo });
    }
    if (NextCandidate < Candidates.size())
        return false;

    // Grind spots on the world's meshes: gather once, then check a few edges per call.
    if (!bShapesGathered)
    {
        bShapesGathered = true;
        ShapeStats = {};
        std::vector<GrindGeom::Edge> All;
        AActor* Self = WorldContext && WorldContext->IsA(AActor::StaticClass()) ? static_cast<AActor*>(WorldContext) : nullptr;
        StairTriangles.clear();
        BuildStairSteps.clear();
        BuildStairRamps.clear();
        ShapeRails::Gather(WorldContext, FVector{ Centre.X, Centre.Y, Feet.Z }, Radius * 1.3f, Self, All, StairTriangles, ShapeStats,
                           &BuildStairSteps, &BuildStairRamps);
        ShapeCandidates.clear();
        const float FeetZ = float(Feet.Z);
        for (const GrindGeom::Edge& E : All)
        {
            const float Mx = (E.A.X + E.B.X) * 0.5f - float(Centre.X), My = (E.A.Y + E.B.Y) * 0.5f - float(Centre.Y);
            const float Mz = (E.A.Z + E.B.Z) * 0.5f;
            if (Mx * Mx + My * My > Radius * Radius * 1.69f || Mz < FeetZ - 600.f || Mz > FeetZ + 900.f) continue;
            ShapeCandidates.push_back(E);
        }
        // Nearest first, and a sane cap for dense areas.
        auto Dist = [&](const GrindGeom::Edge& E) {
            const float X = (E.A.X + E.B.X) * 0.5f - float(Feet.X), Y = (E.A.Y + E.B.Y) * 0.5f - float(Feet.Y), Z = (E.A.Z + E.B.Z) * 0.5f - FeetZ;
            return X * X + Y * Y + Z * Z;
        };
        std::sort(ShapeCandidates.begin(), ShapeCandidates.end(),
                  [&](const GrindGeom::Edge& L, const GrindGeom::Edge& R) { return Dist(L) < Dist(R); });
        if (ShapeCandidates.size() > 1500) ShapeCandidates.resize(1500);
        ShapeFound.clear();
        ShapeWidths.clear();
        NextShape = 0;
        Budget -= 20;
    }
    for (; NextShape < ShapeCandidates.size() && Budget > 0; NextShape++)
    {
        float Width = 0.f;
        if (CheckShapeEdge(WorldContext, ShapeCandidates[NextShape], &Width))
        {
            ShapeFound.push_back(ShapeCandidates[NextShape]);
            ShapeWidths.push_back(Width);
        }
        Budget -= 2;
    }
    if (NextShape < ShapeCandidates.size())
        return false;

    Finished.swap(Working);
    FinishedCentre = Centre;
    bHasPatch = true;   // (GroundAt reads the finished columns below)
    BuildRailChains();
    MergeShapeRails();
    FinishedStairTriangles.swap(StairTriangles);
    if (!BuildStairSteps.empty())
    {
        static size_t LastSteps = 0, LastRamps = 0;
        static bool bLastTop = false;
        if (BuildStairSteps.size() != LastSteps || BuildStairRamps.size() != LastRamps || bOnTop != bLastTop)
            Log("[stairs] player-built stairs: %zu step triangles, %zu smooth-ramp triangles (%s), on top of the map %d",
                BuildStairSteps.size() / 9, BuildStairRamps.size() / 9,
                BuildStairRamps == BuildStairSteps ? "no ramp: steps kept" : "clean ramp wedges", int(bOnTop));
        LastSteps = BuildStairSteps.size(); LastRamps = BuildStairRamps.size(); bLastTop = bOnTop;
    }
    FinishedBuildStairs.swap(bOnTop ? BuildStairRamps : BuildStairSteps);
    if (bOnTop) DropRailsAcrossStairEnds();
    LastShapeStats = ShapeStats;
    bRefining = false;
    bBusy = false;
    bHasPatch = true;
    return true;
}

// A ledge cell: flat on top (level with the column behind it and at least one beside it) with a
// clear step down (12 cm .. 3 m) to the next column. Hills and gentle slopes never qualify.
void WorldSampler::FindCandidates()
{
    Candidates.clear();
    static const int DI[4] = { 1, -1, 0, 0 }, DJ[4] = { 0, 0, 1, -1 };
    auto H = [&](int I, int J) {
        return (I < 0 || J < 0 || I >= Size || J >= Size) ? std::numeric_limits<float>::quiet_NaN() : Working[size_t(I) * Size + J];
    };
    for (int I = 1; I + 1 < Size; I++)
        for (int J = 1; J + 1 < Size; J++)
        {
            const float Top = H(I, J);
            if (std::isnan(Top)) continue;
            for (int d = 0; d < 4; d++)
            {
                const float Low = H(I + DI[d], J + DJ[d]);
                const float Back = H(I - DI[d], J - DJ[d]);
                const float SideA = H(I + DJ[d], J + DI[d]), SideB = H(I - DJ[d], J - DI[d]);
                if (std::isnan(Back)) continue;
                const float Drop = std::isnan(Low) ? 1000.f : Top - Low;
                if (Drop < 12.f || Drop > 300.f) continue;
                // A sharp convex edge: the top behind the lip rides at most ~35 degrees, and the
                // drop is much steeper than it (natural terrain is never perfectly flat).
                const float TopRise = std::abs(Back - Top);
                if (TopRise > 35.f || Drop < 2.f * TopRise + 12.f) continue;
                const bool bSideA = !std::isnan(SideA) && std::abs(SideA - Top) < 25.f;
                const bool bSideB = !std::isnan(SideB) && std::abs(SideB - Top) < 25.f;
                if (!bSideA && !bSideB) continue;                             // a lip, not a lone spike
                Candidates.push_back({ I, J, d });
            }
        }
}

// Chains refined lip points that face the same way into rails, then straightens them.
bool WorldSampler::ThinRaisedSolid(const GrindGeom::Edge& Rail) const
{
    if (!Rail.Solid || Rail.Solid->empty()) return false;
    const std::vector<float>& T = *Rail.Solid;
    const float Hx = Rail.B.X - Rail.A.X, Hy = Rail.B.Y - Rail.A.Y, HL = std::sqrt(Hx * Hx + Hy * Hy);
    if (HL < 1.f) return false;
    const float Nx = -Hy / HL, Ny = Hx / HL;   // horizontal, across the rail
    float Lo = 1e30f, Hi = -1e30f, Top = -1e30f, Bottom = 1e30f;
    for (size_t k = 0; k + 2 < T.size(); k += 3)
    {
        const float Across = T[k] * Nx + T[k + 1] * Ny;
        Lo = std::min(Lo, Across);
        Hi = std::max(Hi, Across);
        Top = std::max(Top, T[k + 2]);
        Bottom = std::min(Bottom, T[k + 2]);
    }
    if (Hi - Lo > 40.f || Top - Bottom > 400.f) return false;
    // Raised: the rail stands clear of the ground the columns found under its middle.
    const float Mx = (Rail.A.X + Rail.B.X) * 0.5f, My = (Rail.A.Y + Rail.B.Y) * 0.5f, Mz = (Rail.A.Z + Rail.B.Z) * 0.5f;
    const float Ground = GroundAt(Mx, My);
    return !std::isnan(Ground) && Mz - Ground >= 20.f;
}

void WorldSampler::AddStrip(const FVector& A, const FVector& B, float InX, float InY, float WA, float WB)
{
    // Only for a patch scanned on top of the world: under the map (superdude and other clips) the
    // collision stays exactly what the column grid gives.
    if (!bOnTop || (WA < 1.f && WB < 1.f)) return;
    const float Ax = float(A.X), Ay = float(A.Y), Az = float(A.Z), Bx = float(B.X), By = float(B.Y), Bz = float(B.Z);
    const float P[4][3] = { { Ax, Ay, Az }, { Bx, By, Bz }, { Bx + InX * WB, By + InY * WB, Bz }, { Ax + InX * WA, Ay + InY * WA, Az } };
    // Two triangles, wound so that (b-a)x(c-a) points up.
    static const int Tris[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    for (const auto& T : Tris)
    {
        const float* a = P[T[0]];
        const float* b = P[T[1]];
        const float* c = P[T[2]];
        const float Nz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        if (std::abs(Nz) < 1e-3f) continue;
        const float* Order[3] = { a, Nz > 0 ? b : c, Nz > 0 ? c : b };
        for (const float* V : Order) FinishedStrips.insert(FinishedStrips.end(), V, V + 3);
    }
}

void WorldSampler::BuildRailChains()
{
    FinishedRails.clear();
    FinishedStrips.clear();
    const size_t N = Edges.size();
    std::vector<bool> Used(N, false);
    const float Link = Spacing * 1.6f;
    auto Near = [&](size_t a, size_t b) {
        const Edge& A = Edges[a];
        const Edge& B = Edges[b];
        return A.Dir == B.Dir && std::abs(A.Z - B.Z) < 30.f &&
               (A.X - B.X) * (A.X - B.X) + (A.Y - B.Y) * (A.Y - B.Y) < Link * Link;
    };
    for (size_t s0 = 0; s0 < N; s0++)
    {
        if (Used[s0]) continue;
        // Grow a chain both ways from s0, always taking the nearest unused linked point.
        std::vector<size_t> Chain{ s0 };
        Used[s0] = true;
        for (int Side = 0; Side < 2; Side++)
            for (;;)
            {
                const size_t End = Side == 0 ? Chain.back() : Chain.front();
                size_t Best = N;
                float BestD = 1e30f;
                for (size_t k = 0; k < N; k++)
                    if (!Used[k] && Near(End, k))
                    {
                        const float Dx = Edges[k].X - Edges[End].X, Dy = Edges[k].Y - Edges[End].Y;
                        const float D = Dx * Dx + Dy * Dy;
                        if (D < BestD) { BestD = D; Best = k; }
                    }
                if (Best == N) break;
                Used[Best] = true;
                if (Side == 0) Chain.push_back(Best); else Chain.insert(Chain.begin(), Best);
            }
        if (Chain.size() < 2) continue;  // a single point is not an edge
        static const float SX[4] = { 1, -1, 0, 0 }, SY[4] = { 0, 0, 1, -1 };
        for (size_t k = 0; k + 1 < Chain.size(); k++)
        {
            const Edge& P0 = Edges[Chain[k]];
            const Edge& P1 = Edges[Chain[k + 1]];
            AddStrip(FVector{ P0.X, P0.Y, P0.Z }, FVector{ P1.X, P1.Y, P1.Z }, -SX[P0.Dir], -SY[P0.Dir],
                     std::min(P0.W, 25.f), std::min(P1.W, 25.f));
        }
        std::vector<FVector> Pts;
        for (size_t k : Chain) Pts.push_back(FVector{ Edges[k].X, Edges[k].Y, Edges[k].Z });
        // Douglas-Peucker (5 cm): straight edges become two-point rails, corners keep their bend.
        std::vector<bool> Keep(Pts.size(), false);
        Keep.front() = Keep.back() = true;
        std::vector<std::pair<size_t, size_t>> Stack{ { 0, Pts.size() - 1 } };
        while (!Stack.empty())
        {
            const auto Seg = Stack.back();
            Stack.pop_back();
            const size_t a = Seg.first, b = Seg.second;
            const FVector& A = Pts[a];
            const FVector& B = Pts[b];
            const double Lx = B.X - A.X, Ly = B.Y - A.Y, Lz = B.Z - A.Z, L2 = Lx * Lx + Ly * Ly + Lz * Lz;
            double Worst = 0;
            size_t W = a;
            for (size_t k = a + 1; k < b; k++)
            {
                const double Px = Pts[k].X - A.X, Py = Pts[k].Y - A.Y, Pz = Pts[k].Z - A.Z;
                const double T = L2 > 0 ? std::fmax(0.0, std::fmin(1.0, (Px * Lx + Py * Ly + Pz * Lz) / L2)) : 0;
                const double Dx = Px - T * Lx, Dy = Py - T * Ly, Dz = Pz - T * Lz;
                const double D = Dx * Dx + Dy * Dy + Dz * Dz;
                if (D > Worst) { Worst = D; W = k; }
            }
            if (Worst > 5.0 * 5.0)
            {
                Keep[W] = true;
                Stack.push_back({ a, W });
                Stack.push_back({ W, b });
            }
        }
        std::vector<FVector> Rail;
        for (size_t k = 0; k < Pts.size(); k++)
            if (Keep[k]) Rail.push_back(Pts[k]);
        FinishedRails.push_back(std::move(Rail));
    }
}

// Player-built stairs ride as smooth ramps, but the ground grid traces through them, so the
// floor piece at the top of a flight (and the ground at its foot) looked like a ledge: a grind
// rail along it caught the board / threw it into the air rolling from the floor onto the stairs
// (2026-10-08 17:45 clip). Rail pieces running ACROSS a ramp's slope within 50 cm of it and
// within 40 cm of its height are dropped (the surface continues there); rails along a ramp's
// sides stay grindable. On top of the map only.
void WorldSampler::DropRailsAcrossStairEnds()
{
    if (FinishedBuildStairs.empty() || FinishedRails.empty()) return;
    struct Slope { float X[3], Y[3], Z[3], Gx, Gy; };  // a ramp's sloped face and its down-slope direction
    std::vector<Slope> Faces;
    for (size_t t = 0; t + 8 < FinishedBuildStairs.size(); t += 9)
    {
        const float* V = &FinishedBuildStairs[t];
        const float Ax = V[3] - V[0], Ay = V[4] - V[1], Az = V[5] - V[2], Bx = V[6] - V[0], By = V[7] - V[1], Bz = V[8] - V[2];
        float Nx = Ay * Bz - Az * By, Ny = Az * Bx - Ax * Bz, Nz = Ax * By - Ay * Bx;
        if (Nz < 0) { Nx = -Nx; Ny = -Ny; Nz = -Nz; }
        const float NL = std::sqrt(Nx * Nx + Ny * Ny + Nz * Nz), HL = std::sqrt(Nx * Nx + Ny * Ny);
        if (NL < 1e-3f || Nz / NL < 0.3f || HL / NL < 0.2f) continue;  // the sloped top only
        Faces.push_back({ { V[0], V[3], V[6] }, { V[1], V[4], V[7] }, { V[2], V[5], V[8] }, Nx / HL, Ny / HL });
    }
    if (Faces.empty()) return;
    auto NearEnd = [&](const FVector& P, float Dx, float Dy) -> bool
    {
        for (const Slope& F : Faces)
        {
            if (std::fabs(Dx * F.Gx + Dy * F.Gy) > 0.5f) continue;  // runs along this ramp: a side edge
            const float Mx = std::fmin(F.X[0], std::fmin(F.X[1], F.X[2])) - 50.f, Xx = std::fmax(F.X[0], std::fmax(F.X[1], F.X[2])) + 50.f;
            const float My = std::fmin(F.Y[0], std::fmin(F.Y[1], F.Y[2])) - 50.f, Xy = std::fmax(F.Y[0], std::fmax(F.Y[1], F.Y[2])) + 50.f;
            const float Mz = std::fmin(F.Z[0], std::fmin(F.Z[1], F.Z[2])) - 40.f, Xz = std::fmax(F.Z[0], std::fmax(F.Z[1], F.Z[2])) + 40.f;
            if (P.X < Mx || P.X > Xx || P.Y < My || P.Y > Xy || P.Z < Mz || P.Z > Xz) continue;
            // Near one of the face's lowest or highest corners' heights: its foot or its top.
            const float Lo = Mz + 40.f, Hi = Xz - 40.f;
            if (std::fabs(P.Z - Lo) <= 40.f || std::fabs(P.Z - Hi) <= 40.f) return true;
        }
        return false;
    };
    std::vector<std::vector<FVector>> Kept;
    int Dropped = 0;
    for (const auto& R : FinishedRails)
    {
        std::vector<FVector> Cur;
        for (size_t i = 0; i + 1 < R.size(); i++)
        {
            const FVector& A = R[i];
            const FVector& B = R[i + 1];
            const float Dx = float(B.X - A.X), Dy = float(B.Y - A.Y), L = std::sqrt(Dx * Dx + Dy * Dy);
            const FVector Mid{ (A.X + B.X) * 0.5, (A.Y + B.Y) * 0.5, (A.Z + B.Z) * 0.5 };
            const bool bDrop = L > 1.f && NearEnd(Mid, Dx / L, Dy / L);
            if (bDrop)
            {
                Dropped++;
                if (Cur.size() >= 2) Kept.push_back(Cur);
                Cur.clear();
                continue;
            }
            if (Cur.empty()) Cur.push_back(A);
            Cur.push_back(B);
        }
        if (Cur.size() >= 2) Kept.push_back(Cur);
    }
    if (Dropped)
    {
        static int Logged = 0;
        if (Logged++ < 20) Log("[stairs] dropped %d grind-rail piece(s) across the ends of player-built stairs", Dropped);
        FinishedRails.swap(Kept);
    }
}

void WorldSampler::BuildRails(std::vector<float>& Points, std::vector<uint32_t>& Lengths) const
{
    Points.clear();
    Lengths.clear();
    if (!bHasPatch) return;
    for (const auto& R : FinishedRails)
    {
        for (const FVector& P : R)
        {
            Points.push_back(float(P.X));
            Points.push_back(float(P.Y));
            Points.push_back(float(P.Z));
        }
        Lengths.push_back(uint32_t(R.size()));
    }
}


bool WorldSampler::NeedsRecentre(const FVector& At) const
{
    return NeedsRecentre(At, 300.0);
}

bool WorldSampler::NeedsRecentre(const FVector& At, double MaxRiseOrDrop) const
{
    if (!bHasPatch)
        return true;
    const double DX = At.X - FinishedCentre.X, DY = At.Y - FinishedCentre.Y;
    return DX * DX + DY * DY > 450.0 * 450.0 || std::abs(At.Z - FinishedCentre.Z) > MaxRiseOrDrop;
}

void WorldSampler::BuildTriangles(std::vector<float>& Out) const
{
    Out.clear();
    if (!bHasPatch)
        return;

    const float Half = Radius;
    auto Point = [&](int I, int J, float* P) -> bool
    {
        const float Z = Finished[size_t(I) * Size + J];
        if (std::isnan(Z)) return false;
        P[0] = float(FinishedCentre.X) - Half + I * Spacing;
        P[1] = float(FinishedCentre.Y) - Half + J * Spacing;
        P[2] = Z;
        return true;
    };

    float A[3], B[3], C[3], D[3];
    for (int I = 0; I + 1 < Size; I++)
    {
        for (int J = 0; J + 1 < Size; J++)
        {
            // (I,J) -> (I+1,J) -> (I+1,J+1) -> (I,J+1): +X then +Y, so (b-a)x(c-a) points up.
            if (!Point(I, J, A) || !Point(I + 1, J, B) || !Point(I + 1, J + 1, C) || !Point(I, J + 1, D))
                continue;
            for (const float* P : { A, B, C, A, C, D })
                Out.insert(Out.end(), P, P + 3);
        }
    }
    Out.insert(Out.end(), FinishedStairTriangles.begin(), FinishedStairTriangles.end());
    Out.insert(Out.end(), FinishedBuildStairs.begin(), FinishedBuildStairs.end());
    Out.insert(Out.end(), FinishedStrips.begin(), FinishedStrips.end());
    Out.insert(Out.end(), FinishedSolids.begin(), FinishedSolids.end());
}

float WorldSampler::GroundAt(float X, float Y) const
{
    if (!bHasPatch) return Miss;
    const float Fx = (X - (float(FinishedCentre.X) - Radius)) / Spacing;
    const float Fy = (Y - (float(FinishedCentre.Y) - Radius)) / Spacing;
    const int I = int(std::floor(Fx)), J = int(std::floor(Fy));
    if (I < 0 || J < 0 || I + 1 >= Size || J + 1 >= Size) return Miss;
    const float A = Finished[size_t(I) * Size + J], B = Finished[size_t(I + 1) * Size + J];
    const float C = Finished[size_t(I) * Size + J + 1], D = Finished[size_t(I + 1) * Size + J + 1];
    if (std::isnan(A) || std::isnan(B) || std::isnan(C) || std::isnan(D)) return Miss;
    const float U = Fx - I, V = Fy - J;
    return (A * (1 - U) + B * U) * (1 - V) + (C * (1 - U) + D * U) * V;
}

bool WorldSampler::CheckShapeEdge(UObject* WorldContext, const GrindGeom::Edge& E, float* TopWidth) const
{
    *TopWidth = 0.f;
    const float Mx = (E.A.X + E.B.X) * 0.5f, My = (E.A.Y + E.B.Y) * 0.5f, Mz = (E.A.Z + E.B.Z) * 0.5f;
    // Outward direction of the edge's side face, flattened (capsule rails have none).
    float Ox = E.Side.X, Oy = E.Side.Y;
    const float OL = std::sqrt(Ox * Ox + Oy * Oy);
    if (OL > 0.05f) { Ox /= OL; Oy /= OL; } else { Ox = Oy = 0.f; }
    // Clear on top: from above, the first thing hit just inside the edge is the edge's own top.
    const float Top = TraceDown(WorldContext, Mx - Ox * 4.f, My - Oy * 4.f, Mz + 80.f, Mz - 15.f);
    if (std::isnan(Top) || std::abs(Top - Mz) > 12.f) return false;
    if (OL <= 0.05f) return true;
    // Open below the outer side: whatever is under it lies clearly lower (not flush, not buried).
    const float Below = TraceDown(WorldContext, Mx + Ox * 15.f, My + Oy * 15.f, Mz - 2.f, Mz - 400.f);
    if (!(std::isnan(Below) || Below < Mz - 12.f)) return false;
    // How far the flat top reaches inward (for the lip strip).
    *TopWidth = 4.f;
    for (const float D : { 12.f, 25.f })
    {
        const float Z = TraceDown(WorldContext, Mx - Ox * D, My - Oy * D, Mz + 40.f, Mz - 15.f);
        if (std::isnan(Z) || std::abs(Z - Mz) > 3.f) break;
        *TopWidth = D;
    }
    return true;
}

// Shape edges become two-point rails. Thin rails and fence tops have two parallel top edges a
// few centimetres apart: they become one rail along their middle.
void WorldSampler::MergeShapeRails()
{
    std::vector<bool> Gone(ShapeFound.size(), false);
    for (size_t i = 0; i < ShapeFound.size(); i++)
    {
        if (Gone[i]) continue;
        GrindGeom::Edge Rail = ShapeFound[i];
        const float Dx = Rail.B.X - Rail.A.X, Dy = Rail.B.Y - Rail.A.Y, Dz = Rail.B.Z - Rail.A.Z;
        const float L = std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
        if (L < 1.f) continue;
        float PairGap = -1.f;  // distance to the merged parallel edge, if any
        for (size_t j = i + 1; j < ShapeFound.size(); j++)
        {
            if (Gone[j]) continue;
            GrindGeom::Edge O = ShapeFound[j];
            const float Ex = O.B.X - O.A.X, Ey = O.B.Y - O.A.Y, Ez = O.B.Z - O.A.Z;
            const float OLn = std::sqrt(Ex * Ex + Ey * Ey + Ez * Ez);
            if (OLn < 1.f) continue;
            const float Cos = (Dx * Ex + Dy * Ey + Dz * Ez) / (L * OLn);
            if (std::abs(Cos) < 0.98f) continue;
            if (Cos < 0) std::swap(O.A, O.B);
            // Distance between the lines (midpoint of O to Rail) and overlap along Rail.
            const float Px = (O.A.X + O.B.X) * 0.5f - Rail.A.X, Py = (O.A.Y + O.B.Y) * 0.5f - Rail.A.Y, Pz = (O.A.Z + O.B.Z) * 0.5f - Rail.A.Z;
            const float T = (Px * Dx + Py * Dy + Pz * Dz) / (L * L);
            const float Cx = Px - T * Dx, Cy = Py - T * Dy, Cz = Pz - T * Dz;
            if (Cx * Cx + Cy * Cy > 15.f * 15.f || std::abs(Cz) > 10.f || T < 0.f || T > 1.f) continue;
            PairGap = std::max(PairGap, std::sqrt(Cx * Cx + Cy * Cy));
            Rail.A = { (Rail.A.X + O.A.X) * 0.5f, (Rail.A.Y + O.A.Y) * 0.5f, std::max(Rail.A.Z, O.A.Z) };
            Rail.B = { (Rail.B.X + O.B.X) * 0.5f, (Rail.B.Y + O.B.Y) * 0.5f, std::max(Rail.B.Z, O.B.Z) };
            Gone[j] = true;
        }
        FinishedRails.push_back({ FVector{ Rail.A.X, Rail.A.Y, Rail.A.Z }, FVector{ Rail.B.X, Rail.B.Y, Rail.B.Z } });
        {
            const float Hx = Rail.B.X - Rail.A.X, Hy = Rail.B.Y - Rail.A.Y, HL = std::sqrt(Hx * Hx + Hy * Hy);
            if (HL > 1.f && !(bOnTop && ThinRaisedSolid(Rail)))   // a fence or railing gets its real shape instead
            {
                const float Nx = -Hy / HL, Ny = Hx / HL;   // horizontal, across the rail
                const FVector A{ Rail.A.X, Rail.A.Y, Rail.A.Z }, B{ Rail.B.X, Rail.B.Y, Rail.B.Z };
                float Sx = Rail.Side.X, Sy = Rail.Side.Y;
                const float SL = std::sqrt(Sx * Sx + Sy * Sy);
                if (PairGap >= 0.f || SL <= 0.05f)
                {
                    // Thin top (two merged edges) or a round rail: centred on the rail line.
                    const float Half = PairGap >= 0.f ? std::clamp(PairGap * 0.5f, 3.f, 7.5f) : 3.f;
                    const FVector A0{ A.X - Nx * Half, A.Y - Ny * Half, A.Z }, B0{ B.X - Nx * Half, B.Y - Ny * Half, B.Z };
                    AddStrip(A0, B0, Nx, Ny, Half * 2.f, Half * 2.f);
                }
                else
                {
                    Sx /= SL; Sy /= SL;
                    const float W = std::min(ShapeWidths[i], 25.f);
                    AddStrip(A, B, -Sx, -Sy, W, W);
                }
            }
        }
    }
    LastShapeRails = int(ShapeFound.size());
    FinishedSolids.clear();
    if (bOnTop)
    {
        std::vector<const std::vector<float>*> Added;
        for (const GrindGeom::Edge& E : ShapeFound)
        {
            const std::vector<float>* T = E.Solid.get();
            if (!T || T->empty() || std::find(Added.begin(), Added.end(), T) != Added.end() || !ThinRaisedSolid(E)) continue;
            Added.push_back(T);
            FinishedSolids.insert(FinishedSolids.end(), T->begin(), T->end());
        }
    }
}
