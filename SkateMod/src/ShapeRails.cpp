#include "ShapeRails.h"

#include <cctype>
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

using namespace SDK;
using GrindGeom::V3;

namespace
{
    // Unit quaternion as Unreal stores it in FTransform / builds it from an FRotator.
    struct Quat { double X, Y, Z, W; };

    Quat FromRotator(const FRotator& R)
    {
        const double D = 3.14159265358979 / 360.0;  // degrees -> half radians
        const double SP = std::sin(R.Pitch * D), CP = std::cos(R.Pitch * D);
        const double SY = std::sin(R.Yaw * D), CY = std::cos(R.Yaw * D);
        const double SR = std::sin(R.Roll * D), CR = std::cos(R.Roll * D);
        return { CR * SP * SY - SR * CP * CY, -CR * SP * CY - SR * CP * SY, CR * CP * SY - SR * SP * CY, CR * CP * CY + SR * SP * SY };
    }

    V3 Rotate(const Quat& Q, double X, double Y, double Z)
    {
        // v + 2w(q x v) + 2 q x (q x v)
        const double Tx = 2 * (Q.Y * Z - Q.Z * Y), Ty = 2 * (Q.Z * X - Q.X * Z), Tz = 2 * (Q.X * Y - Q.Y * X);
        return { float(X + Q.W * Tx + (Q.Y * Tz - Q.Z * Ty)), float(Y + Q.W * Ty + (Q.Z * Tx - Q.X * Tz)), float(Z + Q.W * Tz + (Q.X * Ty - Q.Y * Tx)) };
    }

    // Point through an FTransform: scale, rotate, translate.
    V3 Apply(const FTransform& T, double X, double Y, double Z)
    {
        const Quat Q{ T.Rotation.X, T.Rotation.Y, T.Rotation.Z, T.Rotation.W };
        const V3 R = Rotate(Q, X * T.Scale3D.X, Y * T.Scale3D.Y, Z * T.Scale3D.Z);
        return { float(R.X + T.Translation.X), float(R.Y + T.Translation.Y), float(R.Z + T.Translation.Z) };
    }

    bool IsFoliageName(std::string N)
    {
        for (char& C : N) C = char(tolower(static_cast<unsigned char>(C)));
        static const char* Words[] = { "tree", "foliage", "leaf", "leaves", "bush", "shrub", "plant", "pine", "palm",
                                       "oak", "birch", "branch", "canopy", "flower", "grass", "hedge", "vine" };
        for (const char* W : Words)
            if (N.find(W) != std::string::npos) return true;
        return false;
    }

    // Tree names only, as whole parts of the name (split at '_', digits and other non-letters).
    bool IsTreeName(const std::string& N)
    {
        static const char* Words[] = { "tree", "trees", "pine", "palm", "oak", "birch", "spruce", "maple", "willow",
                                       "redwood", "cypress", "sapling", "conifer" };
        std::string Part;
        auto Check = [&]() {
            for (const char* W : Words)
                if (Part == W) return true;
            return false;
        };
        for (size_t i = 0; i <= N.size(); i++)
        {
            const char C = i < N.size() ? N[i] : ' ';
            const bool bLetter = std::isalpha(static_cast<unsigned char>(C)) != 0;
            // A new part starts at a non-letter or at a lower -> upper case step ("PineTree").
            const bool bCamel = bLetter && i > 0 && std::isupper(static_cast<unsigned char>(C)) &&
                                std::islower(static_cast<unsigned char>(N[i - 1]));
            if (!bLetter || bCamel) { if (!Part.empty() && Check()) return true; Part.clear(); }
            if (bLetter) Part += char(tolower(static_cast<unsigned char>(C)));
        }
        return false;
    }

    bool BlocksPlayers(UPrimitiveComponent* Comp)
    {
        const ECollisionEnabled E = Comp->GetCollisionEnabled();  // players are stopped by queries
        return (E == ECollisionEnabled::QueryOnly || E == ECollisionEnabled::QueryAndPhysics) &&
               Comp->GetCollisionResponseToChannel(ECollisionChannel::ECC_Pawn) == ECollisionResponse::ECR_Block;
    }

    std::string MeshAndOwnerName(UPrimitiveComponent* Comp)
    {
        std::string Name;
        if (Comp->IsA(UStaticMeshComponent::StaticClass()))
            if (UStaticMesh* Mesh = static_cast<UStaticMeshComponent*>(Comp)->StaticMesh) Name = Mesh->GetName();
        if (AActor* Owner = Comp->GetOwner()) Name += " " + Owner->GetName();
        return Name + " " + Comp->GetName();
    }

    bool HasSimpleShapes(UPrimitiveComponent* Comp)
    {
        if (!Comp->IsA(UStaticMeshComponent::StaticClass())) return false;
        UStaticMesh* Mesh = static_cast<UStaticMeshComponent*>(Comp)->StaticMesh;
        UBodySetup* Body = Mesh ? Mesh->BodySetup : nullptr;
        return Body && Body->AggGeom.BoxElems.Num() + Body->AggGeom.ConvexElems.Num() + Body->AggGeom.SphylElems.Num() > 0;
    }

    // Convex faces per collision element, computed once per mesh (element space).
    std::map<const void*, GrindGeom::Faces> FaceCache;
    // Finished edges per mesh instance (component, instance), with where it was when computed.
    struct Cached { V3 At; bool bDone = false; std::vector<GrindGeom::Edge> Edges; std::vector<float> Triangles, Ramp; };
    // Instance transforms of instanced meshes (fetched once; the world's instances don't move).
    std::unordered_map<const void*, std::vector<FTransform>> InstanceCache;
    std::unordered_map<uint64_t, Cached> EdgeCache;

    const GrindGeom::Rules& Rules()
    {
        static GrindGeom::Rules R = [] { GrindGeom::Rules r; r.MinTopUp = 0.7f; return r; }();
        return R;
    }

    void CapsuleTriangles(const FKSphylElem& C, const FTransform& World, std::vector<float>& Tris);

    // Gives the edges found since First the triangles of their collision element.
    void GiveSolid(std::vector<GrindGeom::Edge>& Out, size_t First, std::vector<float>&& Tris)
    {
        if (First >= Out.size() || Tris.empty()) return;
        auto Solid = std::make_shared<const std::vector<float>>(std::move(Tris));
        for (size_t e = First; e < Out.size(); e++) Out[e].Solid = Solid;
    }

    // All grindable edges of one body setup placed by World (component or instance transform).
    void EdgesOf(UBodySetup* Body, const FTransform& World, std::vector<GrindGeom::Edge>& Out, std::vector<float>* Tris,
                 ShapeRails::Stats& S)
    {
        const FKAggregateGeom& G = Body->AggGeom;
        std::vector<V3> Local, Placed;
        for (int i = 0; i < G.BoxElems.Num(); i++)
        {
            const FKBoxElem& B = G.BoxElems[i];
            const Quat Q = FromRotator(B.Rotation);
            const V3 Axis[3] = { Rotate(Q, B.X * 0.5, 0, 0), Rotate(Q, 0, B.Y * 0.5, 0), Rotate(Q, 0, 0, B.Z * 0.5) };
            GrindGeom::BoxCorners({ float(B.Center.X), float(B.Center.Y), float(B.Center.Z) }, Axis, Local);
            Placed.clear();
            for (const V3& P : Local) Placed.push_back(Apply(World, P.X, P.Y, P.Z));
            static const GrindGeom::Faces BoxFaces = [] {
                std::vector<V3> P;
                const V3 A[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
                GrindGeom::BoxCorners({ 0, 0, 0 }, A, P);
                GrindGeom::Faces F;
                GrindGeom::HullFaces(P, F);
                return F;
            }();
            const size_t First = Out.size();
            GrindGeom::Edges(Placed, BoxFaces, Rules(), Out);
            if (Tris) GrindGeom::Triangles(Placed, BoxFaces, *Tris);
            else if (Out.size() > First) { std::vector<float> T; GrindGeom::Triangles(Placed, BoxFaces, T); GiveSolid(Out, First, std::move(T)); }
            S.Shapes++;
        }
        for (int i = 0; i < G.ConvexElems.Num(); i++)
        {
            const FKConvexElem& C = G.ConvexElems[i];
            const int N = C.VertexData.Num();
            if (N < 4) continue;
            auto It = FaceCache.find(&C);
            if (It == FaceCache.end())
            {
                Local.clear();
                for (int v = 0; v < N; v++) Local.push_back({ float(C.VertexData[v].X), float(C.VertexData[v].Y), float(C.VertexData[v].Z) });
                GrindGeom::Faces F;
                if (!GrindGeom::HullFaces(Local, F)) S.TooComplex++;
                It = FaceCache.emplace(&C, std::move(F)).first;
            }
            if (It->second.empty()) continue;
            Placed.clear();
            for (int v = 0; v < N; v++)
            {
                const V3 E = Apply(C.Transform, C.VertexData[v].X, C.VertexData[v].Y, C.VertexData[v].Z);
                Placed.push_back(Apply(World, E.X, E.Y, E.Z));
            }
            const size_t First = Out.size();
            GrindGeom::Edges(Placed, It->second, Rules(), Out);
            if (Tris) GrindGeom::Triangles(Placed, It->second, *Tris);
            else if (Out.size() > First) { std::vector<float> T; GrindGeom::Triangles(Placed, It->second, T); GiveSolid(Out, First, std::move(T)); }
            S.Shapes++;
        }
        for (int i = 0; i < G.SphylElems.Num(); i++)
        {
            // A lying capsule (pipe, round rail): its top line, if level enough to ride.
            const FKSphylElem& C = G.SphylElems[i];
            const Quat Q = FromRotator(C.Rotation);
            const V3 Ax = Rotate(Q, 0, 0, C.Length * 0.5);
            const V3 A = Apply(World, C.Center.X - Ax.X, C.Center.Y - Ax.Y, C.Center.Z - Ax.Z);
            const V3 B = Apply(World, C.Center.X + Ax.X, C.Center.Y + Ax.Y, C.Center.Z + Ax.Z);
            const float Dx = B.X - A.X, Dy = B.Y - A.Y, Dz = B.Z - A.Z;
            const float Run = std::sqrt(Dx * Dx + Dy * Dy);
            const float Up = float(C.Radius * std::fabs(World.Scale3D.Z));
            if (Run < Rules().MinLength || std::fabs(Dz) > Rules().MaxSlope * Run || Up > 40.f) continue;
            const size_t First = Out.size();
            Out.push_back({ { A.X, A.Y, A.Z + Up }, { B.X, B.Y, B.Z + Up }, { 0, 0, 1 }, { 0, 0, 0 } });
            if (!Tris) { std::vector<float> T; CapsuleTriangles(C, World, T); GiveSolid(Out, First, std::move(T)); }
            S.Shapes++;
        }
    }

    // One smooth shell over a build stair's steps: the convex hull of all its collision points
    // (computed once per mesh in its own space, then placed). Empty if it has too many points.
    struct ShellCache { std::vector<V3> Local; GrindGeom::Faces Faces; };
    std::map<const void*, ShellCache> StairShells;
    void StairShell(UBodySetup* Body, const FTransform& World, std::vector<float>& Tris)
    {
        auto It = StairShells.find(Body);
        if (It == StairShells.end())
        {
            ShellCache Sc;
            const FKAggregateGeom& G = Body->AggGeom;
            std::vector<V3> Pts;
            for (int i = 0; i < G.BoxElems.Num(); i++)
            {
                const FKBoxElem& B = G.BoxElems[i];
                const Quat Q = FromRotator(B.Rotation);
                const V3 Axis[3] = { Rotate(Q, B.X * 0.5, 0, 0), Rotate(Q, 0, B.Y * 0.5, 0), Rotate(Q, 0, 0, B.Z * 0.5) };
                std::vector<V3> Corners;
                GrindGeom::BoxCorners({ float(B.Center.X), float(B.Center.Y), float(B.Center.Z) }, Axis, Corners);
                Pts.insert(Pts.end(), Corners.begin(), Corners.end());
            }
            for (int i = 0; i < G.ConvexElems.Num(); i++)
            {
                const FKConvexElem& C = G.ConvexElems[i];
                for (int v = 0; v < C.VertexData.Num(); v++)
                    Pts.push_back(Apply(C.Transform, C.VertexData[v].X, C.VertexData[v].Y, C.VertexData[v].Z));
            }
            // Duplicates (shared step corners) to within 1 cm.
            for (const V3& P : Pts)
            {
                bool bDup = false;
                for (const V3& Q : Sc.Local)
                    if (std::fabs(P.X - Q.X) < 1.f && std::fabs(P.Y - Q.Y) < 1.f && std::fabs(P.Z - Q.Z) < 1.f) { bDup = true; break; }
                if (!bDup) Sc.Local.push_back(P);
            }
            // A build stair is one straight ramp across its grid tile, rising toward the side where
            // its highest points are. Rebuilt as one clean solid whose top is the piece's own top
            // surface, neighbouring pieces (snapped to the build grid) continue one straight slope,
            // where their collision shapes left lips the skater hopped over going down.
            float Lo[3] = { 1e30f, 1e30f, 1e30f }, Hi[3] = { -1e30f, -1e30f, -1e30f };
            for (const V3& P : Sc.Local)
            {
                const float C[3] = { P.X, P.Y, P.Z };
                for (int k = 0; k < 3; k++) { Lo[k] = std::fmin(Lo[k], C[k]); Hi[k] = std::fmax(Hi[k], C[k]); }
            }
            float Tx = 0, Ty = 0; int Top = 0;
            for (const V3& P : Sc.Local)
                if (P.Z >= Hi[2] - 2.f) { Tx += P.X; Ty += P.Y; Top++; }
            const float Cx = (Lo[0] + Hi[0]) * 0.5f, Cy = (Lo[1] + Hi[1]) * 0.5f;
            const float Dx = Top ? Tx / Top - Cx : 0.f, Dy = Top ? Ty / Top - Cy : 0.f;
            const bool AlongX = std::fabs(Dx) >= std::fabs(Dy);
            const float Off = AlongX ? Dx : Dy, Half = AlongX ? (Hi[0] - Lo[0]) * 0.5f : (Hi[1] - Lo[1]) * 0.5f;
            if (Sc.Local.size() >= 6 && Hi[2] - Lo[2] > 20.f && Half > 20.f && std::fabs(Off) > Half * 0.25f)
            {
                // a = rising axis (high end where the top points are), b = across.
                const float A0 = Off > 0 ? (AlongX ? Lo[0] : Lo[1]) : (AlongX ? Hi[0] : Hi[1]);
                const float A1 = Off > 0 ? (AlongX ? Hi[0] : Hi[1]) : (AlongX ? Lo[0] : Lo[1]);
                const float B0 = AlongX ? Lo[1] : Lo[0], B1 = AlongX ? Hi[1] : Hi[0];
                auto Pt = [&](float A, float B, float Z) { return AlongX ? V3{ A, B, Z } : V3{ B, A, Z }; };
                // The ramp's top follows the piece's real top surface (its collision is a thick
                // slanted slab, so the bounds' diagonal ran 60-80 cm inside it): the longest
                // rising edge of the upper outline seen from the side (u 0 = low end, 1 = high end).
                std::vector<std::pair<float, float>> Uz;
                for (const V3& P : Sc.Local) Uz.push_back({ ((AlongX ? P.X : P.Y) - A0) / (A1 - A0), P.Z });
                std::sort(Uz.begin(), Uz.end());
                std::vector<std::pair<float, float>> Up;  // upper hull, left to right
                for (const auto& Q : Uz)
                {
                    while (Up.size() >= 2)
                    {
                        const auto& O = Up[Up.size() - 2]; const auto& Am = Up.back();
                        if ((Am.first - O.first) * (Q.second - O.second) - (Am.second - O.second) * (Q.first - O.first) >= 0) Up.pop_back();
                        else break;
                    }
                    Up.push_back(Q);
                }
                // Profile (u, z) of the ramp: the top edge's line from the low end to where the
                // edge ends, flat from there to the high end, floor below. Convex, so its hull
                // (extruded across the piece) is the solid.
                float Z0 = Lo[2], Ue = 1.f, Ze = Hi[2], Best = 0;
                for (size_t k = 1; k < Up.size(); k++)
                {
                    const float Du = Up[k].first - Up[k - 1].first, Dz = Up[k].second - Up[k - 1].second;
                    if (Du > Best && Dz > 0)
                    {
                        Best = Du;
                        Z0 = Up[k - 1].second - Up[k - 1].first * (Dz / Du);
                        Ue = Up[k].first;
                        Ze = Up[k].second;
                    }
                }
                Z0 = std::fmax(Lo[2], Z0);
                // One straight slope from the piece's bottom height to its top height across the
                // whole tile: Fortnite's short flat landing at each piece's top (Ue..1) made a
                // chain of stairs slope-flat-slope-flat, and the board snagged at every joint
                // (2026-10-08 19:51 trace: speed 8.5 -> 5.9 m/s exactly at a joint). Consecutive
                // pieces now continue one slope; the ends meet floors at the same heights.
                (void)Ue;
                // Only the riding surface and the two sides are kept: the closed block's end walls
                // (a 16 cm wall under the low end, the full-height wall under the high end) stood
                // right at every joint, and the wheels caught their top edges (2026-10-08 20:01 and
                // 20:19 traces: stops going up, launches going down, the lip from a floor onto the
                // stairs). The bottom goes too. No closed blocks overlapping each other or floors: a
                // board inside an overlapping block fell straight through it (20:20, top of the stairs).
                // Without the low end's wall the riding surface carries on down its own slope to the
                // piece's bottom (a ~40 cm lead-in from the ground; at a joint it lies in the same
                // plane as the piece below, so the surface stays one straight slope).
                const float ULow = Ze > Z0 + 1.f ? -(Z0 - Lo[2]) / (Ze - Z0) : 0.f;
                std::vector<std::pair<float, float>> Profile = { { ULow, Lo[2] }, { 1.f, Lo[2] }, { 1.f, Ze } };
                Sc.Local.clear();
                for (const auto& [U, Z] : Profile)
                {
                    const float A = A0 + U * (A1 - A0);
                    Sc.Local.push_back(Pt(A, B0, Z));
                    Sc.Local.push_back(Pt(A, B1, Z));
                }
                if (!GrindGeom::HullFaces(Sc.Local, Sc.Faces, 160)) Sc.Faces.clear();
                auto AOf = [&](int i) { return AlongX ? Sc.Local[i].X : Sc.Local[i].Y; };
                Sc.Faces.erase(std::remove_if(Sc.Faces.begin(), Sc.Faces.end(), [&](const std::vector<int>& F)
                {
                    bool bSameA = true, bBottom = true;
                    for (int i : F)
                    {
                        bSameA = bSameA && std::fabs(AOf(i) - AOf(F[0])) < 0.5f;
                        bBottom = bBottom && std::fabs(Sc.Local[i].Z - Lo[2]) < 0.5f;
                    }
                    return bSameA || bBottom;
                }), Sc.Faces.end());
                const float Z1 = Ze;
                static int Logged = 0;
                if (Logged++ < 6)
                    Log("[stairs] open ramp shape (no end walls): %zu collision points, rise %.0f cm over %.0f cm, top %.0f..%.0f cm (bounds %.0f..%.0f)",
                        Uz.size(), Z1 - Z0, std::fabs(A1 - A0), Z0, Z1, Lo[2], Hi[2]);
            }
            else if (!GrindGeom::HullFaces(Sc.Local, Sc.Faces, 160)) Sc.Faces.clear();
            It = StairShells.emplace(Body, std::move(Sc)).first;
        }
        if (It->second.Faces.empty()) return;
        std::vector<V3> Placed;
        for (const V3& P : It->second.Local) Placed.push_back(Apply(World, P.X, P.Y, P.Z));
        GrindGeom::Triangles(Placed, It->second.Faces, Tris);
    }

    // A capsule as a convex 12-sided prism running cap tip to cap tip (tree trunks).
    std::map<const void*, GrindGeom::Faces> CapsuleFaceCache;
    void CapsuleTriangles(const FKSphylElem& C, const FTransform& World, std::vector<float>& Tris)
    {
        const Quat Q = FromRotator(C.Rotation);
        const double Half = C.Length * 0.5 + C.Radius;
        std::vector<V3> Local, Placed;
        for (int End = 0; End < 2; End++)
            for (int k = 0; k < 12; k++)
            {
                const double A = k * 3.14159265358979 / 6.0;
                const V3 P = Rotate(Q, C.Radius * std::cos(A), C.Radius * std::sin(A), End ? Half : -Half);
                Local.push_back({ float(C.Center.X + P.X), float(C.Center.Y + P.Y), float(C.Center.Z + P.Z) });
            }
        auto It = CapsuleFaceCache.find(&C);
        if (It == CapsuleFaceCache.end())
        {
            GrindGeom::Faces F;
            GrindGeom::HullFaces(Local, F);
            It = CapsuleFaceCache.emplace(&C, std::move(F)).first;
        }
        if (It->second.empty()) return;
        for (const V3& P : Local) Placed.push_back(Apply(World, P.X, P.Y, P.Z));
        GrindGeom::Triangles(Placed, It->second, Tris);
    }

    // A tree's solid collision: boxes and hulls through EdgesOf (edges dropped), capsules here.
    void TreeTriangles(UBodySetup* Body, const FTransform& World, std::vector<float>& Tris, ShapeRails::Stats& S)
    {
        std::vector<GrindGeom::Edge> Unused;
        EdgesOf(Body, World, Unused, &Tris, S);
        for (int i = 0; i < Body->AggGeom.SphylElems.Num(); i++)
            CapsuleTriangles(Body->AggGeom.SphylElems[i], World, Tris);
    }

    uint64_t Key(const void* Comp, int Instance) { return (uint64_t(uintptr_t(Comp)) << 20) ^ uint64_t(uint32_t(Instance + 1)); }
}

std::string ShapeRails::Stats::Summary() const
{
    char B[320];
    snprintf(B, sizeof(B), "%d meshes (%d instances), %d collision shapes, %d edges; %d without simple collision, %d hulls too detailed; "
             "%d stairs (%d triangles); %d trees (%d triangles)", Components, Instances, Shapes, Edges, NoSimpleCollision, TooComplex,
             Stairs, StairTriangles, Trees, TreeTriangles);
    return B;
}

bool ShapeRails::IsPassThroughTree(UPrimitiveComponent* Comp)
{
    if (!Comp || !IsTreeName(MeshAndOwnerName(Comp))) return false;
    return !BlocksPlayers(Comp) || HasSimpleShapes(Comp);
}

void ShapeRails::Gather(UObject* WorldContext, const FVector& Centre, float Radius, AActor* Ignore,
                        std::vector<GrindGeom::Edge>& Out, std::vector<float>& StairTriangles, Stats& S,
                        std::vector<float>* BuildStairSteps, std::vector<float>* BuildStairRamps)
{
    // Every object channel except Pawn (3): world static / dynamic, vehicles, destructibles and
    // Fortnite's own channels (player builds).
    EObjectTypeQuery T[] = { EObjectTypeQuery::ObjectTypeQuery1, EObjectTypeQuery::ObjectTypeQuery2, EObjectTypeQuery::ObjectTypeQuery4,
                             EObjectTypeQuery::ObjectTypeQuery5, EObjectTypeQuery::ObjectTypeQuery6, EObjectTypeQuery::ObjectTypeQuery7,
                             EObjectTypeQuery::ObjectTypeQuery8, EObjectTypeQuery::ObjectTypeQuery9, EObjectTypeQuery::ObjectTypeQuery10 };
    const int NT = int(sizeof(T) / sizeof(T[0]));
    TArray<EObjectTypeQuery> Types(T, NT, NT);
    TArray<AActor*> Ignored;
    AActor* IgnoreList[1] = { Ignore };
    if (Ignore) Ignored = TArray<AActor*>(IgnoreList, 1, 1);
    // Reused so the engine-owned buffer is not leaked on every patch.
    static TArray<UPrimitiveComponent*> Found;
    UKismetSystemLibrary::SphereOverlapComponents(WorldContext, Centre, Radius, Types, UStaticMeshComponent::StaticClass(), Ignored, &Found);
    for (int c = 0; c < Found.Num(); c++)
    {
        UPrimitiveComponent* Comp = Found[c];
        if (!Comp || !Comp->IsA(UStaticMeshComponent::StaticClass())) continue;
        UStaticMesh* Mesh = static_cast<UStaticMeshComponent*>(Comp)->StaticMesh;
        if (!Mesh) continue;
        std::string Name = Mesh->GetName();
        if (AActor* Owner = Comp->GetOwner()) Name += " " + Owner->GetName();
        // Trees: only their solid collision (what blocks a Fortnite player), never grind spots.
        const bool bTree = IsTreeName(Name) && BlocksPlayers(Comp) && HasSimpleShapes(Comp);
        if (!bTree && IsFoliageName(Name)) continue;
        if (bTree)
        {
            S.Trees++;
            static std::unordered_map<std::string, bool> TreeLogged;
            const std::string TreeKey = Mesh->GetName();
            if (TreeLogged.size() < 100 && !TreeLogged[TreeKey]) { TreeLogged[TreeKey] = true; Log("[trees] solid collision from: %s", Name.c_str()); }
        }
        S.Components++;
        std::string Lower = Name;
        for (char& Ch : Lower) Ch = char(tolower(static_cast<unsigned char>(Ch)));
        const bool bStairs = !bTree && Lower.find("stair") != std::string::npos;
        // Player-built stair pieces (PBW_W1_StairW, PBWA_..._Stair...): one straight ramp each.
        const bool bBuildStair = bStairs && Lower.find("pbw") != std::string::npos && BuildStairSteps && BuildStairRamps;
        if (bStairs)
        {
            S.Stairs++;
            static std::unordered_map<std::string, bool> Logged;
            const std::string Key = Mesh->GetName();
            if (Logged.size() < 100 && !Logged[Key]) { Logged[Key] = true; Log("[stairs] solid collision from: %s", Name.c_str()); }
        }
        UBodySetup* Body = Mesh->BodySetup;
        if (!Body || (Body->AggGeom.BoxElems.Num() + Body->AggGeom.ConvexElems.Num() + Body->AggGeom.SphylElems.Num()) == 0)
        {
            S.NoSimpleCollision++;
            continue;
        }
        auto Place = [&](int Instance, const FTransform& W)
        {
            const V3 At{ float(W.Translation.X), float(W.Translation.Y), float(W.Translation.Z) };
            Cached& C = EdgeCache[Key(Comp, Instance)];
            if (!C.bDone || std::fabs(C.At.X - At.X) > 1 || std::fabs(C.At.Y - At.Y) > 1 || std::fabs(C.At.Z - At.Z) > 1)
            {
                C.At = At;
                C.bDone = true;
                C.Edges.clear();
                C.Triangles.clear();
                C.Ramp.clear();
                if (bTree) TreeTriangles(Body, W, C.Triangles, S);
                else EdgesOf(Body, W, C.Edges, bStairs ? &C.Triangles : nullptr, S);
                if (bBuildStair) StairShell(Body, W, C.Ramp);
            }
            if (bTree)
            {
                StairTriangles.insert(StairTriangles.end(), C.Triangles.begin(), C.Triangles.end());
                S.TreeTriangles += int(C.Triangles.size() / 9);
                return;
            }
            S.Edges += int(C.Edges.size());
            Out.insert(Out.end(), C.Edges.begin(), C.Edges.end());
            if (bBuildStair)
            {
                BuildStairSteps->insert(BuildStairSteps->end(), C.Triangles.begin(), C.Triangles.end());
                // No shell (too many points): the steps stand in for it.
                const std::vector<float>& Smooth = C.Ramp.empty() ? C.Triangles : C.Ramp;
                BuildStairRamps->insert(BuildStairRamps->end(), Smooth.begin(), Smooth.end());
                S.StairTriangles += int(C.Triangles.size() / 9);
            }
            else if (bStairs)
            {
                StairTriangles.insert(StairTriangles.end(), C.Triangles.begin(), C.Triangles.end());
                S.StairTriangles += int(C.Triangles.size() / 9);
            }
        };
        if (Comp->IsA(UInstancedStaticMeshComponent::StaticClass()))
        {
            auto* Ism = static_cast<UInstancedStaticMeshComponent*>(Comp);
            auto It = InstanceCache.find(Comp);
            if (It == InstanceCache.end())
            {
                std::vector<FTransform> All;
                const int Count = Ism->GetInstanceCount();
                if (Count <= 20000)
                    for (int i = 0; i < Count; i++)
                    {
                        FTransform W{};
                        if (Ism->GetInstanceTransform(i, &W, true)) All.push_back(W);
                    }
                It = InstanceCache.emplace(Comp, std::move(All)).first;
            }
            const double R2 = double(Radius) * Radius;
            for (size_t i = 0; i < It->second.size(); i++)
            {
                const FTransform& W = It->second[i];
                const double Dx = W.Translation.X - Centre.X, Dy = W.Translation.Y - Centre.Y, Dz = W.Translation.Z - Centre.Z;
                if (Dx * Dx + Dy * Dy + Dz * Dz > R2) continue;
                S.Instances++;
                Place(int(i), W);
            }
        }
        else
            Place(-1, Comp->K2_GetComponentToWorld());
    }
    if (EdgeCache.size() > 20000) EdgeCache.clear();  // long sessions: forget far-away meshes
    if (InstanceCache.size() > 2000) InstanceCache.clear();
}
