#pragma once

// Skate 3's sound engine, ported from the game (default.xex):
//  - AEMS modules: each ABK bank holds dataflow graphs ("modules") run at the control rate by a
//    small VM (FUN_82af5c20) whose node operations are the 40 handlers of table 0x82fb3600.
//  - Classes: the game starts a sound by creating an instance of a class (Class_grind,
//    Class_rolling...) with a list of integer values; every module bound to that class runs
//    with those values as its inputs (FUN_828bb268 / 828bb438 / 828bb398).
//  - Voices: player nodes start rw::audio voices: SndPlayer > Resample (pitch) > HighPassIir2 >
//    LowPassIir2 > Send > Gain > Pan2D, driven by voice parameters 0..8 (FUN_82af37c0).
// Independent of Unreal: Mix() renders stereo float audio for any output (miniaudio in the mod).

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Aems
{
    struct Sample
    {
        std::vector<float> Pcm;  // interleaved
        int Channels = 1;
        int Rate = 48000;
        bool Loop = false;
        uint32_t LoopStart = 0, LoopEnd = 0;  // frames
    };

    class Engine;

    // One loaded ABK bank (big-endian, relocated in place to its base address).
    struct Bank
    {
        std::string Name;
        uint32_t Base = 0;
        std::vector<uint8_t> Data;
        std::vector<uint32_t> Modules;                 // file offsets of the module headers
        std::unordered_map<int, Sample> Samples;       // by bank sample index
    };

    // A running module instance (memory copied from the module's template).
    struct Instance
    {
        const Bank* Bank = nullptr;
        uint32_t Module = 0;           // file offset of the module header
        uint32_t Base = 0;             // address of the instance memory
        std::vector<uint8_t> Mem;
        uint32_t Program = 0;          // address of the program
        uint32_t StopNode = 0, ParamNode = 0;
        std::vector<uint32_t> ImportNodes, MessageNodes;
        std::vector<std::string> MessageNames;  // message each message node subscribes to
        bool Finished = false;
        int ClassInstance = 0;         // owner (id)
    };

    struct Voice;

    class Engine
    {
    public:
        Engine();
        ~Engine();
        // Root: skate-data/aems (bank.abk, bank.txt, bank/<idx>.wav)
        bool LoadBank(const std::string& Root, const std::string& Name);
        void SetLog(void (*Fn)(const char*)) { LogFn = Fn; }
        void Seed(uint64_t S) { RngState = S ? S : 1; }

        // Sound classes, as the game uses them.
        int Create(const std::string& ClassName, const std::vector<int32_t>& Values);  // 0 = nothing bound
        void Update(int Instance, const std::vector<int32_t>& Values);
        void Destroy(int Instance);          // asks the modules to stop (they may release / tail off)
        bool Alive(int Instance) const;

        void SetController(const std::string& Name, int32_t Value);

        // Game thread: runs the module programs at the control rate (41.6 Hz in Skate 3).
        void Tick(double Seconds);
        // Audio thread: mixes the voices into interleaved stereo float.
        void Mix(float* Out, uint32_t Frames, uint32_t Rate);

        std::vector<std::string> ClassNames() const
        {
            std::vector<std::string> Out;
            for (auto& [K, V] : Classes) Out.push_back(K);
            return Out;
        }
        int ParamCount(const std::string& ClassName);   // values the class takes (param node size)

        float MasterGain = 1.0f;
        bool Trace = false;
        int ActiveVoices() const;
        // Diagnostics: module programs and sound class instances alive (a count that only grows = a leak).
        int ModuleCount() const { return int(Instances.size()); }
        int ClassInstanceCount() const { return int(ClassInstances.size()); }

    private:
        // ---- address space ----
        uint8_t* Ptr(uint32_t Addr, uint32_t Size = 4);
        int32_t R32(uint32_t a);
        uint32_t U32(uint32_t a) { return uint32_t(R32(a)); }
        void W32(uint32_t a, int32_t v);
        float RF(uint32_t a);
        void WF(uint32_t a, float v);
        uint16_t R16(uint32_t a);
        int16_t S16(uint32_t a) { return int16_t(R16(a)); }
        void W16(uint32_t a, uint16_t v);
        uint8_t R8(uint32_t a);
        int8_t S8(uint32_t a) { return int8_t(R8(a)); }
        void W8(uint32_t a, uint8_t v);

        // ---- VM ----
        void RunInstance(Instance& I);
        int32_t Handler(int Op, uint32_t Node);
        int32_t Player(uint32_t Node);
        uint32_t Random();

        // ---- voices (shared with the audio thread) ----
        int StartVoice(uint32_t Node, const Sample* S);
        void SetVoiceParam(int Voice, int Id, int32_t Value);
        void StopVoice(int Voice);
        void PauseVoice(int Voice, bool Pause);
        bool VoicePlaying(int Voice, int32_t* RemainingMs, int32_t* ElapsedMs);

        struct ClassInst
        {
            std::vector<Instance*> Modules;
            bool Stopping = false;
        };

        std::vector<std::unique_ptr<Bank>> Banks;
        std::unordered_map<std::string, std::vector<std::pair<Bank*, uint32_t>>> Classes;  // class -> modules
        std::vector<std::unique_ptr<Instance>> Instances;
        std::unordered_map<int, ClassInst> ClassInstances;
        std::unordered_map<std::string, int32_t> Controllers;
        std::unordered_map<uint32_t, std::pair<uint8_t, std::string>> ImportNames;  // field address -> (kind, name)
        int NextClassInstance = 1;
        uint32_t NextInstanceBase = 0x40000000;
        Instance* Cur = nullptr;
        double Clock = 0;
        float DtMs = 1000.0f / 41.6f;
        uint64_t RngState = 0x853c49e6748fea9bull;

        mutable std::mutex VoiceLock;
        std::unordered_map<int, std::unique_ptr<Voice>> Voices;
        int NextVoice = 1;
        void (*LogFn)(const char*) = nullptr;
        void Log(const char* Fmt, ...);
    };
}
