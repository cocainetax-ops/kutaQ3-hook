#pragma once

// =============================================================================================== //
// kutaQ3 hook tests - a stand-in for Microsoft Detours, for the vmHook.cpp syntax check only
//
// Declares the six entry points vmHook.cpp calls, with the real signatures. There are no bodies:
// this exists so the detour attach/detach code in vmHook.cpp compiles and type-checks on a host
// that has no Detours library. The repository's own detours.h / detours.lib (x86, Detours 3.0) are
// what the DLL is really built against.
// =============================================================================================== //

#include <windows.h>

LONG DetourTransactionBegin(void);
LONG DetourTransactionAbort(void);
LONG DetourTransactionCommit(void);
LONG DetourUpdateThread(HANDLE thread);
LONG DetourAttach(PVOID* ppPointer, PVOID pDetour);
LONG DetourDetach(PVOID* ppPointer, PVOID pDetour);
