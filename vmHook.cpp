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

	// The fallback view (nameEsp.h) without a captured refdef: newest usercmd + cg_fov, the way
	// PM_UpdateViewAngles() / CG_DrawActiveFrame() read them. Captured from the cgame's own
	// reads so the bridge can serve them back to Gather().
	q3::usercmd_t          s_userCmd;
	bool                   s_haveUserCmd    = false;
	int                    s_cmdNumber      = 0;      // cl.cmdNumber, from CG_GETCURRENTCMDNUMBER
	char                   s_fov[32]        = { 0 };  // cg_fov value, from CG_CVAR_VARIABLESTRINGBUFFER
	bool                   s_haveFov        = false;

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
		memset(&s_userCmd, 0, sizeof(s_userCmd));
		s_gameState      = NULL;
		s_haveSnapshot   = false;
		s_haveRefdef     = false;
		s_haveUserCmd    = false;
		s_snapshotNumber = 0;
		s_snapshotTime   = 0;
		s_cmdNumber      = 0;
		// s_fov / s_haveFov survive: cg_fov is a client cvar, not per-level state, so the last
		// captured value is still what the cgame would read after the reload.
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
			// CG_Init fetches the whole gamestate once - and CG_ConfigStringModified() fetches it
			// again on EVERY "cs" server command (cg_servercmds.c), i.e. whenever any configstring
			// changes. Either way the address is &cgs.gameState; the copy is read live rather than
			// kept here so that configstring changes show up without a re-scan.
			const uintptr_t dest = Resolve(args[1]);
			if (dest)
				s_gameState = (const q3::gameState_t*)dest;
			break;
		}

		case q3::CG_CVAR_VARIABLESTRINGBUFFER:
		{
			// trap_Cvar_VariableStringBuffer(name, buffer, bufsize). The cgame reads a handful of
			// cvars per frame; only cg_fov is kept - Gather() rebuilds the view's fov_x from it
			// when there is no captured refdef to take it from.
			const uintptr_t nameAddr = Resolve(args[1]);
			const uintptr_t bufAddr  = Resolve(args[2]);
			const int bufsize = args[3];
			if (nameAddr && bufAddr && bufsize > 0)
			{
				const char* name = (const char*)nameAddr;
				if (strncmp(name, "cg_fov", 7) == 0)   // bounded: 6 chars + NUL
				{
					const char* val = (const char*)bufAddr;
					size_t n = 0;
					while (n + 1 < sizeof(s_fov) && n + 1 < (size_t)bufsize && val[n])
					{
						s_fov[n] = val[n];
						++n;
					}
					s_fov[n] = 0;
					s_haveFov = true;
				}
			}
			break;
		}

		case q3::CG_GETCURRENTCMDNUMBER:
			// trap_GetCurrentCmdNumber() takes no arguments; the command number is the return value.
			s_cmdNumber = result;
			break;

		case q3::CG_GETUSERCMD:
			// trap_GetUserCmd(cmdNumber, ucmd). CL_GetUserCmd() only writes the destination when
			// it returns true. Prediction asks for a range ending at the current command, so the
			// last sample taken is usually the current one - which is what Gather() asks for.
			if (result)
			{
				const uintptr_t dest = Resolve(args[2]);
				if (dest)
				{
					memcpy(&s_userCmd, (const void*)dest, sizeof(s_userCmd));
					s_haveUserCmd = true;
				}
			}
			break;

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
			// Active-play HUD models render after the world with their own camera.
			// Never let those RDF_NOWORLDMODEL scenes replace the world view.
			const uintptr_t src = Resolve(args[1]);
			if (src)
			{
				q3::refdef_t candidate;
				memcpy(&candidate, (const void*)src, sizeof(candidate));
				if (NameEsp::CaptureWorldRefdef(candidate, s_refdef))
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
		// Each case reads exactly the varargs its trap takes. Reading a fixed 4 would walk off
		// the caller's argument list for traps that take fewer (CG_GETCURRENTCMDNUMBER takes
		// none) - undefined behaviour that happens to read stack garbage on x86. va_end without
		// consuming a trailing argument Gather() passes but the real trap does not take
		// (CG_GETSNAPSHOT / CG_GETUSERCMD's sizeof) is fine and simply ignores it.
		va_list ap;
		va_start(ap, arg);

		switch (arg)
		{
		case q3::CG_GETCURRENTSNAPSHOTNUMBER:
		{
			const intptr_t pNumber = va_arg(ap, intptr_t);
			const intptr_t pTime   = va_arg(ap, intptr_t);
			va_end(ap);
			if (!pNumber || !pTime)
				return 0;
			*(int*)pNumber = s_snapshotNumber;
			*(int*)pTime   = s_snapshotTime;
			return 0;
		}

		case q3::CG_GETSNAPSHOT:
		{
			va_arg(ap, intptr_t);                       // snapshot number - the latest is served
			const intptr_t dest = va_arg(ap, intptr_t);
			va_end(ap);
			if (!s_haveSnapshot || !dest)
				return 0;                               // not connected / nothing valid yet
			memcpy((void*)dest, &s_snapshot, sizeof(s_snapshot));
			return 1;
		}

		case q3::CG_GETGAMESTATE:
		{
			const intptr_t dest = va_arg(ap, intptr_t);
			va_end(ap);
			if (!s_gameState || !VmFind::GameStateLooksLive(s_gameState) || !dest)
				return 0;
			memcpy((void*)dest, s_gameState, sizeof(q3::gameState_t));
			return 0;
		}

		case q3::CG_GETCURRENTCMDNUMBER:
		{
			va_end(ap);                                 // no arguments; the number is returned
			return s_cmdNumber;
		}

		case q3::CG_GETUSERCMD:
		{
			va_arg(ap, intptr_t);                       // requested number - the latest sample is served
			const intptr_t dest = va_arg(ap, intptr_t);
			va_end(ap);
			if (!s_haveUserCmd || !dest)
				return 0;
			memcpy((void*)dest, &s_userCmd, sizeof(s_userCmd));
			return 1;
		}

		case q3::CG_CVAR_VARIABLESTRINGBUFFER:
		{
			const intptr_t pName = va_arg(ap, intptr_t);
			const intptr_t pBuf  = va_arg(ap, intptr_t);
			const intptr_t size  = va_arg(ap, intptr_t);
			va_end(ap);
			if (!pName || !pBuf || size <= 0)
				return 0;
			char* buf = (char*)pBuf;
			buf[0] = 0;
			if (strcmp((const char*)pName, "cg_fov") != 0)
				return 0;                               // only cg_fov is captured; the rest read empty
			if (!s_haveFov)
				return 0;                               // not seen yet - Gather() falls back to 90
			size_t n = 0;
			while (n + 1 < (size_t)size && s_fov[n])
			{
				buf[n] = s_fov[n];
				++n;
			}
			buf[n] = 0;
			return 0;
		}

		default:
			va_end(ap);
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
	// A refdef older than the newest snapshot is a frozen camera (R_RenderScene stopped
	// arriving while snapshots kept flowing); BuildView rejects it for the view, and the frame
	// time must not come from it either or every snapshot age computed from it goes negative.
	// Small negatives are legitimate - the cgame reads the next snapshot ahead for
	// interpolation - so only staleness past NameEsp::kRefdefStaleMs falls back.
	if (s_haveRefdef && s_refdef.time != 0 &&
	    s_snapshotTime - s_refdef.time <= NameEsp::kRefdefStaleMs)
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
