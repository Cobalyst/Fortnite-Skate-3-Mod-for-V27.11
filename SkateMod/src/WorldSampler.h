#pragma once

#include <string>
#include <unordered_set>

#include "Common.h"
#include "ShapeRails.h"
#include <vector>

// Builds skateable collision around the skater from line traces against the live 27.11 world.
//
// A square grid of columns centred on the skater is traced straight down, twice per column:
// once from just above head height (finds the floor you are on, even indoors) and once from
// high above (finds tops of boxes, ledges and steps the low trace started inside). The two
// are merged into one height per column, and neighbouring columns become triangles, so
// height jumps between columns turn into walls.
//
// Tracing is spread over frames (Step) so the game does not hitch while skating.
class WorldSampler
{
public:
    static constexpr float Spacing = 50.f;     // cm between columns
    static constexpr float Radius = 1200.f;    // cm from centre to edge
    static constexpr int   Size = int(2 * Radius / Spacing) + 1;

    // Starts a new patch around Feet (skater feet position). Skater = where the skater actually
    // is (the patch centre leads it in the direction of travel); defaults to Feet.
    void Begin(const SDK::FVector& Feet, const SDK::FVector* Skater = nullptr);

    // Traces up to Budget columns. Returns true once the whole patch is done.
    bool Step(SDK::UObject* WorldContext, int Budget);

    bool Busy() const { return bBusy; }

    // Rocks, cliffs and walls taller than the trace starts (mod_settings.txt: solid_walls).
    // A line trace that starts inside a mesh passes through it, so next to a tall rock both
    // traces start inside it and only find the ground underneath. With this on, each column
    // also traces from high above; a much higher hit with nothing overhead between the ground
    // and it (no underside) is solid all the way up, so the column takes its top.
    bool bSolidWalls = false;  // off by default: it blocks clipping glitches (superdude)
    // The same solid-hill handling, but only for a patch scanned while the skater stands on top
    // of the world (nothing solid above its feet). Under the map (superdude and other clips) the
    // patch is scanned exactly as with solid_walls off. Without it a hill mesh resting on flat
    // ground had no collision: both traces started inside it and found the ground underneath.
    bool bHillsWhenOnTop = true;
    bool bOnTop = false;       // this patch: skater found on top of the world
    std::string OnTopWhy;
    void NoteWall(const std::string& What);
    std::unordered_set<std::string> WallNames;  // objects already logged as walls
    bool HasPatch() const { return bHasPatch; }
    bool HasBuildStairs() const { return !FinishedBuildStairs.empty(); }
    SDK::FVector PatchCentre() const { return FinishedCentre; }   // the finished patch's centre (Unreal cm)
    static constexpr double PatchRadius() { return Radius; }

    // True when Feet is far enough from the current patch centre that a new one is due.
    bool NeedsRecentre(const SDK::FVector& Feet) const;
    // Same, with the vertical distance that triggers a new patch (default 300 cm).
    bool NeedsRecentre(const SDK::FVector& Feet, double MaxRiseOrDrop) const;

    // Triangles (9 floats each, Unreal cm, wound with the face up) of the last finished patch.
    void BuildTriangles(std::vector<float>& Out) const;

    // Grind rails along the ledges of the last finished patch: points (3 floats each, cm) and the
    // point count of each rail. Ledges are where the ground steps down from a flat top (curbs,
    // boxes, steps, wall tops); each edge point is refined with traces to the real lip.
    void BuildRails(std::vector<float>& Points, std::vector<uint32_t>& Lengths) const;
    int RailCount() const { return int(FinishedRails.size()); }
    const std::vector<std::vector<SDK::FVector>>& Rails() const { return FinishedRails; }
    // What the last patch found on the world's meshes (log line).
    std::string ShapeSummary() const { return LastShapeStats.Summary() + ", " + std::to_string(LastShapeRails) + " grindable"; }

    // Height of the scanned ground at (X, Y) (bilinear between columns), or NaN outside the patch
    // or over a hole.
    float GroundAt(float X, float Y) const;

    int Traced() const { return Next; }

private:
    // TraceDown that carries on below any tree it lands on (trees are added as their own
    // collision shapes instead: a 50 cm column grid turned a thin trunk into a wide pillar).
    // bInside (optional): the trace started inside a collision shape, and the "hit" is just its
    // start point, not a surface.
    float TraceDownPastTrees(SDK::UObject* WorldContext, float X, float Y, float Top, float Bottom, bool* bInside = nullptr) const;
    float TraceDown(SDK::UObject* WorldContext, float X, float Y, float Top, float Bottom,
                    SDK::UPrimitiveComponent** HitComp = nullptr, bool* bInside = nullptr, bool bPawn = false) const;

    SDK::FVector Centre{};
    SDK::FVector Feet{};
    SDK::FVector SkaterAt{};
    std::vector<float> Working;   // being traced
    std::vector<float> Finished;  // last complete patch
    struct Edge { float X, Y, Z; int Dir; float W; };  // refined lip point; Dir 0..3 = drop toward +X,-X,+Y,-Y;
                                                         // W = flat top traced inward from the lip (cm)
    struct Candidate { int I, J, Dir; };
    std::vector<Candidate> Candidates;            // ledge cells waiting for refinement
    std::vector<Edge> Edges;
    size_t NextCandidate = 0;
    bool bRefining = false;
    std::vector<std::vector<SDK::FVector>> FinishedRails;
    // Grind lips: the 50 cm column grid turns a ledge's sharp edge into a ramp up to 50 cm wide,
    // and the skate engine dropped every grind there at once (the board met the ramp, not the
    // ledge top). Each rail gets a thin flat strip of the real top along it (inward from the lip,
    // at most 25 cm, never past what traces found; ~6 cm centred for round rails). No walls.
    std::vector<float> FinishedStrips;  // triangles, face up
    // Fences, railings, handrails and posts: thin (<= 40 cm across the rail) and raised (rail
    // >= 20 cm above the scanned ground) collision elements with a grind spot keep their real
    // collision shape. The column grid cannot see them at all (no collision, grinds on nothing).
    // Only for a patch scanned on top of the world; nothing at or below ground level.
    std::vector<float> FinishedSolids;
    bool ThinRaisedSolid(const GrindGeom::Edge& Rail) const;
    void AddStrip(const SDK::FVector& A, const SDK::FVector& B, float InX, float InY, float WA, float WB);
    void FindCandidates();
    // Grind edges of the world's meshes (ShapeRails), checked a few per call: clear on top
    // (nothing built over it) and open below its outer side (not flush with the ground).
    // Width (cm) of the flat top traced inward from the edge goes to TopWidth (0 = a capsule rail).
    bool CheckShapeEdge(SDK::UObject* WorldContext, const GrindGeom::Edge& E, float* TopWidth) const;
    std::vector<float> ShapeWidths;   // per ShapeFound entry
    void MergeShapeRails();
    std::vector<GrindGeom::Edge> ShapeCandidates;
    std::vector<GrindGeom::Edge> ShapeFound;
    std::vector<float> StairTriangles, FinishedStairTriangles;  // stairs' own collision (ShapeRails)
    // Player-built stairs: their stepped collision, or one smooth ramp over the steps. A patch
    // scanned on top of the world rides the smooth ramp; under the map the steps (as before).
    std::vector<float> BuildStairSteps, BuildStairRamps, FinishedBuildStairs;
    size_t NextShape = 0;
    bool bShapesGathered = false;
    ShapeRails::Stats ShapeStats, LastShapeStats;
    int LastShapeRails = 0;
    void BuildRailChains();
    void DropRailsAcrossStairEnds();
    SDK::FVector FinishedCentre{};
    int Next = 0;
    bool bBusy = false;
    bool bHasPatch = false;
};
