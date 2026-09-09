#pragma once

// =============================================================================================== //
// kutaQ3 hook - NAME ESP
//
// Draws every other player's name on the screen, above their head, through walls - using the
// GL::Font display-list text renderer in glText.h / glText.cpp, from inside the hooked
// SwapBuffers in main.cpp (the only place the GL context is current and the frame is complete).
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
// segment by address.
//
// Without a refdef - no VM hook, or a captured one that fails the check - the view is rebuilt from
// what the traps expose:
//
//   - angles: the newest usercmd (CG_GETUSERCMD) plus playerState_t::delta_angles - the same
//     SHORT2ANGLE(cmd->angles[i] + ps->delta_angles[i]) the engine's PM_UpdateViewAngles() does -
//     falling back to the snapshot's own viewangles;
//   - origin: playerState_t::origin of the newest snapshot, pushed forward by velocity for the age
//     of that snapshot (clamped to 250 ms). Client side prediction is not re-implemented, so this
//     is accurate to a few units rather than exact;
//   - fov_x: the cg_fov cvar, clamped the way CG_DrawActiveFrame() clamps it;
//   - fov_y and the screen rect: derived from the GL viewport at draw time.
//
// Other players' positions are smoothed the same way the engine interpolates entities: the
// previous snapshot's sample is kept per client and the resulting velocity carries the tag forward
// to the current frame, so tags do not sit a snapshot behind a moving player.
//
// Split into two halves on purpose: everything above (Gather and the maths) lives in
// nameEspCore.cpp, which needs no <windows.h> and no GL, so tests/ can compile and run that exact
// code against a fabricated snapshot. nameEsp.cpp holds only the GL / GL::Font half.
// =============================================================================================== //

#include "q3sdk.h"

namespace NameEsp
{
	// clientinfo's "\t\" value, mapped to the palette below. Unknown / missing -> TeamFree.
	enum Team
	{
		TeamFree = 0,
		TeamRed,
		TeamBlue,
		TeamSpectator,
		TeamCount
	};

	// One name tag, in world space. origin is the anchor the text is centred on: the player's
	// smoothed position plus q3::kPlayerTagHeight, i.e. just above the head.
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
		View      view;
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
	bool Gather(int serverTime, q3::syscall_t syscall, const q3::refdef_t* refdef = NULL);

	// Drop the frame and the per-client smoothing history: the cgame shut down or was unloaded, so
	// the last gathered data is about a level that no longer exists.
	void Reset();

	// Project a world position into the overlay's screen space. Returns false when there is no
	// usable view / viewport or the point is behind the viewer.
	bool ProjectWorldToScreen(const View& view, const Viewport& vp, const float world[3], ScreenPoint& out);

	// Palette for a Team value (Team out of range -> the free-for-all colour).
	void TeamColor(int team, unsigned char rgb[3]);

	// Parses the clientinfo configstring ("\n\Player\t\red\model\sarge\...") into a tag.
	// Returns false when the configstring carries no usable name. Exposed for the tests; Gather()
	// uses it internally.
	bool ParseClientInfo(const char* infoString, int clientNum, PlayerTag& out);

	// Draw() - the GL half, in nameEsp.cpp. Called from the hooked SwapBuffers every frame.
	void Draw();
}
