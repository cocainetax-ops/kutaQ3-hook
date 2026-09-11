#pragma once

// =============================================================================================== //
// kutaQ3 hook - NAME ESP
//
// Draws every other player's name on the screen, above their head, through walls - using the
// GL::Font display-list text renderer in glText.h / glText.cpp, from inside the hooked
// SwapBuffers in main.cpp (the only place the GL context is current and the frame is complete).
// Up close, where aiming up/down pushes the above-the-head anchor off the screen while the
// player is still visible, the tag re-anchors to the chest instead of sliding to the edge.
//
// Where the data comes from
// -------------------------
// Not from guessed offsets in quake3.exe, and not from a native cgame DLL either. vmHook.h detours
// the engine's per-VM syscall dispatcher, which every cgame trap passes through whether the cgame
// is the bytecode in pak0.pk3 (the retail default) or a DLL:
//
//   1. the dispatcher detour watches CG_GETSNAPSHOT / CG_GETGAMESTATE / CG_R_RENDERSCENE go past
//      and copies out the player entity positions, the address of the cgame's configstrings and
//      the refdef the frame was rendered with;
//   2. Gather() below is called from the hooked SwapBuffers with a trampoline that answers those
//      same trap numbers out of the copies, so it never has to be inside a VM call - which is what
//      used to tie this feature to vmMain, and with it to vm_cgame;
//   3. Draw() runs in the same frame, right after, and renders the tags.
//
// The view
// --------
// Preferred source is the refdef_t the cgame handed the renderer (CG_R_RENDERSCENE), captured by
// vmHook.cpp: the exact vieworg, viewaxis and fov_x the frame was drawn with. It is only used once
// it passes a shape check (see RefdefUsable below), because it is read out of the cgame's data
// segment by address - and a freshness check (see kRefdefStaleMs), because a refdef older than
// the snapshot is a frozen camera, and projecting through it pins every tag to the edge.
//
// Without a refdef - no VM hook, or a captured one that fails the check - the view is rebuilt from
// what the traps expose:
//
//   - angles: the newest usercmd (CG_GETUSERCMD) plus playerState_t::delta_angles - the same
//     SHORT2ANGLE(cmd->angles[i] + ps->delta_angles[i]) the engine's PM_UpdateViewAngles() does,
//     including its frozen view while dead-and-playing or in intermission and its +/-87.9 degree
//     pitch clamp - falling back to the snapshot's own viewangles;
//   - origin: playerState_t::origin of the newest snapshot, pushed forward by velocity for the age
//     of that snapshot (clamped to 250 ms). Client side prediction is not re-implemented, so this
//     is accurate to a few units rather than exact;
//   - fov_x: the cg_fov cvar, clamped the way CG_DrawActiveFrame() clamps it;
//   - fov_y and the screen rect: derived from the GL viewport at draw time.
//
// Other players are positioned exactly where the renderer puts their models: the VM hook keeps
// the previous server snapshot as well as the newest one, and Gather() lerps each player's
// position between the two at the refdef's cg.time - the same interpolation
// CG_InterpolateEntityPosition() in cg_ents.c does (frameInterpolation clamped to 0..1). The
// engine never velocity-extrapolates players, and neither does this: while waiting for the next
// snapshot the tag holds on the newest server position, exactly like the model. A player missing
// from the previous snapshot (just entered the PVS / respawned) or carrying a toggled
// EF_TELEPORT_BIT is placed at the newest sample without lerping, matching CG_ResetEntity().
//
// Split into two halves on purpose: everything above (Gather and the maths) lives in
// nameEspCore.cpp, which needs no <windows.h> and no GL, so tests/ can compile and run that exact
// code against a fabricated snapshot. nameEsp.cpp holds only the GL / GL::Font half.
// =============================================================================================== //

#include "q3sdk.h"

namespace NameEsp
{
	// A captured refdef older than the snapshot it would be projected against is not the view
	// the frame was rendered with - R_RenderScene stopped arriving while snapshots kept
	// flowing - and projecting through it pins every tag to the edge (a frozen camera). Past
	// this many milliseconds of staleness BuildView rejects the refdef and the rebuilt view
	// takes over, and Vm::ServerTime stops taking the frame time from it. Small negatives are
	// legitimate and must NOT trip this: the cgame reads the NEXT snapshot ahead for
	// interpolation, so the newest captured snapshot can be a server frame newer than the
	// rendered one (tens of ms on a local game).
	const int kRefdefStaleMs = 500;

	// clientinfo's "\t\" value, mapped to the palette below. Stock 1.32 sends it numeric
	// (CG_NewClientInfo: atoi), with the same numbering as team_t - 0 free, 1 red, 2 blue,
	// 3 spectator - so the enum matches on purpose. Unknown / missing -> TeamFree.
	enum Team
	{
		TeamFree = 0,
		TeamRed,
		TeamBlue,
		TeamSpectator,
		TeamCount
	};

	// One name tag, in world space. origin is the anchor the text is centred on: the player's
	// smoothed position plus q3::kPlayerTagHeight, i.e. just above the head - or the raw snapshot
	// position when the smoothing overshot to behind the viewer (see Gather).
	struct PlayerTag
	{
		char  name[64];       // Q3 "^1" colour codes already stripped
		float origin[3];
		int   clientNum;
		int   team;           // Team
	};

	// The view the tags are projected with (see the header comment for how it is rebuilt).
	struct View
	{
		bool  valid;
		float origin[3];
		q3::vec3_t axis[3];   // AnglesToAxis(): [0] forward, [1] LEFT, [2] up
		float fovX;           // degrees, horizontal
	};

	struct Viewport
	{
		int x, y, width, height;
	};

	// Screen space of the ortho overlay GL::SetupOrtho() installs: x to the right, y DOWN from the
	// top of the viewport. inView is false when the point had to be clamped to the viewport edge.
	struct ScreenPoint
	{
		float x, y;
		bool  inView;
	};

	struct Frame
	{
		bool      valid;
		int       serverTime;    // cl.serverTime passed to CG_DRAW_ACTIVE_FRAME
		int       snapshotTime;  // the snapshot the tags were built from
		int       numEntities;   // entities in that snapshot before any filtering (self, the dead
		                         // and non-players all still counted). The menu shows it when zero
		                         // tags were built, which separates "the snapshot carries only the
		                         // viewer" (solo map, no bots, PVS) from "entities are there but
		                         // every name was rejected" (configstrings).
		// diagnostic counters for the menu's "why no names?" report. playerEntities counts
		// every ET_PLAYER entity in the newest snapshot before any filter (the server never
		// sends the local viewer's own entity, so on a healthy connection it is the count of
		// alive players currently in this client's potentially-visible set). The skip counters
		// explain what happened to each one. All are zeroed by Gather() every frame.
		int       selfClientNum;       // snapshot ps.clientNum
		int       selfPmType;          // snapshot ps.pm_type (0 normal, 2 spectator, ...)
		int       selfHealth;          // snapshot ps.stats[STAT_HEALTH]
		int       playerEntities;      // ET_PLAYER entities in the newest snapshot
		int       skippedSelf;         // ET_PLAYER entities whose clientNum is the viewer
		int       skippedDead;         // corpses: eFlags & EF_DEAD
		int       skippedBadSlot;      // clientNum outside [0, kMaxClients)
		int       skippedNoInfo;       // CS_PLAYERS configstring empty / no usable name
		int       interpolatedPlayers; // tags placed by lerping the previous and newest snapshot

		View      view;
		bool      usedRefdef;    // the view above is the cgame's captured refdef (else rebuilt)
		int       playerCount;
		PlayerTag players[q3::kMaxClients];
	};

	// The frame Gather() last produced. Never NULL; valid == false when there is nothing to draw
	// (feature off, no cgame attached, not connected, no snapshot yet).
	const Frame& Current();

	// Called from the hooked SwapBuffers, on the game thread, just before Draw(). syscall is
	// vmHook.cpp's trampoline (or NULL, which gathers nothing); refdef is the view the cgame
	// rendered this frame when the VM hook captured one, else NULL.
	// Returns false (and leaves Current() invalid) when there is nothing usable to read.
	// Keep only a usable world camera. HUD/model scenes leave captured unchanged.
	// Shared with the VM dispatcher so capture ordering is testable without Windows.
	bool CaptureWorldRefdef(const q3::refdef_t& candidate, q3::refdef_t& captured);

	bool Gather(int serverTime, q3::syscall_t syscall, const q3::refdef_t* refdef = NULL);

	// Drop the frame and the per-client smoothing history: the cgame shut down or was unloaded, so
	// the last gathered data is about a level that no longer exists.
	void Reset();

	// Drop the GL half's per-client state (the fade-in ramp and which anchor each tag is using).
	// Called from vmHook.cpp's level-boundary drop, alongside Reset(): every client number now
	// refers to a different player in a different map, so a ramp that was already finished must
	// not carry over to them. Lives in nameEsp.cpp because it is GL-side state, and the portable
	// half (nameEspCore.cpp) has no GL to talk to.
	void ResetDrawState();

	// Project a world position into the overlay's screen space. Returns false when there is no
	// usable view / viewport or the point is behind the viewer.
	bool ProjectWorldToScreen(const View& view, const Viewport& vp, const float world[3], ScreenPoint& out);

	// Palette for a Team value (Team out of range -> the free-for-all colour).
	void TeamColor(int team, unsigned char rgb[3]);

	// Parses the clientinfo configstring ("\n\Player\t\1\model\sarge\...") into a tag.
	// Returns false when the configstring carries no usable name. The name comes out stripped of
	// Q3 "^1" colour codes and sanitised to printable ASCII (32..126, anything else becomes '?'),
	// because the GL::Font display lists only hold glyphs 32..127 and glCallLists() with an
	// out-of-range byte is undefined behaviour. Exposed for the tests; Gather() uses it internally.
	bool ParseClientInfo(const char* infoString, int clientNum, PlayerTag& out);

	// What Draw() did with the frame it was given: how many tags were drawn at their unclamped
	// position, how many were clamped to the edge, and how many were skipped as behind the
	// viewer. Read by the menu; it separates "tags gather but project off screen" from "tags are
	// behind the viewer" without guessing.
	struct DrawStats
	{
		int drawn;    // tags issued to GL::Font (shadow + text each)
		int inView;   // ... drawn unclamped, full brightness
		int edge;     // ... clamped to the viewport edge, dimmed
		int behind;   // skipped as behind the viewer
	};

	// The stats for the last Draw() call - zeros when that call drew nothing.
	const DrawStats& LastDrawStats();

	// Draw() - the GL half, in nameEsp.cpp. Called from the hooked SwapBuffers every frame.
	void Draw();
}
