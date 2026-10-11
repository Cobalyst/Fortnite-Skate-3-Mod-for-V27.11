#pragma once

// Client -> server link through shared memory. The ATLAS server and the client run on the
// same PC, so while skating the client publishes the skater here and the server moves its copy
// of the character to match. Fortnite's own movement networking is paused meanwhile (it kept
// pulling the skater back to stale positions).
#include <Windows.h>
#include <cstdint>

struct SkateShare
{
    volatile LONG Sequence;   // odd while the client is writing
    uint32_t Skating;         // 1 while skate mode is on
    double   X, Y, Z;         // character (capsule centre) location, cm
    float    Yaw;
    float    VelX, VelY, VelZ;  // board velocity, cm/s (exit momentum)
    uint64_t TimeMs;          // GetTickCount64() of the last write
};

// Both processes map the same named block (Local\ = this Windows session only).
inline SkateShare* OpenSkateShare()
{
    static SkateShare* Share = nullptr;
    if (!Share)
    {
        HANDLE Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SkateShare), L"Local\\FortniteSkateModShare");
        if (Mapping)
            Share = static_cast<SkateShare*>(MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SkateShare)));
    }
    return Share;
}

inline void WriteSkateShare(bool bSkating, double X, double Y, double Z, float Yaw, const float* Vel)
{
    SkateShare* S = OpenSkateShare();
    if (!S) return;
    InterlockedIncrement(&S->Sequence);
    S->Skating = bSkating ? 1 : 0;
    S->X = X; S->Y = Y; S->Z = Z;
    S->Yaw = Yaw;
    S->VelX = Vel[0]; S->VelY = Vel[1]; S->VelZ = Vel[2];
    S->TimeMs = GetTickCount64();
    InterlockedIncrement(&S->Sequence);
}

// Consistent copy (retries while the writer is mid-update). False if nothing was published.
inline bool ReadSkateShare(SkateShare& Out)
{
    SkateShare* S = OpenSkateShare();
    if (!S) return false;
    for (int Try = 0; Try < 8; Try++)
    {
        const LONG Before = S->Sequence;
        if (Before & 1) continue;
        Out.Skating = S->Skating;
        Out.X = S->X; Out.Y = S->Y; Out.Z = S->Z;
        Out.Yaw = S->Yaw;
        Out.VelX = S->VelX; Out.VelY = S->VelY; Out.VelZ = S->VelZ;
        Out.TimeMs = S->TimeMs;
        if (S->Sequence == Before)
            return Before != 0;
    }
    return false;
}

// Client -> server: the object dropper's placed objects, so the server has the same solid
// objects as the client (Fortnite movement is decided by the server; without its own copy the
// character walked through them there). Rewritten whole whenever the client's objects change.
struct DropperShareEntry
{
    char Id[24];          // catalog model id ("0x2c70...")
    double X, Y, Z;       // Unreal world position, cm
    double Qx, Qy, Qz, Qw;  // rotation
};

struct DropperShareHeader
{
    volatile LONG Sequence;  // odd while the client is writing
    uint32_t Count;
    uint64_t Generation;     // the client's DropperWorld::Generation() when written
};

constexpr uint32_t DropperShareCapacity = 65536;  // no placement limit in practice (~5.8 MB)

inline DropperShareHeader* OpenDropperShare()
{
    static DropperShareHeader* Share = nullptr;
    if (!Share)
    {
        const DWORD Size = DWORD(sizeof(DropperShareHeader) + sizeof(DropperShareEntry) * DropperShareCapacity);
        HANDLE Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, Size, L"Local\FortniteSkateModDropper");
        if (Mapping)
            Share = static_cast<DropperShareHeader*>(MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, Size));
    }
    return Share;
}

inline DropperShareEntry* DropperShareEntries(DropperShareHeader* H) { return reinterpret_cast<DropperShareEntry*>(H + 1); }
