#include "DropperSnap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

using namespace SDK;

namespace DropperSnap
{
    namespace
    {
        // All solving is in Skate 3's space (metres, y up, right-handed), as in the game.
        struct V3
        {
            double x = 0, y = 0, z = 0;
            V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
            V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
            V3 operator*(double s) const { return { x * s, y * s, z * s }; }
            V3 operator-() const { return { -x, -y, -z }; }
        };
        double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
        double Len(const V3& a) { return std::sqrt(Dot(a, a)); }
        V3 Norm(const V3& a) { const double l = Len(a); return l > 1e-12 ? a * (1.0 / l) : V3{}; }

        // A frame as the game stores it: rows x, y, z (z = the outward normal) and the position.
        struct Frame { V3 X{ 1, 0, 0 }, Y{ 0, 1, 0 }, Z{ 0, 0, 1 }, P{}; };
        V3 ToWorld(const Frame& F, const V3& L) { return F.P + F.X * L.x + F.Y * L.y + F.Z * L.z; }
        V3 ToLocal(const Frame& F, const V3& W) { const V3 D = W - F.P; return { Dot(D, F.X), Dot(D, F.Y), Dot(D, F.Z) }; }
        // A * B (B in A's frame)
        Frame Compose(const Frame& A, const Frame& B)
        {
            Frame R;
            R.X = A.X * B.X.x + A.Y * B.X.y + A.Z * B.X.z;
            R.Y = A.X * B.Y.x + A.Y * B.Y.y + A.Z * B.Y.z;
            R.Z = A.X * B.Z.x + A.Y * B.Z.y + A.Z * B.Z.z;
            R.P = ToWorld(A, B.P);
            return R;
        }
        Frame Inverse(const Frame& F)
        {
            Frame R;
            R.X = { F.X.x, F.Y.x, F.Z.x };
            R.Y = { F.X.y, F.Y.y, F.Z.y };
            R.Z = { F.X.z, F.Y.z, F.Z.z };
            R.P = -(R.X * F.P.x + R.Y * F.P.y + R.Z * F.P.z);
            return R;
        }

        // One snap locator of a piece in its model space.
        struct Locator { int Group = 0; Frame F; };
        std::unordered_map<std::string, std::vector<Locator>> Pieces;
        bool bLoaded = false;

        struct Point { Frame F; bool bPiece = true; };   // +0x40: piece locator (procedural points: false)
        struct Face
        {
            Frame F;                 // the first corner's frame
            V3 Min2{ 1e30, 1e30, 0 }, Max2{ -1e30, -1e30, 0 };   // the corners in F (x, y)
            std::vector<V3> Corners; // in F
            V3 BMin{ 1e30, 1e30, 1e30 }, BMax{ -1e30, -1e30, -1e30 };   // world bounds
            bool bPiece = true;
        };
        struct Features { std::vector<Point> Points; std::vector<Face> Faces; };

        constexpr double MaxDistance = 1.5;                  // editor_cursor B0C05776
        const double MaxAngle = 45.0 * 3.14159265358979 / 180.0;   // editor_cursor 31255295, degrees
        constexpr double SameKindWeight = 0.025;             // DAT_8208e9a8: two pieces' locators
        constexpr double Grid = 0.75;                        // DAT_8217f5a0: faces snap on this grid
        constexpr double RayLength = 0.75, RayLift = 0.3;    // 8284b948 (+0x4c / +0x50)

        // Unreal pose (cm, z up) <-> Skate 3 frame (m, y up): y and z swap.
        V3 FromUe(const FVector& V) { return { V.X / 100.0, V.Z / 100.0, V.Y / 100.0 }; }
        FVector ToUe(const V3& V) { return FVector{ V.x * 100.0, V.z * 100.0, V.y * 100.0 }; }
        V3 DirFromUe(const FVector& V) { return { V.X, V.Z, V.Y }; }
        FVector DirToUe(const V3& V) { return FVector{ V.x, V.z, V.y }; }

        Frame PoseFrame(const FVector& Pos, const DropperWorld::Quat& Q)
        {
            // the rotation's Skate columns are its Unreal columns with y / z swapped
            const V3 Cx = DirFromUe(DropperWorld::Rotate(Q, FVector{ 1, 0, 0 }));   // Skate x
            const V3 Cy = DirFromUe(DropperWorld::Rotate(Q, FVector{ 0, 0, 1 }));   // Skate y = Unreal z
            const V3 Cz = DirFromUe(DropperWorld::Rotate(Q, FVector{ 0, 1, 0 }));   // Skate z = Unreal y
            Frame F;
            // rows of the matrix whose columns are Cx, Cy, Cz: the frame maps local -> world as
            // P + X*lx + Y*ly + Z*lz, so X/Y/Z here are the columns (the images of the axes)
            F.X = Cx; F.Y = Cy; F.Z = Cz;
            F.P = FromUe(Pos);
            return F;
        }
        DropperWorld::Quat QuatOfFrame(const Frame& F)
        {
            // Unreal rotation matrix columns: images of Unreal x, y, z
            const FVector Ux = DirToUe(F.X), Uy = DirToUe(F.Z), Uz = DirToUe(F.Y);
            const double m00 = Ux.X, m10 = Ux.Y, m20 = Ux.Z, m01 = Uy.X, m11 = Uy.Y, m21 = Uy.Z, m02 = Uz.X, m12 = Uz.Y, m22 = Uz.Z;
            DropperWorld::Quat Q;
            const double T = m00 + m11 + m22;
            if (T > 0)
            {
                const double S = std::sqrt(T + 1.0) * 2;
                Q.W = 0.25 * S; Q.X = (m21 - m12) / S; Q.Y = (m02 - m20) / S; Q.Z = (m10 - m01) / S;
            }
            else if (m00 > m11 && m00 > m22)
            {
                const double S = std::sqrt(1.0 + m00 - m11 - m22) * 2;
                Q.W = (m21 - m12) / S; Q.X = 0.25 * S; Q.Y = (m01 + m10) / S; Q.Z = (m02 + m20) / S;
            }
            else if (m11 > m22)
            {
                const double S = std::sqrt(1.0 + m11 - m00 - m22) * 2;
                Q.W = (m02 - m20) / S; Q.X = (m01 + m10) / S; Q.Y = 0.25 * S; Q.Z = (m12 + m21) / S;
            }
            else
            {
                const double S = std::sqrt(1.0 + m22 - m00 - m11) * 2;
                Q.W = (m10 - m01) / S; Q.X = (m02 + m20) / S; Q.Y = (m12 + m21) / S; Q.Z = 0.25 * S;
            }
            const double L = std::sqrt(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W);
            if (L > 1e-12) { Q.X /= L; Q.Y /= L; Q.Z /= L; Q.W /= L; }
            return Q;
        }

        // 828402a8 / 8284c290: a piece's points and faces in the world.
        void Gather(const std::string& Id, const Frame& Obj, Features& Out)
        {
            auto It = Pieces.find(Id);
            if (It == Pieces.end()) return;
            std::unordered_map<int, size_t> FaceOf;
            for (const Locator& L : It->second)
            {
                // the locator frame in the model, placed with the object
                Frame W;
                W.X = Obj.X * L.F.X.x + Obj.Y * L.F.X.y + Obj.Z * L.F.X.z;
                W.Y = Obj.X * L.F.Y.x + Obj.Y * L.F.Y.y + Obj.Z * L.F.Y.z;
                W.Z = Obj.X * L.F.Z.x + Obj.Y * L.F.Z.y + Obj.Z * L.F.Z.z;
                W.P = ToWorld(Obj, L.F.P);
                if (L.Group == 0) { Out.Points.push_back({ W, true }); continue; }
                auto F = FaceOf.find(L.Group);
                if (F == FaceOf.end())
                {
                    F = FaceOf.emplace(L.Group, Out.Faces.size()).first;
                    Face Nf;
                    Nf.F = W;
                    Out.Faces.push_back(Nf);
                }
                Face& Fc = Out.Faces[F->second];
                const V3 C = ToLocal(Fc.F, W.P);
                Fc.Corners.push_back(C);
                Fc.Min2 = { std::min(Fc.Min2.x, C.x), std::min(Fc.Min2.y, C.y), 0 };
                Fc.Max2 = { std::max(Fc.Max2.x, C.x), std::max(Fc.Max2.y, C.y), 0 };
                Fc.BMin = { std::min(Fc.BMin.x, W.P.x), std::min(Fc.BMin.y, W.P.y), std::min(Fc.BMin.z, W.P.z) };
                Fc.BMax = { std::max(Fc.BMax.x, W.P.x), std::max(Fc.BMax.y, W.P.y), std::max(Fc.BMax.z, W.P.z) };
            }
        }

        // 8284a1b8: the two normals face each other within the angle.
        bool Facing(const V3& A, const V3& B)
        {
            const double C = std::clamp(Dot(Norm(A), -Norm(B)), -1.0, 1.0);
            return std::acos(C) < MaxAngle;
        }

        // The best pair so far (solver result, 0xd0 bytes): the target's frame and the carried
        // piece's frame that goes onto it (positions = the points that meet).
        struct Best { bool bFound = false; double Score = 1e30; Frame Env, Mov; };

        // 828497e0: two points.
        void PointPoint(const Point& Env, const Point& Mov, Best& B)
        {
            const double D2 = Dot(Env.F.P - Mov.F.P, Env.F.P - Mov.F.P);
            const double W = Env.bPiece == Mov.bPiece ? SameKindWeight : 1.0;
            const double S = W * D2;
            if (S >= B.Score || D2 >= MaxDistance * MaxDistance) return;
            B = { true, S, Env.F, Mov.F };
        }

        // 828499a8: a point and a face: the point's distance to the face plane, then to the nearest
        // grid point inside the face. PointOnEnv: the face is the target (else the carried piece's).
        void PointFace(const Point& Pt, const Face& Fc, bool bPointIsEnv, Best& B)
        {
            const V3 L = ToLocal(Fc.F, Pt.F.P);
            const double W = Pt.bPiece == Fc.bPiece ? SameKindWeight : 1.0;
            if (W * L.z * L.z >= B.Score || std::fabs(L.z) >= MaxDistance) return;
            const double Gx = std::floor(L.x / Grid + 0.5) * Grid, Gy = std::floor(L.y / Grid + 0.5) * Grid;
            if (!(Gx > Fc.Min2.x - 1e-6 && Gx < Fc.Max2.x + 1e-6 && Gy > Fc.Min2.y - 1e-6 && Gy < Fc.Max2.y + 1e-6)) return;   // 82849920
            const V3 T = ToWorld(Fc.F, V3{ Gx, Gy, 0 });
            const double D2 = Dot(Pt.F.P - T, Pt.F.P - T);
            const double S = W * D2;
            if (S >= B.Score || D2 >= MaxDistance * MaxDistance) return;
            Frame OnFace = Fc.F;
            OnFace.P = T;
            if (bPointIsEnv) B = { true, S, Pt.F, OnFace };
            else B = { true, S, OnFace, Pt.F };
        }

        // 82849f78: two faces: the target face's grid samples (inside its bounds) against the
        // carried face.
        void FaceFace(const Face& Env, const Face& Mov, Best& B)
        {
            const int Nx = int(std::floor((Env.Max2.x - Env.Min2.x) / Grid + 1e-6)), Ny = int(std::floor((Env.Max2.y - Env.Min2.y) / Grid + 1e-6));
            // only the samples that can be within reach of the carried face (its world bounds,
            // grown by the snap distance, seen in the target face's frame)
            double Lx = 1e30, Hx = -1e30, Ly = 1e30, Hy = -1e30;
            for (int c = 0; c < 8; c++)
            {
                const V3 W{ (c & 1) ? Mov.BMax.x + MaxDistance : Mov.BMin.x - MaxDistance, (c & 2) ? Mov.BMax.y + MaxDistance : Mov.BMin.y - MaxDistance,
                            (c & 4) ? Mov.BMax.z + MaxDistance : Mov.BMin.z - MaxDistance };
                const V3 L = ToLocal(Env.F, W);
                Lx = std::min(Lx, L.x); Hx = std::max(Hx, L.x); Ly = std::min(Ly, L.y); Hy = std::max(Hy, L.y);
            }
            const int I0 = std::max(0, int(std::ceil((Lx - Env.Min2.x) / Grid - 1e-6))), I1 = std::min(Nx, int(std::floor((Hx - Env.Min2.x) / Grid + 1e-6)));
            const int J0 = std::max(0, int(std::ceil((Ly - Env.Min2.y) / Grid - 1e-6))), J1 = std::min(Ny, int(std::floor((Hy - Env.Min2.y) / Grid + 1e-6)));
            for (int i = I0; i <= I1; i++)
                for (int j = J0; j <= J1; j++)
                {
                    Point S;
                    S.F = Env.F;
                    S.F.P = ToWorld(Env.F, V3{ Env.Min2.x + i * Grid, Env.Min2.y + j * Grid, 0 });
                    S.bPiece = Env.bPiece;
                    PointFace(S, Mov, true, B);
                }
        }

        bool Near(const V3& P, const Face& F)
        {
            return P.x > F.BMin.x - MaxDistance && P.x < F.BMax.x + MaxDistance && P.y > F.BMin.y - MaxDistance && P.y < F.BMax.y + MaxDistance &&
                   P.z > F.BMin.z - MaxDistance && P.z < F.BMax.z + MaxDistance;
        }
        bool Near(const Face& A, const Face& F)
        {
            return A.BMax.x > F.BMin.x - MaxDistance && A.BMin.x < F.BMax.x + MaxDistance && A.BMax.y > F.BMin.y - MaxDistance &&
                   A.BMin.y < F.BMax.y + MaxDistance && A.BMax.z > F.BMin.z - MaxDistance && A.BMin.z < F.BMax.z + MaxDistance;
        }

        // 8284b948: rays from the carried piece (sideways and up / down) make "Procedural Snap To
        // Point" targets where they hit the world (the hit point, its normal as z).
        void RayPoints(UWorld* World, const Frame& Obj, const std::vector<AActor*>& Ignore, Features& Env)
        {
#ifdef DROPPERSNAP_NO_TRACE   // offline test: no world
            (void)World; (void)Obj; (void)Ignore; (void)Env;
        }
#else
            if (!World) return;
            const V3 Up{ 0, 1, 0 };
            V3 Side = std::fabs(Obj.Z.y) < 0.99 ? Norm(Cross(Up, Obj.Z)) : Norm(Obj.X);
            const V3 From = Obj.P + Up * RayLift;
            const V3 Dirs[4] = { Side, -Side, Up, -Up };
            const FLinearColor Color{};
            for (const V3& D : Dirs)
            {
                TArray<AActor*> Ig(const_cast<AActor**>(Ignore.data()), int32(Ignore.size()), int32(Ignore.size()));
                FHitResult Hit{};
                const bool bHit = UKismetSystemLibrary::LineTraceSingle(World, ToUe(From), ToUe(From + D * RayLength),
                    ETraceTypeQuery::TraceTypeQuery1 /* Visibility */, false, Ig, EDrawDebugTrace::None, &Hit, true, Color, Color, 0.f);
                if (!bHit || !Hit.bBlockingHit || Hit.bStartPenetrating || Hit.Time <= 0.f) continue;
                Point P;
                P.bPiece = false;
                P.F.Z = Norm(DirFromUe(FVector{ Hit.ImpactNormal.X, Hit.ImpactNormal.Y, Hit.ImpactNormal.Z }));
                P.F.Y = Obj.Z;
                P.F.X = Norm(Cross(P.F.Y, P.F.Z));
                if (Len(P.F.X) < 1e-6) { P.F.X = Norm(Cross(Obj.X, P.F.Z)); }
                P.F.Y = Norm(Cross(P.F.Z, P.F.X));
                P.F.P = FromUe(FVector{ Hit.ImpactPoint.X, Hit.ImpactPoint.Y, Hit.ImpactPoint.Z });
                Env.Points.push_back(P);
            }
        }
#endif

        // 8284b168: the carried frame turned to face the target: z flipped, y = whichever of the
        // frame's y / x is closer to the target's y, signed so they agree.
        Frame Facing(const Frame& Mov, const Frame& Env)
        {
            Frame R;
            R.Z = -Mov.Z;
            const double Dy = Dot(Env.Y, Mov.Y), Dx = Dot(Env.Y, Mov.X);
            R.Y = std::fabs(Dy) > std::fabs(Dx) ? Mov.Y * (Dy >= 0 ? 1.0 : -1.0) : Mov.X * (Dx >= 0 ? 1.0 : -1.0);
            R.X = Cross(R.Y, R.Z);
            R.P = Mov.P;
            return R;
        }
    }

    bool Load(const std::string& Path, std::string& Error)
    {
        if (bLoaded) return true;
        std::ifstream In(Path);
        if (!In) { Error = "cannot read " + Path; return false; }
        std::string Line;
        while (std::getline(In, Line))
        {
            if (Line.empty() || Line[0] == '#') continue;
            std::istringstream S(Line);
            std::string Id;
            Locator L;
            double v[12];
            S >> Id >> L.Group;
            for (double& x : v) S >> x;
            if (!S) continue;
            L.F.X = { v[0], v[1], v[2] };
            L.F.Y = { v[3], v[4], v[5] };
            L.F.Z = { v[6], v[7], v[8] };
            L.F.P = { v[9], v[10], v[11] };
            Pieces[Id].push_back(L);
        }
        bLoaded = true;
        return true;
    }

    bool HasLocators(const std::string& Id) { return Pieces.count(Id) != 0; }

    bool Solve(UWorld* World, int Held, const std::vector<int>& Others, FVector& Pos, DropperWorld::Quat& Rot)
    {
        const DropperWorld::Placed* H = DropperWorld::Get(Held);
        if (!H || !HasLocators(H->Id)) return false;
        const Frame Obj = PoseFrame(Pos, Rot);
        Features Mov;
        Gather(H->Id, Obj, Mov);
        if (Mov.Points.empty() && Mov.Faces.empty()) return false;
        V3 MMin{ 1e30, 1e30, 1e30 }, MMax{ -1e30, -1e30, -1e30 };
        auto Grow = [&](const V3& P) { MMin = { std::min(MMin.x, P.x), std::min(MMin.y, P.y), std::min(MMin.z, P.z) }; MMax = { std::max(MMax.x, P.x), std::max(MMax.y, P.y), std::max(MMax.z, P.z) }; };
        for (auto& P : Mov.Points) Grow(P.F.P);
        for (auto& F : Mov.Faces) { Grow(F.BMin); Grow(F.BMax); }
        Grow(Obj.P);

        // the placed terrain pieces near it (82857210) and the procedural ray points
        Features Env;
        for (const DropperWorld::Placed& P : DropperWorld::All())
        {
            if (P.Handle == Held || P.bGhost || std::find(Others.begin(), Others.end(), P.Handle) != Others.end()) continue;
            if (!HasLocators(P.Id)) continue;
            FVector Mn, Mx;
            DropperWorld::Bounds(P.Handle, Mn, Mx);
            const V3 A = FromUe(Mn), Bv = FromUe(Mx);
            const V3 Lo{ std::min(A.x, Bv.x), std::min(A.y, Bv.y), std::min(A.z, Bv.z) }, Hi{ std::max(A.x, Bv.x), std::max(A.y, Bv.y), std::max(A.z, Bv.z) };
            if (Hi.x < MMin.x - MaxDistance || Lo.x > MMax.x + MaxDistance || Hi.y < MMin.y - MaxDistance || Lo.y > MMax.y + MaxDistance ||
                Hi.z < MMin.z - MaxDistance || Lo.z > MMax.z + MaxDistance)
                continue;
            Gather(P.Id, PoseFrame(P.Position, P.Rotation), Env);
        }
        std::vector<AActor*> Ignore;
        RayPoints(World, Obj, Ignore, Env);
        if (Env.Points.empty() && Env.Faces.empty()) return false;

        // 8284a240
        Best B;
        for (const Point& E : Env.Points)
        {
            for (const Point& M : Mov.Points)
                if (Facing(E.F.Z, M.F.Z)) PointPoint(E, M, B);
            for (const Face& M : Mov.Faces)
                if (Near(E.F.P, M) && Facing(E.F.Z, M.F.Z)) PointFace(E, M, true, B);
        }
        for (const Face& E : Env.Faces)
        {
            for (const Point& M : Mov.Points)
                if (Near(M.F.P, E) && Facing(E.F.Z, M.F.Z)) PointFace(M, E, false, B);
            for (const Face& M : Mov.Faces)
                if (Near(M, E) && Facing(E.F.Z, M.F.Z)) FaceFace(E, M, B);
        }
        if (!B.bFound) return false;

        // 8284c018: the piece moves so its frame (turned to face the target) lies on the target
        const Frame Turned = Facing(B.Mov, B.Env);
        const Frame New = Compose(B.Env, Compose(Inverse(Turned), Obj));
        Pos = ToUe(New.P);
        Rot = QuatOfFrame(New);
        return true;
    }
}
