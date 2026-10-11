#pragma once

#include <Windows.h>
#include <string>

#include "SDK/Basic.hpp"
#include "SDK/CoreUObject_classes.hpp"
#include "SDK/Engine_classes.hpp"

// The mod's folder: the folder that holds bin\SkateMod.dll (any drive or folder). Rel is relative
// to it, e.g. ModPath("skate-data\\settings").
const std::string& ModRoot();
std::string ModPath(const char* Rel);
std::wstring ModPathW(const char* Rel);

// Log file in <mod folder>\Logs (one per process, readable while the game runs).
void Log(const char* Fmt, ...);

// Called once per game frame on the game thread (from the ProcessEvent hook).
namespace Client
{
    void Tick(SDK::UWorld* World, float DeltaSeconds);
    void DrawHud(SDK::UCanvas* Canvas);
    bool IsSkating();
    // The skater and where the mod last placed it (false when not skating).
    bool GetPin(SDK::ACharacter** Character, SDK::FVector* Location);
}
namespace Server { void Tick(SDK::UWorld* World, float DeltaSeconds); bool AnySkating(); }

// Diagnostics for the health log (Main.cpp): the mod's own time per rendered frame (skate tick +
// HUD draw) and the game's ProcessEvent calls, summed since the last call, which resets them.
struct ModFrameCost { double TotalMs = 0, WorstMs = 0, HudMs = 0; long long Frames = 0, GameCalls = 0; };
ModFrameCost TakeModFrameCost();
// The game functions called most on the game thread since the last call (per frame), "name n, ...".
std::string TakeTopGameCalls(int Count, long long Frames);

// The local player's character, or null (client side).
SDK::ACharacter* GetLocalCharacter(SDK::UWorld* World);
SDK::APlayerController* GetLocalController(SDK::UWorld* World);

// Adds an object we created to the garbage collector's root set so it is never freed
// (EInternalObjectFlags::RootSet on its GUObjectArray item).
void KeepAlive(SDK::UObject* Object);

// A loaded material by object name (a package of the same name is skipped; FindObjectFast
// returns whichever object with that name it meets first).
SDK::UMaterialInterface* FindMaterialByName(const char* Name);

// True while the Fortnite window has keyboard focus.
bool GameWindowFocused();
inline bool KeyDown(int VirtualKey) { return (GetAsyncKeyState(VirtualKey) & 0x8000) != 0; }
