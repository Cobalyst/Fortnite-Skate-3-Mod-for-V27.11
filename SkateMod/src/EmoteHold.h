#pragma once

#include "Common.h"

// Non-traversal emotes while skating. Fortnite runs them as "movement cancellable actions": the
// player controller counts them (AFortPlayerController::InMovementCancellableAction) and cancels
// them on movement input or when the pawn leaves a leash around where the action started
// (AFortPlayerControllerZone::MovementCancellableActionLeash*Location). Skating moves the pawn and
// uses the stick, so they were cancelled at once (traversal emotes are not movement cancellable).
//
// While skating only: the counter is parked (kept in Hold::Parked, so emotes that start or end
// while skating keep it balanced) and handed back when skating ends, the leash follows the pawn
// and the controller's "actively moving" flag is cleared. Outside skate mode nothing is touched.
namespace EmoteHold
{
    struct Hold
    {
        int Parked = 0;
        bool bHolding = false;
    };

    void WhileSkating(SDK::APlayerController* PC, const SDK::FVector& PawnAt, Hold& H, const char* Side);
    void Release(SDK::APlayerController* PC, Hold& H, const char* Side);
}
