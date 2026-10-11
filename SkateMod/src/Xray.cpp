#include "Xray.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Xray
{
    namespace
    {
        // Retail render item order (FUN_82762810: one "Bones_*" item per HoM bone).
        const char* const HomPieceNames[25] = {
            "skull", "neck", "hand_left", "forearm_left", "bicep_left", "bicep_left", "hand_right",
            "forearm_right", "bicep_right", "bicep_right", "ribcage", "ribcage", "lower_spine",
            "lower_spine", "toes_left", "ankle_left", "calf_left", "thigh_left", "toes_right",
            "ankle_right", "calf_right", "thigh_right", "hips", "skull", "hips",
        };
        // Atlas layout (tools/hom_skeleton.py): 4 x 4 tiles, rim levels along x, white rows then red.
        constexpr int Levels = 8;
        constexpr int TilesX = 4;
        constexpr float TileSize = 1.f / 4.f;
        // m_params[1].w of material_character/default_hom: the rim falloff exponent.
        constexpr float RimPower = 2.f;

        float Dot(const float* A, const float* B) { return A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; }
    }

    bool Skeleton::Load(const std::string& Path, std::string& Error)
    {
        FILE* F = fopen(Path.c_str(), "rb");
        if (!F) { Error = "cannot open " + Path; return false; }
        std::vector<uint8_t> D;
        uint8_t Buf[65536];
        size_t N;
        while ((N = fread(Buf, 1, sizeof(Buf), F)) > 0) D.insert(D.end(), Buf, Buf + N);
        fclose(F);
        size_t O = 0;
        auto Need = [&](size_t Bytes) { return O + Bytes <= D.size(); };
        auto U32 = [&]() { uint32_t V; memcpy(&V, &D[O], 4); O += 4; return V; };
        auto Name = [&]() { std::string S(reinterpret_cast<const char*>(&D[O]), strnlen(reinterpret_cast<const char*>(&D[O]), 24)); O += 24; return S; };
        if (!Need(16) || memcmp(D.data(), "XRAY", 4) != 0) { Error = "not a skeleton.bin"; return false; }
        O = 4;
        const uint32_t Version = U32(), NumBones = U32(), NumPieces = U32();
        if (Version != 1 || !Need(24 * size_t(NumBones))) { Error = "bad skeleton.bin header"; return false; }
        Bones.clear();
        Pieces.clear();
        for (uint32_t i = 0; i < NumBones; i++) Bones.push_back(Name());
        for (uint32_t p = 0; p < NumPieces; p++)
        {
            if (!Need(32)) { Error = "truncated skeleton.bin"; return false; }
            Piece Pc;
            Pc.Name = Name();
            const uint32_t Nv = U32(), Ni = U32();
            if (!Need(40 * size_t(Nv) + 2 * size_t(Ni))) { Error = "truncated skeleton.bin"; return false; }
            Pc.V.resize(Nv);
            for (auto& V : Pc.V) { memcpy(&V, &D[O], 40); O += 40; }
            Pc.I.resize(Ni);
            memcpy(Pc.I.data(), &D[O], 2 * size_t(Ni));
            O += 2 * size_t(Ni);
            for (const Vertex& V : Pc.V)
                for (int k = 0; k < 4; k++)
                    if (V.Weight[k] && V.Bone[k] >= NumBones) { Error = "bad bone index in " + Pc.Name; return false; }
            Pieces.push_back(std::move(Pc));
        }
        for (int b = 0; b < 25; b++)
        {
            HomPiece[b] = -1;
            for (size_t p = 0; p < Pieces.size(); p++)
                if (Pieces[p].Name == std::string("dem_bones_") + HomPieceNames[b]) HomPiece[b] = int(p);
            if (HomPiece[b] < 0) { Error = std::string("no piece for ") + HomPieceNames[b]; Pieces.clear(); return false; }
        }
        Show.assign(Pieces.size(), -1);
        return true;
    }

    bool Skeleton::SetLevels(const int8_t* Lv)
    {
        std::fill(Show.begin(), Show.end(), -1);
        bool Any = false;
        // Later bones overwrite a shared mesh's colour, as in the retail loop.
        for (int b = 0; b < 25; b++)
            if (Lv[b] > 1)
            {
                Show[size_t(HomPiece[b])] = Lv[b] >= 4 ? 1 : 0;
                Any = true;
            }
        return Any;
    }

    void Skeleton::Build(const float* Skin, const Camera& Cam, std::vector<Tri>& Out) const
    {
        Out.clear();
        for (size_t p = 0; p < Pieces.size(); p++)
        {
            if (Show[p] < 0) continue;
            const Piece& Pc = Pieces[p];
            const size_t Nv = Pc.V.size();
            P.resize(Nv * 3); N.resize(Nv * 3); G.resize(Nv);
            Sx.resize(Nv); Sy.resize(Nv); Sz.resize(Nv);
            for (size_t v = 0; v < Nv; v++)
            {
                const Vertex& V = Pc.V[v];
                float Pw[3] = {}, Nw[3] = {};
                for (int k = 0; k < 4; k++)
                {
                    if (!V.Weight[k]) continue;
                    const float* M = Skin + 16 * V.Bone[k];
                    const float W = V.Weight[k] / 255.f;
                    for (int r = 0; r < 3; r++)
                    {
                        Pw[r] += W * (M[r] * V.Pos[0] + M[4 + r] * V.Pos[1] + M[8 + r] * V.Pos[2] + M[12 + r]);
                        Nw[r] += W * (M[r] * V.Nrm[0] + M[4 + r] * V.Nrm[1] + M[8 + r] * V.Nrm[2]);
                    }
                }
                const float Len = std::sqrt(Dot(Nw, Nw));
                if (Len > 0) for (float& C : Nw) C /= Len;
                float ToEye[3] = { Cam.Pos[0] - Pw[0], Cam.Pos[1] - Pw[1], Cam.Pos[2] - Pw[2] };
                const float Dist = std::sqrt(Dot(ToEye, ToEye));
                if (Dist > 0) for (float& C : ToEye) C /= Dist;
                // defaulthom_defaultPS: F = 1 - sat(N.V); g = 1 - F^power.
                const float F = 1.f - std::clamp(Dot(Nw, ToEye), 0.f, 1.f);
                G[v] = 1.f - std::pow(F, RimPower);
                memcpy(&P[v * 3], Pw, 12);
                memcpy(&N[v * 3], Nw, 12);
                const float D[3] = { Pw[0] - Cam.Pos[0], Pw[1] - Cam.Pos[1], Pw[2] - Cam.Pos[2] };
                Sz[v] = Dot(D, Cam.Fwd);
                const float Z = std::max(Sz[v], 1.f);
                Sx[v] = Cam.Cx + Cam.Fx * Dot(D, Cam.Right) / Z;
                Sy[v] = Cam.Cy - Cam.Fy * Dot(D, Cam.Up) / Z;
            }
            for (size_t t = 0; t + 2 < Pc.I.size(); t += 3)
            {
                const uint16_t A = Pc.I[t], B = Pc.I[t + 1], C = Pc.I[t + 2];
                if (Sz[A] < 1.f || Sz[B] < 1.f || Sz[C] < 1.f) continue;
                // Back faces (closed meshes): the face's smoothed normal points away from the eye.
                float Fn[3], Ctr[3];
                for (int r = 0; r < 3; r++)
                {
                    Fn[r] = N[A * 3 + r] + N[B * 3 + r] + N[C * 3 + r];
                    Ctr[r] = Cam.Pos[r] - (P[A * 3 + r] + P[B * 3 + r] + P[C * 3 + r]) / 3.f;
                }
                if (Dot(Fn, Ctr) <= 0.f) continue;
                const float Gt = (G[A] + G[B] + G[C]) / 3.f;
                const int Level = std::clamp(int(std::lround(Gt * (Levels - 1))), 0, Levels - 1);
                const int Tile = Show[p] * Levels + Level;
                const float Ou = float(Tile % TilesX) * TileSize, Ov = float(Tile / TilesX) * TileSize;
                Tri T;
                const uint16_t Ix[3] = { A, B, C };
                for (int k = 0; k < 3; k++)
                {
                    const Vertex& V = Pc.V[Ix[k]];
                    T.X[k] = Sx[Ix[k]];
                    T.Y[k] = Sy[Ix[k]];
                    T.U[k] = Ou + std::clamp(V.UV[0], 0.f, 1.f) * TileSize;
                    T.V[k] = Ov + std::clamp(V.UV[1], 0.f, 1.f) * TileSize;
                }
                T.Depth = Sz[A] + Sz[B] + Sz[C];
                Out.push_back(T);
            }
        }
        std::sort(Out.begin(), Out.end(), [](const Tri& L, const Tri& R) { return L.Depth > R.Depth; });
    }
}
