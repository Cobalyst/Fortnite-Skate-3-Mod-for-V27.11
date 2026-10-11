#include "DropperEditor.h"

#include "DropperInput.h"
#include "DropperSnap.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace SDK;

namespace DropperEditor
{
    namespace
    {
        constexpr double Pi = 3.14159265358979323846;
        constexpr double Deg = 0.01745329238474369;   // DAT_8206d108
        constexpr double M = 100.0;                   // Skate 3 metres -> Unreal cm
        // FUN_82865170 runs once per world tick (82834e00, from 82837968), the 60 Hz simulation tick:
        // the tick hands DAT_82084998 = 1/60 to its systems and the editor's camera helpers.
        constexpr double Step = 1.0 / 60.0;
        constexpr double FltEps = 1.1920928955078125e-07;   // DAT_8217f228 (FLT_EPSILON)

        // ---- Attribute records (skatercollections.vlt, Tools/vault_dump.py) ----------------
        struct DropperTuning      // objectdropper/<key>
        {
            float Move, ZoomMove, Elevate, Rotate, YawSpeed, PitchSpeed, ZoomSpeed, PickupFov;
            bool FreeCamLike;     // D41775D7
            float CurveX[8], CurveY[8];   // D6BCD320 Sk8::PointNegGraphData8 (+0x10 xs, +0x30 ys)
            float MinFrame, MaxFrame;     // layout C4A69E25 / D9DEC255
        };
        const DropperTuning Tunings[4] = {
            // default (manipulate inherits default)
            { 0.5f, 0.f, 0.15f, 0.04f, 0.04f, 0.03f, 0.7f, 62.5f, false,
              { 0.f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 1.f }, { 0.2f, 0.3625f, 0.525f, 0.6875f, 0.85f, 1.0125f, 1.175f, 1.5f },
              1.f, 40.f },
            // free_cam
            { 0.3f, 0.3f, 0.1f, 0.04f, 0.04f, 0.02f, 1.f, 62.5f, true,
              { 0.f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 1.f }, { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f },
              1.f, 40.f },
            // manipulate (= default)
            { 0.5f, 0.f, 0.15f, 0.04f, 0.04f, 0.03f, 0.7f, 62.5f, false,
              { 0.f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 1.f }, { 0.2f, 0.3625f, 0.525f, 0.6875f, 0.85f, 1.0125f, 1.175f, 1.5f },
              1.f, 40.f },
            // object_spawner (record fields not decoded yet: as default)
            { 0.5f, 0.f, 0.15f, 0.04f, 0.04f, 0.03f, 0.7f, 62.5f, false,
              { 0.f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 1.f }, { 0.2f, 0.3625f, 0.525f, 0.6875f, 0.85f, 1.0125f, 1.175f, 1.5f },
              1.f, 40.f },
        };
        // editor_camera/objectdropper
        constexpr float CamStartDistance = 20.f;   // 61D0436E
        constexpr float CamMinDistance = 5.f;      // 7F9E4FD2
        constexpr float CamMaxDistance = 40.f;     // C1D41A1A
        constexpr float CamStartPitch = -20.f;     // 93D10E5C (degrees, first update's pitch change)
        constexpr float CamLayout[5] = { 0.7f, 5.f, -80.f, 80.f, 60.f };   // smoothing, pitch min/free min, max/free max
        // editor_cursor/objectdropper
        constexpr float LeashHorizontal = 9.f;     // 893DA1B9
        constexpr float LeashVertical = 7.f;       // 0B2454C6
        constexpr bool LeashEnabled = true;        // 26835378
        constexpr float PickRadius = 0.35f;        // 1E93420A
        constexpr float PickDistance = 200.f;      // E1CD8EB7
        // FUN_82861ad0: a move smaller than this on every axis is ignored (DAT_8217f228 = FLT_EPSILON m, in cm)
        constexpr double MoveEpsilon = FltEps * M;
        // 8285fbf8: the camera's collision sweep, a sphere of DAT_8217fc30 = 0.4 m
        constexpr double CamSweepRadius = 0.4;

        float Curve(const DropperTuning& T, float X)   // FUN_8246fd40 (n = 8)
        {
            if (X < T.CurveX[0]) return T.CurveY[0];
            if (X >= T.CurveX[7]) return T.CurveY[7];
            for (int i = 1; i < 8; i++)
                if (X < T.CurveX[i])
                {
                    const float Dx = T.CurveX[i] - T.CurveX[i - 1];
                    if (Dx > 0.f) return (T.CurveY[i] - T.CurveY[i - 1]) / Dx * (X - T.CurveX[i - 1]) + T.CurveY[i - 1];
                    return T.CurveY[i];
                }
            return T.CurveY[0];
        }

        // ---- State (offsets of the original object in comments) ------------------------------
        bool bActive = false;
        Mode CurMode = Off;                 // +0x868
        Tuning CurTuning = Default;         // +0x858
        double Accum = 0;
        uint32_t LockBits = 0;              // +0x870
        float ElevateAccel = 0.3f;          // +0x8a8 (DAT_820d04e0)
        float SpeedScale = 1.f;             // +0x8b0 (cursor speed option)
        float CamYawScale = 1.f, CamPitchScale = 1.f;   // +0x874 / +0x878
        Options Opt;                         // +0x8ac
        int StepCounter = 0;                // +0x8a4

        // Cursor (+0x550)
        FVector Cursor{};                   // +0x60
        FVector LeashCentre{};              // +0x70
        bool bLeash = false;                // +0x80
        int HeldHandle = 0;                 // [0]
        float PendingZoom = 0;              // 82866a70's addition to the next update's zoom input
        int HoverHandle = 0;                // [1]
        FVector HeldOffset{};               // object position - cursor
        // A carried group's other objects: where each is in the carried object's frame.
        struct Follower { int Handle; FVector Offset; DropperWorld::Quat Rel; };
        std::vector<Follower> Followers;
        std::vector<int> FollowerHandles;
        // Animated rotation (828623d8): angle done / target, axis.
        bool bRotAnim = false;              // +0x1a0
        float RotDone = 0, RotTarget = 0;   // +0x1c0 / +0x1c4
        FVector RotAxis{ 0, 0, 1 };
        // Animated stepped move (82861f88): distance done / target, direction.
        bool bMoveAnim = false;             // +0x1c8
        float MoveDone = 0, MoveTarget = 0, MoveRate = 0;   // +0x1e0 / +0x1e4 / +0x1e8
        FVector MoveDir{};
        DropperWorld::Quat HeldStartRotation;
        // Cursor rotation (+0xe0): what a spawned object gets (828626e0). The last placed object's
        // (82862ee0) or the reset one (828624a0); none (828614b0 zeroes it) = identity.
        DropperWorld::Quat CursorRotation;
        bool bCatalogCamera = false;        // +0x7e8 (camera +0x88): the catalog's camera looks at the cursor
        bool bCamFirstUpdate = true;        // camera +0x89: set by the reset (8285fa98), cleared after every update (82865260)
        UWorld* SweepWorld = nullptr;       // the world the camera sweep runs in
        AActor* Viewer = nullptr;           // the player's character (not something the camera collides with)

        // Camera (+0x760)
        double Yaw = 0, Pitch = 0;           // view direction angles (Unreal: yaw about Z, pitch up positive)
        float DistTarget = CamStartDistance, DistSmooth = CamStartDistance;   // +0x7c / +0x78
        float DYaw = 0, DPitch = 0, DZoom = 0;   // +0x74 / +0x70 / +0x80 (per-update input)
        FVector CamPos{};
        FVector CamForward{ 1, 0, 0 };
        // Smoothing between fixed updates (drawing only)
        FVector PrevCamPos{}, PrevCamForward{ 1, 0, 0 };
        FVector PrevHeldPos{};
        DropperWorld::Quat PrevHeldRot;
        int PrevHeldHandle = 0;
        double PresentAlpha = 1.0;

        FVector Add(const FVector& A, const FVector& B) { return FVector{ A.X + B.X, A.Y + B.Y, A.Z + B.Z }; }
        FVector Sub(const FVector& A, const FVector& B) { return FVector{ A.X - B.X, A.Y - B.Y, A.Z - B.Z }; }
        FVector Mul(const FVector& A, double S) { return FVector{ A.X * S, A.Y * S, A.Z * S }; }
        double Dot(const FVector& A, const FVector& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
        double Len(const FVector& A) { return std::sqrt(Dot(A, A)); }

        const DropperTuning& T() { return Tunings[CurTuning]; }

        // Camera basis in the horizontal plane (the move vector of 82865968: right x lateral + flat forward x forward + up x elevate)
        FVector FlatForward() { return FVector{ std::cos(Yaw), std::sin(Yaw), 0 }; }
        FVector FlatRight() { return FVector{ -std::sin(Yaw), std::cos(Yaw), 0 }; }

        DropperWorld::Quat Conj(const DropperWorld::Quat& Q) { return DropperWorld::Quat{ -Q.X, -Q.Y, -Q.Z, Q.W }; }

        // A carried group's others at the carried object's pose (Show: drawn only).
        void PoseFollowers(const FVector& Pos, const DropperWorld::Quat& Rot, bool bShow)
        {
            for (const Follower& F : Followers)
            {
                const FVector At = Add(Pos, DropperWorld::Rotate(Rot, F.Offset));
                const DropperWorld::Quat R = DropperWorld::Mul(Rot, F.Rel);
                if (bShow) DropperWorld::ShowAt(F.Handle, At, R);
                else DropperWorld::Move(F.Handle, At, R);
            }
        }

        void MoveHeld(const FVector& Pos, const DropperWorld::Quat& Rot)
        {
            DropperWorld::Move(HeldHandle, Pos, Rot);
            PoseFollowers(Pos, Rot, false);
        }

        // The held object follows the cursor.
        void PlaceHeld()
        {
            if (!HeldHandle) return;
            const DropperWorld::Placed* P = DropperWorld::Get(HeldHandle);
            if (!P) { HeldHandle = 0; return; }
            MoveHeld(Add(Cursor, HeldOffset), P->Rotation);
        }

        // FUN_82861ad0: move the cursor by Delta (cm), with the leash around LeashCentre.
        void MoveCursor(const FVector& DeltaIn, bool bAnimatedStep)
        {
            FVector Delta = DeltaIn;
            if (std::fabs(Delta.X) <= MoveEpsilon && std::fabs(Delta.Y) <= MoveEpsilon && std::fabs(Delta.Z) <= MoveEpsilon)
                return;
            if (bAnimatedStep)
            {
                // Stepped move: animate Delta's length along its direction (rate = 0.1 of the step per update).
                const double L = Len(Delta);
                if (L <= 0) return;
                bMoveAnim = true;
                MoveDir = Mul(Delta, 1.0 / L);
                MoveDone = 0;
                MoveTarget = float(L);
                MoveRate = float(L * 0.1);   // DAT_820641a0
                return;
            }
            FVector Target = Add(Cursor, Delta);
            if (LeashEnabled && bLeash)
            {
                FVector Off = Sub(Target, LeashCentre);
                const double R = LeashHorizontal * M;
                const double H = std::sqrt(Off.X * Off.X + Off.Y * Off.Y);
                if (H > R) { Off.X *= R / H; Off.Y *= R / H; }
                const double V = LeashVertical * M;
                if (std::fabs(Off.Z) > V) Off.Z = Off.Z < 0 ? -V : V;
                Target = Add(LeashCentre, Off);
            }
            Cursor = Target;
            PlaceHeld();
        }

        // 82861f88: advance an animated stepped move.
        void StepMoveAnim(bool bLimit)
        {
            if (!bMoveAnim || !HeldHandle) { bMoveAnim = false; return; }
            float D = MoveTarget - MoveDone;
            if (bLimit) D = std::min(std::fabs(D), MoveRate) * (D < 0 ? -1.f : 1.f);
            MoveCursor(Mul(MoveDir, D), false);
            MoveDone += D;
            if (MoveDone == MoveTarget) bMoveAnim = false;
        }

        // 82862088: rotate the held object by Angle about Axis (world), around the cursor.
        void RotateHeld(float Angle, const FVector& Axis)
        {
            if (!HeldHandle) return;
            const DropperWorld::Placed* P = DropperWorld::Get(HeldHandle);
            if (!P) return;
            const double S = std::sin(Angle * 0.5), C = std::cos(Angle * 0.5);
            DropperWorld::Quat Q{ Axis.X * S, Axis.Y * S, Axis.Z * S, C };
            HeldOffset = DropperWorld::Rotate(Q, HeldOffset);
            MoveHeld(Add(Cursor, HeldOffset), DropperWorld::Mul(Q, P->Rotation));
        }

        // 828623d8: advance an animated rotation (at most 0.15 rad per update when limited).
        void StepRotAnim(bool bLimit)
        {
            if (!bRotAnim || !HeldHandle) { bRotAnim = false; return; }
            float D = RotTarget - RotDone;
            if (bLimit) D = std::min(std::fabs(D), 0.15f) * (D < 0 ? -1.f : 1.f);   // DAT_82099458
            RotateHeld(D, RotAxis);
            RotDone += D;
            if (RotDone == RotTarget) bRotAnim = false;
        }

        void StartRotAnim(float Angle, const FVector& Axis)
        {
            if (bRotAnim) StepRotAnim(false);   // finish the previous step first
            bRotAnim = true;
            RotDone = 0;
            RotTarget = Angle;
            RotAxis = Axis;
        }

        // 82861270: the object under the cursor (free camera: along the view, PickDistance, PickRadius).
        void UpdateHover()
        {
            if (HeldHandle)
            {
                if (HoverHandle) DropperWorld::SetDisplayMode(HoverHandle, 0);
                HoverHandle = 0;
                return;
            }
            int Best = 0;
            double BestT = PickDistance * M;
            const FVector O = CamPos, D = CamForward;
            for (const DropperWorld::Placed& P : DropperWorld::All())
            {
                if (P.bGhost) continue;
                FVector Lo, Hi;
                DropperWorld::Bounds(P.Handle, Lo, Hi);
                // ray vs bounds grown by the pick radius
                const double R = PickRadius * M;
                double T0 = 0, T1 = BestT;
                bool bHit = true;
                const double Os[3] = { O.X, O.Y, O.Z }, Ds[3] = { D.X, D.Y, D.Z }, Ls[3] = { Lo.X - R, Lo.Y - R, Lo.Z - R }, Hs[3] = { Hi.X + R, Hi.Y + R, Hi.Z + R };
                for (int k = 0; k < 3 && bHit; k++)
                {
                    if (std::fabs(Ds[k]) < 1e-9) { if (Os[k] < Ls[k] || Os[k] > Hs[k]) bHit = false; continue; }
                    double A = (Ls[k] - Os[k]) / Ds[k], B = (Hs[k] - Os[k]) / Ds[k];
                    if (A > B) std::swap(A, B);
                    T0 = std::max(T0, A); T1 = std::min(T1, B);
                    if (T0 > T1) bHit = false;
                }
                if (bHit && T0 < BestT) { BestT = T0; Best = P.Handle; }
            }
            // 82861270: the object under the cursor shows display mode 1 (cursor +0xc highlight on)
            if (Best != HoverHandle)
            {
                if (HoverHandle) DropperWorld::SetDisplayMode(HoverHandle, 0);
                if (Best) DropperWorld::SetDisplayMode(Best, 1);
            }
            HoverHandle = Best;
        }

        // One sphere cast of 8285fbf8 (radius 0.4 m) through the world's collision, the dropped pieces
        // included. Skate 3's casts run in Havok: mode 1 ignores what the sphere already touches at
        // its start, mode 3 reports it (fraction 0). Unreal's sweep always reports a start overlap,
        // so for mode 1 the overlapped actors are left out and the cast is repeated.
        struct CastHit { bool bHit = false; double T = 0; FVector Normal{}; };
        CastHit Cast(const FVector& From, const FVector& To, bool bIgnoreStart)
        {
            CastHit R;
            if (!SweepWorld) return R;
            std::vector<AActor*> Ignore;
            if (Viewer) Ignore.push_back(Viewer);
            const FLinearColor Color{};
            for (int Try = 0; Try < 8; Try++)
            {
                TArray<AActor*> Ig(Ignore.data(), int32(Ignore.size()), int32(Ignore.size()));
                FHitResult Hit{};
                const bool bHit = UKismetSystemLibrary::SphereTraceSingle(SweepWorld, From, To, float(CamSweepRadius * M),
                    ETraceTypeQuery::TraceTypeQuery1 /* Visibility */, false, Ig, EDrawDebugTrace::None, &Hit, true, Color, Color, 0.f);
                if (!bHit || !Hit.bBlockingHit) return R;
                if (bIgnoreStart && Hit.bStartPenetrating)
                {
                    UPrimitiveComponent* C = Hit.Component.Get();
                    AActor* A = C ? C->GetOwner() : nullptr;
                    if (!A || std::find(Ignore.begin(), Ignore.end(), A) != Ignore.end()) return R;
                    Ignore.push_back(A);
                    continue;
                }
                R.bHit = true;
                R.T = Hit.bStartPenetrating ? 0.0 : double(Hit.Time);
                R.Normal = FVector{ Hit.Normal.X, Hit.Normal.Y, Hit.Normal.Z };
                return R;
            }
            return R;
        }

        // 8285fbf8: the eye moves from Old towards Eye (the wanted spot) around Target (the cursor).
        // Returns true when the eye was changed. The +0x8a height ceiling is never set for the dropper.
        bool CameraSweep(const FVector& Target, const FVector& Old, FVector& Eye)
        {
            const FVector Desired = Eye;
            const FVector Back = Sub(Target, Desired);
            const double Eps = FltEps * M * M;   // squared lengths, metres^2 in Skate 3
            const bool bTarget = HeldHandle != 0 && Dot(Back, Back) > Eps;
            const bool bMove = !bCamFirstUpdate && Dot(Sub(Desired, Old), Sub(Desired, Old)) > Eps;
            if (!bTarget && !bMove) return false;
            const CastHit A = Cast(Target, Desired, true);    // cursor -> wanted eye (mode 1)
            const CastHit B = Cast(Old, Desired, true);       // last eye -> wanted eye (mode 1)
            const CastHit C = Cast(Target, Desired, false);   // cursor -> wanted eye (mode 3)
            if (bTarget && A.bHit && std::fabs(A.T) > FltEps && (bCamFirstUpdate || B.bHit))
            {
                // pulled in to where the cast from the cursor hit
                const FVector In = Sub(Target, Mul(Back, A.T));
                if (C.bHit && !(std::fabs(C.T) > FltEps))
                    Eye = Cast(Old, In, true).bHit ? Old : In;   // the cursor itself is in something
                else
                    Eye = In;
                return true;
            }
            if (bMove && B.bHit)
            {
                // slide along what the eye ran into
                const FVector N = B.Normal;
                const FVector Slide = Sub(Desired, Mul(N, Dot(Sub(Desired, Old), N)));
                Eye = Cast(Old, Slide, true).bHit ? Old : Slide;
                return true;
            }
            return false;
        }

        // 82865968 + 82866498: one fixed update of input -> cursor / held object / camera.
        void SnapHeld();
        void CheckHeld();

        void Update()
        {
            using namespace DropperInput;
            PrevCamPos = CamPos;
            PrevCamForward = CamForward;
            PrevHeldHandle = HeldHandle;
            if (const DropperWorld::Placed* P = HeldHandle ? DropperWorld::Get(HeldHandle) : nullptr)
            {
                PrevHeldPos = P->Position;
                PrevHeldRot = P->Rotation;
            }
            const DropperTuning& Tu = T();
            const bool bHolding = HeldHandle != 0;

            // ---- camera input (82866498) ----
            const float RR = CameraRotateRight(), RL = CameraRotateLeft(), RU = CameraRotateUp(), RD = CameraRotateDown();
            DYaw = RL <= RR ? CamYawScale * Tu.YawSpeed * RR : -(CamYawScale * Tu.YawSpeed * RL);
            DPitch = RD <= RU ? CamPitchScale * Tu.PitchSpeed * RU : -(CamPitchScale * Tu.PitchSpeed * RD);
            DZoom = 0;
            if (CurMode != Off && bHolding)
            {
                const float Zi = CameraZoomIn(), Zo = CameraZoomOut();
                DZoom = Zi <= Zo ? Tu.ZoomSpeed * Zo : -(Tu.ZoomSpeed * Zi);
            }
            if (bCatalogCamera) DYaw = DPitch = DZoom = 0;   // 82866498: no camera input while it is locked
            DZoom += PendingZoom;
            PendingZoom = 0;

            // ---- cursor input (82865968) ----
            if (CurMode == FreeCam || CurMode == Rotate || CurMode == LockAxis)
            {
                const float Frac = (DistSmooth - CamMinDistance) / (CamMaxDistance - CamMinDistance);
                const float K = Curve(Tu, bHolding ? Frac : 0.2f) * SpeedScale;   // DAT_8209923c when the camera is fixed
                const float Move = Tu.Move * K, Zoom = Tu.ZoomMove * K, Elev = Tu.Elevate * K * ElevateAccel;
                const bool bAdv = AdvancedModifier() && !(LockBits & 0x10);
                float Mr = 0, Ml = 0, Mf = 0, Mb = 0, Eu = 0, Ed = 0;
                const float Zin = CameraZoomIn() * Zoom, Zout = CameraZoomOut() * Zoom;
                if (!(LockBits & 1)) { Mr = CursorMoveRight() * Move; Ml = CursorMoveLeft() * Move; }
                if (!(LockBits & 2)) { Mf = CursorMoveForward() * Move; Mb = CursorMoveBack() * Move; }
                if (!(LockBits & 4)) { Eu = CursorElevateUp() * Elev; Ed = CursorElevateDown() * Elev; }
                ElevateAccel = (Eu + Ed == 0.f) ? 0.3f : std::min(ElevateAccel + 0.15f, 3.f);   // DAT_82099458 / DAT_82063b04
                bool bStep = false;
                FVector Right = FlatRight(), Fwd = FlatForward();
                if (CurMode == LockAxis)
                {
                    // Stepped moves along the object's own axes (Simple_* repeats), the bigger stick axis only.
                    if (bAdv) { StepCounter = 0; StepMoveAnim(false); }
                    else
                    {
                        const float StepLen = StepCounter > 2 ? 2.25f : 0.75f;   // DAT_822f3dbc / DAT_8217f5a0
                        if (Mr + Ml + Mf + Mb + Eu + Ed == 0.f) StepCounter = 0;
                        Mr = SimpleMoveRight() ? StepLen : 0.f;  Ml = SimpleMoveLeft() ? StepLen : 0.f;
                        Mf = SimpleMoveForward() ? StepLen : 0.f; Mb = SimpleMoveBack() ? StepLen : 0.f;
                        Eu = SimpleElevateUp() ? StepLen : 0.f;  Ed = SimpleElevateDown() ? StepLen : 0.f;
                        bStep = true;
                        if (Mr + Ml + Mf + Mb + Eu + Ed > 0.f) StepCounter++;
                    }
                    if (std::max(Mr, Ml) > std::max(Mf, Mb)) Mf = Mb = 0; else Mr = Ml = 0;
                    if (HeldHandle)
                        if (const DropperWorld::Placed* P = DropperWorld::Get(HeldHandle))
                        {
                            // the object's local X / Y (Skate: its local axes at +0x90 / +0xa0)
                            Right = DropperWorld::Rotate(P->Rotation, FVector{ 0, 1, 0 });
                            Fwd = DropperWorld::Rotate(P->Rotation, FVector{ 1, 0, 0 });
                        }
                }
                float Lat = Mr - Ml, Fw = Mf - Mb, Up = Eu - Ed;
                if (CurMode == Rotate) Lat = Fw = 0;
                FVector Delta = Add(Add(Mul(Right, Lat * M), Mul(Fwd, Fw * M)), Mul(FVector{ 0, 0, 1 }, Up * M));
                if (Tu.FreeCamLike)   // D41775D7: zoom input flies along the view direction
                    Delta = Add(Delta, Mul(CamForward, (Zin - Zout) * M));
                MoveCursor(Delta, bStep);
            }

            // ---- rotation (82865968, mode 2) ----
            if (CurMode == Rotate && HeldHandle)
            {
                const float Ang = Tu.Rotate;   // 32D332B6
                const float H = (RotateRight() - RotateLeft()) * Ang, V = (RotateUp() - RotateDown()) * Ang;
                if (AdvancedModifier())
                {
                    // Step rotations: 90 degrees per press, animated (0x8217fcf0 = 90, 82862088 / 828623d8)
                    if (SimpleRotateLeft()) StartRotAnim(float(-90.0 * Deg), FVector{ 0, 0, 1 });
                    else if (SimpleRotateRight()) StartRotAnim(float(90.0 * Deg), FVector{ 0, 0, 1 });
                    else if (SimpleRotateUp()) StartRotAnim(float(90.0 * Deg), FlatRight());
                    else if (SimpleRotateDown()) StartRotAnim(float(-90.0 * Deg), FlatRight());
                }
                else if (std::fabs(H) > std::fabs(V)) RotateHeld(H, FVector{ 0, 0, 1 });
                else if (V != 0.f) RotateHeld(V, FlatRight());
            }
            StepRotAnim(true);
            StepMoveAnim(true);

            // ---- camera (828603c0) ----
            if (bCatalogCamera)
            {
                // 82860270: the camera stays where it is and looks at the cursor; its distance becomes
                // the separation (DAT_8217f228 = FLT_EPSILON in metres).
                const FVector D = Sub(Cursor, CamPos);
                const double Eps = 1.1920928955078125e-07 * M;
                if (std::fabs(D.X) > Eps || std::fabs(D.Y) > Eps || std::fabs(D.Z) > Eps)
                {
                    const double L = Len(D);
                    DistSmooth = DistTarget = float(L / M);
                    CamForward = Mul(D, 1.0 / L);
                    Yaw = std::atan2(CamForward.Y, CamForward.X);
                    Pitch = std::asin(std::clamp(CamForward.Z, -1.0, 1.0));   // view pitch (free camera's)
                }
                UpdateHover();
                return;
            }
            const bool bOrbit = HeldHandle != 0;
            // Skate 3's yaw is counter-clockwise (right-handed, Y up), Unreal's clockwise (Z up): its
            // free camera turn (-DYaw) and orbit (+DYaw) are mirrored here.
            Yaw += bOrbit ? -DYaw : DYaw;
            if (bOrbit)
            {
                DistTarget = std::clamp(DistTarget + DZoom, CamMinDistance, CamMaxDistance);
                DistSmooth = DistSmooth + (DistTarget - DistSmooth) * CamLayout[0];
            }
            const double Lo = (bOrbit ? CamLayout[1] : CamLayout[2]) * Deg, Hi = (bOrbit ? CamLayout[3] : CamLayout[4]) * Deg;
            // Orbit pitch is the camera's height angle above the target (looking down); the free camera
            // pitch is the view's own (up positive).
            Pitch = std::clamp(Pitch + DPitch, Lo, Hi);
            FVector Eye;
            if (bOrbit)
            {
                const double Cp = std::cos(Pitch), Sp = std::sin(Pitch);
                const FVector Back{ -std::cos(Yaw) * Cp, -std::sin(Yaw) * Cp, Sp };
                Eye = Add(Cursor, Mul(Back, DistSmooth * M));
                CamForward = Mul(Back, -1.0);
            }
            else
            {
                Eye = Cursor;
                CamForward = FVector{ std::cos(Yaw) * std::cos(Pitch), std::sin(Yaw) * std::cos(Pitch), std::sin(Pitch) };
            }
            // 8285fbf8 from the last eye; carrying, a pulled in eye sets the distance (+0x78, and the
            // target +0x7c when zooming) and still looks at the cursor (82947548).
            if (CameraSweep(Cursor, CamPos, Eye) && bOrbit)
            {
                const FVector D = Sub(Cursor, Eye);
                const double L = Len(D);
                DistSmooth = float(L / M);
                if (DZoom != 0.f) DistTarget = DistSmooth;
                if (L > 1e-9) CamForward = Mul(D, 1.0 / L);
            }
            CamPos = Eye;
            bCamFirstUpdate = false;   // 82865260
            UpdateHover();
        }

        // 82860c60 / 82863038: the carried object (and its group) can be put down where nothing else
        // is (collision on: editor +0x8ac bit 6); display mode 2 if so, 5 (red) if not.
        bool bHeldValid = true;
        void CheckHeld()
        {
            if (!HeldHandle) { bHeldValid = true; return; }
            bool bOk = true;
            if (Opt.Collision)
            {
                std::vector<int> Mine = FollowerHandles;
                Mine.push_back(HeldHandle);
                for (int H : Mine)
                    if (DropperWorld::OverlapsOthers(H, Mine, 2.0)) { bOk = false; break; }
            }
            if (bOk != bHeldValid || bOk == false)
            {
                DropperWorld::SetDisplayMode(HeldHandle, bOk ? 0 : 5);
                for (int F : FollowerHandles) DropperWorld::SetDisplayMode(F, bOk ? 0 : 5);
            }
            bHeldValid = bOk;
        }

        // 82863650 / 82863e94: with snapping on (the properties menu's option, editor +0x8ac bit 7),
        // the carried terrain piece snaps to the pieces and ground near where the cursor has it.
        void SnapHeld()
        {
            if (!HeldHandle || !Opt.Snap) return;
            const DropperWorld::Placed* P = DropperWorld::Get(HeldHandle);
            if (!P || !DropperSnap::HasLocators(P->Id)) return;
            FVector Pos = Add(Cursor, HeldOffset);
            DropperWorld::Quat Rot = P->Rotation;
            if (DropperSnap::Solve(SweepWorld, HeldHandle, FollowerHandles, Pos, Rot)) MoveHeld(Pos, Rot);
            else if (std::fabs(P->Position.X - Pos.X) + std::fabs(P->Position.Y - Pos.Y) + std::fabs(P->Position.Z - Pos.Z) > 1e-3)
                MoveHeld(Pos, P->Rotation);   // off the snap: back with the cursor
        }
    }

    void Enter(const FVector& Start, double FacingYaw)
    {
        bActive = true;
        Accum = 0;
        Cursor = Start;
        LeashCentre = Start;
        // 82864dd0's leash flag (+0x5d0): the dropper screen's open (825af5f0) passes 0; only the
        // other entry (825ad788) leashes the cursor.
        bLeash = false;
        CursorRotation = DropperWorld::Quat{};
        bCatalogCamera = false;
        bCamFirstUpdate = true;   // 8285fa98: the eye starts at the cursor
        CamPos = Start;
        PendingZoom = 0;
        HeldHandle = 0;
        HoverHandle = 0;
        bRotAnim = bMoveAnim = false;
        LockBits = 0;
        StepCounter = 0;
        ElevateAccel = 0.3f;
        Yaw = FacingYaw * Pi / 180.0;
        // 8285fa98: distance 20 (61D0436E), first pitch change -20 deg (93D10E5C) from level.
        DistTarget = DistSmooth = CamStartDistance;
        Pitch = std::clamp(double(-CamStartPitch) * Deg, CamLayout[2] * Deg, CamLayout[4] * Deg);
        DropperInput::Reset();
        CurMode = FreeCam;
        CurTuning = FreeCamTuning;
        Update();
        PrevCamPos = CamPos;
        PrevCamForward = CamForward;
        PresentAlpha = 1.0;
    }

    void Exit()
    {
        bActive = false;
        CurMode = Off;
    }

    bool Active() { return bActive; }
    void SetMode(Mode Mo)
    {
        if (CurMode == Mo) return;
        if (CurMode == Rotate) StepRotAnim(false);
        if (CurMode == LockAxis) { StepMoveAnim(false); StepCounter = 0; }
        CurMode = Mo;
    }
    Mode GetMode() { return CurMode; }
    void SetTuning(Tuning Tn) { CurTuning = Tn; }

    void Tick(UWorld* World, float Dt)
    {
        if (!bActive) return;
        SweepWorld = World;
        Accum += Dt;
        int N = 0;
        while (Accum >= Step && N < 4)
        {
            Accum -= Step;
            DropperInput::Update(float(Step));
            Update();
            N++;
        }
        if (Accum > Step) Accum = 0;
    }

    void UpdateStep(UWorld* World)
    {
        SweepWorld = World;
        if (bActive) Update();
    }

    // After a frame's editor steps: snapping and the placement check once, for where the carried
    // object ended up (per step they cost several times their worth with big pieces near).
    void FinishSteps()
    {
        if (!bActive) return;
        SnapHeld();
        CheckHeld();
    }

    void SetViewer(AActor* Character) { Viewer = Character; }

    void Present(double Alpha)
    {
        PresentAlpha = std::clamp(Alpha, 0.0, 1.0);
        if (!HeldHandle || HeldHandle != PrevHeldHandle) return;
        const DropperWorld::Placed* P = DropperWorld::Get(HeldHandle);
        if (!P) return;
        const double A = PresentAlpha;
        const FVector Pos = Add(PrevHeldPos, Mul(Sub(P->Position, PrevHeldPos), A));
        // normalised lerp on the shorter arc
        DropperWorld::Quat Q0 = PrevHeldRot, Q1 = P->Rotation;
        if (Q0.X * Q1.X + Q0.Y * Q1.Y + Q0.Z * Q1.Z + Q0.W * Q1.W < 0) { Q1.X = -Q1.X; Q1.Y = -Q1.Y; Q1.Z = -Q1.Z; Q1.W = -Q1.W; }
        DropperWorld::Quat Q{ Q0.X + (Q1.X - Q0.X) * A, Q0.Y + (Q1.Y - Q0.Y) * A, Q0.Z + (Q1.Z - Q0.Z) * A, Q0.W + (Q1.W - Q0.W) * A };
        const double L = std::sqrt(Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W);
        if (L > 1e-9) { Q.X /= L; Q.Y /= L; Q.Z /= L; Q.W /= L; }
        DropperWorld::ShowAt(HeldHandle, Pos, Q);
        PoseFollowers(Pos, Q, true);
    }

    bool View(FVector& Location, FRotator& Rotation, float& FovDegrees)
    {
        if (!bActive) return false;
        const double A = PresentAlpha;
        Location = Add(PrevCamPos, Mul(Sub(CamPos, PrevCamPos), A));
        FVector F = Add(PrevCamForward, Mul(Sub(CamForward, PrevCamForward), A));
        const double Fl = Len(F);
        F = Fl > 1e-9 ? Mul(F, 1.0 / Fl) : CamForward;
        Rotation.Yaw = std::atan2(F.Y, F.X) * 180.0 / Pi;
        Rotation.Pitch = std::asin(std::clamp(F.Z, -1.0, 1.0)) * 180.0 / Pi;
        Rotation.Roll = 0;
        // TODO(exact): the editor camera's lens (editor_camera field 1FD11E7A = 80 is a candidate).
        FovDegrees = 0.f;   // 0 = keep the skate camera's lens
        return true;
    }

    FVector CursorPosition() { return Cursor; }
    void SetCursor(const FVector& At) { Cursor = At; LeashCentre = At; }
    FVector ViewLocation() { return CamPos; }
    FVector ViewForward() { return CamForward; }
    int Held() { return HeldHandle; }
    void Hold(int Handle, bool bFrame)
    {
        if (HoverHandle) DropperWorld::SetDisplayMode(HoverHandle, 0);
        DropperWorld::SetDisplayMode(Handle, 0);
        HeldHandle = Handle;
        HoverHandle = 0;
        if (const DropperWorld::Placed* P = DropperWorld::Get(Handle))
        {
            HeldOffset = Sub(P->Position, Cursor);
            HeldStartRotation = P->Rotation;
            DropperWorld::SetGhost(Handle, true);
            if (!bFrame) return;
            // 82866a70: zoom the orbit camera so the object fits the view (no leash only).
            if (bLeash) return;
            const FVector Box = DropperWorld::LocalSize(P->Id);
            // The object's box (vtbl+0x14: full size, metres) on its own axes (vtbl+0x1c), seen along
            // the camera's right and up rows (+0x780 / +0x790): its width and height on screen.
            FVector Right{ CamForward.Y, -CamForward.X, 0 };   // forward x up
            if (Len(Right) < 1e-6) Right = FlatRight();
            Right = Mul(Right, 1.0 / Len(Right));
            const FVector Up{ Right.Y * CamForward.Z - Right.Z * CamForward.Y, Right.Z * CamForward.X - Right.X * CamForward.Z,
                              Right.X * CamForward.Y - Right.Y * CamForward.X };
            const FVector Axis[3] = { DropperWorld::Rotate(P->Rotation, FVector{ 1, 0, 0 }), DropperWorld::Rotate(P->Rotation, FVector{ 0, 1, 0 }),
                                      DropperWorld::Rotate(P->Rotation, FVector{ 0, 0, 1 }) };
            double W = 0, H = 0;
            const double Sizes[3] = { Box.X / M, Box.Y / M, Box.Z / M };
            for (int j = 0; j < 3; j++)
            {
                const double Size = Sizes[j];
                W += std::fabs(Dot(Right, Axis[j])) * Size;
                H += std::fabs(Dot(Up, Axis[j])) * Size;
            }
            // half the bigger side over tan(half the 7F95A55E view angle) (82517a28 = tan)
            const double Fit = std::max(W, H) * 0.5 / std::tan(T().PickupFov * 0.5 * Deg);
            const double Now = Len(Sub(CamPos, Cursor)) / M;   // eye (+0x7b0) to cursor (+0x5b0)
            double Want = std::max(double(T().MinFrame), std::max(Now, Fit));   // objectdropper layout C4A69E25
            if (bCatalogCamera) return;
            Want = std::min(double(T().MaxFrame), Want);                     // D9DEC255
            PendingZoom += float(Want - DistTarget);                          // into the camera's zoom input (+0x7e0)
        }
    }
    void HoldGroup(int Handle, const std::vector<int>& Others, bool bFrame)
    {
        Hold(Handle, bFrame);
        Followers.clear();
        FollowerHandles.clear();
        const DropperWorld::Placed* P = DropperWorld::Get(Handle);
        if (!P) return;
        const DropperWorld::Quat Inv = Conj(P->Rotation);
        for (int O : Others)
        {
            const DropperWorld::Placed* Q = O != Handle ? DropperWorld::Get(O) : nullptr;
            if (!Q) continue;
            Followers.push_back({ O, DropperWorld::Rotate(Inv, Sub(Q->Position, P->Position)), DropperWorld::Mul(Inv, Q->Rotation) });
            FollowerHandles.push_back(O);
            DropperWorld::SetDisplayMode(O, 0);
            DropperWorld::SetGhost(O, true);
        }
    }
    const std::vector<int>& HeldGroup() { return FollowerHandles; }
    bool HeldValid() { return !HeldHandle || bHeldValid; }

    int Release()
    {
        const int H = HeldHandle;
        if (H) DropperWorld::SetGhost(H, false);
        for (const Follower& F : Followers) DropperWorld::SetGhost(F.Handle, false);
        Followers.clear();
        FollowerHandles.clear();
        HeldHandle = 0;
        bRotAnim = bMoveAnim = false;
        return H;
    }
    int Hovered() { return HoverHandle; }
    void ResetRotation()
    {
        if (!HeldHandle) return;
        MoveHeld(Add(Cursor, HeldOffset), DropperWorld::Quat{});
        CursorRotation = DropperWorld::Quat{};   // 828624a0 keeps the reset rotation at +0xe0
    }

    Options GetOptions() { Opt.CursorSpeed = SpeedScale; return Opt; }
    void SetOptions(const Options& O)
    {
        Opt = O;
        SpeedScale = O.CursorSpeed;
        CamYawScale = O.InvertX ? -1.f : 1.f;
        CamPitchScale = O.InvertY ? -1.f : 1.f;
    }

    void SetCatalogCamera(bool bOn)
    {
        // 825b5ed4 (catalog opens): the camera's turn input (+0x7d0 / +0x7d4) is zeroed and it locks
        // on the cursor; 825b6090 (catalog closes) frees it. Carried on (the catalog object was
        // chosen), the orbit continues from the same eye: its pitch is the height angle (minus the
        // view pitch).
        if (bOn) DYaw = DPitch = 0;
        else if (bCatalogCamera && HeldHandle) Pitch = -Pitch;
        bCatalogCamera = bOn;
    }

    FVector SpawnCursor()
    {
        // 825b91d0 / 82866848: without a leash the cursor goes to camera eye + forward * distance
        // (82861630; its park bounds check 8285f388 has no counterpart here: no park volume).
        if (!bLeash) Cursor = Add(CamPos, Mul(CamForward, DistSmooth * M));
        return Cursor;
    }

    DropperWorld::Quat SpawnRotation() { return CursorRotation; }

    void CursorToCamera()
    {
        // 82866988 / 828668d8: the carried object went (removed or placed); without a leash the
        // cursor goes back to the camera eye (82861580).
        if (!bLeash) Cursor = CamPos;
    }

    void Placed(int Handle)
    {
        // 828668d8 -> 82862ee0: the placed object's rotation becomes the cursor's.
        if (const DropperWorld::Placed* P = DropperWorld::Get(Handle)) CursorRotation = P->Rotation;
        CursorToCamera();
    }
}
