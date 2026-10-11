#include "HomHud.h"
#include "SkateHud.h"

#include <cmath>
#include <cstdio>
#include <fstream>

namespace HomHud
{
    namespace
    {
        // How long the HUD stays up after the bail ends (Skate 3 keeps it until the respawn).
        enum : int { SoundTransition = 1, SoundScoreRed = 2 };

        // "{0:speed}" (m/s -> MPH), "{0:distance}" (m -> ft), "{0:d}", "{0:f.N}" and plain "{0}".
        std::string FormatCustom(const std::string& Pattern, double V)
        {
            std::string Out;
            for (size_t i = 0; i < Pattern.size();)
            {
                if (Pattern[i] == '{')
                {
                    const size_t End = Pattern.find('}', i);
                    if (End != std::string::npos)
                    {
                        const std::string Spec = Pattern.substr(i + 1, End - i - 1);
                        char Buf[64];
                        if (Spec.find("speed") != std::string::npos) snprintf(Buf, sizeof(Buf), "%.1f MPH", V * 2.236936);
                        else if (Spec.find("distance") != std::string::npos) snprintf(Buf, sizeof(Buf), "%.1f ft", V * 3.280840);
                        else if (Spec.find(":f.") != std::string::npos) snprintf(Buf, sizeof(Buf), "%.*f", atoi(Spec.c_str() + Spec.find(":f.") + 3), V);
                        else snprintf(Buf, sizeof(Buf), "%d", int(std::floor(V)));
                        Out += Buf;
                        i = End + 1;
                        continue;
                    }
                }
                Out += Pattern[i++];
            }
            return Out;
        }
    }

    void Driver::SetLog(std::function<void(const std::string&)> Log)
    {
        LogFn = Log;
        Movie.LogLine = [this](const std::string& S) { if (LogFn) LogFn("[hom apt] " + S); };
    }

    bool Driver::Load(const std::string& DataRoot, std::string& Error)
    {
        std::ifstream In(DataRoot + "\\strings_en.txt", std::ios::binary);
        std::string Line;
        while (std::getline(In, Line))
        {
            if (!Line.empty() && Line.back() == '\r') Line.pop_back();
            const size_t Start = Line.find_first_not_of(' ');
            const size_t Tab = Line.find('\t');
            if (Start == std::string::npos || Tab == std::string::npos || Tab < Start) continue;
            Strings[Line.substr(Start, Tab - Start)] = Line.substr(Tab + 1);
        }
        Movie.Localize = [this](const std::string& Key)
        {
            auto It = Strings.find(Key);
            return It == Strings.end() ? std::string() : It->second;
        };
        const std::string Font = DataRoot + "\\font\\";
        const std::unordered_map<std::string, std::string> Fonts = {
            { "Futura Shadow", Font + "futurashadow.txt" },
            { "FuturaOuterGlow", Font + "futuraglow.txt" },
            { "Futura Std Medium", Font + "futuraheavy.txt" },
        };
        if (!Movie.Load(DataRoot + "\\extracted\\homscoring", "homscoring", DataRoot + "\\hud\\homscoring", Fonts))
        {
            Error = Movie.Error();
            return false;
        }

        using Apt::Value;
        using Apt::Args;
        Apt::ObjRef G = Movie.Global();
        auto Numbers = [](Apt::Player& P, const float* V, int N) {
            Args A;
            for (int i = 0; i < N; i++) A.push_back(Value::Number(V[i]));
            return A;
        };

        Apt::ObjRef Components = Movie.NewObject();
        Components->props["HOM_GetScore"] = Movie.NewNative([this](Apt::Player&, const Value&, Args&)
        {
            return Value::Number(std::floor(Cur.Score));
        });
        // [0..4] metric values (speed, drop, air, bail time, rotation), [5..9] "show anyway" flags
        Components->props["HOM_GetBonusData"] = Movie.NewNative([this, Numbers](Apt::Player& P, const Value&, Args&)
        {
            Args A = Numbers(P, Cur.Metrics, 5);
            for (int i = 0; i < 5; i++) A.push_back(Value::Boolean(false));
            return Value::Object(P.NewArray(A));
        });
        Components->props["HOM_GetBonusScores"] = Movie.NewNative([this](Apt::Player& P, const Value&, Args&)
        {
            Args A;
            for (float V : Cur.Points) A.push_back(Value::Number(std::floor(V)));
            return Value::Object(P.NewArray(A));
        });
        // Collision bonuses (cars, objects, body tweak, pedestrians, skaters): none in Fortnite.
        auto Zeros = Movie.NewNative([](Apt::Player& P, const Value&, Args&)
        {
            return Value::Object(P.NewArray({ Value::Number(0), Value::Number(0), Value::Number(0), Value::Number(0), Value::Number(0) }));
        });
        Components->props["HOM_GetBonusCollisionData"] = Zeros;
        Components->props["HOM_GetBonusCollisionScore"] = Zeros;
        G->props["HUDComponents"] = Value::Object(Components);

        Apt::ObjRef Language = Movie.NewObject();
        Language->props["GetLanguage"] = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value::String("english"); });
        Language->props["GetLocalizedFormatNumberString"] = Movie.NewNative([](Apt::Player&, const Value&, Args& A)
        {
            return Value::String(SkateHud::Thousands(A.empty() ? 0.0 : std::floor(A[0].ToNum())));
        });
        // (key, value) in either order: the ID_ string is the format.
        Language->props["GetLocalizedFormatStringCustom"] = Movie.NewNative([this](Apt::Player&, const Value&, Args& A)
        {
            std::string Key, Num;
            for (auto& V : A)
            {
                const std::string S = V.ToStr();
                if (S.rfind("ID_", 0) == 0) Key = S; else Num = S;
            }
            auto It = Strings.find(Key);
            return Value::String(FormatCustom(It == Strings.end() ? "{0}" : It->second, atof(Num.c_str())));
        });
        Language->props["GetLocaleText"] = Movie.NewNative([this](Apt::Player&, const Value&, Args& A)
        {
            auto It = A.empty() ? Strings.end() : Strings.find(A[0].ToStr());
            return Value::String(It == Strings.end() ? std::string() : It->second);
        });
        Language->props["GetLocalizedTime"] = Movie.NewNative([](Apt::Player&, const Value&, Args& A)
        {
            char Buf[32];
            snprintf(Buf, sizeof(Buf), "%05.2f", A.empty() ? 0.0 : A[0].ToNum());
            return Value::String(Buf);
        });
        G->props["FELanguage"] = Value::Object(Language);

        Apt::ObjRef Audio = Movie.NewObject();
        Audio->props["PlaySound"] = Movie.NewNative([this](Apt::Player&, const Value&, Args& A)
        {
            const int Id = A.empty() ? 0 : int(A[0].ToNum());
            if (PlaySound) PlaySound(Id == SoundScoreRed ? "score_red" : "transition");
            return Value();
        });
        G->props["Audio"] = Value::Object(Audio);
        G->props["FE_SOUND_HOM_TRANSITION"] = Value::Number(SoundTransition);
        G->props["FE_SOUND_HOM_SCORE_RED"] = Value::Number(SoundScoreRed);

        Apt::ObjRef Screens = Movie.NewObject();
        Screens->props["OnLoaded"] = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value(); });
        G->props["ScreenManager"] = Value::Object(Screens);
        G->props["Screen_EdgeOffset"] = Value::Number(0);
        // The class registers itself with the front end through this.RegisterScreen().
        Movie.ObjectPrototype()->props["RegisterScreen"] = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value(); });

        bLoaded = true;
        return true;
    }

    void Driver::SetState(int State)
    {
        if (Screen.t == Apt::Value::Obj) Movie.CallMethod(Screen, "SetState", { Apt::Value::Number(State) });
    }

    void Driver::Hide()
    {
        if (bLoaded && bVisible) SetState(2);
        bVisible = false;
    }

    void Driver::Update(const Input& In, double Seconds)
    {
        if (!bLoaded) return;
        if (bFirst)
        {
            Movie.Advance(0);  // builds the movie; frame 0 creates "screen = new homscoring(this)"
            Screen = Movie.Get(Movie.RootObject(), "screen");
            if (Screen.t != Apt::Value::Obj && LogFn) LogFn("[hom] homscoring screen object not found");
            // Sprites call screen.AnimStarted() / AnimFinished() through "screen" from their own
            // scope; Skate 3 resolves it to the root's local, this player through _global.
            Movie.Global()->props["screen"] = Screen;
            bFirst = false;
        }
        Last = Cur;
        Cur = In;
        // A new bail: show the HUD (its intro and transition sound) and start scoring.
        if (In.Active && !Last.Active)
        {
            if (bVisible) SetState(2);
            SetState(1);
            if (Screen.t == Apt::Value::Obj) Movie.CallMethod(Screen, "PlayIntroAudio", {});  // Skate 3 plays it with the show
            bVisible = true;
            SinceEnd = 0;
        }
        if (bVisible)
        {
            bool bChanged = std::floor(Cur.Score) != std::floor(Last.Score);
            for (int i = 0; i < 5; i++) bChanged |= std::floor(Cur.Points[i]) != std::floor(Last.Points[i]);
            if (bChanged || (Cur.Active && !Last.Active)) SetState(3);
            // Skate 3 respawns the skater after a Hall of Meat bail and the HUD goes with it; here
            // the skater gets up instead, so the HUD goes as the bail ends.
            if (!In.Active) Hide();
        }
        Movie.Advance(Seconds);
    }
}
