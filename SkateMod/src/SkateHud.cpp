#include "SkateHud.h"

#include <cmath>
#include <fstream>

namespace SkateHud
{
    std::string Thousands(double Value)
    {
        long long N = (long long)std::llround(Value);
        std::string S = std::to_string(N < 0 ? -N : N);
        for (int i = int(S.size()) - 3; i > 0; i -= 3) S.insert(size_t(i), ",");
        return N < 0 ? "-" + S : S;
    }

    void Driver::SetLog(std::function<void(const std::string&)> Log)
    {
        LogFn = Log;
        Movie.LogLine = [this](const std::string& S) { if (LogFn) LogFn("[apt] " + S); };
    }

    bool Driver::Load(const std::string& DataRoot, std::string& Error)
    {
        // Skate 3 English strings (KEY<TAB>text) for the movie's ID_ text keys.
        std::ifstream In(DataRoot + "\\strings_en.txt", std::ios::binary);
        std::string Line;
        while (std::getline(In, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            size_t Start = Line.find_first_not_of(' ');
            const size_t Tab = Line.find('\t');
            if (Start == std::string::npos || Tab == std::string::npos || Tab < Start) continue;
            Strings[Line.substr(Start, Tab - Start)] = Line.substr(Tab + 1);
        }
        Movie.Localize = [this](const std::string& Key)
        {
            auto It = Strings.find(Key);
            return It == Strings.end() ? std::string() : It->second;
        };

        // The movie's three fonts are Skate 3's bitmap fonts (miscboot.big), same families.
        const std::string Font = DataRoot + "\\font\\";
        const std::unordered_map<std::string, std::string> Fonts = {
            { "Futura Shadow", Font + "futurashadow.txt" },
            { "FuturaOuterGlow", Font + "futuraglow.txt" },
            { "Futura Std Medium", Font + "futuraheavy.txt" },
        };
        if (!Movie.Load(DataRoot + "\\extracted\\trickdisplay", "trickdisplay2", DataRoot + "\\hud\\textures", Fonts))
        {
            Error = Movie.Error();
            return false;
        }

        // ---- the natives the movie's script calls ----
        using Apt::Value;
        using Apt::Args;
        Apt::ObjRef G = Movie.Global();
        auto Num = [](double V) { return Value::Number(V); };
        auto Obj = [](Apt::ObjRef O) { return Value::Object(O); };

        Apt::ObjRef Tricks = Movie.NewObject();
        Tricks->props["GetLineTimerMaxPoints"] = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value::Number(0); });
        // [0] sequence points, [1] as text, [2] sketchy, [3] clean, [4] line time left,
        // [5] line points, [6] as text
        Tricks->props["GetGeneralInfo"] = Movie.NewNative([this](Apt::Player& P, const Value&, Args&)
        {
            return Value::Object(P.NewArray({
                Value::Number(std::floor(Cur.Sequence)), Value::String(Thousands(std::floor(Cur.Sequence))),
                Value::Boolean(Cur.Sketchy), Value::Boolean(Cur.Clean),
                Value::Number(std::ceil(Cur.LineSeconds)),
                Value::Number(std::floor(Cur.LineScore)), Value::String(Thousands(std::floor(Cur.LineScore))) }));
        });
        Tricks->props["GetLineTimeRemaining"] = Movie.NewNative([this](Apt::Player&, const Value&, Args&) { return Value::Number(Cur.LineSeconds); });
        Tricks->props["GetLineScore"] = Movie.NewNative([this](Apt::Player&, const Value&, Args&) { return Value::Number(std::floor(Cur.LineScore)); });
        Tricks->props["GetCurrentTrickStance"] = Movie.NewNative([this](Apt::Player& P, const Value&, Args&)
        {
            return Value::Object(P.NewArray({ Value::Boolean(Cur.Switch), Value::Boolean(Cur.Fakie), Value::Boolean(false), Value::Boolean(false) }));
        });
        Tricks->props["GetCurrentTrickName"] = Movie.NewNative([this](Apt::Player&, const Value&, Args&) { return Value::String(Cur.Trick); });
        // [0] name, [3] true = the trick ended the display (bail)
        Tricks->props["GetCurrentTrickMetrics"] = Movie.NewNative([this](Apt::Player& P, const Value&, Args&)
        {
            return Value::Object(P.NewArray({ Value::String(Cur.Trick), Value::Number(0), Value::Number(0), Value::Boolean(false) }));
        });
        // Callouts (gaps, distances): count, then (active, title) pairs. The engine has no gaps yet.
        Tricks->props["TrickDisplay_GetAllTrickData"] = Movie.NewNative([](Apt::Player& P, const Value&, Args&)
        {
            return Value::Object(P.NewArray({ Value::Number(0) }));
        });
        G->props["Tricks"] = Obj(Tricks);

        Apt::ObjRef Components = Movie.NewObject();
        Components->props["TrickDisplay_GetSequenceMultiplier"] = Movie.NewNative([this](Apt::Player&, const Value&, Args&)
        {
            return Value::Number(Cur.Multiplier);
        });
        G->props["HUDComponents"] = Obj(Components);

        Apt::ObjRef Language = Movie.NewObject();
        Language->props["GetLanguage"] = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value::String("english"); });
        G->props["FELanguage"] = Obj(Language);

        Apt::ObjRef Screens = Movie.NewObject();
        Screens->props["OnLoaded"] = Movie.NewNative([this](Apt::Player&, const Value&, Args& A)
        {
            if (A.size() >= 2) Screen = A[1];
            return Value();
        });
        G->props["ScreenManager"] = Obj(Screens);
        G->props["Screen_EdgeOffset"] = Num(0);

        bLoaded = true;
        return true;
    }

    void Driver::SetState(int State)
    {
        if (Screen.t == Apt::Value::Obj) Movie.CallMethod(Screen, "SetState", { Apt::Value::Number(State) });
    }

    void Driver::CallTrickDisplay(const char* Method)
    {
        Apt::Value Td = Movie.Get(Apt::Value::Object(Movie.Global()), "TrickDisplay");
        if (Td.t == Apt::Value::Obj) Movie.CallMethod(Td, Method, {});
    }

    void Driver::Start()
    {
        bStarted = true;
        // Re-enable the display (Stop's ScreenShow(false) cleared mForceShow) but keep it hidden:
        // like in Skate 3 the HUD appears with the first trick (RefreshTrickName plays "intro").
        if (bLoaded && !bFirst)
            Movie.Set(Movie.Get(Apt::Value::Object(Movie.Global()), "trickdisplay2"), "mForceShow", Apt::Value::Boolean(true));
    }

    bool Driver::DisplayOpen()
    {
        return Movie.Get(Movie.Get(Apt::Value::Object(Movie.Global()), "trickdisplay2"), "mTrickDisplayOpen").ToBool();
    }

    void Driver::Stop()
    {
        bStarted = false;
        if (!bLoaded) return;
        Cur = Input{};
        SinceSequence = 1e9;
        SetState(7);                          // scores gone, trick names play their outros
        CallTrickDisplay("CloseTrickDisplay"); // so the next trick plays the intro again
        SetState(19);                         // ScreenShow(false)
    }

    void Driver::Event(int Kind, const std::string& Text)
    {
        if (!bLoaded || Screen.t != Apt::Value::Obj) return;
        switch (Kind)
        {
        case EventTrick:
            Cur.Trick = Text;
            SetState(8);                        // stance (SWITCH / FAKIE beside the badge)
            CallTrickDisplay("RefreshTrickName");
            break;
        case EventModified:
            Cur.Trick = Text;
            CallTrickDisplay("RefreshTrickNameOnly");
            break;
        default:
            break;
        }
    }

    void Driver::Update(const Input& InRaw, double Seconds)
    {
        if (!bLoaded) return;
        if (bFirst)
        {
            Movie.Advance(0);  // builds the movie: the class registers its screen object
            bFirst = false;
        }
        Input In = InRaw;
        In.Trick = Cur.Trick;  // names arrive through Event()
        // Skate 3 publishes a landed sequence once (final points, CLEAN / SKETCHY) and then
        // clears it: the movie's own "hide" animation keeps the number up ~1.5 s and fades it,
        // and the trick names play their outros; the badge and LINE stay while the line runs.
        if (In.SequenceActive) SinceSequence = 0;
        else SinceSequence += Seconds;
        if (!In.SequenceActive && SinceSequence > Seconds * 1.5) In.Sequence = 0;
        Last = Cur;
        Cur = In;

        // Once the line is over, close the display so the next trick plays the intro again.
        if (!In.SequenceActive && In.Sequence <= 0 && In.LineSeconds <= 0) Idle += Seconds;
        else Idle = 0;
        if (Idle > 0.6 && DisplayOpen()) CallTrickDisplay("CloseTrickDisplay");

        if (Screen.t == Apt::Value::Obj && bStarted)
        {
            const bool Score = std::floor(Cur.Sequence) != std::floor(Last.Sequence) || Cur.Clean != Last.Clean ||
                               Cur.Sketchy != Last.Sketchy || std::floor(Cur.LineScore) != std::floor(Last.LineScore);
            if (Score) SetState(7);
            if (Cur.Multiplier != Last.Multiplier) SetState(10);
            if (Cur.Switch != Last.Switch || Cur.Fakie != Last.Fakie) SetState(8);
            // The ring is the line timer: restart it at the right frame whenever time is added
            // (it then plays down in real time on its own).
            if (Cur.LineSeconds > Last.LineSeconds + 0.05f) SetState(11);
        }
        Movie.Advance(Seconds);
    }
}
