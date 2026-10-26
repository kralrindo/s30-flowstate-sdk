#ifndef CORE_S30_BRIDGE_SCRIPTREMOTE_H
#define CORE_S30_BRIDGE_SCRIPTREMOTE_H
//=============================================================================//
//
// Purpose: the dedi's server-to-client script remote calls on the S30 VMs.
//
// The dedi SDK carries them by name in net_ScriptMessage (S3 68). Each call
// runs on the CLIENT or UI VM through CSquirrelVM::FindFunction /
// ExecuteFunction, only for names the client scripts registered with
// Remote_RegisterClientFunction / Remote_RegisterUIFunction, and only once the
// snapshot tick it was stamped with has arrived (the S21 bridge's
// S21Bridge_HandleS2CScriptRemote / InjectScriptRemote). See s30/docs/12.
//
//=============================================================================//
#include <cstdint>

// Resolves the VM functions and hooks the remote-function registration natives.
bool S30ScriptRemote_Install(void);
// One net_ScriptMessage call frame (engine thread).
void S30ScriptRemote_OnFrame(const uint8_t* pFrame, uint32_t nBytes);
// A snapshot for nTick was processed: runs the calls that were waiting for it.
void S30ScriptRemote_OnSnapshot(uint32_t nTick);

// sdk_script_call <name>: runs a no-argument CLIENT VM function with the next snapshot.
void S30ScriptRemote_QueueClientCall(const char* pszName);

struct S30BitReader_t;
// The client's Remote_ServerCallFunction (S30 net_ScriptMessage 4, r after its 7-bit
// type, nEndBit its end): decoded with the S30 registration and queued to the dedi
// by name (its "C2RS" frame in S3 68, sequence nSeq). False when dropped.
bool S30ScriptRemote_RelayC2S(S30BitReader_t& r, int nEndBit, uint32_t nSeq);

#endif // CORE_S30_BRIDGE_SCRIPTREMOTE_H
