#pragma once

// Port of Skate 3's grain player (default.xex 0x828C4318..0x828C5118), which plays the rolling
// sound from one long recording of a board rolling over a surface (data/audio/grains.big, decoded
// to skate-data/sounds/grains/<name>.wav): two voices take turns playing short grains (attack,
// sustain, release) from the part of the recording the speed picks, crossfading into each other.
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Grain
{
    // GrainPlayer::GrainParams (attrib D18D1174735E5CDE): attack, sustain, release, window (s) and
    // the position change that restarts the grain early.
    struct Params { float Attack = 0.1f, Sustain = 0.2f, Release = 0.1f, Window = 1.6f, Threshold = 0.05f; };

    struct Recording
    {
        std::vector<float> Pcm;  // mono
        uint32_t Rate = 44100;
        float Seconds() const { return Rate ? float(Pcm.size()) / float(Rate) : 0.f; }
    };
    std::shared_ptr<const Recording> Load(const std::string& WavPath);

    class Player
    {
    public:
        void Start(std::shared_ptr<const Recording> Rec, const Params& P);  // 0x828C46A8 + first grain
        void Stop();                                                        // 0x828C45F8
        bool Running() const;
        // Per game frame (the board component writes these straight into the player): gain,
        // position in the recording (0..1, from the speed curve) and playback rate.
        void Set(float Gain, float Position, float Pitch);
        // Audio thread: adds this player to a stereo float buffer.
        void Mix(float* Out, uint32_t Frames, uint32_t Rate);

    private:
        struct Voice
        {
            bool Active = false;
            int State = 0;          // 1 attack, 2 sustain, 3 release
            float Timer = 0;
            float StartTarget = 0;  // the position control when the grain started
            double Pos = 0;         // samples
            float Env = 0, Slope = 0;
        };
        struct Used { float Start, End; };
        void Tick(float Dt);                 // 0x828C4D58
        float PickStart();                   // 0x828C5118
        void StartGrain(int V, float Start); // 0x828C4A58 / 0x828C4870
        void Release(int V);                 // 0x828C4960
        uint32_t NextRandom();

        mutable std::mutex Lock;
        std::shared_ptr<const Recording> Rec;
        Params P;
        bool bRunning = false;
        float Gain = 0, Position = 0, Pitch = 1;
        float MixGain = 0;
        Voice Voices[2];
        int Cur = 0;
        std::vector<Used> UsedList;          // recently played grains, by start (16 slots)
        int LastUsed = -1;
        uint32_t Seed = 0x1234567;
    };
}
