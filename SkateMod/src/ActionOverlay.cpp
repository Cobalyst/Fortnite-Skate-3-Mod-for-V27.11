#include "ActionOverlay.h"

#include <cstdio>
#include <fstream>

namespace ActionOverlay
{
    // actionoverlay2's state constants (its SetState compares against them). The native HUD state
    // 82648608 maps its modes to 0 (none), 1 (challenge), 2 (teleport), 4 (DMO grab), 5 (single
    // OTS challenge) and 6; PressStart is the remaining 3.
    static const struct { const char* Name; int Value; } States[] = {
        { "kActionOverlayState_Off", 0 }, { "kActionOverlayState_Challenge", 1 }, { "kActionOverlayState_Teleport", 2 },
        { "kActionOverlayState_PressStart", 3 }, { "kActionOverlayState_DMOGrab", 4 }, { "kActionOverlayState_OTS", 5 },
        { "kActionOverlayState_Disabled", 6 },
    };
    static const int DmoGrab = 4;

    void Driver::SetLog(std::function<void(const std::string&)> Log)
    {
        LogFn = Log;
        Movie.LogLine = [this](const std::string& S) { if (LogFn) LogFn("[actionoverlay apt] " + S); };
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
        Movie.PlaceImports = true;   // mButton / mButtonItem are button_item2 instances
        IconDir = DataRoot + "\\hud\\buttons\\";
        const std::unordered_map<std::string, std::string> Fonts = {
            { "Futura Shadow", DataRoot + "\\font\\futurashadow.txt" },
        };
        if (!Movie.Load(DataRoot + "\\extracted\\actionoverlay2", "actionoverlay2", DataRoot + "\\hud\\actionoverlay2", Fonts))
        {
            Error = Movie.Error();
            return false;
        }

        using Apt::Value;
        using Apt::Args;
        Apt::ObjRef G = Movie.Global();
        Apt::ObjRef Components = Movie.NewObject();
        Components->props["DMO_GetParams"] = Movie.NewNative([this](Apt::Player& P, const Value&, Args&)
        {
            Args Items;
            for (bool B : Cur.Params) Items.push_back(Value::Boolean(B));
            return Value::Object(P.NewArray(Items));
        });
        // The other states are not driven by the mod (challenges, teleports, OTS spots).
        Components->props["SignUp_GetChallengeTitle"] = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value::String(""); });
        G->props["HUDComponents"] = Value::Object(Components);
        Apt::ObjRef Screens = Movie.NewObject();
        Screens->props["OnLoaded"] = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value(); });
        G->props["ScreenManager"] = Value::Object(Screens);
        G->props["Screen_EdgeOffset"] = Value::Number(0);
        Movie.ObjectPrototype()->props["RegisterScreen"] = Movie.NewNative([](Apt::Player&, const Value&, Args&) { return Value(); });
        // button_item2.SetButtonType(button): the HUD glyph (useHUD appends "_hud").
        Movie.ObjectPrototype()->props["SetButtonType"] = Movie.NewNative([this](Apt::Player&, const Value& Self, Args& A)
        {
            if (Self.t == Value::Obj && !A.empty()) SetButton(Self, A[0].ToStr());
            return Value();
        });
        bLoaded = true;
        return true;
    }

    void Driver::SetButton(const Apt::Value& Item, const std::string& Button)
    {
        if (Item.t != Apt::Value::Obj) return;
        const std::string Name = (Button.size() > 4 && Button.compare(Button.size() - 4, 4, "_hud") == 0) ? Button : Button + "_hud";
        if (!IconIndex.count(Name))
        {
            const std::string Path = IconDir + Name + ".png";
            if (FILE* F = nullptr; fopen_s(&F, Path.c_str(), "rb") == 0 && F) { fclose(F); IconIndex[Name] = int(IconPaths.size()); IconPaths.push_back(Path); }
            else { IconIndex[Name] = -1; if (LogFn) LogFn("[actionoverlay] no icon for " + Name); }
        }
        Item.o->props["mButtonImage"] = Apt::Value::String(Button);
        Item.o->props["__button"] = Apt::Value::String(Name);
    }

    void Driver::Render(std::vector<Apt::DrawCmd>& Out)
    {
        Movie.Render(Out);
        const Apt::Value Root = Movie.RootObject();
        if (Root.t != Apt::Value::Obj) return;
        if (auto I = Root.o->inst.lock()) AddIcons(*I, Apt::Mat{}, Apt::Cx{}, Out);
    }

    // button_item2's glyph: 32x32 centred on the item, with its transform and colour.
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
        if (bLoaded && Cur.Show) SetState(0);
        Cur = {};
    }

    void Driver::Update(const Input& In, double Seconds)
    {
        if (!bLoaded) return;
        if (bFirst)
        {
            Movie.Advance(0);   // frame 0: overlay = new actionoverlay2(this)
            Screen = Movie.Get(Movie.RootObject(), "overlay");
            if (Screen.t != Apt::Value::Obj && LogFn) LogFn("[actionoverlay] actionoverlay2 screen object not found");
            Apt::Value Class = Apt::Value::Object(Movie.Global());
            Class = Movie.Get(Class, "actionoverlay2");
            for (const auto& S : States)
            {
                if (Screen.t == Apt::Value::Obj) Screen.o->props[S.Name] = Apt::Value::Number(S.Value);
                if (Class.t == Apt::Value::Obj) Class.o->props[S.Name] = Apt::Value::Number(S.Value);
            }
            // DMO0 / DMO1's mButton clip actions name their glyphs (DPad_Up, DPad_Down); the
            // button_item2 constructor passes that to SetButtonType.
            const Apt::Value Dmo = Movie.Get(Movie.Get(Class, "mScreen"), "mDmo");   // actionoverlay2.mScreen (static)
            const char* Rows[3][2] = { { "DMO0", "DPad_Up" }, { "DMO1", "DPad_Down" }, { "DMO2", "DPad_Right" } };
            for (auto& R : Rows)
            {
                const Apt::Value Button = Movie.Get(Movie.Get(Dmo, R[0]), "mButton");
                const Apt::Value Image = Movie.Get(Button, "mButtonImage");
                SetButton(Button, Image.t == Apt::Value::Str ? Image.ToStr() : std::string(R[1]));
            }
            SetState(0);
            bFirst = false;
        }
        Last = Cur;
        Cur = In;
        bool bParamsChanged = false;
        for (int i = 0; i < 5; i++) bParamsChanged |= Cur.Params[i] != Last.Params[i];
        if (Cur.Show && (!Last.Show || bParamsChanged)) SetState(DmoGrab);   // shows / refreshes the rows
        else if (!Cur.Show && Last.Show) SetState(0);
        Movie.Advance(Seconds);
    }
}
