//=============================================================================//
//
// Purpose: Script remote-function server path
//
//=============================================================================//
#if defined(CLIENT_DLL)
#ifndef SCRIPTREMOTEFUNCTIONS_SERVER_H
#define SCRIPTREMOTEFUNCTIONS_SERVER_H

#include "scriptremotefunctions_shared.h"

class CClient;
class NET_ScriptMessage;

bool ScriptRemoteServer_ProcessMessage(CClient* pClient, NET_ScriptMessage* pMsg);

#endif // SCRIPTREMOTEFUNCTIONS_SERVER_H
#else // !CLIENT_DLL
#ifndef SCRIPTREMOTEFUNCTIONS_SERVER_H
#define SCRIPTREMOTEFUNCTIONS_SERVER_H

#include "thirdparty/detours/include/idetour.h"
#include "scriptremotefunctions_shared.h"

class CClient;
class NET_ScriptMessage;

bool ScriptRemoteServer_ProcessMessage(CClient* pClient, NET_ScriptMessage* pMsg);
// A BRIDGE_C2S_SCRIPTREMOTE_MAGIC frame (untyped): resolved by name, re-encoded with this
// dedi's registration and run through ScriptRemoteServer_ProcessMessage.
bool ScriptRemoteServer_ProcessNamedMessage(CClient* pClient, NET_ScriptMessage* pMsg, const uint8_t* pFrame, int nBytes);
void ScriptRemoteC2S_DropFnCache(void);
void ScriptRemoteC2S_LevelShutdown(void);

//-----------------------------------------------------------------------------
// S->C ScriptRemote send-side: read name+args off the SQVM, then native encode.
//-----------------------------------------------------------------------------
class VScriptRemoteS2CBridge : public IDetour
{
	virtual void GetAdr(void) const;
	virtual void GetFun(void) const;
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // SCRIPTREMOTEFUNCTIONS_SERVER_H
#endif // CLIENT_DLL
