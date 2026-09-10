// =============================================================================================== //
// kutaQ3 hook - NAME ESP, portable half (see nameEsp.h for the overview)
//
// No <windows.h>, no GL, no Detours in here: this file is compiled into kutaQ3.dll AND into
// tests/test_nameesp.cpp, which drives Gather() with a fake engine syscall trampoline and checks
// what comes out. Keeping the two halves apart is what makes the feature testable off Windows.
// =============================================================================================== //

#include "nameEsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace
{
	// The engine fills these in; they are big (snapshot_t alone is ~52 KB, gameState_t ~20 KB), so
	// they are static and never put on the stack.
	q3::snapshot_t   s_snapshot;
	q3::gameState_t  s_gameState;

	NameEsp::Frame   s_frame;

	// Previous snapshot sample per client, used to carry a moving player's tag forward to the
	// current frame (see SmoothOrigin). Invalidated by NameEsp::Reset().
	struct Track
	{
		bool  valid;
		int   time;
		float origin[3];
	};
	Track s_track[q3::kMaxClients];

	// ---- limits that keep a stale or bogus sample from throwing a tag across the map -------------
	const int   kMaxSnapshotAgeMs = 250;    // beyond this a snapshot is treated as "where it says"
	const float kMaxExtrapolatedStep = 200.0f;  // units; ~2x what a player covers in 250 ms
	const float kMaxTrackedSpeed = 2000.0f;     // units/s; anything faster is a teleport, not motion

	int ClampAge(int serverTime, int snapshotTime)
	{
		int age = serverTime - snapshotTime;
		if (age < 0)
			return 0;
		if (age > kMaxSnapshotAgeMs)
			return kMaxSnapshotAgeMs;
		return age;
	}

	float ClampStep(float v)
	{
		if (v > kMaxExtrapolatedStep)  return  kMaxExtrapolatedStep;
		if (v < -kMaxExtrapolatedStep) return -kMaxExtrapolatedStep;
		return v;
	}

	bool SameNoCase(const char* a, const char* b)
	{
		for (;; ++a, ++b)
		{
			char ca = *a, cb = *b;
			if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
			if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
			if (ca != cb)
				return false;
			if (ca == 0)
				return true;
		}
	}

	void TrimSpaces(char* s)
	{
		if (!s)
			return;
		char* start = s;
		while (*start == ' ')
			++start;
		if (start != s)
			memmove(s, start, strlen(start) + 1);
		size_t len = strlen(s);
		while (len > 0 && s[len - 1] == ' ')
			s[--len] = 0;
	}

	// --------------------------------------------------------------------------------------------
	// cg_fov, the way CG_DrawActiveFrame() reads and clamps it. Falls back to 90 when the cvar
	// cannot be read, which is also the engine's own default.
	// --------------------------------------------------------------------------------------------
	float ReadFovX(q3::syscall_t syscall)
	{
		const float kDefaultFov = 90.0f;
		if (!syscall)
			return kDefaultFov;

		char buf[32] = { 0 };
		syscall(q3::CG_CVAR_VARIABLESTRINGBUFFER,
		        (intptr_t)"cg_fov", (intptr_t)buf, (intptr_t)sizeof(buf));
		if (buf[0] == 0)
			return kDefaultFov;

		char* end = NULL;
		const double v = strtod(buf, &end);
		if (end == buf)
			return kDefaultFov;
		if (v < 1.0)   return 1.0f;
		if (v > 179.0) return 179.0f;
		return (float)v;
	}

	// --------------------------------------------------------------------------------------------
	// Is a captured refdef_t one we can project with? It is read out of the cgame's data segment by
	// address (vmHook.cpp), so it is checked rather than trusted - and the handedness test is what
	// catches a wrong viewaxis convention, not just garbage:
	//
	//   - fov the way CG_DrawActiveFrame() clamps cg_fov, and a sane screen rectangle;
	//   - every viewaxis row a unit vector (AnglesToAxis() normalises them);
	//   - the triple right handed: cross(viewaxis[0], viewaxis[1]) == +viewaxis[2], measured to be
	//     +1.000 for the engine's own AnglesToAxis(). It comes out that way because AngleVectors()
	//     reports "right" as (0,-1,0) at zero angles and AnglesToAxis() negates it, so viewaxis[1]
	//     is the world LEFT vector - which is what the projection below assumes when it flips the
	//     sign to make screen x grow to the right. A renderer that handed over the un-negated right
	//     vector would measure -1 here and mirror every tag, so it is rejected rather than trusted.
	// --------------------------------------------------------------------------------------------
	bool RefdefUsable(const q3::refdef_t& rd)
	{
		if (rd.fov_x < 1.0f || rd.fov_x > 179.0f)
			return false;
		if (rd.width <= 0 || rd.height <= 0 || rd.width > 8192 || rd.height > 8192)
			return false;

		for (int i = 0; i < 3; ++i)
		{
			const float* a = rd.viewaxis[i];
			const float lengthSq = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
			if (!(lengthSq > 0.96f && lengthSq < 1.04f))     // also rejects NaN, which fails every compare
				return false;
		}

		const float* f = rd.viewaxis[0];
		const float* l = rd.viewaxis[1];
		const float* u = rd.viewaxis[2];
		const float cx = f[1] * l[2] - f[2] * l[1];
		const float cy = f[2] * l[0] - f[0] * l[2];
		const float cz = f[0] * l[1] - f[1] * l[0];
		const float det = cx * u[0] + cy * u[1] + cz * u[2];
		if (!(det > 0.96f && det < 1.04f))
			return false;

		for (int i = 0; i < 3; ++i)
		{
			if (!(rd.vieworg[i] > -1e7f && rd.vieworg[i] < 1e7f))
				return false;
		}
		return true;
	}

	// --------------------------------------------------------------------------------------------
	// The view. See the "The view" block in nameEsp.h for why each piece comes from where.
	// --------------------------------------------------------------------------------------------
	void BuildView(int serverTime, const q3::snapshot_t& snap, q3::syscall_t syscall,
	               const q3::refdef_t* refdef, NameEsp::View& view, bool& usedRefdef)
	{
		const q3::playerState_t& ps = snap.ps;
		memset(&view, 0, sizeof(view));
		usedRefdef = false;

		// ---- the view the cgame actually rendered, when the VM hook captured it ----------------
		if (refdef && RefdefUsable(*refdef))
		{
			for (int i = 0; i < 3; ++i)
			{
				view.origin[i] = refdef->vieworg[i];
				view.axis[i][0] = refdef->viewaxis[i][0];
				view.axis[i][1] = refdef->viewaxis[i][1];
				view.axis[i][2] = refdef->viewaxis[i][2];
			}
			view.fovX  = refdef->fov_x;
			view.valid = true;
			usedRefdef = true;
			return;
		}

		// ---- view angles: newest usercmd + delta_angles, i.e. PM_UpdateViewAngles() -------------
		// This mirrors SDK/code/game/bg_pmove.c PM_UpdateViewAngles(), all of it, not just the
		// steady-state formula - the branches matter as much as the maths:
		//
		//   - during either intermission, or while dead (health <= 0) and playing (anything but
		//     PM_SPECTATOR), the engine returns without touching the viewangles: the view is frozen
		//     where death left it while the mouse keeps moving. The snapshot already carries that
		//     frozen value, so it is used as-is instead of rebuilding from the usercmd - rebuilding
		//     would follow the mouse and swing every tag across the screen while dead;
		//   - otherwise viewangles[i] = SHORT2ANGLE((short)(cmd->angles[i] + ps->delta_angles[i])),
		//     with pitch (index 0) clamped to +/-16000 shorts (+/-87.9 degrees). The (short)
		//     truncation only ever changes the sum by multiples of 65536 - whole turns - so it keeps
		//     a spawn-sized delta_angles honest rather than introducing error. The engine also writes
		//     the clamped pitch back into ps->delta_angles; there is nothing to write back to here,
		//     the value is recomputed every frame.
		//
		// Rebuilding it this way reproduces the direction the client actually rendered this frame.
		// Falls back to the snapshot's own angles when no usercmd is available.
		float angles[3];
		bool  haveAngles = false;
		const bool frozen = (ps.pm_type == q3::kPmIntermission ||
		                     ps.pm_type == q3::kPmSpIntermission ||
		                     (ps.pm_type != q3::kPmSpectator && ps.stats[q3::kStatHealth] <= 0));
		if (syscall && !frozen)
		{
			const int cmdNumber = (int)syscall(q3::CG_GETCURRENTCMDNUMBER);
			q3::usercmd_t cmd;
			memset(&cmd, 0, sizeof(cmd));
			if (syscall(q3::CG_GETUSERCMD, (intptr_t)cmdNumber, (intptr_t)&cmd, (intptr_t)sizeof(cmd)))
			{
				for (int i = 0; i < 3; ++i)
				{
					short temp = (short)(cmd.angles[i] + ps.delta_angles[i]);
					if (i == 0 && temp > q3::kMaxViewPitchShort)         // PITCH
						temp = (short)q3::kMaxViewPitchShort;
					else if (i == 0 && temp < -q3::kMaxViewPitchShort)
						temp = (short)-q3::kMaxViewPitchShort;
					angles[i] = q3::ShortToAngle(temp);
				}
				haveAngles = true;
			}
		}
		if (!haveAngles)
		{
			for (int i = 0; i < 3; ++i)
				angles[i] = ps.viewangles[i];
		}

		// ---- view origin: snapshot origin, pushed forward by the age of that snapshot -----------
		const float age = (float)ClampAge(serverTime, snap.serverTime) * 0.001f;
		for (int i = 0; i < 3; ++i)
			view.origin[i] = ps.origin[i] + ClampStep(ps.velocity[i] * age);

		const int viewheight = (ps.viewheight > 0) ? ps.viewheight : q3::kDefaultViewHeight;
		view.origin[2] += (float)viewheight;

		q3::AnglesToAxis(angles, view.axis);
		view.fovX = ReadFovX(syscall);
		view.valid = true;
	}

	// --------------------------------------------------------------------------------------------
	// A snapshot says where a player WAS at snapshot.serverTime. The engine interpolates remote
	// players between two snapshots for the same reason; with only the newest one in hand the next
	// best thing is the velocity implied by the previous sample, carried forward to this frame.
	// --------------------------------------------------------------------------------------------
	void SmoothOrigin(int clientNum, const float* raw, int snapshotTime, int serverTime, float* out)
	{
		const Track& prev = s_track[clientNum];
		float velocity[3] = { 0.0f, 0.0f, 0.0f };
		bool  haveVelocity = false;

		const int dtMs = snapshotTime - prev.time;
		if (prev.valid && dtMs > 0 && dtMs <= kMaxSnapshotAgeMs)
		{
			const float dt = (float)dtMs * 0.001f;
			float speedSq = 0.0f;
			for (int i = 0; i < 3; ++i)
			{
				velocity[i] = (raw[i] - prev.origin[i]) / dt;
				speedSq += velocity[i] * velocity[i];
			}
			// a jump this big is a teleport or a re-entry into the PVS, not motion
			haveVelocity = (speedSq <= kMaxTrackedSpeed * kMaxTrackedSpeed);
		}

		s_track[clientNum].valid = true;
		s_track[clientNum].time  = snapshotTime;
		s_track[clientNum].origin[0] = raw[0];
		s_track[clientNum].origin[1] = raw[1];
		s_track[clientNum].origin[2] = raw[2];

		if (!haveVelocity)
		{
			out[0] = raw[0];
			out[1] = raw[1];
			out[2] = raw[2];
			return;
		}

		const float age = (float)ClampAge(serverTime, snapshotTime) * 0.001f;
		for (int i = 0; i < 3; ++i)
			out[i] = raw[i] + ClampStep(velocity[i] * age);
	}

	// Depth of a world point along the view direction - the same near-plane test
	// ProjectWorldToScreen() rejects points with. Gather() uses it to notice when the smoothing
	// carried a tag to behind the viewer (a fast player running at the camera on an old snapshot)
	// while the raw snapshot position is still in front, and keeps the raw one then instead of
	// hiding a visible player.
	bool IsBehindView(const NameEsp::View& view, const float world[3])
	{
		if (!view.valid || !world)
			return true;
		const float dx = world[0] - view.origin[0];
		const float dy = world[1] - view.origin[1];
		const float dz = world[2] - view.origin[2];
		const float z = dx * view.axis[0][0] + dy * view.axis[0][1] + dz * view.axis[0][2];
		return z <= 4.0f;
	}
}

// =============================================================================================== //

const NameEsp::Frame& NameEsp::Current()
{
	return s_frame;
}

void NameEsp::Reset()
{
	memset(&s_frame, 0, sizeof(s_frame));
	memset(&s_track, 0, sizeof(s_track));
}

bool NameEsp::ParseClientInfo(const char* infoString, int clientNum, PlayerTag& out)
{
	memset(&out, 0, sizeof(out));
	out.clientNum = clientNum;
	out.team      = TeamFree;

	if (!infoString || !infoString[0])
		return false;

	// "\n\" is the name key in the CS_PLAYERS configstring (cg_players.c CG_NewClientInfo, which is
	// not part of SDK/). "name" is only a belt-and-braces fallback for mods that spell it out.
	char raw[128];
	raw[0] = 0;
	if (!q3::InfoValueForKey(infoString, "n", raw, sizeof(raw)))
		q3::InfoValueForKey(infoString, "name", raw, sizeof(raw));

	if (q3::StripColorCodes(raw, out.name, sizeof(out.name)) == 0)
		return false;
	TrimSpaces(out.name);
	if (out.name[0] == 0)
		return false;

	// The GL::Font display lists only hold glyphs 32..127, and glCallLists() with an out-of-range
	// byte is undefined behaviour - while player names do carry high bytes (umlauts and the like).
	// '?' keeps the tag, and with it the player's position, where stripping could empty the name
	// and hide them.
	for (char* p = out.name; *p; ++p)
	{
		const unsigned char c = (unsigned char)*p;
		if (c < 32 || c > 126)
			*p = '?';
	}

	// "\t\" is the team. In 1.32 it is NUMERIC (cg_players.c CG_NewClientInfo does
	// newInfo.team = atoi(Info_ValueForKey(configstring, "t"))): 0 free, 1 red, 2 blue,
	// 3 spectator - the same numbering as team_t and as the Team enum below, so a numeric
	// value maps across directly. String names ("red" / "blue" / ...) are only a fallback
	// for mods that spell the team out. Anything unrecognised keeps the free-for-all
	// colour, so a surprise here costs a colour and nothing else.
	char team[32];
	if (q3::InfoValueForKey(infoString, "t", team, sizeof(team)))
	{
		TrimSpaces(team);
		char* end = NULL;
		const long n = strtol(team, &end, 10);
		if (end != team && end && *end == 0 && n >= TeamFree && n < TeamCount)
			out.team = (int)n;                            // stock 1.32: "0".."3"
		else if (SameNoCase(team, "red"))            out.team = TeamRed;
		else if (SameNoCase(team, "blue"))           out.team = TeamBlue;
		else if (SameNoCase(team, "spectator"))      out.team = TeamSpectator;
		else if (SameNoCase(team, "free"))           out.team = TeamFree;
	}

	return true;
}

bool NameEsp::Gather(int serverTime, q3::syscall_t syscall, const q3::refdef_t* refdef)
{
	s_frame.valid       = false;
	s_frame.playerCount = 0;
	s_frame.numEntities = 0;
	s_frame.view.valid  = false;
	s_frame.usedRefdef  = false;

	if (!syscall)
		return false;

	// ---- the state the trampoline will hand over ------------------------------------------------
	// Same trap numbers and arguments the cgame itself uses. What answers them is vmHook.cpp's
	// bridge, which serves the copies its dispatcher detour took during this frame - so the
	// pointers below are plain host pointers and this can run anywhere, not only inside a VM call.
	int snapNumber = 0;
	int snapServerTime = 0;
	syscall(q3::CG_GETCURRENTSNAPSHOTNUMBER, (intptr_t)&snapNumber, (intptr_t)&snapServerTime);

	if (!syscall(q3::CG_GETSNAPSHOT, (intptr_t)snapNumber,
	             (intptr_t)&s_snapshot, (intptr_t)sizeof(s_snapshot)))
		return false;                                   // not connected / snapshot not valid yet

	// Zeroed first: the bridge leaves the destination untouched when the gameState is not live
	// (mid-load), and without this the tags would be named from the previous level's clientinfo.
	// A zeroed gameState reads as empty configstrings, which ParseClientInfo rejects - so a failed
	// read yields no tags instead of wrong ones.
	memset(&s_gameState, 0, sizeof(s_gameState));
	syscall(q3::CG_GETGAMESTATE, (intptr_t)&s_gameState);

	s_frame.serverTime   = serverTime;
	s_frame.snapshotTime = s_snapshot.serverTime;

	BuildView(serverTime, s_snapshot, syscall, refdef, s_frame.view, s_frame.usedRefdef);

	// ---- one tag per live player entity ---------------------------------------------------------
	const int self = s_snapshot.ps.clientNum;
	int numEntities = s_snapshot.numEntities;
	if (numEntities < 0)
		numEntities = 0;
	if (numEntities > q3::kMaxEntitiesInSnapshot)
		numEntities = q3::kMaxEntitiesInSnapshot;
	s_frame.numEntities = numEntities;

	for (int i = 0; i < numEntities && s_frame.playerCount < q3::kMaxClients; ++i)
	{
		const q3::entityState_t& e = s_snapshot.entities[i];

		if (e.eType != q3::kEtPlayer)
			continue;
		if (e.eFlags & q3::kEfDead)
			continue;                                   // corpse on the floor

		const int clientNum = e.clientNum;
		if (clientNum < 0 || clientNum >= q3::kMaxClients)
			continue;
		if (clientNum == self)
			continue;                                   // your own name, in your own face

		const char* info = q3::ConfigString(&s_gameState, q3::kCsPlayers + clientNum);

		PlayerTag& tag = s_frame.players[s_frame.playerCount];
		if (!ParseClientInfo(info, clientNum, tag))
			continue;                                   // slot is empty / no name yet

		float smoothed[3];
		SmoothOrigin(clientNum, e.pos.trBase, s_snapshot.serverTime, serverTime, smoothed);

		tag.origin[0] = smoothed[0];
		tag.origin[1] = smoothed[1];
		tag.origin[2] = smoothed[2] + q3::kPlayerTagHeight;   // just above the 32 unit player bbox

		// The extrapolation above can overshoot to behind the viewer - a fast player running at
		// the camera on an old snapshot - while the snapshot position itself is still in front.
		// Draw() would skip that tag as behind-the-viewer and a visible player would lose their
		// name, so fall back to the raw snapshot position (at most a snapshot behind, instead of
		// nothing at all).
		if (IsBehindView(s_frame.view, tag.origin))
		{
			float rawAnchor[3];
			rawAnchor[0] = e.pos.trBase[0];
			rawAnchor[1] = e.pos.trBase[1];
			rawAnchor[2] = e.pos.trBase[2] + q3::kPlayerTagHeight;
			if (!IsBehindView(s_frame.view, rawAnchor))
			{
				tag.origin[0] = rawAnchor[0];
				tag.origin[1] = rawAnchor[1];
				tag.origin[2] = rawAnchor[2];
			}
		}

		++s_frame.playerCount;
	}

	s_frame.valid = s_frame.view.valid;
	return s_frame.valid;
}

bool NameEsp::ProjectWorldToScreen(const View& view, const Viewport& vp, const float world[3], ScreenPoint& out)
{
	out.x = 0.0f;
	out.y = 0.0f;
	out.inView = false;

	if (!view.valid || !world || vp.width <= 0 || vp.height <= 0)
		return false;

	const float dx = world[0] - view.origin[0];
	const float dy = world[1] - view.origin[1];
	const float dz = world[2] - view.origin[2];

	const float* fwd = view.axis[0];
	const float* up  = view.axis[2];

	// depth along the view direction; anything at or behind the near plane has no screen position
	const float z = dx * fwd[0] + dy * fwd[1] + dz * fwd[2];
	if (z <= 4.0f)
		return false;

	// q3::AnglesToAxis() puts LEFT in axis[1]; screen x grows to the right
	const float x = -(dx * view.axis[1][0] + dy * view.axis[1][1] + dz * view.axis[1][2]);
	const float y =   dx * up[0]           + dy * up[1]           + dz * up[2];

	// glFrustum's half angles: fov_x is given, fov_y follows from the viewport aspect ratio the
	// same way the renderer derives it (x = width / tan(half_fov_x); half_fov_y = atan(height / x)).
	const float kDeg2Rad = 3.14159265358979323846f / 180.0f;
	const float tanHalfX = tanf(view.fovX * 0.5f * kDeg2Rad);
	if (tanHalfX <= 1e-6f)
		return false;
	const float tanHalfY = tanHalfX * (float)vp.height / (float)vp.width;
	if (tanHalfY <= 1e-6f)
		return false;

	const float ndcX = (x / z) / tanHalfX;
	const float ndcY = (y / z) / tanHalfY;

	out.inView = (ndcX >= -1.0f && ndcX <= 1.0f && ndcY >= -1.0f && ndcY <= 1.0f);

	// viewport-local pixels, v measured up from the bottom of the viewport
	float u = (ndcX + 1.0f) * 0.5f * (float)vp.width;
	float v = (ndcY + 1.0f) * 0.5f * (float)vp.height;

	// keep off-screen tags pinned to the edge instead of letting them disappear
	if (u < 0.0f) u = 0.0f; else if (u > (float)vp.width)  u = (float)vp.width;
	if (v < 0.0f) v = 0.0f; else if (v > (float)vp.height) v = (float)vp.height;

	// GL::SetupOrtho() installs glViewport(0, 0, w, h) with glOrtho(0, w, h, 0), i.e. its y grows
	// downwards from the bottom-left corner of the window. The game's own viewport starts at
	// (vp.x, vp.y), so map the viewport-local point into that space.
	out.x = (float)vp.x + u;
	out.y = (float)vp.height - (float)vp.y - v;
	return true;
}

void NameEsp::TeamColor(int team, unsigned char rgb[3])
{
	static const unsigned char palette[TeamCount][3] =
	{
		{ 255, 235,  80 },   // TeamFree      - warm yellow
		{ 255,  90,  90 },   // TeamRed
		{ 110, 170, 255 },   // TeamBlue
		{ 175, 175, 175 }    // TeamSpectator
	};

	const int i = (team >= 0 && team < TeamCount) ? team : (int)TeamFree;
	rgb[0] = palette[i][0];
	rgb[1] = palette[i][1];
	rgb[2] = palette[i][2];
}
