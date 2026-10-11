#include "NetGuard.h"
#include "Common.h"
#include "MinHook.h"

#include <cstring>

namespace
{
    constexpr uintptr_t DestroyNetObjectOffset = 0x67BA470;  // FNetRefHandleManager::InternalDestroyNetObject
    constexpr size_t EntriesOffset = 0x238;                   // this+0x238: net object entries, 0x38 bytes each
    constexpr size_t EntrySize = 0x38;
    constexpr size_t EntryObjectOffset = 0x10;                // entry+0x10: the object it read (+0x44) and crashed on
    const unsigned char Prologue[16] = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };

    using DestroyFn = void(__fastcall*)(void*, uint32_t, void*, void*);
    DestroyFn Original = nullptr;
    int Skipped = 0;

    bool EntryHasObject(void* This, uint32_t Index)
    {
        __try
        {
            const unsigned char* Entries = *reinterpret_cast<unsigned char* const*>(static_cast<unsigned char*>(This) + EntriesOffset);
            if (!Entries) return false;
            const unsigned char* Entry = Entries + size_t(int32_t(Index)) * EntrySize;
            return *reinterpret_cast<void* const*>(Entry + EntryObjectOffset) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void __fastcall Destroy(void* This, uint32_t Index, void* A3, void* A4)
    {
        if (!EntryHasObject(This, Index))
        {
            if (Skipped++ < 20) Log("[netguard] skipped destroying net object %u: its data is already gone (would have crashed the server)", Index);
            return;
        }
        Original(This, Index, A3, A4);
    }
}

void NetGuard::Install()
{
    void* Target = reinterpret_cast<void*>(SDK::InSDKUtils::GetImageBase() + DestroyNetObjectOffset);
    if (std::memcmp(Target, Prologue, sizeof(Prologue)) != 0)
    {
        Log("[netguard] InternalDestroyNetObject does not match this build: not installed");
        return;
    }
    const bool bOk = MH_CreateHook(Target, reinterpret_cast<void*>(&Destroy), reinterpret_cast<void**>(&Original)) == MH_OK &&
                     MH_EnableHook(Target) == MH_OK;
    Log("[netguard] net object destroy guard %s", bOk ? "on" : "FAILED");
}
