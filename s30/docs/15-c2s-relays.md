# 15. Client-to-server relays: S30 wire, S3 dedi reader, transforms (2026-10-26)

The spec for section B of doc 13. All S30 facts come from the S30.1 IDB (copy in `analysis/idb/fork_c2s`) and
`analysis/netmsg_decomp`. All dedi facts come from this repo's server.dll source (DEDICATED half) and the S21
bridge (`tools/s21-r5sdk`). Anything not proven is marked **unproven**.

Bit order everywhere is LSB first. S30's bf_write (`0x2380C0` WriteUBitLong) ORs `value << (bit & 31)` into dwords,
which is the S3 order. A "31+1" field below is S30 writing the low 31 bits, then the sign as one bit. That equals a
plain 32-bit two's-complement value, as `ReadLong` reads it.

## 1. Remote_ServerCallFunction (client script → server function)

### Path
`Remote_ServerCallFunction` native `0x6B4640` (registered at `0x68E6E2`) takes the local player from handle table
`0x58D4A18`. It calls `CallRemoteFunction` `0xAD48A0(dict 0x24DDB60, vm, 2, 0, player)`, which sends the call through
`SendNetMsg` `0x2A03D0(g_netchan 0x1E1BDC8, &msg, 0, 0)` (call site `0xAD48A0` decomp line 941). Calls are only
encoded in remote-function state 2 (`dword_1424DDBE0`). Registration needs state 1.

### The message: S30 type 4
- vtable `0x17F2EF0`
- GetType `0x2F96D0` returns `mov eax, 4`
- sizeof `0x88`
- Process `0x2F96E0`
- ReadFromBuffer `0x48F770`
- WriteToBuffer `0x48F6D0`

Message fields (stack object `v119` in `0xAD48A0`):

| offset | field |
|---|---|
| +8 | group = 2 (slot 6 returns it) |
| +12 | 1 |
| +32 | typed flag = registration flags & 1 |
| +104 | bf_write data pointer |
| +112 | bytes = 2048 |
| +116 | max bits = 0x4000 |
| +120 | bits written |
| +124 | overflow byte |

S30's bf_write is `{+0 data, +8 bytes, +12 max bits, +16 cursor, +20 overflow}`.

WriteToBuffer `0x48F6D0`, after the 7-bit type:
```
[1  typed]            msg+32 != 0
[15 payloadBytes]     (bitsWritten + 7) >> 3, written 15 bits + 1 sign bit
[1  0]                (the sign of the length)
[payloadBytes*8 payload]
```
This is byte-for-byte the S21 type-4 framing: S30 `t04_wtb` = S21 `t04_wtb`, IDENTICAL in `netmsg_decomp`. It is
also what the dedi's `NET_ScriptMessage::ReadFromBuffer` reads (`common/netmessages.cpp:539`: 1 bit, then
`ReadShort` = 15 length + sign, then the bytes).

### Payload (built by `0xAD48A0`, decomp lines 249–946)
```
[10 funcIndex]   entry+8 (u16), the S30 server-function index (registration order)
[31 0][1 0]      the reserved 32-bit sequence field, always 0 from S30 (S21's bridge stamps it, see 3.4)
args, in schema order, as below
pad to a byte (WriteToBuffer sends whole bytes)
```
Only typed registrations reach the client: every server function is registered through `0xAD4140` →
`0xAD2FA0(…, name, flags=1, …)` (the only caller, `0xAD430B`, `mov r8d, 1`; flags become `1|4`). So the untyped
"extras" tail (below) never occurs for server calls.

Per param, by S30 schema type byte (`entry+20`, see 1.3):

| S30 type | script value accepted | bits written |
|---|---|---|
| 1 float | int or float (int converted) | bits ≥ 32: 32 raw IEEE (`0x5D5560` WriteBits). Else the value must be in [min,max] or the call fails; writes `bits` of `(int)(((v-min)/(max-min))*((1<<bits)-1)+0.5)` |
| 3 vector | vector | 3 × the float rule with the same min/max/bits |
| 5 int | int or float (truncated) | must be min ≤ v ≤ **max (inclusive)**; writes `v-min` in `bitlen(max-min)` bits (loop `for(i=max-min;i;i>>=1) ++bits`; 0 bits if max == min) |
| 6 bool | bool only (SQ tag 5) | 1 bit |
| 36 (0x24) string | string < 256 | each char as 8 bits including the NUL (7 + sign). No S30 registration produces type 36: `0xAD3550` only accepts bool/int/float/vector/entity/typed_entity/itemflavor |
| 42 (0x2A) entity | entity or null | 32 bits ("31+1") = `*(entity+8)`. Null writes 0, or fails when convar `qword_14269D5C8+92` is set. Lightweight and non-networked entities fail |
| 43 (0x2B) typed_entity | same as 42 | same as 42. Also, when convar `qword_14266B5F8+92` is set, the class signifier string (in the schema) is compared with `*(entity+1144)` |
| 44 (0x2C) itemflavor | int or float | 32 bits ("31+1"), the raw value. The schema kind byte (range or type) only moves the schema cursor (+9 or +5); nothing is range-checked |
| 2, 4, 7..35, 37..41 | — | nothing written |

Entity handle format, proven: `GetEncodedEHandle` `0xAC8C90` reads `*(ent+8)` as index = low 16 bits and serial =
high 16 bits. The networked check at `0xAD48A0` line 606 builds `index | (serial << 16)` from table `0x58D4A18`. The
dedi's handle is the same: `sdk_entity_state.h`, "dedi memory EHANDLE is (serial<<16)|index". **So entity values
pass to the dedi unchanged.** Null is 0, which the dedi treats as "no entity" (`ScriptRemote_DecodeParam`).

Untyped extras tail (flags bit 0 clear, lines 819–938; not reachable for server functions):
- `[4 count ≤ 16]`
- then per value `[2 tag]`: 0 null; 1 bool + 1 bit; 2 int + 32 ("31+1"); 3 float + 32 raw.

### Where registrations live, and lookups
Server functions are kept in a dictionary at `0x24DDB60`:

| | |
|---|---|
| tree / less-func object | `off_1424DDB70` |
| elements | `qword_1424DDB78`, stride **336** bytes: `{u16 left, u16 right, u16 parent, u16 tag}` then the 328-byte entry at **element+8** |
| root | low word of `dword_1424DDB90` |
| count | `0x24DDB92` |

The tree is keyed by the name hash `0x5D5140(name)`.

Entry layout. The same struct is used by the client/UI store `0xAD32B0`; field use was checked in `0xAD48A0`.

| offset | field |
|---|---|
| +0 | int64: hFunc for local functions, -1 for server functions |
| +8 | u16 **index** |
| +12 | int name hash |
| +16 | flags (1 typed, 2 UI, 4 server) |
| +20 | schema[256] |
| +276 | schema bytes |
| +280 | param count |
| +288 | name string pointer. **Stored only** when flags & 2 or convar `qword_14269EA88+92` is set, so not for server functions by default |

- **Index**: `0xAD2FA0` reads the counter at `0xAD30B6` (`movzx eax, word [rip+…]` → `0x24DDB68`) into entry+8. It
  increments it at `0xAD3228` only when the insert succeeds, so it is the 0-based order of successful
  `Remote_RegisterServerFunction` calls.
- **By name**: `0xAD2E20(unused, name)` returns entry or 0 (`0xAD48A0` line 211).
- **By index**: no engine function exists. Walk the elements comparing entry+8 and flags & 4. The name is not
  stored, so the bridge must capture it at registration.
- **Recommended capture**: detour `0xAD2FA0(this, name, flags, schema, schemaBytes, params)`. Signature
  `40 55 56 57 48 81 EC 70 01 00 00 49 8B E9 41 8B F0 48 8B FA E8` (unique). Read the u16 at `0x24DDB68`
  (rip-relative at `0xAD30B6`+3) before calling the original; on return 0, record
  `index → {name, schema, bytes, params}`. Return 3 means the name already exists (hash), 2 means insert failed.

Schema format (built by `0xAD3550`, doc 12 step 9): per param a type byte, then:
- float 1 / vector 3: + min f32, max f32, bits u32
- int 5: + min i32, max i32
- bool 6: nothing
- entity 0x2A: nothing
- typed_entity 0x2B: + NUL-terminated class
- itemflavor 0x2C: + kind u8, then 2 ints (kind ≠ 0) or 1 int

## 2. Dedi side today, and the name-carried frame

### How S3 68 reaches the dedi now
`NET_ScriptMessage::ReadFromBuffer` (`common/netmessages.cpp:539`) reads `[1 typed][16 ReadShort len][len bytes]`
into `m_Buffer` (2048) and `m_DataIn`. `Process` (`:574`):
- **typed=1** → `ScriptRemoteServer_ProcessMessage` (`game/server/scriptremotefunctions.cpp:260`):
  1. `[10 index][31 seqLo][1 seqHi]`; seq ≠ 0 → `CClientExtended::AcceptBridgeRelSeq` (`engine/client/client.cpp:430`,
     64-wide window shared with chat).
  2. `ScriptRemoteC2S_CheckRate` (`:67`).
  3. `ScriptRemoteServer_GetFunctionByIndex` (`game/shared/scriptremotefunctions_shared.cpp:88`).
  4. `ScriptRemoteC2S_IsDeniedName` (`:166`).
  5. Player script instance.
  6. `ScriptRemote_DecodeParam` per param (`scriptremotefunctions_shared.cpp:328`, scalar codec
     `game/shared/scriptremote_scalecodec.h`).
  7. Padding check (0–7 bits left).
  8. `ScriptRemoteC2S_ResolveFunction`.
  9. `ExecuteFunction`, with the SCRIPT_ERROR containment `ScriptRemoteC2S_CancelHostShutdown` +
     `ScriptRemoteC2S_NoteScriptError` (`:396–416`).
- **typed=0** → only the latency report (magic `0x31545242` 'BRT1' + 16-bit RTT).

The dedi's codec decodes exactly what S30 encodes:
- int `IntBitsForRange` = bit length of (max-min)
- float/vector quantize with the same formula; raw at ≥ 32 bits
- bool 1 bit
- entity and itemflavor `ReadLong`

Its type bytes are S3's: entity **40**, typed_entity **41**, itemflavor **42**, i.e. S30's minus 2; scalars are the
same. **The only incompatibility is the index and the per-function schema.** The dedi registers its own
`Remote_RegisterServerFunction` list (`ScriptRemoteServer_RegisterFunction`: index = dedi order, max 8 params, 768
functions). S30's client scripts register a different list, order and ranges. A verbatim relay would call the wrong
function or misdecode.

### Design: name-carried C2S frame (untyped S3 68)
The client sends on the existing untyped lane. `[7 68][1 typed=0][16 byteLen][frame]`, where the frame is byte-aligned
like the S2C `SR2D` frame:
```
u32 magic        BRIDGE_C2S_SCRIPTREMOTE_MAGIC = 0x53523243 ("C2RS" in memory; distinct from 'SR2D' 0x53523244
                 and 'BRT1' 0x31545242)
u32 relSeq       0 = none; else deduped by AcceptBridgeRelSeq. The client uses ONE counter for this and chat ("brc"),
                 as S21's s_c2sRelSeq does
u8  nameLen      1..64; name printable ASCII
u8[nameLen] name
u8  argCount     ≤ SCRIPT_REMOTE_SERVER_MAX_PARAMS (8)
per arg: u8 tag + value
  1    float        f32
  3    vector       3 × f32
  5    int          i32
  6    bool         u8
  0x28 entity       u32 handle (serial<<16 | index; 0 = null)
  0x29 typed_ent    u32 handle
  0x2A itemflavor   i32
```
The client fills values from S30's decoded payload: decode with the captured S30 schema (section 1), dequantizing
floats with the S30 range, then re-tag with the S3 numbers. S30 type 0x24 strings have no S3 counterpart: drop the
call.

Dedi changes (server.dll, DEDICATED half):
1. `game/shared/scriptremotefunctions_shared.h` (`!CLIENT_DLL` half, next to `BRIDGE_S2C_SCRIPTREMOTE_MAGIC`): add
   `BRIDGE_C2S_SCRIPTREMOTE_MAGIC` and a max name length (64).
2. `common/netmessages.cpp` `NET_ScriptMessage::Process` (`:574`), untyped branch: if ≥ 32 bits, peek u32. If it is
   the C2S magic, call the new `ScriptRemoteServer_ProcessNamedMessage(pClient, this)`; keep the 'BRT1' path as is.
   Today's code reads magic + 16 bits only when ≥ 48 bits are left; restructure to read the magic first.
3. `game/server/scriptremotefunctions.cpp`:
   - Split `ScriptRemoteServer_ProcessMessage` after decode (lines 320–416: player instance, `ResolveFunction`,
     weapon-switch mirror suppress, `ExecuteFunction` + SCRIPT_ERROR containment) into
     `static bool ScriptRemoteC2S_Execute(CClient*, const ScriptRemoteFuncDesc_t*, ScriptVariant_t*, int)`, used by
     both paths. The player script instance stays arg 0.
   - Add `ScriptRemoteServer_ProcessNamedMessage`:
     1. IsActive check (as `:265`).
     2. Bounds-checked byte walk of the frame. Total ≤ 2048; trailing bytes are a reject.
     3. `AcceptBridgeRelSeq(relSeq)` when ≠ 0 (silent drop as `:281`).
     4. `ScriptRemoteC2S_CheckRate`.
     5. `ScriptRemoteServer_FindFunction(name)` (`shared.cpp:78`); unknown → `Warning` + reject.
     6. `ScriptRemoteC2S_IsDeniedName`.
     7. `argCount == pFunc->nParamCount`.
     8. Per param, build `ScriptVariant_t`s with the dedi's own desc:
        - tag 1 ↔ SRP_FLOAT, 3 ↔ SRP_VECTOR, 6 ↔ SRP_BOOL
        - 5 ↔ SRP_INT, and also SRP_ITEMFLAVOR (both plain ints)
        - 0x2A ↔ SRP_ITEMFLAVOR, and also SRP_INT (int range applies)
        - 0x28/0x29 ↔ SRP_ENTITY or SRP_TYPED_ENTITY
        - Range rules as the codec: int [min,max]; float/vector finite and in range when floatBits < 32, finite only
          at 32.
        - Entity: reuse `DecodeParam`'s entity branch (`shared.cpp:398–440`: 0 → null, `SDKEntityState_Resolve`,
          typed classname check against `szEntityClass`). Factor it into a helper, e.g.
          `ScriptRemote_EntityArgToVariant(uint32_t eh, desc, out…)`, called by both.
     9. `ScriptRemoteC2S_Execute`.
   - Optional: a `sv_scriptremote_c2s_named` ConVar (`FCVAR_RELEASE`, default 1) to turn the lane off.
4. No change to the typed path, so S21 clients keep working.

## 3. The other relays (S30 writer vs S3 dedi reader)

S30 writers are in `analysis/netmsg_decomp/s30/tNN_wtb.c`. S21 counterparts are in `s21/`. "Same as S21" means the
normalized decompiles differ only in decompiler variable names. The S21 bridge relays those S21 messages to the dedi
verbatim, with the type renumbered (`net_bridge_c2s.cpp:317–326`).

| S30 type | message | S30 body (after 7-bit type) | vs S21 | dedi S3 type | transform |
|---:|---|---|---|---:|---|
| 3 | net_StringCmd | NUL-terminated string (`msg+32`) | same as S21 3 | 3 | none. Optional seq lane, 3.4 |
| 5 | net_SetConVar | `[8 count]` then count × (name string, value string). S30 entry stride 4356 (value buffer 4096), S21 2308 | same wire as S21 5 | 4 | renumber. The dedi's value buffer is 260 (`kUserInfoMirrorValueMax`, S21 c2s): drop or clip values ≥ 260 bytes |
| 70 | clc_ClaimClientSidePickup | `[10][8][32 "31+1"][32 "31+1"]` (`a[8]`, `a[9]`, then two ints) | **IDENTICAL** to S21 63 | 57 | renumber only |
| 73 | clc_ClientSayText | string (`msg+32`), `[1 team]` (`msg+288`) | same as S21 66 | 61 | renumber. Optional "brc" seq, 3.4 |
| 82 | clc_GamepadMsg | `[8][32][32 "31+1" NEW: msg+44][8][64][str][str][str]` | S21 74 is `[8][32][8][64][str][str][str]` | 65 | **drop the 32 bits after the first 40**, copy the rest verbatim |
| 72 | clc_ClientTick | 3 × 32 (doc 11) | S21 had 2 | 60 | done in `bridge_netchan_s30` |
| 64 | clc_Move | | | 46 | done in `bridge_usercmd_s30` |

Notes:
- GamepadMsg: S21 74 → S30 82 is the doc 11 mapping (+8). The three strings sit at +56/+72/+80 in S21 and
  +57/+73/+81 in S30, so the struct only gained the int at +44. That S3 65 reads exactly S21 74's layout rests on
  the S21 bridge relaying it verbatim. It is **unproven against `r5apex_ds.exe`** itself (no S3 dedi IDB).
- ClaimClientSidePickup, SayText and StringCmd S3 readers are likewise taken from the S21 bridge's verbatim relay
  (S21 "VERIFIED (GetType; RFB …)" comments). They were not re-derived from the dedi binary.

### 3.4 Sequencing and redundancy (S21 bridge, and what the dedi strips)
- **StringCmd**: S21 `S21Bridge_RelayStringCmdSeq` (`net_bridge_c2s.cpp:2185`) rewrites the command to
  `"brq <seq> <cmd>"`, with its own counter `s_stringCmdSeq`, when `nLen + 16 < 512`. It is sent now and then
  `bridge_c2s_stringcmd_redundancy` (2) more times, 50 ms apart (ring of 32). The dedi strips it in
  `CClient::VProcessStringCmd` (`engine/client/client.cpp:1374`): it parses `brq <digits ≤ 10> `, keeps a per-slot
  64-wide window (`m_nBridgeStrCmdSeqHigh` / `m_nBridgeStrCmdSeenMask`) before the quota, and drops duplicates.
- **Chat**: `S21Bridge_C2SRel_RelayChat` (`net_bridge_c2s_reliable.cpp:186`) writes S3 61 with
  `"brc <seq> <text>"` + team bit, using the shared counter `s_c2sRelSeq`. The dedi strips it in
  `CServerGameDLL::OnReceivedSayTextMessage` (`game/server/gameinterface.cpp:492–513`) through `AcceptBridgeRelSeq`.
- **ScriptRemote (typed)**: `S21Bridge_C2SRel_RelayScriptRemote` (`net_bridge_c2s_reliable.cpp:136`) rewrites the
  31-bit seq field (+ the seq-high bit 0) in the payload with `s_c2sRelSeq`. The dedi dedupes at
  `scriptremotefunctions.cpp:276–288`.
- Redundancy for chat and ScriptRemote: ring of 32, `bridge_c2s_rel_redundancy` (2) extra sends 50 ms apart, pumped
  under the C2S lock (`S21Bridge_C2SRel_PumpLocked`). For the S30 named frame, put the seq in its `relSeq` field and
  use the same ring.

## 4. Capturing the messages on S30
The bridge already detours `SendNetMsg` `0x2A03D0` (`bridge_usercmd_s30`, for clc_Move). For every message, get its
type from vtable slot 7 (GetType). For types 3, 4, 5, 70, 73, 82, either:
- serialize with its own WriteToBuffer (vtable slot 5) into a scratch S30 bf_write
  (`{data, bytes, maxbits, cursor=0, overflow=0}` as above), then transform; or
- for type 4, read the payload directly: `msg+104` data, `msg+120` bits, `msg+32` typed.

The engine's own datagrams never reach the dedi (the bridge builds the C2S packets), so nothing needs suppressing.
