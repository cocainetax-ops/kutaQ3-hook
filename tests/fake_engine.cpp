// =============================================================================================== //
// kutaQ3 hook tests - fake engine syscall trampoline (see fake_engine.h)
// =============================================================================================== //

#include "fake_engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace
{
	q3::snapshot_t   s_snapshot;
	q3::gameState_t  s_gameState;
	q3::usercmd_t    s_cmd;

	bool s_connected   = true;
	bool s_haveUserCmd = true;
	int  s_cmdNumber   = 7;
	char s_fov[32]     = "90";

	int s_snapshotRequests  = 0;
	int s_gameStateRequests = 0;
	int s_userCmdRequests   = 0;
	int s_entityCount       = 0;

	// -------------------------------------------------------------------------------------------
	// configstrings, exactly the way the engine packs them into gameState_t
	// -------------------------------------------------------------------------------------------
	void SetConfigString(int index, const char* value)
	{
		if (index < 0 || index >= q3::kMaxConfigStrings)
			return;

		const size_t len = value ? strlen(value) : 0;
		if (s_gameState.dataCount + (int)len + 1 > q3::kMaxGamestateChars)
			return;

		s_gameState.stringOffsets[index] = s_gameState.dataCount;
		memcpy(s_gameState.stringData + s_gameState.dataCount, value ? value : "", len + 1);
		s_gameState.dataCount += (int)len + 1;
	}

	q3::entityState_t& AddEntity(int eType, int number, const float origin[3], int eFlags)
	{
		q3::entityState_t& e = s_snapshot.entities[s_entityCount];
		memset(&e, 0, sizeof(e));
		e.eType        = eType;
		e.number       = number;
		e.clientNum    = number;
		e.eFlags       = eFlags;
		e.pos.trType   = 1;                     // TR_INTERPOLATE, what the server sends for players
		e.pos.trTime   = s_snapshot.serverTime;
		if (origin)
		{
			e.pos.trBase[0] = origin[0];
			e.pos.trBase[1] = origin[1];
			e.pos.trBase[2] = origin[2];
		}
		++s_entityCount;
		s_snapshot.numEntities = s_entityCount;
		return e;
	}

	// -------------------------------------------------------------------------------------------
	// the trampoline: args[0] is the call, the rest are what that call takes. Pointers pass
	// through untouched - VM_ArgPtr() does that for a native DLL VM, which is the mode the hook
	// runs in.
	// -------------------------------------------------------------------------------------------
	int ArgsFor(int call)
	{
		switch (call)
		{
		case q3::CG_GETSNAPSHOT:              return 3;
		case q3::CG_CVAR_VARIABLESTRINGBUFFER: return 3;
		case q3::CG_GETCURRENTSNAPSHOTNUMBER: return 2;
		case q3::CG_GETUSERCMD:               return 2;
		case q3::CG_GETGAMESTATE:             return 1;
		default:                              return 0;
		}
	}

	int Dispatch(const intptr_t* args)
	{
		switch (args[0])
		{
		case q3::CG_MILLISECONDS:
			return s_snapshot.serverTime;

		case q3::CG_CVAR_VARIABLESTRINGBUFFER:
		{
			const char* name = (const char*)args[1];
			char*       buf  = (char*)args[2];
			const int   size = (int)args[3];
			const char* value = (name && strcmp(name, "cg_fov") == 0) ? s_fov : "";
			if (buf && size > 0)
			{
				strncpy(buf, value, (size_t)size - 1);
				buf[size - 1] = 0;
			}
			return 0;
		}

		case q3::CG_GETGAMESTATE:
			++s_gameStateRequests;
			*(q3::gameState_t*)args[1] = s_gameState;      // CL_GetGameState: *gs = cl.gameState
			return 0;

		case q3::CG_GETCURRENTSNAPSHOTNUMBER:
			*(int*)args[1] = 0;                            // CL_GetCurrentSnapshotNumber
			*(int*)args[2] = s_snapshot.serverTime;
			return 0;

		case q3::CG_GETSNAPSHOT:
			++s_snapshotRequests;
			if (!s_connected)
				return 0;                                  // CL_GetSnapshot says "not valid"
			*(q3::snapshot_t*)args[2] = s_snapshot;
			return 1;

		case q3::CG_GETCURRENTCMDNUMBER:
			return s_cmdNumber;

		case q3::CG_GETUSERCMD:
			++s_userCmdRequests;
			if (!s_haveUserCmd)
				return 0;
			*(q3::usercmd_t*)args[2] = s_cmd;              // CL_GetUserCmd: *ucmd = cl.cmds[n & CMD_MASK]
			return 1;

		default:
			return 0;
		}
	}

	int Q3SDK_CDECL FakeSystemCalls(int arg, ...)
	{
		intptr_t args[4] = { (intptr_t)arg, 0, 0, 0 };

		va_list ap;
		va_start(ap, arg);
		const int n = ArgsFor(arg);
		for (int i = 1; i <= n && i < 4; ++i)
			args[i] = va_arg(ap, intptr_t);
		va_end(ap);

		return Dispatch(args);
	}
}

// =============================================================================================== //

namespace FakeEngine
{
	void Reset()
	{
		memset(&s_snapshot, 0, sizeof(s_snapshot));
		memset(&s_gameState, 0, sizeof(s_gameState));
		memset(&s_cmd, 0, sizeof(s_cmd));

		s_connected         = true;
		s_haveUserCmd       = true;
		s_cmdNumber         = 7;
		strcpy(s_fov, "90");
		s_snapshotRequests  = 0;
		s_gameStateRequests = 0;
		s_userCmdRequests   = 0;
		s_entityCount       = 0;

		s_snapshot.ps.viewheight = q3::kDefaultViewHeight;
	}

	void SetConnected(bool connected)   { s_connected = connected; }
	void SetSnapshotTime(int serverTime){ s_snapshot.serverTime = serverTime; }
	void SetServerTime(int)             { /* the caller passes it to Gather() directly */ }

	void SetLocalPlayer(int clientNum, const float origin[3], const float velocity[3],
	                    const float viewAngles[3], int viewHeight)
	{
		s_snapshot.ps.clientNum  = clientNum;
		s_snapshot.ps.viewheight = viewHeight;
		for (int i = 0; i < 3; ++i)
		{
			s_snapshot.ps.origin[i]     = origin[i];
			s_snapshot.ps.velocity[i]   = velocity[i];
			s_snapshot.ps.viewangles[i] = viewAngles[i];
			s_cmd.angles[i]             = q3::AngleToShort(viewAngles[i]);
		}
		s_cmd.serverTime = s_snapshot.serverTime;

		// the engine puts the local player's entity in the snapshot too - Gather() must skip it
		AddEntity(q3::kEtPlayer, clientNum, origin, 0);
	}

	void SetDeltaAngles(int pitch, int yaw, int roll)
	{
		s_snapshot.ps.delta_angles[0] = pitch;
		s_snapshot.ps.delta_angles[1] = yaw;
		s_snapshot.ps.delta_angles[2] = roll;
	}

	void SetCmdAngles(int pitch, int yaw, int roll)
	{
		s_cmd.angles[0] = pitch;
		s_cmd.angles[1] = yaw;
		s_cmd.angles[2] = roll;
		s_haveUserCmd   = true;
	}

	void SetCmdServerTime(int serverTime) { s_cmd.serverTime = serverTime; }
	void SetNoUserCmd()                   { s_haveUserCmd = false; }
	void SetFovString(const char* value)  { strncpy(s_fov, value, sizeof(s_fov) - 1); }

	void SetPlayer(int clientNum, const char* infoString, const float origin[3])
	{
		SetConfigString(q3::kCsPlayers + clientNum, infoString);
		AddEntity(q3::kEtPlayer, clientNum, origin, 0);
	}

	void SetDeadPlayer(int clientNum, const char* infoString, const float origin[3])
	{
		SetConfigString(q3::kCsPlayers + clientNum, infoString);
		AddEntity(q3::kEtPlayer, clientNum, origin, q3::kEfDead);
	}

	void SetNonPlayerEntity(int number, const float origin[3])
	{
		AddEntity(2 /* ET_ITEM */, number, origin, 0);
	}

	q3::syscall_t Syscall() { return FakeSystemCalls; }

	int SnapshotRequests()  { return s_snapshotRequests; }
	int GameStateRequests() { return s_gameStateRequests; }
	int UserCmdRequests()   { return s_userCmdRequests; }
	const q3::snapshot_t& Snapshot() { return s_snapshot; }
}
