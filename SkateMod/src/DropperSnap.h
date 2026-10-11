#pragma once

// Skate 3's object dropper snapping (UCC::SnappingBatch, the cursor's "Snap To List" / "Snap From List"
// in 82863e94 / 82863650, solved by 8284a240):
//  - skate.park terrain pieces carry snap locators (arena list 0xeb001e, type 8): points and faces
//    (the corners of one group; 828402a8 / 8284c290), extracted to skate-data/dropper/snaps.txt;
//  - the carried piece's points / faces are matched with those of the placed terrain pieces near it
//    (TerrainItemProvider vtable +0x48, 82857210) and with points where short rays from the piece hit
//    the world (8284b948, "Procedural Snap To Point");
//  - a pair counts when its normals face each other within 45 degrees (8284a1b8) and it is closer
//    than 1.5 m (editor_cursor B0C05776); point-point by distance (828497e0), point-face by the
//    distance to the face's 0.75 m grid inside the face (828499a8), face-face by the face's corners or
//    grid samples (82849f78); pairs of two pieces count 0.025 of a piece-to-ground pair;
//  - the best pair turns the piece so the two frames face each other with the up axes on the nearest
//    quarter turn (8284b168) and moves it there (8284c018).
#include "Common.h"
#include "DropperWorld.h"

#include <string>
#include <vector>

namespace DropperSnap
{
    // skate-data/dropper/snaps.txt (once). False if it cannot be read.
    bool Load(const std::string& Path, std::string& Error);
    bool HasLocators(const std::string& Id);

    // The carried piece Held (Others: its group, left out of the targets) at Pos / Rot: where snapping
    // puts it. False = nothing to snap to.
    bool Solve(SDK::UWorld* World, int Held, const std::vector<int>& Others, SDK::FVector& Pos, DropperWorld::Quat& Rot);
}
