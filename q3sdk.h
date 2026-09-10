#pragma once

// =============================================================================================== //
// kutaQ3 hook - the Quake III Arena 1.32b cgame module ABI, mirrored by hand
//
// The NAME ESP reads the client state out of the cgame VM (see vmHook.h / vmFind.h), so it has to
// agree with the engine on the exact layout of the structures that cross that boundary:
// gameState_t, snapshot_t, playerState_t, entityState_t, usercmd_t, refdef_t, plus the syscall /
// command numbers.
//
// SDK/ holds the authoritative 1.32b headers (GPL v2, id Software release dbe4ddb). They are
// deliberately NOT #included here and nothing under SDK/ is listed in kutaQ3.vcxproj -
// SDK/README.md explains why: compiling the GPL headers into the DLL would make the whole hook a
// GPL derivative. So the handful of types below are transcribed by hand, and every size, offset
// and enum value is cross-checked against the real headers by
//
//     SDK/code/client/cl_sdkmirror.c
//
// which includes BOTH the engine headers and this file and static_asserts that they agree. Build
// and run that harness after touching anything below.
//
// This header is intentionally free of <windows.h> and <gl/GL.h> so the portable half of the ESP
// (nameEspCore.cpp) can be compiled and exercised off Windows by tests/.
// =============================================================================================== //

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

// The cgame module and the engine talk __cdecl. MSVC needs the keyword spelled out for a function
// pointer type; x86 gcc accepts it too. On x86-64 there is only one convention and the keyword does
// not exist, so it is dropped (the hook itself is a Win32 build - see README "Building").
#if defined(_MSC_VER) || defined(__i386__) || defined(_M_IX86)
#define Q3SDK_CDECL __cdecl
#else
#define Q3SDK_CDECL
#endif

namespace q3
{
	typedef unsigned char byte;
	typedef float         vec3_t[3];

	// ------------------------------------------------------------------------------------------
	// limits - SDK/code/game/q_shared.h, SDK/code/cgame/cg_public.h, SDK/code/game/bg_public.h
	//
	// These are deliberately kCamelCase instead of the engine's SCREAMING_CASE: the engine spells
	// them as #defines, so keeping the same names would make "q3::MAX_CLIENTS" expand to
	// "q3::64" in any translation unit that has both this header and the SDK headers in scope
	// (SDK/code/client/cl_sdkmirror.cpp does exactly that).
	// ------------------------------------------------------------------------------------------
	const int kMaxClients              = 64;      // q_shared.h:1092
	const int kMaxConfigStrings        = 1024;    // q_shared.h:1110
	const int kMaxGamestateChars       = 16000;   // q_shared.h:1119
	const int kMaxStats                = 16;      // q_shared.h:1129
	const int kMaxPersistant           = 16;      // q_shared.h:1130
	const int kMaxPowerups             = 16;      // q_shared.h:1131
	const int kMaxWeapons              = 16;      // q_shared.h:1132
	const int kMaxPsEvents             = 2;       // q_shared.h:1134
	const int kMaxMapAreaBytes         = 32;      // q_shared.h:390
	const int kMaxEntitiesInSnapshot   = 256;     // cg_public.h:32
	const int kMaxNameLength           = 32;      // q_shared.h:374
	const int kMaxModels               = 256;     // q_shared.h:1106
	const int kMaxSounds               = 256;     // q_shared.h:1107
	const int kCsModels                = 32;      // bg_public.h:85
	const int kCsSounds                = kCsModels + kMaxModels;
	const int kCsPlayers               = kCsSounds + kMaxSounds;   // == 544

	const int kEtPlayer                = 1;       // bg_public.h entityType_t (ET_GENERAL is 0)
	const int kEfDead                  = 0x00000001;   // bg_public.h:242

	const int kDefaultViewHeight       = 26;      // bg_public.h:50 (standing eye height)

	// pmtype_t + STAT_HEALTH - bg_public.h. The fallback view (nameEspCore.cpp BuildView) needs
	// these to match PM_UpdateViewAngles() (bg_pmove.c) exactly: while dead (health <= 0) and
	// playing (anything but PM_SPECTATOR), or during either intermission, the engine leaves the
	// viewangles frozen instead of rebuilding them from the usercmd.
	const int kPmSpectator             = 2;       // pmtype_t: PM_SPECTATOR
	const int kPmIntermission          = 5;       // pmtype_t: PM_INTERMISSION
	const int kPmSpIntermission        = 6;       // pmtype_t: PM_SPINTERMISSION
	const int kStatHealth              = 0;       // statIndex_t: STAT_HEALTH

	// PM_UpdateViewAngles() clamps pitch to +/-16000 shorts (+/-87.9 degrees) so the player can
	// never look straight up or down past 90 degrees. The fallback view applies the same clamp.
	const int kMaxViewPitchShort       = 16000;

	// The player bbox is MINS_Z..+32 while standing (bg_pmove.c:1293 / 1307), so a name tag a few
	// units above that clears the head.
	const float kPlayerTagHeight          = 36.0f;

	// Mid-torso of the same standing bbox. When the head anchor (+36) leaves the screen up close
	// - aiming up/down at a nearby player - Draw() re-anchors the tag here rather than clamping
	// a visible player's name to the edge; see the chest-anchor test in tests/test_gl.cpp.
	const float kChestHeight              = 16.0f;

	// ------------------------------------------------------------------------------------------
	// angle packing - q_shared.h:1082
	// ------------------------------------------------------------------------------------------
	inline int   AngleToShort(float x) { return ((int)((x) * 65536 / 360) & 65535); }
	inline float ShortToAngle(int x)   { return (float)((x) * (360.0 / 65536)); }

	// ------------------------------------------------------------------------------------------
	// trajectory_t / entityState_t / playerState_t - q_shared.h
	// ------------------------------------------------------------------------------------------
	struct trajectory_t
	{
		int     trType;      // trType_t
		int     trTime;
		int     trDuration;
		vec3_t  trBase;
		vec3_t  trDelta;
	};

	struct entityState_t
	{
		int          number;
		int          eType;
		int          eFlags;

		trajectory_t pos;
		trajectory_t apos;

		int          time;
		int          time2;

		vec3_t       origin;
		vec3_t       origin2;

		vec3_t       angles;
		vec3_t       angles2;

		int          otherEntityNum;
		int          otherEntityNum2;

		int          groundEntityNum;

		int          constantLight;
		int          loopSound;

		int          modelindex;
		int          modelindex2;
		int          clientNum;
		int          frame;

		int          solid;

		int          event;
		int          eventParm;

		int          powerups;
		int          weapon;
		int          legsAnim;
		int          torsoAnim;

		int          generic1;
	};

	struct playerState_t
	{
		int     commandTime;
		int     pm_type;
		int     bobCycle;
		int     pm_flags;
		int     pm_time;

		vec3_t  origin;
		vec3_t  velocity;
		int     weaponTime;
		int     gravity;
		int     speed;
		int     delta_angles[3];

		int     groundEntityNum;

		int     legsTimer;
		int     legsAnim;

		int     torsoTimer;
		int     torsoAnim;

		int     movementDir;

		vec3_t  grapplePoint;

		int     eFlags;

		int     eventSequence;
		int     events[kMaxPsEvents];
		int     eventParms[kMaxPsEvents];

		int     externalEvent;
		int     externalEventParm;
		int     externalEventTime;

		int     clientNum;
		int     weapon;
		int     weaponstate;

		vec3_t  viewangles;
		int     viewheight;

		int     damageEvent;
		int     damageYaw;
		int     damagePitch;
		int     damageCount;

		int     stats[kMaxStats];
		int     persistant[kMaxPersistant];
		int     powerups[kMaxPowerups];
		int     ammo[kMaxWeapons];

		int     generic1;
		int     loopSound;
		int     jumppad_ent;

		int     ping;
		int     pmove_framecount;
		int     jumppad_frame;
		int     entityEventSequence;
	};

	struct usercmd_t
	{
		int           serverTime;
		int           angles[3];
		int           buttons;
		byte          weapon;
		signed char   forwardmove, rightmove, upmove;
	};

	// ------------------------------------------------------------------------------------------
	// snapshot_t - cg_public.h
	// ------------------------------------------------------------------------------------------
	struct snapshot_t
	{
		int            snapFlags;
		int            ping;

		int            serverTime;

		byte           areamask[kMaxMapAreaBytes];

		playerState_t  ps;

		int            numEntities;
		entityState_t  entities[kMaxEntitiesInSnapshot];

		int            numServerCommands;
		int            serverCommandSequence;
	};

	// ------------------------------------------------------------------------------------------
	// gameState_t - q_shared.h:1120. Note the member order: offsets, then the packed strings, then
	// the count. A configstring is stringData + stringOffsets[index].
	// ------------------------------------------------------------------------------------------
	struct gameState_t
	{
		int   stringOffsets[kMaxConfigStrings];
		char  stringData[kMaxGamestateChars];
		int   dataCount;
	};

	// ------------------------------------------------------------------------------------------
	// refdef_t - cgame/tr_types.h. The view the cgame handed the renderer for this frame, and the
	// only place the exact view origin / angles / fov the frame was rendered with exist outside
	// the cgame module. vmHook.cpp captures it from the cgame's own CG_R_RENDERSCENE trap, which
	// may also fire for HUD models, whether the cgame is bytecode or a native DLL.
	// ------------------------------------------------------------------------------------------
	const int kRdfNoWorldModel       = 1;      // tr_types.h:RDF_NOWORLDMODEL
	const int kMaxRenderStrings      = 8;      // tr_types.h:MAX_RENDER_STRINGS
	const int kMaxRenderStringLength = 32;     // tr_types.h:MAX_RENDER_STRING_LENGTH

	struct refdef_t
	{
		int    x, y, width, height;
		float  fov_x, fov_y;
		vec3_t vieworg;
		vec3_t viewaxis[3];   // transformation matrix; [0] is the forward vector

		int    time;          // cg.time, i.e. the serverTime the frame was rendered for
		int    rdflags;

		byte   areamask[kMaxMapAreaBytes];
		char   text[kMaxRenderStrings][kMaxRenderStringLength];
	};

	// ------------------------------------------------------------------------------------------
	// syscall numbers - cgameImport_t, cg_public.h. These are the values arriving as args[0] of
	// the engine's per-VM syscall dispatcher (CL_CgameSystemCalls), which is what vmHook.cpp
	// detours. Only the ones the hook uses are mirrored; the values are checked against the real
	// enum by SDK/code/client/cl_sdkmirror.cpp.
	// ------------------------------------------------------------------------------------------
	enum cgameImport
	{
		CG_MILLISECONDS               = 2,
		CG_CVAR_VARIABLESTRINGBUFFER  = 6,
		CG_CM_LOADMAP                 = 18,   // fired during CG_Init - the level boundary
		CG_R_RENDERSCENE              = 44,   // args[1] = refdef (world or HUD model scene)
		CG_GETGLCONFIG                = 49,
		CG_GETGAMESTATE               = 50,   // args[1] = &cgs.gameState, once per level
		CG_GETCURRENTSNAPSHOTNUMBER   = 51,
		CG_GETSNAPSHOT                = 52,   // args[1] = number, args[2] = destination
		CG_GETCURRENTCMDNUMBER        = 54,
		CG_GETUSERCMD                 = 55
	};

	// ------------------------------------------------------------------------------------------
	// commands the engine sends INTO the cgame - cgameExport_t, cg_public.h. These travel through
	// the engine's VM_Call() and never reach the syscall dispatcher this hook detours, so nothing
	// in the DLL switches on them any more; they are mirrored because cl_sdkmirror.cpp checks the
	// whole cgameExport_t numbering against the engine header.
	// ------------------------------------------------------------------------------------------
	enum cgameExport
	{
		CG_INIT               = 0,
		CG_SHUTDOWN           = 1,
		CG_CONSOLE_COMMAND    = 2,
		CG_DRAW_ACTIVE_FRAME  = 3,
		CG_CROSSHAIR_PLAYER   = 4,
		CG_LAST_ATTACKER      = 5,
		CG_KEY_EVENT          = 6,
		CG_MOUSE_EVENT        = 7,
		CG_EVENT_HANDLING     = 8
	};

	// The syscall trampoline. The engine hands the *cgame* one of these through dllEntry() for a
	// native VM (VM_DllSyscall in vm.c), which resolves every pointer with VM_ArgPtr() - i.e. only
	// while the cgame VM is current. What NameEsp::Gather() gets is vmHook.cpp's bridge, which
	// takes the same arguments but answers them from state the hook already copied out, so the
	// pointers are plain host pointers and it works from any point in the frame. int is the 1.32
	// argument type (intptr_t on x86 is the same size).
	typedef int (Q3SDK_CDECL *syscall_t)(int arg, ...);

	// ------------------------------------------------------------------------------------------
	// gameState_t accessor - CG_ConfigString(), cg_main.c:1168
	// ------------------------------------------------------------------------------------------
	inline const char* ConfigString(const gameState_t* gs, int index)
	{
		if (!gs || index < 0 || index >= kMaxConfigStrings)
			return "";
		return gs->stringData + gs->stringOffsets[index];
	}

	// ------------------------------------------------------------------------------------------
	// Info_ValueForKey(), q_shared.c - the clientinfo configstrings ("\n\Name\t\red\model\sarge")
	// are info strings, and 1.32 compares keys case-insensitively. Returns false when the key is
	// absent, so callers can distinguish "no such key" from "empty value" (the engine returns ""
	// for both).
	// ------------------------------------------------------------------------------------------
	inline bool InfoValueForKey(const char* s, const char* key, char* out, size_t outSize)
	{
		if (out && outSize)
			out[0] = 0;
		if (!s || !key || !out || outSize == 0)
			return false;

		if (*s == '\\')
			++s;

		for (;;)
		{
			// key
			char pkey[kMaxNameLength * 4];
			size_t ki = 0;
			while (*s != '\\')
			{
				if (!*s)
				{
					out[0] = 0;                 // ran off the end - the key is not in here
					return false;
				}
				if (ki + 1 < sizeof(pkey))
					pkey[ki++] = *s;
				++s;
			}
			pkey[ki] = 0;
			++s;

			// value
			size_t vi = 0;
			while (*s && *s != '\\')
			{
				if (vi + 1 < outSize)
					out[vi++] = *s;
				++s;
			}
			out[vi] = 0;

			// case-insensitive compare, like Q_stricmp in 1.32
			bool match = true;
			for (size_t i = 0; ; ++i)
			{
				const char a = pkey[i];
				const char b = key[i];
				if (a == 0 || b == 0) { match = (a == 0 && b == 0); break; }
				char la = (a >= 'A' && a <= 'Z') ? (char)(a + 32) : a;
				char lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
				if (la != lb) { match = false; break; }
			}
			if (match)
				return true;

			if (!*s)
			{
				out[0] = 0;                     // walked the whole string with no match: leave
				                                // nothing behind, or the caller would read the
				                                // last value scanned instead of "not found"
				return false;
			}
			++s;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Q3 name colour codes: "^" followed by a digit selects one of the 8 console colours. The
	// cgame interprets them while drawing; printing them raw would put "^1" in front of a name.
	// Returns the number of characters written (excluding the terminator).
	// ------------------------------------------------------------------------------------------
	inline size_t StripColorCodes(const char* in, char* out, size_t outSize)
	{
		if (!out || outSize == 0)
			return 0;
		size_t n = 0;
		if (in)
		{
			for (; *in; ++in)
			{
				if (in[0] == '^' && in[1] >= '0' && in[1] <= '9')
				{
					++in;          // skip the digit; the loop's ++in consumes the '^' pair
					continue;
				}
				if (n + 1 < outSize)
					out[n++] = *in;
			}
		}
		out[n] = 0;
		return n;
	}

	// ------------------------------------------------------------------------------------------
	// AnglesToAxis(), q_math.c. axis[0] = forward, axis[1] = LEFT (that is -right, Q3 builds it as
	// -AngleVectors' right vector), axis[2] = up.
	// ------------------------------------------------------------------------------------------
	inline void AngleVectors(const vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
	{
		const float k = 3.14159265358979323846f * 2.0f / 360.0f;
		const float sy = sinf(angles[1] * k), cy = cosf(angles[1] * k);
		const float sp = sinf(angles[0] * k), cp = cosf(angles[0] * k);
		const float sr = sinf(angles[2] * k), cr = cosf(angles[2] * k);

		forward[0] = cp * cy;
		forward[1] = cp * sy;
		forward[2] = -sp;

		right[0] = (-1 * sr * sp * cy + -1 * cr * -sy);
		right[1] = (-1 * sr * sp * sy + -1 * cr * cy);
		right[2] = -1 * sr * cp;

		up[0] = (cr * sp * cy + -sr * -sy);
		up[1] = (cr * sp * sy + -sr * cy);
		up[2] = cr * cp;
	}

	inline void AnglesToAxis(const vec3_t angles, vec3_t axis[3])
	{
		vec3_t right;
		AngleVectors(angles, axis[0], right, axis[2]);
		axis[1][0] = -right[0];
		axis[1][1] = -right[1];
		axis[1][2] = -right[2];
	}

	// ------------------------------------------------------------------------------------------
	// Which module name is a native cgame? The engine builds "<vm>_mp_" ARCH_STRING ".dll"
	// (retail: cgame_mp_x86.dll); mod and ioquake3 builds use cgamex86.dll. Anything that is not a
	// cgame DLL - qagame_mp_x86.dll, ui_mp_x86.dll, cgame.mpq - is rejected.
	// Accepts a bare name or a full path.
	//
	// Nothing depends on a native cgame being loaded any more - vmHook.cpp hooks the VM layer, so
	// a bytecode cgame from pak0.pk3 works exactly the same. This is only used to label the one in
	// the menu status line when vm_cgame happens to select VMI_NATIVE.
	// ------------------------------------------------------------------------------------------
	inline bool IsNativeCgameModule(const char* nameOrPath)
	{
		if (!nameOrPath)
			return false;

		// strip the directory
		const char* name = nameOrPath;
		for (const char* p = nameOrPath; *p; ++p)
		{
			if (*p == '\\' || *p == '/' || *p == ':')
				name = p + 1;
		}

		char lower[128];
		size_t i = 0;
		for (; name[i] && i + 1 < sizeof(lower); ++i)
		{
			const char c = name[i];
			lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
		}
		lower[i] = 0;

		if (strncmp(lower, "cgame", 5) != 0)
			return false;

		const size_t len = strlen(lower);
		if (len < 8 || strcmp(lower + len - 4, ".dll") != 0)
			return false;

		return strstr(lower, "x86") != NULL;
	}

	// ------------------------------------------------------------------------------------------
	// build-time tripwires. The authoritative cross-check against the real engine headers lives in
	// SDK/code/client/cl_sdkmirror.c; these catch a bad edit here on their own.
	// ------------------------------------------------------------------------------------------
	static_assert(sizeof(trajectory_t) == 36, "trajectory_t does not match Q3 1.32b");
	static_assert(sizeof(entityState_t) == 208, "entityState_t does not match Q3 1.32b");
	static_assert(sizeof(playerState_t) == 468, "playerState_t does not match Q3 1.32b");
	static_assert(sizeof(usercmd_t) == 24, "usercmd_t does not match Q3 1.32b");
	static_assert(sizeof(snapshot_t) == 53772, "snapshot_t does not match Q3 1.32b");
	static_assert(sizeof(gameState_t) == 20100, "gameState_t does not match Q3 1.32b");

	static_assert(offsetof(entityState_t, number) == 0, "entityState_t::number moved");
	static_assert(offsetof(entityState_t, eType) == 4, "entityState_t::eType moved");
	static_assert(offsetof(entityState_t, eFlags) == 8, "entityState_t::eFlags moved");
	static_assert(offsetof(entityState_t, pos) == 12, "entityState_t::pos moved");
	static_assert(offsetof(entityState_t, clientNum) == 168, "entityState_t::clientNum moved");

	static_assert(offsetof(trajectory_t, trBase) == 12, "trajectory_t::trBase moved");

	static_assert(offsetof(playerState_t, origin) == 20, "playerState_t::origin moved");
	static_assert(offsetof(playerState_t, velocity) == 32, "playerState_t::velocity moved");
	static_assert(offsetof(playerState_t, delta_angles) == 56, "playerState_t::delta_angles moved");
	static_assert(offsetof(playerState_t, clientNum) == 140, "playerState_t::clientNum moved");
	static_assert(offsetof(playerState_t, viewangles) == 152, "playerState_t::viewangles moved");
	static_assert(offsetof(playerState_t, viewheight) == 164, "playerState_t::viewheight moved");

	static_assert(offsetof(snapshot_t, serverTime) == 8, "snapshot_t::serverTime moved");
	static_assert(offsetof(snapshot_t, ps) == 44, "snapshot_t::ps moved");
	static_assert(offsetof(snapshot_t, numEntities) == 512, "snapshot_t::numEntities moved");
	static_assert(offsetof(snapshot_t, entities) == 516, "snapshot_t::entities moved");
	static_assert(offsetof(usercmd_t, angles) == 4, "usercmd_t::angles moved");

	static_assert(offsetof(gameState_t, stringOffsets) == 0, "gameState_t::stringOffsets moved");
	static_assert(offsetof(gameState_t, stringData) == 4096, "gameState_t::stringData moved");
	static_assert(offsetof(gameState_t, dataCount) == 20096, "gameState_t::dataCount moved");

	static_assert(sizeof(refdef_t) == 368, "refdef_t does not match Q3 1.32b");
	static_assert(offsetof(refdef_t, x) == 0, "refdef_t::x moved");
	static_assert(offsetof(refdef_t, width) == 8, "refdef_t::width moved");
	static_assert(offsetof(refdef_t, fov_x) == 16, "refdef_t::fov_x moved");
	static_assert(offsetof(refdef_t, fov_y) == 20, "refdef_t::fov_y moved");
	static_assert(offsetof(refdef_t, vieworg) == 24, "refdef_t::vieworg moved");
	static_assert(offsetof(refdef_t, viewaxis) == 36, "refdef_t::viewaxis moved");
	static_assert(offsetof(refdef_t, time) == 72, "refdef_t::time moved");
	static_assert(offsetof(refdef_t, rdflags) == 76, "refdef_t::rdflags moved");
	static_assert(offsetof(refdef_t, areamask) == 80, "refdef_t::areamask moved");
	static_assert(offsetof(refdef_t, text) == 112, "refdef_t::text moved");

	// No member of anything mirrored here is a pointer or a long, so these sizes and offsets are
	// the same on ILP32 (retail x86 quake3.exe) and LP64. That is what lets the harness above be
	// checked on a 64-bit host - see its header comment.
	static_assert(sizeof(byte) == 1 && sizeof(int) == 4 && sizeof(float) == 4,
	              "mirrored types must stay pointer-free and 4 bytes wide");
}
