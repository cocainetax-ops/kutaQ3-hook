#pragma once

// =============================================================================================== //
// kutaQ3 hook - locating the cgame VM (and its gameState) inside quake3.exe, portably
//
// Why this exists
// ---------------
// Retail Quake 3 does not load a cgame DLL. CL_InitCGame() (SDK/code/client/cl_cgame.c:734) picks
// an interpreter and calls
//
//     cgvm = VM_Create( "cgame", CL_CgameSystemCalls, interpret );
//
// and for a bytecode cgame VM_Create() reads vm/cgame.mp.qvm out of pak0.pk3 and runs it inside
// quake3.exe (vm.c). There is therefore no module to hook and no vmMain export to find: the whole
// cgame <-> engine boundary is internal to the executable. The one thing that does exist for both
// cases is the per-VM syscall dispatcher the engine stored in the VM record -
//
//     vm->systemCall = systemCalls;      // CL_CgameSystemCalls for the cgame
//
// Every trap the cgame makes ends up there, whether the cgame is bytecode (the interpreter calls
// vm->systemCall directly) or a native DLL (VM_DllSyscall does currentVM->systemCall(&arg)). So the
// hook needs exactly one address - vm->systemCall - and it reads it out of the engine's own vm_t.
//
// Finding that vm_t without hardcoding an address
// -----------------------------------------------
// quake3.exe exports nothing, so the vm_t has to be located by looking for it. vm.c keeps the three
// VM records in a static table inside the executable (vmTable[MAX_VM]), so scanning the writable
// sections of the main module finds them. A record is only accepted when every invariant
// VM_Create() / VM_PrepareInterpreter() establish holds at once (vm.c, verified against the GPL
// 1.32b source):
//
//     name            == "cgame"
//     systemCall      inside the main module   (it is CL_CgameSystemCalls)
//     bytecode: dataMask == (1<<n)-1 with n >= 16, dataBase != NULL,
//               programStack == dataMask + 1, stackBottom == programStack - STACK_SIZE,
//               codeBase != NULL, codeLength > 0
//     native:   dllHandle != NULL, entryPoint != NULL, dataBase == NULL, dataMask == 0,
//               programStack == 0, stackBottom == 0
//
// The first two vm_t fields are ABI-locked by the engine itself - vm_local.h pins them with
// VM_OFFSET_PROGRAM_STACK 0 / VM_OFFSET_SYSTEM_CALL 4 because the x86 interpreter is written in
// assembly - so the record layout this file mirrors is the one the shipped binary uses.
//
// The same trick locates the configstrings: the cgame keeps its own copy of the engine's
// gameState_t in its data segment (cgs.gameState, filled by trap_GetGameState() in CG_Init), and a
// gameState_t is recognisable by shape - the nonzero string offsets (strictly increasing and all
// inside the 16000 byte string pool, with zero gaps for indices the server never set), a
// serverinfo carrying the mapname, and CS_PLAYERS entries pointing at "\n\...\t\..." infostrings.
// That is what makes injecting into a map that is already running work: the CG_GETGAMESTATE trap
// that hands the address over only fires at level load, so on a late inject the copy is found by
// scanning instead.
//
// Nothing in this file needs Windows, so tests/test_vmfind.cpp builds a real vm_t / gameState_t
// from the SDK headers and checks the scanners against it.
// =============================================================================================== //

#include "q3sdk.h"

namespace VmFind
{
	// ---- engine constants, vm.c / qfiles.h -----------------------------------------------------
	const int kNameSize    = 64;        // MAX_QPATH
	const int kStackSize   = 0x20000;   // vm.c STACK_SIZE, used for stackBottom
	const int kMinDataBits = 16;        // a cgame QVM's data+bss segment is at least 64 KB

	// ------------------------------------------------------------------------------------------
	// struct vm_s, mirrored from SDK/code/qcommon/vm_local.h.
	//
	// Every pointer member is a uint32_t on purpose: the hook is an x86 build (see README
	// "Building"), and spelling the pointers as uint32 makes every offset below identical on ILP32
	// and LP64 - which is what lets SDK/code/client/cl_sdkmirror.cpp check them against the real
	// header on a 64-bit host. On x86 the layouts are byte for byte the same.
	// ------------------------------------------------------------------------------------------
	struct Record
	{
		int32_t  programStack;              //   0 - VM_OFFSET_PROGRAM_STACK
		uint32_t systemCall;                //   4 - VM_OFFSET_SYSTEM_CALL
		char     name[kNameSize];           //   8 - "cgame" / "ui" / "qagame"
		uint32_t dllHandle;                 //  72 - non-zero for a native DLL cgame
		uint32_t entryPoint;                //  76 - the DLL's vmMain (bytecode VMs leave it 0)
		int32_t  currentlyInterpreting;     //  80
		int32_t  compiled;                  //  84 - 1 for VMI_COMPILED (JIT), 0 for VMI_BYTECODE
		uint32_t codeBase;                  //  88
		int32_t  codeLength;                //  92
		uint32_t instructionPointers;       //  96
		int32_t  instructionPointersLength; // 100
		uint32_t dataBase;                  // 104 - host address of the VM's data segment
		int32_t  dataMask;                  // 108 - (data segment size) - 1
		int32_t  stackBottom;               // 112
		int32_t  numSymbols;                // 116
		uint32_t symbols;                   // 120
		int32_t  callLevel;                 // 124
		int32_t  breakFunction;             // 128
		int32_t  breakCount;                // 132
		char     fqpath[kNameSize + 1];     // 136 - full path Sys_LoadDll() filled in
	};

	static_assert(sizeof(Record) == 204, "vm_t mirror does not match Q3 1.32b (x86)");
	static_assert(offsetof(Record, programStack) == 0, "vm_t::programStack moved");
	static_assert(offsetof(Record, systemCall) == 4, "vm_t::systemCall moved");
	static_assert(offsetof(Record, name) == 8, "vm_t::name moved");
	static_assert(offsetof(Record, dllHandle) == 72, "vm_t::dllHandle moved");
	static_assert(offsetof(Record, entryPoint) == 76, "vm_t::entryPoint moved");
	static_assert(offsetof(Record, compiled) == 84, "vm_t::compiled moved");
	static_assert(offsetof(Record, codeBase) == 88, "vm_t::codeBase moved");
	static_assert(offsetof(Record, dataBase) == 104, "vm_t::dataBase moved");
	static_assert(offsetof(Record, dataMask) == 108, "vm_t::dataMask moved");
	static_assert(offsetof(Record, stackBottom) == 112, "vm_t::stackBottom moved");
	static_assert(offsetof(Record, fqpath) == 136, "vm_t::fqpath moved");

	struct Found
	{
		const Record* record;   // points into the scanned region; NULL when nothing was found
		bool          native;   // dllHandle != 0, i.e. a DLL cgame rather than bytecode
	};

	// Is this record a live cgame VM? codeLow/codeHigh bound the main module (the address
	// systemCall has to fall inside). bytesAvailable is how much of the record is readable from
	// candidate onwards - pass sizeof(Record). native may be NULL.
	bool IsCgameVm(const Record* candidate, size_t bytesAvailable,
	               uintptr_t codeLow, uintptr_t codeHigh, bool* native);

	// Scan [region, region + size) at 4 byte alignment for a cgame VM record.
	bool FindVm(const void* region, size_t size, uintptr_t codeLow, uintptr_t codeHigh, Found& out);

	// Scan [region, region + size) at 4 byte alignment for a gameState_t copy (the cgame's
	// cgs.gameState). Returns false when nothing passes the shape checks.
	bool FindGameState(const void* region, size_t size, const q3::gameState_t** out);

	// Cheap re-check for a gameState_t already in hand: is this still a live copy, or has the hunk
	// moved on since the address was captured? Two int reads, safe to call every frame.
	bool GameStateLooksLive(const q3::gameState_t* gs);
}
