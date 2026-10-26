#ifndef CORE_S30_BRIDGE_ACTIVITY_H
#define CORE_S30_BRIDGE_ACTIVITY_H
//=============================================================================//
//
// Purpose: the dedi's queued player activity (S3 ids) on the S30 client: the
// activity selector reads it translated to S30 numbering (s30/docs/14).
// Weapon activities come from the dedi already translated
// (+bridge_client_activity_set s30 on the dedi).
//
//=============================================================================//

bool S30Activity_Install(void);

#endif // CORE_S30_BRIDGE_ACTIVITY_H
