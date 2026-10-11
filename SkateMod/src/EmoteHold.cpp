#include "EmoteHold.h"

using namespace SDK;

namespace
{
    // ATLAS 27.11 (Dumper-7 SDK, FortniteGame_classes.hpp).
    constexpr size_t ActivelyMovingOffset = 0x109E;      // AFortPlayerController, bit 0 bIsPlayerActivelyMoving
    constexpr size_t CancellableCountOffset = 0x10A0;    // AFortPlayerController::InMovementCancellableAction (int32)
    constexpr size_t LeashLocationOffset = 0x3930;       // AFortPlayerControllerZone::MovementCancellableActionLeashLocation
    constexpr size_t StartLeashLocationOffset = 0x3948;  // AFortPlayerControllerZone::MovementCancellableActionStartingLeashLocation

    UClass* FortControllerClass()
    {
        static UClass* C = UObject::FindClassFast("FortPlayerController");
        return C;
    }
    UClass* ZoneControllerClass()
    {
        static UClass* C = UObject::FindClassFast("FortPlayerControllerZone");
        return C;
    }
    template <typename T> T& At(APlayerController* PC, size_t Offset) { return *reinterpret_cast<T*>(reinterpret_cast<uint8*>(PC) + Offset); }
}

void EmoteHold::WhileSkating(APlayerController* PC, const FVector& PawnAt, Hold& H, const char* Side)
{
    if (!PC || !FortControllerClass() || !PC->IsA(FortControllerClass()))
        return;
    if (!H.bHolding)
    {
        H.bHolding = true;
        Log("[emote] %s: holding movement-cancellable emotes while skating (count %d)", Side, At<int32>(PC, CancellableCountOffset));
    }
    int32& Count = At<int32>(PC, CancellableCountOffset);
    if (Count != 0)
    {
        H.Parked += Count;
        Log("[emote] %s: emote %s while skating (parked %d)", Side, Count > 0 ? "started" : "ended", H.Parked);
        Count = 0;
    }
    At<uint8>(PC, ActivelyMovingOffset) &= uint8(~1u);
    if (ZoneControllerClass() && PC->IsA(ZoneControllerClass()))
    {
        At<FVector>(PC, LeashLocationOffset) = PawnAt;
        At<FVector>(PC, StartLeashLocationOffset) = PawnAt;
    }
}

void EmoteHold::Release(APlayerController* PC, Hold& H, const char* Side)
{
    if (!H.bHolding)
        return;
    H.bHolding = false;
    if (PC && FortControllerClass() && PC->IsA(FortControllerClass()))
    {
        At<int32>(PC, CancellableCountOffset) += H.Parked;
        if (ZoneControllerClass() && PC->IsA(ZoneControllerClass()))
        {
            // A still-running emote is leashed from where skating ended.
            const FVector Here = PC->Pawn ? PC->Pawn->K2_GetActorLocation() : FVector{};
            At<FVector>(PC, LeashLocationOffset) = Here;
            At<FVector>(PC, StartLeashLocationOffset) = Here;
        }
    }
    Log("[emote] %s: skating ended, %d emote(s) back to Fortnite's movement cancelling", Side, H.Parked);
    H.Parked = 0;
}
