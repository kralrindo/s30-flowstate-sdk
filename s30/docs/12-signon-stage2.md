# 12. Server connection, stage 2: the dedi hands the S30 engine its connection (2026-10-26)

Stage 1 (docs/09) left the dedi with the client connected and the S30 engine idle. Stage 2 moves the S30 engine into
the connection the way the S21 bridge does on S21: the engine runs its own native connect and builds its own
netchannel; the bridge only feeds it the replies it expects.

## How S21 does it (reference)

The S21 bridge rewrites the dedi's replies on the engine socket into the S21 engine's native connectionless type
`0x04` (`ffffffff 04 <map>\0 ...`). The S21 dispatcher (`0x3BE9F0`, case 4) runs `FullConnect` and starts the level
load; the bridge then drives `SetSignonState(2)` and replaces `CNetChan::ProcessPacket`.

## S30 facts (verified in the S30 IDB)

| | S30.1 | S21 |
|---|---|---|
| connectionless dispatcher | `0x3EF530` (cases 10, 11, 12, 13, 14, 22, 78) | `0x3BE9F0` (2, 4, 5, 6, 9, 23, 78) |
| "server map" reply | **case 12**: three 256-byte strings + 1 bit; `FullConnect`, `maps/%s%s.bsp`, level load | case 4 |
| `FullConnect` | `0x2FEDF0`: `NET_CreateNetChannel(cl+96 socket, adr, "ClientToServer", cl)` (`0x2A5430`), netchannel at `cl+104` | `cl+0x60` |
| client state fields | signon `+196`, server adr `+136` type / `+140` address / `+156` port / `+159` flag, level file `+412`, base `+476` | 32 bytes earlier |
| client state array | `0x1E1BD60`, stride `0x69378`, vtable `0x17F1828` | |
| `NET_ReceiveDatagram` | `0x2A5A90`: `recvfrom`, sender from `sockaddr_in6` into the netpacket (`+0` type 3, `+4` address, `+20` port) | |
| `NET_DecodeDatagram` | `0x2A5870(sock, packet, bEncrypted, buf, len, pDrop)`: decrypts a 28-byte header when `bEncrypted` | |
| `NET_ProcessSocket` | `0x2A5F80`: `bEncrypted = 1` on the client sockets unless a Valve relay runs with `valve_relay_encrypt_traffic 0`; a `-1` packet goes to `handler->vtable[1]` | |
| connectionless wrapper | `0x3F2FB0`: types other than 11, 13, 22, 78 reach the dispatcher directly only if `cl_strict_transport_checks`, `net_encrypt_dtls` or a flag is off; otherwise an undecrypted packet from a non-loopback sender is dropped (`dtls...violation`) | |

Case 12 needs signon 1 (the native connect sets it) and the sender equal to the server address. After a native
`connect 127.0.0.1:37015` the client state holds type 3, `::ffff:127.0.0.1`, port 37015, flag 0.

## The bridge's part (`src/core_s30/bridge_signon_s30.cpp`)

`bridge_connect host:port` now does both:

1. **The engine connects natively** to the same address: `net_usesocketsforloopback 1` (without it a `127.0.0.1`
   connect never touches a socket), `cl_strict_transport_checks 0` (read only by the wrapper above), then `connect`.
   Both ConVars are DEVELOPMENTONLY in S30.1; the bridge clears that flag on these two only, and restores the
   values on `bridge_disconnect`.
2. **Its datagrams to the dedi are dropped** in the `sendto` hook (encrypted S30 challenges, later netchannel
   traffic); the hook learns the engine's socket from them. The bridge's own sends are left alone.
3. **The S3 handshake** runs on the bridge socket as in stage 1.
4. **On CONNACCEPT** the bridge queues `ffffffff 0C <map>\0 \0 \0 00`; the `recvfrom` hook returns it on the engine's
   socket with the dedi as sender, and the `NET_DecodeDatagram` detour treats datagrams from the dedi as plaintext.

`sdk_clstate` prints client state 0 (signon, server address, netchannel, level) and the bridge's routing counters.

## Result (`tests/E6_signon`)

One `bridge_connect 127.0.0.1:37015` against the workspace dedi:

```
[SIGNON] engine socket to the dedi: 1600
[SIGNON] engine datagram to the dedi dropped (78 bytes, #1)        <- native challenge
[BRIDGE] CONNACCEPT: map 'mp_rr_canyonlands_staging_mu1' mode 'survival'
[SIGNON] delivered a 38-byte datagram to the engine socket
[SIGNON] datagram from the dedi taken as plaintext (sock 0, 38 bytes, #1)
client state 0: signon 2 netchan 000002B96E179FA0
  server adr type 3, ::ffff:127.0.0.1 port 37015, level 'maps/mp_rr_canyonlands_staging_mu1.bsp'
```

The client shows the Firing Range loading screen (`tests/E6_signon/connected.png`). The engine's netchannel now
sends to the dedi (1,228- and 256-byte datagrams, about 120 a second), dropped for now.

## Notes

- Without `net_usesocketsforloopback 1` the native connect stays on the in-process loopback path: no `sendto`, no
  socket for the bridge to answer on.
- Without `cl_strict_transport_checks 0` the type-12 reply is decoded but dropped by the wrapper; signon stays 1
  and the engine keeps resending its challenge.
- The workspace dedi crashes (call to address 0) seconds after `ready` when started with `+launchplaylist
  survival_firingrange`; started like the user's `launch_server.bat` with `+map mp_rr_canyonlands_staging_mu1` it
  stays up. Not yet investigated.

## Next
The netchannel: the dedi's S3 packets (now counted on the bridge socket) go to the engine socket through the same
`recvfrom` route, and `CNetChan::ProcessPacket` is replaced by S3 header parsing plus dispatch into S30's message
array with the docs/11 type map. The engine's own netchannel output is translated to S3 on the C2S side.
