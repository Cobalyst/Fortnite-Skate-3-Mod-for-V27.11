#pragma once

// Player for Skate 3 APT movies (EA's Flash derivative, big-endian "Apt Data:1:8:4").
// Loads <name>.const/.apt/.geo, plays the timeline at the movie's frame rate and runs the
// movie's own ActionScript bytecode (opcode set as documented by OpenSAGE). Independent of
// Unreal: Render() produces textured triangle batches in stage pixels, which the HUD draws
// on the game canvas (and Tools' test harness rasterises to PNG).

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Apt
{
    // 2D affine transform: x' = a*x + c*y + tx, y' = b*x + d*y + ty.
    struct Mat
    {
        float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
        void Apply(float x, float y, float& ox, float& oy) const { ox = a * x + c * y + tx; oy = b * x + d * y + ty; }
        Mat operator*(const Mat& r) const;  // this ∘ r (r applied first)
    };

    // Flash colour transform: out = in * mul + add (rgba, 0..1).
    struct Cx
    {
        float mul[4] = { 1, 1, 1, 1 };
        float add[4] = { 0, 0, 0, 0 };
        Cx operator*(const Cx& child) const;  // parent ∘ child
    };

    struct Vertex { float x, y, u, v; };

    // One batch of triangles sharing a texture (-1 = untextured) and colour transform.
    struct DrawCmd
    {
        int Texture = -1;
        float Mul[4];
        float Add[4];
        std::vector<Vertex> Verts;  // 3 per triangle, stage pixels
    };

    // A textured rectangle for APIs that draw rotated quads (Unreal's UCanvas::K2_DrawTexture):
    // (x, y, w, h) is the rectangle before rotation, rotated by Rotation degrees (clockwise on
    // screen, y down) about its centre; (u, v, du, dv) the texture rectangle mapped onto it.
    struct Quad { float x, y, w, h, Rotation, u, v, du, dv; };

    // Splits a batch into quads (every 6 vertices = two triangles of one rectangle). False if
    // some pair is not a rectangle with axis-aligned texture mapping.
    bool ToQuads(const DrawCmd& Cmd, std::vector<Quad>& Out);

    // ---- movie data ----
    enum class CharType : uint32_t { Shape = 1, Text = 2, Font = 3, Button = 4, Sprite = 5, Sound = 6, Image = 7, Morph = 8, Movie = 9, StaticText = 10 };

    struct FrameItem
    {
        enum Kind { Action = 1, Label = 2, Place = 3, Remove = 4, Background = 5, InitAction = 8 } kind;
        uint32_t code = 0;     // Action / InitAction: bytecode offset
        std::string name;      // Label / Place instance name
        uint32_t flags = 0;    // Place
        int depth = 0;         // Place / Remove
        int character = -1;    // Place
        Mat matrix;            // Place
        Cx cx;                 // Place
        float ratio = 0;       // Place
        // Place with flag 0x80: onClipEvent handlers (SWF event flags as a little-endian number:
        // 0x01 load, 0x200 initialize, 0x40000 construct) and their bytecode offsets
        std::vector<std::pair<uint32_t, uint32_t>> clipEvents;
    };

    struct Frame { std::vector<FrameItem> items; };

    struct GeoPart
    {
        float color[4];
        int image = 0;           // Image character id (0 = untextured)
        int tex = -1;            // its texture (index into Textures()), resolved at load
        float uvm[4], uvt[2];    // texture pixels = uvm·p + uvt
        std::vector<float> xy;   // 2 floats per vertex, 3 vertices per triangle
    };

    struct Module;

    struct Character
    {
        CharType type{};
        Module* module = nullptr; // the movie file the character belongs to
        float bounds[4] = {};    // left, top, right, bottom
        // Shape
        std::vector<GeoPart> parts;
        // Text
        int font = 0, align = 0;
        uint32_t color = 0xFFFFFFFF;
        float height = 0;
        bool multiline = false, wordwrap = false;
        std::string text;
        int fontIndex = -1;      // Text: its bitmap font (Player font list), resolved at load
        // Font
        std::string fontName;
        // Sprite / Movie
        std::vector<Frame> frames;
        std::unordered_map<std::string, int> labels;
        // Image
        int texture = -1;        // index into Movie::Textures
    };

    struct Glyph { float x, y, w, h, bx, by, adv; };

    struct BitmapFont
    {
        int Texture = -1;  // index into Movie::Textures
        float TexW = 512, TexH = 512;
        float EmPixels = 37.333f;  // the bitmap's em size (Size pt at 96 dpi)
        float Ascent = 37;
        std::unordered_map<uint32_t, Glyph> Glyphs;
        bool Load(const std::string& TxtPath);
        float Width(const std::string& S, float Scale) const;
    };

    struct TextureInfo { std::string Path; int Width = 0, Height = 0; };

    // ---- script values ----
    struct Object;
    struct Instance;
    class Player;
    using ObjRef = std::shared_ptr<Object>;

    struct Value
    {
        enum Type : uint8_t { Undef, Null, Bool, Num, Str, Obj } t = Undef;
        bool b = false;
        double n = 0;
        std::string s;
        ObjRef o;

        Value() = default;
        static Value Boolean(bool v) { Value r; r.t = Bool; r.b = v; return r; }
        static Value Number(double v) { Value r; r.t = Num; r.n = v; return r; }
        static Value String(std::string v) { Value r; r.t = Str; r.s = std::move(v); return r; }
        static Value Object(ObjRef v) { Value r; if (v) { r.t = Obj; r.o = std::move(v); } return r; }
        static Value NullValue() { Value r; r.t = Null; return r; }

        bool ToBool() const;
        double ToNum() const;
        std::string ToStr() const;
    };

    using Args = std::vector<Value>;
    using Native = std::function<Value(Player&, const Value& self, Args& args)>;

    struct Function
    {
        uint32_t start = 0, end = 0;
        bool v2 = false;
        uint8_t registers = 0;
        uint16_t flags = 0;
        std::vector<std::pair<int, std::string>> params;  // (register or 0, name)
        std::vector<Value> pool;                         // constant pool at definition
        std::weak_ptr<Instance> target;                  // clip the function was defined in
        Module* module = nullptr;                        // movie file holding the bytecode
        Native native;
    };

    struct Object
    {
        std::unordered_map<std::string, Value> props;
        ObjRef proto;
        std::unique_ptr<Function> fn;
        std::weak_ptr<Instance> inst;  // set when this is a display object's script object
        bool isArray = false;
    };

    // One movie file (.const/.apt/.geo): the main movie or one it imports characters from
    // (objectdropper imports its menu, tabs and scrollbar from source/controls/*).
    struct Module
    {
        std::string Name;
        std::vector<uint8_t> Data;      // .apt.bin (bytecode offsets are into this)
        std::vector<Value> Consts;
        uint32_t AptOffset = 0;
        std::unordered_map<int, Character> Chars;
        Character MovieChar;
        std::unordered_map<std::string, int> Exports;                  // linkage name -> character id
        std::unordered_map<int, const Character*> Imported;           // character id -> another movie's character
        std::vector<std::pair<int, std::pair<std::string, std::string>>> Imports;  // id, (movie, name)
        std::string TextureDir;
    };

    // A placed character on the display list.
    // Clips made by script (attachMovie / createTextField) live in their own depth range above the
    // timeline's, as in Flash (where timeline depths are offset by -16384): sk8popup creates its
    // body text at depth 1, where its timeline has mFrameBottom.
    constexpr int ScriptDepth = 1 << 20;

    struct Instance : std::enable_shared_from_this<Instance>
    {
        const Character* ch = nullptr;
        int charId = -1;
        Instance* parent = nullptr;
        int depth = 0;
        std::string name;
        Mat m;
        Cx cx;
        bool visible = true;
        bool scripted = false;  // a script moved it: the timeline no longer does
        // Sprite / movie
        int frame = -1;
        bool playing = true;
        bool fresh = false;     // placed during this tick (does not advance until the next)
        std::map<int, std::shared_ptr<Instance>> children;  // by depth
        // Text
        std::string text;
        bool autoSize = false;
        uint32_t textColor = 0;  // TextField.textColor (rgb) when set
        bool hasTextColor = false;
        ObjRef obj;              // script object (created on demand)
        // A text field made by createTextField, or one whose format a script changed
        // (setTextFormat): its own copy of the text character.
        std::shared_ptr<Character> ownCh;

        std::shared_ptr<Instance> Child(const std::string& Name) const;
    };

    class Player
    {
    public:
        // Dir/<Name>.const/.apt/.geo (decompressed .bin files); textures are
        // TextureDir/<image id>.png; Fonts maps the APT font name to a bitmap font .txt
        // (its .png beside it).
        bool Load(const std::string& Dir, const std::string& Name, const std::string& TextureDir,
                  const std::unordered_map<std::string, std::string>& Fonts);
        const std::string& Error() const { return Err; }

        // Translates "ID_..." text keys; empty result keeps the key.
        std::function<std::string(const std::string&)> Localize;
        std::function<void(const std::string&)> LogLine;
        // Inline button glyphs in texts ("<B:B_hud>"): texture index to draw (the host's textures
        // follow Textures()), or -1 to leave a gap.
        std::function<int(const std::string&)> InlineIcon;
        // Movies that characters are imported from: movie name (the last part of the import's
        // path, "source/controls/tabs" -> "tabs") -> (directory of its .bin files, texture directory).
        // Loaded with the main movie; their class definitions run before the main movie's.
        std::unordered_map<std::string, std::pair<std::string, std::string>> ImportDirs;
        // Characters imported from movies not in ImportDirs are placed as empty sprites with their
        // names, so scripts can still address them (hudphonelist's button items).
        bool PlaceImports = false;

        // Plays Seconds of the movie (fixed steps at the movie's frame rate).
        void Advance(double Seconds);
        void Render(std::vector<DrawCmd>& Out) const;
        // Diagnostics: display objects in the tree and frame actions waiting (both should stay bounded).
        size_t InstanceCount() const;
        size_t QueuedCount() const { return Queue.size(); }

        const std::vector<TextureInfo>& Textures() const { return TextureList; }
        float StageWidth() const { return float(Width); }
        float StageHeight() const { return float(Height); }

        // Script access for the host.
        ObjRef Global() const { return GlobalObj; }
        ObjRef ObjectPrototype() const { return ObjectProto; }
        // The root timeline's script object (frame-action locals such as "screen" live on it).
        Value RootObject();
        ObjRef NewObject();
        ObjRef NewArray(const Args& Items);
        Value NewNative(Native Fn);
        Value Get(const Value& Obj, const std::string& Name);
        void Set(const Value& Obj, const std::string& Name, const Value& V);
        Value Call(const Value& Fn, const Value& This, Args A);
        Value CallMethod(const Value& Obj, const std::string& Name, Args A);
        int Errors() const { return ScriptErrors; }

    private:
        struct Ctx;

        // loading (into the module M)
        Module* LoadModule(const std::string& Dir, const std::string& Name, const std::string& TextureDir);
        bool LoadConst(const std::string& Path);
        bool LoadApt(const std::string& Path);
        bool LoadGeo(const std::string& Path);
        bool LoadAssets();
        int LoadFont(const std::string& Name);   // FontList index of a Fonts entry (loaded once), -1 if unknown
        Character& OwnText(Instance& I);          // the instance's own text character (copied on first change)
        void ApplyTextFormat(Instance& I, const Value& Format);
        const Character* FindExport(Module* In, const std::string& Name) const;
        Module* ModuleOf(const Instance& I) const;
        std::shared_ptr<Instance> Create(Instance& Parent, const Character* Ch, int CharId, int Depth, const std::string& Name,
                                         const Mat& Placed = {}, const Cx& Tint = {});
        void Construct(Instance& I);
        std::vector<Frame> ReadFrames(uint32_t Count, uint32_t Ptr);
        uint32_t U32(uint32_t o) const;
        float F32(uint32_t o) const;
        std::string CStr(uint32_t o) const;
        Value ConstValue(uint32_t Index) const;

        // timeline
        std::shared_ptr<Instance> Place(Instance& Parent, const FrameItem& It);
        void ApplyFrame(Instance& Clip, int Frame, bool Actions);
        void GotoFrame(Instance& Clip, int Frame, bool Play);
        int FindFrame(const Instance& Clip, const Value& Target) const;
        void AdvanceClip(Instance& Clip);
        void Step();
        void ClearFresh(Instance& I);
        void RunQueue();
        void Bounds(const Instance& I, const Mat& M, float Out[4], bool& Any) const;
        float TextWidth(const Instance& I) const;
        float MeasureText(const BitmapFont& F, const std::string& S, float Scale) const;

        // script
        ObjRef ScriptObject(Instance& I);
        Value Lookup(Ctx& C, const std::string& Name);
        Value Execute(Ctx& C, uint32_t Pc, uint32_t End);
        Value GetProp(Instance& I, const std::string& Name, bool& Found);
        bool SetProp(Instance& I, const std::string& Name, const Value& V);
        void SetText(Instance& I, const Value& V);
        Value MakeFunction(Ctx& C, uint32_t Start, uint32_t End, bool V2, uint8_t Regs, uint16_t Flags,
                           std::vector<std::pair<int, std::string>> Params);
        void InstallBuiltins();
        void Fail(const char* Fmt, ...);

        // rendering
        void RenderInstance(const Instance& I, const Mat& M, const Cx& C, std::vector<DrawCmd>& Out) const;
        void RenderText(const Instance& I, const Mat& M, const Cx& C, std::vector<DrawCmd>& Out) const;

        std::vector<std::unique_ptr<Module>> Mods;    // [0] = the main movie, then its imports
        Module* M = nullptr;                          // module being loaded / whose bytecode runs
        std::unordered_map<std::string, std::string> FontPaths;
        std::vector<TextureInfo> TextureList;
        std::vector<BitmapFont> FontList;
        std::unordered_map<std::string, int> FontByName;
        std::unordered_map<const Character*, Value> Classes;  // Object.registerClass
        int Width = 1280, Height = 720;
        double FrameSeconds = 1.0 / 60.0;
        double Clock = 0;
        double Accum = 0;

        std::shared_ptr<Instance> Root;
        ObjRef GlobalObj, ObjectProto, MovieClipProto, FunctionProto, ArrayProto;
        struct Queued { std::weak_ptr<Instance> clip; uint32_t code; Module* module; };
        std::vector<Queued> Queue;  // frame actions to run
        std::vector<Value> GlobalRegs = std::vector<Value>(4);
        std::string Err;
        int ScriptErrors = 0;
        bool InitDone = false;
    };
}
