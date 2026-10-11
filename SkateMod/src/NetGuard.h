#pragma once

// Guard for an ATLAS 27.11 server crash in Fortnite's network replication (Iris):
// FNetRefHandleManager::InternalDestroyNetObject (exe+0x67BA470) read the object of a net object
// entry that was already null (access violation at 0x44, 2026-10-07 21:38:26, Magnesium crash
// report p14052). The hosted match server died and the client could no longer move on foot.
// The guard skips destroying an entry that has no object left (nothing to destroy) instead of
// crashing; everything else goes to Fortnite unchanged. Installed only when the function's
// first bytes match this build.
namespace NetGuard
{
    void Install();  // after MH_Initialize
}
