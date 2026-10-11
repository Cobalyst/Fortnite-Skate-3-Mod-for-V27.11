#pragma once

// Placed Skate 3 object-dropper objects in the Fortnite world: their original models and textures
// (procedural mesh sections), and their original collision (RenderWare triangles with native edge
// codes and surface ids) and authored grind splines for the skate engine. Client side only.

#include "Common.h"
#include "DropperData.h"
#include "../../SkateEngine/skate_engine.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace DropperWorld
{
    // A rotation as a unit quaternion (Unreal convention: X, Y, Z, W).
    struct Quat { double X = 0, Y = 0, Z = 0, W = 1; };
    Quat FromRotator(double Pitch, double Yaw, double Roll);   // degrees, Unreal FRotator order
    SDK::FRotator ToRotator(const Quat& Q);
    SDK::FVector Rotate(const Quat& Q, const SDK::FVector& V);
    Quat Mul(const Quat& A, const Quat& B);

    // Skate 3 model space (metres, Y up, right-handed) -> Unreal local space (cm, Z up).
    inline SDK::FVector FromSkate(float X, float Y, float Z) { return SDK::FVector{ X * 100.0, Z * 100.0, Y * 100.0 }; }

    struct Placed
    {
        int Handle = 0;
        std::string Id;               // catalog id (model id)
        SDK::FVector Position{};      // Unreal world, cm
        Quat Rotation;
        bool bGhost = false;          // being carried by the dropper cursor (drawn, not solid)
        // Where the dropper put it (the DMO record's +0x40 frame): physics moves Position and
        // Rotation, Reset Objects puts it back here.
        SDK::FVector HomePosition{};
        Quat HomeRotation;
    };

    bool Load(std::string& Error);                 // the catalog (once)
    DropperData::Library& Library();

    int Add(SDK::UWorld* World, const std::string& Id, const SDK::FVector& Position, const Quat& Rotation, bool bGhost = false);
    void Move(int Handle, const SDK::FVector& Position, const Quat& Rotation);
    // Draws the object at a pose without changing where it is (smoothing between editor updates).
    void ShowAt(int Handle, const SDK::FVector& Position, const Quat& Rotation);
    void SetGhost(int Handle, bool bGhost);
    // Skate 3's object display modes (vtbl+0x4c, stored at +0xe4 and read by the draw 82841ae0):
    // 0 normal ("maskstencil"), 1 under the cursor ("green"), 2 / 4 carried ("foreground_noblur"),
    // 3 "foreground_blur", 5 "red". The colours are the visual indicator records of the same names
    // (visual_indicator_green / _red textures), drawn over the piece.
    void SetDisplayMode(int Handle, int Mode);
    void Remove(int Handle);
    // The object becomes another catalog piece where it is (the properties menu's style swap,
    // 828667d0); its handle stays. False if the piece is unknown.
    bool Replace(SDK::UWorld* World, int Handle, const std::string& NewId);
    // Placement validity (82863038 with collision on): the object's box, Inset cm smaller, overlaps
    // another placed object's box (oriented boxes). Ignore: objects left out (its own group).
    bool OverlapsOthers(int Handle, const std::vector<int>& Ignore, double InsetCm);
    void RemoveAll();
    const Placed* Get(int Handle);
    const std::vector<Placed>& All();

    // Bumped whenever solid geometry changes (add / move / remove / ghost): the client re-sends its
    // collision patch with the new objects.
    uint64_t Generation();

    // Native triangles (Unreal cm, wound for sk_set_collision2: (b-a)x(c-a) out of the surface) with
    // edge codes and surface ids, and grind rails (segment chords, which is what the game's grind
    // contact uses), for every solid object whose bounds come within Radius of Centre.
    // GroundAt(x, y): the scanned ground height (cm, NaN = none), for the edges where a piece meets it.
    void AppendCollision(const SDK::FVector& Centre, double Radius, std::vector<float>& Triangles, std::vector<uint8_t>& Edges,
                         std::vector<uint16_t>& Surfaces, std::vector<float>& RailPoints, std::vector<uint32_t>& RailLengths,
                         const std::function<float(float, float)>& GroundAt);

    // Skate 3 DMOs ("dynamic" catalog objects with a RenderWare rigid body): rigid bodies in the
    // skate engine (sk_set_dmos), built from each object's own PartDefinition, collision volumes and
    // collision mesh. Static terrain stays in the collision patch. AppendCollision leaves their
    // triangles out (the engine holds them at the bodies' poses).
    bool IsPhysical(const std::string& Id);
    // A lit opaque material showing Diffuse (the same Fortnite parent the placed objects use: light
    // and shadows), or nullptr when none takes the texture. Used for the skateboard too.
    SDK::UMaterialInstanceDynamic* LitMaterial(SDK::UObject* Context, SDK::UTexture2D* Diffuse);
    // Also the grab splines (the object's 0xEB001F splines, as the skob keeps them for grinding):
    // Skate 3's off-board grab (hold RB) pushes and pulls the body through them.
    void BuildPhysics(std::vector<SkDmo>& Dmos, std::vector<SkDmoVolume>& Volumes, std::vector<SkDmoTriangle>& Triangles,
                      std::vector<SkDmoSpline>& Splines, std::vector<float>& Points);
    // A body's pose after an engine tick (Unreal space). Moves the object without bumping
    // Generation while it moves; returns true when it has just come to rest somewhere new (the
    // caller then bumps Generation so grind rails, the server copies and the patch follow).
    bool ApplyPhysicsPose(int Handle, const SDK::FVector& Position, const Quat& Rotation, bool bAwake);
    void Touch();   // a prop came to rest: PoseGeneration++ (not Generation: the set is the same)
    uint64_t PoseGeneration();

    // The client's object actors (the world sampler's traces skip them: the skate engine gets their
    // exact Skate 3 collision separately).
    const std::vector<SDK::AActor*>& Actors();

    // Client: publish the solid objects to the server (Share.h) when they changed.
    void PublishToServer();
    // Server: keep invisible solid copies of the client's objects (collision only).
    void MirrorOnServer(SDK::UWorld* World);

    // The model's horizontal bounds centre in Unreal local space (cm), for centring on a cursor.
    SDK::FVector LocalCentre(const std::string& Id);
    SDK::FVector LocalSize(const std::string& Id);   // the piece's box size on its own axes (cm, Unreal axes)

    // World-space bounds of a placed object (cm), from its model bounds.
    void Bounds(int Handle, SDK::FVector& Min, SDK::FVector& Max);

    // ---- The dropper's undo / redo: whole-park snapshots (82502500 saves the park, 824FFA08 loads
    // one back) ----
    struct Snapshot { std::vector<Placed> Objects; };
    Snapshot Save();
    // Puts the park back as saved (objects keep their handles; missing ones come back).
    void Load(SDK::UWorld* World, const Snapshot& S);

    // ---- Holding a prop on foot: the action overlay's four actions ----
    // Session marker DMOs (Skate 3's SessionMarker manager, ctor 82873AB8: a map of DMO -> frame,
    // at most 50). Add (vt+0x2C 828745A8) saves the prop and, recursively, every prop touching it
    // (82C21948: bounds within 5 cm); Remove (vt+0x30 828748F0) drops one; placing or clearing a
    // marker forgets them all (vt+0x3C); going to the marker puts each back (vt+0x38 82874AD0).
    bool MarkerHas(int Handle);
    bool MarkerFull();
    void MarkerAdd(int Handle);
    void MarkerRemove(int Handle);
    void MarkerClear();
    void MarkerRestore();
    // Upright Object is unavailable when the prop's up axis is within 10 degrees of world up
    // (82C2D330).
    bool IsUpright(int Handle);
    // Reset Objects (world vt+0x24 82C221D0): the prop and the props on it back where the dropper
    // put them; available (vt+0x1C 82C21BE0) when nothing else is in those places.
    bool CanReset(int Handle);
    void Reset(int Handle);
}
