#pragma once

// Runs Skate 3's own Hall of Meat HUD movie (fedynamic.big homscoring: THRASHER score panel,
// metric panels with icons, bonus panels) in the APT player, fed the way Skate 3 does: the
// movie's class (__Packages.homscoring) pulls its numbers from the HUDComponents.HOM_* natives
// and is driven by SetState(1 show, 2 hide, 3 update scoring). Independent of Unreal.

#include "AptPlayer.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace HomHud
{
    struct Input
    {
        bool Active = false;   // a bail is being scored
        bool Shown = false;    // there is a bail to show
        float Score = 0;       // running total
        float Metrics[5] = {}; // speed m/s, drop m, air s, bail time s, rotation degrees
        float Points[5] = {};
    };

    class Driver
    {
    public:
        // DataRoot = <mod folder>\skate-data
        bool Load(const std::string& DataRoot, std::string& Error);
        void SetLog(std::function<void(const std::string&)> Log);
        // The movie's Audio.PlaySound: "transition" (FE_SOUND_HOM_TRANSITION) or "score_red".
        std::function<void(const std::string&)> PlaySound;

        void Update(const Input& In, double Seconds);       // once per rendered frame
        void Hide();                                         // leaving skate mode
        void Render(std::vector<Apt::DrawCmd>& Out) const { Movie.Render(Out); }

        const Apt::Player& Player() const { return Movie; }
        bool Loaded() const { return bLoaded; }
        bool Visible() const { return bVisible; }

    private:
        void SetState(int State);

        Apt::Player Movie;
        Apt::Value Screen;
        Input Cur, Last;
        bool bLoaded = false;
        bool bFirst = true;
        bool bVisible = false;
        double SinceEnd = 0;   // seconds since the shown bail ended
        std::unordered_map<std::string, std::string> Strings;
        std::function<void(const std::string&)> LogFn;
    };
}
