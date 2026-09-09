#pragma once

// =============================================================================================== //
// kutaQ3 hook - the cgame VM hook: one detour on the engine's per-VM syscall dispatcher
//
// Why not the cgame module
// ------------------------
// Retail Quake 3 has no cgame module to hook. The default client game logic is vm/cgame.mp.qvm
// inside baseq3/pak0.pk3 - bytecode that VM_Create() loads into quake3.exe and runs there
// (SDK/code/client/cl_cgame.c:741, vm.c). vm_cgame only chooses *how* it is run, and even that is
// overruled on a pure server:
//
//     if ( cl_connectedToPureServer != 0 ) {
//         interpret = VMI_COMPILED;          // qvms only - vm_cgame is ignored
//     } else {
//         interpret = Cvar_VariableValue( "vm_cgame" );
//     }
//     cgvm = VM_Create( "cgame", CL_CgameSystemCalls, interpret );
//
// VMI_NATIVE (0) is the only value that loads a DLL, VM_Init() defaults the cvar to 2
// (VMI_COMPILED), and sv_pure servers force bytecode whatever the cvar says. So a hook that needs
// cgame_mp_x86.dll to be mapped needs the user to change a cvar, only works off pure servers, and
// silently does nothing otherwise. This hook needs none of that.
//
// What it hooks instead
// ---------------------
// Every call the cgame makes into the engine - trap_GetSnapshot, trap_GetGameState,
// trap_R_RenderScene, ... - funnels through one function pointer the engine stored in the VM
// record when it created the VM:
//
//     vm->systemCall = systemCalls;      // CL_CgameSystemCalls for the cgame
//
// The interpreter calls it directly for a bytecode VM; a native DLL reaches the same pointer
// through VM_DllSyscall(). So a single Detours hook on that address covers both, and because the
// address is engine code in quake3.exe it is never unmapped - the hook survives map changes,
// reconnects and the engine freeing the VM, and can always be cleanly detached.
//
// The vm_t itself is found, not hardcoded: vmFind.h scans the main module's writable sections for
// the record, matching the invariants VM_Create() leaves behind (name "cgame", systemCall inside
// the module, dataMask/programStack/stackBottom consistent).
//
// How the ESP gets its data
// -------------------------
// The detour watches the traps flow past and keeps what the ESP needs. Nothing here calls the
// engine, so none of it depends on being inside a VM call:
//
//   CG_GETSNAPSHOT  -> the snapshot the engine just copied into the cgame's own buffer, copied
//                      again into ours: player entity positions + the local playerState_t
//   CG_GETGAMESTATE -> the address of the cgame's cgs.gameState, read live for the CS_PLAYERS
//                      configstrings (found by scanning instead when the hook was installed after
//                      the level loaded, see vmFind.h)
//   CG_R_RENDERSCENE-> the refdef_t the cgame rendered this frame: the exact view origin, view
//                      axis and fov, which is what NameEsp projects with
//   CG_CM_LOADMAP   -> level boundary: everything captured so far is dropped
//
// NameEsp::Gather() is handed a trampoline (Syscall()) that answers those same trap numbers out of
// the copies above, so the portable half of the ESP is unchanged and still testable.
// =============================================================================================== //

#include "q3sdk.h"
#include "vmFind.h"     // VmFind::Record - the mirrored vm_t, and the scanners that locate it

namespace Vm
{
	// Scan for the cgame VM and hook its syscall dispatcher. Cheap when there is no cgame loaded
	// yet (that is the normal state at the main menu). Idempotent.
	bool Install();

	// Called once per frame from the hooked SwapBuffers. Re-finds the VM after the engine frees it
	// (VM_Free() memsets the record), notices a new level and re-scans for the configstrings when
	// they have not been seen. Throttled internally. Returns true while a cgame VM is known.
	bool Poll();

	// Detach. The dispatcher lives in quake3.exe, which cannot unmap itself, so unlike a detour in
	// a cgame DLL this one is always safe to remove.
	void Shutdown();

	// One-line state for the menu / log.txt. Never NULL.
	const char* Status();

	// The trampoline NameEsp::Gather() reads through, or NULL when no cgame VM is known. Answers
	// CG_GETSNAPSHOT / CG_GETGAMESTATE / CG_GETCURRENTSNAPSHOTNUMBER from captured state; the
	// pointers it takes are plain host pointers (see q3sdk.h syscall_t).
	q3::syscall_t Syscall();

	// The refdef the cgame rendered this frame, or NULL when it has not been seen yet. Stays valid
	// until the next level load.
	const q3::refdef_t* Refdef();

	// The serverTime the frame being drawn belongs to: refdef_t::time, falling back to the newest
	// snapshot's serverTime. 0 when neither is known.
	int ServerTime();

	// True once a snapshot and the configstrings are both in hand, i.e. tags can be built.
	bool Ready();

	// Drop the captured state (level change, VM freed). The detour stays in place.
	void Reset();
}
