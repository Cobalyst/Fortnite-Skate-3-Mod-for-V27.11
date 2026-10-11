#pragma once

// Runs Skate 3's own action overlay movie (actionoverlay2) in its DMO grab state, the way the game
// does while the skater holds a prop on foot: the HUD state (ours 82648608) calls the screen's
// SetState(4) = kActionOverlayState_DMOGrab, whose InitDMOHUD asks HUDComponents.DMO_GetParams for
// five flags and lays out the three rows itself:
//   DMO0 Upright Object (D-pad up), DMO1 Reset Objects (D-pad down), DMO2 Add to / Remove from
//   Session Marker (D-pad right / left), unavailable rows at 30% alpha.
// SetState(0) hides it. Independent of Unreal.

#include "AptPlayer.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace ActionOverlay
{
    struct Input
    {
        bool Show = false;           // the skater holds a prop
        // DMO_GetParams, as InitDMOHUD reads them: [0] Upright unavailable (the prop is already
        // upright) -> DMO0 at 30%; [1] Reset unavailable -> DMO1 at 30%; [2] a session marker is
        // set (else DMO2 "Add" at 30%); [3] the prop is in it (DMO2 "Remove", D-pad left);
        // [4] the marker is full (DMO2 "Max objects in Session Marker" at 30%).
        bool Params[5] = {};
    };

    class Driver
    {
    public:
        bool Load(const std::string& DataRoot, std::string& Error);   // DataRoot = ...\skate-data
        void SetLog(std::function<void(const std::string&)> Log);
        void Update(const Input& In, double Seconds);
        void Hide();
        void Render(std::vector<Apt::DrawCmd>& Out);
        const Apt::Player& Player() const { return Movie; }
        const std::vector<std::string>& ButtonIcons() const { return IconPaths; }
        bool Loaded() const { return bLoaded; }

    private:
        void SetState(int State);
        void SetButton(const Apt::Value& Item, const std::string& Name);
        void AddIcons(const Apt::Instance& I, const Apt::Mat& Parent, const Apt::Cx& ParentCx, std::vector<Apt::DrawCmd>& Out);
        std::vector<std::string> IconPaths;
        std::unordered_map<std::string, int> IconIndex;
        std::string IconDir;
        Apt::Player Movie;
        Apt::Value Screen;
        Input Cur, Last;
        bool bLoaded = false, bFirst = true;
        std::unordered_map<std::string, std::string> Strings;
        std::function<void(const std::string&)> LogFn;
    };
}
