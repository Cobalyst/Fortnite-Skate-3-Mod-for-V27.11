#include "PhoneHud.h"

#include <cstdio>
#include <fstream>

namespace PhoneHud
{
    void Driver::SetLog(std::function<void(const std::string&)> Log)
    {
        LogFn = Log;
        Movie.LogLine = [this](const std::string& S) { if (LogFn) LogFn("[phonelist apt] " + S); };
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
        Movie.PlaceImports = true;  // the button items (mButton0..5) are imported from button_item2
        IconDir = DataRoot + "\\hud\\buttons\\";
        const std::string Font = DataRoot + "\\font\\";
        const std::unordered_map<std::string, std::string> Fonts = {
            { "Futura Shadow", Font + "futurashadow.txt" },
        };
        if (!Movie.Load(DataRoot + "\\extracted\\hudphonelist", "hudphonelist", DataRoot + "\\hud\\hudphonelist", Fonts))
        {
            Error = Movie.Error();
            return false;
        }

        using Apt::Value;
        using Apt::Args;
        Apt::ObjRef G = Movie.Global();

        auto Flag = [this](bool Input::* Field)
        {
            return Movie.NewNative([this, Field](Apt::Player&, const Value&, Args&) { return Value::Boolean(Cur.*Field); });
        };
        auto False = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value::Boolean(false); });

        Apt::ObjRef Game = Movie.NewObject();
        Game->props["CanUseSessionMarker"] = Flag(&Input::CanGoTo);
        Game->props["CanPlaceSessionMarker"] = Flag(&Input::CanPlace);
        G->props["Game"] = Value::Object(Game);

        // Not in the mod (yet): photographer, reset objects, music track skipping.
        Apt::ObjRef Components = Movie.NewObject();
        Components->props["GetPhoneListCanShowObjectDropperOption"] = Flag(&Input::CanDrop);
        Components->props["GetPhoneListCanShowPhotographerOption"] = False;
        Components->props["GetPhoneListCanResetAllObjectsOption"] = False;
        Components->props["GetPhoneListCanShowMusicOption"] = False;
        G->props["HUDComponents"] = Value::Object(Components);

        Apt::ObjRef Park = Movie.NewObject();
        Park->props["AreWeInSkatePark"] = False;
        G->props["SkatePark"] = Value::Object(Park);

        Apt::ObjRef Screens = Movie.NewObject();
        Screens->props["OnLoaded"] = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value(); });
        G->props["ScreenManager"] = Value::Object(Screens);
        G->props["Screen_EdgeOffset"] = Value::Number(0);
        Movie.ObjectPrototype()->props["RegisterScreen"] = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value(); });
        // button_item2.SetButtonType(button): remembers which button glyph the item shows.
        Movie.ObjectPrototype()->props["SetButtonType"] = Movie.NewNative([this](Apt::Player&, const Value& Self, Args& A)
        {
            if (Self.t != Value::Obj || A.empty()) return Value();
            const std::string Name = A[0].ToStr();
            if (!IconIndex.count(Name))
            {
                const std::string Path = IconDir + Name + ".png";
                if (FILE* F = nullptr; fopen_s(&F, Path.c_str(), "rb") == 0 && F) { fclose(F); IconIndex[Name] = int(IconPaths.size()); IconPaths.push_back(Path); }
                else { IconIndex[Name] = -1; if (LogFn) LogFn("[phonelist] no icon for " + Name); }
            }
            Self.o->props["__button"] = Value::String(Name);
            return Value();
        });

        bLoaded = true;
        return true;
    }

    void Driver::Render(std::vector<Apt::DrawCmd>& Out)
    {
        Movie.Render(Out);
        const Apt::Value Root = Movie.RootObject();
        if (Root.t != Apt::Value::Obj) return;
        if (auto I = Root.o->inst.lock()) AddIcons(*I, Apt::Mat{}, Apt::Cx{}, Out);
    }

    // A button item's glyph: 32x32 centred on the item (button_item2's 'Button Item' bounds
    // -16.5..15.5), with the item's transform and colour (the movie dims unavailable options).
    void Driver::AddIcons(const Apt::Instance& I, const Apt::Mat& Parent, const Apt::Cx& ParentCx, std::vector<Apt::DrawCmd>& Out)
    {
        if (!I.visible) return;
        const Apt::Mat M = Parent * I.m;
        const Apt::Cx C = ParentCx * I.cx;
        if (I.obj)
        {
            auto B = I.obj->props.find("__button");
            if (B != I.obj->props.end())
            {
                auto Ix = IconIndex.find(B->second.ToStr());
                if (Ix != IconIndex.end() && Ix->second >= 0 && C.mul[3] + C.add[3] > 0.004f)
                {
                    Apt::DrawCmd D;
                    D.Texture = int(Movie.Textures().size()) + Ix->second;
                    for (int k = 0; k < 4; k++) { D.Mul[k] = C.mul[k]; D.Add[k] = C.add[k]; }
                    const float X0 = -16.5f, Y0 = -16.5f, X1 = 15.5f, Y1 = 15.5f;
                    Apt::Vertex V[4];
                    const float Px[4] = { X0, X1, X1, X0 }, Py[4] = { Y0, Y0, Y1, Y1 }, U[4] = { 0, 1, 1, 0 }, W[4] = { 0, 0, 1, 1 };
                    for (int k = 0; k < 4; k++) { M.Apply(Px[k], Py[k], V[k].x, V[k].y); V[k].u = U[k]; V[k].v = W[k]; }
                    D.Verts = { V[0], V[1], V[2], V[0], V[2], V[3] };
                    Out.push_back(std::move(D));
                }
            }
        }
        for (auto& Child : I.children) AddIcons(*Child.second, M, C, Out);
    }

    void Driver::SetState(int State)
    {
        if (Screen.t == Apt::Value::Obj) Movie.CallMethod(Screen, "SetState", { Apt::Value::Number(State) });
    }

    void Driver::Hide()
    {
        if (bLoaded && Cur.Open) SetState(0);
        Cur = {};
    }

    void Driver::Update(const Input& In, double Seconds)
    {
        if (!bLoaded) return;
        if (bFirst)
        {
            Movie.Advance(0);  // builds the movie; frame 0 creates "screen = new hudphonelist(this)"
            Screen = Movie.Get(Movie.RootObject(), "screen");
            if (Screen.t != Apt::Value::Obj && LogFn) LogFn("[phonelist] hudphonelist screen object not found");
            Movie.Global()->props["screen"] = Screen;
            bFirst = false;
        }
        Last = Cur;
        Cur = In;
        if (Cur.Open && !Last.Open) SetState(1);        // maximized: opens and fills the menu
        else if (!Cur.Open && Last.Open) SetState(0);   // minimized -> hidden
        else if (Cur.Open && (Cur.CanGoTo != Last.CanGoTo || Cur.CanPlace != Last.CanPlace || Cur.CanDrop != Last.CanDrop)) SetState(50);  // refresh
        Movie.Advance(Seconds);
    }
}
