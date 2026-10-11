#pragma once

// Runs Skate 3's own LB quick menu movie (fedynamic.big hudphonelist: the D-pad strip with
// "Go To Marker", "Place Marker", "Object Dropper") in the APT player, driven the way Skate 3
// does: the movie's class (__Packages.hudphonelist) is shown with SetState(1), refreshed with
// SetState(50), closed with SetState(0), and asks the game through natives what is available
// (Game.CanUseSessionMarker / CanPlaceSessionMarker, HUDComponents.GetPhoneListCan*). Options
// that are not available are drawn at 40% alpha by the movie itself. Independent of Unreal.

#include "AptPlayer.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace PhoneHud
{
    struct Input
    {
        bool Open = false;       // LB held
        bool CanGoTo = false;    // a marker exists and can be gone to
        bool CanPlace = false;   // a marker can be set here
        bool CanDrop = false;    // the object dropper can be opened (LB + B)
    };

    class Driver
    {
    public:
        // DataRoot = <mod folder>\skate-data
        bool Load(const std::string& DataRoot, std::string& Error);
        void SetLog(std::function<void(const std::string&)> Log);

        void Update(const Input& In, double Seconds);       // once per rendered frame
        void Hide();                                         // leaving skate mode
        // The movie, then the button icons of its button items (see ButtonIcons()).
        void Render(std::vector<Apt::DrawCmd>& Out);

        const Apt::Player& Player() const { return Movie; }
        // Button icon images, drawn as texture indices Player().Textures().size() + i. Skate 3's
        // engine draws the HUD button glyphs itself (button_item2 sets mButtonRender._test =
        // "DPad_Up_hud"); their art is not in the game's data files, so these are Skate 3's own
        // button icons from miscboot.big (button_DPad_Up.png, button_DPad_Down.png, button_B.png).
        const std::vector<std::string>& ButtonIcons() const { return IconPaths; }
        bool Loaded() const { return bLoaded; }

    private:
        void SetState(int State);
        void AddIcons(const Apt::Instance& I, const Apt::Mat& Parent, const Apt::Cx& ParentCx, std::vector<Apt::DrawCmd>& Out);
        std::vector<std::string> IconPaths;
        std::unordered_map<std::string, int> IconIndex;   // "DPad_Up_hud" -> IconPaths index
        std::string IconDir;

        Apt::Player Movie;
        Apt::Value Screen;
        Input Cur, Last;
        bool bLoaded = false;
        bool bFirst = true;
        std::unordered_map<std::string, std::string> Strings;
        std::function<void(const std::string&)> LogFn;
    };
}
