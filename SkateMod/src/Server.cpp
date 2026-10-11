// Server side: trust each player's client movement, and keep skaters free of fall damage.
//
// A skating client parks its character in MOVE_None, which reaches the server with its
// moves. While a pawn is in MOVE_None we keep AFortPawn::bIgnoreNextFallingDamage set, and
// after skating ends we keep it until the pawn has been on the ground for a moment, so the
// landing from a jump that started on the board never hurts.

#include "Common.h"
#include "Share.h"
#include "EmoteHold.h"
#include "DropperWorld.h"
#include "RawInputGuard.h"

#include <psapi.h>
#include <cmath>
#include <unordered_map>

using namespace SDK;

namespace
{
    // AFortPawn::bIgnoreNextFallingDamage: byte 0x750, bit 2 (FortniteGame_classes.hpp in the 27.11 dump).
    // The FortniteGame headers do not compile as dumped, so the flag is reached by offset.
    constexpr uintptr_t IgnoreNextFallingDamageOffset = 0x750;
    constexpr uint8 IgnoreNextFallingDamageBit = 1 << 2;

    void SetIgnoreNextFallingDamage(APawn* Pawn, bool bIgnore)
    {
        uint8& Flags = *(reinterpret_cast<uint8*>(Pawn) + IgnoreNextFallingDamageOffset);
        Flags = bIgnore ? (Flags | IgnoreNextFallingDamageBit) : (Flags & ~IgnoreNextFallingDamageBit);
    }

    UClass* FortPawnClass()
    {
        static UClass* Class = nullptr;
        if (!Class)
            Class = UObject::FindClassFast("FortPawn");
        return Class;
    }

    // AFortPlayerPawn::TemporarilyIgnoreClientMovementErrorChecksAndCorrection(float Duration).
    // Fortnite manages bIgnoreClientMovementErrorChecksAndCorrection on a timer of its own and
    // switches it back off every tick otherwise, so setting the flag alone does not stick:
    // the server kept snapping skaters back to where it thought they were.
    UFunction* IgnoreCorrectionsFunction()
    {
        static UFunction* Fn = nullptr;
        static bool bLogged = false;
        if (!Fn)
        {
            Fn = static_cast<UFunction*>(UObject::FindObjectFast("TemporarilyIgnoreClientMovementErrorChecksAndCorrection"));
            if (!bLogged) { bLogged = true; Log("TemporarilyIgnoreClientMovementErrorChecksAndCorrection: %p", Fn); }
        }
        return Fn;
    }

    void TrustClientFor(APawn* Pawn, float Seconds)
    {
        if (UFunction* Fn = IgnoreCorrectionsFunction())
        {
            struct { float Duration; } Parms{ Seconds };
            Pawn->ProcessEvent(Fn, &Parms);
        }
    }

    struct PawnInfo
    {
        bool  Skating = false;
        bool  Protecting = false;  // skating ended; protect until the first real landing
        float GroundTime = 0;
        bool  Trusted = false;     // trust flags forced by us (the originals below are kept)
        bool  OrigAccept = false, OrigIgnore = false;
        EmoteHold::Hold Emotes;    // non-traversal emotes while skating
        float LastHealth = -1.f;   // diagnostics: health drops are logged
    };

    std::unordered_map<ACharacter*, PawnInfo> Pawns;
    float PruneTimer = 0;

    // Diagnostics only (reads, never writes): ATLAS's "cheat pausesafezone" sometimes stops
    // holding (the storm moved on 2.5 and 8 min after the command on 2026-10-08, held 10+ min in
    // other matches). Logs the storm's pause flag and timers whenever they change, plus the
    // server's longest frame gap since the last line, so the next time it happens the log shows
    // what changed and whether the server was hitching.
    // Offsets from the 27.11 dump (FortniteGame_classes.hpp does not compile as dumped).
    constexpr uintptr_t PhaseLogicIndicator = 0x298;      // AFortSafeZoneIndicator* SafeZoneIndicator
    constexpr uintptr_t PhaseLogicStep = 0x3D9;           // EAthenaGamePhaseStep
    constexpr uintptr_t PhaseLogicStartTime = 0x400;      // float SafeZonesStartTime
    constexpr uintptr_t PhaseLogicPauseTime = 0x404;      // float SafeZonePauseTime
    constexpr uintptr_t IndicatorPaused = 0x2FA;          // bool bPausedForPreview
    constexpr uintptr_t IndicatorStartShrink = 0x4F8;     // float SafeZoneStartShrinkTime
    constexpr uintptr_t IndicatorFinishShrink = 0x4FC;    // float SafeZoneFinishShrinkTime

    struct StormValues
    {
        int Step = -1, Paused = -1;
        float Start = 0, PauseTime = 0, StartShrink = 0, FinishShrink = 0;
    };

    void WatchStorm(AGameStateBase* GameState, float Dt)
    {
        static UClass* LogicClass = nullptr;
        static UObject* Logic = nullptr;
        static int32 LogicIndex = -1;
        static float FindTimer = 0, MaxDt = 0, SinceLog = 0;
        static StormValues Last;
        static int Lines = 0;
        // Real gap between server ticks (Dt is capped at 0.1 s by the caller).
        static ULONGLONG LastMs = 0;
        const ULONGLONG NowMs = GetTickCount64();
        if (LastMs) MaxDt = std::fmax(MaxDt, float(NowMs - LastMs) / 1000.f);
        LastMs = NowMs;
        SinceLog += Dt;
        if (Lines > 400) return;

        if (Logic && UObject::GObjects->GetByIndex(LogicIndex) != Logic) Logic = nullptr;
        if (!Logic)
        {
            FindTimer -= Dt;
            if (FindTimer > 0) return;
            FindTimer = 10.f;
            if (!LogicClass) LogicClass = UObject::FindClassFast("FortGameStateComponent_BattleRoyaleGamePhaseLogic");
            if (!LogicClass) return;
            for (int i = 0; i < UObject::GObjects->Num(); i++)
            {
                UObject* O = UObject::GObjects->GetByIndex(i);
                if (O && O->IsA(LogicClass) && !O->IsDefaultObject()) { Logic = O; LogicIndex = i; break; }
            }
            if (!Logic) return;
            Log("[storm] watching %s", Logic->GetName().c_str());
        }

        const uint8* L = reinterpret_cast<const uint8*>(Logic);
        StormValues Now;
        Now.Step = *(L + PhaseLogicStep);
        Now.Start = *reinterpret_cast<const float*>(L + PhaseLogicStartTime);
        Now.PauseTime = *reinterpret_cast<const float*>(L + PhaseLogicPauseTime);
        UObject* Indicator = *reinterpret_cast<UObject* const*>(L + PhaseLogicIndicator);
        if (Indicator && UObject::GObjects->GetByIndex(Indicator->Index) == Indicator)
        {
            const uint8* I = reinterpret_cast<const uint8*>(Indicator);
            Now.Paused = *(I + IndicatorPaused) ? 1 : 0;
            Now.StartShrink = *reinterpret_cast<const float*>(I + IndicatorStartShrink);
            Now.FinishShrink = *reinterpret_cast<const float*>(I + IndicatorFinishShrink);
        }
        auto Moved = [](float A, float B) { return std::fabs(A - B) > 0.01f; };
        const bool bChanged = Now.Step != Last.Step || Now.Paused != Last.Paused || Moved(Now.Start, Last.Start) ||
                              Moved(Now.PauseTime, Last.PauseTime) || Moved(Now.StartShrink, Last.StartShrink) ||
                              Moved(Now.FinishShrink, Last.FinishShrink);
        // Timers that slide every tick (one way a pause can be done) would flood the log: at most
        // one line a second for those, but a pause flag or phase change is logged at once.
        const bool bUrgent = Now.Step != Last.Step || Now.Paused != Last.Paused;
        if (!bChanged || (!bUrgent && SinceLog < 1.f)) return;
        Lines++;
        Log("[storm] server time %.1f | phase step %d, paused %d | zones start %.1f, pause time %.1f | shrink %.1f -> %.1f | longest gap between server ticks %.0f ms",
            GameState->GetServerWorldTimeSeconds(), Now.Step, Now.Paused, Now.Start, Now.PauseTime, Now.StartShrink, Now.FinishShrink,
            MaxDt * 1000.f);
        Last = Now;
        MaxDt = 0;
        SinceLog = 0;
    }
}

bool Server::AnySkating()
{
    for (auto& [Pawn, Info] : Pawns)
        if (Info.Skating) return true;
    return false;
}

void Server::Tick(UWorld* World, float Dt)
{
    AGameStateBase* GameState = World->GameState;
    if (!GameState)
        return;
    WatchStorm(GameState, Dt);
    RawInputGuard::Tick(true);
    DropperWorld::MirrorOnServer(World);   // solid copies of the client's dropped objects

    // Once a minute: what the server holds (the hosted server shares the PC with the game, so a
    // server that slows down steals frames too). Live objects, level actors, game calls, memory.
    {
        static float HealthTimer = 0;
        HealthTimer += Dt;
        if (HealthTimer >= 60.f)
        {
            HealthTimer = 0;
            int Live = 0;
            const int32 N = UObject::GObjects->Num();
            for (int32 i = 0; i < N; i++) if (UObject::GObjects->GetByIndex(i)) Live++;
            const int Actors = World->PersistentLevel ? World->PersistentLevel->Actors.Num() : 0;
            const ModFrameCost Calls = TakeModFrameCost();
            PROCESS_MEMORY_COUNTERS Mem{};
            K32GetProcessMemoryInfo(GetCurrentProcess(), &Mem, sizeof(Mem));
            Log("[health] server: %d live objects (%d slots), %d actors in the level, %.0f game calls a second, %.1f GB",
                Live, N, Actors, double(Calls.GameCalls) / 60.0, double(Mem.WorkingSetSize) / 1073741824.0);
        }
    }

    // Pointers of pawns that were destroyed are never dereferenced (we only look up
    // live pawns), but clear the table now and then so it does not grow forever.
    PruneTimer += Dt;
    if (PruneTimer > 60.f)
    {
        PruneTimer = 0;
        // Keep pawns we are still managing (their original trust flags must be restored).
        for (auto It = Pawns.begin(); It != Pawns.end();)
            It = (It->second.Skating || It->second.Protecting || It->second.Trusted) ? std::next(It) : Pawns.erase(It);
    }

    // The local client's skater, published through shared memory (Share.h). Solo play has one
    // human player, so it belongs to the first non-bot pawn.
    SkateShare Shared{};
    const bool bShared = ReadSkateShare(Shared) && GetTickCount64() - Shared.TimeMs < 500;
    bool bAppliedShare = false;

    auto& Players = GameState->PlayerArray;
    for (int i = 0; i < Players.Num(); i++)
    {
        APlayerState* PlayerState = Players[i];
        if (!PlayerState || PlayerState->bIsABot)
            continue;  // bots idle in MOVE_None too; only real players can skate
        APawn* Pawn = PlayerState->PawnPrivate;
        if (!Pawn || !FortPawnClass() || !Pawn->IsA(FortPawnClass()))
            continue;

        ACharacter* FortPawn = static_cast<ACharacter*>(Pawn);  // AFortPawn derives from ACharacter
        UCharacterMovementComponent* Move = FortPawn->CharacterMovement;
        if (!Move)
            continue;

        PawnInfo& Info = Pawns[FortPawn];

        // Diagnostics (a sudden death while skating could not be explained from the logs):
        // every health drop of a real player, with where it happened and whether it was skating.
        {
            static UFunction* GetHealth = UObject::FindObject<UFunction>("Function FortniteGame.FortPawn.GetHealth");
            if (GetHealth)
            {
                struct { float ReturnValue; } P{ 0.f };
                FortPawn->ProcessEvent(GetHealth, &P);
                if (Info.LastHealth >= 0.f && P.ReturnValue < Info.LastHealth - 0.5f)
                {
                    const FVector At = FortPawn->K2_GetActorLocation();
                    Log("[health] player %d lost %.0f health (%.0f -> %.0f) at (%.0f, %.0f, %.0f), %s, movement mode %d", i,
                        Info.LastHealth - P.ReturnValue, Info.LastHealth, P.ReturnValue, At.X, At.Y, At.Z,
                        (bShared && Shared.Skating) ? "skating" : "on foot", int(Move->MovementMode));
                }
                Info.LastHealth = P.ReturnValue;
            }
        }

        const bool bHuman = !bAppliedShare;
        bAppliedShare = true;
        const bool Skating = bHuman && bShared && Shared.Skating;
        if (Skating)
        {
            // Follow the client's skater exactly (its movement networking is paused while skating).
            FortPawn->K2_SetActorLocationAndRotation(FVector{ Shared.X, Shared.Y, Shared.Z }, FRotator{ 0.0, Shared.Yaw, 0.0 }, false, nullptr, true);
            // Walking with the movement component switched off: Fortnite's emote ability
            // cancels itself unless the pawn is on the ground and still (MOVE_None counts as
            // neither), and an inactive component never moves the teleported pawn itself.
            if (Move->MovementMode != EMovementMode::MOVE_Walking)
                Move->SetMovementMode(EMovementMode::MOVE_Walking, 0);
            if (Move->IsActive())
                Move->Deactivate();
            Move->Velocity = FVector{};
        }
        else if (Info.Skating && bHuman)
        {
            // Skating just ended: carry on with the exit momentum until the client's moves resume.
            Move->Activate(true);
            Move->SetMovementMode(EMovementMode::MOVE_Falling, 0);
            Move->Velocity = FVector{ Shared.VelX, Shared.VelY, Shared.VelZ };
        }
        APlayerController* PC = FortPawn->Controller && FortPawn->Controller->IsA(APlayerController::StaticClass())
            ? static_cast<APlayerController*>(FortPawn->Controller) : nullptr;
        if (Skating)
            EmoteHold::WhileSkating(PC, FVector{ Shared.X, Shared.Y, Shared.Z }, Info.Emotes, "server");
        else if (Info.Skating)
            EmoteHold::Release(PC, Info.Emotes, "server");
        if (Skating && !Info.Skating)
            Log("Player %d started skating: fall damage off", i);
        if (!Skating && Info.Skating)
        {
            Log("Player %d stopped skating (mode %d): protecting the next landing", i, int(Move->MovementMode));
            Info.Protecting = true;
            Info.GroundTime = 0;
        }
        Info.Skating = Skating;

        // While skating (and until the first landing after it) the server accepts where the client
        // says it is and never corrects it. Otherwise the pawn keeps Fortnite's own settings: forced
        // all match long, the bus drop's skydive got no steering until the glider was toggled.
        if (Skating || Info.Protecting)
        {
            if (!Info.Trusted)
            {
                Info.OrigAccept = Move->bServerAcceptClientAuthoritativePosition;
                Info.OrigIgnore = Move->bIgnoreClientMovementErrorChecksAndCorrection;
                Info.Trusted = true;
                Log("Player %d: server now trusts client movement", i);
            }
            Move->bServerAcceptClientAuthoritativePosition = 1;
            Move->bIgnoreClientMovementErrorChecksAndCorrection = 1;
        }
        else if (Info.Trusted)
        {
            Move->bServerAcceptClientAuthoritativePosition = Info.OrigAccept;
            Move->bIgnoreClientMovementErrorChecksAndCorrection = Info.OrigIgnore;
            Info.Trusted = false;
            Log("Player %d: movement trust back to Fortnite's own", i);
        }

        if (Skating || Info.Protecting)
            TrustClientFor(FortPawn, 1.f);  // renewed every check, so it never lapses while skating

        if (Skating)
        {
            SetIgnoreNextFallingDamage(FortPawn, true);
        }
        else if (Info.Protecting)
        {
            if (Move->MovementMode == EMovementMode::MOVE_Walking)
            {
                Info.GroundTime += Dt;
                if (Info.GroundTime > 0.5f)
                {
                    SetIgnoreNextFallingDamage(FortPawn, false);
                    Info.Protecting = false;
                    Log("Player %d landed after skating: fall damage back to normal", i);
                }
            }
            else
            {
                Info.GroundTime = 0;
                SetIgnoreNextFallingDamage(FortPawn, true);
            }
        }
    }
}
