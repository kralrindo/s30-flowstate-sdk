# 7. SDK console, engine log and script print capture (2026-10-25)

## Engine output: where S30 prints

| | S30.1 RVA | role |
|---|---|---|
| `CoreMsg(type, fmt, ...)` | 0x269B40 | variadic engine print, **241 references** |
| `CoreMsgV(type, fmt, args)` | 0xD39C30 | dispatcher: types 0–2 → `LoggingSystem_LogV`; 3/4 formatted then dropped; 5 = fatal `Error()`; 9 ignored |
| `LoggingSystem_LogV(ch, sev, fmt, args)` | 0x5C7680 | drops anything below the channel's minimum severity (channel record +16) |
| `CLoggingSystem::LogDirect(sys, ch, sev, color, text)` | 0x5C6C40 | sinks: stdout (`+0xFE49`, `-logtostdout`), `-logtofile` handle (`+0xFE40`), `OutputDebugStringA` (`+0xFE38`, `-logtodebugoutput`), then every listener; sev 3 exits |

The logging-system object is `sub_1405C6AB0()`. `*(int*)sys` is the channel count, and channels are 56-byte
`LoggingChannel_t` records from `+8` (name at `+24 + 56*ch`). LauncherMain still parses `-logtofile`,
`-logflush`, `-logtostdout` and `-logtodebugoutput`.

**Retail emits almost nothing.** A 45 s boot to the title screen produced one line, a type-3 developer message
(`Couldn't open ... psoCache.pso`) that retail would drop anyway. `echo` exists as a ConCommand, but its handler is
`return 0` in retail.

client_s30 hooks `CoreMsgV`, which sees the text before any filtering, and `LogDirect`, for code that logs to the
LoggingSystem directly. A thread-local flag stops double logging. Lines go to the log as `[ENGINE][t<type>]` and to the
console, coloured by severity.

## Script output

| | S30.1 | S21 DX12 |
|---|---|---|
| print callback, SERVER/CLIENT VMs | 0xF5B1D0 | 0xE286C0 (one callback) |
| print callback, UI VM | 0xF5B2B0 | — |
| VM setup that installs them (`"PrintFunc"`, `"PrintFunc_Client"`, `"PrintFunc_UI"`) | 0xF575F0 | 0xE24780 |
| per-VM state table (print pointer at `+17016`) | 0x40F48C8 | — |
| `printl` native (base library table, `.data` 0x277A430) | 0xF33D90 | |

Retail print callbacks only format into a per-VM capture buffer. client_s30 detours both, logs `[SCRIPT][VM]` lines,
and forwards `"%s"`. The two callbacks share their first 0x6D bytes, so each signature extends into the body.
The S21 signature misses on S30 because S21 has `xor` where S30 has `mov r9, rbx`.

**Verified at runtime:** the table resolved from the UI callback's own `lea` equals 0x40F48C8. At the title screen only
the UI VM exists, and its print pointer is the hooked 0xF5B2B0. In 40 s the UI scripts called `printl` **0 times**
(counted on the native itself), so nothing prints in this state. Retail has no `script`/`script_ui` command to force a
print, so the capture fires once scripts that print run (Flowstate's, later).

## Script errors (2026-10-26)

| | S30.1 | role |
|---|---|---|
| `sqstd_aux_printerror` | 0xF61690 | VM error handler: writes `SCRIPT ERROR: [VM] msg`, ` -> ` source line, `CALLSTACK`, `*FUNCTION [...]` lines, `LOCALS` in pieces through `*(table[vm] + 17024)`, then raises the error dialog (0xF5E230, `"%s #%d\n%s"`) |
| error text callback, SERVER/CLIENT VMs | 0xF5B3B0 | `(vm, text)`: appends to the per-VM capture buffer (`0x147765560 + 0x2002 * vm`) only; installed at `+17024` by the VM setup (0xF575F0) |
| error text callback, UI VM | 0xF5B420 | same, UI VM |

Retail never logs this text; the dialog shows only the file, line and message. S21 sends the same text through its
print callback (the SDK flags it as an error). client_s30 detours both S30 error callbacks, buffers the pieces per VM
and logs whole lines as `[SCRIPT-ERROR][VM]` (console: fault colour), then forwards the text unchanged. Verified with
the pdef mismatch: the full callstack (`BuildStatTemplateEnumKeyResolversRecursive` ... `ClientCodeCallback_MapSpawn`)
and locals are in the log. The ` -> ` source line is empty (no script source on the client).

## Console

`console_s30` opens a "R5F S30 Client" console window (skip it with `-s30_noconsole`). Engine, script and SDK lines
appear in different colours; network-guard lines are rate-limited per destination (first 3, then every 500th).
Input uses `ReadConsoleA` → `Cbuf_AddText(0, line, kCommandSrcCode)`, as the S21 client does.

`Cbuf_AddText` is S30.1 0x248760 (S21 DX12 0x223D90): identical body, same lock-protected buffer, same 9872-byte
per-target stride. Found with the S21 SDK signature, which is unique on S30.

**Verified end to end** with `-s30_selftest`: `quit` sent through `Cbuf_AddText` shut the engine down cleanly
(LauncherMain returned into WinMain at 0x589FC6, then `ExitProcess(0)`), where every other run stayed up until stopped.
The console window was confirmed open and visible.

Logs: `tests\E3_console\`.

## Next
- Retail strips most command bodies (`echo`), so useful console commands are SDK-provided. Port the SDK
  ConCommand/ConVar layer once the S30 ConVar ABI is checked (ICommandLine already lost a vtable slot).
- An ImGui overlay console (the S21 SDK has one) needs D3D12 present hooks on S30; later.
