#pragma once

// Skate 3 object-dropper objects as converted by Tools/dropper_convert.py (skate-data/dropper):
// the catalog (objects.txt) and each object's .skob (render meshes + materials, the RenderWare
// collision mesh with its native edge codes and surface ids, physics volumes, the raw rigid body
// blocks and the authored grind splines). Model space is Skate 3's (metres, Y up). No Unreal here.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace DropperData
{
    struct Material
    {
        uint32_t Flags = 0;            // 1 = alpha test (dynamicObjectAlphaTest)
        std::string Diffuse, Normal, Specular;   // texture ids -> skate-data/dropper/textures/<id>.png
    };

    struct Mesh
    {
        uint32_t Material = 0;
        // 13 floats per vertex: position[3] normal[3] tangent[3] uv0[2] uv1[2]
        std::vector<float> Vertices;
        std::vector<uint32_t> Indices;
        uint32_t VertexCount() const { return uint32_t(Vertices.size() / 13); }
    };

    struct CollisionTriangle
    {
        float V[9];          // three corners
        uint16_t Surface;    // RW surface id (audio | physics << 7 | pattern << 12)
        uint8_t Edges[3];    // RW ClusteredMesh edge codes (edge i = corner i -> i+1)
        uint8_t UnitFlags;   // the unit's flag byte
    };

    struct Volume
    {
        uint32_t Type;       // 1 sphere, 2 capsule, 3 triangle, 4 box, 5 cylinder
        float Rot[9];        // rows
        float Pos[3];
        float Params[4];     // box: half extents; sphere: radius; capsule/cylinder: radius, half height
        uint32_t Surface;
    };

    struct GrindSpline
    {
        uint32_t Flags = 0, Surface = 0;
        float Normal[3] = {};
        std::vector<float> Segments;   // per segment 4 cubic Bezier control points x 3 floats
        uint32_t SegmentCount() const { return uint32_t(Segments.size() / 12); }
    };

    struct Object
    {
        float Min[3] = {}, Max[3] = {};
        std::vector<Material> Materials;
        std::vector<Mesh> Meshes;
        std::vector<CollisionTriangle> Collision;
        std::vector<Volume> Volumes;
        std::vector<uint8_t> RigidBody, RigidBodyShape;   // RW 0x40007 / 0x40006, big-endian as stored
        std::vector<GrindSpline> Grinds;
    };

    struct CatalogEntry
    {
        std::string Db;        // "dynamic" (DMO, droppable anywhere) / "static" (park terrain)
        std::string Category;  // DMO_Barriers, Rails, Vert, ...
        std::string Id;        // 0x... model id (file name)
        std::string Name;      // e.g. ae_DMO_Glbl_BarrierPlastic
        std::string Arena;
        std::map<std::string, std::string> Caps;   // non-empty cap.* properties
    };

    class Library
    {
    public:
        bool Load(const std::string& Root, std::string& Error);   // Root = ...\skate-data\dropper
        const std::vector<CatalogEntry>& Catalog() const { return Entries; }
        const CatalogEntry* Find(const std::string& Id) const;
        // Loads (once) and returns the object; null if its file is missing or invalid.
        std::shared_ptr<const Object> Get(const std::string& Id, std::string* Error = nullptr);
        std::string TexturePath(const std::string& TextureId) const { return Dir + "\\textures\\" + TextureId + ".png"; }

    private:
        std::string Dir;
        std::vector<CatalogEntry> Entries;
        std::map<std::string, size_t> ById;
        std::map<std::string, std::shared_ptr<const Object>> Loaded;
    };

    bool ReadSkob(const std::string& Path, Object& Out, std::string& Error);
}
