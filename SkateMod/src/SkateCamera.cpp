#include "SkateCamera.h"

#include <cmath>

using namespace SDK;

namespace
{
    ACameraActor* Camera = nullptr;
    UWorld* CameraWorld = nullptr;  // the camera belongs to this world (a new match makes a new one)
    bool bViewing = false;
    int LoggedFov = 0;

    // Mouse orbit around the skater, on top of Skate 3's camera.
    float OrbitYaw = 0, OrbitPitch = 0;      // degrees
    float SinceLook = 10.f;                  // seconds since the last mouse look
    constexpr float LookDegreesPerUnit = 0.2f;
    constexpr float MinPitch = -25.f, MaxPitch = 45.f;  // camera lower / higher than Skate 3's
    constexpr float ReturnDelay = 1.0f, ReturnRate = 2.5f;
    constexpr float PivotHeight = 90.f;      // orbit around the skater's middle, not the feet

    FVector RotateAround(const FVector& V, const FVector& Axis, float Degrees)
    {
        const double A = Degrees * 3.14159265358979 / 180.0, C = std::cos(A), S = std::sin(A);
        const double Dot = V.X * Axis.X + V.Y * Axis.Y + V.Z * Axis.Z;
        const FVector Cr{ Axis.Y * V.Z - Axis.Z * V.Y, Axis.Z * V.X - Axis.X * V.Z, Axis.X * V.Y - Axis.Y * V.X };
        return FVector{ V.X * C + Cr.X * S + Axis.X * Dot * (1 - C), V.Y * C + Cr.Y * S + Axis.Y * Dot * (1 - C),
                        V.Z * C + Cr.Z * S + Axis.Z * Dot * (1 - C) };
    }

    // Skate 3's lens field of view (FUN lens_field_of_view, 16:9) is vertical; Unreal's camera
    // takes the horizontal angle of a 16:9 view (wider screens keep the same vertical framing).
    float HorizontalFov(float Vertical)
    {
        const float V = Vertical * 3.14159265f / 180.f;
        return 2.f * std::atan(std::tan(V * 0.5f) * (16.f / 9.f)) * 180.f / 3.14159265f;
    }
}

void SkateCamera::Begin(UWorld* World, APlayerController* PC)
{
    if (!World || !PC) return;
    if (World != CameraWorld) { Camera = nullptr; CameraWorld = World; }
    if (!Camera)
    {
        FTransform T{};
        T.Rotation.W = 1;
        T.Scale3D = FVector{ 1, 1, 1 };
        AActor* A = UGameplayStatics::BeginDeferredActorSpawnFromClass(World, ACameraActor::StaticClass(), T,
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, ESpawnActorScaleMethod::MultiplyWithRoot);
        if (A) A = UGameplayStatics::FinishSpawningActor(A, T, ESpawnActorScaleMethod::MultiplyWithRoot);
        Camera = static_cast<ACameraActor*>(A);
        if (!Camera)
        {
            Log("Camera: could not spawn the skate camera");
            return;
        }
        if (Camera->CameraComponent)
            Camera->CameraComponent->bConstrainAspectRatio = 0;  // no black bars on wide screens
    }
    bViewing = false;
    LoggedFov = 0;
    OrbitYaw = OrbitPitch = 0;
    SinceLook = 10.f;
}

void SkateCamera::AddLook(float DeltaX, float DeltaY)
{
    if (DeltaX == 0 && DeltaY == 0) return;
    OrbitYaw += DeltaX * LookDegreesPerUnit;
    OrbitYaw = std::remainder(OrbitYaw, 360.f);
    OrbitPitch = std::fmin(MaxPitch, std::fmax(MinPitch, OrbitPitch - DeltaY * LookDegreesPerUnit));
    SinceLook = 0;
}

void SkateCamera::UpdateFixed(APlayerController* PC, const FVector& Location, const FRotator& Rotation)
{
    if (!Camera || !PC)
        return;
    Camera->K2_SetActorLocationAndRotation(Location, Rotation, false, nullptr, true);
    PC->SetControlRotation(FRotator{ Rotation.Pitch, Rotation.Yaw, 0.0 });
    if (PC->GetViewTarget() != Camera)
    {
        PC->SetViewTargetWithBlend(Camera, 0.f, EViewTargetBlendFunction::VTBlend_EaseInOut, 2.f, false);
        bViewing = true;
    }
}

void SkateCamera::Update(APlayerController* PC, const SkState& Sk)
{
    if (!Camera || !PC || !Sk.camera_valid)
        return;
    FVector Forward{ Sk.camera_forward[0], Sk.camera_forward[1], Sk.camera_forward[2] };
    FVector Up{ Sk.camera_up[0], Sk.camera_up[1], Sk.camera_up[2] };
    FVector Position{ Sk.camera_position[0], Sk.camera_position[1], Sk.camera_position[2] };

    // Mouse orbit: swing Skate 3's camera around the skater, easing back once the mouse rests.
    static ULONGLONG LastTick = GetTickCount64();
    const ULONGLONG Now = GetTickCount64();
    const float Dt = std::fmin(0.1f, float(Now - LastTick) / 1000.f);
    LastTick = Now;
    SinceLook += Dt;
    if (SinceLook > ReturnDelay)
    {
        const float K = std::exp(-Dt * ReturnRate);
        OrbitYaw *= K;
        OrbitPitch *= K;
    }
    if (std::fabs(OrbitYaw) > 0.05f || std::fabs(OrbitPitch) > 0.05f)
    {
        const FVector Pivot{ Sk.position[0], Sk.position[1], Sk.position[2] + PivotHeight };
        FVector Arm{ Position.X - Pivot.X, Position.Y - Pivot.Y, Position.Z - Pivot.Z };
        const FVector WorldUp{ 0, 0, 1 };
        // Pitch about the camera's horizontal right axis, then yaw about world up.
        FVector Right{ Forward.Y, -Forward.X, 0 };
        const double RL = std::sqrt(Right.X * Right.X + Right.Y * Right.Y);
        if (RL > 1e-3)
        {
            Right = FVector{ Right.X / RL, Right.Y / RL, 0 };
            Arm = RotateAround(Arm, Right, -OrbitPitch);
            Forward = RotateAround(Forward, Right, -OrbitPitch);
            Up = RotateAround(Up, Right, -OrbitPitch);
        }
        Arm = RotateAround(Arm, WorldUp, OrbitYaw);
        Forward = RotateAround(Forward, WorldUp, OrbitYaw);
        Up = RotateAround(Up, WorldUp, OrbitYaw);
        Position = FVector{ Pivot.X + Arm.X, Pivot.Y + Arm.Y, Pivot.Z + Arm.Z };
    }
    const FRotator Rotation = UKismetMathLibrary::MakeRotFromXZ(Forward, Up);
    Camera->K2_SetActorLocationAndRotation(Position, Rotation, false, nullptr, true);
    if (Camera->CameraComponent && Sk.camera_fov > 1.f)
    {
        const float Fov = HorizontalFov(Sk.camera_fov);
        Camera->CameraComponent->SetFieldOfView(Fov);
        if (LoggedFov++ == 0)
            Log("Camera: Skate 3 camera active (vertical %.1f -> horizontal %.1f degrees)", Sk.camera_fov, Fov);
    }
    // Keep Fortnite's own aim pointing where the skate camera looks (mouse aim and leaving skate mode).
    PC->SetControlRotation(FRotator{ Rotation.Pitch, Rotation.Yaw, 0.0 });
    // Fortnite may switch the view back (abilities, emotes): reclaim it.
    if (PC->GetViewTarget() != Camera)
    {
        PC->SetViewTargetWithBlend(Camera, bViewing ? 0.f : 0.35f, EViewTargetBlendFunction::VTBlend_EaseInOut, 2.f, false);
        bViewing = true;
    }
}

void SkateCamera::End(APlayerController* PC, AActor* ViewTarget)
{
    if (PC && ViewTarget && bViewing)
        PC->SetViewTargetWithBlend(ViewTarget, 0.35f, EViewTargetBlendFunction::VTBlend_EaseInOut, 2.f, false);
    bViewing = false;
}

void SkateCamera::SetHomGrade(float Weight)
{
    if (!Camera || !Camera->CameraComponent) return;
    UCameraComponent* C = Camera->CameraComponent;
    FPostProcessSettings& P = C->PostProcessSettings;
    P.bOverride_ColorSaturation = Weight > 0.f ? 1 : 0;
    P.bOverride_ColorGain = Weight > 0.f ? 1 : 0;
    P.ColorSaturation = FVector4{ 0.5, 0.5, 0.5, 1.0 };
    P.ColorGain = FVector4{ 1.0, 1.0, 1.2, 1.0 };
    C->PostProcessBlendWeight = Weight;
}
