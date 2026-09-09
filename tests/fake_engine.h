#pragma once

// =============================================================================================== //
// kutaQ3 hook tests - a stand-in for the engine half of the cgame <-> engine boundary
//
// NameEsp::Gather() talks to the engine through the syscall trampoline the engine hands the cgame
// in dllEntry(). This file implements that trampoline the way the engine does - varargs in,
// packed into an int array, dispatched on the first element, pointers passed straight through
// because a native DLL VM is "current" - so the tests exercise the real ABI usage in
// nameEspCore.cpp instead of a simplified copy of it.
//
// On the 32-bit target the engine's arguments are `int` and intptr_t is the same size; the tests
// use intptr_t so the same code builds and runs on a 64-bit host.
// =============================================================================================== //

#include "q3sdk.h"

namespace FakeEngine
{
	void Reset();

	// ---- connection / timing -------------------------------------------------------------------
	void SetConnected(bool connected);          // false makes CG_GETSNAPSHOT fail, like CL_GetSnapshot
	void SetSnapshotTime(int serverTime);       // snapshot_t::serverTime
	void SetServerTime(int serverTime);         // what the engine passes as CG_DRAW_ACTIVE_FRAME arg0

	// ---- the local player (snapshot ps + the usercmd the client built this frame) ---------------
	void SetLocalPlayer(int clientNum, const float origin[3], const float velocity[3],
	                    const float viewAngles[3], int viewHeight);
	void SetDeltaAngles(int pitch, int yaw, int roll);   // playerState_t::delta_angles, raw shorts
	void SetCmdAngles(int pitch, int yaw, int roll);     // usercmd_t::angles, packed with ANGLE2SHORT
	void SetCmdServerTime(int serverTime);
	void SetNoUserCmd();                                 // CG_GETUSERCMD starts failing
	void SetFovString(const char* value);                // what "cg_fov" reads as

	// ---- other clients --------------------------------------------------------------------------
	// infoString is the raw CS_PLAYERS configstring, e.g. "\\n\\Bitterman\\t\\1\\model\\sarge"
	// (the team is numeric in 1.32: 0 free, 1 red, 2 blue, 3 spectator)
	void SetPlayer(int clientNum, const char* infoString, const float origin[3]);
	void SetDeadPlayer(int clientNum, const char* infoString, const float origin[3]);
	void SetNonPlayerEntity(int number, const float origin[3]);   // ET_ITEM etc. - must be ignored

	// ---- the trampoline -------------------------------------------------------------------------
	q3::syscall_t Syscall();

	// ---- what the tests assert on ---------------------------------------------------------------
	int SnapshotRequests();
	int GameStateRequests();
	int UserCmdRequests();
	const q3::snapshot_t& Snapshot();
}
