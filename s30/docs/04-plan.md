# 4. Minimum change set, risks, plan

## Minimum set of changes

**Client: a new `client_s30` product in this tree.** It builds `client_s30.dll` from the same sources with `GAME_S30`, so
the S21 `client` product keeps building unchanged.

1. Loader selects the product by host image (TimeDateStamp + SizeOfImage), not by exe name.
2. Boot layer: offline stubs for Origin/EA/EOS and Steam, a loopback-only network guard, and the integrity checks dealt with.
3. Core detours re-signed and verified: tier0/tier1, convar, cmd, filesystem, pak, host state.
4. Net bridge retargeted: the S3→S30 message map, NetObs symbols, CClientState/CNetChan offsets, and each FORMAT BRIDGE
   body checked against S30's readers.
5. Script layer: Flowstate client/UI scripts compile on S30. Loose-file loading takes priority over `script_*.rpak`;
   missing natives get shims, and script-side changes are made where shims cannot work.

**Server.** No change until B2 shows which S30 props the dedi must publish. Then a dt_extend profile for S30 clients,
selected by ConVar.

**Assets.** None for the first connection on `mp_rr_canyonlands_staging_mu1`, using the dedi's existing data. Then
S30 server-side data per target map: dedi BSP from the S30 BSP, models, anims, datatables, all through the
s30-port-kit dedi steps.

## Risks, ranked

| rank | risk | why it matters | first check |
|---|---|---|---|
| 1 | Flowstate client/UI scripts vs S30 natives and UI assets | 1,395 shared scripts; UI depends on S21 RUI/menu assets that S30 paks may not have | B3: dump the S30 native table, compile the client VM against the Flowstate tree, count errors |
| 2 | S30 message numbering and body formats | without the map, nothing after the challenge works; no RTTI | B1: runtime msgArray dump from inside S30 |
| 3 | Entity/recvprop drift | snapshot desyncs are bit-exact; one wrong prop desyncs everything after it | B2: dump S30 client classes and recvtables |
| 4 | Anti-tamper or integrity checks in the S30 exe | could block injection outright | experiment E0 below |
| 5 | Accidental contact with live EA/Steam services | project rule: local only | network guard in place before any boot |
| 6 | BSP mismatch between S30 maps and the dedi's S21-built data | brush model indices and static props must agree | E1: compare BSP lumps; rebuild dedi BSP from S30 |
| 7 | Size of the bridge (31k lines of S21-specific offsets) | slow, error-prone porting | verify every offset by disassembly (signature census + manual check) |

## First experiment (E0): boot probe

A minimal DLL, not the full SDK, injected into a **copy** of the S30 exe in `build\s30_client\`. The depot stays pristine.
The copy uses hardlinks plus `steam_appid.txt`, so Steam cannot restart into the live app.

The probe must:
1. log from DllMain (proves injection and that the image tolerates an added import);
2. hook `connect`/`WSAConnect`/`sendto`/`getaddrinfo`/WinHTTP/WinINet and refuse anything that is not loopback,
   logging every attempt;
3. run the signature resolver in-process and dump the netchannel message array once one exists;
4. log how far boot gets (window, `CEngine` state, main menu).

Pass criterion: the probe logs from inside S30, no non-loopback connection is made, and we learn whether S30
reaches the menu offline. This one run settles risk 4, prepares 5, and yields the data for B1.

## Phases and milestones

The test criteria are in `s30/status/milestones.json`, which also drives the status image.

| phase | milestones | done when |
|---|---|---|
| A Recon | A1–A4 | done (this document set) |
| B Compat | B1 netmsg map · B2 recvtables · B3 script natives · B4 asset versions · B5 connect trace · B6 integrity | each has a dump + diff in `analysis/` |
| C SDK | C1 loader · C2 offline + guard · C3 core detours · C4 main menu · C5 scripts compile · C6 bridge symbols verified | S30 client at the menu with the SDK console, scripts compiled |
| D Connect | D1 connect · D2 signon · D3 snapshots · D4 entities · D5 script remotes · D6 10-minute session · D7 rejoin | in game on firing range against the unmodified dedi |
| E Assets | E1 closure · E2 models · E3 anims · E4 BSP · E5 dedi paks | an S30-only map (e.g. mp_rr_olympus_mu3) hosted with zero missing-asset errors |
| F Integration | F1 launch · F2 S21 regression · F3 60-minute soak · F4 report | test report in `tests/` |
