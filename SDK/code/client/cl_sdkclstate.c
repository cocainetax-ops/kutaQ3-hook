/*
===========================================================================
kutaQ3 hook - SDK layout harness, part 1 of 2: client / shared / renderer ABI

WHY A LAYOUT HARNESS AT ALL
---------------------------
Every engine file in this SDK came from the id Software Quake III Arena 1.32b GPL
source, so the field ORDER matches retail quake3.exe 1.32b. Order is only half of
a memory layout though - the other half is padding, and padding depends on the
ABI. Retail Quake 3 is a 32-bit (x86) Win32 binary, so the numbers this prints
are only the real ones when it is compiled 32-bit. Compile it 64-bit and every
pointer field grows from 4 bytes to 8, silently shifting every offset after the
first pointer in a struct.

So: build this for x86 (the same configuration as the hook DLL), read the
numbers, and only then trust an offset you found by reading the engine source.

    MSVC, from an "x86 Native Tools Command Prompt" at the SDK root:
        cl /nologo /W3 code\client\cl_sdkclstate.c /Fe:cl_sdkclstate.exe
        cl_sdkclstate.exe

    GCC / MinGW-w64, 32-bit, from the SDK root:
        gcc -m32 -o cl_sdkclstate code/client/cl_sdkclstate.c && ./cl_sdkclstate

It lives in code/client/ (not in a separate tools/ folder) on purpose. The engine
headers use relative includes like "../game/q_shared.h", and both MSVC and gcc
resolve a quoted include against the directory of the file doing the including.
Sitting next to cl_cgame.c is what makes those paths resolve with no extra -I
flags, exactly as they do for the real engine sources.

WHY THIS IS SPLIT INTO TWO FILES
--------------------------------
Most of the Q3 headers in this SDK have NO include guard - client.h,
cgame/cg_public.h, cgame/cg_local.h, game/bg_public.h and game/g_public.h are all
unguarded. Only q_shared.h, tr_types.h, tr_public.h, qcommon.h, qfiles.h,
ui_public.h and keycodes.h are guarded. The engine gets away with it because no
single translation unit ever mixes the client-side and cgame-side header
families. That rule is load-bearing, so this harness follows it too:

    cl_sdkclstate.c  (this file) -> client.h family: entityState_t,
                                   playerState_t, usercmd_t, clSnapshot_t,
                                   clientActive_t, clientStatic_t, refdef_t,
                                   refEntity_t, glconfig_t
    cl_sdkcgstate.c               -> cgame/cg_local.h family: cg_t, cgs_t,
                                   centity_t, clientInfo_t, weaponInfo_t, and
                                   the cgame/ui/game syscall enums

Including client.h and cgame/cg_local.h in the same .c makes cg_public.h arrive
twice by two different relative paths and the build dies on redefinition. Run
both programs and read the two outputs together.

TWO NAMING TRAPS WORTH KNOWING BEFORE YOU GO LOOKING
----------------------------------------------------
There is no cl_t in the 1.32b source, and no clientSnapshot_t on the client side
(that name is the server's, in code/server/server.h). The client's live state is
the clientActive_t global "cl" and the clientStatic_t global "cls", and its
snapshots are clSnapshot_t in the cl.snapshots[] ring. cg_t also has no
"entities" field - the per-entity array is the separate cg_entities[] global,
which is why sizeof(centity_t) is printed as the array stride in part 2.

Also note where the version macros actually live: Q3_VERSION is in
game/q_shared.h, but PROTOCOL_VERSION (68) is in qcommon/qcommon.h, which this
file reaches through client.h.

This file includes the engine headers but contains no id Software source code.
It only prints facts about GPL code, so it is not itself GPL.
===========================================================================
*/

#include "../client/client.h"	/* pulls in q_shared, qcommon, tr_public,
								 * tr_types, ui_public, cg_public, bg_public */

#include <stdio.h>

#define SIZEOF(t)    printf("  %-24s %5u\n", #t, (unsigned)sizeof(t))
#define OFFSET(t, m) printf("    %-20s %-22s @ %4u\n", #t, #m, \
                             (unsigned)((size_t)&((t *)0)->m))
#define CONST(n)     printf("  %-26s %d\n", #n, (int)(n))

static void section(const char *name)
{
	printf("\n%s\n", name);
	printf("  ------------------------------------------------------\n");
}

int main(void)
{
	/* Fail loudly on a 64-bit build: those numbers do not describe retail
	 * quake3.exe and must never be used as offsets into it. */
	if (sizeof(void *) != 4)
	{
		printf("======================================================\n");
		printf(" WARNING: sizeof(void*) == %u - this is a %d-bit build.\n",
			(unsigned)sizeof(void *), (int)(sizeof(void *) * 8));
		printf(" Retail Quake 3 is 32-bit. The offsets below are WRONG\n");
		printf(" for quake3.exe. Rebuild this from an x86 toolchain.\n");
		printf("======================================================\n");
	}

	printf("kutaQ3 hook SDK layout harness - client / shared / renderer\n");
	printf("  Q3_VERSION       = %s\n", Q3_VERSION);
	printf("  PROTOCOL_VERSION = %d\n", PROTOCOL_VERSION);
	printf("  sizeof(void*)    = %u  (must be 4 to match quake3.exe)\n",
		(unsigned)sizeof(void *));

	section("shared game-state types (game/q_shared.h)");
	SIZEOF(entityState_t);
	SIZEOF(playerState_t);
	SIZEOF(usercmd_t);
	SIZEOF(trajectory_t);
	SIZEOF(vec3_t);
	SIZEOF(vec4_t);

	/* The fields a hook most often walks. Confirm each against the retail
	 * binary before relying on it. */
	OFFSET(entityState_t, number);
	OFFSET(entityState_t, eType);
	OFFSET(entityState_t, eFlags);
	OFFSET(entityState_t, pos);
	OFFSET(entityState_t, apos);
	OFFSET(entityState_t, origin);
	OFFSET(entityState_t, angles);
	OFFSET(entityState_t, modelindex);
	OFFSET(entityState_t, clientNum);
	OFFSET(entityState_t, powerups);
	OFFSET(entityState_t, weapon);
	OFFSET(entityState_t, legsAnim);
	OFFSET(entityState_t, torsoAnim);
	OFFSET(trajectory_t, trType);
	OFFSET(trajectory_t, trTime);
	OFFSET(trajectory_t, trBase);
	OFFSET(trajectory_t, trDelta);
	OFFSET(playerState_t, commandTime);
	OFFSET(playerState_t, pm_type);
	OFFSET(playerState_t, bobCycle);
	OFFSET(playerState_t, origin);
	OFFSET(playerState_t, velocity);
	OFFSET(playerState_t, viewangles);
	OFFSET(playerState_t, weapon);
	OFFSET(playerState_t, stats);
	OFFSET(playerState_t, persistant);
	OFFSET(usercmd_t, serverTime);
	OFFSET(usercmd_t, angles);
	OFFSET(usercmd_t, buttons);
	OFFSET(usercmd_t, weapon);

	section("protocol limits (game/q_shared.h)");
	CONST(MAX_CLIENTS);
	CONST(GENTITYNUM_BITS);
	CONST(MAX_GENTITIES);
	CONST(MAX_CONFIGSTRINGS);
	CONST(MAX_WEAPONS);

	section("renderer ABI (cgame/tr_types.h, renderer/tr_public.h)");
	SIZEOF(refdef_t);
	SIZEOF(refEntity_t);
	SIZEOF(glconfig_t);
	SIZEOF(refimport_t);
	SIZEOF(refexport_t);
	CONST(REF_API_VERSION);
	/* NOTE: in 1.32b refdef_t carries no entity list - entities are submitted
	 * to the renderer one at a time through re.AddRefEntityToScene(). */
	OFFSET(refdef_t, x);
	OFFSET(refdef_t, y);
	OFFSET(refdef_t, width);
	OFFSET(refdef_t, height);
	OFFSET(refdef_t, fov_x);
	OFFSET(refdef_t, fov_y);
	OFFSET(refdef_t, vieworg);
	OFFSET(refdef_t, viewaxis);
	OFFSET(refdef_t, time);
	OFFSET(refdef_t, rdflags);
	OFFSET(refdef_t, areamask);
	OFFSET(refEntity_t, reType);
	OFFSET(refEntity_t, renderfx);
	OFFSET(refEntity_t, hModel);
	OFFSET(refEntity_t, axis);
	OFFSET(refEntity_t, origin);
	OFFSET(refEntity_t, frame);
	OFFSET(refEntity_t, oldorigin);
	OFFSET(refEntity_t, oldframe);
	OFFSET(refEntity_t, backlerp);
	OFFSET(refEntity_t, skinNum);
	OFFSET(refEntity_t, customShader);
	OFFSET(refEntity_t, shaderRGBA);
	OFFSET(refEntity_t, radius);
	OFFSET(glconfig_t, renderer_string);
	OFFSET(glconfig_t, vendor_string);
	OFFSET(glconfig_t, version_string);
	OFFSET(glconfig_t, maxTextureSize);
	OFFSET(glconfig_t, vidWidth);
	OFFSET(glconfig_t, vidHeight);

	section("client state (client/client.h) - the globals are 'cl' and 'cls'");
	SIZEOF(clientActive_t);
	SIZEOF(clientStatic_t);
	SIZEOF(clSnapshot_t);
	SIZEOF(gameState_t);
	/* clientActive_t ("cl") is the per-server-connection state: the snapshot
	 * ring, the entity baseline/parse tables and the outgoing usercmds. */
	OFFSET(clientActive_t, timeoutcount);
	OFFSET(clientActive_t, snap);
	OFFSET(clientActive_t, serverTime);
	OFFSET(clientActive_t, serverTimeDelta);
	OFFSET(clientActive_t, gameState);
	OFFSET(clientActive_t, parseEntitiesNum);
	OFFSET(clientActive_t, cmds);
	OFFSET(clientActive_t, cmdNumber);
	OFFSET(clientActive_t, viewangles);
	OFFSET(clientActive_t, serverId);
	OFFSET(clientActive_t, snapshots);
	OFFSET(clientActive_t, entityBaselines);
	OFFSET(clientActive_t, parseEntities);
	/* clientStatic_t ("cls") is the across-connections state, and holds the
	 * glconfig the cgame module is handed. */
	OFFSET(clientStatic_t, state);
	OFFSET(clientStatic_t, keyCatchers);
	OFFSET(clientStatic_t, rendererStarted);
	OFFSET(clientStatic_t, cgameStarted);
	OFFSET(clientStatic_t, framecount);
	OFFSET(clientStatic_t, frametime);
	OFFSET(clientStatic_t, realtime);
	OFFSET(clientStatic_t, glconfig);
	/* clSnapshot_t is one slot of the cl.snapshots[] ring - the client-side
	 * equivalent of the cgame's snapshot_t. */
	OFFSET(clSnapshot_t, valid);
	OFFSET(clSnapshot_t, snapFlags);
	OFFSET(clSnapshot_t, serverTime);
	OFFSET(clSnapshot_t, ping);
	OFFSET(clSnapshot_t, ps);
	OFFSET(clSnapshot_t, numEntities);
	OFFSET(clSnapshot_t, parseEntitiesNum);
	OFFSET(clSnapshot_t, serverCommandNum);

	printf("\nPart 1 of 2. Run cl_sdkcgstate for cg_t / cgs_t / centity_t.\n");
	return 0;
}
