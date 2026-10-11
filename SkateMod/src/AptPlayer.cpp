#include "AptPlayer.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

namespace Apt
{
    // ---------------------------------------------------------------- basics

    Mat Mat::operator*(const Mat& r) const
    {
        Mat o;
        o.a = a * r.a + c * r.b;
        o.b = b * r.a + d * r.b;
        o.c = a * r.c + c * r.d;
        o.d = b * r.c + d * r.d;
        o.tx = a * r.tx + c * r.ty + tx;
        o.ty = b * r.tx + d * r.ty + ty;
        return o;
    }

    Cx Cx::operator*(const Cx& child) const
    {
        // child applied first: (in*cm + ca)*pm + pa
        Cx o;
        for (int i = 0; i < 4; i++)
        {
            o.mul[i] = child.mul[i] * mul[i];
            o.add[i] = child.add[i] * mul[i] + add[i];
        }
        return o;
    }

    bool Value::ToBool() const
    {
        switch (t)
        {
        case Bool: return b;
        case Num: return n != 0 && !std::isnan(n);
        case Str: return !s.empty();
        case Obj: return true;
        default: return false;
        }
    }

    double Value::ToNum() const
    {
        switch (t)
        {
        case Bool: return b ? 1 : 0;
        case Num: return n;
        case Str:
        {
            if (s.empty()) return NAN;
            char* End = nullptr;
            const double v = strtod(s.c_str(), &End);
            return End && *End == 0 ? v : NAN;
        }
        case Null: return 0;
        default: return NAN;
        }
    }

    std::string Value::ToStr() const
    {
        switch (t)
        {
        case Bool: return b ? "true" : "false";
        case Num:
        {
            if (std::isnan(n)) return "NaN";
            char Buf[64];
            if (n == std::floor(n) && std::fabs(n) < 1e15)
                snprintf(Buf, sizeof(Buf), "%.0f", n);
            else
                snprintf(Buf, sizeof(Buf), "%.15g", n);
            return Buf;
        }
        case Str: return s;
        case Null: return "null";
        case Obj: return "[object Object]";
        default: return "undefined";
        }
    }

    static bool PngSize(const std::string& Path, int& W, int& H)
    {
        std::ifstream In(Path, std::ios::binary);
        unsigned char B[24];
        if (!In.read(reinterpret_cast<char*>(B), 24)) return false;
        W = (B[16] << 24) | (B[17] << 16) | (B[18] << 8) | B[19];
        H = (B[20] << 24) | (B[21] << 16) | (B[22] << 8) | B[23];
        return W > 0 && H > 0;
    }

    static std::vector<uint8_t> ReadFile(const std::string& Path)
    {
        std::ifstream In(Path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    }

    // ---------------------------------------------------------------- bitmap font

    bool BitmapFont::Load(const std::string& TxtPath)
    {
        std::ifstream In(TxtPath, std::ios::binary);
        if (!In) return false;
        std::stringstream Buffer;
        Buffer << In.rdbuf();
        const std::string Text = Buffer.str();

        auto Number = [&](const char* Key, float Default)
        {
            const size_t At = Text.find(Key);
            return At == std::string::npos ? Default : float(atof(Text.c_str() + At + strlen(Key)));
        };
        EmPixels = Number("\nSize: ", 28) * 96.f / 72.f;
        Ascent = Number("\nAscent: ", 37);

        // A section runs until the next line that starts a new "Key:".
        auto Section = [&](const char* Key) -> std::string
        {
            const size_t At = Text.find(Key);
            if (At == std::string::npos) return {};
            const size_t Start = At + strlen(Key);
            size_t End = Start;
            while ((End = Text.find('\n', End)) != std::string::npos)
            {
                if (End + 1 < Text.size() && isalpha((unsigned char)Text[End + 1]))
                    break;
                End++;
            }
            std::string Out = Text.substr(Start, End == std::string::npos ? std::string::npos : End - Start);
            for (char& C : Out) if (C == '\r' || C == '\n' || C == ',') C = ' ';
            return Out;
        };

        std::vector<Glyph> ByIndex;
        std::istringstream M(Section("GlyphMetricsMap: "));
        int Count = 0;
        M >> Count;
        ByIndex.assign(size_t(Count > 0 ? Count : 0), Glyph{});
        for (int i = 0; i < Count; i++)
        {
            int Index, Page;
            Glyph G;
            M >> Index >> Page >> G.x >> G.y >> G.w >> G.h >> G.bx >> G.by >> G.adv;
            if (Index >= 0 && Index < Count) ByIndex[size_t(Index)] = G;
        }
        std::istringstream C(Section("CharMapSet: "));
        int Pairs = 0;
        C >> Pairs;
        for (int i = 0; i < Pairs; i++)
        {
            uint32_t Char;
            int Index;
            C >> Char >> Index;
            if (Index >= 0 && Index < Count) Glyphs[Char] = ByIndex[size_t(Index)];
        }
        return !Glyphs.empty();
    }

    float BitmapFont::Width(const std::string& S, float Scale) const
    {
        float W = 0;
        for (unsigned char Ch : S)
        {
            auto It = Glyphs.find(Ch);
            if (It != Glyphs.end()) W += It->second.adv * Scale;
        }
        return W;
    }

    std::shared_ptr<Instance> Instance::Child(const std::string& Name) const
    {
        for (auto& [Depth, C] : children)
            if (C->name == Name) return C;
        return nullptr;
    }

    // ---------------------------------------------------------------- loading

    uint32_t Player::U32(uint32_t o) const
    {
        const std::vector<uint8_t>& D = M->Data;
        if (o + 4 > D.size()) return 0;
        return (uint32_t(D[o]) << 24) | (uint32_t(D[o + 1]) << 16) | (uint32_t(D[o + 2]) << 8) | D[o + 3];
    }

    float Player::F32(uint32_t o) const
    {
        const uint32_t v = U32(o);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }

    std::string Player::CStr(uint32_t o) const
    {
        const std::vector<uint8_t>& D = M->Data;
        if (!o || o >= D.size()) return {};
        const char* S = reinterpret_cast<const char*>(D.data() + o);
        return std::string(S, strnlen(S, D.size() - o));
    }

    Value Player::ConstValue(uint32_t Index) const
    {
        return Index < M->Consts.size() ? M->Consts[Index] : Value();
    }

    bool Player::LoadConst(const std::string& Path)
    {
        const std::vector<uint8_t> D = ReadFile(Path);
        if (D.size() < 0x20) return false;
        auto U = [&](size_t o) -> uint32_t
        {
            return o + 4 > D.size() ? 0 : (uint32_t(D[o]) << 24) | (uint32_t(D[o + 1]) << 16) | (uint32_t(D[o + 2]) << 8) | D[o + 3];
        };
        M->AptOffset = U(0x14);
        const uint32_t Count = U(0x18);
        for (uint32_t i = 0; i < Count; i++)
        {
            const uint32_t T = U(0x20 + i * 8), V = U(0x24 + i * 8);
            Value Out;
            switch (T)
            {
            case 1:
                if (V < D.size()) Out = Value::String(std::string(reinterpret_cast<const char*>(D.data() + V), strnlen(reinterpret_cast<const char*>(D.data() + V), D.size() - V)));
                break;
            case 5: Out = Value::Boolean(V != 0); break;
            case 6: { float F; memcpy(&F, &V, 4); Out = Value::Number(F); break; }
            case 7: Out = Value::Number(double(int32_t(V))); break;
            default: break;
            }
            M->Consts.push_back(std::move(Out));
        }
        return true;
    }

    std::vector<Frame> Player::ReadFrames(uint32_t Count, uint32_t Ptr)
    {
        std::vector<Frame> Frames(Count);
        for (uint32_t f = 0; f < Count; f++)
        {
            const uint32_t N = U32(Ptr + f * 8), Items = U32(Ptr + f * 8 + 4);
            for (uint32_t k = 0; k < N; k++)
            {
                const uint32_t o = U32(Items + k * 4);
                FrameItem It{};
                const uint32_t T = U32(o);
                switch (T)
                {
                case 1: It.kind = FrameItem::Action; It.code = U32(o + 4); break;
                case 2: It.kind = FrameItem::Label; It.name = CStr(U32(o + 4)); break;
                case 3:
                {
                    It.kind = FrameItem::Place;
                    It.flags = U32(o + 4);
                    It.depth = int32_t(U32(o + 8));
                    It.character = int32_t(U32(o + 12));
                    It.matrix.a = F32(o + 16);
                    It.matrix.b = F32(o + 20);
                    It.matrix.c = F32(o + 24);
                    It.matrix.d = F32(o + 28);
                    It.matrix.tx = F32(o + 32);
                    It.matrix.ty = F32(o + 36);
                    const uint32_t Tint = U32(o + 40), Add = U32(o + 44);
                    It.cx.mul[0] = ((Tint >> 16) & 255) / 255.f;
                    It.cx.mul[1] = ((Tint >> 8) & 255) / 255.f;
                    It.cx.mul[2] = (Tint & 255) / 255.f;
                    It.cx.mul[3] = (Tint >> 24) / 255.f;
                    It.cx.add[0] = ((Add >> 16) & 255) / 255.f;
                    It.cx.add[1] = ((Add >> 8) & 255) / 255.f;
                    It.cx.add[2] = (Add & 255) / 255.f;
                    It.cx.add[3] = (Add >> 24) / 255.f;
                    It.ratio = F32(o + 48);
                    It.name = CStr(U32(o + 52));
                    if ((It.flags & 0x80) && U32(o + 60))
                    {
                        const uint32_t Ca = U32(o + 60);
                        for (uint32_t k = 0, n = std::min<uint32_t>(U32(Ca), 64), e = U32(Ca + 4); k < n; k++)
                            It.clipEvents.push_back({ U32(e + 12 * k), U32(e + 12 * k + 8) });
                    }
                    break;
                }
                case 4: It.kind = FrameItem::Remove; It.depth = int32_t(U32(o + 4)); break;
                case 5: It.kind = FrameItem::Background; break;
                case 8: It.kind = FrameItem::InitAction; It.code = U32(o + 8); break;
                default: continue;
                }
                Frames[f].items.push_back(std::move(It));
            }
        }
        return Frames;
    }

    static void CollectLabels(Character& C)
    {
        for (size_t f = 0; f < C.frames.size(); f++)
            for (auto& It : C.frames[f].items)
                if (It.kind == FrameItem::Label) C.labels[It.name] = int(f);
    }

    bool Player::LoadApt(const std::string& Path)
    {
        M->Data = ReadFile(Path);
        const uint32_t Mo = M->AptOffset;
        if (U32(Mo + 4) != 0x09876543) { Err = "bad movie signature in " + Path; return false; }
        M->MovieChar.type = CharType::Movie;
        M->MovieChar.module = M;
        M->MovieChar.frames = ReadFrames(U32(Mo + 16), U32(Mo + 20));
        CollectLabels(M->MovieChar);
        if (M == Mods.front().get())
        {
            Width = int(U32(Mo + 36));
            Height = int(U32(Mo + 40));
            const uint32_t Ms = U32(Mo + 44);
            if (Ms > 0 && Ms < 1000) FrameSeconds = Ms / 1000.0;
        }

        // imports (header +48 count, +52 table of 16-byte entries: movie, name, character, 0)
        // and exports (+56 count, +60 table of 8-byte entries: name, character)
        for (uint32_t i = 0, n = U32(Mo + 48), t = U32(Mo + 52); i < n; i++)
            M->Imports.push_back({ int(U32(t + i * 16 + 8)), { CStr(U32(t + i * 16)), CStr(U32(t + i * 16 + 4)) } });
        for (uint32_t i = 0, n = U32(Mo + 56), t = U32(Mo + 60); i < n; i++)
            M->Exports[CStr(U32(t + i * 8))] = int(U32(t + i * 8 + 4));

        const uint32_t Count = U32(Mo + 28), Table = U32(Mo + 32);
        for (uint32_t i = 0; i < Count; i++)
        {
            const uint32_t o = U32(Table + i * 4);
            if (!o || U32(o + 4) != 0x09876543) continue;
            Character C;
            C.type = CharType(U32(o));
            C.module = M;
            const uint32_t b = o + 16;
            switch (C.type)
            {
            case CharType::Shape:
                for (int k = 0; k < 4; k++) C.bounds[k] = F32(b + 4 * k);
                C.texture = int(U32(b + 16));  // geometry id until the .geo is read
                break;
            case CharType::Text:
                for (int k = 0; k < 4; k++) C.bounds[k] = F32(b + 4 * k);
                C.font = int(U32(b + 16));
                C.align = int(U32(b + 20));
                C.color = U32(b + 24);
                C.height = F32(b + 28);
                C.multiline = U32(b + 36) != 0;
                C.wordwrap = U32(b + 40) != 0;
                C.text = CStr(U32(b + 44));
                break;
            case CharType::Font:
                C.fontName = CStr(U32(b));
                break;
            case CharType::Sprite:
                C.frames = ReadFrames(U32(b), U32(b + 4));
                CollectLabels(C);
                break;
            case CharType::Image:
                C.texture = int(U32(b));
                break;
            default:
                break;
            }
            M->Chars[int(i)] = std::move(C);
        }
        return true;
    }

    bool Player::LoadGeo(const std::string& Path)
    {
        const std::vector<uint8_t> D = ReadFile(Path);
        if (D.size() < 8 || memcmp(D.data(), "Sk8", 3) != 0) { Err = "bad geometry file " + Path; return false; }
        auto U = [&](size_t o) -> uint32_t
        {
            return o + 4 > D.size() ? 0 : (uint32_t(D[o]) << 24) | (uint32_t(D[o + 1]) << 16) | (uint32_t(D[o + 2]) << 8) | D[o + 3];
        };
        auto F = [&](size_t o) { const uint32_t v = U(o); float f; memcpy(&f, &v, 4); return f; };
        std::unordered_map<int, std::vector<GeoPart>> Shapes;
        const uint32_t Count = U(4);
        for (uint32_t i = 0; i < Count; i++)
        {
            const uint32_t o = U(8 + 4 * i);
            const int Id = int(U(o));
            const uint32_t Parts = U(o + 4);
            for (uint32_t k = 0; k < Parts; k++)
            {
                const uint32_t p = U(o + 8 + 4 * k);
                GeoPart G;
                // +0 is the fill (1 solid colour, 2 image), +48 the triangle count (they agree for
                // image fills, which is all most movies have)
                const uint32_t Tris = U(p + 48);
                for (int j = 0; j < 4; j++) G.color[j] = F(p + 4 + 4 * j);
                G.image = int(U(p + 20));
                for (int j = 0; j < 4; j++) G.uvm[j] = F(p + 24 + 4 * j);
                G.uvt[0] = F(p + 40);
                G.uvt[1] = F(p + 44);
                for (uint32_t v = 0; v < Tris * 3; v++)
                {
                    G.xy.push_back(F(p + 52 + 8 * v));
                    G.xy.push_back(F(p + 56 + 8 * v));
                }
                Shapes[Id].push_back(std::move(G));
            }
        }
        for (auto& [Id, C] : M->Chars)
            if (C.type == CharType::Shape)
            {
                auto It = Shapes.find(C.texture);
                if (It == Shapes.end()) It = Shapes.find(Id);
                if (It != Shapes.end()) C.parts = It->second;
                C.texture = -1;
            }
        return true;
    }

    // Textures and fonts of module M, into the player's lists.
    bool Player::LoadAssets()
    {
        for (auto& [Id, C] : M->Chars)
            if (C.type == CharType::Image)
            {
                TextureInfo T;
                T.Path = M->TextureDir + "\\" + std::to_string(C.texture) + ".png";
                if (!PngSize(T.Path, T.Width, T.Height)) { Err = "missing texture " + T.Path; return false; }
                C.texture = int(TextureList.size());
                TextureList.push_back(T);
            }
        std::unordered_map<int, int> FontOf;  // Font character id -> FontList index
        for (auto& [Id, C] : M->Chars)
        {
            if (C.type != CharType::Font) continue;
            auto Known = FontByName.find(C.fontName);
            if (Known != FontByName.end()) { FontOf[Id] = Known->second; continue; }
            auto It = FontPaths.find(C.fontName);
            if (It == FontPaths.end()) continue;
            BitmapFont F;
            if (!F.Load(It->second)) { Err = "cannot read font " + It->second; return false; }
            TextureInfo T;
            T.Path = It->second.substr(0, It->second.size() - 4) + ".png";
            if (!PngSize(T.Path, T.Width, T.Height)) { Err = "missing font texture " + T.Path; return false; }
            F.TexW = float(T.Width);
            F.TexH = float(T.Height);
            F.Texture = int(TextureList.size());
            TextureList.push_back(T);
            FontByName[C.fontName] = FontOf[Id] = int(FontList.size());
            FontList.push_back(std::move(F));
        }
        for (auto& [Id, C] : M->Chars)
        {
            if (C.type == CharType::Text)
            {
                auto F = FontOf.find(C.font);
                if (F != FontOf.end()) C.fontIndex = F->second;
            }
            for (auto& P : C.parts)
                if (P.image)
                {
                    auto Img = M->Chars.find(P.image);
                    if (Img != M->Chars.end() && Img->second.type == CharType::Image) P.tex = Img->second.texture;
                }
        }
        return true;
    }

    Module* Player::LoadModule(const std::string& Dir, const std::string& Name, const std::string& TextureDir)
    {
        for (auto& Mo : Mods)
            if (Mo->Name == Name) return Mo.get();
        Mods.push_back(std::make_unique<Module>());
        Module* Prev = M;
        M = Mods.back().get();
        M->Name = Name;
        M->TextureDir = TextureDir;
        const std::string Base = Dir + "\\" + Name;
        bool Ok = LoadConst(Base + ".const.bin");
        if (!Ok) Err = "cannot read " + Base + ".const.bin";
        Ok = Ok && LoadApt(Base + ".apt.bin");
        if (!Ok && Err.empty()) Err = "cannot read " + Base + ".apt.bin";
        Ok = Ok && LoadGeo(Base + ".geo.bin") && LoadAssets();
        Module* Loaded = M;
        // The movies this one imports from (before running anything, so their classes exist).
        for (size_t i = 0; Ok && i < Loaded->Imports.size(); i++)
        {
            const auto Id = Loaded->Imports[i].first;
            const auto What = Loaded->Imports[i].second;
            const std::string Movie = What.first.substr(What.first.find_last_of("/\\") + 1);
            auto D = ImportDirs.find(Movie);
            if (D == ImportDirs.end()) continue;  // PlaceImports placeholder
            Module* From = LoadModule(D->second.first, Movie, D->second.second);
            if (!From) { Ok = false; break; }
            const Character* Ch = FindExport(From, What.second);
            if (!Ch) { Err = Name + ": " + Movie + " does not export '" + What.second + "'"; Ok = false; break; }
            Loaded->Imported[Id] = Ch;
        }
        M = Prev;
        return Ok ? Loaded : nullptr;
    }

    const Character* Player::FindExport(Module* In, const std::string& Name) const
    {
        if (!In) return nullptr;
        auto E = In->Exports.find(Name);
        if (E != In->Exports.end())
        {
            auto C = In->Chars.find(E->second);
            if (C != In->Chars.end()) return &C->second;
            auto I = In->Imported.find(E->second);
            if (I != In->Imported.end()) return I->second;
        }
        // a name the movie imports (attachMovie of an imported symbol)
        for (auto& [Id, What] : In->Imports)
            if (What.second == Name)
            {
                auto I = In->Imported.find(Id);
                if (I != In->Imported.end()) return I->second;
            }
        return nullptr;
    }

    Module* Player::ModuleOf(const Instance& I) const
    {
        return I.ch && I.ch->module ? I.ch->module : Mods.front().get();
    }

    bool Player::Load(const std::string& Dir, const std::string& Name, const std::string& TextureDir,
                      const std::unordered_map<std::string, std::string>& Fonts)
    {
        FontPaths = Fonts;
        Mods.clear();
        if (!LoadModule(Dir, Name, TextureDir)) return false;
        M = Mods.front().get();
        // Text fields made at run time (createTextField / TextFormat.font) name their font: every
        // configured font is loaded now, so its texture is in Textures() before the host uploads them.
        for (auto& [FontName, Path] : FontPaths)
            if (LoadFont(FontName) < 0) { Err = "cannot read font " + Path; return false; }
        InstallBuiltins();
        return true;
    }

    int Player::LoadFont(const std::string& Name)
    {
        auto Known = FontByName.find(Name);
        if (Known != FontByName.end()) return Known->second;
        auto It = FontPaths.find(Name);
        if (It == FontPaths.end()) return -1;
        BitmapFont F;
        if (!F.Load(It->second)) return -1;
        TextureInfo T;
        T.Path = It->second.substr(0, It->second.size() - 4) + ".png";
        if (!PngSize(T.Path, T.Width, T.Height)) return -1;
        F.TexW = float(T.Width);
        F.TexH = float(T.Height);
        F.Texture = int(TextureList.size());
        TextureList.push_back(T);
        const int Ix = int(FontList.size());
        FontByName[Name] = Ix;
        FontList.push_back(std::move(F));
        return Ix;
    }

    Character& Player::OwnText(Instance& I)
    {
        if (!I.ownCh)
        {
            I.ownCh = std::make_shared<Character>(I.ch ? *I.ch : Character{});
            I.ownCh->type = CharType::Text;
            I.ch = I.ownCh.get();
        }
        return *I.ownCh;
    }

    // TextField.setTextFormat(TextFormat): font (by name), size (pixels), color (rgb), align.
    void Player::ApplyTextFormat(Instance& I, const Value& Format)
    {
        if (Format.t != Value::Obj) return;
        Character& C = OwnText(I);
        const Value Font = Get(Format, "font"), Size = Get(Format, "size"), Color = Get(Format, "color"), Align = Get(Format, "align");
        if (Font.t == Value::Str) { const int F = LoadFont(Font.s); if (F >= 0) C.fontIndex = F; else Fail("setTextFormat: no font '%s'", Font.s.c_str()); }
        if (Size.t == Value::Num && !std::isnan(Size.n)) C.height = float(Size.n);
        if (Color.t == Value::Num && !std::isnan(Color.n))
        {
            C.color = (C.color & 0xFF000000u) | (uint32_t(int64_t(Color.n)) & 0xFFFFFF);
            I.hasTextColor = false;
        }
        if (Align.t == Value::Str) C.align = Align.s == "right" ? 1 : Align.s == "center" ? 2 : 0;
    }

    // ---------------------------------------------------------------- script objects

    void Player::Fail(const char* Fmt, ...)
    {
        ScriptErrors++;
        if (ScriptErrors > 64 || !LogLine) return;
        char Buf[512];
        va_list A;
        va_start(A, Fmt);
        vsnprintf(Buf, sizeof(Buf), Fmt, A);
        va_end(A);
        LogLine(Buf);
    }

    Value Player::RootObject()
    {
        return Root ? Value::Object(ScriptObject(*Root)) : Value();
    }

    size_t Player::InstanceCount() const
    {
        size_t N = 0;
        std::vector<const Instance*> Stack;
        if (Root) Stack.push_back(Root.get());
        while (!Stack.empty())
        {
            const Instance* I = Stack.back();
            Stack.pop_back();
            N++;
            for (const auto& [Depth, C] : I->children) if (C) Stack.push_back(C.get());
        }
        return N;
    }

    ObjRef Player::NewObject()
    {
        auto O = std::make_shared<Object>();
        O->proto = ObjectProto;
        return O;
    }

    ObjRef Player::NewArray(const Args& Items)
    {
        auto O = std::make_shared<Object>();
        O->proto = ArrayProto;
        O->isArray = true;
        for (size_t i = 0; i < Items.size(); i++) O->props[std::to_string(i)] = Items[i];
        O->props["length"] = Value::Number(double(Items.size()));
        return O;
    }

    Value Player::NewNative(Native Fn)
    {
        auto O = std::make_shared<Object>();
        O->proto = FunctionProto;
        O->fn = std::make_unique<Function>();
        O->fn->native = std::move(Fn);
        return Value::Object(O);
    }

    ObjRef Player::ScriptObject(Instance& I)
    {
        if (!I.obj)
        {
            I.obj = std::make_shared<Object>();
            I.obj->proto = I.ch && I.ch->type == CharType::Text ? ObjectProto : MovieClipProto;
            I.obj->inst = I.weak_from_this();
        }
        return I.obj;
    }

    static double Deg(double R) { return R * 57.29577951308232; }

    Value Player::GetProp(Instance& I, const std::string& N, bool& Found)
    {
        Found = true;
        if (N.size() > 1 && N[0] == '_')
        {
            if (N == "_x") return Value::Number(I.m.tx);
            if (N == "_y") return Value::Number(I.m.ty);
            if (N == "_visible") return Value::Boolean(I.visible);
            if (N == "_alpha") return Value::Number(I.cx.mul[3] * 100.0);
            if (N == "_xscale") return Value::Number(std::sqrt(I.m.a * I.m.a + I.m.b * I.m.b) * 100.0);
            if (N == "_yscale") return Value::Number(std::sqrt(I.m.c * I.m.c + I.m.d * I.m.d) * 100.0);
            if (N == "_rotation") return Value::Number(Deg(std::atan2(I.m.b, I.m.a)));
            if (N == "_currentframe") return Value::Number(I.frame + 1);
            if (N == "_totalframes") return Value::Number(I.ch ? double(I.ch->frames.size()) : 0);
            if (N == "_name") return Value::String(I.name);
            if (N == "_parent") return I.parent ? Value::Object(ScriptObject(*I.parent)) : Value();
            if (N == "_width" || N == "_height")
            {
                float B[4] = { 0, 0, 0, 0 };
                bool Any = false;
                Bounds(I, I.m, B, Any);
                return Value::Number(Any ? (N == "_width" ? B[2] - B[0] : B[3] - B[1]) : 0);
            }
        }
        if (I.ch && I.ch->type == CharType::Text)
        {
            if (N == "text") return Value::String(I.text);
            if (N == "textColor") return Value::Number(double((I.hasTextColor ? I.textColor : I.ch->color) & 0xFFFFFF));
            if (N == "autoSize") return Value::String(I.autoSize ? "left" : "none");
            if (N == "multiline") return Value::Boolean(I.ch->multiline);
            if (N == "wordWrap") return Value::Boolean(I.ch->wordwrap);
            if (N == "textWidth") return Value::Number(TextWidth(I));
            if (N == "maxscroll") return Value::Number(1);
            if (N == "textHeight")
            {
                // lines of the field's layout (explicit breaks, word wrap) x the line step RenderText uses
                if (I.ch->fontIndex < 0 || I.text.empty()) return Value::Number(0);
                const BitmapFont& F = FontList[size_t(I.ch->fontIndex)];
                const float S = I.ch->height / F.EmPixels, Inner = I.ch->bounds[2] - I.ch->bounds[0] - 4;
                int Lines = 0;
                std::string Cur;
                auto Flush = [&](const std::string& L)
                {
                    if (!(I.ch->wordwrap && !I.autoSize)) { Lines++; return; }
                    std::string Row, Word;
                    std::istringstream In(L);
                    int N2 = 0;
                    while (In >> Word)
                    {
                        const std::string Try = Row.empty() ? Word : Row + " " + Word;
                        if (!Row.empty() && MeasureText(F, Try, S) > Inner) { N2++; Row = Word; } else Row = Try;
                    }
                    Lines += N2 + 1;
                };
                for (char Chr : I.text)
                {
                    if ((Chr == '\r' || Chr == '\n') && I.ch->multiline) { Flush(Cur); Cur.clear(); continue; }
                    Cur += Chr;
                }
                Flush(Cur);
                return Value::Number(Lines * (F.Ascent + 6 + 7) * S);
            }
            if (N == "setTextFormat")
                return NewNative([](Player& P, const Value& Self, Args& A)
                {
                    if (Self.t == Value::Obj && !A.empty())
                        if (auto In = Self.o->inst.lock()) P.ApplyTextFormat(*In, A.back());
                    return Value();
                });
            if (N == "removeTextField")
                return NewNative([](Player&, const Value& Self, Args&)
                {
                    if (Self.t == Value::Obj)
                        if (auto In = Self.o->inst.lock(); In && In->parent)
                        {
                            auto It = In->parent->children.find(In->depth);
                            if (It != In->parent->children.end() && It->second == In) In->parent->children.erase(It);
                        }
                    return Value();
                });
        }
        if (auto C = I.Child(N)) return Value::Object(ScriptObject(*C));
        Found = false;
        return {};
    }

    void Player::SetText(Instance& I, const Value& V)
    {
        std::string S = V.ToStr();
        if (!S.empty() && S[0] == '#')
            S.erase(0, 1);
        else if (S.rfind("ID_", 0) == 0 && Localize)
        {
            std::string T = Localize(S);
            if (!T.empty()) S = T;
        }
        I.text = S;
    }

    bool Player::SetProp(Instance& I, const std::string& N, const Value& V)
    {
        if (N.size() > 1 && N[0] == '_')
        {
            const double v = V.ToNum();
            if (N == "_visible") { I.visible = V.ToBool(); return true; }
            if (N == "_name") { I.name = V.ToStr(); return true; }
            static const char* const Known[] = { "_x", "_y", "_alpha", "_xscale", "_yscale", "_rotation", "_width", "_height" };
            bool bKnown = false;
            for (const char* K : Known) bKnown = bKnown || N == K;
            if (!bKnown) return false;   // e.g. button_item2's mButtonRender._test: a plain member
            if (std::isnan(v)) return true;
            if (N == "_x") { I.m.tx = float(v); I.scripted = true; return true; }
            if (N == "_y") { I.m.ty = float(v); I.scripted = true; return true; }
            if (N == "_alpha") { I.cx.mul[3] = float(v / 100.0); I.scripted = true; return true; }
            if (N == "_xscale" || N == "_yscale")
            {
                const bool X = N == "_xscale";
                const float Cur = X ? std::sqrt(I.m.a * I.m.a + I.m.b * I.m.b) : std::sqrt(I.m.c * I.m.c + I.m.d * I.m.d);
                const float K = Cur > 1e-6f ? float(v / 100.0) / Cur : 0.f;
                if (X) { I.m.a *= K; I.m.b *= K; } else { I.m.c *= K; I.m.d *= K; }
                I.scripted = true;
                return true;
            }
            if (N == "_rotation")
            {
                const float Sx = std::sqrt(I.m.a * I.m.a + I.m.b * I.m.b), Sy = std::sqrt(I.m.c * I.m.c + I.m.d * I.m.d);
                const float R = float(v / 57.29577951308232);
                I.m.a = Sx * std::cos(R); I.m.b = Sx * std::sin(R);
                I.m.c = -Sy * std::sin(R); I.m.d = Sy * std::cos(R);
                I.scripted = true;
                return true;
            }
            if (N == "_width" || N == "_height")
            {
                // scales the clip so its bounds (in the parent) get this size
                float B[4] = { 0, 0, 0, 0 };
                bool Any = false;
                Bounds(I, I.m, B, Any);
                const float Cur = Any ? (N == "_width" ? B[2] - B[0] : B[3] - B[1]) : 0.f;
                if (Cur > 1e-6f)
                {
                    const float K = float(v) / Cur;
                    if (N == "_width") { I.m.a *= K; I.m.b *= K; } else { I.m.c *= K; I.m.d *= K; }
                }
                I.scripted = true;
                return true;
            }
        }
        if (I.ch && I.ch->type == CharType::Text)
        {
            if (N == "text") { SetText(I, V); return true; }
            if (N == "textColor") { I.textColor = uint32_t(int64_t(V.ToNum())) & 0xFFFFFF; I.hasTextColor = true; return true; }
            if (N == "autoSize") { I.autoSize = V.t == Value::Bool ? V.b : (V.t == Value::Str && V.s != "none"); return true; }
            if (N == "multiline") { OwnText(I).multiline = V.ToBool(); return true; }
            if (N == "wordWrap") { OwnText(I).wordwrap = V.ToBool(); return true; }
            if (N == "type" || N == "selectable" || N == "embedFonts" || N == "html") return true;
        }
        return false;
    }

    Value Player::Get(const Value& Ov, const std::string& N)
    {
        if (Ov.t == Value::Str)
            return N == "length" ? Value::Number(double(Ov.s.size())) : Value();
        if (Ov.t != Value::Obj) return {};
        Object* O = Ov.o.get();
        auto Own = O->props.find(N);
        if (Own != O->props.end()) return Own->second;
        if (auto I = O->inst.lock())
        {
            bool Found;
            Value V = GetProp(*I, N, Found);
            if (Found) return V;
        }
        for (Object* P = O->proto.get(); P; P = P->proto.get())
        {
            auto It = P->props.find(N);
            if (It != P->props.end()) return It->second;
        }
        return {};
    }

    void Player::Set(const Value& Ov, const std::string& N, const Value& V)
    {
        if (Ov.t != Value::Obj) return;
        Object* O = Ov.o.get();
        if (auto I = O->inst.lock())
            if (SetProp(*I, N, V)) return;
        O->props[N] = V;
        if (O->isArray && !N.empty() && isdigit((unsigned char)N[0]))
        {
            const double Index = atof(N.c_str());
            if (Index + 1 > O->props["length"].ToNum()) O->props["length"] = Value::Number(Index + 1);
        }
    }

    struct Player::Ctx
    {
        std::vector<Value> ownPool;
        std::vector<Value>* pool = &ownPool;
        std::vector<Value> ownRegs;
        std::vector<Value>* regs = &ownRegs;
        std::unordered_map<std::string, Value> locals;
        bool inFunction = false;
        Value self;
        std::weak_ptr<Instance> target;
        Module* mod = nullptr;   // whose bytecode / constants
        int depth = 0;
    };

    Value Player::Lookup(Ctx& C, const std::string& N)
    {
        if (N == "this") return C.self;
        if (N == "_global") return Value::Object(GlobalObj);
        if (N == "_root") return Root ? Value::Object(ScriptObject(*Root)) : Value();
        auto L = C.locals.find(N);
        if (L != C.locals.end()) return L->second;
        if (auto T = C.target.lock())
        {
            if (N == "_parent") return T->parent ? Value::Object(ScriptObject(*T->parent)) : Value();
            Value V = Get(Value::Object(ScriptObject(*T)), N);
            if (V.t != Value::Undef) return V;
        }
        auto G = GlobalObj->props.find(N);
        return G != GlobalObj->props.end() ? G->second : Value();
    }

    Value Player::MakeFunction(Ctx& C, uint32_t Start, uint32_t End, bool V2, uint8_t Regs, uint16_t Flags,
                               std::vector<std::pair<int, std::string>> Params)
    {
        auto O = std::make_shared<Object>();
        O->proto = FunctionProto;
        O->fn = std::make_unique<Function>();
        Function& F = *O->fn;
        F.start = Start;
        F.end = End;
        F.v2 = V2;
        F.registers = Regs;
        F.flags = Flags;
        F.params = std::move(Params);
        F.pool = *C.pool;
        F.target = C.target;
        F.module = C.mod ? C.mod : M;
        O->props["prototype"] = Value::Object(NewObject());
        return Value::Object(O);
    }

    Value Player::Call(const Value& Fv, const Value& This, Args A)
    {
        if (Fv.t != Value::Obj || !Fv.o->fn) return {};
        Function& F = *Fv.o->fn;
        if (F.native) return F.native(*this, This, A);

        Ctx C;
        C.ownPool = F.pool;
        C.self = This;
        C.target = F.target;
        C.mod = F.module;
        C.inFunction = true;
        if (F.v2)
        {
            int Highest = F.registers;
            for (auto& P : F.params) Highest = std::max(Highest, P.first + 1);
            C.ownRegs.assign(size_t(std::max(Highest, 8)), Value());
            size_t R = 1;
            if (F.flags & 0x01) C.ownRegs[R++] = This;
            if (F.flags & 0x04) C.ownRegs[R++] = Value::Object(NewArray(A));
            if (F.flags & 0x10) C.ownRegs[R++] = NewNative([](Player&, const Value&, Args&) { return Value(); });
            if (F.flags & 0x40) C.ownRegs[R++] = Root ? Value::Object(ScriptObject(*Root)) : Value();
            if (F.flags & 0x80)
            {
                auto T = F.target.lock();
                C.ownRegs[R++] = T && T->parent ? Value::Object(ScriptObject(*T->parent)) : Value();
            }
            if (F.flags & 0x100) C.ownRegs[R++] = Value::Object(GlobalObj);
            for (size_t i = 0; i < F.params.size(); i++)
            {
                Value V = i < A.size() ? A[i] : Value();
                if (F.params[i].first > 0) C.ownRegs[size_t(F.params[i].first)] = V;
                else C.locals[F.params[i].second] = V;
            }
        }
        else
        {
            C.ownRegs.assign(4, Value());
            for (size_t i = 0; i < F.params.size(); i++)
                C.locals[F.params[i].second] = i < A.size() ? A[i] : Value();
        }
        return Execute(C, F.start, F.end);
    }

    Value Player::CallMethod(const Value& Obj, const std::string& Name, Args A)
    {
        return Call(Get(Obj, Name), Obj, std::move(A));
    }

    // ---------------------------------------------------------------- interpreter

    static bool Aligned(uint8_t Op)
    {
        switch (Op)
        {
        case 0x9B: case 0x8E: case 0x88: case 0x9D: case 0x99: case 0x96: case 0x83: case 0x8C: case 0x87:
        case 0x8B: case 0x81: case 0x9F: case 0x94: case 0xA1: case 0xB8: case 0xA4: case 0xA6: case 0xA5: case 0xA7:
            return true;
        }
        return false;
    }

    static bool LooseEquals(const Value& A, const Value& B)
    {
        if (A.t == B.t)
        {
            switch (A.t)
            {
            case Value::Undef: case Value::Null: return true;
            case Value::Bool: return A.b == B.b;
            case Value::Num: return A.n == B.n;
            case Value::Str: return A.s == B.s;
            case Value::Obj: return A.o == B.o;
            }
        }
        const bool An = A.t == Value::Undef || A.t == Value::Null, Bn = B.t == Value::Undef || B.t == Value::Null;
        if (An || Bn) return An && Bn;
        if (A.t == Value::Obj || B.t == Value::Obj) return false;
        return A.ToNum() == B.ToNum();
    }

    Value Player::Execute(Ctx& C, uint32_t Pc, uint32_t End)
    {
        if (++C.depth > 64) { Fail("script recursion too deep"); return {}; }
        std::vector<Value> St;
        auto Pop = [&]() -> Value
        {
            if (St.empty()) return {};
            Value V = std::move(St.back());
            St.pop_back();
            return V;
        };
        auto PopArgs = [&]() -> Args
        {
            const double N = Pop().ToNum();
            Args A;
            const int Count = std::isnan(N) || N < 0 || N > 64 ? 0 : int(N);
            for (int i = 0; i < Count; i++) A.push_back(Pop());
            return A;
        };
        auto PoolValue = [&](uint32_t i) -> Value { return i < C.pool->size() ? (*C.pool)[i] : Value(); };
        auto Reg = [&](uint32_t i) -> Value& { if (i >= C.regs->size()) C.regs->resize(i + 1); return (*C.regs)[i]; };
        auto Target = [&]() { return C.target.lock(); };
        auto CallNamed = [&](const Value& Obj, const std::string& Name, Args A) -> Value
        {
            Value Fn = Get(Obj, Name);
            if ((Fn.t != Value::Obj || !Fn.o->fn) && Obj.t == Value::Obj)
            {
                // Apt (like Flash before SWF 7) matches names without regard to case: objectquickmenu
                // calls its slider's SetSubtitle as "SetSubTitle". Only a call that would fail looks.
                for (Object* P = Obj.o.get(); P && (Fn.t != Value::Obj || !Fn.o->fn); P = P->proto.get())
                    for (auto& Kv : P->props)
                        if (Kv.second.t == Value::Obj && Kv.second.o->fn && _stricmp(Kv.first.c_str(), Name.c_str()) == 0) { Fn = Kv.second; break; }
            }
            if (Fn.t != Value::Obj || !Fn.o->fn)
            {
                std::string On = Obj.t != Value::Obj ? "undefined" : Obj.o->inst.lock() ? "clip '" + Obj.o->inst.lock()->name + "'" : "object";
                Fail("call of missing method '%s' on %s", Name.c_str(), On.c_str());
                return {};
            }
            return Call(Fn, Obj, std::move(A));
        };
        auto Define = [&](const std::string& Name, const Value& V)
        {
            if (C.inFunction) C.locals[Name] = V;
            else if (auto T = Target()) Set(Value::Object(ScriptObject(*T)), Name, V);
        };

        // Bytecode and constants are those of the context's movie file for the whole run.
        struct Restore { Player& P; Module* Old; ~Restore() { P.M = Old; } } Keep{ *this, M };
        if (C.mod) M = C.mod;
        else C.mod = M;
        const uint8_t* D = M->Data.data();
        const uint32_t Size = uint32_t(M->Data.size());
        int Guard = 200000;
        while (true)
        {
            if (End && Pc >= End) break;
            if (Pc >= Size || --Guard < 0) { Fail("script ran off at %#x", Pc); break; }
            const uint32_t At = Pc;
            const uint8_t Op = D[Pc++];
            if (Aligned(Op)) Pc = (Pc + 3) & ~3u;
            switch (Op)
            {
            case 0x00: C.depth--; return {};  // End
            case 0x04: if (auto T = Target()) GotoFrame(*T, T->frame + 1, false); break;
            case 0x05: if (auto T = Target()) GotoFrame(*T, std::max(0, T->frame - 1), false); break;
            case 0x06: if (auto T = Target()) T->playing = true; break;
            case 0x07: if (auto T = Target()) T->playing = false; break;
            case 0x08: case 0x09: break;
            case 0x0A: { Value b = Pop(), a = Pop(); St.push_back(Value::Number(a.ToNum() + b.ToNum())); break; }
            case 0x0B: { Value b = Pop(), a = Pop(); St.push_back(Value::Number(a.ToNum() - b.ToNum())); break; }
            case 0x0C: { Value b = Pop(), a = Pop(); St.push_back(Value::Number(a.ToNum() * b.ToNum())); break; }
            case 0x0D: { Value b = Pop(), a = Pop(); St.push_back(Value::Number(a.ToNum() / b.ToNum())); break; }
            case 0x0E: { Value b = Pop(), a = Pop(); St.push_back(Value::Boolean(a.ToNum() == b.ToNum())); break; }
            case 0x0F: { Value b = Pop(), a = Pop(); St.push_back(Value::Boolean(a.ToNum() < b.ToNum())); break; }
            case 0x10: { Value b = Pop(), a = Pop(); St.push_back(Value::Boolean(a.ToBool() && b.ToBool())); break; }
            case 0x11: { Value b = Pop(), a = Pop(); St.push_back(Value::Boolean(a.ToBool() || b.ToBool())); break; }
            case 0x12: St.push_back(Value::Boolean(!Pop().ToBool())); break;
            case 0x13: { Value b = Pop(), a = Pop(); St.push_back(Value::Boolean(a.ToStr() == b.ToStr())); break; }
            case 0x14: St.push_back(Value::Number(double(Pop().ToStr().size()))); break;
            case 0x17: Pop(); break;
            case 0x18: St.push_back(Value::Number(std::trunc(Pop().ToNum()))); break;
            case 0x1C: St.push_back(Lookup(C, Pop().ToStr())); break;  // GetVariable
            case 0x1D: { Value V = Pop(); Define(Pop().ToStr(), V); break; }  // SetVariable
            case 0x21: { Value b = Pop(), a = Pop(); St.push_back(Value::String(a.ToStr() + b.ToStr())); break; }
            case 0x26: Pop(); break;  // Trace
            case 0x30: { const double N = Pop().ToNum(); St.push_back(Value::Number(N > 0 ? double(rand() % int(N)) : 0)); break; }
            case 0x34: St.push_back(Value::Number(double(int64_t(Clock * 1000)))); break;
            case 0x3A: { const std::string N = Pop().ToStr(); Value O = Pop(); if (O.t == Value::Obj) O.o->props.erase(N); St.push_back(Value::Boolean(true)); break; }
            case 0x3B: { const std::string N = Pop().ToStr(); C.locals.erase(N); St.push_back(Value::Boolean(true)); break; }
            case 0x3C: { Value V = Pop(); Define(Pop().ToStr(), V); break; }  // DefineLocal
            case 0x41: { const std::string N = Pop().ToStr(); if (C.inFunction) C.locals[N] = Value(); break; }  // Var
            case 0x3D:  // CallFunction
            {
                const std::string N = Pop().ToStr();
                Args A = PopArgs();
                Value Fn = Lookup(C, N);
                if (Fn.t != Value::Obj || !Fn.o->fn) { Fail("call of missing function '%s'", N.c_str()); St.push_back({}); }
                else St.push_back(Call(Fn, Value(), std::move(A)));
                break;
            }
            case 0x3E: { Value R = Pop(); C.depth--; return R; }  // Return
            case 0x3F: { Value b = Pop(), a = Pop(); St.push_back(Value::Number(std::fmod(a.ToNum(), b.ToNum()))); break; }
            case 0x40:  // NewObject
            {
                const std::string N = Pop().ToStr();
                Args A = PopArgs();
                Value Ctor = Lookup(C, N);
                if (Ctor.t != Value::Obj || !Ctor.o->fn) { Fail("new of missing class '%s'", N.c_str()); St.push_back({}); break; }
                if (Ctor.o->fn->native) { St.push_back(Call(Ctor, Value(), std::move(A))); break; }
                ObjRef O = NewObject();
                Value Proto = Get(Ctor, "prototype");
                if (Proto.t == Value::Obj) O->proto = Proto.o;
                Call(Ctor, Value::Object(O), std::move(A));
                St.push_back(Value::Object(O));
                break;
            }
            case 0x42: St.push_back(Value::Object(NewArray(PopArgs()))); break;  // InitArray
            case 0x43:  // InitObject
            {
                const double N = Pop().ToNum();
                ObjRef O = NewObject();
                for (int i = 0; i < int(N) && i < 64; i++) { Value V = Pop(); O->props[Pop().ToStr()] = V; }
                St.push_back(Value::Object(O));
                break;
            }
            case 0x44:
            {
                Value V = Pop();
                const char* T = V.t == Value::Bool ? "boolean" : V.t == Value::Num ? "number" : V.t == Value::Str ? "string" :
                    V.t == Value::Null ? "null" : V.t == Value::Undef ? "undefined" :
                    V.o->fn ? "function" : V.o->inst.lock() ? "movieclip" : "object";
                St.push_back(Value::String(T));
                break;
            }
            case 0x47:  // Add2
            {
                Value b = Pop(), a = Pop();
                if (a.t == Value::Str || b.t == Value::Str) St.push_back(Value::String(a.ToStr() + b.ToStr()));
                else St.push_back(Value::Number(a.ToNum() + b.ToNum()));
                break;
            }
            case 0x48:  // LessThan2
            {
                Value b = Pop(), a = Pop();
                St.push_back(Value::Boolean(a.t == Value::Str && b.t == Value::Str ? a.s < b.s : a.ToNum() < b.ToNum()));
                break;
            }
            case 0x67:  // Greater
            {
                Value b = Pop(), a = Pop();
                St.push_back(Value::Boolean(a.t == Value::Str && b.t == Value::Str ? a.s > b.s : a.ToNum() > b.ToNum()));
                break;
            }
            case 0x49: { Value b = Pop(), a = Pop(); St.push_back(Value::Boolean(LooseEquals(a, b))); break; }
            case 0x66: { Value b = Pop(), a = Pop(); St.push_back(Value::Boolean(a.t == b.t && LooseEquals(a, b))); break; }
            case 0x4A: St.push_back(Value::Number(Pop().ToNum())); break;
            case 0x4B: St.push_back(Value::String(Pop().ToStr())); break;
            case 0x4C: { Value V = St.empty() ? Value() : St.back(); St.push_back(V); break; }
            case 0x4D: if (St.size() >= 2) std::swap(St[St.size() - 1], St[St.size() - 2]); break;
            case 0x4E: { const std::string N = Pop().ToStr(); Value O = Pop(); St.push_back(Get(O, N)); break; }  // GetMember
            case 0x4F: { Value V = Pop(); const std::string N = Pop().ToStr(); Value O = Pop(); Set(O, N, V); break; }  // SetMember
            case 0x50: St.push_back(Value::Number(Pop().ToNum() + 1)); break;
            case 0x51: St.push_back(Value::Number(Pop().ToNum() - 1)); break;
            case 0x52: case 0x5D: case 0x5E:  // CallMethod (0x5D pops the result)
            {
                Value N = Pop(), O = Pop();
                Args A = PopArgs();
                Value R;
                if (N.t == Value::Undef || (N.t == Value::Str && N.s.empty())) R = Call(O, Value(), std::move(A));
                else R = CallNamed(O, N.ToStr(), std::move(A));
                if (Op != 0x5D) St.push_back(R);
                break;
            }
            case 0x56: St.push_back(C.self); break;                       // PushThis
            case 0x58: St.push_back(Value::Object(GlobalObj)); break;     // PushGlobal
            case 0x59: St.push_back(Value::Number(0)); break;
            case 0x5A: St.push_back(Value::Number(1)); break;
            case 0x5B: case 0x5C:  // CallFunc (EA): function object on the stack
            {
                Value Fn = Pop();
                Args A = PopArgs();
                Value R = Call(Fn, Value(), std::move(A));
                if (Op == 0x5C) St.push_back(R);
                break;
            }
            case 0x60: { Value b = Pop(), a = Pop(); St.push_back(Value::Number(double(int32_t(a.ToNum()) & int32_t(b.ToNum())))); break; }
            case 0x61: { Value b = Pop(), a = Pop(); St.push_back(Value::Number(double(int32_t(a.ToNum()) | int32_t(b.ToNum())))); break; }
            case 0x62: { Value b = Pop(), a = Pop(); St.push_back(Value::Number(double(int32_t(a.ToNum()) ^ int32_t(b.ToNum())))); break; }
            case 0x69:  // Extends
            {
                Value Super = Pop(), Sub = Pop();
                ObjRef P = NewObject();
                Value SuperProto = Get(Super, "prototype");
                if (SuperProto.t == Value::Obj) P->proto = SuperProto.o;
                P->props["__constructor__"] = Super;
                Set(Sub, "prototype", Value::Object(P));
                break;
            }
            case 0x70: St.push_back(C.self); break;                       // PushThisVar
            case 0x71: St.push_back(Value::Object(GlobalObj)); break;     // PushGlobalVar
            case 0x72: St.push_back(Value::Number(0)); break;             // ZeroVar
            case 0x73: St.push_back(Value::Boolean(true)); break;
            case 0x74: St.push_back(Value::Boolean(false)); break;
            case 0x75: St.push_back(Value::NullValue()); break;
            case 0x76: St.push_back(Value()); break;
            case 0x77: break;
            case 0x81: { const int F = int32_t(U32(Pc)); Pc += 4; if (auto T = Target()) GotoFrame(*T, F, false); break; }
            case 0x83: Pc += 8; break;  // GetURL
            case 0x87: { const uint32_t R = U32(Pc); Pc += 4; Reg(R) = St.empty() ? Value() : St.back(); break; }
            case 0x88:  // ConstantPool
            {
                const uint32_t N = U32(Pc), Ptr = U32(Pc + 4);
                Pc += 8;
                C.ownPool.clear();
                for (uint32_t i = 0; i < N; i++) C.ownPool.push_back(ConstValue(U32(Ptr + 4 * i)));
                C.pool = &C.ownPool;
                break;
            }
            case 0x8C:  // GotoLabel
            {
                const std::string L = CStr(U32(Pc));
                Pc += 4;
                if (auto T = Target()) GotoFrame(*T, FindFrame(*T, Value::String(L)), false);
                break;
            }
            case 0x8B: Pc += 4; break;  // SetTarget (unused by these movies)
            case 0x8E:  // DefineFunction2
            {
                const std::string Name = CStr(U32(Pc));
                const uint32_t NParams = U32(Pc + 4);
                const uint8_t Regs = D[Pc + 9];
                const uint16_t Flags = uint16_t((D[Pc + 10] << 8) | D[Pc + 11]);
                const uint32_t Ptr = U32(Pc + 12), BodySize = U32(Pc + 16);
                std::vector<std::pair<int, std::string>> Params;
                for (uint32_t i = 0; i < NParams; i++) Params.push_back({ int32_t(U32(Ptr + 8 * i)), CStr(U32(Ptr + 8 * i + 4)) });
                Pc += 28;
                Value Fn = MakeFunction(C, Pc, Pc + BodySize, true, Regs, Flags, std::move(Params));
                Pc += BodySize;
                if (Name.empty()) St.push_back(Fn); else Define(Name, Fn);
                break;
            }
            case 0x9B:  // DefineFunction
            {
                const std::string Name = CStr(U32(Pc));
                const uint32_t NParams = U32(Pc + 4), Ptr = U32(Pc + 8), BodySize = U32(Pc + 12);
                std::vector<std::pair<int, std::string>> Params;
                for (uint32_t i = 0; i < NParams; i++) Params.push_back({ 0, CStr(U32(Ptr + 4 * i)) });
                Pc += 24;
                Value Fn = MakeFunction(C, Pc, Pc + BodySize, false, 0, 0, std::move(Params));
                Pc += BodySize;
                if (Name.empty()) St.push_back(Fn); else Define(Name, Fn);
                break;
            }
            case 0x94: Pc += 4; break;  // With (unused)
            case 0x96:  // PushData
            {
                const uint32_t N = U32(Pc), Ptr = U32(Pc + 4);
                Pc += 8;
                for (uint32_t i = 0; i < N; i++) St.push_back(ConstValue(U32(Ptr + 4 * i)));
                break;
            }
            case 0x99: { const int32_t Off = int32_t(U32(Pc)); Pc += 4; Pc = uint32_t(int32_t(Pc) + Off); break; }
            case 0x9D: { const int32_t Off = int32_t(U32(Pc)); Pc += 4; if (Pop().ToBool()) Pc = uint32_t(int32_t(Pc) + Off); break; }
            case 0xB8: { const int32_t Off = int32_t(U32(Pc)); Pc += 4; if (!Pop().ToBool()) Pc = uint32_t(int32_t(Pc) + Off); break; }
            case 0x9F: { Pc += 4; Value F = Pop(); if (auto T = Target()) GotoFrame(*T, FindFrame(*T, F), false); break; }
            case 0xA1: St.push_back(Value::String(CStr(U32(Pc)))); Pc += 4; break;
            case 0xA2: St.push_back(PoolValue(D[Pc])); Pc += 1; break;
            case 0xA3: St.push_back(PoolValue(uint32_t((D[Pc] << 8) | D[Pc + 1]))); Pc += 2; break;
            case 0xA4: St.push_back(Lookup(C, CStr(U32(Pc)))); Pc += 4; break;
            case 0xA5: { const std::string N = CStr(U32(Pc)); Pc += 4; Value O = Pop(); St.push_back(Get(O, N)); break; }
            // SetStringVar: the variable's name is on the stack, its value the inline string (every
            // use is an onClipEvent(construct) "mButtonImage = 'X'" / "_type = '...'")
            case 0xA6: { const std::string V = CStr(U32(Pc)); Pc += 4; Define(Pop().ToStr(), Value::String(V)); break; }
            case 0xA7: { const std::string N = CStr(U32(Pc)); Pc += 4; Value V = Pop(); Value O = Pop(); Set(O, N, V); break; }
            case 0xAE: St.push_back(Lookup(C, PoolValue(D[Pc]).ToStr())); Pc += 1; break;
            case 0xAF: { const std::string N = PoolValue(D[Pc]).ToStr(); Pc += 1; Value O = Pop(); St.push_back(Get(O, N)); break; }
            case 0xB0: case 0xB1:  // CallNamedFunc(Pop)
            {
                const std::string N = PoolValue(D[Pc]).ToStr();
                Pc += 1;
                Args A = PopArgs();
                Value R = Call(Lookup(C, N), Value(), std::move(A));
                if (Op == 0xB1) St.push_back(R);
                break;
            }
            case 0xB2: case 0xB3:  // CallNamedMethod(Pop)
            {
                const std::string N = PoolValue(D[Pc]).ToStr();
                Pc += 1;
                Value O = Pop();
                Args A = PopArgs();
                Value R = CallNamed(O, N, std::move(A));
                if (Op == 0xB3) St.push_back(R);
                break;
            }
            case 0xB4: St.push_back(Value::Number(F32(Pc))); Pc += 4; break;
            case 0xB5: St.push_back(Value::Number(D[Pc])); Pc += 1; break;
            case 0xB6: St.push_back(Value::Number((D[Pc] << 8) | D[Pc + 1])); Pc += 2; break;
            case 0xB7: St.push_back(Value::Number(int32_t(U32(Pc)))); Pc += 4; break;
            case 0xB9: St.push_back(Reg(D[Pc])); Pc += 1; break;
            default:
                Fail("unsupported opcode %#x at %#x", Op, At);
                C.depth--;
                return {};
            }
        }
        C.depth--;
        return {};
    }

    // ---------------------------------------------------------------- builtins

    void Player::InstallBuiltins()
    {
        ObjectProto = std::make_shared<Object>();
        FunctionProto = std::make_shared<Object>();
        FunctionProto->proto = ObjectProto;
        ArrayProto = std::make_shared<Object>();
        ArrayProto->proto = ObjectProto;
        MovieClipProto = std::make_shared<Object>();
        MovieClipProto->proto = ObjectProto;
        GlobalObj = std::make_shared<Object>();

        auto ClipOf = [](const Value& Self) -> std::shared_ptr<Instance>
        {
            return Self.t == Value::Obj ? Self.o->inst.lock() : nullptr;
        };
        MovieClipProto->props["stop"] = NewNative([ClipOf](Player&, const Value& Self, Args&) { if (auto I = ClipOf(Self)) I->playing = false; return Value(); });
        MovieClipProto->props["play"] = NewNative([ClipOf](Player&, const Value& Self, Args&) { if (auto I = ClipOf(Self)) I->playing = true; return Value(); });
        MovieClipProto->props["gotoAndPlay"] = NewNative([ClipOf](Player& P, const Value& Self, Args& A)
        {
            if (auto I = ClipOf(Self); I && !A.empty()) P.GotoFrame(*I, P.FindFrame(*I, A[0]), true);
            return Value();
        });
        MovieClipProto->props["gotoAndStop"] = NewNative([ClipOf](Player& P, const Value& Self, Args& A)
        {
            if (auto I = ClipOf(Self); I && !A.empty()) P.GotoFrame(*I, P.FindFrame(*I, A[0]), false);
            return Value();
        });
        MovieClipProto->props["nextFrame"] = NewNative([ClipOf](Player& P, const Value& Self, Args&)
        {
            if (auto I = ClipOf(Self)) P.GotoFrame(*I, I->frame + 1, false);
            return Value();
        });

        // attachMovie(linkage name, instance name, depth[, init object]): a new instance of an
        // exported (or imported) symbol of the clip's movie, constructed with its registered class.
        MovieClipProto->props["attachMovie"] = NewNative([ClipOf](Player& P, const Value& Self, Args& A)
        {
            auto I = ClipOf(Self);
            if (!I || A.size() < 3) return Value();
            const Character* Ch = P.FindExport(P.ModuleOf(*I), A[0].ToStr());
            if (!Ch)
                for (auto& Mo : P.Mods)
                    if ((Ch = P.FindExport(Mo.get(), A[0].ToStr()))) break;
            if (!Ch) { P.Fail("attachMovie: no symbol '%s'", A[0].ToStr().c_str()); return Value(); }
            const int Depth = int(A[2].ToNum());
            auto Made = P.Create(*I, Ch, -1, ScriptDepth + Depth, A[1].ToStr());
            if (A.size() > 3 && A[3].t == Value::Obj)
                for (auto& [K, V] : A[3].o->props) P.Set(Value::Object(P.ScriptObject(*Made)), K, V);
            P.Construct(*Made);
            return Value::Object(P.ScriptObject(*Made));
        });
        MovieClipProto->props["removeMovieClip"] = NewNative([ClipOf](Player&, const Value& Self, Args&)
        {
            if (auto I = ClipOf(Self); I && I->parent)
            {
                auto It = I->parent->children.find(I->depth);
                if (It != I->parent->children.end() && It->second == I) I->parent->children.erase(It);
            }
            return Value();
        });
        // createTextField(name, depth, x, y, width, height): an empty text field (no font until
        // setTextFormat names one; sk8popup's body and button_menu's labels).
        MovieClipProto->props["createTextField"] = NewNative([ClipOf](Player& P, const Value& Self, Args& A)
        {
            auto I = ClipOf(Self);
            if (!I || A.size() < 6) return Value();
            auto Ch = std::make_shared<Character>();
            Ch->type = CharType::Text;
            Ch->module = P.ModuleOf(*I);
            Ch->bounds[0] = 0; Ch->bounds[1] = 0;
            Ch->bounds[2] = float(A[4].ToNum()); Ch->bounds[3] = float(A[5].ToNum());
            Ch->height = 12;
            Ch->color = 0xFF000000u;
            Ch->fontIndex = P.FontList.empty() ? -1 : 0;
            auto Made = P.Create(*I, Ch.get(), -1, ScriptDepth + int(A[1].ToNum()), A[0].ToStr());
            Made->ownCh = Ch;
            Made->m.tx = float(A[2].ToNum());
            Made->m.ty = float(A[3].ToNum());
            Made->scripted = true;
            return Value::Object(P.ScriptObject(*Made));
        });
        MovieClipProto->props["getDepth"] = NewNative([ClipOf](Player&, const Value& Self, Args&)
        {
            auto I = ClipOf(Self);
            return I ? Value::Number(I->depth >= ScriptDepth ? I->depth - ScriptDepth : I->depth) : Value();
        });

        ArrayProto->props["push"] = NewNative([](Player& P, const Value& Self, Args& A)
        {
            if (Self.t != Value::Obj) return Value();
            double N = P.Get(Self, "length").ToNum();
            if (std::isnan(N)) N = 0;
            for (auto& V : A) P.Set(Self, std::to_string(int64_t(N++)), V);
            Self.o->props["length"] = Value::Number(N);
            return Value::Number(N);
        });
        ArrayProto->props["pop"] = NewNative([](Player& P, const Value& Self, Args&)
        {
            if (Self.t != Value::Obj) return Value();
            const double N = P.Get(Self, "length").ToNum();
            if (std::isnan(N) || N < 1) return Value();
            const std::string Key = std::to_string(int64_t(N - 1));
            Value V = P.Get(Self, Key);
            Self.o->props.erase(Key);
            Self.o->props["length"] = Value::Number(N - 1);
            return V;
        });

        Value MovieClipCtor = NewNative([](Player&, const Value&, Args&) { return Value(); });
        MovieClipCtor.o->props["prototype"] = Value::Object(MovieClipProto);
        GlobalObj->props["MovieClip"] = MovieClipCtor;

        Value ObjectCtor = NewNative([](Player& P, const Value&, Args&) { return Value::Object(P.NewObject()); });
        ObjectCtor.o->props["prototype"] = Value::Object(ObjectProto);
        GlobalObj->props["Object"] = ObjectCtor;
        // Object.registerClass(linkage name, class): instances of that symbol of the movie whose
        // code is running get the class's prototype and constructor.
        ObjectCtor.o->props["registerClass"] = NewNative([](Player& P, const Value&, Args& A)
        {
            if (A.size() < 2) return Value::Boolean(false);
            const Character* Ch = P.FindExport(P.M, A[0].ToStr());
            if (!Ch) { P.Fail("registerClass: no symbol '%s' in %s", A[0].ToStr().c_str(), P.M->Name.c_str()); return Value::Boolean(false); }
            P.Classes[Ch] = A[1];
            return Value::Boolean(true);
        });

        Value ArrayCtor = NewNative([](Player& P, const Value&, Args& A)
        {
            // new Array(n) makes n empty slots; any other argument list is the contents.
            if (A.size() == 1 && A[0].t == Value::Num)
            {
                ObjRef O = P.NewArray({});
                O->props["length"] = A[0];
                return Value::Object(O);
            }
            return Value::Object(P.NewArray(A));
        });
        ArrayCtor.o->props["prototype"] = Value::Object(ArrayProto);
        GlobalObj->props["Array"] = ArrayCtor;
        // new TextFormat(): a plain object the script fills (font, size, color, align)
        Value TextFormatCtor = NewNative([](Player& P, const Value&, Args&) { return Value::Object(P.NewObject()); });
        TextFormatCtor.o->props["prototype"] = Value::Object(ObjectProto);
        GlobalObj->props["TextFormat"] = TextFormatCtor;

        ObjRef MathObj = NewObject();
        auto Unary = [this](double (*F)(double)) { return NewNative([F](Player&, const Value&, Args& A) { return Value::Number(F(A.empty() ? NAN : A[0].ToNum())); }); };
        MathObj->props["floor"] = Unary([](double x) { return std::floor(x); });
        MathObj->props["ceil"] = Unary([](double x) { return std::ceil(x); });
        MathObj->props["round"] = Unary([](double x) { return std::floor(x + 0.5); });
        MathObj->props["abs"] = Unary([](double x) { return std::fabs(x); });
        MathObj->props["random"] = NewNative([](Player&, const Value&, Args&) { return Value::Number(double(rand()) / (double(RAND_MAX) + 1.0)); });
        MathObj->props["min"] = NewNative([](Player&, const Value&, Args& A) { return Value::Number(A.size() < 2 ? NAN : std::min(A[0].ToNum(), A[1].ToNum())); });
        MathObj->props["max"] = NewNative([](Player&, const Value&, Args& A) { return Value::Number(A.size() < 2 ? NAN : std::max(A[0].ToNum(), A[1].ToNum())); });
        GlobalObj->props["Math"] = Value::Object(MathObj);

        GlobalObj->props["ASSetPropFlags"] = NewNative([](Player&, const Value&, Args&) { return Value(); });
    }

    // ---------------------------------------------------------------- timeline

    // A new instance of Ch on Parent at Depth. A class registered for the character
    // (Object.registerClass) gives it its prototype and its constructor runs once the
    // instance's first frame is built (call Construct).
    std::shared_ptr<Instance> Player::Create(Instance& Parent, const Character* Ch, int CharId, int Depth, const std::string& Name,
                                             const Mat& Placed, const Cx& Tint)
    {
        auto I = std::make_shared<Instance>();
        I->ch = Ch;
        I->charId = CharId;
        I->parent = &Parent;
        I->depth = Depth;
        I->name = Name;
        I->m = Placed;
        I->cx = Tint;
        I->fresh = true;
        Parent.children[Depth] = I;
        auto Cls = Classes.find(Ch);
        if (Cls != Classes.end())
        {
            Value Proto = Get(Cls->second, "prototype");
            if (Proto.t == Value::Obj) ScriptObject(*I)->proto = Proto.o;
        }
        if (Ch->type == CharType::Text)
            SetText(*I, Value::String(Ch->text));
        if (Ch->type == CharType::Sprite && !Ch->frames.empty())
            ApplyFrame(*I, 0, true);
        return I;
    }

    void Player::Construct(Instance& I)
    {
        auto Cls = Classes.find(I.ch);
        if (Cls != Classes.end()) Call(Cls->second, Value::Object(ScriptObject(I)), {});
    }

    std::shared_ptr<Instance> Player::Place(Instance& Parent, const FrameItem& It)
    {
        const bool Move = It.flags & 1, HasChar = It.flags & 2;
        auto Existing = Parent.children.find(It.depth);
        const bool Same = Existing != Parent.children.end() && Existing->second->charId == It.character;
        if (HasChar && !(Move && Same))
        {
            Module* Mo = ModuleOf(Parent);
            const Character* Ch = nullptr;
            auto C = Mo->Chars.find(It.character);
            if (C != Mo->Chars.end()) Ch = &C->second;
            else if (auto Im = Mo->Imported.find(It.character); Im != Mo->Imported.end()) Ch = Im->second;
            if (!Ch)
            {
                if (!PlaceImports) return nullptr;
                static Character Imported = [] { Character E; E.type = CharType::Sprite; return E; }();
                auto I = std::make_shared<Instance>();
                I->ch = &Imported;
                I->charId = It.character;
                I->parent = &Parent;
                I->depth = It.depth;
                if (It.flags & 0x20) I->name = It.name;
                if (It.flags & 0x04) I->m = It.matrix;
                if (It.flags & 0x08) I->cx = It.cx;
                I->fresh = true;
                Parent.children[It.depth] = I;
                return I;
            }
            Mat Placed;
            Cx Tint;
            if (It.flags & 0x04) Placed = It.matrix;
            if (It.flags & 0x08) Tint = It.cx;
            auto Made = Create(Parent, Ch, It.character, It.depth, (It.flags & 0x20) ? It.name : std::string(), Placed, Tint);
            // onClipEvent(construct / initialize) runs before the class constructor (button_item2's
            // mButtonImage = "X" on the dropper's size box), onClipEvent(load) after it
            auto RunClipEvents = [&](uint32_t Mask)
            {
                for (auto& [Flags, Code] : It.clipEvents)
                    if ((Flags & Mask) && Code)
                    {
                        Ctx C;
                        C.regs = &GlobalRegs;
                        C.target = Made;
                        C.self = Value::Object(ScriptObject(*Made));
                        C.mod = Mo;
                        Execute(C, Code, 0);
                    }
            };
            RunClipEvents(0x40000 | 0x200);
            Construct(*Made);
            RunClipEvents(0x01);
            return Made;
        }
        if (Existing != Parent.children.end())
        {
            Instance& I = *Existing->second;
            if (!I.scripted)
            {
                if (It.flags & 0x04) I.m = It.matrix;
                if (It.flags & 0x08) I.cx = It.cx;
            }
            if (It.flags & 0x20) I.name = It.name;
            return Existing->second;
        }
        return nullptr;
    }

    void Player::ApplyFrame(Instance& Clip, int F, bool Actions)
    {
        const Character& Ch = Clip.ch ? *Clip.ch : Mods.front()->MovieChar;
        if (F < 0 || F >= int(Ch.frames.size())) return;
        Clip.frame = F;
        const Frame& Fr = Ch.frames[size_t(F)];
        // Init actions (class definitions) run first, once; then this clip's frame actions are
        // queued before those of the children placed on this frame.
        for (auto& It : Fr.items)
            if (It.kind == FrameItem::InitAction && !InitDone)
            {
                Ctx C;
                C.regs = &GlobalRegs;
                C.target = Clip.weak_from_this();
                C.self = Value::Object(ScriptObject(Clip));
                C.mod = ModuleOf(Clip);
                Execute(C, It.code, 0);
            }
        if (Actions)
            for (auto& It : Fr.items)
                if (It.kind == FrameItem::Action) Queue.push_back({ Clip.weak_from_this(), It.code, ModuleOf(Clip) });
        for (auto& It : Fr.items)
        {
            if (It.kind == FrameItem::Place) Place(Clip, It);
            else if (It.kind == FrameItem::Remove) Clip.children.erase(It.depth);
        }
    }

    int Player::FindFrame(const Instance& Clip, const Value& T) const
    {
        const Character& Ch = Clip.ch ? *Clip.ch : Mods.front()->MovieChar;
        if (T.t == Value::Str)
        {
            auto L = Ch.labels.find(T.s);
            if (L != Ch.labels.end()) return L->second;
            const double N = T.ToNum();
            return std::isnan(N) ? Clip.frame : int(N) - 1;
        }
        const double N = T.ToNum();
        return std::isnan(N) ? Clip.frame : int(N) - 1;
    }

    void Player::GotoFrame(Instance& Clip, int Target, bool Play)
    {
        const Character& Ch = Clip.ch ? *Clip.ch : Mods.front()->MovieChar;
        Clip.playing = Play;
        if (Ch.frames.empty()) return;
        Target = std::max(0, std::min(Target, int(Ch.frames.size()) - 1));
        if (Target == Clip.frame) return;
        if (Target > Clip.frame)
        {
            for (int f = Clip.frame + 1; f <= Target; f++) ApplyFrame(Clip, f, f == Target);
            return;
        }

        // Backwards: rebuild the display list as of the target frame, keeping instances
        // that are still there (with their script state).
        std::map<int, FrameItem> Want;
        for (int f = 0; f <= Target; f++)
            for (auto& It : Ch.frames[size_t(f)].items)
            {
                if (It.kind == FrameItem::Remove) { Want.erase(It.depth); continue; }
                if (It.kind != FrameItem::Place) continue;
                auto W = Want.find(It.depth);
                const bool HasChar = It.flags & 2;
                if (W == Want.end() || (HasChar && W->second.character != It.character) || (HasChar && !(It.flags & 1)))
                {
                    if (!HasChar) continue;
                    FrameItem N = It;
                    N.flags |= 2;
                    N.flags &= ~1u;
                    Want[It.depth] = N;
                }
                else
                {
                    FrameItem& N = W->second;
                    if (It.flags & 0x04) { N.matrix = It.matrix; N.flags |= 0x04; }
                    if (It.flags & 0x08) { N.cx = It.cx; N.flags |= 0x08; }
                    if (It.flags & 0x20) { N.name = It.name; N.flags |= 0x20; }
                }
            }
        for (auto It = Clip.children.begin(); It != Clip.children.end();)
        {
            auto W = Want.find(It->first);
            if (W == Want.end() || W->second.character != It->second->charId) It = Clip.children.erase(It);
            else ++It;
        }
        for (auto& [Depth, W] : Want)
        {
            auto E = Clip.children.find(Depth);
            if (E == Clip.children.end()) Place(Clip, W);
            else if (!E->second->scripted)
            {
                E->second->m = (W.flags & 0x04) ? W.matrix : Mat{};
                E->second->cx = (W.flags & 0x08) ? W.cx : Cx{};
            }
        }
        Clip.frame = Target;
        for (auto& It : Ch.frames[size_t(Target)].items)
            if (It.kind == FrameItem::Action) Queue.push_back({ Clip.weak_from_this(), It.code, ModuleOf(Clip) });
    }

    void Player::AdvanceClip(Instance& Clip)
    {
        const Character& Ch = Clip.ch ? *Clip.ch : Mods.front()->MovieChar;
        const bool IsClip = !Clip.ch || Clip.ch->type == CharType::Sprite;
        if (!IsClip) return;
        if (Clip.fresh)
            Clip.fresh = false;
        else if (Clip.playing && Ch.frames.size() > 1)
        {
            if (Clip.frame + 1 >= int(Ch.frames.size())) GotoFrame(Clip, 0, true);
            else ApplyFrame(Clip, Clip.frame + 1, true);
        }
        std::vector<std::shared_ptr<Instance>> Kids;
        for (auto& [Depth, C] : Clip.children) Kids.push_back(C);
        for (auto& C : Kids)
            if (C->parent == &Clip) AdvanceClip(*C);
    }

    void Player::RunQueue()
    {
        for (size_t i = 0; i < Queue.size() && i < 10000; i++)
        {
            auto Clip = Queue[i].clip.lock();
            if (!Clip) continue;
            Ctx C;
            C.regs = &GlobalRegs;
            C.target = Clip;
            C.self = Value::Object(ScriptObject(*Clip));
            C.mod = Queue[i].module;
            Execute(C, Queue[i].code, 0);
        }
        Queue.clear();
    }

    void Player::Step()
    {
        if (!Root)
        {
            Root = std::make_shared<Instance>();
            // The imported movies' class definitions (their init actions; their own timelines
            // are not played), last loaded first so a movie's imports are defined before it.
            for (size_t m = Mods.size(); m-- > 1;)
                for (auto& Fr : Mods[m]->MovieChar.frames)
                    for (auto& It : Fr.items)
                        if (It.kind == FrameItem::InitAction)
                        {
                            Ctx C;
                            C.regs = &GlobalRegs;
                            C.target = Root;
                            C.self = Value::Object(ScriptObject(*Root));
                            C.mod = Mods[m].get();
                            Execute(C, It.code, 0);
                        }
            ApplyFrame(*Root, 0, true);
            InitDone = true;
            Root->fresh = true;
        }
        AdvanceClip(*Root);
        RunQueue();
        ClearFresh(*Root);
    }

    void Player::ClearFresh(Instance& I)
    {
        I.fresh = false;
        for (auto& [Depth, C] : I.children) ClearFresh(*C);
    }

    void Player::Advance(double Seconds)
    {
        Clock += Seconds;
        if (!Root) { Step(); return; }
        Accum += Seconds;
        int Steps = 0;
        while (Accum >= FrameSeconds && Steps < 6)
        {
            Accum -= FrameSeconds;
            Step();
            Steps++;
        }
        if (Accum > FrameSeconds) Accum = 0;
    }

    // ---------------------------------------------------------------- layout & rendering

    // "<B:name>" in a text is an inline button glyph (Skate 3 strings, e.g. "<B:B_hud> Back"); a
    // leading '~' in the name is dropped. Icon = (ascent + descent) square.
    static size_t InlineToken(const std::string& S, size_t At, std::string& Name)
    {
        if (S.compare(At, 3, "<B:") != 0) return 0;
        const size_t End = S.find('>', At + 3);
        if (End == std::string::npos) return 0;
        Name = S.substr(At + 3, End - At - 3);
        if (!Name.empty() && Name[0] == '~') Name.erase(0, 1);
        return End + 1 - At;
    }

    float Player::MeasureText(const BitmapFont& F, const std::string& S, float Scale) const
    {
        float W = 0;
        for (size_t i = 0; i < S.size();)
        {
            std::string Name;
            if (const size_t N = InlineToken(S, i, Name)) { W += (F.Ascent + 6) * Scale; i += N; continue; }
            auto It = F.Glyphs.find((unsigned char)S[i]);
            if (It != F.Glyphs.end()) W += It->second.adv * Scale;
            i++;
        }
        return W;
    }

    float Player::TextWidth(const Instance& I) const
    {
        if (I.ch->fontIndex < 0) return 0;
        const BitmapFont& F = FontList[size_t(I.ch->fontIndex)];
        return MeasureText(F, I.text, I.ch->height / F.EmPixels);
    }

    void Player::Bounds(const Instance& I, const Mat& M, float Out[4], bool& Any) const
    {
        if (!I.ch && &I != Root.get()) return;
        auto Add = [&](float x, float y)
        {
            float X, Y;
            M.Apply(x, y, X, Y);
            if (!Any) { Out[0] = Out[2] = X; Out[1] = Out[3] = Y; Any = true; return; }
            Out[0] = std::min(Out[0], X); Out[1] = std::min(Out[1], Y);
            Out[2] = std::max(Out[2], X); Out[3] = std::max(Out[3], Y);
        };
        const Character* Ch = I.ch;
        if (Ch && (Ch->type == CharType::Shape || Ch->type == CharType::Text))
        {
            float L = Ch->bounds[0], T = Ch->bounds[1], R = Ch->bounds[2], B = Ch->bounds[3];
            if (Ch->type == CharType::Text && I.autoSize) R = L + TextWidth(I) + 4;
            Add(L, T); Add(R, T); Add(L, B); Add(R, B);
            return;
        }
        for (auto& [Depth, C] : I.children)
            Bounds(*C, M * C->m, Out, Any);
    }

    static void Emit(std::vector<DrawCmd>& Out, int Texture, const float Mul[4], const float Add[4], const Vertex* V, size_t N)
    {
        if (Out.empty() || Out.back().Texture != Texture || memcmp(Out.back().Mul, Mul, 16) != 0 || memcmp(Out.back().Add, Add, 16) != 0)
        {
            DrawCmd C;
            C.Texture = Texture;
            memcpy(C.Mul, Mul, 16);
            memcpy(C.Add, Add, 16);
            Out.push_back(std::move(C));
        }
        Out.back().Verts.insert(Out.back().Verts.end(), V, V + N);
    }

    void Player::RenderText(const Instance& I, const Mat& M, const Cx& C, std::vector<DrawCmd>& Out) const
    {
        if (I.ch->fontIndex < 0 || I.text.empty()) return;
        const BitmapFont& F = FontList[size_t(I.ch->fontIndex)];
        const Character& Ch = *I.ch;
        const float S = Ch.height / F.EmPixels;

        float Mul[4], Add[4];
        // TextField.textColor replaces the rgb (the field's alpha stays)
        const uint32_t Rgba = I.hasTextColor ? (Ch.color & 0xFF000000u) | I.textColor : Ch.color;
        const float Col[4] = { ((Rgba >> 16) & 255) / 255.f, ((Rgba >> 8) & 255) / 255.f, (Rgba & 255) / 255.f, (Rgba >> 24) / 255.f };
        for (int k = 0; k < 4; k++) { Mul[k] = C.mul[k] * Col[k]; Add[k] = C.add[k]; }
        if (Mul[3] <= 0.004f && Add[3] <= 0.004f) return;

        // Lines: explicit breaks, then word wrap inside the field.
        const float Inner = Ch.bounds[2] - Ch.bounds[0] - 4;
        std::vector<std::string> Lines;
        {
            std::string Cur;
            for (char Chr : I.text)
            {
                if (Chr == '\r' || Chr == '\n') { if (Ch.multiline) { Lines.push_back(Cur); Cur.clear(); } else Cur += ' '; continue; }
                Cur += Chr;
            }
            Lines.push_back(Cur);
        }
        if (Ch.wordwrap && !I.autoSize)
        {
            std::vector<std::string> Wrapped;
            for (auto& L : Lines)
            {
                std::string Row, Word;
                std::istringstream In(L);
                while (In >> Word)
                {
                    const std::string Try = Row.empty() ? Word : Row + " " + Word;
                    if (!Row.empty() && MeasureText(F, Try, S) > Inner) { Wrapped.push_back(Row); Row = Word; }
                    else Row = Try;
                }
                Wrapped.push_back(Row);
            }
            Lines = std::move(Wrapped);
        }

        const float LineStep = (F.Ascent + 6 + 7) * S;  // ascent + descent + leading
        float Baseline = Ch.bounds[1] + 2 + F.Ascent * S;
        std::vector<Vertex> V;
        std::vector<std::pair<int, std::vector<Vertex>>> Icons;
        for (auto& L : Lines)
        {
            const float W = MeasureText(F, L, S);
            float X = Ch.bounds[0] + 2;
            if (!I.autoSize)
            {
                if (Ch.align == 1) X = Ch.bounds[2] - 2 - W;
                else if (Ch.align == 2) X = (Ch.bounds[0] + Ch.bounds[2]) * 0.5f - W * 0.5f;
            }
            for (size_t Pos = 0; Pos < L.size(); Pos++)
            {
                std::string IconName;
                if (const size_t N = InlineToken(L, Pos, IconName))
                {
                    const float Size = (F.Ascent + 6) * S;
                    const int Tex = InlineIcon ? InlineIcon(IconName) : -1;
                    if (Tex >= 0)
                    {
                        const float y0 = Baseline - F.Ascent * S, x1 = X + Size, y1 = y0 + Size;
                        Vertex Q[4];
                        M.Apply(X, y0, Q[0].x, Q[0].y); Q[0].u = 0; Q[0].v = 0;
                        M.Apply(x1, y0, Q[1].x, Q[1].y); Q[1].u = 1; Q[1].v = 0;
                        M.Apply(x1, y1, Q[2].x, Q[2].y); Q[2].u = 1; Q[2].v = 1;
                        M.Apply(X, y1, Q[3].x, Q[3].y); Q[3].u = 0; Q[3].v = 1;
                        Icons.push_back({ Tex, { Q[0], Q[1], Q[2], Q[0], Q[2], Q[3] } });
                    }
                    X += Size;
                    Pos += N - 1;
                    continue;
                }
                const unsigned char Chr = (unsigned char)L[Pos];
                auto G = F.Glyphs.find(Chr);
                if (G == F.Glyphs.end()) continue;
                const Glyph& g = G->second;
                if (g.w > 0 && g.h > 0)
                {
                    const float x0 = X + g.bx * S, y0 = Baseline - g.by * S, x1 = x0 + g.w * S, y1 = y0 + g.h * S;
                    // the glyph table's (x, y) is the glyph's pen origin (on the baseline) in the sheet
                    const float sx = g.x + g.bx, sy = g.y - g.by;
                    const float u0 = sx / F.TexW, v0 = sy / F.TexH, u1 = (sx + g.w) / F.TexW, v1 = (sy + g.h) / F.TexH;
                    Vertex Q[4];
                    M.Apply(x0, y0, Q[0].x, Q[0].y); Q[0].u = u0; Q[0].v = v0;
                    M.Apply(x1, y0, Q[1].x, Q[1].y); Q[1].u = u1; Q[1].v = v0;
                    M.Apply(x1, y1, Q[2].x, Q[2].y); Q[2].u = u1; Q[2].v = v1;
                    M.Apply(x0, y1, Q[3].x, Q[3].y); Q[3].u = u0; Q[3].v = v1;
                    V.insert(V.end(), { Q[0], Q[1], Q[2], Q[0], Q[2], Q[3] });
                }
                X += g.adv * S;
            }
            Baseline += LineStep;
        }
        if (!V.empty()) Emit(Out, F.Texture, Mul, Add, V.data(), V.size());
        if (!Icons.empty())
        {
            // button glyphs keep their own colours (only the field's alpha / colour transform)
            float IMul[4], IAdd[4];
            for (int k = 0; k < 4; k++) { IMul[k] = C.mul[k]; IAdd[k] = C.add[k]; }
            IMul[3] = C.mul[3] * Col[3];
            for (auto& [Tex, Q] : Icons) Emit(Out, Tex, IMul, IAdd, Q.data(), Q.size());
        }
    }

    void Player::RenderInstance(const Instance& I, const Mat& Parent, const Cx& ParentCx, std::vector<DrawCmd>& Out) const
    {
        if (!I.visible) return;
        const Mat M = Parent * I.m;
        const Cx C = ParentCx * I.cx;
        if (C.mul[3] <= 0.004f && C.add[3] <= 0.004f) return;
        const Character* Ch = I.ch;
        if (Ch && Ch->type == CharType::Shape)
        {
            for (auto& P : Ch->parts)
            {
                int Tex = -1;
                float Tw = 1, Th = 1;
                if (P.tex >= 0)
                {
                    {
                        Tex = P.tex;
                        Tw = float(TextureList[size_t(Tex)].Width);
                        Th = float(TextureList[size_t(Tex)].Height);
                    }
                }
                float Mul[4], Add[4];
                for (int k = 0; k < 4; k++) { Mul[k] = C.mul[k] * P.color[k]; Add[k] = C.add[k]; }
                std::vector<Vertex> V(P.xy.size() / 2);
                for (size_t k = 0; k < V.size(); k++)
                {
                    const float x = P.xy[2 * k], y = P.xy[2 * k + 1];
                    M.Apply(x, y, V[k].x, V[k].y);
                    V[k].u = (P.uvm[0] * x + P.uvm[2] * y + P.uvt[0]) / Tw;
                    V[k].v = (P.uvm[1] * x + P.uvm[3] * y + P.uvt[1]) / Th;
                }
                Emit(Out, Tex, Mul, Add, V.data(), V.size());
            }
            return;
        }
        if (Ch && Ch->type == CharType::Text)
        {
            RenderText(I, M, C, Out);
            return;
        }
        for (auto& [Depth, Child] : I.children)
            RenderInstance(*Child, M, C, Out);
    }

    void Player::Render(std::vector<DrawCmd>& Out) const
    {
        Out.clear();
        if (Root) RenderInstance(*Root, Mat{}, Cx{}, Out);
    }

    bool ToQuads(const DrawCmd& Cmd, std::vector<Quad>& Out)
    {
        const std::vector<Vertex>& V = Cmd.Verts;
        if (V.size() % 6 != 0) return false;
        bool Ok = true;
        for (size_t i = 0; i < V.size(); i += 6)
        {
            // The rectangle's four distinct corners.
            Vertex C[4];
            int N = 0;
            for (size_t k = i; k < i + 6 && N < 4; k++)
            {
                bool Seen = false;
                for (int j = 0; j < N; j++)
                    if (std::fabs(C[j].x - V[k].x) < 1e-3f && std::fabs(C[j].y - V[k].y) < 1e-3f) Seen = true;
                if (!Seen) C[N++] = V[k];
            }
            if (N != 4) { Ok = false; continue; }

            // Pick the corner (with its two edges, e2 on the +y side of e1) whose edges carry
            // u and v separately, and whose rotation is smallest.
            bool Found = false;
            Quad Best{};
            for (int r = 0; r < 4; r++)
            {
                // The opposite corner is the farthest; the other two are the edges' ends.
                int Far = -1;
                float FarD = -1;
                for (int k = 0; k < 4; k++)
                {
                    if (k == r) continue;
                    const float D = (C[k].x - C[r].x) * (C[k].x - C[r].x) + (C[k].y - C[r].y) * (C[k].y - C[r].y);
                    if (D > FarD) { FarD = D; Far = k; }
                }
                int A = -1, B = -1;
                for (int k = 0; k < 4; k++)
                    if (k != r && k != Far) (A < 0 ? A : B) = k;
                Vertex R = C[r], P1 = C[A], P2 = C[B];
                float e1x = P1.x - R.x, e1y = P1.y - R.y, e2x = P2.x - R.x, e2y = P2.y - R.y;
                if (e1x * e2y - e1y * e2x < 0)
                {
                    std::swap(P1, P2);
                    std::swap(e1x, e2x);
                    std::swap(e1y, e2y);
                }
                if (std::fabs(P1.v - R.v) > 1e-4f || std::fabs(P2.u - R.u) > 1e-4f) continue;
                const float W = std::sqrt(e1x * e1x + e1y * e1y), H = std::sqrt(e2x * e2x + e2y * e2y);
                if (W < 1e-4f || H < 1e-4f) continue;
                Quad Q;
                const float Cx = R.x + (e1x + e2x) * 0.5f, Cy = R.y + (e1y + e2y) * 0.5f;
                Q.x = Cx - W * 0.5f;
                Q.y = Cy - H * 0.5f;
                Q.w = W;
                Q.h = H;
                Q.Rotation = std::atan2(e1y, e1x) * 57.29577951308232f;
                Q.u = R.u; Q.v = R.v; Q.du = P1.u - R.u; Q.dv = P2.v - R.v;
                if (!Found || std::fabs(Q.Rotation) < std::fabs(Best.Rotation)) Best = Q;
                Found = true;
            }
            if (Found) Out.push_back(Best);
            else Ok = false;
        }
        return Ok;
    }
}
