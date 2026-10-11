#include "Aems.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Aems
{
    // ------------------------------------------------------------------ voices (DSP)

    // RBJ biquad (Butterworth, Q = 1/sqrt 2), per channel state; the IIR2 filters of rw::audio.
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1[2] = {}, z2[2] = {};
        float Freq = -1;
        bool High = false, Bypass = true;
        void Set(float F, float Rate, bool Hp)
        {
            if (F == Freq && Hp == High) return;
            Freq = F;
            High = Hp;
            Bypass = Hp ? F <= 10.f : F >= Rate * 0.45f;
            if (Bypass) return;
            if (F > Rate * 0.45f) F = Rate * 0.45f;  // a high-pass above Nyquist would be unstable
            const float w = 2.f * 3.14159265f * F / Rate, c = std::cos(w), s = std::sin(w), alpha = s / (2.f * 0.70710678f);
            const float a0 = 1.f + alpha;
            if (Hp) { b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = (1 + c) / 2 / a0; }
            else { b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = (1 - c) / 2 / a0; }
            a1 = -2 * c / a0;
            a2 = (1 - alpha) / a0;
        }
        float Run(int Ch, float x)
        {
            if (Bypass) return x;
            const float y = b0 * x + z1[Ch];
            z1[Ch] = b1 * x - a1 * y + z2[Ch];
            z2[Ch] = b2 * x - a2 * y;
            return y;
        }
    };

    struct Voice
    {
        const Sample* S = nullptr;
        double Pos = 0;
        float Pitch = 1;          // param 0 / 4096
        float GainA = 1, GainB = 1, GainSend = 1;  // params 2, 8, 5 (/32767)
        float PanAngle = 0;       // param 3, degrees clockwise from front
        float Lpf = 25000, Hpf = 0;
        bool Paused = false, Done = false, Stop = false;
        Biquad Lp, Hp;
        float CurL = 0, CurR = 0;  // smoothed gains (no zipper noise)
        bool First = true;
    };

    Engine::Engine() = default;
    Engine::~Engine() = default;

    void Engine::Log(const char* Fmt, ...)
    {
        if (!LogFn) return;
        char Buf[512];
        va_list A;
        va_start(A, Fmt);
        vsnprintf(Buf, sizeof(Buf), Fmt, A);
        va_end(A);
        LogFn(Buf);
    }

    // ------------------------------------------------------------------ loading

    static uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
    static void PutBE32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

    static bool ReadWav(const std::string& Path, Sample& S)
    {
        // One read for the whole file (byte-by-byte stream iterators were very slow on a cold disk).
        std::ifstream In(Path, std::ios::binary | std::ios::ate);
        if (!In) return false;
        const std::streamsize Bytes = In.tellg();
        if (Bytes <= 0) return false;
        std::vector<uint8_t> D(static_cast<size_t>(Bytes));
        In.seekg(0);
        if (!In.read(reinterpret_cast<char*>(D.data()), Bytes)) return false;
        if (D.size() < 44 || memcmp(D.data(), "RIFF", 4) || memcmp(D.data() + 8, "WAVE", 4)) return false;
        size_t p = 12;
        int Bits = 16;
        while (p + 8 <= D.size())
        {
            const uint32_t Size = D[p + 4] | (D[p + 5] << 8) | (D[p + 6] << 16) | (uint32_t(D[p + 7]) << 24);
            if (!memcmp(D.data() + p, "fmt ", 4))
            {
                S.Channels = D[p + 10] | (D[p + 11] << 8);
                S.Rate = int(D[p + 12] | (D[p + 13] << 8) | (D[p + 14] << 16) | (uint32_t(D[p + 15]) << 24));
                Bits = D[p + 22] | (D[p + 23] << 8);
            }
            else if (!memcmp(D.data() + p, "data", 4))
            {
                const size_t n = std::min<size_t>(Size, D.size() - p - 8) / (Bits / 8);
                S.Pcm.resize(n);
                const uint8_t* q = D.data() + p + 8;
                for (size_t i = 0; i < n; i++)
                    S.Pcm[i] = Bits == 16 ? int16_t(q[2 * i] | (q[2 * i + 1] << 8)) / 32768.f : 0.f;
                return S.Channels > 0;
            }
            p += 8 + Size + (Size & 1);
        }
        return false;
    }

    bool Engine::LoadBank(const std::string& Root, const std::string& Name)
    {
        auto B = std::make_unique<Bank>();
        B->Name = Name;
        {
            std::ifstream In(Root + "\\" + Name + ".abk", std::ios::binary | std::ios::ate);
            if (!In) { Log("AEMS: no bank %s", Name.c_str()); return false; }
            B->Data.resize(size_t(In.tellg()));
            In.seekg(0);
            In.read(reinterpret_cast<char*>(B->Data.data()), std::streamsize(B->Data.size()));
        }
        auto& D = B->Data;
        if (D.size() < 0x60 || memcmp(D.data(), "ABKC", 4)) { Log("AEMS: %s is not an ABKC bank", Name.c_str()); return false; }
        B->Base = 0x10000000u + uint32_t(Banks.size()) * 0x01000000u;

        // Pointer relocations (list at header 0x34): data pointers become absolute addresses.
        const uint32_t Reloc = BE32(&D[0x34]);
        for (uint32_t i = 0, n = BE32(&D[Reloc]); i < n; i++)
        {
            const uint32_t o = BE32(&D[Reloc + 4 + 4 * i]);
            if (o + 4 <= D.size()) PutBE32(&D[o], BE32(&D[o]) + B->Base);
        }
        // Modules: header table at 0x1C, each 0x3C bytes + 4 per player / class.
        const uint16_t NumModules = uint16_t((D[0x0A] << 8) | D[0x0B]);
        uint32_t t = BE32(&D[0x1C]);
        for (int m = 0; m < NumModules && t + 0x3C <= D.size(); m++)
        {
            B->Modules.push_back(t);
            t += 0x3C + (D[t + 0x24] + D[t + 0x27]) * 4;
        }
        // Imports (list at header 0x38): (field, -> {u16 project, u16 hash, name}, kind).
        // Kind 1 at a module header + 4 binds that module to a sound class.
        const uint32_t Imports = BE32(&D[0x38]);
        for (uint32_t i = 0, n = BE32(&D[Imports]); i < n; i++)
        {
            const uint32_t e = Imports + 4 + 12 * i;
            auto Off = [&](uint32_t v) { return v >= B->Base ? v - B->Base : v; };
            const uint32_t Field = Off(BE32(&D[e])), NamePtr = Off(BE32(&D[e + 4]));
            if (NamePtr + 5 >= D.size()) continue;
            const uint8_t Kind = D[e + 8];
            const std::string ImportName(reinterpret_cast<const char*>(&D[NamePtr + 4]));
            for (uint32_t Mod : B->Modules)
                if (Kind == 1 && Field == Mod + 4)
                    Classes[ImportName].push_back({ B.get(), Mod });
            ImportNames[B->Base + Field] = { Kind, ImportName };
        }
        // Samples (decoded once by Tools/build_aems_assets.py) with their loop points.
        std::ifstream Meta(Root + "\\" + Name + ".txt");
        std::string Line;
        int Loaded = 0;
        while (std::getline(Meta, Line))
        {
            if (Line.empty() || Line[0] == '#') continue;
            std::istringstream L(Line);
            int Idx, Rate, Ch, Frames, Loop, Ls, Le;
            if (!(L >> Idx >> Rate >> Ch >> Frames >> Loop >> Ls >> Le)) continue;
            Sample S;
            if (!ReadWav(Root + "\\" + Name + "\\" + std::to_string(Idx) + ".wav", S)) continue;
            S.Loop = Loop != 0;
            S.LoopStart = uint32_t(Ls);
            S.LoopEnd = uint32_t(Le);
            B->Samples[Idx] = std::move(S);
            Loaded++;
        }
        Log("AEMS: bank %s: %zu modules, %d samples", Name.c_str(), B->Modules.size(), Loaded);
        Banks.push_back(std::move(B));
        return true;
    }

    // ------------------------------------------------------------------ address space

    uint8_t* Engine::Ptr(uint32_t a, uint32_t Size)
    {
        if (Cur && a >= Cur->Base && a + Size <= Cur->Base + Cur->Mem.size())
            return &Cur->Mem[a - Cur->Base];
        if (a >= 0x40000000u)
        {
            for (auto& I : Instances)
                if (a >= I->Base && a + Size <= I->Base + I->Mem.size()) return &I->Mem[a - I->Base];
        }
        else
        {
            for (auto& B : Banks)
                if (a >= B->Base && a + Size <= B->Base + B->Data.size()) return &B->Data[a - B->Base];
        }
        static uint8_t Dummy[16];
        memset(Dummy, 0, sizeof(Dummy));
        return Dummy;
    }
    int32_t Engine::R32(uint32_t a) { return int32_t(BE32(Ptr(a))); }
    void Engine::W32(uint32_t a, int32_t v) { PutBE32(Ptr(a), uint32_t(v)); }
    float Engine::RF(uint32_t a) { const uint32_t u = BE32(Ptr(a)); float f; memcpy(&f, &u, 4); return f; }
    void Engine::WF(uint32_t a, float v) { uint32_t u; memcpy(&u, &v, 4); PutBE32(Ptr(a), u); }
    uint16_t Engine::R16(uint32_t a) { const uint8_t* p = Ptr(a, 2); return uint16_t((p[0] << 8) | p[1]); }
    void Engine::W16(uint32_t a, uint16_t v) { uint8_t* p = Ptr(a, 2); p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
    uint8_t Engine::R8(uint32_t a) { return *Ptr(a, 1); }
    void Engine::W8(uint32_t a, uint8_t v) { *Ptr(a, 1) = v; }

    uint32_t Engine::Random()
    {
        RngState ^= RngState << 13;
        RngState ^= RngState >> 7;
        RngState ^= RngState << 17;
        return uint32_t(RngState >> 16);
    }

    static int32_t Round(float f) { return f >= 0 ? int32_t(f + 0.5f) : int32_t(f - 0.5f); }

    // ------------------------------------------------------------------ classes

    int Engine::Create(const std::string& ClassName, const std::vector<int32_t>& Values)
    {
        auto C = Classes.find(ClassName);
        if (C == Classes.end()) return 0;
        const int Id = NextClassInstance++;
        ClassInst& Ci = ClassInstances[Id];
        for (auto& [B, Mod] : C->second)
        {
            const uint8_t* H = &B->Data[Mod];
            auto I = std::make_unique<Instance>();
            I->Bank = B;
            I->Module = Mod;
            I->ClassInstance = Id;
            I->Program = B->Base + BE32(H + 0x28);                       // header fields are file offsets
            const uint32_t Tmpl = BE32(H + 0x2C), Size = BE32(H + 0x30);
            if (Tmpl + Size > B->Data.size()) continue;
            I->Mem.assign(B->Data.begin() + Tmpl, B->Data.begin() + Tmpl + Size);
            I->Base = NextInstanceBase;
            NextInstanceBase += (Size + 0xFFFF) & ~0xFFFFu;
            if (NextInstanceBase > 0xF0000000u) NextInstanceBase = 0x40000000;
            // Input layout (FUN_82af5210): stop node, imported controllers, parameters, messages.
            uint32_t p = I->Base + 0x18;
            const bool HasStop = H[0x25] != 0, HasParams = H[0x26] != 0;
            const uint16_t NumImports = uint16_t((H[0x20] << 8) | H[0x21]), NumMessages = uint16_t((H[0x22] << 8) | H[0x23]);
            Instance* Raw = I.get();
            Instances.push_back(std::move(I));
            Cur = Raw;
            if (HasStop) { Raw->StopNode = p; p += 0x14; }
            for (int k = 0; k < NumImports; k++) { Raw->ImportNodes.push_back(p); p += 0x1C; }
            if (HasParams) { Raw->ParamNode = p; p += (R8(p + 0x10) + 5) * 4; }
            for (int k = 0; k < NumMessages; k++)
            {
                Raw->MessageNodes.push_back(p);
                auto Nm = ImportNames.find(B->Base + Tmpl + (p - Raw->Base));
                Raw->MessageNames.push_back(Nm != ImportNames.end() ? Nm->second.second : std::string());
                p += (R8(p + 0x18) + 7) * 4;
            }
            // Imported controllers start with their current values.
            for (uint32_t n : Raw->ImportNodes)
            {
                auto Nm = ImportNames.find(B->Base + Tmpl + (n - Raw->Base));
                if (Nm != ImportNames.end())
                {
                    auto V = Controllers.find(Nm->second.second);
                    W32(n + 0x18, V == Controllers.end() ? 0 : V->second);
                }
            }
            Cur = nullptr;
            Ci.Modules.push_back(Raw);
        }
        Update(Id, Values);
        return Id;
    }

    void Engine::Update(int Id, const std::vector<int32_t>& Values)
    {
        auto C = ClassInstances.find(Id);
        if (C == ClassInstances.end()) return;
        for (Instance* I : C->second.Modules)
        {
            if (!I->ParamNode) continue;
            Cur = I;
            const int n = R8(I->ParamNode + 0x10);
            for (int k = 0; k < n; k++)
                W32(I->ParamNode + 0x14 + 4 * k, k < int(Values.size()) ? Values[size_t(k)] : 0);
            Cur = nullptr;
        }
    }

    void Engine::Destroy(int Id)
    {
        auto C = ClassInstances.find(Id);
        if (C == ClassInstances.end() || C->second.Stopping) return;
        C->second.Stopping = true;
        for (Instance* I : C->second.Modules)
        {
            Cur = I;
            if (I->StopNode) W32(I->StopNode + 0x10, 1);
            else I->Finished = true;
            Cur = nullptr;
        }
    }

    int Engine::ParamCount(const std::string& ClassName)
    {
        auto C = Classes.find(ClassName);
        if (C == Classes.end()) return 0;
        int Best = 0;
        for (auto& [B, Mod] : C->second)
        {
            const uint8_t* H = &B->Data[Mod];
            if (!H[0x26]) continue;
            uint32_t o = BE32(H + 0x2C) + 0x18;
            if (H[0x25]) o += 0x14;
            o += 0x1C * ((H[0x20] << 8) | H[0x21]);
            if (o + 0x11 <= B->Data.size()) Best = std::max(Best, int(B->Data[o + 0x10]));
        }
        return Best;
    }

    bool Engine::Alive(int Id) const { return ClassInstances.count(Id) != 0; }

    void Engine::SetController(const std::string& Name, int32_t Value)
    {
        auto It = Controllers.find(Name);
        if (It != Controllers.end() && It->second == Value) return;
        Controllers[Name] = Value;
        for (auto& I : Instances)
        {
            Cur = I.get();
            const uint32_t Tmpl = BE32(&I->Bank->Data[I->Module + 0x2C]);
            for (uint32_t n : I->ImportNodes)
            {
                auto Nm = ImportNames.find(I->Bank->Base + Tmpl + (n - I->Base));
                if (Nm != ImportNames.end() && Nm->second.second == Name) W32(n + 0x18, Value);
            }
        }
        Cur = nullptr;
    }

    // ------------------------------------------------------------------ VM

    void Engine::Tick(double Seconds)
    {
        Clock += Seconds;
        int Steps = 0;
        while (Clock >= DtMs / 1000.0 && Steps < 8)
        {
            Clock -= DtMs / 1000.0;
            Steps++;
            for (size_t i = 0; i < Instances.size(); i++)
                if (!Instances[i]->Finished) RunInstance(*Instances[i]);
            // Finished modules release their voices; class instances go when all modules are done.
            for (auto it = Instances.begin(); it != Instances.end();)
            {
                Instance& I = **it;
                if (I.Finished)
                {
                    auto C = ClassInstances.find(I.ClassInstance);
                    if (C != ClassInstances.end())
                    {
                        auto& Ms = C->second.Modules;
                        Ms.erase(std::remove(Ms.begin(), Ms.end(), &I), Ms.end());
                        if (Ms.empty()) ClassInstances.erase(C);
                    }
                    it = Instances.erase(it);
                }
                else ++it;
            }
        }
        if (Clock > 1.0) Clock = 0;
    }

    void Engine::RunInstance(Instance& I)
    {
        Cur = &I;
        uint32_t p = I.Program, Node = I.Base + 0x18;
        for (int Guard = 0; Guard < 4000; Guard++)
        {
            const uint8_t Op = R8(p);
            if (Op == 0xFF) break;
            const uint8_t Moves = R8(p + 1);
            const int32_t Val = Handler(Op, Node);
            uint32_t q = p + 4;
            for (int k = 0; k < Moves; k++, q += 8)
            {
                const int32_t Src = R32(q), Dst = R32(q + 4);
                W32(Node + uint32_t(Dst), Src == -1 ? Val : R32(Node + uint32_t(Src)));
            }
            Node += uint32_t(R32(q));
            p = q + 4;
        }
        if (I.Finished)
        {
            // stop every voice this module still owns
            uint32_t pp = I.Program, nn = I.Base + 0x18;
            for (int Guard = 0; Guard < 4000; Guard++)
            {
                const uint8_t Op = R8(pp);
                if (Op == 0xFF) break;
                const uint8_t Moves = R8(pp + 1);
                if (Op == 27 && R32(nn + 8)) { StopVoice(R32(nn + 8)); W32(nn + 8, 0); }
                const uint32_t q = pp + 4 + 8 * Moves;
                nn += uint32_t(R32(q));
                pp = q + 4;
            }
        }
        Cur = nullptr;
    }

    int32_t Engine::Handler(int Op, uint32_t n)
    {
        switch (Op)
        {
        case 0: { const int32_t v = R32(n + 0x10); W32(n + 0x10, 0); return v; }   // stop requested (one shot)
        case 1: return R32(n + 0x14);                                               // class parameter input
        case 2: return R32(n + 0x18);                                               // imported controller
        case 3: { const int32_t v = R32(n); W32(n, 0); return v; }                  // one shot
        case 4: if (R32(n + 0xC) != 0) Cur->Finished = true; return 0;              // end: finished flag
        case 5:                                                                     // send message
        {
            const int Cnt = R8(n + 9);
            uint32_t P = n + 0xC;
            if (R8(n + 8))
            {
                const uint32_t Lim = P;
                P = Lim + Cnt * 8;
                for (int i = 0; i < Cnt; i++)
                {
                    const int32_t Lo = R32(Lim + 8 * i), Hi = R32(Lim + 8 * i + 4), V = R32(P + 4 + 4 * i);
                    W32(P + 4 + 4 * i, std::max(Lo, std::min(Hi, V)));
                }
            }
            if (R32(P) != 0)
            {
                // FUN_828bb0e0: every module subscribed to this message gets the values
                // (receiver callback FUN_82af51d0: count byte +0x18, values +0x1C, received flag +0x19).
                auto Nm = ImportNames.find(Cur->Bank->Base + BE32(&Cur->Bank->Data[Cur->Module + 0x2C]) + (n - Cur->Base));
                if (Nm != ImportNames.end())
                {
                    int32_t Values[32];
                    for (int i = 0; i < Cnt && i < 32; i++) Values[i] = R32(P + 4 + 4 * i);
                    Instance* Saved = Cur;
                    for (auto& I : Instances)
                        for (size_t k = 0; k < I->MessageNodes.size() && k < I->MessageNames.size(); k++)
                            if (I->MessageNames[k] == Nm->second.second)
                            {
                                Cur = I.get();
                                const uint32_t R = I->MessageNodes[k];
                                const int Count = std::min<int>(R8(R + 0x18), std::min(Cnt, 32));
                                for (int i = 0; i < Count; i++) W32(R + 0x1C + 4 * i, Values[i]);
                                W8(R + 0x19, 1);
                            }
                    Cur = Saved;
                }
            }
            return 0;
        }
        case 6:                                                                     // counter within [lo, hi]
        {
            const int32_t Lo = R32(n), Hi = R32(n + 4), In = R32(n + 0x14);
            if (Lo <= In && In <= Hi) return In;
            if (R32(n + 0x10) > 0)
            {
                const int32_t c = S8(n + 0xC) + R32(n + 8);
                W32(n + 8, c);
                if (Hi < c) { W32(n + 8, Lo); return Lo; }
                if (c < Lo) W32(n + 8, Hi);
            }
            return R32(n + 8);
        }
        case 7:                                                                     // random on trigger
        {
            if (R32(n + 0xC) == 0) return R32(n + 8);
            const uint32_t Range = U32(n + 4);
            const int32_t v = int32_t(Range ? Random() % Range : 0) + R32(n);
            W32(n + 8, v);
            return v;
        }
        case 8:                                                                     // shuffle bag
        {
            if (R32(n + R16(n)) == 0) return R32(n + 0xC);
            const int Idx = R16(n + 8), Count = R16(n + 0xA), Flag = S8(n + 3);
            const int Span = Count - Flag - Idx;
            const int Pick = Idx + (Span > 0 ? int(Random() % uint32_t(Span)) : 0);
            int32_t Val;
            if (R8(n + 2) == 1)
            {
                Val = R8(n + 0x10 + Pick);
                W8(n + 0x10 + Pick, R8(n + 0x10 + Idx));
                W8(n + 0x10 + Idx, uint8_t(Val));
            }
            else
            {
                Val = R16(n + 0x10 + 2 * Pick);
                W16(n + 0x10 + 2 * Pick, R16(n + 0x10 + 2 * Idx));
                W16(n + 0x10 + 2 * Idx, uint16_t(Val));
            }
            Val += R32(n + 4);
            W32(n + 0xC, Val);
            if (Idx + 1 < Count) { W16(n + 8, uint16_t(Idx + 1)); W8(n + 3, 0); }
            else { W16(n + 8, 0); W8(n + 3, 1); }
            return Val;
        }
        case 9:                                                                     // weighted random
        {
            if (R32(n + 0x10) != 0)
            {
                const uint32_t r = Random() % 100, Table = U32(n);
                uint32_t Acc = 0;
                for (int i = 0; i < R32(n + 8); i++)
                {
                    Acc += uint32_t(S8(Table + 0x10 + i));
                    if (r < Acc) { W32(n + 0xC, R32(n + 4) + i); break; }
                }
            }
            return R32(n + 0xC);
        }
        case 10:                                                                    // hysteresis trigger
        {
            const int32_t a = R32(n), b = R32(n + 4), c = R32(n + 8), d = R32(n + 0xC), v = R32(n + 0x14);
            if (a <= v && v <= b)
            {
                if (R8(n + 0x10) == 0) { W8(n + 0x10, 1); W8(n + 0x11, 1); return 1; }
            }
            else if (c <= v && v <= d) W8(n + 0x10, 0);
            W8(n + 0x11, 0);
            return 0;
        }
        case 11:                                                                    // timer (ms)
        {
            float t = RF(n);
            if (R32(n + 8) != 0) t = 0.f;
            else if (t < 0.f) { W8(n + 4, 0); return 0; }
            if (float(R32(n + 0xC)) <= t) { W8(n + 4, 1); WF(n, -1.f); return 1; }
            WF(n, t + DtMs);
            W8(n + 4, 0);
            return 0;
        }
        case 12:                                                                    // first nonzero selects
        {
            const int k = R8(n + 2);
            const uint32_t In = n + R16(n);
            for (int i = 0; i < k; i++)
                if (R32(In + 4 * i) != 0) { W32(n + 4, R32(n + (i + 2) * 4)); break; }
            return R32(n + 4);
        }
        case 13:                                                                    // any nonzero
            for (int i = 0; i < R8(n); i++)
                if (R32(n + 4 + 4 * i) != 0) return 1;
            return 0;
        case 14:                                                                    // envelope
        {
            const int32_t Gate = R32(n + R16(n));
            const int Prev = R8(n + 2), Seg = R8(n + 3), NSeg = R8(n + 0x10), Sustain = R16(n + 0x12);
            auto Time = [&](int i) { return RF(n + 0x18 + 8 * i); };
            auto Level = [&](int i) { return RF(n + 0x1C + 8 * i); };
            auto Out = [&]() { W8(n + 2, uint8_t(Gate)); return Round(RF(n + 0xC)); };
            if (Gate == 1 && Prev == 0)
            {
                W8(n + 3, 0);
                WF(n + 0xC, RF(n + 0x14));
                WF(n + 4, Time(0));
                WF(n + 8, Time(0) != 0 ? (Level(0) - RF(n + 0x14)) / Time(0) * DtMs : 0.f);
                return Out();
            }
            if (Gate == 3 && Prev != 3 && Seg < Sustain)
            {
                W8(n + 3, uint8_t(Sustain));
                WF(n + 4, Time(Sustain));
                WF(n + 8, Time(Sustain) != 0 ? (Level(Sustain) - RF(n + 0xC)) / Time(Sustain) * DtMs : 0.f);
                return Out();
            }
            if ((Gate == 1 || Gate == 3) && Seg < NSeg)
            {
                const float Remain = RF(n + 4) - DtMs;
                WF(n + 4, Remain);
                if (Remain > 0.f) { WF(n + 0xC, RF(n + 8) + RF(n + 0xC)); return Out(); }
                const float L = Level(Seg);
                WF(n + 0xC, L);
                W8(n + 3, uint8_t(Seg + 1));
                if (Seg + 1 < NSeg)
                {
                    WF(n + 4, Time(Seg + 1));
                    WF(n + 8, Time(Seg + 1) != 0 ? (Level(Seg + 1) - L) / Time(Seg + 1) * DtMs : 0.f);
                }
                return Out();
            }
            if (Gate != 2) WF(n + 0xC, 0.f);
            return Out();
        }
        case 15:                                                                    // curve lookup
        {
            const int32_t In = R32(n + 0xC);
            if (In == R32(n + 4)) return R32(n + 8);
            const uint32_t T = U32(n);
            W32(n + 4, In);
            const int Kind = R8(T), Cnt = R16(T + 2);
            const int32_t Lo = R32(T + 4), Hi = R32(T + 8);
            const float Step = RF(T + 0xC);
            const int32_t x = std::min(std::max(In, Lo), Hi) - Lo;
            auto Entry = [&](int i) -> int32_t
            {
                if (Kind == 2) return S16(T + 0x10 + 2 * i);
                if (Kind == 1) return S8(T + 0x10 + i);
                return R32(T + 0x10 + 4 * i);
            };
            int32_t v;
            if (Step == 1.0f) v = Entry(x);
            else
            {
                const float fx = float(x) * Step;
                const int i0 = Round(fx - 0.5f);
                const int i1 = std::min(i0 + 1, Cnt - 1);
                v = Round(float(Entry(i1) - Entry(i0)) * (fx - float(i0)) + float(Entry(i0)));
            }
            W32(n + 8, v);
            return v;
        }
        case 16:                                                                    // delay line (ms)
        {
            const uint32_t In = n + R16(n);
            const int Size = R16(n + 2);
            if (R32(In + 4) != R32(n + 8))
            {
                const int32_t d = std::max(0, R32(In + 4));
                W32(n + 8, d);
                const int k = std::min(Round(float(d) / DtMs), Size - 1);
                W16(n + 4, uint16_t(R16(n + 6) + k));
            }
            int w = R16(n + 4), r = R16(n + 6);
            if (w >= Size) w -= Size;
            if (r >= Size) r = 0;
            W32(n + 0xC + 4 * w, R32(In));
            const int32_t v = R32(n + 0xC + 4 * r);
            W16(n + 6, uint16_t(r + 1));
            W16(n + 4, uint16_t(w + 1));
            return v;
        }
        case 17: { const int32_t i = R32(n + 4); return (i > 0 && i <= R8(n)) ? R32(n + 4 * (i + 1)) : 0; }  // select
        case 18:                                                                    // route to slot
        {
            const int Prev = S16(n + 2);
            W32(n + 4 * (Prev + 2), 0);
            const int32_t i = R32(n + 4);
            if (i > 0 && i <= R8(n)) { W32(n + 4 * (i + 2), R32(n + 8)); W16(n + 2, uint16_t(i)); }
            return R32(n + 0xC);
        }
        case 19: { int32_t m = R32(n + 4); for (int i = 1; i < R8(n); i++) m = std::min(m, R32(n + 4 + 4 * i)); return m; }
        case 20: { int32_t m = R32(n + 4); for (int i = 1; i < R8(n); i++) m = std::max(m, R32(n + 4 + 4 * i)); return m; }
        case 21:                                                                    // scale * product
        {
            float p = float(R32(n + 8));
            for (int i = 1; i < R8(n); i++) p *= float(R32(n + 8 + 4 * i));
            return Round(RF(n + 4) * p);
        }
        case 22: { int32_t s = R32(n + 4); for (int i = 1; i < R8(n); i++) s += R32(n + 4 + 4 * i); return s; }
        case 23: return R32(n) - R32(n + 4);
        case 24: return R32(n + 4) * R32(n);
        case 25: { const int32_t b = R32(n + 4); return b ? R32(n) / b : 0; }
        case 26: { const int32_t b = R32(n + 4); return b ? R32(n) % b : 0; }
        case 27: return Player(n);
        case 28:                                                                    // LFO
        {
            const int32_t Rate = R32(n + 8);
            if (Rate < 1) return 0;
            float Phase = RF(n + 4);
            const float Depth = float(R32(n + 0xC));
            while (Phase >= 1.f) Phase -= 1.f;
            float v;
            switch (S8(n))
            {
            case 0: v = std::sin(Phase * 6.2831853f) * Depth; break;
            case 1: v = Phase < 0.5f ? 0.f : Depth; break;
            case 2: v = Phase * Depth; break;
            default: v = (Phase >= 0.5f ? (1.f - Phase) : Phase) * Depth * 2.f; break;
            }
            WF(n + 4, Phase + DtMs / float(Rate));
            return Round(v);
        }
        case 29:                                                                    // slew limiter
        {
            const int32_t Target = R32(n + 0x18);
            float c = RF(n);
            if (float(Target) == c) return Target;
            if (Target != R32(n + 8) || R32(n + 0x10) != R32(n + 0xC))
            {
                W32(n + 8, Target);
                W32(n + 0xC, R32(n + 0x10));
                const int32_t t = R32(n + 0x10);
                if (t < 1) { WF(n, float(Target)); return Target; }
                WF(n + 4, (float(Target) - c) / float(t) * DtMs * (1.f / 4096.f));
            }
            const float Rate = RF(n + 4);
            c = float(R32(n + 0x14)) * Rate + c;
            if ((Rate < 0 && c < float(Target)) || (Rate >= 0 && c > float(Target))) c = float(Target);
            WF(n, c);
            return Round(c);
        }
        case 30: { int32_t s = R32(n + 8); for (int i = 1; i < R8(n); i++) s += R32(n + 8 + 4 * i); const int32_t l = R32(n + 4); return s <= l ? s : l; }
        case 31: { const int32_t d = R32(n + 4) - R32(n + 8); return R32(n) <= d ? d : R32(n); }
        case 32: { const int32_t p = R32(n + 8) * R32(n + 4); return p <= R32(n) ? p : R32(n); }
        case 33: { const int32_t a = R32(n), b = R32(n + 4); return b <= a ? b : a; }
        case 34: { const int32_t a = R32(n), b = R32(n + 4); return a <= b ? b : a; }
        case 35: return Round(float(R32(n + 8)) * float(R32(n + 4)) * RF(n));
        case 36: return R32(n + 4) + R32(n);
        case 37: { const int32_t v = R8(n + 0x19); W8(n + 0x19, 0); return v; }    // message received
        case 38: return 0;   // child class instance: not used by the skateboard banks we drive
        case 39:                                                                    // set controller
        {
            const int32_t v = R32(n + 0x14);
            if (R32(n + 0x10) != v)
            {
                W32(n + 0x10, v);
                const int32_t c = std::max(R32(n + 8), std::min(R32(n + 0xC), v));
                auto Nm = ImportNames.find(Cur->Bank->Base + BE32(&Cur->Bank->Data[Cur->Module + 0x2C]) + (n - Cur->Base));
                if (Nm != ImportNames.end())
                {
                    Instance* Saved = Cur;
                    SetController(Nm->second.second, c);
                    Cur = Saved;
                }
            }
            return 0;
        }
        default: return 0;
        }
    }

    // Player node (FUN_82af4bd0): +4 sample table, +8 voice, +C state, +D previous state,
    // +E slot count, +F/+11 status outputs, +14 sample index, +18 play state, +1C slots.
    int32_t Engine::Player(uint32_t n)
    {
        int32_t State = R32(n + 0x18);
        State = State < 0 ? 0 : std::min(State, 2);
        int Voice = R32(n + 8);
        if (State != S8(n + 0xC))
        {
            if (State == 0)
            {
                if (Voice) { StopVoice(Voice); W32(n + 8, 0); }
            }
            else if (State == 1)
            {
                if (Voice == 0)
                {
                    if (!(S8(n + 0xC) == 2 && S8(n + 0xD) == 1))
                    {
                        const uint32_t Table = U32(n + 4);
                        const int32_t Count = R32(Table);
                        int32_t Idx = R32(n + 0x14);
                        Idx = Idx < Count ? std::max(0, Idx) : Count - 1;
                        const uint32_t Entry = Table + 4 + 12 * uint32_t(Idx);
                        if (Count <= 0 || S16(Entry) == -1) W32(n + 8, 0);
                        else
                        {
                            auto S = Cur->Bank->Samples.find(R16(Entry));
                            const int V = S == Cur->Bank->Samples.end() ? 0 : StartVoice(n, &S->second);
                            W32(n + 8, V);
                            // FUN_82af6df0: every slot is sent once at start
                            for (int i = 0; i < R8(n + 0xE); i++)
                            {
                                const uint32_t Slot = n + 0x1C + 12 * i;
                                if (V) SetVoiceParam(V, R8(Slot), R32(Slot + 8));
                                W32(Slot + 4, R32(Slot + 8));
                            }
                        }
                    }
                }
                else PauseVoice(Voice, false);
            }
            else if (Voice) PauseVoice(Voice, true);
            const int8_t Prev = S8(n + 0xC);
            W8(n + 0xC, uint8_t(State));
            W8(n + 0xD, uint8_t(Prev));
        }
        Voice = R32(n + 8);
        if (State == 1 && Voice)
        {
            // FUN_82af6f20: changed slots go to the voice; status outputs follow the slots.
            const int Slots = R8(n + 0xE);
            for (int i = 0; i < Slots; i++)
            {
                const uint32_t Slot = n + 0x1C + 12 * i;
                if (R32(Slot + 8) != R32(Slot + 4)) { SetVoiceParam(Voice, R8(Slot), R32(Slot + 8)); W32(Slot + 4, R32(Slot + 8)); }
            }
            int32_t Remaining = 0, Elapsed = 0;
            if (!VoicePlaying(Voice, &Remaining, &Elapsed))
            {
                StopVoice(Voice);
                W32(n + 8, 0);
                if (R8(n + 0xF)) { const uint32_t o = n + 0x1C + 12 * Slots; W32(o, 0); W32(o + 4, 0); }
                return 0;
            }
            uint32_t o = n + 0x1C + 12 * Slots;
            if (R8(n + 0xF)) { W32(o, Remaining); W32(o + 4, Elapsed); o += 8; }
            return 1;
        }
        return 0;
    }

    // ------------------------------------------------------------------ voices

    int Engine::StartVoice(uint32_t, const Sample* S)
    {
        if (!S || S->Pcm.empty()) return 0;
        std::lock_guard<std::mutex> L(VoiceLock);
        if (Voices.size() >= 96) return 0;
        auto V = std::make_unique<Voice>();
        V->S = S;
        const int Id = NextVoice++;
        if (Trace) Log("  voice %d start (%d frames @ %d Hz, loop %d)", Id, int(S->Pcm.size() / size_t(S->Channels)), S->Rate, int(S->Loop));
        Voices[Id] = std::move(V);
        return Id;
    }

    // FUN_82af37c0 + voice SetParam (FUN_82af5e28 / 82af60b0)
    void Engine::SetVoiceParam(int Id, int P, int32_t v)
    {
        if (Trace) Log("  voice %d param %d = %d", Id, P, v);
        std::lock_guard<std::mutex> L(VoiceLock);
        auto It = Voices.find(Id);
        if (It == Voices.end()) return;
        Voice& V = *It->second;
        switch (P)
        {
        case 0: V.Pitch = float(std::min(std::max(v, 0), 0xFFFF)) / 4096.f; break;
        case 2: V.GainA = float(std::min(std::max(v, 0), 0x7FFF)) / 32767.f; break;
        case 3: V.PanAngle = float(v) * (360.f / 65536.f); break;
        case 5: V.GainSend = float(std::min(std::max(v, 0), 0x7FFF)) / 32767.f; break;
        case 6: V.Lpf = float(std::min(std::max(v, 0), 0xFFFF)); break;
        case 7: V.Hpf = float(std::min(std::max(v, 0), 0xFFFF)); break;
        case 8: V.GainB = float(std::min(std::max(v, 0), 0x7FFF)) / 32767.f; break;
        default: break;
        }
    }

    void Engine::StopVoice(int Id)
    {
        std::lock_guard<std::mutex> L(VoiceLock);
        Voices.erase(Id);
    }

    void Engine::PauseVoice(int Id, bool Pause)
    {
        std::lock_guard<std::mutex> L(VoiceLock);
        auto It = Voices.find(Id);
        if (It != Voices.end()) It->second->Paused = Pause;
    }

    bool Engine::VoicePlaying(int Id, int32_t* RemainingMs, int32_t* ElapsedMs)
    {
        std::lock_guard<std::mutex> L(VoiceLock);
        auto It = Voices.find(Id);
        if (It == Voices.end() || It->second->Done) return false;
        const Voice& V = *It->second;
        const double Frames = double(V.S->Pcm.size() / size_t(V.S->Channels));
        *ElapsedMs = int32_t(V.Pos / V.S->Rate * 1000.0);
        *RemainingMs = V.S->Loop ? 0x7FFFFFFF : int32_t(std::max(0.0, Frames - V.Pos) / V.S->Rate * 1000.0);
        return true;
    }

    int Engine::ActiveVoices() const
    {
        std::lock_guard<std::mutex> L(VoiceLock);
        return int(Voices.size());
    }

    void Engine::Mix(float* Out, uint32_t Frames, uint32_t Rate)
    {
        std::fill(Out, Out + Frames * 2, 0.f);
        std::lock_guard<std::mutex> L(VoiceLock);
        for (auto& [Id, Vp] : Voices)
        {
            Voice& V = *Vp;
            if (V.Done || V.Paused || !V.S) continue;
            const Sample& S = *V.S;
            const uint32_t Ch = uint32_t(S.Channels);
            const uint32_t Total = uint32_t(S.Pcm.size() / Ch);
            const double Step = double(V.Pitch) * double(S.Rate) / double(Rate);
            if (!(Step > 0) || !std::isfinite(Step)) continue;
            V.Lp.Set(V.Lpf, float(Rate), false);
            V.Hp.Set(V.Hpf, float(Rate), true);
            // Pan2D: angle clockwise from front -> constant power left/right.
            const float Pan = std::sin(V.PanAngle * 3.14159265f / 180.f);
            const float Gain = V.GainA * V.GainB * MasterGain;
            const float TL = Gain * std::cos((Pan + 1.f) * 0.785398f), TR = Gain * std::sin((Pan + 1.f) * 0.785398f);
            if (V.First) { V.CurL = TL; V.CurR = TR; V.First = false; }
            const uint32_t LoopEnd = S.LoopEnd ? std::min(S.LoopEnd, Total) : Total;
            for (uint32_t f = 0; f < Frames; f++)
            {
                if (V.Pos < 0.0) V.Pos = 0.0;
                uint32_t i0 = uint32_t(V.Pos);
                if (i0 + 1 >= (S.Loop ? LoopEnd : Total))
                {
                    if (S.Loop && LoopEnd > S.LoopStart + 1)
                    {
                        // Wrap into the loop however far past its end the read is (a fast step over
                        // a short loop overshoots by more than one loop length).
                        const double Len = double(LoopEnd - S.LoopStart);
                        V.Pos = double(S.LoopStart) + std::fmod(V.Pos - double(S.LoopStart), Len);
                        i0 = uint32_t(V.Pos);
                    }
                    else { V.Done = true; break; }
                }
                if (Total < 2 || i0 >= Total) { V.Done = true; break; }
                const float t = float(V.Pos - double(i0));
                const uint32_t i1 = std::min(i0 + 1, Total - 1);
                float sl = S.Pcm[i0 * Ch] + (S.Pcm[i1 * Ch] - S.Pcm[i0 * Ch]) * t;
                float sr = Ch > 1 ? S.Pcm[i0 * Ch + 1] + (S.Pcm[i1 * Ch + 1] - S.Pcm[i0 * Ch + 1]) * t : sl;
                sl = V.Lp.Run(0, V.Hp.Run(0, sl));
                sr = Ch > 1 ? V.Lp.Run(1, V.Hp.Run(1, sr)) : sl;
                if (!std::isfinite(sl) || !std::isfinite(sr))
                {
                    // Never let one bad filter state poison the mix: reset this voice's filters.
                    V.Lp = Biquad{}; V.Hp = Biquad{};
                    sl = sr = 0.f;
                }
                V.CurL += (TL - V.CurL) * 0.002f;
                V.CurR += (TR - V.CurR) * 0.002f;
                if (Ch > 1) { Out[2 * f] += sl * V.CurL * 1.41421356f; Out[2 * f + 1] += sr * V.CurR * 1.41421356f; }
                else { Out[2 * f] += sl * V.CurL; Out[2 * f + 1] += sl * V.CurR; }
                V.Pos += Step;
            }
        }
        // Soft limiter: transparent below 0.6, smoothly saturating above (no hard clipping).
        for (uint32_t i = 0; i < Frames * 2; i++)
        {
            const float x = Out[i], a = std::fabs(x);
            if (a > 0.6f) Out[i] = std::copysign(0.6f + 0.4f * std::tanh((a - 0.6f) / 0.4f), x);
        }
    }
}
