#pragma once

// Runs Skate 3's own trickline HUD movie (fedata.big trickdisplay2: timeline, art and
// ActionScript) in the APT player, and feeds it the way Skate 3's ScoreModule does: the
// movie's class (__Packages.trickdisplay2) pulls its numbers from the _global.Tricks /
// HUDComponents natives and is told what changed through SetState(n) and the
// _global.TrickDisplay callbacks. Independent of Unreal (shared with Tools' test harness).

#include "AptPlayer.h"

#include <string>
#include <vector>

namespace SkateHud
{
    // What Skate 3's ScoreModule publishes, after the latest simulation tick.
    struct Input
    {
        float Sequence = 0;       // points of the trick sequence (live, then as landed)
        bool SequenceActive = false;  // the sequence is still going (score counting)
        bool Clean = false;
        bool Sketchy = false;
        float LineSeconds = 0;    // time left on the line timer
        float LineScore = 0;      // points banked into the line
        float Multiplier = 1;     // 1, 1.5, 2, 3
        bool Switch = false;
        bool Fakie = false;
        std::string Trick;        // name of the current trick
    };

    enum : int { EventTrick = 1, EventModified = 2, EventBail = 3, EventLine = 4 };

    class Driver
    {
    public:
        // DataRoot = <mod folder>\skate-data
        bool Load(const std::string& DataRoot, std::string& Error);
        void SetLog(std::function<void(const std::string&)> Log);

        void Start();                                       // entering skate mode
        void Stop();                                        // leaving it
        void Event(int Kind, const std::string& Text);      // engine HUD events, in order
        void Update(const Input& In, double Seconds);       // once per rendered frame
        void Render(std::vector<Apt::DrawCmd>& Out) const { Movie.Render(Out); }

        const Apt::Player& Player() const { return Movie; }
        bool Loaded() const { return bLoaded; }

    private:
        void SetState(int State);
        void CallTrickDisplay(const char* Method);
        bool DisplayOpen();

        Apt::Player Movie;
        Apt::Value Screen;      // the trickdisplay2 instance (registered through ScreenManager.OnLoaded)
        Input Cur, Last;
        bool bLoaded = false;
        bool bStarted = false;
        bool bFirst = true;
        double SinceSequence = 1e9;  // seconds since the last sequence ended
        double Idle = 0;             // seconds with no sequence and no line
        std::unordered_map<std::string, std::string> Strings;
        std::function<void(const std::string&)> LogFn;
    };

    std::string Thousands(double Value);
}
