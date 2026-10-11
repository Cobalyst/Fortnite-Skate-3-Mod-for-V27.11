#pragma once
// Joins the collision edges where two object-dropper pieces meet (used by DropperWorld::AppendCollision,
// and by the engine test). No Unreal dependencies.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DropperSeams
{
    struct V3 { double X, Y, Z; };

    // Edge code of the game's collision triangles (read off the converted pieces): low 5 bits k give the
    // edge's bend as 1 - cos = pi^2 / (2 << k) (26 = flat), 0x20 = convex; 0x40 is on most edges (meaning unknown,
    // so Join keeps each edge's own).
    inline uint8_t EdgeCode(double Cos, bool bConvex)
    {
        if (Cos > 0.9995) return 0x40 | 26;
        const double K = std::log2(9.8696044 / (2.0 * (1.0 - Cos)));
        return uint8_t(0x40 | (bConvex ? 0x20 : 0) | int(std::clamp(std::round(K), 0.0, 26.0)));
    }

    // Every piece is a closed solid, so its top face ends in a sharp edge (its own side wall) even where
    // another piece carries the surface on: the wheels catch that edge as a lip at every seam. Where a
    // triangle edge of one piece lies along a triangle edge of another piece that continues the surface
    // on the far side, both edges get the code they'd have if the two were one mesh. Triangles in UE
    // order (u0, u1, u2), edge slot j = (u_j, u_j+1); Starts = first triangle of each object, plus the end.
    inline void Join(const std::vector<float>& Tri, std::vector<uint8_t>& Edges, const std::vector<size_t>& Starts)
    {
        const size_t Objs = Starts.size() - 1;
        if (Objs < 2) return;
        auto V = [&](size_t t, int k) { const float* p = &Tri[t * 9 + size_t(k % 3) * 3]; return V3{ p[0], p[1], p[2] }; };
        auto Sub = [](const V3& a, const V3& b) { return V3{ a.X - b.X, a.Y - b.Y, a.Z - b.Z }; };
        auto Dot = [](const V3& a, const V3& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
        auto Cross = [](const V3& a, const V3& b) { return V3{ a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X }; };
        auto Normal = [&](size_t t) { V3 n = Cross(Sub(V(t, 1), V(t, 0)), Sub(V(t, 2), V(t, 0))); const double l = std::sqrt(Dot(n, n)); return l > 0 ? V3{ n.X / l, n.Y / l, n.Z / l } : n; };
        const double Tol = 2.0;   // cm
        // Only edges that are bent now can need joining.
        struct Edge { size_t T; int K; V3 Lo, Hi; };
        std::vector<std::vector<Edge>> Sharp(Objs);
        std::vector<V3> OLo(Objs, V3{ 1e30, 1e30, 1e30 }), OHi(Objs, V3{ -1e30, -1e30, -1e30 });
        for (size_t o = 0; o < Objs; o++)
            for (size_t t = Starts[o]; t < Starts[o + 1]; t++)
                for (int k = 0; k < 3; k++)
                {
                    const V3 a = V(t, k), b = V(t, k + 1);
                    OLo[o] = V3{ (std::min)(OLo[o].X, a.X), (std::min)(OLo[o].Y, a.Y), (std::min)(OLo[o].Z, a.Z) };
                    OHi[o] = V3{ (std::max)(OHi[o].X, a.X), (std::max)(OHi[o].Y, a.Y), (std::max)(OHi[o].Z, a.Z) };
                    if ((Edges[t * 3 + k] & 0x1f) >= 24) continue;
                    Sharp[o].push_back({ t, k, V3{ (std::min)(a.X, b.X) - Tol, (std::min)(a.Y, b.Y) - Tol, (std::min)(a.Z, b.Z) - Tol },
                                               V3{ (std::max)(a.X, b.X) + Tol, (std::max)(a.Y, b.Y) + Tol, (std::max)(a.Z, b.Z) + Tol } });
                }
        std::vector<std::pair<size_t, uint8_t>> Set;
        for (size_t o = 0; o < Objs; o++)
            for (size_t q = o + 1; q < Objs; q++)
            {
                if (OLo[o].X > OHi[q].X + Tol || OLo[q].X > OHi[o].X + Tol || OLo[o].Y > OHi[q].Y + Tol || OLo[q].Y > OHi[o].Y + Tol ||
                    OLo[o].Z > OHi[q].Z + Tol || OLo[q].Z > OHi[o].Z + Tol) continue;
                for (const Edge& X : Sharp[o])
                    for (const Edge& Y : Sharp[q])
                    {
                        if (X.Lo.X > Y.Hi.X || Y.Lo.X > X.Hi.X || X.Lo.Y > Y.Hi.Y || Y.Lo.Y > X.Hi.Y || X.Lo.Z > Y.Hi.Z || Y.Lo.Z > X.Hi.Z) continue;
                        const V3 p = V(X.T, X.K), d0 = Sub(V(X.T, X.K + 1), p);
                        const double L = std::sqrt(Dot(d0, d0));
                        if (L < 5.0) continue;
                        const V3 d{ d0.X / L, d0.Y / L, d0.Z / L };
                        const V3 r = Sub(V(Y.T, Y.K), p), s = Sub(V(Y.T, Y.K + 1), p);
                        const V3 cr = Cross(d, r), cs = Cross(d, s);
                        if (Dot(cr, cr) > Tol * Tol || Dot(cs, cs) > Tol * Tol) continue;   // not on one line
                        const double r0 = Dot(r, d), s0 = Dot(s, d);
                        const double M = std::sqrt(Dot(Sub(s, r), Sub(s, r)));
                        const double Over = (std::min)(L, (std::max)(r0, s0)) - (std::max)(0.0, (std::min)(r0, s0));
                        // The shared stretch must cover most of each edge, or the bend left on the rest is real.
                        const bool bX = Over >= 0.5 * L, bY = Over >= 0.5 * M;
                        if (!bX && !bY) continue;
                        // The two faces must continue each other, one each side of the line.
                        const V3 nx = Normal(X.T), ny = Normal(Y.T);
                        const V3 ox = Sub(V(X.T, X.K + 2), p), oy = Sub(V(Y.T, Y.K + 2), p);
                        const V3 px = Sub(ox, V3{ d.X * Dot(ox, d), d.Y * Dot(ox, d), d.Z * Dot(ox, d) });
                        const V3 py = Sub(oy, V3{ d.X * Dot(oy, d), d.Y * Dot(oy, d), d.Z * Dot(oy, d) });
                        if (Dot(px, py) >= 0) continue;
                        const double Cos = Dot(nx, ny);
                        if (Cos < 0.85) continue;   // a real corner between the pieces (over ~30 degrees)
                        const bool bConvex = Dot(oy, nx) < -0.05;
                        const uint8_t Code = EdgeCode(Cos, bConvex);
                        if (bX) Set.push_back({ X.T * 3 + size_t(X.K), Code });
                        if (bY) Set.push_back({ Y.T * 3 + size_t(Y.K), Code });
                    }
            }
        for (const auto& S : Set)
            if ((Edges[S.first] & 0x1f) < (S.second & 0x1f) || (S.second & 0x1f) == 26)
                Edges[S.first] = uint8_t((S.second & ~0x40) | (Edges[S.first] & 0x40));   // the 0x40 flag stays the edge's own
    }

    // Grind rails of a static dropper piece. Skate 3's static park pieces carry no grind splines of
    // their own in the converted data (0 of 542; the props have them), and the mod's ledge finder
    // doesn't see dropper pieces (they keep their exact Skate 3 collision). A rail is a run of the
    // piece's own sharp convex edges (Skate 3's edge code: 0x20 convex, bend of 50+ degrees) along the
    // top of an upward face (ledge tops, box edges, copings, rail tops), no steeper than 45 degrees,
    // chained end to end, at least MinLength cm. Edges joined to the ground or to another piece are no
    // longer sharp, so seams and ground lips never become rails. Triangles [First, End), Unreal cm.
    inline void GrindRails(const std::vector<float>& Tri, const std::vector<uint8_t>& Edges, size_t First, size_t End,
                           std::vector<std::vector<V3>>& Out, double MinLength = 50.0)
    {
        struct Seg { V3 A, B; bool Used = false; };
        std::vector<Seg> Segs;
        for (size_t t = First; t < End; t++)
        {
            const float* q = &Tri[t * 9];
            const V3 P[3] = { { q[0], q[1], q[2] }, { q[3], q[4], q[5] }, { q[6], q[7], q[8] } };
            const V3 u{ P[1].X - P[0].X, P[1].Y - P[0].Y, P[1].Z - P[0].Z }, v{ P[2].X - P[0].X, P[2].Y - P[0].Y, P[2].Z - P[0].Z };
            V3 n{ u.Y * v.Z - u.Z * v.Y, u.Z * v.X - u.X * v.Z, u.X * v.Y - u.Y * v.X };
            const double L = std::sqrt(n.X * n.X + n.Y * n.Y + n.Z * n.Z);
            if (L <= 0 || n.Z / L < 0.5) continue;   // the top face: up to 60 degrees tilt
            for (int k = 0; k < 3; k++)
            {
                const uint8_t E = Edges[t * 3 + size_t(k)];
                if (!(E & 0x20) || (E & 0x1f) > 3) continue;   // convex, bent 50+ degrees (1 - cos >= pi^2/16)
                const V3& a = P[k]; const V3& b = P[(k + 1) % 3];
                const double dx = b.X - a.X, dy = b.Y - a.Y, dz = b.Z - a.Z, Run = std::sqrt(dx * dx + dy * dy);
                if (Run < 1.0 || std::fabs(dz) > Run) continue;   // not steeper than 45 degrees
                Segs.push_back({ a, b });
            }
        }
        auto Close = [](const V3& a, const V3& b) { return std::fabs(a.X - b.X) < 1.0 && std::fabs(a.Y - b.Y) < 1.0 && std::fabs(a.Z - b.Z) < 1.0; };
        auto Dir = [](const V3& a, const V3& b) { const double dx = b.X - a.X, dy = b.Y - a.Y, dz = b.Z - a.Z, l = std::sqrt(dx * dx + dy * dy + dz * dz); return V3{ dx / l, dy / l, dz / l }; };
        auto Len = [](const std::vector<V3>& R) { double S = 0; for (size_t i = 1; i < R.size(); i++) { const double dx = R[i].X - R[i - 1].X, dy = R[i].Y - R[i - 1].Y, dz = R[i].Z - R[i - 1].Z; S += std::sqrt(dx * dx + dy * dy + dz * dz); } return S; };
        // the same edge twice (two triangles of one face share it): once
        for (size_t i = 0; i < Segs.size(); i++)
            for (size_t j = i + 1; j < Segs.size() && !Segs[i].Used; j++)
                if (!Segs[j].Used && ((Close(Segs[i].A, Segs[j].A) && Close(Segs[i].B, Segs[j].B)) || (Close(Segs[i].A, Segs[j].B) && Close(Segs[i].B, Segs[j].A))))
                    Segs[j].Used = true;
        for (size_t i = 0; i < Segs.size(); i++)
        {
            if (Segs[i].Used) continue;
            Segs[i].Used = true;
            std::vector<V3> R{ Segs[i].A, Segs[i].B };
            // grow at both ends with segments that carry on in (nearly) the same direction (curves bend gently)
            for (bool bGrew = true; bGrew;)
            {
                bGrew = false;
                for (size_t j = 0; j < Segs.size(); j++)
                {
                    if (Segs[j].Used) continue;
                    const V3 dEnd = Dir(R[R.size() - 2], R.back()), dStart = Dir(R[1], R[0]);
                    for (int Flip = 0; Flip < 2 && !Segs[j].Used; Flip++)
                    {
                        const V3 a = Flip ? Segs[j].B : Segs[j].A, b = Flip ? Segs[j].A : Segs[j].B;
                        const V3 d = Dir(a, b);
                        if (Close(a, R.back()) && dEnd.X * d.X + dEnd.Y * d.Y + dEnd.Z * d.Z > 0.9) { R.push_back(b); Segs[j].Used = bGrew = true; }
                        else if (Close(a, R.front()) && dStart.X * d.X + dStart.Y * d.Y + dStart.Z * d.Z > 0.9) { R.insert(R.begin(), b); Segs[j].Used = bGrew = true; }
                    }
                }
            }
            if (Len(R) >= MinLength) Out.push_back(std::move(R));
        }
    }

    // The same ground join for a physics piece (DMO: kickers, wedges, small quarter pipes...), whose
    // triangles stay in its own model space (Skate 3 metres, y up; the model's 0 height is where it
    // stands on the ground). An edge of an upward face lying at ground height (3 cm above to 6 cm
    // under) gets the code it would have if face and ground were one mesh. P = 9 floats (corners in
    // order), E = its 3 edge codes (edge i = corner i -> i+1). Returns edges changed.
    inline int JoinModelGround(const float* P, uint8_t* E)
    {
        const V3 a{ P[0], P[1], P[2] }, b{ P[3], P[4], P[5] }, c{ P[6], P[7], P[8] };
        const V3 u{ b.X - a.X, b.Y - a.Y, b.Z - a.Z }, v{ c.X - a.X, c.Y - a.Y, c.Z - a.Z };
        V3 n{ u.Y * v.Z - u.Z * v.Y, u.Z * v.X - u.X * v.Z, u.X * v.Y - u.Y * v.X };
        const double L = std::sqrt(n.X * n.X + n.Y * n.Y + n.Z * n.Z);
        if (L <= 0) return 0;
        n = V3{ n.X / L, n.Y / L, n.Z / L };
        if (n.Y < 0.2) return 0;   // only faces the board rides on
        const V3 Q[3] = { a, b, c };
        auto OnGround = [](const V3& p) { return p.Y <= 0.03 && p.Y >= -0.06; };
        int Changed = 0;
        for (int k = 0; k < 3; k++)
        {
            if ((E[k] & 0x1f) >= 24) continue;
            const V3& e0 = Q[k]; const V3& e1 = Q[(k + 1) % 3]; const V3& o = Q[(k + 2) % 3];
            const double dx = e1.X - e0.X, dz = e1.Z - e0.Z;
            if (dx * dx + dz * dz < 1e-4 || !OnGround(e0) || !OnGround(e1)) continue;
            const bool bConvex = o.Y < (e0.Y + e1.Y) * 0.5 - 0.005;
            const uint8_t Code = EdgeCode((std::min)(n.Y, 1.0), bConvex);
            if ((E[k] & 0x1f) < (Code & 0x1f) || (Code & 0x1f) == 26)
            {
                E[k] = uint8_t((Code & ~0x40) | (E[k] & 0x40));
                Changed++;
            }
        }
        return Changed;
    }

    // Faces buried between two stacked pieces: where a face of one piece lies flat against a face of
    // another (same plane within 1 cm, facing each other), e.g. a mega down ramp's bottom on the lower
    // ramp's top deck. In one mesh neither would exist; as two meshes the buried deck's sharp crest edge
    // sat right where the down ramp's slope begins, a wheel there touched it and the board was pushed
    // up off the slope (2026-10-11 game: thrown into the air at the joint, 5.8 -> 2.7 m/s; engine test:
    // the same launch at the same joint). Both covered faces are taken out; every face that can be
    // ridden keeps its shape. Starts = first triangle of each piece plus the end (updated). Returns the
    // number of triangles taken out.
    inline size_t DropCoveredFaces(std::vector<float>& Tri, std::vector<uint8_t>& Edges, std::vector<uint16_t>& Surfaces, std::vector<size_t>& Starts)
    {
        const size_t NT = Edges.size() / 3;
        if (Starts.size() < 3 || NT == 0) return 0;
        const size_t Objs = Starts.size() - 1;
        auto P = [&](size_t t, int k) { const float* q = &Tri[t * 9 + size_t(k) * 3]; return V3{ q[0], q[1], q[2] }; };
        auto Sub = [](const V3& a, const V3& b) { return V3{ a.X - b.X, a.Y - b.Y, a.Z - b.Z }; };
        auto Dot = [](const V3& a, const V3& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; };
        auto Cross = [](const V3& a, const V3& b) { return V3{ a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X }; };
        std::vector<V3> N(NT), Lo(NT), Hi(NT);
        std::vector<char> Ok(NT, 0);
        for (size_t t = 0; t < NT; t++)
        {
            const V3 a = P(t, 0), b = P(t, 1), c = P(t, 2);
            V3 n = Cross(Sub(b, a), Sub(c, a));
            const double L = std::sqrt(Dot(n, n));
            if (L < 1.0) continue;   // under ~1 cm^2 area: leave it
            N[t] = V3{ n.X / L, n.Y / L, n.Z / L };
            Lo[t] = V3{ (std::min)({ a.X, b.X, c.X }), (std::min)({ a.Y, b.Y, c.Y }), (std::min)({ a.Z, b.Z, c.Z }) };
            Hi[t] = V3{ (std::max)({ a.X, b.X, c.X }), (std::max)({ a.Y, b.Y, c.Y }), (std::max)({ a.Z, b.Z, c.Z }) };
            Ok[t] = 1;
        }
        // point inside triangle u (both on its plane), in the plane's dominant 2D projection
        auto Inside = [&](size_t u, const V3& q) {
            const V3& n = N[u];
            const int Drop = std::fabs(n.X) > std::fabs(n.Y) ? (std::fabs(n.X) > std::fabs(n.Z) ? 0 : 2) : (std::fabs(n.Y) > std::fabs(n.Z) ? 1 : 2);
            auto Two = [&](const V3& v, double& x, double& y) { if (Drop == 0) { x = v.Y; y = v.Z; } else if (Drop == 1) { x = v.X; y = v.Z; } else { x = v.X; y = v.Y; } };
            double ax, ay, bx, by, cx, cy, px, py;
            Two(P(u, 0), ax, ay); Two(P(u, 1), bx, by); Two(P(u, 2), cx, cy); Two(q, px, py);
            const double D = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
            if (std::fabs(D) < 1e-9) return false;
            const double l1 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / D;
            const double l2 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / D;
            const double Eps = 1e-3;
            return l1 >= -Eps && l2 >= -Eps && 1 - l1 - l2 >= -Eps;
        };
        std::vector<V3> PLo(Objs, V3{ 1e30, 1e30, 1e30 }), PHi(Objs, V3{ -1e30, -1e30, -1e30 });
        for (size_t o = 0; o < Objs; o++)
            for (size_t t = Starts[o]; t < Starts[o + 1]; t++)
                if (Ok[t]) { PLo[o] = V3{ (std::min)(PLo[o].X, Lo[t].X), (std::min)(PLo[o].Y, Lo[t].Y), (std::min)(PLo[o].Z, Lo[t].Z) };
                             PHi[o] = V3{ (std::max)(PHi[o].X, Hi[t].X), (std::max)(PHi[o].Y, Hi[t].Y), (std::max)(PHi[o].Z, Hi[t].Z) }; }
        std::vector<char> Remove(NT, 0);
        for (size_t o = 0; o < Objs; o++)
            for (size_t q = 0; q < Objs; q++)
            {
                if (q == o) continue;
                if (PLo[q].X > PHi[o].X + 1 || PLo[o].X > PHi[q].X + 1 || PLo[q].Y > PHi[o].Y + 1 || PLo[o].Y > PHi[q].Y + 1 ||
                    PLo[q].Z > PHi[o].Z + 1 || PLo[o].Z > PHi[q].Z + 1) continue;   // not touching
                for (size_t t = Starts[o]; t < Starts[o + 1]; t++)
                {
                    if (!Ok[t]) continue;
                    // the face's sample points: corners and edge middles pulled 2% toward its middle, and the middle
                    const V3 a = P(t, 0), b = P(t, 1), c = P(t, 2);
                    const V3 m{ (a.X + b.X + c.X) / 3, (a.Y + b.Y + c.Y) / 3, (a.Z + b.Z + c.Z) / 3 };
                    auto In = [&](const V3& v) { return V3{ v.X + (m.X - v.X) * 0.02, v.Y + (m.Y - v.Y) * 0.02, v.Z + (m.Z - v.Z) * 0.02 }; };
                    const V3 S[7] = { In(a), In(b), In(c), In(V3{ (a.X + b.X) / 2, (a.Y + b.Y) / 2, (a.Z + b.Z) / 2 }),
                                      In(V3{ (b.X + c.X) / 2, (b.Y + c.Y) / 2, (b.Z + c.Z) / 2 }), In(V3{ (c.X + a.X) / 2, (c.Y + a.Y) / 2, (c.Z + a.Z) / 2 }), m };
                    bool Covered[7] = {};
                    int Left = 7;
                    for (size_t u = Starts[q]; u < Starts[q + 1] && Left; u++)
                    {
                        if (!Ok[u] || Dot(N[t], N[u]) > -0.999) continue;   // facing each other
                        if (Lo[u].X > Hi[t].X + 1 || Lo[t].X > Hi[u].X + 1 || Lo[u].Y > Hi[t].Y + 1 || Lo[t].Y > Hi[u].Y + 1 ||
                            Lo[u].Z > Hi[t].Z + 1 || Lo[t].Z > Hi[u].Z + 1) continue;
                        const V3 pu = P(u, 0);
                        if (std::fabs(Dot(Sub(a, pu), N[u])) > 1.0 || std::fabs(Dot(Sub(b, pu), N[u])) > 1.0 || std::fabs(Dot(Sub(c, pu), N[u])) > 1.0) continue;   // same plane
                        for (int k = 0; k < 7; k++)
                            if (!Covered[k] && Inside(u, S[k])) { Covered[k] = true; Left--; }
                    }
                    if (!Left) Remove[t] = 1;
                }
            }
        size_t Removed = 0;
        std::vector<float> T2; std::vector<uint8_t> E2; std::vector<uint16_t> S2; std::vector<size_t> St2;
        T2.reserve(Tri.size()); E2.reserve(Edges.size()); S2.reserve(Surfaces.size());
        for (size_t o = 0; o < Objs; o++)
        {
            St2.push_back(S2.size());
            for (size_t t = Starts[o]; t < Starts[o + 1]; t++)
            {
                if (Remove[t]) { Removed++; continue; }
                T2.insert(T2.end(), Tri.begin() + long long(t * 9), Tri.begin() + long long(t * 9 + 9));
                E2.insert(E2.end(), Edges.begin() + long long(t * 3), Edges.begin() + long long(t * 3 + 3));
                S2.push_back(Surfaces[t]);
            }
        }
        St2.push_back(S2.size());
        if (!Removed) return 0;
        Tri.swap(T2); Edges.swap(E2); Surfaces.swap(S2); Starts.swap(St2);
        return Removed;
    }

    // Where a piece's riding surface comes down onto the ground, or onto another piece it rests on
    // (mega ramps stacked in the air: a lower ramp's bottom edge lies across the top of the block under
    // it, nowhere near that block's own edges, so Join can't pair it). The two are separate meshes, so
    // the piece's last edge there keeps the sharp corner it has against its own bottom / end wall, and
    // the wheels catch it as a lip (engine test: a kicker took 7.4 -> 6.3 m/s in one frame at its front
    // edge; 7.4 -> 7.2 with that edge smooth; 2026-10-11 game: thrown into the air at a stacked mega
    // ramp's foot, 5.8 -> 2.7 m/s). An edge of an upward face that lies along a supporting surface (the
    // scanned ground, or another piece's upward face; within 3 cm above it or up to 6 cm under, over its
    // whole length) gets the code it would have if the face and that surface were one mesh, as Join
    // does between pieces. The shape is unchanged. GroundAt(x, y) = ground height (Unreal cm) or NaN.
    // Starts = first triangle of each piece, plus the end. Returns edges changed.
    // bPieces = false: the ground only (cheap; no lookup of the other pieces' faces).
    template <class GroundFn>
    inline int JoinSupport(const std::vector<float>& Tri, std::vector<uint8_t>& Edges, const std::vector<size_t>& Starts, GroundFn&& GroundAt, bool bPieces = true)
    {
        const size_t NT = Edges.size() / 3;
        if (Starts.size() < 2) return 0;
        auto P = [&](size_t t, int k) { const float* q = &Tri[t * 9 + size_t(k) * 3]; return V3{ q[0], q[1], q[2] }; };
        auto Norm = [&](size_t t, V3& n) {
            const V3 a = P(t, 0), b = P(t, 1), c = P(t, 2);
            const V3 u{ b.X - a.X, b.Y - a.Y, b.Z - a.Z }, v{ c.X - a.X, c.Y - a.Y, c.Z - a.Z };
            n = V3{ u.Y * v.Z - u.Z * v.Y, u.Z * v.X - u.X * v.Z, u.X * v.Y - u.Y * v.X };
            const double L = std::sqrt(n.X * n.X + n.Y * n.Y + n.Z * n.Z);
            if (L <= 0) return false;
            n = V3{ n.X / L, n.Y / L, n.Z / L };
            return true;
        };
        std::vector<uint32_t> Piece(NT, 0);
        for (size_t o = 0; o + 1 < Starts.size(); o++)
            for (size_t t = Starts[o]; t < Starts[o + 1] && t < NT; t++) Piece[t] = uint32_t(o);
        // Upward faces of every piece, by 1 m cell (what an edge can rest on).
        const double Cell = 100.0;
        std::vector<V3> Normals(NT);
        std::vector<char> Up(NT, 0);
        std::unordered_map<long long, std::vector<uint32_t>> Cells;
        auto Key = [](long long i, long long j) { return (i << 32) ^ (j & 0xffffffffLL); };
        for (size_t t = 0; t < NT; t++)
        {
            if (!Norm(t, Normals[t]) || Normals[t].Z < 0.2) continue;
            Up[t] = 1;
            if (!bPieces) continue;
            const V3 a = P(t, 0), b = P(t, 1), c = P(t, 2);
            const long long I0 = (long long)std::floor((std::min)({ a.X, b.X, c.X }) / Cell), I1 = (long long)std::floor((std::max)({ a.X, b.X, c.X }) / Cell);
            const long long J0 = (long long)std::floor((std::min)({ a.Y, b.Y, c.Y }) / Cell), J1 = (long long)std::floor((std::max)({ a.Y, b.Y, c.Y }) / Cell);
            if ((I1 - I0 + 1) * (J1 - J0 + 1) > 4096) continue;
            for (long long i = I0; i <= I1; i++)
                for (long long j = J0; j <= J1; j++) Cells[Key(i, j)].push_back(uint32_t(t));
        }
        // The supporting surface at (x, y) for a point at height z of piece Self: height and normal.
        auto Support = [&](double X, double Y, double Z, uint32_t Self, double& H, V3& N)
        {
            bool bFound = false;
            double Best = 1e30;
            auto Try = [&](double h, const V3& n) {
                if (!(Z - h <= 3.0 && Z - h >= -6.0)) return;
                if (std::fabs(Z - h) < Best) { Best = std::fabs(Z - h); H = h; N = n; bFound = true; }
            };
            const float G = GroundAt(float(X), float(Y));
            if (std::isfinite(G)) Try(G, V3{ 0, 0, 1 });
            auto It = Cells.find(Key((long long)std::floor(X / Cell), (long long)std::floor(Y / Cell)));
            if (It == Cells.end()) return bFound;
            for (uint32_t t : It->second)
            {
                if (Piece[t] == Self) continue;
                const V3 a = P(t, 0), b = P(t, 1), c = P(t, 2);
                const double D = (b.Y - c.Y) * (a.X - c.X) + (c.X - b.X) * (a.Y - c.Y);
                if (std::fabs(D) < 1e-9) continue;
                const double L1 = ((b.Y - c.Y) * (X - c.X) + (c.X - b.X) * (Y - c.Y)) / D;
                const double L2 = ((c.Y - a.Y) * (X - c.X) + (a.X - c.X) * (Y - c.Y)) / D;
                const double L3 = 1 - L1 - L2;
                const double Eps = 1e-4;
                if (L1 < -Eps || L2 < -Eps || L3 < -Eps) continue;
                Try(L1 * a.Z + L2 * b.Z + L3 * c.Z, Normals[t]);
            }
            return bFound;
        };
        int Changed = 0;
        for (size_t t = 0; t < NT; t++)
        {
            if (!Up[t]) continue;
            const V3& n = Normals[t];
            for (int k = 0; k < 3; k++)
            {
                uint8_t& E = Edges[t * 3 + size_t(k)];
                if ((E & 0x1f) >= 24) continue;   // already smooth
                const V3 e0 = P(t, k), e1 = P(t, (k + 1) % 3), o = P(t, (k + 2) % 3);
                const double dx = e1.X - e0.X, dy = e1.Y - e0.Y, Len = std::sqrt(dx * dx + dy * dy);
                if (Len < 1.0) continue;
                // the whole edge on a support: both ends and every metre between
                const int Steps = 1 + int(Len / 100.0);
                bool bOn = true;
                V3 Ns{ 0, 0, 1 };
                for (int s = 0; s <= Steps && bOn; s++)
                {
                    const double f = double(s) / Steps;
                    double H = 0; V3 N{};
                    bOn = Support(e0.X + dx * f, e0.Y + dy * f, e0.Z + (e1.Z - e0.Z) * f, Piece[t], H, N);
                    if (bOn && s == Steps / 2) Ns = N;
                }
                if (!bOn) continue;
                double Cos = n.X * Ns.X + n.Y * Ns.Y + n.Z * Ns.Z;
                if (Cos < 0.0) continue;   // more than a right angle to its support: a real corner
                // the face rising from its support is a concave bend; one going on down into it, convex
                const V3 r{ o.X - e0.X, o.Y - e0.Y, o.Z - e0.Z };
                const bool bConvex = r.X * Ns.X + r.Y * Ns.Y + r.Z * Ns.Z < -0.5;
                const uint8_t Code = EdgeCode((std::min)(Cos, 1.0), bConvex);
                if ((E & 0x1f) < (Code & 0x1f) || (Code & 0x1f) == 26)
                {
                    E = uint8_t((Code & ~0x40) | (E & 0x40));
                    Changed++;
                }
            }
        }
        return Changed;
    }

}
