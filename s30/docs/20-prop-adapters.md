# 20. Snapshot prop adapters for netvars S30 re-typed (2026-10-26)

The S30 client log lists these as `[PROPS] … type (send type X, recv type Y)` (enum: 0 Int, 1 Float, 2 Vector,
3 VectorXY, 4 String, 5 Array, 6 Quaternion, 7 Int64, 8 Ticks, 9 Time, 10 DataTable). Today the bridge's lenient
matcher puts each on a dummy RecvProp of the send type (`Netchan_FillDecoder`), so the stream stays aligned but
nothing is written: no weapon mods, no consumable counts, no class mods, no overheat on the client.
Facts below come from the S30.1 IDB copy (`analysis/idb/fork_props`), the dedi source and generated data.

## S30 shapes (proven)

S30 builds these with one helper, **`0x605850`**: `(RecvProp* out, name, offset, stride, …, count, …, element
template)`. The helper makes a DataTable RecvProp (type 10) at `offset`. Its sub-RecvTable is named after the prop
(`0x605500(table, elems, count, name)`). It holds `count` copies of the element template at `offset + i*stride`,
named from `off_1417A5C00`: `"[0000]"`, `"[0001]"`, … All call sites were listed with `scan605850.py` (scratchpad):

| prop (S30) | offset | elements | element type | S30 call site |
|---|---|---|---|---|
| C_WeaponX `m_modBitfieldFromPlayer` | 0x17CC | 3 | Int, stride 4 | 0x75699A |
| `m_modBitfieldInternal` | 0x17D8 | 3 | Int | 0x756A66 |
| `m_modBitfieldCurrent` | 0x17E4 | 3 | Int | 0x756B32 |
| `m_modBitfieldDisabled` | 0x17F0 | 3 | Int | 0x756BFE |
| projectile `m_modBitfield` / `m_overrideMods` | 0x1610 / 0x161C | 3 | Int | 0x73380A / 0x7338D4 |
| `m_modBitfield` (second class; DT_PropSurvival's `m_modBitField` binds to it case-insensitively) | 0x1614 | 3 | Int | 0x928702 |
| `m_bIsPlayerOverheating` | 6464 (0x1940) | 3 | Int, stride **1** (byte) | 0x8E1BB0 (decomp line 616) |
| `m_playerOverheatValue` | 6468 (0x1944) | 3 | Float, stride 4 | 0x8E3D95 |
| `m_timeLastGeneratedPlayerOverheat` | 6480 (0x1950) | 3 | Time, stride 4 | 0x8E3E6A |
| `m_consumableInventory` | 9896 (0x26A8) | 32 | **Int64**, stride 8 | 0x832ACD |

`m_classModsActive` is a real Array RecvProp (type 5) at 9824 (0x2660): element `"m_classModsActive[0]"` Int64,
stride 8, **count 2** (RecvProp +0x58 = 8, +0x5C = 2; decomp `0x835240` line 370–410). The same offset is set at
`0x8450E0` (second table, DT_PlayerDecoy by the log).

So in S30 each weapon/projectile mod mask is **96 bits** (3 dwords), class mods are **128 bits** (2 × Int64),
consumable slots are **u64**, and the three overheat values are **per index 0..2**.

## Meaning of the values (proven)

- **Overheat:** S30's script natives read element 0 only. `IsPlayerOverheating` (native `0x94FD70`) reads the
  byte at +6464; `GetPlayerOverheatValue` (`0x94FC30`) reads the float at +6468. The dedi's scalar → element
  `[0000]`, elements 1 and 2 = 0.
- **Consumables:** S30 `ConsumableInventory_Get` (native `0x8875D0` → `0xBD1A00`) reads 32 u64 at +0x26A8. Per slot
  it pushes `{slot = i, type = bits 0..15, count = bits 16..31, flags = bits 32..63}` (struct `ConsumableInventoryItem
  {slot, type, count, flags}` in S30 `init.nut`), and skips the slot when bits 16..31 are 0. The dedi's existing
  S21 widen proxy (`dt_extend.cpp` `ConsumableInventory_WidenProxy`) already emits `type | count << 16` — exactly
  S30's low 32 bits, flags 0. **Only the element type changes (Int → Int64).** The loot type numbering (`type`) is
  the dedi's loot index; that it equals S30's loot index for each item is **unproven** (verify by picking up a heal
  and reading the HUD count).
- **Class mods:** a bit per index in the player settings' `$modNames`. The dedi's `common.rpak` carries S30's own
  `settings/player/mp/pilot_survival_*.rpak` (`asset_pipeline/s30_dedi/inv/built_common.csv`), so the index space
  is S30's: 49 mods (`pilot_survival_tracker.json`), `enable_mantle_boost` = index 27. Dedi's 64-bit mask → element 0,
  element 1 = 0.
- **Weapon mods:** a bit per index in the weapon's `Mods` block. The dedi uses its `scripts/weapons/<w>.txt` order
  (the S3 cap is 32; server-only mods were trimmed). S30 uses its weapon settings order
  (`asset_pipeline/weapons_s30/weapon/<w>.txt`, the asset dump; up to 41 mods for e.g. the R-99). **The indices
  differ** (generated, `s30/tools/weapon_mod_remap.py`):
  - 353 dedi weapons with mods; 105 identical order, **197 need a remap**, 51 dedi-only weapons (`*_og`, copycat
    kits, instagib, raygun, …: S30 has no such weapon, so their mods cannot show).
  - **253 dedi mods land on S30 index ≥ 32**, so dwords 1 and 2 are needed. Example R-99: dedi 10 `legendary1` →
    S30 11; dedi 21 `laser_sight_l1` → S30 35; dedi 24 `paintball` → S30 40.
  - 188 dedi mods have no S30 name (dedi additions: `copycat_mod` ×79, `seer_heartbeat_sensor_active`,
    `pariah_ads_melee`, `barrel_stabilizer_l4_flash_hider` ×14, …) → dropped from the S30 mask.
  - Output: `src/game/shared/weapon_mod_remap_s30_table.h` (`kWeaponModRemap[]`: weapon name, dedi count, S30 index
    per dedi index, -1 = none; identity weapons are omitted), `analysis/weapon_mods/weapon_mod_remap.tsv`.
  - Regenerate whenever the dedi's weapon scripts change:
    `py s30/tools/weapon_mod_remap.py build/s3_server/platform/scripts/weapons asset_pipeline/weapons_s30/weapon src/game/shared/weapon_mod_remap_s30_table.h analysis/weapon_mods/weapon_mod_remap.tsv`
  - **Unproven:** that the S30 export lists mods in the asset's index order (it is a raw dump: `// val unk` field
    indices), and that the dedi engine indexes mods in its KV file order (the convention `loot_weapon_mods.py`
    relied on). Check in game: give the R-99 with `laser_sight_l1` and see the laser on the client.

## Wire compatibility (proven)

- The dedi's native Int64 `m_classModsActive` is decoded today by S30's **Int64** decoder on a dummy RecvProp of
  send type 7, and every later DT_Player prop stays aligned. So S30's Int64 decoder consumes exactly what the S3
  Int64 encoder writes.
- S30 type decoders (e.g. Int `0x252A30`) take a decode-info struct: value DVariant at **+32**, RecvProp at **+56**
  (proxy at RecvProp **+0x48**), `pStruct` at **+80**, `pOut` at **+88**; they call `proxy(info, pStruct, pOut)` when the
  RecvProp has one, else store the value at `pOut`.

## Where each adapter goes

| prop | place | why |
|---|---|---|
| weapon/projectile/loot mod masks | **dedi** (`dt_extend`, S30 client set) | the remap needs the entity's weapon, which the dedi has directly (`CWeaponX::m_weaponNameIndex` → WeaponNames string table); the wire then equals S30's (DataTable of 3 Int32) |
| `m_consumableInventory` | **dedi** | one change: element SendProp type Int → Int64 (64-bit proxy write); the sub-table and `[000N]` names already match S30 |
| overheat ×3 | **dedi** | the dedi already appends them (`EI/EF/ET` in `dt_extend.cpp` lines 117–119, Bx*_ValueProxy); emit them as DataTables of 3 (`[0000]` = value, `[0001]`, `[0002]` = 0), like the deathfield struct-of-arrays rebuild (`dt_extend.cpp` ~6472) |
| `m_classModsActive` | **client** adapter (or dedi Array, see below) | a top-level DT_Player/DT_PlayerDecoy prop; an adapter RecvProp of send type 7 with a proxy that writes `*(u64*)(pStruct+0x2660) = value; *(u64*)(pStruct+0x2668) = 0` needs no dedi SendPropArray building (which `dt_extend` has no precedent for, unproven) |

All dedi changes belong under the same launch switch as the activity set (`+bridge_client_set s30`, or a
sibling `bridge_client_dt_set s30`), so S21 clients keep today's S21 shapes.

## Implementation plan

### Dedi (`src/game/shared/dt_extend.cpp`, S30 set only)
1. **Mod masks.** For each SendProp listed above (DT_WeaponX ×4, DT_Projectile/DT_BaseGrenade/DT_CrossbowBolt/
   DT_Missile `m_modBitfield` + `m_overrideMods`, DT_PropSurvival `m_modBitField`, DT_TEProjectileTrail
   `m_modBitfield`): replace the Int SendProp with a DataTable SendProp of the same name. Its sub-SendTable is named
   after the prop and holds three SendPropInts `[0000]`, `[0001]`, `[0002]`: 32 bits, unsigned, all reading the
   original field offset. The value proxy for element k:
   - reads the dedi's 32-bit mask;
   - resolves the weapon name (WeaponX: `m_weaponNameIndex` through the dedi's WeaponNames string table; projectiles,
     loot and the trail TE: find their weapon-name field with dt_extend's SEND-TREE dump — **unproven which field**);
   - looks the weapon up in `kWeaponModRemap` (absent = identity);
   - for each set dedi bit d with `s30 = remap[d] >= 0`: sets bit `s30 % 32` of dword `s30 / 32`;
   - outputs dword k.
   Cache name → remap row per entity (weapon names do not change).
2. **Consumables.** In `ConsumableInventory_Widen…`: element `SP_TYPE` = 7 (Int64), bits/flags copied from the dedi's
   native Int64 SendProp `m_classModsActive` (the encoding S30 already decodes), and the proxy writes
   `*(uint64_t*)pOut = type | count << 16`.
3. **Overheat.** Replace the three appended scalars with DataTables of 3 (`[0000]` from the existing Bx proxies; 1, 2
   zero): bool as Int element (S30 element stride 1), Float, Time.

### Client (`src/core_s30/bridge_netchan_s30.cpp`, owned by the main session)
4. **Class mods adapter.** In `Netchan_FillDecoder`, before falling back to a dummy, look up
   (root table name, SendProp name) in an adapter list. For `DT_Player`/`DT_PlayerDecoy` + `m_classModsActive`, use
   an adapter RecvProp: type 7, proxy writing the two u64 at +0x2660/+0x2668 from `*(int64*)(info+32)` and
   `pStruct = *(void**)(info+80)`. Owner = the root table, as the dummies already do.
5. Keep the `[PROPS]` log; after the change these props must no longer be listed (dedi ones) or must show as
   adapted (client one).

### Verify in game
- R-99 with an optic/laser: the client draws the attachment; `GetWeaponMods` in client script matches the dedi.
- Pick up a medkit: the inventory/HUD count appears.
- Give `enable_mantle_boost` (docs/19): the client predicts the boost without correction.
- An overheating weapon (Rampage / Charge Rifle overheat): the overheat HUD fills.

## Status (2026-10-26, implemented)
- Client `src/core_s30/bridge_props_s30.cpp`: adapters for `m_classModsActive`, the three overheat values (every
  table that carries them) and the four DT_WeaponX mod masks, remapped by the weapon's class name (C_WeaponX +6368,
  the `GetWeaponClassName` native 0x74D7E0); masks whose weapon has no name yet are redone after the snapshot.
  Verified: an R-99 given with `laser_sight_l1 optic_cq_hcog_classic` shows both attachments on the S30 client.
- Dedi `dt_extend.cpp`: `m_consumableInventory` elements are Int64 with `+bridge_client_set s30` (they bind natively;
  the [PROPS] type mismatch is gone). The switch is read from the command line as well, since the SendTables are
  rewritten before command-line convars apply.
- Still on dummies: projectile, loot (`DT_PropSurvival`) and trail mod masks (their weapon-name source is unproven).
