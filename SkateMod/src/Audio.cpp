#include "Audio.h"
#include "Common.h"
#include "Aems.h"
#include "SkaterAudio.h"
#include "Grain.h"

#include "../../third_party/miniaudio/miniaudio.h"

#include <array>
#include <memory>
#include <algorithm>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Board sounds come from Skate 3's own sound engine (Aems.cpp): the real AEMS modules of the
// skateboard banks, driven with the class values the game computes (decompiled from default.xex):
//   Class_rolling   FUN_824b2a70 / 824b49c8   speed = clamp(kmh / 45) * 10000
//   Class_grind     FUN_8249d7c0 / 824b1838   speed = clamp((m/s - 0.5) / 45 * 3.6) * 10000, max 9000
//   Class_wheels_skid FUN_8249d570 / 824b5228 speed = clamp(m/s * 0.08) * 10000, angle 0..90
//   SenseOfSpeed_wind / _rattle FUN_824d5480  value = clamp((kmh - lo) / (hi - lo)) * 1000, EQ chain 7
//   Class_Seams     FUN_824af268              one instance per wheel
//   Class_Flips     FUN_8249d9d0
// Board and body impacts (pop, landing, grind contact, body hits) are Skate 3's own collision sound
// system and board audio logic, ported in SkaterAudio.cpp. HUD cues are single Skate 3 samples
// (skate-data/sounds/sounds.txt).
namespace
{
    const std::string ConfigPath = ModPath("skate-data\\sounds\\sounds.txt");
    const std::string AemsRoot = ModPath("skate-data\\aems");

    ma_engine Engine;
    std::atomic<bool> bReady{ false };
    bool bTried = false, bActive = false;
    std::string Folder;
    float MasterVolume = 1.f;
    std::unordered_map<std::string, std::vector<std::string>> Events;
    std::mt19937 Rng{ 1234 };

    float Random(float Lo, float Hi) { return std::uniform_real_distribution<float>(Lo, Hi)(Rng); }

    // ---- Skate 3 SPLC banks (one-shot collision sounds, e.g. Skate_Collisions.bnk) ----
    // Header "SPLC", version 3, data offset, event count, group count, 0, sample count (big endian).
    // Events (0x24 bytes from 0x40): u16 id, u16 layer count, volume, pitch, pitch spread, ...
    // Groups (0x48 bytes) list up to 32 event ids, one picked at random. Ids >= event count are
    // groups. Then each event's layers in order: u16 1, u16 0, u8 n, u8 mode, u16 0, n entries of
    // 0x48 bytes (u16 sample, u16 0, 17 floats: [0] gain, [4] delay seconds, ...), u32 0. Every
    // layer plays one of its entries; layers sound together. Samples are decoded to
    // skate-data/sounds/full/<bank>_NNN.wav (tools/carve_splc.py, same order as the bank).
    const std::string SplcRoot = ModPath("skate-data\\extracted\\audio_all");
    const std::string SplcWavs = ModPath("skate-data\\sounds\\full");
    struct SplcEntry { uint16_t Sample; float Gain, Delay; };
    struct SplcEvent { float Volume = 1, Pitch = 1, Spread = 0; std::vector<std::vector<SplcEntry>> Layers; };
    struct SplcBank { std::string Name; std::vector<SplcEvent> Events; std::vector<std::vector<uint16_t>> Groups; };
    std::unordered_map<std::string, SplcBank> SplcBanks;
    struct SplcCue { std::string Bank; std::vector<int> Ids; float Gain = 1; };
    std::unordered_map<std::string, std::vector<SplcCue>> SplcCues;  // sounds.txt: splc <cue> <bank> <gain> <id> ...
    // Collision banks per audio material kind (table 0x82FD1930): 0, 1, 2.
    const char* const CollisionBanks[3] = { "Skate_Collisions", "Skate_Metal", "HOM_Set_1" };
    bool bSkaterAudio = false;

    uint32_t Be32(const uint8_t* P) { return uint32_t(P[0]) << 24 | uint32_t(P[1]) << 16 | uint32_t(P[2]) << 8 | P[3]; }
    uint16_t Be16(const uint8_t* P) { return uint16_t(P[0] << 8 | P[1]); }
    float BeF(const uint8_t* P) { const uint32_t U = Be32(P); float F; memcpy(&F, &U, 4); return F; }

    const SplcBank* LoadSplc(const std::string& Name)
    {
        auto It = SplcBanks.find(Name);
        if (It != SplcBanks.end()) return It->second.Events.empty() ? nullptr : &It->second;
        SplcBank& B = SplcBanks[Name];
        B.Name = Name;
        std::ifstream In(std::string(SplcRoot) + "\\" + Name + ".bnk", std::ios::binary);
        if (!In) { Log("Audio: SPLC bank %s not found", Name.c_str()); return nullptr; }
        In.seekg(0, std::ios::end);
        const size_t Size = size_t(In.tellg());
        if (Size < 0x40) return nullptr;
        In.seekg(0);
        std::vector<uint8_t> D(Size);
        In.read(reinterpret_cast<char*>(D.data()), std::streamsize(Size));
        if (memcmp(D.data(), "SPLC", 4) != 0) return nullptr;
        const uint32_t Data = Be32(&D[8]), NEv = Be32(&D[12]), NGr = Be32(&D[16]);
        if (0x40 + size_t(NEv) * 0x24 + size_t(NGr) * 0x48 > Data || Data > Size) return nullptr;
        std::vector<int> LayerCount(NEv);
        B.Events.resize(NEv);
        for (uint32_t i = 0; i < NEv; i++)
        {
            const uint8_t* E = &D[0x40 + i * 0x24];
            LayerCount[i] = Be16(E + 2);
            B.Events[i].Volume = BeF(E + 4);
            B.Events[i].Pitch = BeF(E + 8);
            B.Events[i].Spread = BeF(E + 12);
        }
        const size_t GroupBase = 0x40 + size_t(NEv) * 0x24 - 8;
        B.Groups.resize(NGr);
        for (uint32_t g = 0; g < NGr; g++)
            for (int k = 0; k < 32; k++)
            {
                const uint16_t Id = Be16(&D[GroupBase + g * 0x48 + 8 + k * 2]);
                if (Id != 0xffff) B.Groups[g].push_back(Id);
            }
        size_t O = GroupBase + size_t(NGr) * 0x48 + 8;
        for (uint32_t i = 0; i < NEv; i++)
            for (int l = 0; l < LayerCount[i]; l++)
            {
                if (O + 8 > Data) { Log("Audio: SPLC %s layers overrun", Name.c_str()); B.Events.clear(); return nullptr; }
                const int N = D[O + 4];
                std::vector<SplcEntry> Layer;
                for (int k = 0; k < N && O + 8 + size_t(k + 1) * 0x48 <= Data; k++)
                {
                    const uint8_t* P = &D[O + 8 + k * 0x48];
                    Layer.push_back({ Be16(P), BeF(P + 4), BeF(P + 4 + 4 * 4) });
                }
                if (!Layer.empty()) B.Events[i].Layers.push_back(std::move(Layer));
                O += 8 + size_t(N) * 0x48 + 4;
            }
        Log("Audio: SPLC %s: %u events, %u groups", Name.c_str(), NEv, NGr);
        return &B;
    }

    // ---- Skate 3 sound engine ----
    Aems::Engine Snd;
    // Grain rolling: two tracks (front / back truck wheels), each two grain players (0x824B3A98).
    Grain::Player RollPlayers[2][2];
    bool LoadRolling();
    ma_device Device;
    bool bAems = false;

    void Mix(ma_device* D, void* Out, const void*, ma_uint32 Frames)
    {
        Snd.Mix(static_cast<float*>(Out), Frames, D->sampleRate);
        float* F = static_cast<float*>(Out);
        for (auto& Track : RollPlayers)
            for (Grain::Player& Pl : Track) Pl.Mix(F, Frames, D->sampleRate);
        for (uint32_t i = 0; i < Frames * 2; i++) F[i] = std::clamp(F[i], -1.f, 1.f);
    }

    void AemsLog(const char* S) { Log("%s", S); }

    // One running sound class instance.
    struct Sound
    {
        const char* Class;
        int Id = 0;
        std::vector<int32_t> Values;
        void Set(const std::vector<int32_t>& V)
        {
            Values = V;
            if (Id && Snd.Alive(Id)) Snd.Update(Id, Values);
            else Id = Snd.Create(Class, Values);
        }
        void Stop()
        {
            if (Id) Snd.Destroy(Id);
            Id = 0;
        }
    };
    Sound Rolling{ "Class_rolling" }, Grind{ "Class_grind" }, Skid{ "Class_wheels_skid" };
    Sound Footstep{ "playercharacter_footstep" };
    Sound Wind{ "SenseOfSpeed_wind" }, SpeedRattle{ "SenseOfSpeed_rattle" };
    std::array<Sound, 4> Seams{ Sound{ "Class_Seams" }, Sound{ "Class_Seams" }, Sound{ "Class_Seams" }, Sound{ "Class_Seams" } };
    struct Flip { int Id; float Age; };
    std::vector<Flip> Flips;

    // The local skater is right in front of the camera: the emitter values the game would compute
    // for it (vtable +0x34/+0x38/+0x3c/+0x40) are close to neutral.
    constexpr int32_t EmGain = 0x7fff, EmPan = 0, EmPitch = 0x1000, EmLpf = 25000, EmHpf = 0;

    int32_t Clamp01(float X, float Scale) { return int32_t(std::fmin(1.f, std::fmax(0.f, X)) * Scale); }

    // Per-surface tuning (AttribSys class 0x049861E8F9A8D16B). Fortnite has no Skate surfaces, so
    // everything uses the game's own fallback surface (index 4).
    constexpr int Surface = 4;
    constexpr float GrindLevel = 0.56f;      // Hash_58070BF511809903 of surface 4
    constexpr float GrindGainKind2 = 1.0f;   // Hash_C21983A2160ED3AC
    constexpr int RollingSurface = 2;        // Class_rolling surface with the plain concrete loops

    void LoadBanks()
    {
        const char* Banks[] = { "Common", "GRINDS", "PatchBank_Rolling_Surfaces", "Rolling_Rattles", "Seams_Bank",
            "Sk8_Air_Flip_Tricks", "WHEEL_SKID_BANK", "Brd_Squeaks", "FOOT_DRAG", "Bodyslide", "board_scrapes",
            "sense_of_speed", "Foley_Cloth", "fstep_skateshoe1_sm" };
        Snd.SetLog(AemsLog);
        int Loaded = 0;
        for (const char* B : Banks) Loaded += Snd.LoadBank(AemsRoot.c_str(), B) ? 1 : 0;
        if (!Loaded) return;
        ma_device_config Cfg = ma_device_config_init(ma_device_type_playback);
        Cfg.playback.format = ma_format_f32;
        Cfg.playback.channels = 2;
        Cfg.sampleRate = 48000;
        Cfg.dataCallback = Mix;
        if (ma_device_init(nullptr, &Cfg, &Device) != MA_SUCCESS || ma_device_start(&Device) != MA_SUCCESS)
        {
            Log("Audio: could not open the sound device for the Skate 3 sound engine");
            return;
        }
        Snd.MasterGain = 0.7f * MasterVolume;  // mix headroom
        bAems = true;
        // What the game runs from start-up: global controllers and the foley sample rotation.
        for (const char* C : { "PC_Foley_Spins_vol", "PC_Foley_kickflip_vol", "PC_Foley_Ollie_vol", "PC_Foley_Popshuvs_vol" })
            Snd.SetController(C, 0x7fff);
        Snd.Create("Start_up_Play_ctl", {});
        Snd.Create("c_foley_utility", {});
        Log("Audio: Skate 3 sound engine ready (%d banks)", Loaded);
    }

    // ---- one-shots (sounds.txt) ----
    struct Shot { ma_sound Sound{}; bool bUsed = false; };
    std::array<Shot, 64> Shots;
    // Keeps decoded one-shot files cached. Never played, not attached to the mixer, and never
    // freed (they live as long as the engine), so the audio thread can't touch freed memory.
    std::vector<ma_sound*> Pinned;

    void PinFile(const std::string& Path)
    {
        ma_sound* S = new ma_sound{};
        if (ma_sound_init_from_file(&Engine, Path.c_str(), MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT,
                                    nullptr, nullptr, S) == MA_SUCCESS)
            Pinned.push_back(S);
        else
            delete S;
    }

    enum class EWhat { Other, Ground, Slide, Air, Grinding, Wipeout, OffBoard };
    EWhat Last = EWhat::Other;
    float LastVz = 0;
    float SeamDistance = 0;
    struct PendingSeam { float Time; int Wheel; };
    std::vector<PendingSeam> PendingSeams;

    EWhat Classify(const char* State)
    {
        const std::string S = State;
        if (S.rfind("Grind", 0) == 0)              return EWhat::Grinding;
        if (S == "SlideGround")                    return EWhat::Slide;
        if (S == "WipeoutGround")                  return EWhat::Wipeout;
        if (S == "BipedGround" || S == "OffBoardPushing") return EWhat::OffBoard;
        if (S.find("Air") != std::string::npos)    return EWhat::Air;
        if (S == "PhysicsGround" || S == "RevertGround" || S == "GroundAnimation" || S == "LandingOnDeck" ||
            S == "Skitching" || S == "FollowPath")  return EWhat::Ground;
        return EWhat::Other;
    }

    bool LoadConfig()
    {
        std::ifstream In(ConfigPath);
        if (!In) { Log("Audio: %s not found (run Tools/pick_sounds.py)", ConfigPath.c_str()); return false; }
        std::string Line;
        while (std::getline(In, Line))
        {
            std::istringstream L(Line);
            std::string Key;
            L >> Key;
            if (Key.empty()) continue;
            if (Key == "folder")   // relative to skate-data\sounds (an absolute path still works)
            {
                std::getline(L >> std::ws, Folder);
                if (!(Folder.size() > 1 && (Folder[1] == ':' || Folder[0] == '\\' || Folder[0] == '/'))) Folder = ModPath("skate-data\\sounds\\") + Folder;
                continue;
            }
            if (Key == "volume") { L >> MasterVolume; continue; }
            if (Key == "ground_material") { int M = 2; L >> M; SkaterAudio::SetGroundMaterial(M); continue; }
            if (Key == "splc")  // splc <cue> <bank> <gain> <event or group id> ...  (all ids sound together)
            {
                std::string Cue;
                SplcCue C;
                L >> Cue >> C.Bank >> C.Gain;
                int Id;
                while (L >> Id) C.Ids.push_back(Id);
                if (!C.Ids.empty()) SplcCues[Cue].push_back(C);
                continue;
            }
            std::string File;
            auto& List = Events[Key];
            while (L >> File) List.push_back(File);
        }
        return !Folder.empty();
    }

    std::string Pick(const char* Event)
    {
        auto It = Events.find(Event);
        if (It == Events.end() || It->second.empty()) return {};
        const auto& List = It->second;
        return Folder + "\\" + List[std::uniform_int_distribution<size_t>(0, List.size() - 1)(Rng)];
    }

    // Decodes every file a one-shot cue can play (on the loading thread), so playing them later
    // never touches the disk.
    void CollectSplc(const SplcBank& B, int Id, std::vector<std::string>& Out, int Depth = 0)
    {
        const int NEv = int(B.Events.size());
        if (Id >= NEv)
        {
            const int G = Id - NEv;
            if (G < int(B.Groups.size()) && Depth < 4)
                for (int E : B.Groups[G]) CollectSplc(B, E, Out, Depth + 1);
            return;
        }
        if (Id < 0) return;
        char Path[512];
        for (const auto& Layer : B.Events[Id].Layers)
            for (const auto& X : Layer)
            {
                snprintf(Path, sizeof(Path), "%s\\%s_%03u.wav", SplcWavs.c_str(), B.Name.c_str(), unsigned(X.Sample));
                Out.push_back(Path);
            }
    }

    void PreloadOneShots()
    {
        std::vector<std::string> Files;
        for (auto& [Cue, List] : SplcCues)
            for (auto& C : List)
            {
                auto B = SplcBanks.find(C.Bank);
                if (B != SplcBanks.end())
                    for (int Id : C.Ids) CollectSplc(B->second, Id, Files);
            }
        for (auto& [Key, List] : Events)
            for (auto& F : List) Files.push_back(Folder + "\\" + F);
        // Every sample of the collision banks (the ported collision system can pick any of them)
        // and of the footstep cloth foley.
        for (const char* Name : { CollisionBanks[0], CollisionBanks[1], CollisionBanks[2], "sk8_foley" })
        {
            auto B = SplcBanks.find(Name);
            if (B == SplcBanks.end()) continue;
            for (int Id = 0; Id < int(B->second.Events.size()); Id++) CollectSplc(B->second, Id, Files);
        }
        std::sort(Files.begin(), Files.end());
        Files.erase(std::unique(Files.begin(), Files.end()), Files.end());
        for (auto& F : Files) PinFile(F);
        Log("Audio: %zu one-shot files decoded and cached", Pinned.size());
    }

    bool Init()
    {
        if (bTried) return bReady;
        bTried = true;
        LoadConfig();
        if (ma_engine_init(nullptr, &Engine) != MA_SUCCESS)
        {
            Log("Audio: could not open the sound device");
            return false;
        }
        ma_engine_set_volume(&Engine, MasterVolume);
        LoadBanks();
        for (auto& [Cue, List] : SplcCues)
            for (auto& C : List) LoadSplc(C.Bank);
        for (const char* B : CollisionBanks) LoadSplc(B);
        LoadSplc("sk8_foley");
        bSkaterAudio = SkaterAudio::Load();
        if (bSkaterAudio) LoadRolling();
        PreloadOneShots();
        size_t Count = 0;
        for (auto& [K, V] : Events) Count += V.size();
        Log("Audio: ready, %zu events, %zu sounds", Events.size(), Count);
        bReady = true;
        return true;
    }

    void PlayFile(const std::string& Path, float Volume, float Pitch, float DelaySeconds = 0.f)
    {
        Shot* Free = nullptr;
        for (Shot& S : Shots)
        {
            if (S.bUsed && ma_sound_at_end(&S.Sound))
            {
                ma_sound_uninit(&S.Sound);
                S.bUsed = false;
            }
            if (!S.bUsed && !Free) Free = &S;
        }
        if (!Free) return;  // all one-shot slots busy
        if (ma_sound_init_from_file(&Engine, Path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, &Free->Sound) != MA_SUCCESS)
            return;
        ma_sound_set_volume(&Free->Sound, Volume);
        ma_sound_set_pitch(&Free->Sound, Pitch);
        if (DelaySeconds > 0.0005f)
            ma_sound_set_start_time_in_milliseconds(&Free->Sound,
                ma_engine_get_time_in_milliseconds(&Engine) + ma_uint64(DelaySeconds * 1000.f));
        ma_sound_start(&Free->Sound);
        Free->bUsed = true;
    }

    // One SPLC id: an event (all its layers) or a group (one of its events at random).
    void PlaySplcId(const SplcBank& B, int Id, float Volume, float Pitch, int Depth = 0)
    {
        const int NEv = int(B.Events.size());
        if (Id >= NEv)
        {
            const int G = Id - NEv;
            if (G >= int(B.Groups.size()) || B.Groups[G].empty() || Depth > 3) return;
            const auto& List = B.Groups[G];
            PlaySplcId(B, List[std::uniform_int_distribution<size_t>(0, List.size() - 1)(Rng)], Volume, Pitch, Depth + 1);
            return;
        }
        if (Id < 0) return;
        const SplcEvent& E = B.Events[Id];
        // Pitch spread: up to +Spread (0.122 = 2 semitones, 0.189 = 3) around the event's pitch.
        const float P = Pitch * E.Pitch * (1.f + Random(-0.5f, 0.5f) * E.Spread);
        char Path[512];
        for (const auto& Layer : E.Layers)
        {
            const SplcEntry& X = Layer[std::uniform_int_distribution<size_t>(0, Layer.size() - 1)(Rng)];
            snprintf(Path, sizeof(Path), "%s\\%s_%03u.wav", SplcWavs.c_str(), B.Name.c_str(), unsigned(X.Sample));
            PlayFile(Path, Volume * E.Volume * X.Gain, P, X.Delay);
        }
    }

    void Play(const char* Event, float Volume, float Pitch = 1.f)
    {
        auto Cue = SplcCues.find(Event);
        if (Cue != SplcCues.end())
        {
            const auto& List = Cue->second;
            const SplcCue& C = List[std::uniform_int_distribution<size_t>(0, List.size() - 1)(Rng)];
            auto B = SplcBanks.find(C.Bank);
            if (B == SplcBanks.end() || B->second.Events.empty()) return;
            for (int Id : C.Ids) PlaySplcId(B->second, Id, Volume * C.Gain, Pitch);
            return;
        }
        const std::string Path = Pick(Event);
        if (Path.empty()) return;
        PlayFile(Path, Volume, Pitch);
    }

    // ---- retail trick sounds (flips component: 0x824B9D50 per frame) ----
    // Class_Flips (0x824B9E58 create / 0x824BA678 update): one instance while the skater is in the
    // air with a trick, of the trick table's first audio field; released when the skater lands or
    // the trick's type changes; its spin inputs follow the deck every frame.
    // cloth_trick (0x824BA430): the first field's rustle when a trick type starts; (0x824BA520) the
    // second field's rustle when the trick ends, kept for as long as the trick lasted.
    struct RetailTrickState
    {
        int Flip = 0, FlipType = -1;
        int Cloth = 0, ClothType = -1;
        int ClothEnd = 0;
        int PrevFirst = -1, Second = -1;
        float TrickTime = 0, ClothEndTime = 0;
    } RT;

    std::vector<int32_t> FlipValues(int Type, const float Spin[3])
    {
        auto Rate = [](float W, float Max, int32_t Min) {
            int32_t V = int32_t(std::fabs(W) / Max * 1000.f);
            if (V > 1000) V = 1000;
            return V >= Min ? V : 0;
        };
        const int32_t Pitch = Rate(Spin[0], 3.0f, 297), Yaw = Rate(Spin[1], 4.4f, 703), Roll = Rate(Spin[2], 10.2f, 603);
        return { 0x7fff, EmGain, 0, EmPan, EmPitch, EmLpf, EmHpf, Roll, Yaw, Pitch, 500, Type, 0, 0, 0, 0, 1,
                 27646, 14848, 6656, 0, 10000, 0, 0, 1, 1, EmGain, 6 };
    }

    int StartCloth(int Type) { return Snd.Create("cloth_trick", { 0, EmPan, EmPitch, 0, EmLpf, EmHpf, 0, EmGain, 1, Type, 0 }); }
    void Release(int& Id) { if (Id) Snd.Destroy(Id); Id = 0; }

    void RetailTricks(const SkAudio& A, const float Spin[3], float Dt)
    {
        const int First = A.trick_active ? SkaterAudio::TrickAudio(A.trick_id, false) : -1;
        if (First != -1) RT.Second = SkaterAudio::TrickAudio(A.trick_id, true);  // +0x3C
        const bool bAir = A.air != 0;  // (the on-foot no-comply lift, obj+0x136, is not published)

        // 0x824B9E58: start the flip sound.
        if (bAir && A.trick_active && !RT.Flip)
        {
            RT.FlipType = First;
            if (First != -1 && First != 0x23 && First != 0x24) RT.Flip = Snd.Create("Class_Flips", FlipValues(First, Spin));
        }
        // 0x824BA430: the trick-start rustle.
        if (First != -1)
        {
            if (!RT.Cloth) { RT.Cloth = StartCloth(First); RT.ClothType = First; }
            else if (First != RT.ClothType) Release(RT.Cloth);
        }
        // 0x824BA520: the trick-end rustle.
        bool bEnded = false;
        if (First == -1)
        {
            if (RT.PrevFirst != -1) { bEnded = true; RT.ClothEndTime = RT.TrickTime; RT.TrickTime = 0; }
        }
        else RT.TrickTime += Dt;
        if (RT.ClothEndTime <= 0.f) Release(RT.ClothEnd);
        else if (RT.Second != -1 && bEnded) { Release(RT.ClothEnd); RT.ClothEnd = StartCloth(RT.Second); }
        RT.PrevFirst = First;
        if (RT.ClothEndTime > 0.f) RT.ClothEndTime -= Dt;
        // 0x824BA678: release on landing / type change, else follow the deck's spin.
        if (RT.Flip)
        {
            if (!bAir || First != RT.FlipType) Release(RT.Flip);
            else Snd.Update(RT.Flip, FlipValues(RT.FlipType, Spin));
        }
    }

    // ---- retail grind loop (0x824B0708 start / stop, 0x824B1838 per frame) ----
    // Class_grind while grinding: kind 0 for grind families 1, 2, 4 (board), 3 for family 5, else 2;
    // family 0 (trucks) adds a second instance of kind 1. Speed = clamp((ground speed - 0.5) / 45
    // * 3.6) * 10000, at most 9000; the surface's gain is fixed at creation, its emitter scale is
    // applied every frame.
    struct RetailGrindState { int Inst[2] = {}; float Gain[2] = {}; int Family = -1; int32_t Speed = 0; } RG;

    std::vector<int32_t> GrindValues(int32_t Speed, int Surface, int Kind, float Gain, float Scale)
    {
        // 0x8249D7C0 layout from +4: volume, emitter gain x surface scale, emitter 5, azimuth, pitch,
        // low-pass, high-pass, speed, 0x400, surface, kind, surface gain, 0, local player, local
        // emitter, emitter 6, 0.
        return { 0x7fff, int32_t(std::fmin(float(EmGain) * Scale, 32767.f)), 0, EmPan, EmPitch, EmLpf, EmHpf,
                 std::clamp<int32_t>(Speed, 0, 10000), 0x400, std::clamp(Surface, 0, 14), std::clamp(Kind, 0, 3),
                 int32_t(std::clamp(Gain * 32767.f, 0.f, 32767.f)), 0, 1, 1, 0, 0 };
    }

    void RetailGrind(const SkAudio& A)
    {
        const int Surface = SkaterAudio::GrindSurface();
        const bool bGrind = A.grinding != 0;
        if (bGrind)
        {
            const float X = std::clamp((A.ground_speed - 0.5f) / SkaterAudio::GrindSpeedDivisor() * 3.6f, 0.f, 1.f);
            RG.Speed = std::min<int32_t>(int32_t(X * 10000.f), 9000);
        }
        const int Fam = A.grind_family;
        const int Kind = (Fam == 1 || Fam == 2 || Fam == 4) ? 0 : Fam == 5 ? 3 : 2;
        const float Scale = SkaterAudio::GrindScale(Surface, Kind);
        auto Start = [&](int Slot, int K) {
            RG.Gain[Slot] = SkaterAudio::GrindGain(Surface, K);
            RG.Inst[Slot] = Snd.Create("Class_grind", GrindValues(RG.Speed, Surface, K, RG.Gain[Slot], Scale));
        };
        if (!RG.Inst[0])
        {
            if (bGrind && Surface != 0xe)
            {
                RG.Family = Fam;
                Start(0, Kind);
                if (Fam == 0) Start(1, 1);
            }
        }
        else if (!bGrind)
        {
            for (int& Id : RG.Inst) Release(Id);
            RG.Family = -1;
        }
        if (!bGrind || !RG.Inst[0]) return;
        if (Fam != RG.Family)
        {
            if (Fam == 0) { if (!RG.Inst[1]) Start(1, 1); }
            else if (RG.Family == 0) Release(RG.Inst[1]);
            RG.Family = Fam;
        }
        Snd.Update(RG.Inst[0], GrindValues(RG.Speed, Surface, Kind, RG.Gain[0], Scale));
        if (RG.Inst[1]) Snd.Update(RG.Inst[1], GrindValues(RG.Speed, Surface, 1, RG.Gain[1], Scale));
    }

    // ---- retail wheel skid (0x824B5228 start, 0x824B5810 per frame / stop) ----
    // Class_wheels_skid runs while the board slips sideways (Audio+0x14 > 0: always while the
    // wheels touch, the class itself fades with the slide angle), not during a grind (except
    // grind family 4) nor while a trick with a flip sound is current. Speed = clamp(ground speed
    // * 0.08) * 10000, slide angle = slip * 90, surface = AudioSurfaceMap word 3.
    int RetailSkidId = 0;

    bool SkidWanted(const SkAudio& A, int First)
    {
        if (A.side_slip <= 0.f) return false;  // (the hold timer +0x5EC and obj+0x2B2 are not published)
        if (A.grinding) return A.grind_family == 4;
        return !(First != -1 && First != 0x23);
    }

    std::vector<int32_t> SkidValues(const SkAudio& A)
    {
        const int32_t Speed = int32_t(std::clamp(A.ground_speed * 0.08f, 0.f, 1.f) * 10000.f);
        const int32_t Angle = std::clamp(int32_t(A.side_slip * 90.f), 0, 90);
        // 0x8249D570 layout from +4: volume, emitter gain, emitter 13, azimuth, pitch, low-pass,
        // high-pass, speed, local flag, surface, slide angle, FB10048C (23000), 8CE42E5A (32767), ...
        return { 0x7fff, EmGain, 0, EmPan, EmPitch, EmLpf, EmHpf, Speed, 1, SkaterAudio::SkidSurface(), Angle,
                 23000, 32767, 0, 0, 0, 0x7fff, 0x7fff };
    }

    void RetailSkid(const SkAudio& A)
    {
        const int First = A.trick_active ? SkaterAudio::TrickAudio(A.trick_id, false) : -1;
        const bool bOn = SkidWanted(A, First);
        if (!RetailSkidId)
        {
            if (bOn) RetailSkidId = Snd.Create("Class_wheels_skid", SkidValues(A));
        }
        else if (!bOn) Release(RetailSkidId);
        else Snd.Update(RetailSkidId, SkidValues(A));
    }

    // ---- retail grain rolling (0x824B3A98 setup, 0x824B49C8 per frame, 0x824B3F88 landing) ----
    // Grain-mode surfaces (all but 7 and 8) play their recording through two grain players per
    // track: player A at gain 1 - carve weight, player B at the carve weight, 0.1 lower in the
    // recording. The position in the recording comes from the surface's speed curve over
    // km/h / divisor; a landing briefly raises that speed (a 35 / 200 / 600 ms envelope) and
    // starts Rolling_Rattle_Class. Manual balance lowers the rolling to 0.65.
    // Not ported: the landing filter dip (envelope +0x40C, read by vector code) and the emitter
    // mix gains (neutral).
    struct RollTuning
    {
        std::string File;
        float Div = 65, MaxW = 1, Curve[4] = { 1, 0.707f, 0, 0 };
        Grain::Params Gp[2];
        float Kick0 = 1.4f, Kick1 = 1.1f, KickDiv = 45, Rise = 35, Hold = 200, Fall = 600;
        float RattleDiv = 30, Up = 0.06f, Down = 0.06f;
    };
    RollTuning Roll;
    std::shared_ptr<const Grain::Recording> RollRec;
    int RollSurface = 0;   // rolling surface (AudioSurfaceMap word 1); 0 = not a grain surface

    // 0x8247B1A8 / B200 / B2D8 / B350: piecewise-linear envelope.
    struct Envelope
    {
        struct Seg { float From, To, Seconds; };
        std::vector<Seg> Segs;
        size_t Index = 0;
        float Time = 0, Value = 1;
        bool Done = true;
        void Start(std::vector<Seg> S) { Segs = std::move(S); Index = 0; Time = 0; Done = Segs.empty(); if (!Done) Value = Segs[0].From; }
        void Tick(float Dt)
        {
            if (Done) return;
            Time += Dt;
            while (!Done)
            {
                const Seg& G = Segs[Index];
                if (Time <= G.Seconds) { Value = G.From + (G.To - G.From) * (G.Seconds > 0 ? Time / G.Seconds : 1.f); return; }
                Value = G.To;
                Time -= G.Seconds;
                if (++Index >= Segs.size()) Done = true;
            }
        }
    };
    struct RetailRollState
    {
        bool On[2] = {};
        Envelope Kick;                 // board component +0x390 (speed factor +0x404)
        bool Prev14d = false, Prev14e = false;
        bool ManualLatch = false;      // +0x5E0
        float Carve = 0;               // +0x488
        int Rattle = 0;                // +0x514
    } RR;

    bool LoadRolling()
    {
        RollSurface = SkaterAudio::RollingSurface();
        std::ifstream In(ModPath("skate-data\\sounds\\rolling_surfaces.txt"));
        std::string Line;
        bool bFound = false;
        while (std::getline(In, Line))
        {
            for (char& C : Line) if (C == '|') C = ' ';
            std::istringstream L(Line);
            std::string Key;
            int Surf = -1;
            if (!(L >> Key) || Key != "roll" || !(L >> Surf) || Surf != RollSurface) continue;
            RollTuning T;
            L >> T.File >> T.Div >> T.MaxW >> T.Curve[0] >> T.Curve[1] >> T.Curve[2] >> T.Curve[3];
            for (Grain::Params& G : T.Gp) L >> G.Attack >> G.Sustain >> G.Release >> G.Window >> G.Threshold;
            L >> T.Kick0 >> T.Kick1 >> T.KickDiv >> T.Rise >> T.Hold >> T.Fall >> T.RattleDiv >> T.Up >> T.Down;
            Roll = T;
            bFound = true;
        }
        if (!bFound) { Log("Audio: rolling surface %d has no grain recording (Class_rolling surface)", RollSurface); RollSurface = 0; return false; }
        std::string Wav = Roll.File;
        if (Wav.size() > 6 && Wav.compare(Wav.size() - 6, 6, ".grain") == 0) Wav.resize(Wav.size() - 6);
        RollRec = Grain::Load(ModPath("skate-data\\sounds\\grains\\") + Wav + ".wav");
        Log("Audio: grain rolling surface %d (%s): %s", RollSurface, Roll.File.c_str(),
            RollRec ? "recording loaded" : "recording missing");
        if (!RollRec) RollSurface = 0;
        return RollRec != nullptr;
    }

    void StopRolling()
    {
        for (auto& Track : RollPlayers)
            for (Grain::Player& Pl : Track) Pl.Stop();
        RR.On[0] = RR.On[1] = false;
        Release(RR.Rattle);
    }

    void RetailRolling(const SkAudio& A, float Dt)
    {
        if (!RollSurface || !RollRec) return;
        const bool S55 = A.state_flags & 4, S56 = A.state_flags & 8, S57 = A.state_flags & 16, S60 = A.state_flags & 32;
        // Audio object +0x14D..0x14F (0x8249EE78): the landing pulse, once per landing.
        const bool b14f = S55 && !RR.Prev14e && !RR.Prev14d;
        RR.Prev14e = S55 && S57;
        RR.Prev14d = S55 && !S57 && S56;
        // Manual latch (0x824B8478): State60 (balance) sets it; all wheels down or none clears it.
        if (S60) RR.ManualLatch = true;
        else if (RR.ManualLatch && (A.wheel_count == 0 || A.wheel_count == 4)) RR.ManualLatch = false;

        // 0x824B3F88: the landing kick on the rolling speed, and the rattle.
        const float Kmh1 = (A.ground_speed - 1.f) * 3.6f;
        if (b14f)
        {
            const float X = std::clamp(Kmh1 / Roll.KickDiv, 0.f, 1.f);
            const float Peak = Roll.Kick0 + (Roll.Kick1 - Roll.Kick0) * X;
            const float From = RR.Kick.Done ? 1.f : RR.Kick.Value;
            RR.Kick.Start({ { From, Peak, Roll.Rise * 0.001f }, { Peak, Peak, Roll.Hold * 0.001f }, { Peak, 1.f, Roll.Fall * 0.001f } });
            Release(RR.Rattle);
            int RattleSurface = 0;   // asphalt_smooth 1, concrete_rough 2, concrete_smooth 3, wood_ramp 4, concrete_aggregate 5
            switch (RollSurface) { case 3: RattleSurface = 1; break; case 2: RattleSurface = 2; break; case 4: RattleSurface = 3; break;
                                   case 5: RattleSurface = 4; break; case 6: RattleSurface = 5; break; default: break; }
            const int32_t Speed = int32_t(std::clamp(Kmh1 / Roll.RattleDiv, 0.f, 1.f) * 10000.f);
            RR.Rattle = Snd.Create("Rolling_Rattle_Class", { 0x7fff, EmPan, EmPitch, Speed, RattleSurface, 0, 1, 0, EmLpf, EmHpf, 0x7fff, 8 });
        }
        RR.Kick.Tick(Dt);
        const float Factor = RR.Kick.Done ? 1.f : RR.Kick.Value;

        // 0x824B6378: carve weight (player B), times Ground+0x108 (-Turn).
        float CarveTarget = std::clamp(A.com_speed * 0.24f, 0.f, Roll.MaxW) * A.carve_turn;
        if (RR.ManualLatch) CarveTarget = 0.f;
        const float Rate = CarveTarget <= RR.Carve ? Roll.Down : Roll.Up;
        RR.Carve = CarveTarget > RR.Carve + Rate ? RR.Carve + Rate : CarveTarget < RR.Carve - Rate ? RR.Carve - Rate : CarveTarget;
        const float WB = std::fabs(RR.Carve);

        // 0x824B49C8: speed curve -> position in the recording.
        const float X = std::clamp(A.ground_speed * Factor * 3.6f / Roll.Div, 0.f, 1.f), T = 1.f - X;
        const float* P = Roll.Curve;
        const float Pos = T * T * T * P[3] + 3.f * T * X * (P[1] * X + P[2] * T) + X * X * X * P[0];
        // Stand-in for the emitter gain channel (+0x3C(1), not ported): Skate 3 is silent with
        // the board at rest, the grain recordings are not, so fade out below 0.3 m/s.
        const float Rest = std::clamp((A.ground_speed - 0.1f) / 0.2f, 0.f, 1.f);
        const float GainA = (1.f - WB) * (RR.ManualLatch ? 0.65f : 1.f) * Rest * Snd.MasterGain;
        const float GainB = WB * Rest * Snd.MasterGain;

        // 0x824B6098: a track rolls while its wheels touch (not grinding).
        for (int t = 0; t < 2; t++)
        {
            const bool bOn = !A.grinding && A.wheel_flag[t == 0 ? 0 : 3];
            RollPlayers[t][0].Set(GainA, Pos, 1.f);
            RollPlayers[t][1].Set(GainB, std::max(0.f, Pos - 0.1f), 1.f);
            if (bOn && !RR.On[t])
            {
                RollPlayers[t][0].Start(RollRec, Roll.Gp[0]);
                RollPlayers[t][1].Start(RollRec, Roll.Gp[1]);
            }
            else if (!bOn && RR.On[t])
            {
                RollPlayers[t][0].Stop();
                RollPlayers[t][1].Stop();
            }
            RR.On[t] = bOn;
        }
    }

    // ---- 0x824B5528 truck squeaks (Class_Squeaks, Brd_Squeaks) ----
    // With both feet on the deck and two wheels down, a squeak starts once the truck tilt
    // (Motion+0xB8, as int(|tilt| * 114.59)) reaches A129 (15); it is a one-shot, restarted only
    // when the tilt changes side (or the feet leave the deck). Speed = clamp(gs * 0.08) * 1000,
    // intensity = |deck roll rate| / AC87 (1.5) * 1000 (0 below 50). Tuning: class 7AB23C11
    // "default" A129 / AC87, EQ 42AFE160 22D0 (0).
    struct RetailSqueakState { int Id = 0; bool Positive = false; } SQ;
    std::vector<int32_t> SqueakValues(const SkAudio& A);
    void UpdateSqueak(const SkAudio& A);
    void RetailSqueaks(const SkAudio& A)
    {
        if (!(A.feet_on_deck & 1) || !(A.feet_on_deck & 2) || A.wheel_count < 2) { Release(SQ.Id); return; }
        if (int(std::fabs(A.deck_tilt) * 114.591553f) < 15) return;
        const bool bPositive = A.deck_tilt >= 0.f;
        if (bPositive != SQ.Positive) Release(SQ.Id);
        SQ.Positive = bPositive;
        if (SQ.Id) return;
        SQ.Id = Snd.Create("Class_Squeaks", SqueakValues(A));
    }

    // Speed and intensity, written at the start (0x824B5528) and refreshed every frame while the
    // squeak lives (0x824B5BC0), with the neutral emitter values.
    std::vector<int32_t> SqueakValues(const SkAudio& A)
    {
        const int32_t Speed = int32_t(std::clamp(A.ground_speed * 0.08f, 0.f, 1.f) * 1000.f);
        int32_t Intensity = int32_t(std::fabs(A.deck_roll_rate) / 1.5f * 1000.f);
        if (Intensity > 1000) Intensity = 1000;
        else if (Intensity < 50) Intensity = 0;
        return { EmGain, 0x7fff, 0, 0, 0x1000, 25000, 0, Speed, 15, Intensity, 0 };
    }
    void UpdateSqueak(const SkAudio& A)
    {
        if (SQ.Id && Snd.Alive(SQ.Id)) Snd.Update(SQ.Id, SqueakValues(A));
    }

    // ---- 0x824AF360 / 0x824AF530 / 0x824AFB38 / 0x824AFC90 / 0x824AFDB0 seams (Class_Seams, one per wheel) ----
    // The seam pattern comes from the surface's collision tag; Fortnite has none, so every surface
    // uses the "sidewalk" pattern record (class 7242F328 216A369A: mode 1 = a grid, 3 m x 3 m
    // rotated 85 degrees, seam type 10, volume 0.9; "default": at least 1 frame between hits of a
    // wheel pair, above 0.05 m/s, gain 1, type-10 multiplier 2 on seam surface 3). A wheel whose
    // grid cell changes (wheel x/z rotated by the pattern angle, divided by the spacing) clacks:
    // trigger value 7 alternates 2 / 1 per wheel and is cleared every frame. The back wheels are
    // skipped in a manual (State54 / State60). No change-of-surface hits (Fortnite reports one
    // surface). 0x824AFDB0 refreshes the rest each frame: speed, turn, landing, manual, type.
    struct SeamPattern { float Volume = 0.9f, Angle = 85.f, SpacingA = 3.f, SpacingB = 3.f; int Type = 10, MinFrames = 1;
                         float MinSpeed = 0.05f, Gain = 1.f, TypeMult = 2.f; };
    const SeamPattern SeamRec;
    struct RetailSeamState
    {
        std::vector<int32_t> V[4];
        int Id[4] = {};
        int Cell[2][4] = {};
        bool bHaveCell[2][4] = {};
        bool Latch[4] = {};
        int Counter = 0, Last[4] = {};
        int Turn = 0;   // component +0x98: the turn value, rate-limited to 100 per frame
    } RS;

    bool SeamCellChanged(const SkAudio& A, int Wheel, int Axis)
    {
        if (A.air || A.grinding) return false;
        const float Rad = float(int(SeamRec.Angle)) * 0.0174532924f;
        const float X = A.wheel_position[Wheel][0], Z = A.wheel_position[Wheel][2];
        const float C = std::cos(Rad), S = std::sin(Rad);
        const float Coord = Axis == 1 ? X * S + Z * C : X * C - Z * S;
        const float Spacing = Axis == 1 ? SeamRec.SpacingB : SeamRec.SpacingA;   // record +8 / +0xC
        const float Mult = SkaterAudio::SeamSurface(false) == 3 && SeamRec.Type == 10 ? SeamRec.TypeMult : 1.f;
        const int Cell = int(std::floor(Coord / (Spacing * Mult)));
        const bool bChanged = RS.bHaveCell[Axis][Wheel] && RS.Cell[Axis][Wheel] != Cell;
        RS.Cell[Axis][Wheel] = Cell;
        RS.bHaveCell[Axis][Wheel] = true;
        return bChanged;
    }

    void SeamHit(int Wheel, bool bNotBoth)
    {
        std::vector<int32_t>& V = RS.V[Wheel];
        V[7] = RS.Latch[Wheel] ? 1 : 2;
        RS.Latch[Wheel] = !RS.Latch[Wheel];
        V[9] = bNotBoth ? 1 : 0;
        V[10] = std::clamp(SkaterAudio::SeamSurface(false), 0, 8);
        V[11] = 1;   // 0x824A02C0: the local skater
    }

    void SeamTrack(const SkAudio& A, int Axis)
    {
        if (++RS.Counter > 0x7fff) { RS.Counter = 0; for (int& L : RS.Last) L = 0; }
        const bool bManual = (A.state_flags & 2) || (A.state_flags & 32);
        const bool C0 = SeamCellChanged(A, 0, Axis), C1 = SeamCellChanged(A, 1, Axis);
        const bool C2 = !bManual && SeamCellChanged(A, 2, Axis), C3 = !bManual && SeamCellChanged(A, 3, Axis);
        const bool bFront = C0 && C1, bBack = C2 && C3;
        if (C0) { if (SeamRec.MinFrames < RS.Counter - RS.Last[1]) { SeamHit(0, !bFront); RS.Last[0] = RS.Counter; } }
        else if (C1) { if (SeamRec.MinFrames < RS.Counter - RS.Last[0]) { SeamHit(1, !bFront); RS.Last[1] = RS.Counter; } }
        if (C2) { if (SeamRec.MinFrames < RS.Counter - RS.Last[3]) { SeamHit(2, !bBack); RS.Last[2] = RS.Counter; } }
        else if (C3) { if (SeamRec.MinFrames < RS.Counter - RS.Last[2]) { SeamHit(3, !bBack); RS.Last[3] = RS.Counter; } }
    }

    void RetailSeams(const SkAudio& A)
    {
        // 0x824AFDB0 (before the hits of this frame, as the component updates).
        const int32_t Speed = int32_t(std::clamp((A.ground_speed - 0.5f) * 0.08f, 0.f, 1.f) * 10000.f);
        int Turn = int(A.carve_turn * 1000.f);
        if (Turn > RS.Turn + 100) Turn = RS.Turn + 100;
        else if (Turn < RS.Turn - 100) Turn = RS.Turn - 100;
        RS.Turn = Turn;
        const bool S55 = A.state_flags & 4, S56 = A.state_flags & 8, S57 = A.state_flags & 16;
        const bool bLanding = S55 && (S57 || S56);                              // audio obj +0x14D || +0x14E
        const bool bManual = (A.state_flags & 2) || (A.state_flags & 32);
        for (int w = 0; w < 4; w++)
        {
            if (RS.V[w].empty())
                RS.V[w] = { EmGain, 0x7fff, 0, 0, 0x1000, 25000, 0, 0, 0, 0,
                            std::clamp(SkaterAudio::SeamSurface(false), 0, 8), 1, 0, 0, w, 0, 0, 0, 0, 8 };
            std::vector<int32_t>& V = RS.V[w];
            V[0] = 0x7fff;
            V[8] = Speed;
            V[12] = bLanding ? 1 : 0;
            V[13] = std::clamp(SeamRec.Type, 0, 15);
            V[15] = std::min(1000, std::abs(Turn));
            V[16] = A.ground_speed > 0.5f ? 0x7fff : std::clamp(int(SeamRec.Volume * 32767.f), 0, 0x7fff);
            V[17] = bManual ? 1 : 0;
            V[18] = std::clamp(int(SeamRec.Gain * 32767.f), 0, 0x7fff);
            V[7] = 0;
        }
        if (A.ground_speed > SeamRec.MinSpeed) { SeamTrack(A, 0); SeamTrack(A, 1); }
        for (int w = 0; w < 4; w++)
        {
            if (RS.Id[w] && Snd.Alive(RS.Id[w])) Snd.Update(RS.Id[w], RS.V[w]);
            else RS.Id[w] = Snd.Create("Class_Seams", RS.V[w]);
        }
    }

    void StopSeams()
    {
        for (int& Id : RS.Id) Release(Id);
        RS = RetailSeamState{};
    }

    // ---- 0x824A93D8 foot drag (Class_foot_drag, FOOT_DRAG) ----
    // Starts when a foot drags (State52 = brake contact, or State54): speed clamp((gs - E5A6) /
    // B2C8 * 3.6) * 10000, surface (0x824A8228), 9717 / 194E / 6242 / 9FF8 (4000 / 4500 / 4000 /
    // 22500, class C26949FC "default"), flag = !State52, EQ 7 (42AFE160 C58C / 2DBD).
    // 0x824ACD80 refreshes it every frame (speed, surface, emitter values) and stops it once
    // neither foot drags any more.
    int FootDragId = 0;
    std::vector<int32_t> FootDragValues(const SkAudio& A, bool S52, bool S54)
    {
        const int32_t Speed = int32_t(std::clamp((A.ground_speed - 0.5f) / 50.f * 3.6f, 0.f, 1.f) * 10000.f);
        return { EmGain, 0x7fff, 0, 0, 0x1000, 25000, 0, Speed, std::clamp(SkaterAudio::FootDragSurface(S54), 0, 10),
                 4000, 4500, 4000, 22500, S52 ? 0 : 1, 7 };
    }
    void RetailFootDrag(const SkAudio& A)
    {
        const bool S52 = A.state_flags & 1, S54 = A.state_flags & 2;
        if (!S52 && !S54) { Release(FootDragId); return; }
        if (FootDragId && Snd.Alive(FootDragId)) { Snd.Update(FootDragId, FootDragValues(A, S52, S54)); return; }
        FootDragId = Snd.Create("Class_foot_drag", FootDragValues(A, S52, S54));
    }

    // ---- 0x824B91B8 / 0x824B92B0 board slide (c_board_slide, board_scrapes) ----
    // The deck scraping the ground in a wipeout: starts when the board-slide state turns on, stops
    // when it turns off. Every frame: speed clamp((gs - 0.5) / 15 * 3.6) * 10000 and the level
    // 15000 (class C1831BDB 62109062: 9635 / 662C upside down / AB87 on its side), EQ 7
    // (42AFE160 F2B4); value 11 has to rise after the start for the scrape to sound.
    int BoardSlideId = 0;
    void RetailBoardSlide(const SkAudio& A)
    {
        if (!A.board_slide) { Release(BoardSlideId); return; }
        const int32_t Flag = A.board_slide == 2 ? 1 : 0;
        if (!BoardSlideId || !Snd.Alive(BoardSlideId))
        {
            BoardSlideId = Snd.Create("c_board_slide", { 0, 0, 0x1000, 0, 25000, 0, 0, 0, 7, 0, Flag, 0 });
            return;
        }
        const int32_t Speed = int32_t(std::clamp((A.ground_speed - 0.5f) / 15.f * 3.6f, 0.f, 1.f) * 10000.f);
        Snd.Update(BoardSlideId, { 0x7fff, EmPan, 0x1000, Speed, 25000, 0, 0x7fff, 0x7fff, 7, 0, Flag, 15000 });
    }

    void StopAll()
    {
        Rolling.Stop();
        Footstep.Stop();
        Grind.Stop();
        Skid.Stop();
        Wind.Stop();
        SpeedRattle.Stop();
        for (Sound& S : Seams) S.Stop();
        for (Flip& F : Flips) Snd.Destroy(F.Id);
        Flips.clear();
        Release(RT.Flip);
        Release(RT.Cloth);
        Release(RT.ClothEnd);
        RT = RetailTrickState{};
        for (int& Id : RG.Inst) Release(Id);
        RG = RetailGrindState{};
        Release(RetailSkidId);
        StopRolling();
        PendingSeams.clear();
        StopSeams();
        Release(SQ.Id);
        SQ = RetailSqueakState{};
        Release(FootDragId);
        Release(BoardSlideId);
    }

    // ---- class drivers ----

    // Debounce for continuous sounds: on once wanted for 60 ms, off once unwanted for 200 ms.
    // Without it a board chattering between ground and air (or on and off a rail) restarted a
    // sound every frame: a buzzing, "techy" machine-gun of attacks.
    struct Debounce
    {
        float OnTime = 0, OffTime = 0;
        bool Active = false;
        bool Update(bool bWant, float Dt)
        {
            if (bWant) { OnTime += Dt; OffTime = 0; if (OnTime >= 0.06f) Active = true; }
            else { OffTime += Dt; OnTime = 0; if (OffTime >= 0.2f) Active = false; }
            return Active;
        }
    };
    Debounce RollGate, GrindGate, SkidGate, SeamGate, SpeedGate;

    void DriveRolling(bool bOn, float Kmh)
    {
        if (!bOn) { Rolling.Stop(); return; }
        // FUN_824b2a70 layout, per-frame values from FUN_824b49c8.
        Rolling.Set({ 0x7fff, EmPan, EmPitch, Clamp01(Kmh / 45.f, 10000.f), RollingSurface, 0, 0, 0, 0, EmLpf, EmHpf, EmGain });
    }

    void DriveGrind(bool bOn, float Mps)
    {
        if (!bOn) { Grind.Stop(); return; }
        int32_t Speed = Clamp01((Mps - 0.5f) / 45.f * 3.6f, 10000.f);
        if (Speed > 9000) Speed = 9000;
        // FUN_8249d7c0 layout; FUN_824b1838 sets vol, gain (emitter * surface scale), filters, pan, pitch.
        Grind.Set({ 0x7fff, int32_t(EmGain * GrindGainKind2), 0, EmPan, EmPitch, EmLpf, EmHpf, Speed, 0x400, Surface, 2,
                    int32_t(GrindLevel * 32767.f), 0, 0, 0, 0, 0 });
    }

    void DriveSkid(bool bOn, float Mps, float AngleDeg)
    {
        if (!bOn) { Skid.Stop(); return; }
        // FUN_8249d570 values: speed, flag, surface 0..4, slide angle 0..90, tuning 23000 / 32767.
        Skid.Set({ 0x7fff, EmGain, 0, EmPan, EmPitch, EmLpf, EmHpf, Clamp01(Mps * 0.08f, 10000.f), 1, 0,
                   int32_t(std::fmin(90.f, std::fmax(0.f, AngleDeg))), 23000, 32767, 0, 0, 0, 0x7fff, 0x7fff });
    }

    void DriveSenseOfSpeed(bool bOnBoard, float Kmh)
    {
        // FUN_824d5480: rattle above Lo km/h, wind from the skater speed, EQ chain 7.
        constexpr float RattleLo = 20.f, RattleHi = 45.f, WindLo = 25.f, WindHi = 60.f;
        if (bOnBoard && Kmh >= RattleLo)
            SpeedRattle.Set({ 0x7fff, EmPan, EmPitch, Clamp01((Kmh - RattleLo) / (RattleHi - RattleLo), 1000.f), EmLpf, EmHpf, 0, EmGain, 7, 0x7fff, 0x7fff });
        else SpeedRattle.Stop();
        if (Kmh >= WindLo)
            Wind.Set({ 0x7fff, EmPan, EmPitch, Clamp01((Kmh - WindLo) / (WindHi - WindLo), 1000.f), EmLpf, EmHpf, 0, EmGain, 7, 0x7fff, 0x7fff, 0x7fff, 0x7fff });
        else Wind.Stop();
    }

    // Sidewalk joints every ~1.5 m: each one clacks under the front wheels, then the back wheels.
    void DriveSeams(bool bOn, float Mps, float Dt)
    {
        if (!bOn)
        {
            for (Sound& S : Seams) S.Stop();
            PendingSeams.clear();
            SeamDistance = 0;
            return;
        }
        auto Base = [](int Wheel) {
            // FUN_8249dcc8 layout: surface, flag, wheel index, EQ chain 8; v13 = hit.
            return std::vector<int32_t>{ 0x7fff, EmGain, 0, EmPan, EmPitch, EmLpf, EmHpf, 0, 0, 0, 0, 1, 0, 0, Wheel, 0, 0, 0, 0, 8 };
        };
        for (int w = 0; w < 4; w++)
            if (!Seams[w].Id) Seams[w].Set(Base(w));
        for (int w = 0; w < 4; w++)  // release last frame's hits
            if (Seams[w].Values.size() > 13 && Seams[w].Values[13]) { auto V = Base(w); Seams[w].Set(V); }
        if (Mps < 1.f) return;
        SeamDistance += Mps * Dt;
        if (SeamDistance >= 1.5f)
        {
            SeamDistance -= 1.5f;
            const float Wheelbase = 0.38f;
            PendingSeams.push_back({ 0.f, 0 });
            PendingSeams.push_back({ 0.f, 1 });
            PendingSeams.push_back({ Wheelbase / Mps, 2 });
            PendingSeams.push_back({ Wheelbase / Mps, 3 });
        }
        for (auto it = PendingSeams.begin(); it != PendingSeams.end();)
        {
            it->Time -= Dt;
            if (it->Time <= 0.f)
            {
                auto V = Base(it->Wheel);
                V[13] = 1;
                Seams[size_t(it->Wheel)].Set(V);
                it = PendingSeams.erase(it);
            }
            else ++it;
        }
    }

    // The trick's eSk8AudioTricks value, from Skate 3's trick table (attrib class 0x6918469984A8C596,
    // second audio field): kickflip 0, heelflip 1, pop shuvit 2, FS pop shuvit 3, varial kickflip 4,
    // varial heelflip 5, hardflip 6, inward heelflip 7, 360 pop shuvit 8, FS 360 pop shuvit 9,
    // 360 flip 10, laser flip 11, 360 hardflip 12, 360 inward heelflip 13, the nollie versions
    // 14..27 in the same order, ollie 28, nollie 29, late shuvits / dark catches 30, no-comply 34,
    // grabs 35, fingerflips 37, underflips 38. -1 = no flip sound (grinds, slides, manuals).
    // Every flip trick also plays 28 (the pop whoosh, first audio field) - only ollies play it alone.
    constexpr int SndOllie = 28, SndNollie = 29, SndGrab = 35;
    int TrickType(const char* Name)
    {
        std::string N;
        for (const char* C = Name; *C; C++)
            if (*C != ' ' && *C != '-' && *C != '_') N += char(tolower(*C));
        auto Has = [&](const char* W) { return N.find(W) != std::string::npos; };
        const bool bNollie = N.rfind("nollie", 0) == 0;
        if (Has("underflip")) return 38;
        if (Has("fingerflip")) return 37;
        if (Has("nocomply")) return 34;
        if (Has("darkcatch")) return 30;
        if (Has("late") || Has("darktolight"))
        {
            if (Has("kickflip")) return 0;
            if (Has("heelflip")) return 1;
            return 30;
        }
        static const std::pair<const char*, int> Flips[] = {
            { "360inwardheelflip", 13 }, { "360hardflip", 12 }, { "laserflip", 11 }, { "360flip", 10 },
            { "fs360popshuv", 9 }, { "fs360shuv", 9 }, { "360popshuv", 8 }, { "360shuv", 8 }, { "inwardheelflip", 7 },
            { "hardflip", 6 }, { "varialheelflip", 5 }, { "varialkickflip", 4 }, { "fspopshuv", 3 },
            { "fsshuv", 3 }, { "popshuv", 2 }, { "shuv", 2 }, { "heelflip", 1 }, { "kickflip", 0 } };
        for (const auto& F : Flips)
            if (Has(F.first)) return F.second + (bNollie ? 14 : 0);
        if (Has("ollie")) return bNollie ? SndNollie : SndOllie;
        if (N == "nollie") return SndNollie;
        if (Has("grind") || Has("slide") || Has("manual") || Has("stall") || Has("plant") || Has("lip") ||
            Has("revert") || Has("powerslide") || Has("bodyvar"))
            return -1;
        if (Has("180") || Has("360") || Has("540") || Has("720") || Has("bigspin") || Has("flip"))
            return SndOllie;  // body spins / body flips: the pop whoosh only (BS/FS 180: 28, 28)
        return SndGrab;       // grabs and the rest of the air tricks
    }

    // FUN_824b9e58: the flip sound, started once per trick with the deck's spin on each axis scaled
    // by Skate 3's tuning (attrib "tricks": max rate 3.0 / 4.4 / 10.2 rad/s, below 297 / 703 / 603 -> 0).
    void StartFlip(int Type, const float Spin[3])
    {
        auto Rate = [](float W, float Max, int32_t Min) {
            int32_t V = int32_t(std::fabs(W) / Max * 1000.f);
            if (V > 1000) V = 1000;
            return V >= Min ? V : 0;
        };
        const int32_t Pitch = Rate(Spin[0], 3.0f, 297), Yaw = Rate(Spin[1], 4.4f, 703), Roll = Rate(Spin[2], 10.2f, 603);
        const int Id = Snd.Create("Class_Flips", { 0x7fff, EmGain, 0, EmPan, EmPitch, EmLpf, EmHpf,
            Roll, Yaw, Pitch, 500, Type, 0, 0, 0, 0, 1, 27646, 14848, 6656, 0, 10000, 0, 0, 1, 1, EmGain, 6 });
        if (Id) Flips.push_back({ Id, 0.f });
    }

    // FUN_824ba430 / 824ba520: the clothes rustle of a trick (cloth_trick, Foley_Cloth), at its
    // start and again when it ends. Volume comes from the PC_Foley_* controllers.
    void Cloth(int Type)
    {
        const int Id = Snd.Create("cloth_trick", { 0, EmPan, EmPitch, 0, EmLpf, EmHpf, 0, EmGain, 1, Type, 0 });
        if (Id) Flips.push_back({ Id, 0.f });
    }
    int AirTrickType = -1;
    bool bFlipThisJump = false;
    bool bWhooshThisJump = false;   // the pop whoosh (28) already played this jump
    int PendingFlip = -1;      // trick type waiting for the board to start spinning
    float PeakSpin = 0;        // fastest deck spin (rad/s) of this jump's flip
    bool bCaught = false;      // the catch clack already played this jump
    float LastMultiplier = 1.f;

    // Skate-shoe footsteps (playercharacter_footstep, fstep_skateshoe1_sm, creator FUN_824a52a8):
    // the foot hitting the ground on a push (Skate 3's push_contact animation event) or a step.
    // Inputs 8 / 11 trigger the left / right foot; 3 is the step strength (0..1000).
    float SinceFootEvent = 1.f, SinceStepEvent = 1.f, SinceFoley = 1.f;
    bool bRightFoot = false;
    std::vector<int32_t> FootValues(int Strength, int Trigger)
    {
        std::vector<int32_t> V = { EmGain, EmPan, EmPitch, Strength, EmLpf, EmHpf, 0x7fff, 0x7fff, 0, 800, 800, 0, 1, 1,
                                   800, 0, 1, 1, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff, 0x7fff };
        if (Trigger >= 0) V[size_t(Trigger)] = 1;
        return V;
    }
    float PendingFlipTime = 0;
}

// Loads everything on a background thread (banks are tens of MB: on a cold disk that took 15 s
// on the game thread). Called as soon as the mod knows it is the client.
static bool bLoading = false;

static void PreloadUnsafe()
{
    if (bLoading) return;
    bLoading = true;
    std::thread([] {
        // loads while a match runs: lowest CPU priority and background disk / memory priority
        // (Client.cpp LoadEngine: normal disk priority starved Fortnite's streamed sounds)
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        __try { Init(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("Audio: exception 0x%08X while loading", GetExceptionCode()); }
    }).detach();
}

static void StartUnsafe()
{
    PreloadUnsafe();  // no-op once loading has begun; skating starts right away, sound joins when ready
    bActive = true;
    LastMultiplier = 1.f;
    Last = EWhat::Other;
    SeamDistance = 0;
    SkaterAudio::Reset();
}

static void StopUnsafe()
{
    bActive = false;
    if (bAems) StopAll();
}

// SkaterAudio backend: one SPLC id of a material kind's collision bank.
static void PlayCollision(int Kind, int Id, float Volume, float Pitch)
{
    if (Kind < 0 || Kind > 2) return;
    auto B = SplcBanks.find(CollisionBanks[Kind]);
    if (B == SplcBanks.end() || B->second.Events.empty()) return;
    PlaySplcId(B->second, Id, Volume, Pitch);
}

static void UpdateUnsafe(const SkState& Sk, float Dt, const Audio::Cues& Hud)
{
    if (!bReady) return;
    if (bAems) Snd.Tick(Dt);
    if (!bActive) return;

    const float Speed = std::sqrt(Sk.velocity[0] * Sk.velocity[0] + Sk.velocity[1] * Sk.velocity[1]);  // cm/s
    const float Vz = Sk.velocity[2];
    const float Mps = Speed / 100.f, Kmh = Mps * 3.6f;
    const float Mps3 = std::sqrt(Speed * Speed + Vz * Vz) / 100.f;
    const EWhat Now = Classify(Sk.state);
    const bool bOnBoardGround = Now == EWhat::Ground || Now == EWhat::Slide;

    // Impacts: Skate 3's board / body audio logic (pop, landing, grind contact, body hits).
    if (bSkaterAudio && Hud.Skater)
    {
        SkaterAudio::Backend B;
        B.PlaySplc = PlayCollision;
        SkaterAudio::Update(*Hud.Skater, Dt, B, std::strncmp(Sk.state, "Wipeout", 7) == 0);
    }

    if (bAems)
    {
        // Slide angle: between where the board points and where it travels.
        float Angle = 0;
        if (Speed > 10.f)
        {
            const float Yaw = Sk.yaw * 3.14159265f / 180.f;
            const float Dot = (std::cos(Yaw) * Sk.velocity[0] + std::sin(Yaw) * Sk.velocity[1]) / Speed;
            Angle = std::acos(std::fmin(1.f, std::fabs(Dot))) * 180.f / 3.14159265f;
        }
        if (Hud.Skater && RollSurface) { Rolling.Stop(); RetailRolling(*Hud.Skater, Dt); }
        else DriveRolling(RollGate.Update(bOnBoardGround && Speed > 5.f, Dt), Kmh);
        if (Hud.Skater) RetailGrind(*Hud.Skater);
        else DriveGrind(GrindGate.Update(Now == EWhat::Grinding, Dt), Mps3);
        const bool bSkid = SkidGate.Update(Now == EWhat::Slide, Dt);
        if (Hud.Skater) RetailSkid(*Hud.Skater);
        else DriveSkid(bSkid, Mps, bSkid ? std::fmax(Angle, 45.f) : 0.f);
        // 0x824D5480: the wind follows the skater's own speed (audio obj +0xD4), not the board's.
        DriveSenseOfSpeed(SpeedGate.Update(bOnBoardGround || Now == EWhat::Grinding, Dt),
                          Hud.Skater ? Hud.Skater->com_speed * 3.6f : (Now == EWhat::Air ? Mps3 * 3.6f : Kmh));
        if (Hud.Skater)
        {
            RetailSeams(*Hud.Skater);
            RetailSqueaks(*Hud.Skater);
            UpdateSqueak(*Hud.Skater);
            RetailFootDrag(*Hud.Skater);
            RetailBoardSlide(*Hud.Skater);
        }
        else DriveSeams(SeamGate.Update(Now == EWhat::Ground, Dt), Mps, Dt);

        if (Hud.Skater)
            RetailTricks(*Hud.Skater, Hud.Spin, Dt);
        else
        {
            // Fallback for an engine without audio frames: trick sounds from the HUD's trick names.
            // Tricks (Skate 3 plays both audio fields of the trick through Class_Flips): the pop whoosh
            // (28, random flip samples 11-14) once per jump, then the trick's own flip sound once the
            // deck is spinning; the clothes rustle at each trick's start and when it lands.
            if (Hud.Tricks > 0 && Hud.Trick && Hud.Trick[0])
            {
                const int Type = TrickType(Hud.Trick);
                if (Type != AirTrickType && Type >= 0)
                {
                    Cloth(Type);
                    if (Now == EWhat::Air || bOnBoardGround)  // a trick can register on the pop frame
                    {
                        if (Type != SndGrab && !bWhooshThisJump)
                        {
                            StartFlip(Type == SndNollie ? SndNollie : SndOllie, Hud.Spin);
                            bWhooshThisJump = true;
                        }
                        if (!bFlipThisJump && Type != SndOllie && Type != SndNollie && Type != SndGrab)
                        {
                            // The trick registers as the flick lands; the deck reaches its spin a few
                            // frames later (engine test: kickflip ~3 -> 9 rad/s over 50 ms).
                            PendingFlip = Type;
                            PendingFlipTime = 0.05f;
                            bFlipThisJump = true;
                        }
                    }
                    AirTrickType = Type;
                }
            }
            // Board catch: the feet stop the flip. Detected from the deck's own spin dropping
            // after a real flip (Skate_Collisions 127-130, matched against Skate 3 gameplay).
            if (Now == EWhat::Air)
            {
                const float Spin = std::fmax(std::fabs(Hud.Spin[1]), std::fabs(Hud.Spin[2]));
                PeakSpin = std::fmax(PeakSpin, Spin);
                if (!bCaught && PeakSpin > 5.f && Spin < 1.5f)
                {
                    // (catch click disabled: not present in Skate 3 gameplay recordings)
                    bCaught = true;
                }
            }
            if (PendingFlip >= 0 && (PendingFlipTime -= Dt) <= 0.f)
            {
                // (trick-specific flip sound disabled: never heard in Skate 3 gameplay)
                PendingFlip = -1;
            }
            if (Last == EWhat::Air && Now != EWhat::Air)
            {
                if (AirTrickType >= 0) Cloth(AirTrickType);
                AirTrickType = -1;
                bFlipThisJump = false;
                bWhooshThisJump = false;
                PendingFlip = -1;
                PeakSpin = 0;
                bCaught = false;
            }
        }
        for (auto it = Flips.begin(); it != Flips.end();)
        {
            it->Age += Dt;
            if (it->Age > 1.5f) { Snd.Destroy(it->Id); it = Flips.erase(it); }
            else ++it;
        }
    }

    // Score multiplier stingers (Skate 3's HUD sounds, sk8_menu bank): one per level reached,
    // listed in sounds.txt as mult_1.5, mult_2, mult_3 ...
    if (Hud.Multiplier > LastMultiplier + 0.2f)
    {
        char Key[32];
        const float Level = std::round(Hud.Multiplier * 2.f) / 2.f;
        snprintf(Key, sizeof(Key), "mult_%g", Level);
        Play(Key, 1.f);
    }
    LastMultiplier = Hud.Multiplier;

    // Footsteps / pushes. Contact events repeat every tick while the foot is down: a new step is
    // one that follows a gap.
    if (bAems)
    {
        bool bStep = false;
        int Strength = 700;
        SinceFootEvent += Dt;
        SinceStepEvent += Dt;
        SinceFoley += Dt;
        if (Hud.Pushes > 0)
        {
            if (SinceFootEvent > 0.12f) { bStep = true; Strength = int(std::fmin(1000.f, 450.f + Hud.PushSpeed * 120.f)); }
            SinceFootEvent = 0.f;
        }
        if (Hud.Steps > 0 && Now == EWhat::OffBoard)
        {
            if (SinceStepEvent > 0.12f) bStep = true;
            SinceStepEvent = 0.f;
        }
        if (!Footstep.Id) Footstep.Set(FootValues(Strength, -1));
        if (bStep)
        {
            bRightFoot = !bRightFoot;
            Footstep.Set(FootValues(Strength, bRightFoot ? 11 : 8));
            // 0x824D7AB0 / 0x824814D0: the clothes foley of a step (sk8_foley), by whether it is a
            // fresh step (400 ms since the last one, C1831BDB E82237E2) and the footstep surface
            // class (AudioSurfaceMap word 6). Foot type 1 (a flat step: the planted foot's height
            // change, Audio+0xD4/0xD5, is not published) picks the first two tables.
            static const int Fresh[7] = { 107, 102, 105, 105, 105, 99, 101 };
            static const int Quick[7] = { 86, 82, 86, 91, 86, 78, 75 };
            const int Surface = SkaterAudio::FootSurface() - 1;
            if (Surface >= 0 && Surface < 7)
            {
                auto B = SplcBanks.find("sk8_foley");
                if (B != SplcBanks.end() && !B->second.Events.empty())
                    PlaySplcId(B->second, (SinceFoley >= 0.4f ? Fresh : Quick)[Surface], 1.f, 1.f);
            }
            SinceFoley = 0.f;
        }
        else if (Footstep.Values.size() > 11 && (Footstep.Values[8] || Footstep.Values[11]))
            Footstep.Set(FootValues(Strength, -1));  // release the trigger
    }

    // (no UI tick per trick: Skate 3 gameplay has none)
    // Hall of Meat front-end sounds (Skate 3 'fe' entries; their volumes are in sounds.txt).
    for (int i = 0; i < Hud.FeCount; i++) Play(Hud.Fe[i], 1.f);
    // (No sound when a line ends / the multiplier runs out: Skate 3 is silent there.)
    // (No HUD sound on a bail either: Skate 3 is silent when a line is lost.)

    Last = Now;
    LastVz = Vz;
}

// Sound must never break skating: any fault turns the mod's sound off for the session.
static bool bBroken = false;

static void Broke(const char* Where, unsigned Code)
{
    bBroken = true;
    bActive = false;
    Log("Audio: exception 0x%08X in %s; sound turned off for this session", Code, Where);
}

void Audio::Start()
{
    if (bBroken) return;
    __try { StartUnsafe(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Broke("Start", GetExceptionCode()); }
}

void Audio::Stop()
{
    if (bBroken) return;
    __try { StopUnsafe(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Broke("Stop", GetExceptionCode()); }
}

void Audio::Update(const SkState& Sk, float Dt, const Cues& Hud)
{
    if (bBroken) return;
    __try { UpdateUnsafe(Sk, Dt, Hud); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Broke("Update", GetExceptionCode()); }
}

Audio::Stats Audio::GetStats()
{
    Stats S;
    if (!bReady || bBroken) return S;
    if (bAems) { S.Voices = Snd.ActiveVoices(); S.Modules = Snd.ModuleCount(); S.Classes = Snd.ClassInstanceCount(); }
    for (const Shot& Sh : Shots) S.OneShots += Sh.bUsed ? 1 : 0;
    return S;
}

void Audio::Preload()
{
    if (bBroken) return;
    __try { PreloadUnsafe(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Broke("Preload", GetExceptionCode()); }
}
