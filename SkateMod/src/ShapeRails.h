#pragma once

// Grind spots from the collision shapes of the world around the skater: every static mesh
// (buildings, player builds, rocks, fences, railings, props, including instanced meshes) whose
// simple collision is boxes, convex hulls or capsules contributes its sharp upper edges
// (GrindGeom). Results per mesh instance are cached: the world's meshes don't move.

#include "Common.h"
#include "GrindGeom.h"

#include <string>
#include <vector>

namespace ShapeRails
{
    struct Stats
    {
        int Components = 0, Instances = 0, Shapes = 0, NoSimpleCollision = 0, TooComplex = 0, Edges = 0;
        int Stairs = 0, StairTriangles = 0, Trees = 0, TreeTriangles = 0;
        std::string Summary() const;
    };

    // Candidate grind edges (world space, cm) of meshes overlapping the sphere. Not yet checked
    // against the world (that is WorldSampler's job: clear on top, open below).
    // Stairs (anything named "stair": map stairs and the stair build pieces) also give their own
    // collision shapes as triangles (9 floats each, wound outward): the column scan cannot see
    // open steps or treads above the trace starts, so the skater would fall through them.
    // Trees (the column scan looks through them) add their player-blocking collision the same way,
    // so trunks are solid exactly where they are, with no wall around them. No grind spots.
    // A tree the column scan may look straight through: named as a tree (whole name parts only,
    // so "Street" or "PowerPlant" never count) and either not blocking players at all, or blocking
    // them with simple collision shapes that Gather adds back as solid triangles.
    bool IsPassThroughTree(SDK::UPrimitiveComponent* Comp);

    // Player-built stairs ("PBW" build pieces) go to BuildStairSteps (their own stepped collision)
    // and BuildStairRamps (one smooth shell over all their steps: the convex hull of their
    // collision) instead of StairTriangles, so the caller can pick per patch.
    void Gather(SDK::UObject* WorldContext, const SDK::FVector& Centre, float Radius, SDK::AActor* Ignore,
                std::vector<GrindGeom::Edge>& Out, std::vector<float>& StairTriangles, Stats& S,
                std::vector<float>* BuildStairSteps = nullptr, std::vector<float>* BuildStairRamps = nullptr);
}
