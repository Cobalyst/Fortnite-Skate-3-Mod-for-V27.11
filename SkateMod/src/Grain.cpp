#include "Grain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace Grain
{
    std::shared_ptr<const Recording> Load(const std::string& WavPath)
    {
        std::ifstream In(WavPath, std::ios::binary);
        if (!In) return nullptr;
        std::vector<char> D((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        if (D.size() < 44 || memcmp(D.data(), "RIFF", 4) != 0 || memcmp(D.data() + 8, "WAVE", 4) != 0) return nullptr;
        auto U32 = [&](size_t O) { uint32_t V; memcpy(&V, &D[O], 4); return V; };
        auto U16 = [&](size_t O) { uint16_t V; memcpy(&V, &D[O], 2); return V; };
        uint16_t Channels = 1, Bits = 16;
        auto R = std::make_shared<Recording>();
        for (size_t O = 12; O + 8 <= D.size();)
        {
            const uint32_t Size = U32(O + 4);
            const size_t Body = O + 8;
            if (Body + Size > D.size()) break;
            if (!memcmp(&D[O], "fmt ", 4) && Size >= 16)
            {
                Channels = std::max<uint16_t>(1, U16(Body + 2));
                R->Rate = U32(Body + 4);
                Bits = U16(Body + 14);
            }
            else if (!memcmp(&D[O], "data", 4) && Bits == 16)
            {
                const size_t Frames = Size / (2u * Channels);
                R->Pcm.resize(Frames);
                for (size_t i = 0; i < Frames; i++)
                {
                    int Sum = 0;
                    for (uint16_t c = 0; c < Channels; c++) { int16_t S; memcpy(&S, &D[Body + (i * Channels + c) * 2], 2); Sum += S; }
                    R->Pcm[i] = float(Sum) / (32768.f * float(Channels));
                }
            }
            O = Body + Size + (Size & 1);
        }
        if (R->Pcm.empty()) return nullptr;
        return R;
    }

    uint32_t Player::NextRandom()
    {
        Seed ^= Seed << 13; Seed ^= Seed >> 17; Seed ^= Seed << 5;
        return Seed;
    }

    void Player::Start(std::shared_ptr<const Recording> InRec, const Params& InP)
    {
        std::lock_guard<std::mutex> L(Lock);
        Rec = std::move(InRec);
        P = InP;
        for (Voice& V : Voices) V = Voice{};
        UsedList.clear();
        LastUsed = -1;
        Cur = 0;
        MixGain = Gain;
        bRunning = Rec != nullptr;
        if (bRunning) StartGrain(0, PickStart());
    }

    void Player::Stop()
    {
        std::lock_guard<std::mutex> L(Lock);
        bRunning = false;
        // (the voices fade out over their release time in Mix)
        for (int v = 0; v < 2; v++)
            if (Voices[v].Active && Voices[v].State != 3) Release(v);
    }

    bool Player::Running() const
    {
        std::lock_guard<std::mutex> L(Lock);
        return bRunning;
    }

    void Player::Set(float InGain, float InPosition, float InPitch)
    {
        std::lock_guard<std::mutex> L(Lock);
        Gain = InGain;
        Position = InPosition;
        Pitch = InPitch;
    }

    // 0x828C5118: the start of the next grain. The window is the part of the recording the
    // position control points at; inside it the grain-length slots not used recently (a sorted
    // list of up to 15 played grains) are candidates, and one of the first half is picked at
    // random. With no free slot (or a full list) the list is cleared except the newest grain.
    float Player::PickStart()
    {
        const float Dur = Rec ? Rec->Seconds() : 0.f;
        const float Len = P.Attack + P.Sustain + P.Release;
        const float Win = P.Window;
        const float WinStart = (Dur - Win) * std::clamp(Position, 0.f, 1.f);
        const float WinEnd = WinStart + Win;
        const int Slots = Len > 0.f ? int(Win / Len) : 0;
        for (int Pass = 0; Pass < 2; Pass++)
        {
            std::vector<Used> Cand;
            auto Gap = [&](float A, float B) {   // 0x828C5070
                if (B < WinStart || A > WinEnd) return;
                A = std::max(A, WinStart);
                B = std::min(B, WinEnd);
                while (B - A >= Len && Cand.size() < 64)
                {
                    Cand.push_back({ A, A + Len });
                    A += Len;
                    if (A + Len > WinEnd) break;
                }
            };
            Gap(WinStart, UsedList.empty() ? Dur : UsedList.front().Start);
            for (size_t i = 0; i < UsedList.size(); i++)
                Gap(UsedList[i].End, i + 1 < UsedList.size() ? UsedList[i + 1].Start : Dur);
            if (!Cand.empty() && UsedList.size() < 15)
            {
                const float R01 = float(NextRandom() >> 1) * 4.656612873e-10f;
                const size_t Pick = std::min(Cand.size() - 1, size_t(R01 * 0.5f * float(Cand.size())));
                const Used G = Cand[Pick];
                const auto It = std::lower_bound(UsedList.begin(), UsedList.end(), G.Start,
                                                 [](const Used& U, float S) { return U.Start < S; });
                LastUsed = int(It - UsedList.begin());
                UsedList.insert(It, G);
                return G.Start;
            }
            // Reset: keep only the newest grain.
            if (LastUsed >= 0 && LastUsed < int(UsedList.size())) { const Used Keep = UsedList[size_t(LastUsed)]; UsedList.assign(1, Keep); LastUsed = 0; }
            else UsedList.clear();
            if (Slots < 2) return WinStart;
        }
        return WinStart;
    }

    // 0x828C4A58 (+0x828C4870): a grain starts at Start seconds, fading in over the attack.
    void Player::StartGrain(int V, float Start)
    {
        Voice& G = Voices[V];
        const float Dur = Rec ? Rec->Seconds() : 0.f;
        G.Active = true;
        G.State = 1;
        G.Timer = P.Attack;
        G.StartTarget = Position;
        G.Pos = double(std::clamp(Start, 0.f, Dur)) * double(Rec ? Rec->Rate : 44100);
        G.Env = 0.f;
        G.Slope = P.Attack > 0.f ? 1.f / P.Attack : 1000.f;
    }

    // 0x828C4960: fade out over the release.
    void Player::Release(int V)
    {
        Voice& G = Voices[V];
        G.State = 3;
        G.Timer = P.Release;
        G.Slope = P.Release > 0.f ? -G.Env / P.Release : -1000.f;
    }

    // 0x828C4D58: moves the position control far enough and the current grain hands over to a
    // new one; a grain's sustain runs at the playback rate; at the end of the sustain the next
    // grain starts in the other voice while this one releases.
    void Player::Tick(float Dt)
    {
        Voice& C = Voices[Cur];
        if (bRunning && C.Active && std::fabs(C.StartTarget - Position) > P.Threshold)
        {
            const int O = (Cur + 1) % 2;
            if (!Voices[O].Active)
            {
                if (C.State != 3) Release(Cur);
                StartGrain(O, PickStart());
                Cur = O;
            }
            else if (Voices[O].State != 3) Release(O);
        }
        for (int v = 0; v < 2; v++)
        {
            Voice& G = Voices[v];
            if (!G.Active) continue;
            G.Timer -= G.State == 2 ? Dt * Pitch : Dt;
            if (G.Timer >= 0.f) continue;
            if (G.State == 1) { G.State = 2; G.Timer = P.Sustain; G.Env = 1.f; G.Slope = 0.f; }
            else if (G.State == 2)
            {
                Release(v);
                if (bRunning)
                {
                    const int O = (v + 1) % 2;
                    if (Voices[O].Active) Voices[O] = Voice{};
                    StartGrain(O, PickStart());
                    Cur = O;
                }
            }
            else G = Voice{};
        }
    }

    void Player::Mix(float* Out, uint32_t Frames, uint32_t Rate)
    {
        std::lock_guard<std::mutex> L(Lock);
        if (!Rec || Frames == 0 || Rate == 0) return;
        Tick(float(Frames) / float(Rate));
        const float* Pcm = Rec->Pcm.data();
        const size_t Total = Rec->Pcm.size();
        const double Step = double(Pitch) * double(Rec->Rate) / double(Rate);
        const float G0 = MixGain, G1 = Gain;
        MixGain = Gain;
        for (Voice& G : Voices)
        {
            if (!G.Active) continue;
            const float EnvStep = G.Slope / float(Rate);
            for (uint32_t f = 0; f < Frames; f++)
            {
                const size_t i0 = size_t(G.Pos);
                if (i0 + 1 >= Total) { G.Active = false; break; }
                const float t = float(G.Pos - double(i0));
                const float S = Pcm[i0] + (Pcm[i0 + 1] - Pcm[i0]) * t;
                const float Gain_ = G0 + (G1 - G0) * (float(f) / float(Frames));
                const float V = S * G.Env * Gain_;
                Out[2 * f] += V;
                Out[2 * f + 1] += V;
                G.Pos += Step;
                G.Env = std::clamp(G.Env + EnvStep, 0.f, 1.f);
            }
        }
    }
}
