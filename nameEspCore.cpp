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
	q3::snapshot_t   s_snapshot;     // newest server snapshot the cgame requested
	q3::snapshot_t   s_prevSnapshot; // the one before it, when the hook still had it
	q3::gameState_t  s_gameState;

	NameEsp::Frame   s_frame;

	// ---- limits that keep a stale or bogus sample from throwing a tag across the map -------------
	const int   kMaxSnapshotAgeMs = 250;    // beyond this a snapshot is treated as "where it says"
	const float kMaxExtrapolatedStep = 200.0f;  // units; ~2x what a player covers in 250 ms

	// How far back Gather() looks for the snapshot it interpolates from when the exact previous
	// message number is not served (see the snapshot pair below). The VM hook's ring is 6 deep,
	// so 4 reaches well inside it; a genuine "no older sample exists" (first frames after a level
	// load) is found just as fast.
	const int   kPrevSnapshotLookback = 4;

	// The most the interpolation fraction may be pushed ahead of the newest snapshot when the
	// refdef is missing. One server frame at the default sv_fps 20 is 50 ms; guessing further
	// than a few frames extrapolates a player past where the model is.
	const int   kMaxRenderLagMs = 250;

	// The lag between the time a frame renders for (cg.time) and the newest snapshot's own time,
	// measured on the last frame that had a captured refdef. Vm::ServerTime() hands over the
	// snapshot time itself when there is no refdef, which clamps the interpolation fraction to
	// 1.0: the tag then holds the newest position and steps a whole server frame at a time
	// instead of gliding. Reusing the measured lag across those frames keeps the motion
	// continuous - the client renders a snapshot interval behind the newest snapshot as a rule.
	int   s_renderLag      = 0;
	bool  s_haveRenderLag  = false;

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
		if (rd.rdflags & q3::kRdfNoWorldModel)
			return false;                        // HUD icons are not the world camera
		if (!(rd.fov_x >= 1.0f && rd.fov_x <= 179.0f))
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
	// Freshness, the partner of the shape check above: is a captured refdef the view THIS frame
	// was rendered with? The captured world R_RenderScene supplies the view and the snapshot the tags
	// come from is the newest captured one, so a refdef older than that snapshot means the
	// captures stopped arriving while snapshots kept flowing - a frozen camera. Projecting
	// through it pins every tag to the edge (the world moved on, the camera did not), so past
	// NameEsp::kRefdefStaleMs the refdef is rejected exactly like a shape failure and the
	// rebuilt view (fresh usercmd angles) takes over. Small negatives never reach the threshold:
	// the cgame reads the NEXT snapshot ahead for interpolation, so the newest captured
	// snapshot is routinely a server frame (tens of ms) newer than the rendered one.
	// --------------------------------------------------------------------------------------------
	bool RefdefStale(const q3::refdef_t& rd, int snapshotTime)
	{
		return (snapshotTime - rd.time) > NameEsp::kRefdefStaleMs;
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
		if (refdef && RefdefUsable(*refdef) && !RefdefStale(*refdef, snap.serverTime))
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
	// Find an entity by its snapshot number in another snapshot, the way the cgame matches
	// centity records up when it transitions snapshots (cg.snap->entities[i].number). For players
	// the entity number is the client number (BG_PlayerStateToEntityState: s->number = clientNum).
	// --------------------------------------------------------------------------------------------
	const q3::entityState_t* FindEntityNumber(const q3::snapshot_t& snap, int number)
	{
		int n = snap.numEntities;
		if (n < 0)
			n = 0;
		if (n > q3::kMaxEntitiesInSnapshot)
			n = q3::kMaxEntitiesInSnapshot;
		for (int i = 0; i < n; ++i)
		{
			if (snap.entities[i].number == number)
				return &snap.entities[i];
		}
		return NULL;
	}

	// --------------------------------------------------------------------------------------------
	// Remote player positions, exactly the way the renderer places the models. CG_InterpolateEntity
	// Position() (cg_ents.c) evaluates the player's position at the previous snapshot's time and at
	// the newest one and lerps by cg.frameInterpolation = (cg.time - old.serverTime) /
	// (new.serverTime - old.serverTime), clamped to 0..1 - and it does this for player entities
	// unconditionally, even when the snapshot carried a LINEAR_STOP trajectory: players are never
	// velocity-extrapolated ("it is important to not extrapolate player positions if more recent
	// data is available"). Doing the same lerp here glues the tag to the model on every frame and
	// removes the stepping a one-snapshot-ahead position caused on moving players.
	//
	// When the previous sample is missing (the player just entered this client's PVS, the hook was
	// freshly installed, or EF_TELEPORT_BIT toggled - CG_SetNextSnap sets interpolate = qfalse in
	// exactly those cases), the newest snapshot position is used unlerped, matching CG_ResetEntity()
	// which snaps the model straight onto the new position.
	// renderTime is cg.time: the refdef time captured from R_RenderScene (Vm::ServerTime()). When
	// there is no refdef the caller passes the newest snapshot time, so f clamps to 1 and the tag
	// sits on the newest position (the model holds there too, while waiting for the next snap).
	// --------------------------------------------------------------------------------------------
	void InterpolatedOrigin(const q3::entityState_t* prev, int prevTime,
	                        const q3::entityState_t& cur, int curTime,
	                        int renderTime, float* out)
	{
		out[0] = cur.pos.trBase[0];
		out[1] = cur.pos.trBase[1];
		out[2] = cur.pos.trBase[2];

		if (!prev)
			return;
		// a teleport toggles EF_TELEPORT_BIT on the entity (BG_PlayerStateToEntityState copies the
		// ps flags); the cgame refuses to interpolate across it.
		if (((prev->eFlags ^ cur.eFlags) & q3::kEfTeleport) != 0)
			return;

		const int dt = curTime - prevTime;
		if (dt <= 0)
			return;

		float f = (float)(renderTime - prevTime) / (float)dt;
		if (f < 0.0f)
			f = 0.0f;
		else if (f > 1.0f)
			f = 1.0f;

		for (int i = 0; i < 3; ++i)
			out[i] = prev->pos.trBase[i] + f * (cur.pos.trBase[i] - prev->pos.trBase[i]);
	}
}

// =============================================================================================== //

bool NameEsp::CaptureWorldRefdef(const q3::refdef_t& candidate, q3::refdef_t& captured)
{
	if (!RefdefUsable(candidate))
		return false;
	captured = candidate;
	return true;
}

const NameEsp::Frame& NameEsp::Current()
{
	return s_frame;
}

void NameEsp::Reset()
{
	memset(&s_frame, 0, sizeof(s_frame));
	memset(&s_snapshot, 0, sizeof(s_snapshot));
	memset(&s_prevSnapshot, 0, sizeof(s_prevSnapshot));
	// the level is gone: the next snapshot's serverTime is a different clock, so the lag measured
	// against the old one must not be applied to the first frames of the new one.
	s_renderLag     = 0;
	s_haveRenderLag = false;
	// The GL half keeps per-client state of its own (fade-in ramp, which anchor each tag settled
	// on) and drops it with the level too - but from vmHook's DropLevelState(), not from here:
	// this file is the portable half and has no GL to talk to.
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
	s_frame.selfClientNum       = -1;
	s_frame.selfPmType          = 0;
	s_frame.selfHealth          = 0;
	s_frame.playerEntities      = 0;
	s_frame.skippedSelf         = 0;
	s_frame.skippedDead         = 0;
	s_frame.skippedBadSlot      = 0;
	s_frame.skippedNoInfo       = 0;
	s_frame.interpolatedPlayers = 0;

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

	// The cgame renders remote players by interpolating between the previous and the newest
	// server snapshots (CG_ProcessSnapshots / CG_InterpolateEntityPosition), and the VM hook keeps
	// a ring of the snapshots it saw the cgame request, so ask for the previous one as well.
	//
	// Walking back more than one number matters. The bridge serves the exact message number when
	// it still holds it and the NEWEST one when it does not (the real engine's CL_GetSnapshot just
	// fails, which reads as a miss too), so a miss cannot be told apart from "that is the newest
	// sample" except by the serverTime - and the exact previous number goes missing whenever the
	// cgame advanced by more than one server frame since the ring was last filled: a hitch that
	// skips snapshots, a ring too small to reach back, the first frames after a level load. On
	// those frames an unlerped position means the tag holds still and then jumps a whole server
	// frame, which is exactly the stepping the interpolation exists to remove. So keep asking one
	// number further back until a strictly older sample turns up.
	bool havePrev = false;
	for (int back = 1; back <= kPrevSnapshotLookback && !havePrev; ++back)
	{
		const int number = snapNumber - back;
		if (number < 0)
			break;
		if (!syscall(q3::CG_GETSNAPSHOT, (intptr_t)number,
		             (intptr_t)&s_prevSnapshot, (intptr_t)sizeof(s_prevSnapshot)))
			continue;                            // aged out of the buffer - try the one before
		if (s_prevSnapshot.serverTime > 0 &&
		    s_prevSnapshot.serverTime < s_snapshot.serverTime)
			havePrev = true;
	}
	if (!havePrev)
		memset(&s_prevSnapshot, 0, sizeof(s_prevSnapshot));

	// Zeroed first: the bridge leaves the destination untouched when the gameState is not live
	// (mid-load), and without this the tags would be named from the previous level's clientinfo.
	// A zeroed gameState reads as empty configstrings, which ParseClientInfo rejects - so a failed
	// read yields no tags instead of wrong ones.
	memset(&s_gameState, 0, sizeof(s_gameState));
	syscall(q3::CG_GETGAMESTATE, (intptr_t)&s_gameState);

	s_frame.serverTime   = serverTime;
	s_frame.snapshotTime = s_snapshot.serverTime;
	s_frame.selfClientNum = s_snapshot.ps.clientNum;
	s_frame.selfPmType    = s_snapshot.ps.pm_type;
	s_frame.selfHealth    = s_snapshot.ps.stats[q3::kStatHealth];

	BuildView(serverTime, s_snapshot, syscall, refdef, s_frame.view, s_frame.usedRefdef);

	// ---- the time the tags are interpolated at ---------------------------------------------------
	// The lerp fraction is (renderTime - prevTime) / (newTime - prevTime), and renderTime has to
	// be cg.time - the time the frame is being drawn for. A captured refdef carries exactly that
	// (refdef_t::time), so with one this is serverTime and the measured lag is remembered for the
	// frames that do not have one.
	int renderTime = serverTime;
	if (s_frame.usedRefdef)
	{
		// Either sign is real: a positive lag is the client rendering behind the newest snapshot
		// it holds, a negative one is the cgame having read the NEXT snapshot ahead for
		// interpolation while the frame is still drawn for an earlier cg.time. Clamping the
		// magnitude only, so a bogus capture cannot fling the fraction across the map.
		int lag = serverTime - s_snapshot.serverTime;
		if (lag >  kMaxRenderLagMs) lag =  kMaxRenderLagMs;
		if (lag < -kMaxRenderLagMs) lag = -kMaxRenderLagMs;
		s_renderLag     = lag;
		s_haveRenderLag = true;
	}
	else if (s_haveRenderLag)
	{
		renderTime = s_snapshot.serverTime + s_renderLag;
	}

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
		++s_frame.playerEntities;

		if (e.eFlags & q3::kEfDead)
		{
			++s_frame.skippedDead;                   // corpse on the floor
			continue;
		}

		const int clientNum = e.clientNum;
		if (clientNum < 0 || clientNum >= q3::kMaxClients)
		{
			++s_frame.skippedBadSlot;
			continue;
		}
		if (clientNum == self)
		{
			++s_frame.skippedSelf;                   // your own name, in your own face
			continue;
		}

		const char* info = q3::ConfigString(&s_gameState, q3::kCsPlayers + clientNum);

		PlayerTag& tag = s_frame.players[s_frame.playerCount];
		if (!ParseClientInfo(info, clientNum, tag))
		{
			++s_frame.skippedNoInfo;                 // slot is empty / no name yet
			continue;
		}

		// Same player in the previous snapshot, when present, gives the engine-faithful lerp
		// endpoint. Player entity numbers are client numbers (BG_PlayerStateToEntityState).
		const q3::entityState_t* prev =
			havePrev ? FindEntityNumber(s_prevSnapshot, e.number) : NULL;
		float anchor[3];
		InterpolatedOrigin(prev, s_prevSnapshot.serverTime, e, s_snapshot.serverTime,
		                   renderTime, anchor);
		if (prev)
			++s_frame.interpolatedPlayers;

		tag.origin[0] = anchor[0];
		tag.origin[1] = anchor[1];
		tag.origin[2] = anchor[2] + q3::kPlayerTagHeight;   // just above the 32 unit player bbox

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
