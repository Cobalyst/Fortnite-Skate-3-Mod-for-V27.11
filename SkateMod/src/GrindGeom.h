#pragma once

// Grindable edges of collision shapes (boxes and convex hulls of Fortnite's buildings, rocks,
// fences and props). A grind spot is a convex edge where an upward-facing face meets another face
// at a sharp angle (Fortnite's meshes are blocky, so most are near 90 degrees), the edge itself no
// steeper than a rail Skate 3 would ride. No engine or game types: testable offline.

#include <memory>
#include <vector>

namespace GrindGeom
{
    struct V3 { float X, Y, Z; };

    struct Edge
    {
        V3 A, B;
        V3 Top;   // normal of the upward-facing face
        V3 Side;  // normal of the other face
        // The collision element the edge belongs to, as triangles (9 floats each, wound outward).
        std::shared_ptr<const std::vector<float>> Solid;
    };

    struct Rules
    {
        float MinTopUp = 0.5f;        // top face normal Z at least this (up to 60 degrees tilt)
        float MaxFaceDot = 0.64f;     // faces at least ~50 degrees apart (cos 50)
        float MaxSlope = 1.0f;        // edge rise / run at most this (45 degrees)
        float MinLength = 50.f;       // cm
    };

    // Faces of a convex point set as index lists (points on each face). Computed once per shape
    // in its own space; any affine transform of the points keeps the same faces.
    using Faces = std::vector<std::vector<int>>;
    bool HullFaces(const std::vector<V3>& Points, Faces& Out, size_t MaxPoints = 64);

    // Grindable edges between pairs of faces, for the points placed in the world.
    void Edges(const std::vector<V3>& World, const Faces& F, const Rules& R, std::vector<Edge>& Out);

    // The shape's faces as triangles (9 floats each), wound so (b-a)x(c-a) points outward.
    void Triangles(const std::vector<V3>& World, const Faces& F, std::vector<float>& Out);

    // A box's 8 corners (centre, three half-extent axes).
    void BoxCorners(const V3& Centre, const V3 Axis[3], std::vector<V3>& Out);
}
