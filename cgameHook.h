#pragma once

// =============================================================================================== //
// kutaQ3 hook - Detours hook on the cgame VM's vmMain()
//
// Quake 3 keeps all of the interesting per-frame client state (which players exist, where they
// are, what they are called, what the view is) behind the cgame module boundary. The engine only
// ever talks to that module through two exported functions:
//
//     void dllEntry( int (QDECL *dllSyscall)(int arg, ...) );   // engine hands the module its
//                                                               // syscall trampoline
//     int  vmMain( int command, int arg0 ... int arg11 );       // every call INTO the cgame
//
// Both are plain exports of the native cgame DLL (cgame_mp_x86.dll in retail 1.32, cgamex86.dll in
// mod / ioquake3 builds), so Microsoft Detours - the same library the rest of this hook is built
// on - can attach to them directly, with no offsets to find in quake3.exe:
//
//     GetProcAddress( hCgame, "vmMain" ) -> DetourAttach -> our detour sees every command.
//
// What the hook is used for
// -------------------------
//   - dllEntry captures the syscall trampoline. That pointer is the only supported way in: the
//     engine resolves the pointers handed to it with VM_ArgPtr(), which passes them through only
//     while the cgame VM is the current one, so anything reading client state through it has to be
//     running inside a VM call. The trampoline is exactly what the cgame itself calls
//     (trap_GetSnapshot, trap_GetGameState, ...), and it is called the same way.
//   - vmMain(CG_DRAW_ACTIVE_FRAME, serverTime, ...) fires once per rendered frame, before the
//     renderer presents. That is where NameEsp::Gather() runs.
//   - vmMain(CG_INIT) / vmMain(CG_SHUTDOWN) mark level load and unload, so stale tags are dropped.
//
// Attaching late, and unloading
// -----------------------------
// The engine loads the cgame, calls dllEntry, and then sends CG_INIT, all inside one client frame -
// so the hook has to be in place the moment LoadLibrary returns. main.cpp's LoadLibraryExA /
// LoadLibraryA detours do that (see OnModuleLoaded). If the DLL was already resident when this DLL
// was injected, Install() finds it instead; but if dllEntry has already run by then the trampoline
// is gone and the ESP cannot read anything until the next map load. Status() says so in the menu.
//
// The engine frees the cgame module on disconnect, and the trampoline Detours allocated for it is
// not recoverable afterwards - patching it back would write into unmapped memory. So an unloaded
// module is abandoned, not detached: the pointers are dropped, the next load is hooked afresh, and
// Shutdown() only detaches a module that is still resident.
// =============================================================================================== //

#include "q3sdk.h"

namespace CGame
{
	// Hook the native cgame module if one is already resident. Only uses GetModuleHandle on the
	// known module names, so it is safe to call from DllMain / inside a LoadLibrary detour (under
	// the loader lock). Returns true once attached.
	bool Install();

	// Called once per frame from the hooked SwapBuffers: notices an unloaded cgame, and retries the
	// attach with a full module enumeration (which is not loader-lock safe, so it does not live in
	// Install()). Returns true while attached.
	bool Poll();

	// Called from the LoadLibraryExA / LoadLibraryA detours when the engine has just mapped a
	// module whose name looks like a native cgame. Attach immediately - dllEntry is called a
	// moment later and must not be missed.
	void OnModuleLoaded(const char* modulePath);

	// Detach, but only if the cgame module is still resident. Called on DLL unload.
	void Shutdown();

	// The engine's syscall trampoline, or NULL when it was not captured. Only valid to call from
	// inside a cgame VM call - see the header comment.
	q3::syscall_t Syscall();

	// One-line state for the menu / log.txt. Never NULL.
	const char* Status();
}
