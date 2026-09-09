/*
===========================================================================
kutaQ3 hook - SDK mirror harness: q3sdk.h vs the real 1.32b engine headers

cl_sdkclstate.c and cl_sdkcgstate.c PRINT the engine's layouts. This one
CHECKS something: ../../../q3sdk.h is the hook's hand-written mirror of the
structures that cross the cgame <-> engine boundary, and every size, offset and
enum value in it is asserted here against the authoritative headers in this
directory. If the mirror ever drifts from the engine, this file stops
compiling - which is the whole point, because a drifted mirror reads the wrong
bytes out of a live snapshot instead of failing loudly.

    MSVC, from an "x86 Native Tools Command Prompt" at the SDK root:
        cl /nologo /W3 /TP /EHsc code\client\cl_sdkmirror.cpp /Fe:cl_sdkmirror.exe
        cl_sdkmirror.exe

    GCC / MinGW-w64, 32-bit, from the SDK root:
        g++ -m32 -std=c++11 -o cl_sdkmirror code/client/cl_sdkmirror.cpp && ./cl_sdkmirror

    GCC, 64-bit (see the ABI note below - it still means something here):
        g++ -std=c++11 -o cl_sdkmirror code/client/cl_sdkmirror.cpp && ./cl_sdkmirror

It is a .cpp, not a .c, unlike the other two harnesses: the mirror lives in
namespace q3 (so this file can include both it and the engine headers without
the type names colliding) and the checks are static_asserts, both C++ only.

ABI NOTE - why a 64-bit build is NOT useless for this one
---------------------------------------------------------
The other two harnesses print a warning when sizeof(void*) != 4, because
clientActive_t / cg_t / centity_t are full of pointers and a 64-bit build moves
every offset after the first one. Nothing in q3sdk.h contains a pointer or a
long: gameState_t, snapshot_t, playerState_t, entityState_t, trajectory_t and
usercmd_t are built out of int, byte, char and float arrays only, so their
sizes and offsets are identical on ILP32 (retail x86 quake3.exe) and LP64. The
assertions below therefore hold for the 32-bit target no matter which host
compiles them. q3sdk.h ends with a static_assert that keeps that property true.

This file includes the engine headers but contains no id Software source code.
It only compares facts about GPL code, so it is not itself GPL.
===========================================================================
*/

#include "../../../q3sdk.h"		/* the hook's mirror - namespace q3, checked below */

#include "../../../vmFind.h"	/* the vm_t mirror - checked against vm_local.h below */

#include "../game/q_shared.h"	/* guarded: __Q_SHARED_H */
#include "../game/bg_public.h"	/* unguarded - included once here */
#include "../cgame/cg_public.h"	/* unguarded - included once here; snapshot_t + the syscall enums */
#include "../cgame/tr_types.h"	/* guarded - refdef_t, the view the cgame rendered */
#include "../qcommon/vm_local.h"/* unguarded - struct vm_s, the VM record vmFind.h mirrors */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the mirror must agree with the engine, at compile time ---------------- */

#define CHECK_SIZE(t) \
	static_assert(sizeof(q3::t) == sizeof(::t), "q3sdk.h " #t " size differs from the engine")
#define CHECK_OFFSET(t, m) \
	static_assert(offsetof(q3::t, m) == offsetof(::t, m), \
	              "q3sdk.h " #t "::" #m " offset differs from the engine")
#define CHECK_CONST(mirrorName, engineName) \
	static_assert((int)(q3::mirrorName) == (int)(engineName), \
	              "q3sdk.h " #mirrorName " differs from the engine's " #engineName)

CHECK_SIZE(trajectory_t);
CHECK_SIZE(entityState_t);
CHECK_SIZE(playerState_t);
CHECK_SIZE(usercmd_t);
CHECK_SIZE(snapshot_t);
CHECK_SIZE(gameState_t);
CHECK_SIZE(refdef_t);

CHECK_OFFSET(trajectory_t, trType);
CHECK_OFFSET(trajectory_t, trTime);
CHECK_OFFSET(trajectory_t, trDuration);
CHECK_OFFSET(trajectory_t, trBase);
CHECK_OFFSET(trajectory_t, trDelta);

CHECK_OFFSET(entityState_t, number);
CHECK_OFFSET(entityState_t, eType);
CHECK_OFFSET(entityState_t, eFlags);
CHECK_OFFSET(entityState_t, pos);
CHECK_OFFSET(entityState_t, apos);
CHECK_OFFSET(entityState_t, clientNum);
CHECK_OFFSET(entityState_t, modelindex);
CHECK_OFFSET(entityState_t, weapon);

CHECK_OFFSET(playerState_t, origin);
CHECK_OFFSET(playerState_t, velocity);
CHECK_OFFSET(playerState_t, delta_angles);
CHECK_OFFSET(playerState_t, eFlags);
CHECK_OFFSET(playerState_t, clientNum);
CHECK_OFFSET(playerState_t, weapon);
CHECK_OFFSET(playerState_t, viewangles);
CHECK_OFFSET(playerState_t, viewheight);
CHECK_OFFSET(playerState_t, stats);

CHECK_OFFSET(usercmd_t, serverTime);
CHECK_OFFSET(usercmd_t, angles);
CHECK_OFFSET(usercmd_t, buttons);
CHECK_OFFSET(usercmd_t, weapon);

CHECK_OFFSET(snapshot_t, snapFlags);
CHECK_OFFSET(snapshot_t, ping);
CHECK_OFFSET(snapshot_t, serverTime);
CHECK_OFFSET(snapshot_t, areamask);
CHECK_OFFSET(snapshot_t, ps);
CHECK_OFFSET(snapshot_t, numEntities);
CHECK_OFFSET(snapshot_t, entities);
CHECK_OFFSET(snapshot_t, numServerCommands);
CHECK_OFFSET(snapshot_t, serverCommandSequence);

CHECK_OFFSET(gameState_t, stringOffsets);
CHECK_OFFSET(gameState_t, stringData);
CHECK_OFFSET(gameState_t, dataCount);

/* the view the cgame hands the renderer - vmHook.cpp captures it, NameEsp projects with it */
CHECK_OFFSET(refdef_t, x);
CHECK_OFFSET(refdef_t, y);
CHECK_OFFSET(refdef_t, width);
CHECK_OFFSET(refdef_t, height);
CHECK_OFFSET(refdef_t, fov_x);
CHECK_OFFSET(refdef_t, fov_y);
CHECK_OFFSET(refdef_t, vieworg);
CHECK_OFFSET(refdef_t, viewaxis);
CHECK_OFFSET(refdef_t, time);
CHECK_OFFSET(refdef_t, rdflags);
CHECK_OFFSET(refdef_t, areamask);
CHECK_OFFSET(refdef_t, text);

/* ---- the VM record (vmFind.h) ----------------------------------------------------------------
   vm_local.h pins the first two members itself - the x86 interpreter is written in assembly and
   reads them at these fixed offsets - so those two are checked against the engine's own constants
   and hold on any host. Everything after vm_t::name sits behind pointers, so on an LP64 host the
   engine struct is wider than the x86-only mirror and only a 32-bit build can compare them; that
   is what the #if below is for, and README.md says so. Nothing else in this file has that caveat:
   the other mirrors are pointer free. */
static_assert(offsetof(vm_t, programStack) == VM_OFFSET_PROGRAM_STACK,
              "the engine moved programStack without moving VM_OFFSET_PROGRAM_STACK");
static_assert(offsetof(VmFind::Record, programStack) == VM_OFFSET_PROGRAM_STACK,
              "vmFind.h Record::programStack is not at VM_OFFSET_PROGRAM_STACK");
static_assert(offsetof(VmFind::Record, systemCall) == VM_OFFSET_SYSTEM_CALL,
              "vmFind.h Record::systemCall is not at VM_OFFSET_SYSTEM_CALL");
static_assert(sizeof(((vm_t *)0)->name) == sizeof(VmFind::Record::name),
              "vmFind.h Record::name is not MAX_QPATH wide");
static_assert(sizeof(((vm_t *)0)->fqpath) == sizeof(VmFind::Record::fqpath),
              "vmFind.h Record::fqpath is not MAX_QPATH+1 wide");
static_assert(VmFind::kNameSize == MAX_QPATH, "vmFind.h kNameSize differs from MAX_QPATH");

#if UINTPTR_MAX == 0xffffffff	/* x86: the mirror can be compared field by field */
static_assert(sizeof(VmFind::Record) == sizeof(vm_t), "vmFind.h Record size differs from vm_t");
static_assert(offsetof(VmFind::Record, name) == offsetof(vm_t, name), "vm_t::name moved");
static_assert(offsetof(VmFind::Record, dllHandle) == offsetof(vm_t, dllHandle), "vm_t::dllHandle moved");
static_assert(offsetof(VmFind::Record, entryPoint) == offsetof(vm_t, entryPoint), "vm_t::entryPoint moved");
static_assert(offsetof(VmFind::Record, compiled) == offsetof(vm_t, compiled), "vm_t::compiled moved");
static_assert(offsetof(VmFind::Record, codeBase) == offsetof(vm_t, codeBase), "vm_t::codeBase moved");
static_assert(offsetof(VmFind::Record, codeLength) == offsetof(vm_t, codeLength), "vm_t::codeLength moved");
static_assert(offsetof(VmFind::Record, dataBase) == offsetof(vm_t, dataBase), "vm_t::dataBase moved");
static_assert(offsetof(VmFind::Record, dataMask) == offsetof(vm_t, dataMask), "vm_t::dataMask moved");
static_assert(offsetof(VmFind::Record, stackBottom) == offsetof(vm_t, stackBottom), "vm_t::stackBottom moved");
static_assert(offsetof(VmFind::Record, fqpath) == offsetof(vm_t, fqpath), "vm_t::fqpath moved");
#endif


/* limits the mirror repeats */
CHECK_CONST(kMaxClients, MAX_CLIENTS);
CHECK_CONST(kMaxConfigStrings, MAX_CONFIGSTRINGS);
CHECK_CONST(kMaxGamestateChars, MAX_GAMESTATE_CHARS);
CHECK_CONST(kMaxStats, MAX_STATS);
CHECK_CONST(kMaxPersistant, MAX_PERSISTANT);
CHECK_CONST(kMaxPowerups, MAX_POWERUPS);
CHECK_CONST(kMaxWeapons, MAX_WEAPONS);
CHECK_CONST(kMaxPsEvents, MAX_PS_EVENTS);
CHECK_CONST(kMaxMapAreaBytes, MAX_MAP_AREA_BYTES);
CHECK_CONST(kMaxEntitiesInSnapshot, MAX_ENTITIES_IN_SNAPSHOT);
CHECK_CONST(kMaxNameLength, MAX_NAME_LENGTH);
CHECK_CONST(kCsModels, CS_MODELS);
CHECK_CONST(kCsSounds, CS_SOUNDS);
CHECK_CONST(kCsPlayers, CS_PLAYERS);
CHECK_CONST(kEtPlayer, ET_PLAYER);
CHECK_CONST(kEfDead, EF_DEAD);
CHECK_CONST(kDefaultViewHeight, DEFAULT_VIEWHEIGHT);

/* the syscall numbers the VM hook watches, and the ones the ESP's bridge still answers */
CHECK_CONST(CG_MILLISECONDS, CG_MILLISECONDS);
CHECK_CONST(CG_CVAR_VARIABLESTRINGBUFFER, CG_CVAR_VARIABLESTRINGBUFFER);
CHECK_CONST(CG_CM_LOADMAP, CG_CM_LOADMAP);
CHECK_CONST(CG_R_RENDERSCENE, CG_R_RENDERSCENE);
CHECK_CONST(CG_GETGLCONFIG, CG_GETGLCONFIG);
CHECK_CONST(CG_GETGAMESTATE, CG_GETGAMESTATE);
CHECK_CONST(CG_GETCURRENTSNAPSHOTNUMBER, CG_GETCURRENTSNAPSHOTNUMBER);
CHECK_CONST(CG_GETSNAPSHOT, CG_GETSNAPSHOT);
CHECK_CONST(CG_GETCURRENTCMDNUMBER, CG_GETCURRENTCMDNUMBER);
CHECK_CONST(CG_GETUSERCMD, CG_GETUSERCMD);

CHECK_CONST(CG_INIT, CG_INIT);
CHECK_CONST(CG_SHUTDOWN, CG_SHUTDOWN);
CHECK_CONST(CG_CONSOLE_COMMAND, CG_CONSOLE_COMMAND);
CHECK_CONST(CG_DRAW_ACTIVE_FRAME, CG_DRAW_ACTIVE_FRAME);
CHECK_CONST(CG_CROSSHAIR_PLAYER, CG_CROSSHAIR_PLAYER);
CHECK_CONST(CG_LAST_ATTACKER, CG_LAST_ATTACKER);
CHECK_CONST(CG_KEY_EVENT, CG_KEY_EVENT);
CHECK_CONST(CG_MOUSE_EVENT, CG_MOUSE_EVENT);
CHECK_CONST(CG_EVENT_HANDLING, CG_EVENT_HANDLING);

/* ---- and the same table, printed, so a human can eyeball it ---------------- */

#define PSIZE(t)    printf("  %-22s mirror %6u  engine %6u\n", #t, \
                           (unsigned)sizeof(q3::t), (unsigned)sizeof(::t))
#define POFFSET(t, m) printf("    %-20s %-20s mirror %5u  engine %5u\n", #t, #m, \
                             (unsigned)offsetof(q3::t, m), (unsigned)offsetof(::t, m))
#define PCONST(mirrorName, engineName) \
	printf("  %-34s mirror %6d  engine %6d\n", "q3::" #mirrorName, \
	       (int)(q3::mirrorName), (int)(engineName))

int main(void)
{
	printf("kutaQ3 hook SDK mirror harness - q3sdk.h vs Q3 %s\n", Q3_VERSION);
	if (sizeof(void *) != 4)
	{
		printf("  note: sizeof(void*) == %u (%d-bit build). The types mirrored by\n",
			(unsigned)sizeof(void *), (int)(sizeof(void *) * 8));
		printf("  q3sdk.h contain no pointers, so the comparisons below are the\n");
		printf("  same on x86 and x86-64. See this file's header comment.\n");
	}

	printf("\nsizes\n");
	PSIZE(trajectory_t);
	PSIZE(entityState_t);
	PSIZE(playerState_t);
	PSIZE(usercmd_t);
	PSIZE(snapshot_t);
	PSIZE(gameState_t);
	PSIZE(refdef_t);

	printf("\noffsets the NAME ESP reads\n");
	POFFSET(trajectory_t, trBase);
	POFFSET(entityState_t, number);
	POFFSET(entityState_t, eType);
	POFFSET(entityState_t, eFlags);
	POFFSET(entityState_t, pos);
	POFFSET(entityState_t, clientNum);
	POFFSET(playerState_t, origin);
	POFFSET(playerState_t, velocity);
	POFFSET(playerState_t, delta_angles);
	POFFSET(playerState_t, clientNum);
	POFFSET(playerState_t, viewangles);
	POFFSET(playerState_t, viewheight);
	POFFSET(usercmd_t, serverTime);
	POFFSET(usercmd_t, angles);
	POFFSET(snapshot_t, serverTime);
	POFFSET(snapshot_t, ps);
	POFFSET(snapshot_t, numEntities);
	POFFSET(snapshot_t, entities);
	POFFSET(gameState_t, stringOffsets);
	POFFSET(gameState_t, stringData);
	POFFSET(refdef_t, fov_x);
	POFFSET(refdef_t, vieworg);
	POFFSET(refdef_t, viewaxis);
	POFFSET(refdef_t, time);

	printf("\nthe VM record vmFind.h mirrors (x86 only - see the #if above)\n");
	printf("  %-22s mirror %6u  engine %6u%s\n", "sizeof(Record/vm_t)",
	       (unsigned)sizeof(VmFind::Record), (unsigned)sizeof(vm_t),
	       sizeof(void *) == 4 ? "" : "   <-- host is not 32 bit, not comparable");
	printf("  %-22s mirror %6u  engine %6u\n", "VM_OFFSET_SYSTEM_CALL",
	       (unsigned)offsetof(VmFind::Record, systemCall), (unsigned)VM_OFFSET_SYSTEM_CALL);

	printf("\nlimits and constants\n");
	PCONST(kMaxClients, MAX_CLIENTS);
	PCONST(kMaxConfigStrings, MAX_CONFIGSTRINGS);
	PCONST(kMaxEntitiesInSnapshot, MAX_ENTITIES_IN_SNAPSHOT);
	PCONST(kCsModels, CS_MODELS);
	PCONST(kCsSounds, CS_SOUNDS);
	PCONST(kCsPlayers, CS_PLAYERS);
	PCONST(kEtPlayer, ET_PLAYER);
	PCONST(kEfDead, EF_DEAD);
	PCONST(kDefaultViewHeight, DEFAULT_VIEWHEIGHT);

	printf("\ncgame syscalls the VM hook watches / the ESP bridge answers\n");
	PCONST(CG_MILLISECONDS, CG_MILLISECONDS);
	PCONST(CG_CVAR_VARIABLESTRINGBUFFER, CG_CVAR_VARIABLESTRINGBUFFER);
	PCONST(CG_CM_LOADMAP, CG_CM_LOADMAP);
	PCONST(CG_R_RENDERSCENE, CG_R_RENDERSCENE);
	PCONST(CG_GETGLCONFIG, CG_GETGLCONFIG);
	PCONST(CG_GETGAMESTATE, CG_GETGAMESTATE);
	PCONST(CG_GETCURRENTSNAPSHOTNUMBER, CG_GETCURRENTSNAPSHOTNUMBER);
	PCONST(CG_GETSNAPSHOT, CG_GETSNAPSHOT);
	PCONST(CG_GETCURRENTCMDNUMBER, CG_GETCURRENTCMDNUMBER);
	PCONST(CG_GETUSERCMD, CG_GETUSERCMD);

	printf("\ncgame commands arriving at vmMain()\n");
	PCONST(CG_INIT, CG_INIT);
	PCONST(CG_SHUTDOWN, CG_SHUTDOWN);
	PCONST(CG_CONSOLE_COMMAND, CG_CONSOLE_COMMAND);
	PCONST(CG_DRAW_ACTIVE_FRAME, CG_DRAW_ACTIVE_FRAME);
	PCONST(CG_CROSSHAIR_PLAYER, CG_CROSSHAIR_PLAYER);
	PCONST(CG_LAST_ATTACKER, CG_LAST_ATTACKER);
	PCONST(CG_KEY_EVENT, CG_KEY_EVENT);
	PCONST(CG_MOUSE_EVENT, CG_MOUSE_EVENT);
	PCONST(CG_EVENT_HANDLING, CG_EVENT_HANDLING);

	printf("\nmirror matches the engine: every static_assert above compiled.\n");
	return 0;
}
