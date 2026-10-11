#pragma once

// Skate 3's object dropper screen (FE screen vtable 0x822fc5f8 in the retail executable), ported:
//  - its state machine (FUN_825b40f0) and input handlers per state (free camera 825b3718,
//    catalog 825b2820, holding an object 825b1d80, the per-update part of 825b00b8), with the
//    original front-end sounds ('fe' entries, sk8_menu bank);
//  - the catalog (the park catalog of 8285a200: fe_park_categories tree, items filtered by the
//    size and style parsed from their names (82858640 / 82858418), sorted by name (82859598));
//  - Skate 3's own movie objectdropper.apt (with menu_picker / tabs / scrollbar / button_item2 /
//    swatch_renderer imported from source/controls) driven through SetState(FEObjectDropperScreenMode_*)
//    and the SkatePark.* natives (8259c018..8259e2d8).
// The editor core (camera, cursor, carried object) is DropperEditor; placed objects are DropperWorld.

#include "AptPlayer.h"
#include "Common.h"

#include <functional>
#include <string>
#include <vector>

namespace DropperScreen
{
    // DataRoot = <mod folder>\skate-data
    bool Load(const std::string& DataRoot, std::string& Error);
    void SetLog(std::function<void(const std::string&)> Log);
    // Front-end sound cue names (sounds.txt) as they are played.
    void SetSoundSink(std::function<void(const char*)> Play);

    // Opens the dropper at the skater (state 1, free camera). False if it cannot load.
    bool Open(SDK::UWorld* World, const SDK::FVector& Start, double FacingYaw);
    bool IsOpen();
    // Closed this frame (the dropper asked to exit): the client resumes the skater.
    bool TakeClosed();

    // Once per rendered frame while open: input -> screen -> editor at Skate 3's update rate.
    void Update(SDK::UWorld* World, float Dt);

    // The movie (stage pixels, Player().Textures()) followed by its button glyphs (ButtonIcons()).
    void Render(std::vector<Apt::DrawCmd>& Out);
    const Apt::Player* Player();
    const std::vector<std::string>& ButtonIcons();
    // The screens drawn over the dropper, bottom to top: 0 the dropper, 1 its properties menu
    // (ObjectQuickMenu), 2 a popup. LayerPlayer is null for a layer not shown.
    constexpr int LayerCount = 3;
    const Apt::Player* LayerPlayer(int Layer);
    void RenderLayer(int Layer, std::vector<Apt::DrawCmd>& Out);

    // Leaves the dropper at once (skate mode ending): the carried object is put back / removed.
    void Cancel();

    // Session marker menu "Reset All Objects" (LB + Y): removes every dropped object.
    void ResetAllObjects();
}
