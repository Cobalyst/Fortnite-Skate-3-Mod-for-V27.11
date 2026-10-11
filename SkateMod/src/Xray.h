#pragma once

// Skate 3's Hall of Meat x-ray skeleton (HOMSkaterPresEntity): the 19 dem_bones meshes from
// marquee.big, skinned to the skater's bones and drawn over the body with the defaulthom shader.
// The shader's per-pixel result is baked into an atlas (tools/hom_skeleton.py --atlas): one tile
// per bone colour and rim level, so the whole skeleton is a single sorted triangle list.
// No engine or game types here, so it can be tested offline.

#include <cstdint>
#include <string>
#include <vector>

namespace Xray
{
    struct Camera
    {
        float Pos[3];
        float Fwd[3], Right[3], Up[3];  // view basis (Unreal space)
        float Cx, Cy, Fx, Fy;           // screen centre and focal lengths in pixels
    };

    struct Tri
    {
        float X[3], Y[3];  // screen
        float U[3], V[3];  // atlas
        float Depth;
    };

    class Skeleton
    {
    public:
        bool Load(const std::string& Path, std::string& Error);
        bool Loaded() const { return !Pieces.empty(); }
        // Skater rig bone names the meshes are skinned to.
        const std::vector<std::string>& BoneNames() const { return Bones; }

        // Which pieces show and in which colour, from the 25 HoM bone injury levels (as the
        // retail per-frame update 0x82763A40: a bone shows above level 1, red from level 4).
        // Returns true when any piece shows.
        bool SetLevels(const int8_t* Levels);

        // Skin: per rig bone (BoneNames order) a column-major 4x4 skinning matrix (world *
        // inverse bind), Unreal space. Out: the visible triangles, back to front.
        void Build(const float* Skin, const Camera& Cam, std::vector<Tri>& Out) const;

    private:
        struct Vertex { float Pos[3], Nrm[3], UV[2]; uint8_t Bone[4], Weight[4]; };
        struct Piece { std::string Name; std::vector<Vertex> V; std::vector<uint16_t> I; };
        std::vector<std::string> Bones;
        std::vector<Piece> Pieces;
        int HomPiece[25] = {};
        std::vector<int> Show;  // per piece: -1 hidden, 0 white, 1 red
        mutable std::vector<float> P, N, G;
        mutable std::vector<float> Sx, Sy, Sz;
    };
}
