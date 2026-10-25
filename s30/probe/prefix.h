// Force-included into every probe TU: what the SDK's precompiled header gives
// the in-tree Detours sources (they do not include these themselves).
#pragma once
#include <winsock2.h>
#include <windows.h>
typedef LONG NTSTATUS; // tebpeb64.h needs it; winternl.h would clash with its PEB/TEB
#include <limits.h>
#include <intrin.h>
