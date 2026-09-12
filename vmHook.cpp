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
	// The cgame renders remote players by interpolating between the previous and the newest
	// server snapshots, so a single newest copy is not enough: a small ring of the snapshots the
	// cgame recently requested lets Gather() read both endpoints (keyed by the engine's message
	// number). 6 slots cover ~300 ms of server frames at the default sv_fps of 20.
	const int kSnapRingSize = 6;
	struct SnapSlot
	{
		bool           have;
		int            number;       // cl.snap.messageNum this was requested as
		q3::snapshot_t snap;
	};
	SnapSlot               s_snaps[kSnapRingSize];

	// How often a copy found by SCANNING is re-checked against the full gameState shape. The trap
	// capture is the cgame's own address and needs no re-check; a scan is a guess, so it is
	// verified on a timer (the check walks every configstring) instead of every frame.
	const int kShapeCheckMs = 5000;

	q3::refdef_t           s_refdef;
	const q3::gameState_t* s_gameState      = NULL;   // cgs.gameState, inside the cgame's memory

	// Where that pointer came from. GS_TRAP is authoritative - the cgame handing over the address
	// of its own cgs.gameState - and the two scans are guesses, ranked by how well they can be
	// trusted afterwards:
	//
	//   - GS_ENGINE_SCAN finds the engine's OWN copy, clientActive_t::gameState in quake3.exe's
	//     data. CL_ParseGamestate() fills it before the cgame VM is even created, and
	//     CL_GetGameState() is nothing but `*gs = cl.gameState` (cl_cgame.c) - so its content is
	//     byte-for-byte what the trap would have served, it just arrives without any trap. This is
	//     what closes the fresh-join gap: the CG_INIT trap is always missed when the DLL was
	//     loaded before connecting (see ScanEngineForGameState), and on stock 1.32 that trap only
	//     re-fires when a configstring changes at runtime - on a quiet FFA server, the first score
	//     event of the match. Which is, literally, your first death: the suicide or the bot's frag
	//     updates CS_SCORES1/2, the server sends "cs", CG_ConfigStringModified() re-fetches the
	//     whole gamestate through the trap, and THAT was the moment names came back. The engine
	//     copy is available from CL_ParseGamestate() instead - no death required.
	//   - GS_CGAME_SCAN finds cgs.gameState inside the cgame VM's own data segment (the old
	//     fallback, kept for the native-cgame case where the trap also hands over a cgame
	//     address).
	//
	// The trap capture upgrades either scan the moment it fires, and a scan never replaces a trap
	// capture (Poll() only scans while the pointer is missing or plainly dead).
	enum GsOrigin
	{
		GsTrap = 0,        // CG_GETGAMESTATE observed: the cgame handed over &cgs.gameState
		GsEngineScan,      // shape scan of quake3.exe's data: the engine's cl.gameState
		GsCgameScan        // shape scan of the cgame VM's segment: cgs.gameState
	};

	// The VM instance a TRAP or CGAME_SCAN pointer was read out of, recorded next to the pointer.
	// The pointer is to a global inside the cgame, so it stays valid across level changes as long
	// as the instance that owns it is still the live one (VmFind::SameVmInstance) - it is only
	// stale when the engine loaded a different data segment or a different cgame module. An
	// ENGINE_SCAN pointer belongs to quake3.exe itself, not to any cgame VM instance: the engine
	// keeps cl.gameState across VM_Free / VM_Create, so the instance check must not judge it -
	// only the shape checks below do. s_gsOrigin also feeds the status line.
	uint32_t               s_gsDataBase     = 0;
	uint32_t               s_gsDllHandle    = 0;
	GsOrigin               s_gsOrigin       = GsTrap;
	DWORD                  s_gsStampMs      = 0;      // when the pointer was (re)acquired
	DWORD                  s_lastShapeCheckMs = 0;    // last full shape re-check of that pointer
	bool                   s_haveSnapshot   = false;
	bool                   s_haveRefdef     = false;
	int                    s_snapshotNumber = 0;
	int                    s_snapshotTime   = 0;      // newest snap: cl.snap.serverTime

	// Exact-number lookup for the bridge (Gather() asks for the newest and the previous one), else
	// NULL.
	SnapSlot* FindSnapSlot(int number)
	{
		for (int i = 0; i < kSnapRingSize; ++i)
			if (s_snaps[i].have && s_snaps[i].number == number)
				return &s_snaps[i];
		return NULL;
	}

	// Slot to store a freshly delivered snapshot in: the slot already holding that message number
	// (a re-request overwrites in place), else an empty one, else the oldest slot (lowest number).
	SnapSlot* StoreSnapSlot(int number)
	{
		SnapSlot* empty = NULL;
		SnapSlot* oldest = NULL;
		for (int i = 0; i < kSnapRingSize; ++i)
		{
			SnapSlot* slot = &s_snaps[i];
			if (slot->have && slot->number == number)
				return slot;
			if (!slot->have && !empty)
				empty = slot;
			if (slot->have && (!oldest || slot->number < oldest->number))
				oldest = slot;
		}
		return empty ? empty : oldest;
	}

	// The newest snapshot the ring currently holds, or NULL.
	const SnapSlot* NewestSnapSlot()
	{
		const SnapSlot* best = NULL;
		for (int i = 0; i < kSnapRingSize; ++i)
			if (s_snaps[i].have && (!best || s_snaps[i].number > best->number))
				best = &s_snaps[i];
		return best;
	}

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

	// -------------------------------------------------------------------------------------------
	// the gameState pointer: one owner, one place that sets it
	//
	// Three sources, best first: the CG_GETGAMESTATE trap (the cgame handing over the address of
	// its own cgs.gameState - authoritative), a shape scan of quake3.exe's data for the engine's
	// own cl.gameState (the fresh-join fallback - the engine fills it before the cgame VM exists),
	// and a shape scan of the cgame VM's data segment for cgs.gameState. All go through here so
	// the origin is always recorded next to the pointer.
	// -------------------------------------------------------------------------------------------
	void SetGameState(const q3::gameState_t* gs, GsOrigin origin)
	{
		s_gameState   = gs;
		s_gsOrigin    = origin;
		s_gsStampMs   = timeGetTime();
		// Only a pointer INTO the cgame carries VM identity: the instance check behind
		// VmFind::SameVmInstance judges it against the dataBase / dllHandle it was taken under.
		// The engine's own cl.gameState lives in quake3.exe and outlives every cgame VM, so its
		// identity fields stay zero and unused.
		const bool fromCgame = (origin != GsEngineScan);
		s_gsDataBase  = (fromCgame && s_vm) ? s_vm->dataBase  : 0;
		s_gsDllHandle = (fromCgame && s_vm) ? s_vm->dllHandle : 0;
	}

	void ClearGameState()
	{
		s_gameState   = NULL;
		s_gsOrigin    = GsTrap;
		s_gsDataBase  = 0;
		s_gsDllHandle = 0;
	}

	// The cgame keeps its own copy of the engine's gameState (cgs.gameState). Located by shape
	// inside the cgame VM's own data segment (or a native cgame DLL's image) - the fallback of
	// last resort. A scan that finds nothing leaves whatever pointer was there alone: a guess is
	// only replaced by a better guess.
	bool ScanCgameForGameState()
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
				SetGameState(gs, GsCgameScan);
				found = true;
				Log("[kutaQ3] cgame gameState at %p (found by scanning %u bytes)",
				    (const void*)gs, (unsigned)size);
			}
		});
		return found;
	}

	// The engine's own copy of the gameState: clientActive_t::gameState, a global in quake3.exe.
	//
	// This is the fallback that closes the fresh-join gap, and the reason is an ordering the hook
	// can never beat with the trap alone. CL_InitCGame() runs VM_Create("cgame") and CG_Init()
	// back to back, mid-frame, and CG_Init's FIRST gamestate trap is what the hook wants to see.
	// The detour, though, is installed from the SwapBuffers poll - at the END of that frame or a
	// later one - so a join from the main menu always misses it: there is no cgame VM record (and
	// so no dispatcher address to detour) until VM_Create has finished, and CG_Init fires the trap
	// microseconds after that. On stock 1.32 that trap then only re-fires when a configstring
	// changes at runtime (CG_ConfigStringModified re-fetches the whole gamestate on every "cs"
	// server command, cg_servercmds.c) - on a quiet FFA server, the first score event of the
	// match: the local player's first death. Exactly the "names only show up after I die once"
	// symptom.
	//
	// cl.gameState needs no trap at all. CL_ParseGamestate() fills it before CL_InitCGame() runs -
	// before the cgame VM even exists - and CL_GetGameState() (cl_cgame.c) is nothing but
	// `*gs = cl.gameState`, the very thing the trap hands over. Its configstrings are therefore
	// byte-identical to what the cgame's copy carries, they are FRESHER at a runtime "cs" (the
	// engine integrates the new string first), and the copy is found by the same shape scanner the
	// cgame side uses - no offsets, just the gameState's own invariants (see vmFind.h).
	bool ScanEngineForGameState()
	{
		bool found = false;
		if (s_codeLow >= s_codeHigh)
			return false;

		ForEachWritableRegion(s_codeLow, s_codeHigh, [&](const void* region, size_t size)
		{
			if (found)
				return;
			const q3::gameState_t* gs = NULL;
			if (VmFind::FindGameState(region, size, &gs))
			{
				SetGameState(gs, GsEngineScan);
				found = true;
				Log("[kutaQ3] engine gameState (cl.gameState) at %p (found by scanning, %u bytes)",
				    (const void*)gs, (unsigned)size);
			}
		});
		return found;
	}

	// While no pointer is known: engine copy first (it exists before the cgame VM does and needs
	// no trap), the cgame's own copy second. The first runtime "cs" re-fires CG_GETGAMESTATE and
	// the trap capture upgrades either guess on its own.
	bool ScanForGameState()
	{
		if (!s_vm)
			return false;
		if (ScanEngineForGameState())
			return true;
		return ScanCgameForGameState();
	}

	// -------------------------------------------------------------------------------------------
	// captured state
	// -------------------------------------------------------------------------------------------

	// The per-LEVEL captures: everything the cgame handed over while rendering the level that has
	// just ended. Positions, the camera and the last usercmd of the previous level are worse than
	// nothing - a tag built from them sits somewhere in a map that no longer exists.
	//
	// Deliberately NOT included: s_gameState. That pointer is to a global inside the cgame, it is
	// re-filled by CG_Init's trap_GetGameState() rather than re-allocated, and the copy is read
	// live through it - so it survives a level change. This used to drop it too, on the reasoning
	// that "everything captured so far belongs to the previous level", which is true of the
	// snapshots and false of the address CG_Init had handed over microseconds earlier. Dropping it
	// left every map change dependent on the scan finding the copy again, and until that happened
	// every name was skipped: tags appeared only when the next "cs" server command re-fired the
	// trap, which is the 10-60 second "no names" gap after joining / changing map.
	void DropLevelState()
	{
		memset(s_snaps, 0, sizeof(s_snaps));
		memset(&s_refdef, 0, sizeof(s_refdef));
		memset(&s_userCmd, 0, sizeof(s_userCmd));
		s_haveSnapshot   = false;
		s_haveRefdef     = false;
		s_haveUserCmd    = false;
		s_snapshotNumber = 0;
		s_snapshotTime   = 0;
		s_cmdNumber      = 0;
		// s_fov / s_haveFov survive: cg_fov is a client cvar, not per-level state, so the last
		// captured value is still what the cgame would read after the reload.
		NameEsp::Reset();
		NameEsp::ResetDrawState();      // the GL half's per-client state goes with the level too
	}

	// Everything, including the gameState pointer: the VM instance that owned them is gone.
	void DropCaptured()
	{
		DropLevelState();
		ClearGameState();
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
			// delivers nothing. Every successful request is kept, keyed by the message number:
			// the cgame walks processedSnapshotNum up one server frame at a time, so the previous
			// and newest snapshots both pass here and Gather() can interpolate between the two the
			// same way CG_InterpolateEntityPosition() does.
			if (result)
			{
				const uintptr_t dest = Resolve(args[2]);
				if (dest)
				{
					SnapSlot* slot = StoreSnapSlot(args[1]);
					if (slot)
					{
						memcpy(&slot->snap, (const void*)dest, sizeof(slot->snap));
						slot->have   = true;
						slot->number = args[1];
						const SnapSlot* newest = NewestSnapSlot();
						if (newest)
						{
							s_snapshotNumber = newest->number;
							s_snapshotTime   = newest->snap.serverTime;
							s_haveSnapshot   = true;
						}
					}
				}
			}
			break;

		case q3::CG_GETGAMESTATE:
		{
			// CG_Init fetches the whole gamestate once - and CG_ConfigStringModified() fetches it
			// again on EVERY "cs" server command (cg_servercmds.c: trap_GetGameState(&cgs.gameState)),
			// i.e. whenever any configstring changes - a score, a join, a team change. Either way
			// the address is &cgs.gameState; the copy is read live rather than kept here so that
			// configstring changes show up without a re-scan.
			//
			// CG_Init calls this BEFORE trap_CM_LoadMap (cg_main.c: trap_GetGameState, then
			// CG_ParseServerinfo, then trap_CM_LoadMap), so this is the first thing the hook sees
			// of a new level - and the level boundary below must not throw it away.
			//
			// The one capture this case can miss is CG_Init's own, on a fresh join from the main
			// menu: VM_Create + CG_Init run mid-frame, this detour is installed from the
			// SwapBuffers poll at frame end, and on stock 1.32 nothing re-fires the trap until the
			// first runtime "cs". ScanEngineForGameState() exists for exactly that window - it
			// finds the same content in the engine's own cl.gameState, which CL_ParseGamestate()
			// fills before the cgame VM is even created.
			const uintptr_t dest = Resolve(args[1]);
			if (dest)
				SetGameState((const q3::gameState_t*)dest, GsTrap);
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
			// CG_Init loads the collision map right after trap_GetGameState, i.e. at the start of
			// the new level: every position and camera captured while rendering still describes
			// the level that ended. The gameState pointer does not - it was captured a moment ago,
			// in THIS CG_Init, and points at the new cgame's cgs.gameState.
			DropLevelState();
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
			const int      requested = (int)va_arg(ap, intptr_t);  // message number
			const intptr_t dest      = va_arg(ap, intptr_t);
			va_end(ap);
			if (!dest)
				return 0;
			// Serve the exact snapshot Gather() asked for (the previous one is used to lerp
			// players the way the cgame does); if it has aged out of the ring, fall back to the
			// newest one. Gather() detects the fallback from matching/ascending serverTimes.
			const SnapSlot* slot = FindSnapSlot(requested);
			if (!slot)
				slot = NewestSnapSlot();
			if (!slot || !slot->have)
				return 0;                               // not connected / nothing valid yet
			memcpy((void*)dest, &slot->snap, sizeof(slot->snap));
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

		// Where the CS_PLAYERS configstrings came from, and how long the pointer has been held.
		// "cgame trap" is the cgame handing over the address of its own cgs.gameState (CG_Init,
		// then every "cs" server command); "engine cl.gameState (scan)" is the engine's own copy,
		// located by shape in quake3.exe - the fresh-join fallback; "cgame data (scan)" is the
		// cgame's copy found by shape inside its VM segment. This is the line to read when names
		// are missing: "not found yet" is a gathering failure, and a scan means the authoritative
		// capture has not arrived (yet) - which on a fresh join is normal until the first "cs".
		const char*    source = s_gsOrigin == GsTrap        ? "cgame trap"
		                      : s_gsOrigin == GsEngineScan ? "engine cl.gameState (scan)"
		                                                   : "cgame data (scan)";
		const unsigned ageS   = (unsigned)((timeGetTime() - s_gsStampMs) / 1000u);

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
			if (!s_gameState)
				SetStatus("hooked, native cgame %s - configstrings not found yet", path);
			else
				SetStatus("hooked, native cgame %s - configstrings: %s, %us old", path, source, ageS);
			return;
		}

		if (!s_haveSnapshot)
			SetStatus("hooked, %s - waiting for the first snapshot", kind);
		else if (!s_gameState)
			SetStatus("hooked, %s - configstrings not found yet", kind);
		else
			SetStatus("hooked, %s - configstrings: %s, %us old", kind, source, ageS);
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
		// A new VM instance means a new hunk segment (or a new cgame DLL): every pointer captured
		// from the old one points somewhere else. Compared against the identity the pointer was
		// TAKEN under, not against the last one seen: a level change inside one instance leaves
		// both unchanged, and the pointer to the cgame's own global stays good across it. The
		// engine's own cl.gameState is excluded - it belongs to quake3.exe, not to a cgame VM
		// instance, and outlives every one of them (only the shape checks below judge it).
		if (s_gameState && s_gsOrigin != GsEngineScan &&
		    !VmFind::SameVmInstance(*s_vm, s_gsDataBase, s_gsDllHandle))
		{
			Log("[kutaQ3] cgame VM instance changed - the configstrings pointer is stale");
			ClearGameState();
		}

		if (s_vm->dataBase != s_lastDataBase || s_vm->dllHandle != s_lastDllHandle)
		{
			Log("[kutaQ3] cgame VM reloaded (data %p -> %p)",
			    (void*)(uintptr_t)s_lastDataBase, (void*)(uintptr_t)s_vm->dataBase);
			DropLevelState();                   // the level is over; the pointer was judged above
			s_lastDataBase  = s_vm->dataBase;
			s_lastDllHandle = s_vm->dllHandle;
		}

		// Late inject (no trap has fired since the hook attached), or a pointer into memory the
		// hunk has since reused. Only a pointer that is missing or plainly dead is replaced - a
		// pointer the trap handed over is kept until the instance that owns it is gone.
		if (!s_gameState || !VmFind::GameStateLooksLive(s_gameState))
		{
			ClearGameState();
			ScanForGameState();
		}
		else if (now - s_lastShapeCheckMs >= kShapeCheckMs)
		{
			// A copy the SCAN found is a guess, so re-check it against the full shape on a timer
			// (too costly per frame: it walks every configstring). A pointer that no longer
			// matches gets one rescan attempt; when the scan has nothing better the old pointer is
			// kept, because "names might be wrong" beats "no names until the next cs command".
			s_lastShapeCheckMs = now;
			if (s_gsOrigin != GsTrap && !VmFind::GameStateIsUsable(s_gameState))
			{
				Log("[kutaQ3] the scanned gameState copy no longer matches - rescanning");
				ScanForGameState();
			}
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
