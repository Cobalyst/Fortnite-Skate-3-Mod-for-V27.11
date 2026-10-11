#pragma once

#include "Common.h"
#include "EngineApi.h"

// Skate 3's own camera while skating: the skate engine runs Skate 3's camera system (CameraMan,
// stock camera graph, high camera type) and publishes its frame in SkState; this places a camera
// actor there every frame and makes it the player's view.
namespace SkateCamera
{
    void Begin(SDK::UWorld* World, SDK::APlayerController* PC);
    void Update(SDK::APlayerController* PC, const SkState& Sk);
    void End(SDK::APlayerController* PC, SDK::AActor* ViewTarget);  // back to Fortnite's camera
    // A view set by someone else this frame (the object dropper's editor camera); the lens stays
    // the skate camera's.
    void UpdateFixed(SDK::APlayerController* PC, const SDK::FVector& Location, const SDK::FRotator& Rotation);
    // Mouse look: slow mouse movement orbits the camera around the skater (fast flicks are tricks
    // and never reach here); after a moment without input the camera eases back behind the skater.
    void AddLook(float DeltaX, float DeltaY);
    // Skate 3's Hall of Meat colour grade (post_fx colour_matrix_hall_of_meat: saturation 0.5,
    // multiply (1, 1, 1.2)) on the skate camera, blended by Weight 0..1.
    void SetHomGrade(float Weight);
}
