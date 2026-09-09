#include "main.h"        // Log(), Detours, <Windows.h>
#include "vmHook.h"
#include "nameEsp.h"

#include <stdio.h>
#include <string.h>

// =============================================================================================== //
// kutaQ3 hook - the cgame VM hook (see vmHook.h for the overview)
//
// Everything the ESP reads is captured here, on the game thread, inside the cgame's own trap call.
// Nothing in this file calls back into the engine, which is what makes it work for a bytecode
// cgame: the engine only resolves VM pointers while the cgame VM is current (VM_ArgPtr), and this
// never asks it to.
// =============================================================================================== //

namespace
{
	// The engine declares it as `int (*systemCall)( int *parms )` (vm_local.h); on x86 that is a
	// plain __cdecl function pointer, and it is what CL_CgameSystemCalls is compiled as.
	typedef int (Q3SDK_CDECL *systemCall_t)(int* args);

	const VmFind::Record*  s_vm             = NULL;
	bool                   s_native         = false;   // dllHandle != 0: a DLL cgame, not bytecode
	bool                   s_attached       = false;
	systemCall_t           s_origSystemCall = NULL;

	// ---- what the traps handed over ------------------------------------------------------------
	q3::snapshot_t         s_snapshot;
	q3::refdef_t           s_refdef;
	const q3::gameState_t* s_gameState      = NULL;   // cgs.gameState, inside the cgame's memory
	bool                   s_haveSnapshot   = false;
	bool                   s_haveRefdef     = false;
	int                    s_snapshotNumber = 0;
	int                    s_snapshotTime   = 0;      // cl.snap.serverTime

	uint32_t               s_lastDataBase   = 0;      // VM identity: changes on every level load
	uint32_t               s_lastDllHandle  = 0;
	uintptr_t              s_codeLow        = 0;      // main module range, for the systemCall check
	uintptr_t              s_codeHigh       = 0;
	DWORD                  s_lastPollMs     = 0;
	char                   s_status[192]    = "no cgame VM";

	void SetStatus(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		vsprintf_s(s_status, fmt, ap);
		va_end(ap);
	}

	// -------------------------------------------------------------------------------------------
	// memory walking
	// -------------------------------------------------------------------------------------------

	bool ImageRange(HMODULE module, uintptr_t& low, uintptr_t& high)
	{
		if (!module)
			return false;
		const BYTE* base = (const BYTE*)module;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		low  = (uintptr_t)base;
		high = low + nt->OptionalHeader.SizeOfImage;
		return high > low;
	}

	// Visit every committed, writable, readable region in [low, high). vmTable lives in the main
	// module's .data/.bss and a bytecode cgame's data segment is a Hunk_Alloc block, so those are
	// the only places worth reading - and the only ones it is safe to touch.
	template <typename Visitor>
	void ForEachWritableRegion(uintptr_t low, uintptr_t high, Visitor visit)
	{
		MEMORY_BASIC_INFORMATION info;
		uintptr_t address = low;
		while (address < high)
		{
			if (!VirtualQuery((LPCVOID)address, &info, sizeof(info)))
				return;

			const uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize;
			const bool writable = (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
			                                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
			const bool guarded  = (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0;

			// Clip to the range we were asked for. A region is whatever VirtualQuery reports, which
			// for a hunk-backed VM data segment is the engine's whole hunk block - scanning past
			// the segment would read other levels' leftovers.
			const uintptr_t useStart = ((uintptr_t)info.BaseAddress > low) ? (uintptr_t)info.BaseAddress : low;
			const uintptr_t useEnd   = (end < high) ? end : high;

			if (info.State == MEM_COMMIT && writable && !guarded && useEnd > useStart)
				visit((const void*)useStart, (size_t)(useEnd - useStart));

			if (end <= address)
				return;
			address = end;
		}
	}

	bool FindCgameVm()
	{
		uintptr_t low = 0, high = 0;
		if (!ImageRange(GetModuleHandle(NULL), low, high))
			return false;

		s_codeLow  = low;
		s_codeHigh = high;

		bool found = false;
		ForEachWritableRegion(low, high, [&](const void* region, size_t size)
		{
			if (found)
				return;
			VmFind::Found hit;
			if (VmFind::FindVm(region, size, s_codeLow, s_codeHigh, hit))
			{
				s_vm     = hit.record;
				s_native = hit.native;
				found    = true;
				Log("[kutaQ3] cgame VM at %p (%s, systemCall %p, data %p mask 0x%x)",
				    (const void*)s_vm, s_native ? "native DLL" : "bytecode",
				    (void*)(uintptr_t)s_vm->systemCall, (void*)(uintptr_t)s_vm->dataBase,
				    (unsigned)s_vm->dataMask);
			}
		});
		return found;
	}

	// The cgame keeps its own copy of the engine's gameState (cgs.gameState). The trap that hands
	// the address over only fires in CG_Init, so when the DLL is injected into a map that is
	// already running it is located by shape instead - see vmFind.h.
	bool ScanForGameState()
	{
		if (!s_vm)
			return false;

		bool found = false;
		uintptr_t low = 0, high = 0;
		if (!s_native && s_vm->dataBase && s_vm->dataMask > 0)
		{
			low  = (uintptr_t)s_vm->dataBase;
			high = low + (uintptr_t)s_vm->dataMask + 1;
		}
		else if (s_native && !ImageRange((HMODULE)(uintptr_t)s_vm->dllHandle, low, high))
		{
			return false;
		}

		ForEachWritableRegion(low, high, [&](const void* region, size_t size)
		{
			if (found)
				return;
			const q3::gameState_t* gs = NULL;
			if (VmFind::FindGameState(region, size, &gs))
			{
				s_gameState = gs;
				found = true;
				Log("[kutaQ3] cgame gameState at %p (found by scanning %u bytes)",
				    (const void*)gs, (unsigned)size);
			}
		});
		return found;
	}

	// -------------------------------------------------------------------------------------------
	// captured state
	// -------------------------------------------------------------------------------------------

	void DropCaptured()
	{
		memset(&s_snapshot, 0, sizeof(s_snapshot));
		memset(&s_refdef, 0, sizeof(s_refdef));
		s_gameState      = NULL;
		s_haveSnapshot   = false;
		s_haveRefdef     = false;
		s_snapshotNumber = 0;
		s_snapshotTime   = 0;
		NameEsp::Reset();
	}

	// VM_ArgPtr() (vm.c), spelled out: a native VM passes real host pointers and has dataBase 0, a
	// bytecode VM passes offsets into its hunk segment and has them masked.
	uintptr_t Resolve(int vmAddress)
	{
		if (!s_vm || vmAddress == 0)
			return 0;
		if (s_vm->entryPoint != 0)
			return (uintptr_t)s_vm->dataBase + (uintptr_t)vmAddress;
		return (uintptr_t)s_vm->dataBase + (uintptr_t)(vmAddress & s_vm->dataMask);
	}

	// -------------------------------------------------------------------------------------------
	// the detour: watch the cgame's traps go past and keep what the ESP needs
	// -------------------------------------------------------------------------------------------

	void Observe(const int* args, int result)
	{
		if (!args)
			return;

		switch (args[0])
		{
		case q3::CG_GETSNAPSHOT:
			// CL_GetSnapshot() only writes the destination when it returns true, so a failed read
			// leaves the previous sample in place - which is what the ESP wants anyway.
			if (result)
			{
				const uintptr_t dest = Resolve(args[2]);
				if (dest)
				{
					memcpy(&s_snapshot, (const void*)dest, sizeof(s_snapshot));
					s_snapshotNumber = args[1];
					s_snapshotTime   = s_snapshot.serverTime;
					s_haveSnapshot   = true;
				}
			}
			break;

		case q3::CG_GETGAMESTATE:
		{
			// fired once per level, from CG_Init. The copy is read live rather than kept here so
			// that configstring changes show up without a re-scan.
			const uintptr_t dest = Resolve(args[1]);
			if (dest)
				s_gameState = (const q3::gameState_t*)dest;
			break;
		}

		case q3::CG_GETCURRENTSNAPSHOTNUMBER:
		{
			// CL_GetCurrentSnapshotNumber() writes cl.snap.messageNum / serverTime
			const uintptr_t number = Resolve(args[1]);
			const uintptr_t time   = Resolve(args[2]);
			if (number && time)
			{
				s_snapshotNumber = *(const int*)number;
				s_snapshotTime   = *(const int*)time;
			}
			break;
		}

		case q3::CG_R_RENDERSCENE:
		{
			// once per rendered frame, with the view the cgame just built
			const uintptr_t src = Resolve(args[1]);
			if (src)
			{
				memcpy(&s_refdef, (const void*)src, sizeof(s_refdef));
				s_haveRefdef = true;
			}
			break;
		}

		case q3::CG_CM_LOADMAP:
			// CG_Init loads the collision map right after trap_GetGameState: the hunk has already
			// been cleared, so everything captured so far belongs to the previous level.
			DropCaptured();
			break;

		default:
			break;
		}
	}

	int Q3SDK_CDECL newSystemCall(int* args)
	{
		// The engine has finished writing into the cgame's buffers by the time the original
		// returns, so the copy happens afterwards. Re-entrancy is impossible: everything below
		// touches only this DLL's own statics.
		const int result = s_origSystemCall ? s_origSystemCall(args) : 0;
		Observe(args, result);
		return result;
	}

	// -------------------------------------------------------------------------------------------
	// the trampoline NameEsp::Gather() reads through. Same trap numbers, same return values, but
	// answered from the copies above and taking plain host pointers - so it works from anywhere in
	// the frame, not just from inside a VM call.
	// -------------------------------------------------------------------------------------------

	int Q3SDK_CDECL bridgeSyscall(int arg, ...)
	{
		va_list ap;
		va_start(ap, arg);
		intptr_t a[4];
		for (int i = 0; i < 4; ++i)
			a[i] = va_arg(ap, intptr_t);
		va_end(ap);

		switch (arg)
		{
		case q3::CG_GETCURRENTSNAPSHOTNUMBER:
			if (!a[0] || !a[1])
				return 0;
			*(int*)a[0] = s_snapshotNumber;
			*(int*)a[1] = s_snapshotTime;
			return 0;

		case q3::CG_GETSNAPSHOT:
			if (!s_haveSnapshot || !a[1])
				return 0;                               // not connected / nothing valid yet
			memcpy((void*)a[1], &s_snapshot, sizeof(s_snapshot));
			return 1;

		case q3::CG_GETGAMESTATE:
			if (!s_gameState || !VmFind::GameStateLooksLive(s_gameState) || !a[0])
				return 0;
			memcpy((void*)a[0], s_gameState, sizeof(q3::gameState_t));
			return 0;

		default:
			// CG_GETUSERCMD / CG_GETCURRENTCMDNUMBER / CG_CVAR_VARIABLESTRINGBUFFER need a VM
			// address to write into, so they are not served. Gather() takes the view from the
			// captured refdef instead (nameEsp.h).
			return 0;
		}
	}

	// -------------------------------------------------------------------------------------------

	bool AttachSystemCall()
	{
		if (s_attached || !s_vm || !s_vm->systemCall)
			return s_attached;

		systemCall_t target = (systemCall_t)(uintptr_t)s_vm->systemCall;

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		if (DetourAttach(&(PVOID&)target, (PVOID)newSystemCall) != NO_ERROR)
		{
			DetourTransactionAbort();
			SetStatus("hook failed (DetourAttach refused %p)", (void*)target);
			Log("[kutaQ3] DetourAttach on the cgame syscall dispatcher %p failed", (void*)target);
			return false;
		}
		if (DetourTransactionCommit() != NO_ERROR)
		{
			DetourTransactionAbort();
			SetStatus("hook failed (DetourTransactionCommit)");
			return false;
		}

		s_origSystemCall = target;
		s_attached       = true;
		Log("[kutaQ3] hooked the cgame syscall dispatcher %p", (void*)target);
		return true;
	}

	const char* VmKind()
	{
		if (!s_vm)
			return "no VM";
		if (s_native)
			return "native DLL";
		return s_vm->compiled ? "bytecode (JIT)" : "bytecode (interpreted)";
	}

	void UpdateStatus()
	{
		if (!s_vm)
		{
			// no record to read. If the dispatcher is still hooked, the engine had one a moment ago
			// (VM_Free on disconnect / map change) and the next VM_Create() puts the same function
			// pointer back - which is worth saying, because nothing has to be re-attached.
			SetStatus(s_attached ? "cgame VM gone - waiting for the next level"
			                     : "no cgame VM - not in a game?");
			return;
		}
		if (!s_attached)
			return;                                 // AttachSystemCall() already reported why

		char kind[96];
		strncpy_s(kind, VmKind(), _TRUNCATE);
		if (s_native && q3::IsNativeCgameModule(s_vm->fqpath))
		{
			const char* path = s_vm->fqpath;
			for (const char* p = path; *p; ++p)
			{
				if (*p == '\\' || *p == '/' || *p == ':')
					path = p + 1;
			}
			SetStatus("hooked, native cgame %s", path);
			return;
		}

		if (!s_haveSnapshot)
			SetStatus("hooked, %s - waiting for the first snapshot", kind);
		else if (!s_gameState)
			SetStatus("hooked, %s - configstrings not found yet", kind);
		else
			SetStatus("hooked, %s", kind);
	}
}

// =============================================================================================== //

bool Vm::Install()
{
	if (s_vm && s_attached)
		return true;

	if (!s_vm)
		FindCgameVm();
	if (!s_vm)
	{
		UpdateStatus();
		return false;
	}

	if (AttachSystemCall())
	{
		s_lastDataBase  = s_vm->dataBase;
		s_lastDllHandle = s_vm->dllHandle;
		if (!s_gameState)
			ScanForGameState();
	}
	UpdateStatus();
	return s_attached;
}

bool Vm::Poll()
{
	const DWORD now = timeGetTime();
	if (now - s_lastPollMs < 500)
		return s_vm != NULL;
	s_lastPollMs = now;

	// VM_Free() memsets the record (vm.c), so a cgame that shut down simply stops matching. The
	// detour stays where it is: systemCall is CL_CgameSystemCalls in quake3.exe, which cannot be
	// unmapped, and the next VM_Create("cgame") puts that very same function pointer back in the
	// record - so nothing has to be re-attached, and there is no trampoline in a DLL that could
	// have been freed underneath us.
	if (s_vm && !VmFind::IsCgameVm(s_vm, sizeof(VmFind::Record), s_codeLow, s_codeHigh, NULL))
	{
		Log("[kutaQ3] cgame VM record at %p is gone (VM_Free)", (const void*)s_vm);
		DropCaptured();
		s_vm = NULL;
		s_native = false;
	}

	if (!s_vm && FindCgameVm())
		AttachSystemCall();

	if (s_vm)
	{
		// a new VM instance means a new hunk segment (or a new cgame DLL): every pointer captured
		// from the old one is stale
		if (s_vm->dataBase != s_lastDataBase || s_vm->dllHandle != s_lastDllHandle)
		{
			Log("[kutaQ3] cgame VM reloaded (data %p -> %p)",
			    (void*)(uintptr_t)s_lastDataBase, (void*)(uintptr_t)s_vm->dataBase);
			DropCaptured();
			s_lastDataBase  = s_vm->dataBase;
			s_lastDllHandle = s_vm->dllHandle;
		}

		// late inject, or a level the configstrings were never seen for
		if (!s_gameState || !VmFind::GameStateLooksLive(s_gameState))
		{
			s_gameState = NULL;
			ScanForGameState();
		}
	}

	UpdateStatus();
	return s_vm != NULL;
}

void Vm::Shutdown()
{
	DropCaptured();
	s_vm = NULL;
	s_native = false;

	if (!s_attached)
		return;

	// CL_CgameSystemCalls lives in the executable, so the bytes Detours stole are still there to
	// put back - no "is the module still resident" dance.
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourDetach(&(PVOID&)s_origSystemCall, (PVOID)newSystemCall);
	DetourTransactionCommit();

	Log("[kutaQ3] cgame syscall dispatcher detached");
	s_attached       = false;
	s_origSystemCall = NULL;
}

const char* Vm::Status()
{
	return s_status;
}

q3::syscall_t Vm::Syscall()
{
	return s_vm ? bridgeSyscall : NULL;
}

const q3::refdef_t* Vm::Refdef()
{
	return s_haveRefdef ? &s_refdef : NULL;
}

int Vm::ServerTime()
{
	if (s_haveRefdef && s_refdef.time != 0)
		return s_refdef.time;                  // cg.time, i.e. the serverTime this frame renders
	return s_snapshotTime;
}

bool Vm::Ready()
{
	return s_haveSnapshot && s_gameState != NULL && VmFind::GameStateLooksLive(s_gameState);
}

void Vm::Reset()
{
	DropCaptured();
}
