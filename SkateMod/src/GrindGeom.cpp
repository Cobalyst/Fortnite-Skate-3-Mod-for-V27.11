#include "GrindGeom.h"

#include <algorithm>
#include <cmath>

namespace GrindGeom
{
    namespace
    {
        V3 Sub(const V3& A, const V3& B) { return { A.X - B.X, A.Y - B.Y, A.Z - B.Z }; }
        V3 Cross(const V3& A, const V3& B) { return { A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X }; }
        float Dot(const V3& A, const V3& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
        float Len(const V3& A) { return std::sqrt(Dot(A, A)); }
        V3 Norm(const V3& A) { const float L = Len(A); return L > 0 ? V3{ A.X / L, A.Y / L, A.Z / L } : V3{ 0, 0, 0 }; }

        // Outward unit normal of a face from its points (the shape's centroid is inside).
        bool FaceNormal(const std::vector<V3>& P, const std::vector<int>& Face, const V3& Mid, V3& N)
        {
            const V3& A = P[size_t(Face[0])];
            float Best = 0;
            for (size_t j = 1; j < Face.size(); j++)
                for (size_t k = j + 1; k < Face.size(); k++)
                {
                    const V3 C = Cross(Sub(P[size_t(Face[j])], A), Sub(P[size_t(Face[k])], A));
                    const float L = Len(C);
                    if (L > Best) { Best = L; N = C; }
                }
            if (Best <= 1e-4f) return false;
            N = Norm(N);
            if (Dot(N, Sub(A, Mid)) < 0) N = { -N.X, -N.Y, -N.Z };
            return true;
        }
    }

    bool HullFaces(const std::vector<V3>& P, Faces& Out, size_t MaxPoints)
    {
        Out.clear();
        const size_t N = P.size();
        if (N < 4 || N > MaxPoints) return false;
        float Size = 0;
        for (const V3& Q : P) Size = std::max({ Size, std::fabs(Q.X - P[0].X), std::fabs(Q.Y - P[0].Y), std::fabs(Q.Z - P[0].Z) });
        const float Eps = std::max(0.05f, Size * 2e-4f);
        std::vector<std::vector<bool>> Seen;  // faces as point-membership masks, to skip duplicates
        for (size_t i = 0; i < N; i++)
            for (size_t j = i + 1; j < N; j++)
                for (size_t k = j + 1; k < N; k++)
                {
                    const V3 C = Cross(Sub(P[j], P[i]), Sub(P[k], P[i]));
                    const float L = Len(C);
                    if (L < 1e-3f) continue;
                    const V3 U{ C.X / L, C.Y / L, C.Z / L };
                    const float D = Dot(U, P[i]);
                    bool Pos = false, Neg = false;
                    std::vector<bool> On(N, false);
                    for (size_t m = 0; m < N; m++)
                    {
                        const float S = Dot(U, P[m]) - D;
                        if (S > Eps) Pos = true;
                        else if (S < -Eps) Neg = true;
                        else On[m] = true;
                    }
                    if (Pos && Neg) continue;  // the plane cuts the shape: not a face
                    if (std::find(Seen.begin(), Seen.end(), On) != Seen.end()) continue;
                    Seen.push_back(On);
                    std::vector<int> Face;
                    for (size_t m = 0; m < N; m++) if (On[m]) Face.push_back(int(m));
                    Out.push_back(std::move(Face));
                }
        return Out.size() >= 4;
    }

    void Edges(const std::vector<V3>& P, const Faces& F, const Rules& R, std::vector<Edge>& Out)
    {
        if (P.empty()) return;
        V3 Mid{ 0, 0, 0 };
        for (const V3& Q : P) { Mid.X += Q.X; Mid.Y += Q.Y; Mid.Z += Q.Z; }
        Mid = { Mid.X / P.size(), Mid.Y / P.size(), Mid.Z / P.size() };
        std::vector<V3> Normals(F.size());
        std::vector<bool> Valid(F.size());
        for (size_t f = 0; f < F.size(); f++) Valid[f] = FaceNormal(P, F[f], Mid, Normals[f]);
        for (size_t a = 0; a < F.size(); a++)
            for (size_t b = a + 1; b < F.size(); b++)
            {
                if (!Valid[a] || !Valid[b]) continue;
                const V3& N1 = Normals[a];
                const V3& N2 = Normals[b];
                if (Dot(N1, N2) > R.MaxFaceDot) continue;  // flat or gently bent: not an edge
                // Two faces of a convex shape meet along one edge: their shared points' extremes.
                std::vector<int> Common;
                for (int i : F[a])
                    if (std::find(F[b].begin(), F[b].end(), i) != F[b].end()) Common.push_back(i);
                if (Common.size() < 2) continue;
                const V3 Dir = Norm(Cross(N1, N2));
                int Lo = Common[0], Hi = Common[0];
                for (int i : Common)
                {
                    if (Dot(P[size_t(i)], Dir) < Dot(P[size_t(Lo)], Dir)) Lo = i;
                    if (Dot(P[size_t(i)], Dir) > Dot(P[size_t(Hi)], Dir)) Hi = i;
                }
                const bool bFirstUp = N1.Z >= N2.Z;
                const V3& Top = bFirstUp ? N1 : N2;
                const V3& Side = bFirstUp ? N2 : N1;
                if (Top.Z < R.MinTopUp) continue;  // nothing to ride on top of it
                const V3& A = P[size_t(Lo)];
                const V3& B = P[size_t(Hi)];
                const V3 D = Sub(B, A);
                const float Run = std::sqrt(D.X * D.X + D.Y * D.Y);
                if (Len(D) < R.MinLength || std::fabs(D.Z) > R.MaxSlope * Run) continue;
                Out.push_back({ A, B, Top, Side });
            }
    }

    void Triangles(const std::vector<V3>& P, const Faces& F, std::vector<float>& Out)
    {
        if (P.empty()) return;
        V3 Mid{ 0, 0, 0 };
        for (const V3& Q : P) { Mid.X += Q.X; Mid.Y += Q.Y; Mid.Z += Q.Z; }
        Mid = { Mid.X / P.size(), Mid.Y / P.size(), Mid.Z / P.size() };
        for (const auto& Face : F)
        {
            V3 N;
            if (Face.size() < 3 || !FaceNormal(P, Face, Mid, N)) continue;
            // Order the face's points around its centre, counter-clockwise seen from outside.
            V3 C{ 0, 0, 0 };
            for (int i : Face) { C.X += P[size_t(i)].X; C.Y += P[size_t(i)].Y; C.Z += P[size_t(i)].Z; }
            C = { C.X / Face.size(), C.Y / Face.size(), C.Z / Face.size() };
            const V3 U = Norm(Sub(P[size_t(Face[0])], C));
            const V3 W = Cross(N, U);
            std::vector<std::pair<float, int>> Order;
            for (int i : Face)
            {
                const V3 D = Sub(P[size_t(i)], C);
                Order.push_back({ std::atan2(Dot(D, W), Dot(D, U)), i });
            }
            std::sort(Order.begin(), Order.end());
            for (size_t k = 1; k + 1 < Order.size(); k++)
            {
                const V3& A = P[size_t(Order[0].second)];
                const V3& B = P[size_t(Order[k].second)];
                const V3& D = P[size_t(Order[k + 1].second)];
                Out.insert(Out.end(), { A.X, A.Y, A.Z, B.X, B.Y, B.Z, D.X, D.Y, D.Z });
            }
        }
    }

    void BoxCorners(const V3& C, const V3 A[3], std::vector<V3>& Out)
    {
        Out.clear();
        for (int i = 0; i < 8; i++)
        {
            const float Sx = (i & 1) ? 1.f : -1.f, Sy = (i & 2) ? 1.f : -1.f, Sz = (i & 4) ? 1.f : -1.f;
            Out.push_back({ C.X + Sx * A[0].X + Sy * A[1].X + Sz * A[2].X,
                            C.Y + Sx * A[0].Y + Sy * A[1].Y + Sz * A[2].Y,
                            C.Z + Sx * A[0].Z + Sy * A[1].Z + Sz * A[2].Z });
        }
    }
}
