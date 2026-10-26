#ifndef CORE_S30_BRIDGE_USERMESSAGE_H
#define CORE_S30_BRIDGE_USERMESSAGE_H
//=============================================================================//
//
// Purpose: the dedi's svc_UserMessage (S3 37) on the S30 client.
//
// As the S21 bridge does, only the user messages whose native handler is known
// to read the dedi's payload run, each by a direct call of the handler S30 hooked
// for its name: SayText, WeapProjFireCB, PlayerNotifyDidDamage, RemoteBulletFired,
// RemoteWeaponReload. Others are consumed and logged once per type.
//
//=============================================================================//
#include <cstdint>

struct S30BitReader_t;

// Hooks CUserMessages::HookMessage to learn each message's handler.
bool S30UserMessage_Install(void);
// The body of one S3 37 message ([8 type][12 length bits][payload]). False when
// the stream cannot continue.
bool S30UserMessage_OnS3(S30BitReader_t& r);

#endif // CORE_S30_BRIDGE_USERMESSAGE_H
