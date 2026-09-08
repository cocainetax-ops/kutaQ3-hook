/*
===========================================================================
kutaQ3 hook - SDK layout harness, part 2 of 2: cgame module state + module ABI

Read the header comment in cl_sdkclstate.c first - it explains why the harness is
split in two. The short version: client.h and cgame/cg_local.h must never be
included in the same translation unit, because cgame/cg_public.h has no include
guard and would arrive twice by two different relative paths.

This half covers the cgame VM's own world, which is where a client hook does most
of its reading: cg_t (the cgame module's entire per-frame state), cgs_t (the
per-level state), centity_t (the interpolated per-entity records the HUD and
models are drawn from) and the cgame/ui syscall enums that number the calls
crossing the module <-> engine boundary.

    MSVC, from an "x86 Native Tools Command Prompt" at the SDK root:
        cl /nologo /W3 code\client\cl_sdkcgstate.c /Fe:cl_sdkcgstate.exe
        cl_sdkcgstate.exe

    GCC / MinGW-w64, 32-bit, from the SDK root:
        gcc -m32 -o cl_sdkcgstate code/client/cl_sdkcgstate.c && ./cl_sdkcgstate

Same 32-bit caveat as part 1: retail Quake 3 is x86, so a 64-bit build prints
sizes that do not describe quake3.exe. The program says so itself.

FOUR THINGS THAT SURPRISE PEOPLE HERE
-------------------------------------
1. cgameImport_t, cgameExport_t, uiImport_t and uiExport_t are ENUMS, not structs
   of function pointers (and so are gameImport_t / gameExport_t in
   game/g_public.h). They number the syscalls; the actual functions are passed as
   a flat list of arguments to vmMain()/dllEntry(). So they get sizeof() here but
   never offsetof().
2. There is no CGAME_API_VERSION in 1.32b. GAME_API_VERSION is 8,
   UI_API_VERSION is 6 and REF_API_VERSION is 8, but the cgame module has no
   version handshake at all - see vmMain() in cgame/cg_main.c.
3. cg_t has no "entities" member. The per-entity array is the standalone global
   cg_entities[MAX_GENTITIES], so sizeof(centity_t) is its stride.
4. fraglimit / timelimit / maxclients / mapname live in cgs_t, not cg_t. cg_t
   only has fraglimitWarnings and timelimitWarnings.

This file includes the engine headers but contains no id Software source code.
It only prints facts about GPL code, so it is not itself GPL.
===========================================================================
*/

#include "../game/q_shared.h"
#include "../cgame/cg_local.h"	/* brings in bg_public.h, tr_types.h and
								 * cg_public.h along the correct relative paths */
#include "../ui/ui_public.h"
#include "../qcommon/qcommon.h"		/* PROTOCOL_VERSION (guarded: _QCOMMON_H_) */
#include "../renderer/tr_public.h"	/* REF_API_VERSION (guarded: __TR_PUBLIC_H) */

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
	if (sizeof(void *) != 4)
	{
		printf("======================================================\n");
		printf(" WARNING: sizeof(void*) == %u - this is a %d-bit build.\n",
			(unsigned)sizeof(void *), (int)(sizeof(void *) * 8));
		printf(" Retail Quake 3 is 32-bit. The offsets below are WRONG\n");
		printf(" for quake3.exe. Rebuild this from an x86 toolchain.\n");
		printf("======================================================\n");
	}

	printf("kutaQ3 hook SDK layout harness - cgame module state / module ABI\n");
	printf("  Q3_VERSION       = %s\n", Q3_VERSION);
	printf("  PROTOCOL_VERSION = %d\n", PROTOCOL_VERSION);
	printf("  sizeof(void*)    = %u  (must be 4 to match quake3.exe)\n",
		(unsigned)sizeof(void *));

	section("cgame per-frame state (cgame/cg_local.h, global 'cg')");
	SIZEOF(cg_t);
	SIZEOF(cgs_t);
	SIZEOF(cgMedia_t);
	SIZEOF(playerEntity_t);
	SIZEOF(lerpFrame_t);
	SIZEOF(score_t);
	SIZEOF(localEntity_t);
	SIZEOF(markPoly_t);
	OFFSET(cg_t, clientFrame);
	OFFSET(cg_t, clientNum);
	OFFSET(cg_t, demoPlayback);
	OFFSET(cg_t, latestSnapshotNum);
	OFFSET(cg_t, frameInterpolation);
	OFFSET(cg_t, frametime);
	OFFSET(cg_t, time);
	OFFSET(cg_t, oldTime);
	OFFSET(cg_t, physicsTime);
	OFFSET(cg_t, predictedPlayerState);
	OFFSET(cg_t, predictedPlayerEntity);
	OFFSET(cg_t, validPPS);
	OFFSET(cg_t, weaponSelect);
	OFFSET(cg_t, refdef);
	OFFSET(cg_t, refdefViewAngles);
	OFFSET(cg_t, snap);
	OFFSET(cg_t, nextSnap);
	OFFSET(cg_t, crosshairClientNum);
	OFFSET(cg_t, crosshairClientTime);
	OFFSET(cg_t, numScores);
	OFFSET(cg_t, scores);
	OFFSET(cg_t, warmup);
	OFFSET(cg_t, zoomed);
	OFFSET(cg_t, bobcycle);
	OFFSET(cg_t, xyspeed);

	section("cgame per-level state (cgame/cg_local.h, global 'cgs')");
	OFFSET(cgs_t, glconfig);
	OFFSET(cgs_t, gametype);
	OFFSET(cgs_t, dmflags);
	OFFSET(cgs_t, fraglimit);
	OFFSET(cgs_t, capturelimit);
	OFFSET(cgs_t, timelimit);
	OFFSET(cgs_t, maxclients);
	OFFSET(cgs_t, mapname);
	OFFSET(cgs_t, levelStartTime);
	OFFSET(cgs_t, clientinfo);
	OFFSET(cgs_t, serverCommandSequence);
	OFFSET(cgs_t, processedSnapshotNum);

	section("per-entity records (cgame/cg_local.h, global cg_entities[])");
	SIZEOF(centity_t);
	/* cg_entities[] is a standalone MAX_GENTITIES-sized global, not a member of
	 * cg_t, so this is the stride between two entries. */
	printf("  stride: &cg_entities[1]-&cg_entities[0] == sizeof(centity_t) == %u\n",
		(unsigned)sizeof(centity_t));
	printf("  whole array: MAX_GENTITIES(%d) * %u = %u bytes\n",
		MAX_GENTITIES, (unsigned)sizeof(centity_t),
		(unsigned)(MAX_GENTITIES * sizeof(centity_t)));
	OFFSET(centity_t, currentState);
	OFFSET(centity_t, nextState);
	OFFSET(centity_t, interpolate);
	OFFSET(centity_t, currentValid);
	OFFSET(centity_t, muzzleFlashTime);
	OFFSET(centity_t, previousEvent);
	OFFSET(centity_t, trailTime);
	OFFSET(centity_t, miscTime);
	OFFSET(centity_t, snapShotTime);
	OFFSET(centity_t, pe);
	OFFSET(centity_t, errorTime);
	OFFSET(centity_t, errorOrigin);
	OFFSET(centity_t, errorAngles);
	OFFSET(centity_t, extrapolated);
	OFFSET(centity_t, rawOrigin);
	OFFSET(centity_t, rawAngles);
	/* lerpOrigin / lerpAngles are the interpolated transform the model is
	 * actually drawn with - the most useful pair in the whole struct. */
	OFFSET(centity_t, lerpOrigin);
	OFFSET(centity_t, lerpAngles);

	section("player info / weapon info (cgame/cg_local.h)");
	SIZEOF(clientInfo_t);
	SIZEOF(weaponInfo_t);
	SIZEOF(itemInfo_t);
	OFFSET(clientInfo_t, infoValid);
	OFFSET(clientInfo_t, name);
	OFFSET(clientInfo_t, team);
	OFFSET(clientInfo_t, score);
	OFFSET(clientInfo_t, health);
	OFFSET(clientInfo_t, armor);
	OFFSET(clientInfo_t, curWeapon);
	OFFSET(clientInfo_t, powerups);
	OFFSET(clientInfo_t, modelName);
	OFFSET(clientInfo_t, skinName);
	OFFSET(clientInfo_t, legsModel);
	OFFSET(clientInfo_t, torsoModel);
	OFFSET(clientInfo_t, headModel);
	OFFSET(weaponInfo_t, item);
	OFFSET(weaponInfo_t, weaponModel);
	OFFSET(weaponInfo_t, flashModel);
	OFFSET(weaponInfo_t, weaponIcon);
	OFFSET(weaponInfo_t, missileModel);
	OFFSET(weaponInfo_t, missileSound);
	OFFSET(weaponInfo_t, missileRenderfx);
	OFFSET(weaponInfo_t, wiTrailTime);

	section("module ABI (cg_public.h / ui_public.h / tr_public.h)");
	/* These are ENUMS of syscall numbers - see the header comment above. */
	SIZEOF(cgameImport_t);
	SIZEOF(cgameExport_t);
	SIZEOF(uiImport_t);
	SIZEOF(uiExport_t);
	CONST(UI_API_VERSION);
	CONST(REF_API_VERSION);
	CONST(CG_INIT);
	CONST(CG_SHUTDOWN);
	CONST(CG_CONSOLE_COMMAND);
	CONST(CG_DRAW_ACTIVE_FRAME);
	CONST(CG_CROSSHAIR_PLAYER);
	CONST(CG_LAST_ATTACKER);
	CONST(CG_KEY_EVENT);
	CONST(CG_MOUSE_EVENT);
	CONST(CG_EVENT_HANDLING);
	/* The two cgame syscalls a client hook is most likely to care about:
	 * CG_DRAW_ACTIVE_FRAME is the module's once-per-frame entry point, and
	 * CG_CROSSHAIR_PLAYER is what the engine asks to identify the entity under
	 * the crosshair. */
	printf("  CG_DRAW_ACTIVE_FRAME == %d is the cgame's per-frame entry\n",
		(int)CG_DRAW_ACTIVE_FRAME);

	section("renderer ABI, cgame side (cgame/tr_types.h)");
	SIZEOF(glconfig_t);
	SIZEOF(refdef_t);
	SIZEOF(refEntity_t);
	OFFSET(glconfig_t, renderer_string);
	OFFSET(glconfig_t, maxTextureSize);
	OFFSET(glconfig_t, vidWidth);
	OFFSET(glconfig_t, vidHeight);
	/* cg.refdef is the refdef_t the cgame fills in and hands to the renderer
	 * each frame; vieworg + viewaxis + fov_x/fov_y are everything needed to
	 * project a world point into screen space. */
	OFFSET(refdef_t, x);
	OFFSET(refdef_t, y);
	OFFSET(refdef_t, width);
	OFFSET(refdef_t, height);
	OFFSET(refdef_t, fov_x);
	OFFSET(refdef_t, fov_y);
	OFFSET(refdef_t, vieworg);
	OFFSET(refdef_t, viewaxis);
	OFFSET(refdef_t, time);
	OFFSET(refEntity_t, reType);
	OFFSET(refEntity_t, hModel);
	OFFSET(refEntity_t, origin);
	OFFSET(refEntity_t, frame);
	OFFSET(refEntity_t, shaderRGBA);

	printf("\nPart 2 of 2. Run cl_sdkclstate for entityState_t / clientActive_t.\n");
	return 0;
}
