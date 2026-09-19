// =============================================================================================== //
// kutaQ3 hook tests - NAME ESP portable core (nameEspCore.cpp) and the q3sdk.h mirror helpers
//
// This compiles and links the REAL nameEspCore.cpp - the same file kutaQ3.dll is built from - and
// drives it through tests/fake_engine.cpp, which implements the engine half of the cgame syscall
// boundary. Nothing here re-implements the feature; what is checked is what the DLL will do.
//
// The projection maths is cross-checked against the engine's own AngleVectors(), compiled straight
// out of SDK/code/game/q_math.c and linked in, so the view axes come from id's code rather than
// from another copy of my own formula.
//
//     make -C tests check
// =============================================================================================== //

#include "nameEsp.h"
#include "distanceEsp.h"
#include "weaponEsp.h"
#include "fake_engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// the engine's own angle maths, from SDK/code/game/q_math.c
extern "C" void AngleVectors(const float angles[3], float forward[3], float right[3], float up[3]);
extern "C" void AnglesToAxis(const float angles[3], float axis[3][3]);

#include "check.h"

// =============================================================================================== //

// |a - b|, the distance the fade and the distance ESP are driven by. Computed here rather than
// pulled from an ESP header: the point of the check is that nameEspCore.cpp's own value matches
// an independent one.
static float DistanceBetween(const float a[3], const float b[3])
{
	const float dx = a[0] - b[0];
	const float dy = a[1] - b[1];
	const float dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void TestInfoStringParsing()
{
	Section("q3::InfoValueForKey / StripColorCodes / NameEsp::ParseClientInfo");

	char value[64];
	CHECK_TRUE(q3::InfoValueForKey("\\n\\Bitterman\\t\\red\\model\\sarge", "n", value, sizeof(value)),
	           "name key found");
	CHECK_STR(value, "Bitterman", "name value");
	CHECK_TRUE(q3::InfoValueForKey("\\n\\Bitterman\\t\\red\\model\\sarge", "t", value, sizeof(value)),
	           "team key found");
	CHECK_STR(value, "red", "team value");
	CHECK_TRUE(q3::InfoValueForKey("\\n\\Bitterman", "model", value, sizeof(value)) == false,
	           "absent key reports false");
	CHECK_TRUE(q3::InfoValueForKey("\\N\\Bitterman", "n", value, sizeof(value)),
	           "key compare is case insensitive (1.32 uses Q_stricmp)");
	CHECK_TRUE(q3::InfoValueForKey("n\\Bitterman", "n", value, sizeof(value)),
	           "leading backslash is optional");

	// colour codes
	char clean[64];
	CHECK_INT(q3::StripColorCodes("^1Bitter^7man", clean, sizeof(clean)), 9, "colour codes stripped length");
	CHECK_STR(clean, "Bitterman", "colour codes stripped");
	CHECK_STR(q3::StripColorCodes("no^codes^x9here", clean, sizeof(clean)) == 0 ? "" : clean,
	          "no^codes^x9here", "only ^<digit> is a colour code");
	CHECK_STR(q3::StripColorCodes("trailing^", clean, sizeof(clean)) == 0 ? "" : clean,
	          "trailing^", "a trailing ^ survives");
	CHECK_INT(q3::StripColorCodes("^1^2^3", clean, sizeof(clean)), 0, "all colour codes -> empty");

	// the clientinfo -> tag step Gather() uses
	NameEsp::PlayerTag tag;
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\^1Bitterman^7\\t\\red\\model\\sarge", 3, tag),
	           "clientinfo parsed");
	CHECK_STR(tag.name, "Bitterman", "tag name");
	CHECK_INT(tag.clientNum, 3, "tag clientNum");
	CHECK_INT(tag.team, NameEsp::TeamRed, "tag team red");

	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\Foo\\t\\blue", 1, tag), "blue team parsed");
	CHECK_INT(tag.team, NameEsp::TeamBlue, "tag team blue");
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\Spec\\t\\spectator", 2, tag), "spectator parsed");
	CHECK_INT(tag.team, NameEsp::TeamSpectator, "tag team spectator");
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\Free", 4, tag), "no team key");
	CHECK_INT(tag.team, NameEsp::TeamFree, "missing team defaults to free");

	// stock 1.32 sends the team NUMERIC (CG_NewClientInfo: atoi): 0 free, 1 red, 2 blue,
	// 3 spectator. The string names above are only a mod fallback.
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\R\\t\\1", 11, tag), "numeric red parsed");
	CHECK_INT(tag.team, NameEsp::TeamRed, "numeric 1 is red");
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\B\\t\\2", 12, tag), "numeric blue parsed");
	CHECK_INT(tag.team, NameEsp::TeamBlue, "numeric 2 is blue");
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\F\\t\\0", 13, tag), "numeric free parsed");
	CHECK_INT(tag.team, NameEsp::TeamFree, "numeric 0 is free");
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\S\\t\\3", 14, tag), "numeric spectator parsed");
	CHECK_INT(tag.team, NameEsp::TeamSpectator, "numeric 3 is spectator");
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\X\\t\\9", 15, tag), "out of range numeric accepted");
	CHECK_INT(tag.team, NameEsp::TeamFree, "out of range numeric falls back to free");
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\Y\\t\\ 2 ", 16, tag), "padded numeric parsed");
	CHECK_INT(tag.team, NameEsp::TeamBlue, "padded numeric 2 is blue");

	CHECK_TRUE(!NameEsp::ParseClientInfo("", 5, tag), "empty configstring rejected");
	CHECK_TRUE(!NameEsp::ParseClientInfo("\\t\\red\\model\\sarge", 5, tag), "nameless configstring rejected");
	CHECK_TRUE(!NameEsp::ParseClientInfo("\\n\\^1^2", 5, tag), "colour-only name rejected");
	CHECK_TRUE(NameEsp::ParseClientInfo("\\name\\Fallback\\t\\red", 6, tag), "long \"name\" key fallback");
	CHECK_STR(tag.name, "Fallback", "fallback name value");
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\   Spaced   ", 7, tag), "whitespace name accepted");
	CHECK_STR(tag.name, "Spaced", "whitespace trimmed");

	// a name full of format specifiers must survive verbatim - it is printed through "%s"
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\%s%s%n%x", 8, tag), "format specifier name accepted");
	CHECK_STR(tag.name, "%s%s%n%x", "format specifiers kept literally");

	// the display-list font only holds glyphs 32..127, so anything else becomes '?': the tag -
	// and with it the player's position - stays where stripping could empty the name and hide them
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\B\xF6" "se\\t\\1", 17, tag), "high-byte name accepted");
	CHECK_STR(tag.name, "B?se", "high byte becomes '?'");
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\A\x01" "B\\t\\0", 18, tag), "control-char name accepted");
	CHECK_STR(tag.name, "A?B", "control char becomes '?'");
	// the boundaries: 32 (space) and 126 ('~') survive, 31 and 127 do not
	CHECK_TRUE(NameEsp::ParseClientInfo("\\n\\A\x1F ~\x7F" "B\\t\\2", 19, tag), "boundary bytes accepted");
	CHECK_STR(tag.name, "A? ~?B", "only 32..126 survive");
}

static void TestModuleNameMatching()
{
	Section("q3::IsNativeCgameModule");

	CHECK_TRUE(q3::IsNativeCgameModule("cgame_mp_x86.dll"), "retail native cgame");
	CHECK_TRUE(q3::IsNativeCgameModule("C:\\Quake III Arena\\baseq3\\cgame_mp_x86.dll"), "full windows path");
	CHECK_TRUE(q3::IsNativeCgameModule("/opt/q3/baseq3/cgamex86.dll"), "full posix path");
	CHECK_TRUE(q3::IsNativeCgameModule("CGAME_MP_X86.DLL"), "case insensitive");
	CHECK_TRUE(q3::IsNativeCgameModule("cgame_x86.dll"), "mod spelling");
	CHECK_TRUE(!q3::IsNativeCgameModule("qagame_mp_x86.dll"), "server module rejected");
	CHECK_TRUE(!q3::IsNativeCgameModule("ui_mp_x86.dll"), "ui module rejected");
	CHECK_TRUE(!q3::IsNativeCgameModule("cgame.mpq"), "qvm pak rejected");
	CHECK_TRUE(!q3::IsNativeCgameModule("cgame_mp_x86"), "not a dll");
	CHECK_TRUE(!q3::IsNativeCgameModule("cgame_mp_ppc.dll"), "wrong arch");
	CHECK_TRUE(!q3::IsNativeCgameModule(NULL), "NULL rejected");
	CHECK_TRUE(!q3::IsNativeCgameModule(""), "empty rejected");
}

static void TestGatherGuards()
{
	Section("NameEsp::Gather - guards");

	FakeEngine::Reset();
	NameEsp::Reset();
	CHECK_TRUE(!NameEsp::Gather(1000, NULL), "no syscall trampoline -> no frame");
	CHECK_TRUE(!NameEsp::Current().valid, "frame stays invalid");
	CHECK_INT(NameEsp::Current().playerCount, 0, "no tags");
	CHECK_INT(NameEsp::Current().numEntities, 0, "no entities reported");

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetConnected(false);
	CHECK_TRUE(!NameEsp::Gather(1000, FakeEngine::Syscall()), "not connected -> no frame");
	CHECK_INT(FakeEngine::SnapshotRequests(), 1, "snapshot was asked for once");
	CHECK_TRUE(!NameEsp::Current().valid, "frame invalid when the snapshot is not valid");
}

static void TestGatherPlayers()
{
	Section("NameEsp::Gather - which entities become tags");

	const float here[3] = { 100.0f, 200.0f, 0.0f };
	const float none[3] = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\^1Bitterman^7\\t\\1\\model\\sarge", (const float[]){ 300.0f, 0.0f, 8.0f });
	FakeEngine::SetPlayer(2, "\\n\\Slash\\t\\2\\model\\slash",         (const float[]){ 0.0f, 400.0f, 16.0f });
	FakeEngine::SetPlayer(3, "\\n\\Granger\\t\\free",             (const float[]){ -500.0f, -500.0f, 0.0f });
	FakeEngine::SetDeadPlayer(4, "\\n\\Dead\\t\\red",                 (const float[]){ 10.0f, 10.0f, 0.0f });
	FakeEngine::SetNonPlayerEntity(200, (const float[]){ 5.0f, 5.0f, 5.0f });

	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "frame gathered");
	const NameEsp::Frame& frame = NameEsp::Current();
	CHECK_TRUE(frame.valid, "frame valid");
	CHECK_INT(frame.playerCount, 3, "three live players tagged (self, dead and item excluded)");
	CHECK_INT(frame.numEntities, 6, "entity count is pre-filtering (self + 3 live + dead + item)");
	CHECK_INT(frame.playerEntities, 5, "five ET_PLAYER entities (self + three live + corpse)");
	CHECK_INT(frame.skippedSelf, 1, "the local player entity is skipped as self");
	CHECK_INT(frame.skippedDead, 1, "the corpse is counted as dead");
	CHECK_INT(frame.skippedNoInfo, 0, "every live player has a configstring");
	CHECK_INT(frame.selfClientNum, 0, "self client number comes from the snapshot ps");
	CHECK_INT(frame.selfPmType, 0, "self pm_type is PM_NORMAL");
	CHECK_INT(frame.selfHealth, 100, "self health is reported");
	CHECK_INT(frame.serverTime, 1000, "server time recorded");
	CHECK_INT(frame.snapshotTime, 1000, "snapshot time recorded");

	for (int i = 0; i < frame.playerCount; ++i)
	{
		const NameEsp::PlayerTag& tag = frame.players[i];
		char what[96];
		snprintf(what, sizeof(what), "tag %d is not the local player", i);
		CHECK_TRUE(tag.clientNum != 0, what);
		CHECK_TRUE(tag.clientNum != 4, "tag is not the dead player");
		CHECK_INT(tag.origin[2] >= 0.0f ? 1 : 0, 1, "tag z is above the floor");
	}

	// find them by clientNum and check the details
	const NameEsp::PlayerTag* bitterman = NULL;
	const NameEsp::PlayerTag* slash = NULL;
	const NameEsp::PlayerTag* granger = NULL;
	for (int i = 0; i < frame.playerCount; ++i)
	{
		if (frame.players[i].clientNum == 1) bitterman = &frame.players[i];
		if (frame.players[i].clientNum == 2) slash = &frame.players[i];
		if (frame.players[i].clientNum == 3) granger = &frame.players[i];
	}
	CHECK_TRUE(bitterman && slash && granger, "all three clients present");
	if (bitterman && slash && granger)
	{
		CHECK_STR(bitterman->name, "Bitterman", "colour codes stripped from the name");
		CHECK_INT(bitterman->team, NameEsp::TeamRed, "red team colour");
		CHECK_NEAR(bitterman->origin[0], 300.0f, 0.001, "tag x is the entity x");
		CHECK_NEAR(bitterman->origin[2], 8.0f + q3::kPlayerTagHeight, 0.001, "tag sits above the head");
		CHECK_STR(slash->name, "Slash", "second name");
		CHECK_INT(slash->team, NameEsp::TeamBlue, "blue team colour");
		CHECK_NEAR(slash->origin[1], 400.0f, 0.001, "tag y is the entity y");
		CHECK_STR(granger->name, "Granger", "third name");
		CHECK_INT(granger->team, NameEsp::TeamFree, "free for all colour");
	}

	CHECK_INT(FakeEngine::SnapshotRequests(), 2, "newest snapshot read, plus a prev attempt");
	CHECK_INT(FakeEngine::GameStateRequests(), 1, "one gamestate read");
	CHECK_INT(FakeEngine::UserCmdRequests(), 1, "one usercmd read (for the view angles)");
}

static void TestView()
{
	Section("NameEsp::Gather - the view");

	const float origin[3]   = { 10.0f, 20.0f, 30.0f };
	const float velocity[3] = { 320.0f, -160.0f, 0.0f };
	const float angles[3]   = { -12.5f, 137.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, origin, velocity, angles, 26);
	FakeEngine::SetFovString("110");

	// Gather() with no angle deltas in play, so the usercmd angles are the view angles
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered");
	const NameEsp::View& view = NameEsp::Current().view;
	CHECK_TRUE(view.valid, "view valid");
	CHECK_NEAR(view.origin[0], 10.0f, 0.001, "no snapshot age -> no extrapolation, x");
	CHECK_NEAR(view.origin[1], 20.0f, 0.001, "y");
	CHECK_NEAR(view.origin[2], 30.0f + 26.0f, 0.001, "origin + viewheight");
	CHECK_NEAR(view.fovX, 110.0f, 0.001, "fov_x from the cg_fov cvar");

	// now the snapshot is 100 ms old, so the origin moves forward by velocity * 0.1
	NameEsp::Reset();
	CHECK_TRUE(NameEsp::Gather(5100, FakeEngine::Syscall()), "frame gathered with an aged snapshot");
	const NameEsp::View& aged = NameEsp::Current().view;
	CHECK_NEAR(aged.origin[0], 10.0f + 320.0f * 0.1f, 0.01, "aged snapshot extrapolated, x");
	CHECK_NEAR(aged.origin[1], 20.0f - 160.0f * 0.1f, 0.01, "aged snapshot extrapolated, y");

	// The view angles reach Gather() as usercmd shorts, so the engine's PM_UpdateViewAngles
	// rebuilds them quantised to 1/65536 of a turn. Compare against that, not the raw floats.
	float quantised[3];
	for (int i = 0; i < 3; ++i)
		quantised[i] = q3::ShortToAngle(q3::AngleToShort(angles[i]));
	CHECK_NEAR(quantised[1], angles[1], 0.006, "ANGLE2SHORT round trip stays within one step");

	// the view axes must be the engine's AnglesToAxis for those angles
	float fwd[3], right[3], up[3];
	AngleVectors(quantised, fwd, right, up);
	CHECK_NEAR(view.axis[0][0], fwd[0], 1e-5, "axis[0] == engine forward x");
	CHECK_NEAR(view.axis[0][2], fwd[2], 1e-5, "axis[0] == engine forward z");
	CHECK_NEAR(view.axis[1][0], -right[0], 1e-5, "axis[1] == engine -right x");
	CHECK_NEAR(view.axis[1][1], -right[1], 1e-5, "axis[1] == engine -right y");
	CHECK_NEAR(view.axis[2][1], up[1], 1e-5, "axis[2] == engine up y");

	// delta_angles: viewangles = SHORT2ANGLE(cmd.angles + ps.delta_angles), the engine's
	// PM_UpdateViewAngles. Set a delta and check the axis follows.
	const int deltaYaw = 4096;   // 22.5 degrees
	NameEsp::Reset();
	FakeEngine::SetDeltaAngles(0, deltaYaw, 0);
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered with delta_angles");
	const float expectedYaw = 137.0f + (float)(deltaYaw * (360.0 / 65536));
	float expectAngles[3] = { -12.5f, expectedYaw, 0.0f };
	float ef[3], er[3], eu[3];
	AngleVectors(expectAngles, ef, er, eu);
	CHECK_NEAR(NameEsp::Current().view.axis[0][0], ef[0], 1e-4, "delta_angles applied to yaw");
	CHECK_NEAR(NameEsp::Current().view.axis[0][1], ef[1], 1e-4, "delta_angles applied to yaw (y)");

	// no usercmd available -> the snapshot's own angles are used (these are not quantised)
	NameEsp::Reset();
	FakeEngine::SetNoUserCmd();
	const int userCmdsBefore = FakeEngine::UserCmdRequests();
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered without a usercmd");
	float nf[3], nr[3], nu[3];
	AngleVectors(angles, nf, nr, nu);
	CHECK_NEAR(NameEsp::Current().view.axis[0][0], nf[0], 1e-5, "falls back to ps.viewangles");
	CHECK_INT(FakeEngine::UserCmdRequests() - userCmdsBefore, 1, "the usercmd was attempted once");

	// fov fallbacks
	NameEsp::Reset();
	FakeEngine::SetFovString("");
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered with an empty cg_fov");
	CHECK_NEAR(NameEsp::Current().view.fovX, 90.0f, 0.001, "empty cvar -> 90");
	NameEsp::Reset();
	FakeEngine::SetFovString("0");
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered with cg_fov 0");
	CHECK_NEAR(NameEsp::Current().view.fovX, 1.0f, 0.001, "fov below 1 clamped to 1, like the engine");
	NameEsp::Reset();
	FakeEngine::SetFovString("400");
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered with cg_fov 400");
	CHECK_NEAR(NameEsp::Current().view.fovX, 179.0f, 0.001, "fov above 179 clamped to 179");
}

static void TestFrozenView()
{
	Section("NameEsp::Gather - PM_UpdateViewAngles parity (frozen view, pitch clamp)");

	const float origin[3]   = { 0.0f, 0.0f, 0.0f };
	const float velocity[3] = { 0.0f, 0.0f, 0.0f };

	// Dead and playing: the engine freezes the viewangles where death left them, so the mouse
	// (the usercmd) must NOT move the ESP view. Point the usercmd elsewhere and check the
	// snapshot's own angles win.
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, origin, velocity, (const float[]){ 10.0f, 90.0f, 0.0f }, 26);
	FakeEngine::SetHealth(0);                                                        // dead
	FakeEngine::SetCmdAngles(q3::AngleToShort(-30.0f), q3::AngleToShort(270.0f), 0);  // looking away
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered while dead");
	{
		float want[3] = { 10.0f, 90.0f, 0.0f };   // snapshot angles, unquantised floats
		float f[3], r[3], u[3];
		AngleVectors(want, f, r, u);
		CHECK_NEAR(NameEsp::Current().view.axis[0][0], f[0], 1e-5, "dead view keeps the snapshot forward x");
		CHECK_NEAR(NameEsp::Current().view.axis[0][1], f[1], 1e-5, "dead view keeps the snapshot forward y");
		CHECK_NEAR(NameEsp::Current().view.axis[0][2], f[2], 1e-5, "dead view keeps the snapshot forward z");
	}

	// ... but a spectator with no health still follows the mouse: the engine exempts
	// PM_SPECTATOR from the freeze.
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, origin, velocity, (const float[]){ 10.0f, 90.0f, 0.0f }, 26);
	FakeEngine::SetPmType(q3::kPmSpectator);
	FakeEngine::SetHealth(0);
	FakeEngine::SetCmdAngles(q3::AngleToShort(-30.0f), q3::AngleToShort(270.0f), 0);
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered as a dead spectator");
	{
		float want[3] = { -30.0f, 270.0f, 0.0f };
		for (int i = 0; i < 3; ++i)   // the usercmd path quantises through ANGLE2SHORT
			want[i] = q3::ShortToAngle(q3::AngleToShort(want[i]));
		float f[3], r[3], u[3];
		AngleVectors(want, f, r, u);
		CHECK_NEAR(NameEsp::Current().view.axis[0][0], f[0], 1e-5, "spectator view follows the usercmd");
		CHECK_NEAR(NameEsp::Current().view.axis[0][1], f[1], 1e-5, "spectator view follows the usercmd (y)");
	}

	// Intermission: frozen too, whatever the health and whatever the usercmd says.
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, origin, velocity, (const float[]){ 10.0f, 90.0f, 0.0f }, 26);
	FakeEngine::SetPmType(q3::kPmIntermission);
	FakeEngine::SetCmdAngles(q3::AngleToShort(-30.0f), q3::AngleToShort(270.0f), 0);
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered in intermission");
	{
		float want[3] = { 10.0f, 90.0f, 0.0f };
		float f[3], r[3], u[3];
		AngleVectors(want, f, r, u);
		CHECK_NEAR(NameEsp::Current().view.axis[0][0], f[0], 1e-5, "intermission view frozen");
		CHECK_NEAR(NameEsp::Current().view.axis[0][1], f[1], 1e-5, "intermission view frozen (y)");
	}

	// Pitch clamp: +/-16000 shorts (+/-87.9 degrees), like the engine.
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, origin, velocity, (const float[]){ 0.0f, 90.0f, 0.0f }, 26);
	FakeEngine::SetCmdAngles(30000, q3::AngleToShort(90.0f), 0);   // pitch way past straight up
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered with extreme pitch");
	{
		float want[3] = { q3::ShortToAngle(q3::kMaxViewPitchShort), 90.0f, 0.0f };
		float f[3], r[3], u[3];
		AngleVectors(want, f, r, u);
		CHECK_NEAR(NameEsp::Current().view.axis[0][2], f[2], 1e-5, "pitch clamped to +87.9 degrees");
	}
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, origin, velocity, (const float[]){ 0.0f, 90.0f, 0.0f }, 26);
	FakeEngine::SetCmdAngles(-30000, q3::AngleToShort(90.0f), 0);
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered with extreme negative pitch");
	{
		float want[3] = { q3::ShortToAngle(-q3::kMaxViewPitchShort), 90.0f, 0.0f };
		float f[3], r[3], u[3];
		AngleVectors(want, f, r, u);
		CHECK_NEAR(NameEsp::Current().view.axis[0][2], f[2], 1e-5, "pitch clamped to -87.9 degrees");
	}

	// The (short) truncation happens BEFORE the clamp, like the engine: 40000 wraps to -25536 and
	// clamps to -16000 (down), it must not clamp to +16000 (up).
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, origin, velocity, (const float[]){ 0.0f, 90.0f, 0.0f }, 26);
	FakeEngine::SetCmdAngles(40000, q3::AngleToShort(90.0f), 0);
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "frame gathered with a wrapped pitch");
	{
		float want[3] = { q3::ShortToAngle(-q3::kMaxViewPitchShort), 90.0f, 0.0f };
		float f[3], r[3], u[3];
		AngleVectors(want, f, r, u);
		CHECK_NEAR(NameEsp::Current().view.axis[0][2], f[2], 1e-5, "wrapped pitch truncates before clamping");
	}
}

// =============================================================================================== //
// The captured refdef_t path (vmHook.cpp hands Gather() the view the cgame rendered). Built with
// the engine's own AnglesToAxis() so the axes are id's, not another copy of the formula.
// =============================================================================================== //

static void MakeRefdef(q3::refdef_t& rd, const float origin[3], const float angles[3], float fov)
{
	memset(&rd, 0, sizeof(rd));
	rd.x = 0; rd.y = 0; rd.width = 1280; rd.height = 1024;
	rd.fov_x = fov;
	rd.fov_y = fov * 0.75f;
	rd.time = 5000;
	for (int i = 0; i < 3; ++i)
		rd.vieworg[i] = origin[i];
	AnglesToAxis(angles, rd.viewaxis);
}

static void TestRefdefView()
{
	Section("NameEsp::Gather - the captured refdef_t view");

	const float origin[3]   = { 10.0f, 20.0f, 30.0f };
	const float velocity[3] = { 320.0f, -160.0f, 0.0f };
	const float angles[3]   = { -12.5f, 137.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, origin, velocity, angles, 26);
	FakeEngine::SetFovString("110");
	FakeEngine::SetDeltaAngles(0, 4096, 0);        // the usercmd path would add 22.5 deg of yaw

	const float viewOrigin[3] = { 111.0f, -222.0f, 333.0f };
	q3::refdef_t rd;
	MakeRefdef(rd, viewOrigin, angles, 105.0f);

	// pin the convention RefdefUsable() measures, against the engine's own AnglesToAxis():
	// cross(viewaxis[0], viewaxis[1]) == +viewaxis[2], so the triple is right handed and
	// viewaxis[1] is the world LEFT vector (AngleVectors' "right" is (0,-1,0) at zero angles and
	// AnglesToAxis negates it).
	{
		const float* f = rd.viewaxis[0];
		const float* l = rd.viewaxis[1];
		const float* u = rd.viewaxis[2];
		const float det = (f[1] * l[2] - f[2] * l[1]) * u[0]
		                + (f[2] * l[0] - f[0] * l[2]) * u[1]
		                + (f[0] * l[1] - f[1] * l[0]) * u[2];
		CHECK_NEAR(det, 1.0f, 1e-5, "the engine's AnglesToAxis triple is right handed");
	}

	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), &rd), "frame gathered with a refdef");
	const NameEsp::View& view = NameEsp::Current().view;
	CHECK_TRUE(view.valid, "view valid");
	CHECK_TRUE(NameEsp::Current().usedRefdef, "captured refdef reported as the view source");
	CHECK_NEAR(view.origin[0], 111.0f, 1e-5, "refdef vieworg x, not the snapshot origin");
	CHECK_NEAR(view.origin[1], -222.0f, 1e-5, "refdef vieworg y");
	CHECK_NEAR(view.origin[2], 333.0f, 1e-5, "refdef vieworg z - no viewheight added");
	CHECK_NEAR(view.fovX, 105.0f, 1e-5, "fov_x straight from the refdef, not cg_fov");

	// the axes are the cgame's, verbatim - which is also what proves delta_angles never got applied
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			CHECK_NEAR(view.axis[i][j], rd.viewaxis[i][j], 1e-6, "viewaxis copied verbatim");

	// ---- a refdef that fails the shape check must be ignored, not projected with ---------------
	NameEsp::Reset();
	q3::refdef_t bad = rd;
	bad.fov_x = 0.0f;
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), &bad), "gathered with a zero fov refdef");
	CHECK_NEAR(NameEsp::Current().view.fovX, 110.0f, 0.001, "rejected -> falls back to cg_fov");

	NameEsp::Reset();
	bad = rd;
	bad.width = 0;
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), &bad), "gathered with a zero width refdef");
	CHECK_NEAR(NameEsp::Current().view.origin[0], 10.0f, 0.01, "rejected -> falls back to the snapshot origin");

	// AnglesToAxis() gives a right handed triple (cross(axis0, axis1) == +axis2); negating axis[1]
	// makes it left handed, i.e. a mirrored view, which the handedness check has to catch instead
	// of mirroring every tag on screen
	NameEsp::Reset();
	bad = rd;
	for (int j = 0; j < 3; ++j)
		bad.viewaxis[1][j] = -bad.viewaxis[1][j];
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), &bad), "gathered with a mirrored refdef");

	// the rejected refdef means the fallback view is built instead: usercmd angles plus the
	// delta_angles set above, i.e. yaw + 22.5 degrees - definitely not the mirrored axis
	{
		const float expectedYaw = angles[1] + (float)(4096 * (360.0 / 65536));
		float expectAngles[3] = { angles[0], expectedYaw, angles[2] };
		float ef[3], er[3], eu[3];
		AngleVectors(expectAngles, ef, er, eu);
		const NameEsp::View& fallback = NameEsp::Current().view;
		CHECK_NEAR(fallback.axis[1][0], -er[0], 1e-4, "mirrored axes rejected -> the fallback view is used");
		CHECK_TRUE(fabs(fallback.axis[1][0] - rd.viewaxis[1][0]) > 0.1,
		           "and the mirrored viewaxis[1] was not copied through");
	}

	NameEsp::Reset();
	bad = rd;
	bad.viewaxis[0][0] = 0.0f / 0.0f;              // NaN
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), &bad), "gathered with a NaN axis");
	CHECK_NEAR(NameEsp::Current().view.fovX, 110.0f, 0.001, "NaN refdef rejected");

	// ---- a refdef older than the snapshot is a frozen camera, not the rendered view --------
	// (R_RenderScene stopped arriving while snapshots kept flowing). Projecting through it pins
	// every tag to the edge, so it is rejected like a shape failure and the fallback view -
	// usercmd angles plus cg_fov - takes over instead.
	NameEsp::Reset();
	bad = rd;
	bad.time = 5000 - 1000;                        // a full second stale
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), &bad), "gathered with a stale refdef");
	CHECK_TRUE(!NameEsp::Current().usedRefdef, "stale refdef rejected");
	CHECK_NEAR(NameEsp::Current().view.fovX, 110.0f, 0.001, "stale refdef -> falls back to cg_fov");

	// ...but small negatives are the cgame reading the NEXT snapshot ahead for interpolation,
	// which is legitimate and must keep the refdef.
	NameEsp::Reset();
	bad = rd;
	bad.time = 5000 - 50;
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), &bad), "gathered with a read-ahead refdef");
	CHECK_TRUE(NameEsp::Current().usedRefdef, "50 ms of read-ahead keeps the refdef");
	CHECK_NEAR(NameEsp::Current().view.fovX, 105.0f, 1e-5, "read-ahead refdef still provides the fov");

	// the boundary itself still counts as fresh
	NameEsp::Reset();
	bad = rd;
	bad.time = 5000 - NameEsp::kRefdefStaleMs;
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), &bad), "gathered with a threshold-old refdef");
	CHECK_TRUE(NameEsp::Current().usedRefdef, "exactly kRefdefStaleMs keeps the refdef");

	// no refdef at all still works - the fallback view above is what the tests before this use
	NameEsp::Reset();
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), NULL), "gathered without a refdef");
	CHECK_TRUE(NameEsp::Current().view.valid, "fallback view still valid");
	CHECK_TRUE(!NameEsp::Current().usedRefdef, "rebuilt view reported as the view source");
}

static void TestInterpolation()
{
	Section("NameEsp::Gather - engine-faithful player interpolation");

	const float here[3] = { 0.0f, 0.0f, 0.0f };
	const float none[3] = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };

	// ---- first frame: no previous snapshot yet --------------------------------------------------
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Runner", (const float[]){ 0.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "first frame gathered");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 0.0f, 0.001,
	           "first frame is not extrapolated");
	CHECK_INT(NameEsp::Current().interpolatedPlayers, 0,
	          "nothing is interpolated without a previous snapshot");

	// ---- second server frame: runner covered 5 units in 50 ms (100 u/s) -------------------------
	FakeEngine::NewServerFrame(1050);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Runner", (const float[]){ 5.0f, 0.0f, 0.0f });

	// CG_InterpolateEntityPosition(): cg.time at the midpoint -> the model, and the tag, are
	// halfway between the two server samples.
	CHECK_TRUE(NameEsp::Gather(1025, FakeEngine::Syscall()), "midpoint frame gathered");
	CHECK_INT(NameEsp::Current().interpolatedPlayers, 1, "the runner is interpolated");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 2.5f, 0.01f,
	           "tag sits on the lerp midpoint, not one snapshot ahead");

	// f is clamped to the two samples at both ends, never beyond
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "old-end frame gathered");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 0.0f, 0.01f, "f = 0 -> previous position");
	CHECK_TRUE(NameEsp::Gather(1050, FakeEngine::Syscall()), "new-end frame gathered");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 5.0f, 0.01f, "f = 1 -> newest position");
	// Waiting for the next server snapshot: the engine holds players on the newest sample rather
	// than velocity-extrapolating them, so the tag must not run ahead of the model either.
	CHECK_TRUE(NameEsp::Gather(1150, FakeEngine::Syscall()), "frame waiting for the next snap");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 5.0f, 0.01f,
	           "no forward extrapolation while waiting for the next snapshot");

	// ---- a stationary player is motionless at every interpolation fraction --------------------
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(2000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Sentry", (const float[]){ 20.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(2000, FakeEngine::Syscall()), "static: first frame gathered");
	FakeEngine::NewServerFrame(2050);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Sentry", (const float[]){ 20.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(2025, FakeEngine::Syscall()), "static: mid frame gathered");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 20.0f, 0.01f,
	           "identical endpoints never jitter");

	// ---- a toggled teleport bit snaps to the new position instead of lerping across the map ----
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(3000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Blinker", (const float[]){ 5.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(3000, FakeEngine::Syscall()), "teleport: first frame gathered");
	FakeEngine::NewServerFrame(3050);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayerEx(1, "\\n\\Blinker", (const float[]){ 100.0f, 0.0f, 0.0f },
	                        q3::kEfTeleport);
	CHECK_TRUE(NameEsp::Gather(3025, FakeEngine::Syscall()), "teleport: mid frame gathered");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 100.0f, 0.01f,
	           "a teleport is used as-is instead of lerping across it");

	// ---- a player just entering the PVS is missing from the previous snapshot ------------------
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(4000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	CHECK_TRUE(NameEsp::Gather(4000, FakeEngine::Syscall()), "PVS entry: first frame gathered");
	FakeEngine::NewServerFrame(4050);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Arrival", (const float[]){ 50.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(4025, FakeEngine::Syscall()), "PVS entry: second frame gathered");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 50.0f, 0.01f,
	           "newly visible player uses the newest position, matching CG_ResetEntity()");
	CHECK_INT(NameEsp::Current().interpolatedPlayers, 0,
	          "a player with no previous sample is not interpolated");
}

// =============================================================================================== //
// The previous sample is not always at newest-1, and the VM hook's bridge answers a number its
// ring no longer holds with the NEWEST snapshot. Gather() has to find a genuinely older sample in
// both cases, or those frames lose the interpolation and the tag steps instead of gliding.
// =============================================================================================== //
static void TestInterpolationLookback()
{
	Section("NameEsp::Gather - finding the sample it interpolates from");

	const float here[3]   = { 0.0f, 0.0f, 0.0f };
	const float none[3]   = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };

	// ---- the exact previous number is gone: the cgame advanced by two server frames -------------
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Skipper", (const float[]){ 0.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall()), "lookback: first frame gathered");

	FakeEngine::NewServerFrame(5050);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Skipper", (const float[]){ 10.0f, 0.0f, 0.0f });
	FakeEngine::SetPrevNumberGap(2);                 // newest-1 aged out of the ring

	CHECK_TRUE(NameEsp::Gather(5025, FakeEngine::Syscall()), "lookback: skipped number gathered");
	CHECK_INT(NameEsp::Current().interpolatedPlayers, 1,
	          "the sample two numbers back is still used to interpolate");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 5.0f, 0.01f,
	           "and the tag is on the lerp midpoint, not snapped to the newest position");

	// ---- the bridge serves the NEWEST snapshot for a number it does not hold --------------------
	// Same request, different answer: a miss now reads as a successful read of a snapshot whose
	// serverTime equals the newest one. Only the serverTime tells the two apart, so walking back
	// has to keep going past it.
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(6000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Bridge", (const float[]){ 0.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(6000, FakeEngine::Syscall()), "bridge: first frame gathered");

	// Three server frames, so the sample behind the newest is a real one: message 2 holds the
	// runner at 5, message 3 (the newest) at 10.
	FakeEngine::NewServerFrame(6025);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Bridge", (const float[]){ 5.0f, 0.0f, 0.0f });
	FakeEngine::NewServerFrame(6050);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Bridge", (const float[]){ 10.0f, 0.0f, 0.0f });
	FakeEngine::SetPrevNumberGap(2);                 // the older sample is two numbers back
	FakeEngine::SetServeNewestOnMiss(true);          // everything else is answered with the newest

	// 12/25 of the way from message 2 (x = 5) to message 3 (x = 10)
	CHECK_TRUE(NameEsp::Gather(6037, FakeEngine::Syscall()), "bridge: gathered through the fallback");
	CHECK_INT(NameEsp::Current().interpolatedPlayers, 1,
	          "the newest-on-miss answer is recognised as not the previous sample");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 7.4f, 0.01f,
	           "and the older sample behind it is what the tag lerps from");

	// ---- no older sample at all: the newest position, no lerp -----------------------------------
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(7000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Alone", (const float[]){ 4.0f, 0.0f, 0.0f });
	FakeEngine::SetPrevNumberGap(9);                 // further back than the lookback reaches
	FakeEngine::SetServeNewestOnMiss(true);
	CHECK_TRUE(NameEsp::Gather(7000, FakeEngine::Syscall()), "no older sample: gathered");
	CHECK_INT(NameEsp::Current().interpolatedPlayers, 0, "nothing to interpolate against");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 4.0f, 0.01f,
	           "the newest position is used unlerped");
}

// =============================================================================================== //
// Without a captured refdef there is no cg.time to interpolate at. Holding the fraction at 1 makes
// the tag stop and jump a whole server frame at a time; carrying the lag measured on the last frame
// that DID have a refdef keeps it moving like the model.
// =============================================================================================== //
static void TestRenderLagCarryOver()
{
	Section("NameEsp::Gather - the interpolation clock survives a missing refdef");

	const float origin[3]   = { 0.0f, 0.0f, 0.0f };
	const float velocity[3] = { 0.0f, 0.0f, 0.0f };
	const float angles[3]   = { 0.0f, 0.0f, 0.0f };
	const float viewOrigin[3] = { 0.0f, 0.0f, 26.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(8000);
	FakeEngine::SetLocalPlayer(0, origin, velocity, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Glider", (const float[]){ 0.0f, 0.0f, 0.0f });

	q3::refdef_t rd;
	MakeRefdef(rd, viewOrigin, angles, 90.0f);
	rd.time = 8000;
	CHECK_TRUE(NameEsp::Gather(8000, FakeEngine::Syscall(), &rd), "lag: first frame gathered");

	// one server frame later: the runner is at 10, and the frame renders 25 ms into that frame
	FakeEngine::NewServerFrame(8050);
	FakeEngine::SetLocalPlayer(0, origin, velocity, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Glider", (const float[]){ 10.0f, 0.0f, 0.0f });

	rd.time = 8025;
	CHECK_TRUE(NameEsp::Gather(8025, FakeEngine::Syscall(), &rd), "lag: refdef frame gathered");
	CHECK_TRUE(NameEsp::Current().usedRefdef, "refdef is the view");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 5.0f, 0.01f, "midpoint with the refdef");

	// the next frame has no refdef, but the same serverTime: the lag measured above (8025 - 8000)
	// puts the fraction back where it was instead of clamping it to 1.
	CHECK_TRUE(NameEsp::Gather(8050, FakeEngine::Syscall(), NULL), "lag: frame without a refdef");
	CHECK_TRUE(!NameEsp::Current().usedRefdef, "no refdef -> rebuilt view");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 5.0f, 0.01f,
	           "the remembered lag keeps the fraction instead of clamping it to 1");

	// ... and a second frame with neither a refdef nor a new snapshot holds there. The carried lag
	// is an offset from the newest sample, not an extrapolation, so the tag sits where the model
	// sits instead of running ahead of it.
	CHECK_TRUE(NameEsp::Gather(8050, FakeEngine::Syscall(), NULL), "lag: another frame without one");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 5.0f, 0.01f,
	           "no new snapshot -> the tag holds, it does not extrapolate");

	// a level change resets the clock, so a lag measured against the old one must not be applied
	NameEsp::Reset();
	FakeEngine::Reset();
	FakeEngine::SetSnapshotTime(9000);
	FakeEngine::SetLocalPlayer(0, origin, velocity, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Fresh", (const float[]){ 3.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(9000, FakeEngine::Syscall(), NULL), "lag: reset frame gathered");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 3.0f, 0.01f,
	           "Reset() drops the carried lag with the level");
}

static void TestReset()
{
	Section("NameEsp::Reset");

	const float here[3] = { 0.0f, 0.0f, 0.0f };
	const float none[3] = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Runner", (const float[]){ 0.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "frame gathered");
	CHECK_INT(NameEsp::Current().playerCount, 1, "one tag");

	NameEsp::Reset();
	CHECK_TRUE(!NameEsp::Current().valid, "frame dropped");
	CHECK_INT(NameEsp::Current().playerCount, 0, "tags dropped");
	CHECK_INT(NameEsp::Current().numEntities, 0, "entity count dropped");
	CHECK_TRUE(!NameEsp::Current().view.valid, "view dropped");

	// the smoothing history is dropped too: the next sample must not extrapolate from the old one
	FakeEngine::Reset();
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Runner", (const float[]){ 500.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(5100, FakeEngine::Syscall()), "frame gathered after a reset");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 500.0f, 0.001, "no stale history");
}

// -----------------------------------------------------------------------------------------------
// Independent projection oracle: build the same two matrices the renderer would (a look-at view
// matrix from the engine's AngleVectors, and a perspective matrix with glFrustum's semantics) and
// take the point through them.
// -----------------------------------------------------------------------------------------------
static void Mul44(const float a[16], const float b[16], float out[16])
{
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
		{
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k)
				sum += a[r * 4 + k] * b[k * 4 + c];
			out[r * 4 + c] = sum;
		}
}

static bool OracleProject(const NameEsp::View& view, const NameEsp::Viewport& vp,
                          const float world[3], float& outX, float& outY, bool& outInView)
{
	// AnglesToAxis gave us forward/left/up; the engine's AngleVectors gives forward/right/up
	float angles[3];
	{
		// recover the angles from the axis so the oracle uses the engine's own vectors
		const float* f = view.axis[0];
		angles[0] = atan2f(-f[2], sqrtf(f[0] * f[0] + f[1] * f[1])) * 180.0f / 3.14159265358979f;
		angles[1] = atan2f(f[1], f[0]) * 180.0f / 3.14159265358979f;
		angles[2] = 0.0f;
	}
	float fwd[3], right[3], up[3];
	AngleVectors(angles, fwd, right, up);

	// R_RotateForViewer: camera at view.origin, looking along -Z of view space
	const float o[3] = { view.origin[0], view.origin[1], view.origin[2] };
	const float dotR = right[0] * o[0] + right[1] * o[1] + right[2] * o[2];
	const float dotU = up[0] * o[0] + up[1] * o[1] + up[2] * o[2];
	const float dotF = fwd[0] * o[0] + fwd[1] * o[1] + fwd[2] * o[2];
	const float V[16] = {
		 right[0],  right[1],  right[2], -dotR,
		 up[0],     up[1],     up[2],    -dotU,
		-fwd[0],   -fwd[1],   -fwd[2],    dotF,
		 0.0f,      0.0f,      0.0f,      1.0f
	};

	// R_SetupProjection -> glFrustum(-n*tanX, n*tanX, -n*tanY, n*tanY, n, f)
	const float kDeg2Rad = 3.14159265358979323846f / 180.0f;
	const float tanX = tanf(view.fovX * 0.5f * kDeg2Rad);
	const float tanY = tanX * (float)vp.height / (float)vp.width;
	const float n = 4.0f, f = 16384.0f;
	const float P[16] = {
		1.0f / tanX, 0.0f,       0.0f,                  0.0f,
		0.0f,        1.0f / tanY,0.0f,                  0.0f,
		0.0f,        0.0f,      -(f + n) / (f - n),    -2.0f * f * n / (f - n),
		0.0f,        0.0f,      -1.0f,                  0.0f
	};

	float M[16];
	Mul44(P, V, M);

	const float p[4] = { world[0], world[1], world[2], 1.0f };
	float clip[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			clip[r] += M[r * 4 + c] * p[c];

	if (clip[3] <= 0.0001f)
		return false;

	const float ndcX = clip[0] / clip[3];
	const float ndcY = clip[1] / clip[3];
	outInView = (ndcX >= -1.0f && ndcX <= 1.0f && ndcY >= -1.0f && ndcY <= 1.0f);

	float u = (ndcX + 1.0f) * 0.5f * (float)vp.width;
	float v = (ndcY + 1.0f) * 0.5f * (float)vp.height;
	if (u < 0.0f) u = 0.0f; else if (u > (float)vp.width)  u = (float)vp.width;
	if (v < 0.0f) v = 0.0f; else if (v > (float)vp.height) v = (float)vp.height;

	outX = (float)vp.x + u;
	outY = (float)vp.height - (float)vp.y - v;
	return true;
}

static void TestProjection()
{
	Section("NameEsp::ProjectWorldToScreen vs engine matrices");

	NameEsp::View view;
	memset(&view, 0, sizeof(view));
	view.valid = true;
	view.origin[0] = 0.0f; view.origin[1] = 0.0f; view.origin[2] = 0.0f;
	view.fovX = 90.0f;

	// axis from the engine's own AngleVectors for a yaw of 40, pitch of -10
	float angles[3] = { -10.0f, 40.0f, 0.0f };
	float fwd[3], right[3], up[3];
	AngleVectors(angles, fwd, right, up);
	for (int i = 0; i < 3; ++i)
	{
		view.axis[0][i] = fwd[i];
		view.axis[1][i] = -right[i];
		view.axis[2][i] = up[i];
	}

	// sanity: the axis handed to the projector really is the engine's, so the oracle below is not
	// just comparing my formula with itself
	float mine[3][3];
	q3::AnglesToAxis(angles, mine);
	for (int a = 0; a < 3; ++a)
		for (int i = 0; i < 3; ++i)
		{
			char what[96];
			snprintf(what, sizeof(what), "q3::AnglesToAxis[%d][%d] matches the engine", a, i);
			CHECK_NEAR(mine[a][i], view.axis[a][i], 1e-5, what);
		}

	const NameEsp::Viewport vp = { 0, 0, 800, 600 };
	const float points[][3] = {
		{  300.0f,  250.0f,   40.0f },   // ahead and to the right
		{ -280.0f,  320.0f,  -30.0f },   // ahead and to the left
		{   10.0f,  900.0f,    5.0f },   // straight ahead
		{ 1200.0f, 1200.0f,  500.0f },   // far off to the side -> clamped to the edge
		{   50.0f,   50.0f,   20.0f },   // close
	};

	for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); ++i)
	{
		NameEsp::ScreenPoint got;
		const bool ok = NameEsp::ProjectWorldToScreen(view, vp, points[i], got);

		float wantX = 0.0f, wantY = 0.0f;
		bool  wantInView = false;
		const bool wantOk = OracleProject(view, vp, points[i], wantX, wantY, wantInView);

		char what[128];
		snprintf(what, sizeof(what), "point %zu projects", i);
		CHECK_INT(ok ? 1 : 0, wantOk ? 1 : 0, what);
		if (ok && wantOk)
		{
			snprintf(what, sizeof(what), "point %zu x (%.1f %.1f %.1f)", i, points[i][0], points[i][1], points[i][2]);
			CHECK_NEAR(got.x, wantX, 0.01, what);
			snprintf(what, sizeof(what), "point %zu y (%.1f %.1f %.1f)", i, points[i][0], points[i][1], points[i][2]);
			CHECK_NEAR(got.y, wantY, 0.01, what);
			snprintf(what, sizeof(what), "point %zu in-view flag", i);
			CHECK_INT(got.inView ? 1 : 0, wantInView ? 1 : 0, what);
		}
	}

	// dead centre of the view lands dead centre of the viewport
	{
		const float ahead[3] = { fwd[0] * 500.0f, fwd[1] * 500.0f, fwd[2] * 500.0f };
		NameEsp::ScreenPoint p;
		CHECK_TRUE(NameEsp::ProjectWorldToScreen(view, vp, ahead, p), "a point straight ahead projects");
		CHECK_NEAR(p.x, 400.0f, 0.5, "straight ahead -> viewport centre x");
		CHECK_NEAR(p.y, 300.0f, 0.5, "straight ahead -> viewport centre y");
		CHECK_INT(p.inView ? 1 : 0, 1, "straight ahead is in view");
	}

	// behind the viewer has no screen position at all
	{
		const float behind[3] = { -fwd[0] * 500.0f, -fwd[1] * 500.0f, -fwd[2] * 500.0f };
		NameEsp::ScreenPoint p;
		CHECK_TRUE(!NameEsp::ProjectWorldToScreen(view, vp, behind, p), "behind the viewer is rejected");
	}

	// a letterboxed viewport shifts the overlay by the viewport origin
	{
		const NameEsp::Viewport boxed = { 40, 25, 800, 600 };
		const float ahead[3] = { fwd[0] * 500.0f, fwd[1] * 500.0f, fwd[2] * 500.0f };
		NameEsp::ScreenPoint p;
		CHECK_TRUE(NameEsp::ProjectWorldToScreen(view, boxed, ahead, p), "viewport offset projects");
		CHECK_NEAR(p.x, 40.0f + 400.0f, 0.5, "viewport x offset applied");
		CHECK_NEAR(p.y, 600.0f - 25.0f - 300.0f, 0.5, "viewport y offset applied");
	}

	// guards
	{
		NameEsp::ScreenPoint p;
		NameEsp::View bad = view;
		bad.valid = false;
		CHECK_TRUE(!NameEsp::ProjectWorldToScreen(bad, vp, points[0], p), "invalid view rejected");
		const NameEsp::Viewport tiny = { 0, 0, 0, 0 };
		CHECK_TRUE(!NameEsp::ProjectWorldToScreen(view, tiny, points[0], p), "empty viewport rejected");
	}
}

static void TestWorldThenHudCapture()
{
	Section("world camera survives active-play HUD render scenes");
	FakeEngine::Reset();
	NameEsp::Reset();
	const float origin[3] = { 1000.0f, 2000.0f, 0.0f };
	const float zero[3] = { 0.0f, 0.0f, 0.0f };
	const float eye[3] = { 1000.0f, 2000.0f, 26.0f };
	const float enemy[3] = { 1500.0f, 2000.0f, 0.0f };
	FakeEngine::SetSnapshotTime(5000);
	FakeEngine::SetLocalPlayer(0, origin, zero, zero, 26);
	FakeEngine::SetPlayer(0, "\\n\\Self\\t\\0", origin);
	FakeEngine::SetPlayer(1, "\\n\\Enemy\\t\\0", enemy);

	q3::refdef_t world, hud, captured = {};
	MakeRefdef(world, eye, zero, 90.0f);
	MakeRefdef(hud, zero, zero, 30.0f);
	hud.width = hud.height = 32;
	hud.rdflags = q3::kRdfNoWorldModel | 4; // bit test, not equality
	hud.time = world.time + 10;
	CHECK_TRUE(!NameEsp::CaptureWorldRefdef(hud, captured), "HUD-only frame cannot initialise world capture");
	CHECK_TRUE(captured.width == 0, "rejected capture leaves destination untouched");
	CHECK_TRUE(NameEsp::CaptureWorldRefdef(world, captured), "spectator/world scene captured");
	for (int i = 0; i < 3; ++i)
		CHECK_TRUE(!NameEsp::CaptureWorldRefdef(hud, captured), "active HUD model does not replace camera");
	CHECK_TRUE(memcmp(&world, &captured, sizeof(world)) == 0, "world camera and frame time preserved exactly");
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), &captured), "gather after world plus HUD");
	CHECK_TRUE(NameEsp::Current().usedRefdef, "uses retained world camera");
	CHECK_TRUE(NameEsp::Current().playerCount == 1, "self skip keeps the following opponent");
	NameEsp::ScreenPoint point;
	const NameEsp::Viewport vp = { 0, 0, 800, 600 };
	CHECK_TRUE(NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp,
	           NameEsp::Current().players[0].origin, point) && point.inView,
	           "opponent remains on screen after HUD renders");
	CHECK_NEAR(point.x, 400.0f, 0.01, "opponent centred using world camera");
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), &hud), "HUD passed directly still allows fallback");
	CHECK_TRUE(!NameEsp::Current().usedRefdef, "defensive view check rejects HUD camera");
	world.vieworg[0] += 20.0f;
	world.time += 16;
	CHECK_TRUE(NameEsp::CaptureWorldRefdef(world, captured), "next world frame refreshes capture");
	CHECK_NEAR(captured.vieworg[0], world.vieworg[0], 0.001, "camera is not frozen by HUD rejection");
}

static void TestTeamColors()
{
	Section("NameEsp::TeamColor");

	unsigned char rgb[3];
	NameEsp::TeamColor(NameEsp::TeamRed, rgb);
	CHECK_TRUE(rgb[0] > rgb[2], "red team reads red");
	NameEsp::TeamColor(NameEsp::TeamBlue, rgb);
	CHECK_TRUE(rgb[2] > rgb[0], "blue team reads blue");
	NameEsp::TeamColor(NameEsp::TeamSpectator, rgb);
	CHECK_TRUE(rgb[0] == rgb[1] && rgb[1] == rgb[2], "spectator is grey");
	NameEsp::TeamColor(99, rgb);
	unsigned char free_[3];
	NameEsp::TeamColor(NameEsp::TeamFree, free_);
	CHECK_TRUE(rgb[0] == free_[0] && rgb[1] == free_[1] && rgb[2] == free_[2],
	           "out of range team falls back to the free-for-all colour");
}

// =============================================================================================== //
// DISTANCE ESP: the "128M" text, the distance fade, and the row stack the head-anchored ESP
// overlays share

// =============================================================================================== //
static void TestDistanceEsp()
{
	Section("DISTANCE ESP - text format, distance fade, row stack");

	// "128M": the rounded metre count + "M", digits and M only, so the GL::Font display
	// lists (glyphs 32..127) always hold it
	char buf[8];
	CHECK_TRUE(DistanceEsp::FormatDistance(128.0f, buf), "whole distance formats");
	CHECK_STR(buf, "128M", "whole distance keeps the number + M");
	CHECK_TRUE(DistanceEsp::FormatDistance(128.4f, buf), "sub-metre distance formats");
	CHECK_STR(buf, "128M", "rounds to the nearest metre");
	CHECK_TRUE(DistanceEsp::FormatDistance(128.5f, buf), "half metre formats");
	CHECK_STR(buf, "129M", "half rounds up");
	CHECK_TRUE(DistanceEsp::FormatDistance(0.4f, buf), "small distance formats");
	CHECK_STR(buf, "0M", "under half a metre is zero");
	CHECK_TRUE(DistanceEsp::FormatDistance(9999.6f, buf), "large distance formats");
	CHECK_STR(buf, "10000M", "five digits + M fits the buffer");
	CHECK_TRUE(!DistanceEsp::FormatDistance(-1.0f, buf), "negative rejected");
	CHECK_TRUE(!DistanceEsp::FormatDistance(0.0f / 0.0f, buf), "NaN rejected");

	// the fade: full size and opacity up close, min scale / alpha 0 far away
	{
		float scale = 0.0f, alpha = 0.0f;
		DistanceEsp::DistanceFade(0.0f, scale, alpha);
		CHECK_NEAR(scale, 1.0f, 1e-6, "close: full scale");
		CHECK_NEAR(alpha, 1.0f, 1e-6, "close: full alpha");
		DistanceEsp::DistanceFade(DistanceEsp::kFadeStartDist, scale, alpha);
		CHECK_NEAR(scale, 1.0f, 1e-6, "fade start: full scale");
		CHECK_NEAR(alpha, 1.0f, 1e-6, "fade start: full alpha");
		DistanceEsp::DistanceFade(DistanceEsp::kFadeEndDist, scale, alpha);
		CHECK_NEAR(scale, DistanceEsp::kMinScale, 1e-6, "fade end: min scale");
		CHECK_NEAR(alpha, 0.0f, 1e-6, "fade end: alpha 0");
		DistanceEsp::DistanceFade(DistanceEsp::kFadeEndDist + 5000.0f, scale, alpha);
		CHECK_NEAR(scale, DistanceEsp::kMinScale, 1e-6, "past the fade end holds the min scale");
		CHECK_NEAR(alpha, 0.0f, 1e-6, "past the fade end holds alpha 0");
		DistanceEsp::DistanceFade((DistanceEsp::kFadeStartDist + DistanceEsp::kFadeEndDist) * 0.5f,
		                         scale, alpha);
		CHECK_TRUE(scale > DistanceEsp::kMinScale && scale < 1.0f, "mid fade interpolates scale");
		CHECK_TRUE(alpha > 0.0f && alpha < 1.0f, "mid fade interpolates alpha");
	}

	// the row stack: the distance only steps down for the name
	{
		NameEsp::EspRows r;

		r = NameEsp::ComputeEspRows(false);   // distance alone
		CHECK_NEAR(r.distance, 0.0f, 1e-6, "distance alone sits on the head anchor");

		r = NameEsp::ComputeEspRows(true);    // name + distance
		CHECK_NEAR(r.name, 0.0f, 1e-6, "name on top of the anchor");
		CHECK_NEAR(r.distance, NameEsp::kEspRowHeight, 1e-6,
		           "distance one row under the name, never overlapping it");
	}

	// the value the fade is driven by: |cg.refdef.vieworg - cent->lerpOrigin|, gathered per tag
	{
		const float here[3]   = { 0.0f, 0.0f, 0.0f };
		const float none[3]   = { 0.0f, 0.0f, 0.0f };
		const float angles[3] = { 0.0f, 0.0f, 0.0f };
		const float botAt[3]  = { 300.0f, 400.0f, 8.0f };

		FakeEngine::Reset();
		NameEsp::Reset();
		FakeEngine::SetSnapshotTime(1000);
		FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
		FakeEngine::SetPlayer(1, "\\n\\FarOut\\t\\0", botAt);
		CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "frame gathered");
		CHECK_INT(NameEsp::Current().playerCount, 1, "one tag");
		const NameEsp::PlayerTag& tag = NameEsp::Current().players[0];
		const float want = DistanceBetween(NameEsp::Current().view.origin, tag.lerpOrigin);
		CHECK_NEAR(tag.distance, want, 0.05, "tag distance is |vieworg - lerpOrigin|");
		// view at (0,0,26), bot feet at (300,400,8): sqrt(300^2 + 400^2 + 18^2) = 500.32...
		CHECK_NEAR(tag.distance, sqrtf(300.0f * 300.0f + 400.0f * 400.0f + 18.0f * 18.0f), 0.1,
		         "the worked value");
		char text[8];
		CHECK_TRUE(DistanceEsp::FormatDistance(tag.distance, text), "the gathered distance formats");
		CHECK_STR(text, "500M", "and it is the rounded metre count + M");
	}
}

// =============================================================================================== //
// WEAPON ESP - the portable half (weaponEspCore.cpp)
// =============================================================================================== //

static void TestWeaponNumberGather()
{
	Section("PlayerTag::weapon - the snapshot's weapon field reaches the tag");

	const float here[3]   = { 0.0f, 0.0f, 0.0f };
	const float none[3]   = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\^1Bitterman^7\\t\\1", (const float[]){ 128.0f, 0.0f, 0.0f });
	FakeEngine::SetPlayer(2, "\\n\\Slash\\t\\2",         (const float[]){ 0.0f, -128.0f, 0.0f });
	FakeEngine::SetPlayerWeapon(1, 4);   // WP_GRENADE_LAUNCHER
	FakeEngine::SetPlayerWeapon(2, 2);   // WP_MACHINEGUN

	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "frame gathered");
	const NameEsp::Frame& frame = NameEsp::Current();
	CHECK_INT(frame.playerCount, 2, "two players tagged");

	int found1 = -1, found2 = -1;
	for (int i = 0; i < frame.playerCount; ++i)
	{
		if (frame.players[i].clientNum == 1) found1 = i;
		if (frame.players[i].clientNum == 2) found2 = i;
	}
	CHECK_INT(found1, 0, "client 1 tagged (first other player in entity order)");
	CHECK_INT(found2, 1, "client 2 tagged");
	if (found1 >= 0 && found2 >= 0)
	{
		CHECK_INT(frame.players[found1].weapon, 4, "client 1 weapon is the snapshot's (4)");
		CHECK_INT(frame.players[found2].weapon, 2, "client 2 weapon is the snapshot's (2)");
	}

	// a mod weapon number outside the stock set still reaches the tag untouched - resolving it
	// to a display string is the ESP's job, not Gather's
	FakeEngine::NewServerFrame(1050);
	FakeEngine::SetPlayer(1, "\\n\\^1Bitterman^7\\t\\1", (const float[]){ 128.0f, 0.0f, 0.0f });
	FakeEngine::SetPlayer(2, "\\n\\Slash\\t\\2",         (const float[]){ 0.0f, -128.0f, 0.0f });
	FakeEngine::SetPlayerWeapon(1, 15);
	FakeEngine::SetPlayerWeapon(2, 2);
	CHECK_TRUE(NameEsp::Gather(1050, FakeEngine::Syscall()), "frame gathered (weapon 15)");
	const NameEsp::Frame& frame2 = NameEsp::Current();
	for (int i = 0; i < frame2.playerCount; ++i)
		if (frame2.players[i].clientNum == 1)
			CHECK_INT(frame2.players[i].weapon, 15, "an out-of-stock weapon number is preserved");
}

static void TestWeaponStockTable()
{
	Section("WeaponEsp::StockTable - the built-in 1.32 names and icons");

	WeaponEsp::WeaponTable table;
	WeaponEsp::StockTable(table);

	CHECK_INT(table.weaponCount, 13, "thirteen stock weapons listed");
	CHECK_TRUE(table.haveName[1]  && strcmp(table.weapons[1].name,  "Gauntlet") == 0, "1 = Gauntlet");
	CHECK_TRUE(table.haveName[2]  && strcmp(table.weapons[2].name,  "Machinegun") == 0, "2 = Machinegun");
	CHECK_TRUE(table.haveName[4]  && strcmp(table.weapons[4].name,  "Grenade Launcher") == 0, "4 = Grenade Launcher");
	CHECK_TRUE(table.haveName[6]  && strcmp(table.weapons[6].name,  "Lightning Gun") == 0, "6 = Lightning Gun");
	CHECK_TRUE(table.haveName[10] && strcmp(table.weapons[10].name, "Grappling Hook") == 0, "10 = Grappling Hook");
	CHECK_TRUE(table.haveIcon[2]  && strcmp(table.weapons[2].icon,  "icons/iconw_machinegun") == 0, "icon 2");
	CHECK_TRUE(table.haveIcon[4]  && strcmp(table.weapons[4].icon,  "icons/iconw_grenade") == 0, "icon 4 (iconw_grenade)");
	CHECK_TRUE(table.haveIcon[5]  && strcmp(table.weapons[5].icon,  "icons/iconw_rocket") == 0, "icon 5 (iconw_rocket)");
	CHECK_TRUE(!table.haveName[0], "WP_NONE (0) has no name");

	char name[64];
	CHECK_TRUE(WeaponEsp::WeaponName(table, 2, name, sizeof(name)) && strcmp(name, "Machinegun") == 0,
	           "WeaponName returns the table name");
	CHECK_TRUE(!WeaponEsp::WeaponName(table, 0, name, sizeof(name)), "WeaponName: WP_NONE -> nothing to show");
	CHECK_TRUE(WeaponEsp::WeaponName(table, 14, name, sizeof(name)) && strcmp(name, "W14") == 0,
	           "WeaponName: an unlisted in-range number falls back to W<number>");
	CHECK_TRUE(WeaponEsp::WeaponName(table, 99, name, sizeof(name)) && strcmp(name, "W99") == 0,
	           "WeaponName: an out-of-range number still gets a tag (W<number>)");

	char icon[64];
	CHECK_TRUE(WeaponEsp::WeaponIcon(table, 2, icon, sizeof(icon)) && strcmp(icon, "icons/iconw_machinegun") == 0,
	           "WeaponIcon returns the table icon");
	CHECK_TRUE(!WeaponEsp::WeaponIcon(table, 14, icon, sizeof(icon)), "WeaponIcon: unlisted -> none");
	CHECK_TRUE(!WeaponEsp::WeaponIcon(table, 0, icon, sizeof(icon)), "WeaponIcon: WP_NONE -> none");
}

// -------------------------------------------------------------------------------------------
// The shape scan, driven with a fabricated cgame data segment: the exact bytes a bytecode cgame
// VM's hunk segment would hold - strings first, then bg_itemlist[] with 32-bit offset pointers,
// exactly the way the qvm's .data section lays them out.
// ------------------------------------------------------------------------------------------- //
namespace
{
	// write a u32 field at an absolute offset of the segment
	void PutU32(unsigned char* buf, size_t off, uint32_t v)
	{
		buf[off]     = (unsigned char)(v & 0xFF);
		buf[off + 1] = (unsigned char)((v >> 8) & 0xFF);
		buf[off + 2] = (unsigned char)((v >> 16) & 0xFF);
		buf[off + 3] = (unsigned char)((v >> 24) & 0xFF);
	}

	// one gitem_t entry at `base` with the stock 1.32 offsets; NULL pointers stay 0
	void PutItem132(unsigned char* buf, size_t base, uint32_t classname, uint32_t pickup,
	               uint32_t icon, int quantity, int giType, int giTag, uint32_t precaches, uint32_t sounds)
	{
		PutU32(buf, base + 0,  classname);
		PutU32(buf, base + 4,  0);               // pickup_sound
		PutU32(buf, base + 8,  0);               // world_model[0..3]
		PutU32(buf, base + 24, icon);
		PutU32(buf, base + 28, pickup);
		PutU32(buf, base + 32, (uint32_t)quantity);
		PutU32(buf, base + 36, (uint32_t)giType);
		PutU32(buf, base + 40, (uint32_t)giTag);
		PutU32(buf, base + 44, precaches);
		PutU32(buf, base + 48, sounds);
	}

	// the same entry with the ioquake3 1.36+ layout (stride 72)
	void PutItemIoq(unsigned char* buf, size_t base, uint32_t classname, uint32_t pickup,
	                uint32_t icon, int quantity, int giType, int giTag, uint32_t precaches, uint32_t sounds)
	{
		PutU32(buf, base + 0,  classname);
		PutU32(buf, base + 4,  0);
		PutU32(buf, base + 8,  0);
		PutU32(buf, base + 24, icon);
		PutU32(buf, base + 28, pickup);
		PutU32(buf, base + 32, (uint32_t)quantity);
		PutU32(buf, base + 36, (uint32_t)giType);
		PutU32(buf, base + 40, (uint32_t)giTag);
		PutU32(buf, base + 44, 0);               // giFlags
		PutU32(buf, base + 48, 0);               // giFlags2
		PutU32(buf, base + 52, 0);               // pickup_sound2
		PutU32(buf, base + 56, precaches);
		PutU32(buf, base + 60, sounds);
		PutU32(buf, base + 64, 0);               // use_func
		PutU32(buf, base + 68, 0);               // pmove_frame
	}

	// a NUL-terminated string at an offset; returns its offset (the pointer value to store).
	// (strncpy is deliberately NOT used: it pads the rest of the field with NULs, which would
	// clobber the next string in the segment.)
	uint32_t PutStr(unsigned char* buf, size_t off, const char* s)
	{
		const size_t n = strlen(s);
		memcpy(buf + off, s, n);
		buf[off + n] = 0;
		return (uint32_t)off;
	}

	// The fabricated segment: 1KB of non-zero garbage, the strings, the table, then more
	// garbage that must not look like a second table. Returns the table's offset.
	size_t BuildFakeCgameSegment(unsigned char* buf, size_t size, bool ioqLayout)
	{
		memset(buf, 0xA5, size);               // non-zero padding: never a zero prefilter
		size_t off = 0x100;
		const uint32_t sGauntletC    = PutStr(buf, off += 32, "weapon_gauntlet");
		const uint32_t sGauntletP    = PutStr(buf, off += 32, "Gauntlet");
		const uint32_t sGauntletI    = PutStr(buf, off += 48, "icons/iconw_gauntlet");
		const uint32_t sMachineC     = PutStr(buf, off += 40, "weapon_machinegun");
		const uint32_t sMachineP     = PutStr(buf, off += 32, "Machinegun");
		const uint32_t sMachineI     = PutStr(buf, off += 48, "icons/iconw_machinegun");
		const uint32_t sCustomC      = PutStr(buf, off += 32, "weapon_custom");
		const uint32_t sCustomI      = PutStr(buf, off += 40, "icons/iconw_custom");
		// a total-conversion weapon numbered past the stock range (MAX_WEAPONS 16): a TC that
		// recompiles bg_public.h with more weapons still ships the number over the wire
		const uint32_t sTcC          = PutStr(buf, off += 40, "weapon_tc_heavy");
		const uint32_t sTcP          = PutStr(buf, off += 40, "TC Heavy Cannon");
		const uint32_t sTcI          = PutStr(buf, off += 48, "icons/iconw_tc_heavy");
		const uint32_t sArmorC       = PutStr(buf, off += 40, "item_armor_shard");
		const uint32_t sArmorP       = PutStr(buf, off += 40, "Armor Shard");
		const uint32_t sEmpty        = PutStr(buf, off += 4,  "");
		(void)sEmpty;

		const size_t stride = ioqLayout ? 72 : 52;
		const size_t table  = 0x400;
		for (size_t i = 0; i < 16; ++i)
		{
			size_t e = table + i * stride;
			// entries 5..15 are all-zero records: the region started as 0xA5 padding, and the
			// run validation must see valid (empty) gitem_t entries, not garbage
			memset(buf + e, 0, stride);
			if (ioqLayout)
			{
				if (i == 0)
				{
					PutItemIoq(buf, e, 0, 0, 0, 0, 0, 0, sEmpty, sEmpty);
				}
				else if (i == 1)
				{
					PutItemIoq(buf, e, sArmorC, sArmorP, 0, 5, 2 /* IT_ARMOR */, 0, sEmpty, sEmpty);
				}
				else if (i == 2)
				{
					PutItemIoq(buf, e, sGauntletC, sGauntletP, sGauntletI, 0, 1 /* IT_WEAPON */, 1, sEmpty, sEmpty);
				}
				else if (i == 3)
				{
					PutItemIoq(buf, e, sMachineC, sMachineP, sMachineI, 0, 1, 2, sEmpty, sEmpty);
				}
				else if (i == 4)
				{
					// no pickup name: the classname must be used instead
					PutItemIoq(buf, e, sCustomC, 0, sCustomI, 0, 1, 5, sEmpty, sEmpty);
				}
				else if (i == 5)
				{
					// a TC weapon numbered past the stock 16: the number must survive the scan
					PutItemIoq(buf, e, sTcC, sTcP, sTcI, 0, 1, 19, sEmpty, sEmpty);
				}
				// entries 6..15: all-zero records (valid gitem_t shape, no strings)
			}
			else
			{
				if (i == 0)
				{
					PutItem132(buf, e, 0, 0, 0, 0, 0, 0, sEmpty, sEmpty);
				}
				else if (i == 1)
				{
					PutItem132(buf, e, sArmorC, sArmorP, 0, 5, 2 /* IT_ARMOR */, 0, sEmpty, sEmpty);
				}
				else if (i == 2)
				{
					PutItem132(buf, e, sGauntletC, sGauntletP, sGauntletI, 0, 1 /* IT_WEAPON */, 1, sEmpty, sEmpty);
				}
				else if (i == 3)
				{
					PutItem132(buf, e, sMachineC, sMachineP, sMachineI, 0, 1, 2, sEmpty, sEmpty);
				}
				else if (i == 4)
				{
					PutItem132(buf, e, sCustomC, 0, sCustomI, 0, 1, 5, sEmpty, sEmpty);
				}
				else if (i == 5)
				{
					PutItem132(buf, e, sTcC, sTcP, sTcI, 0, 1, 19, sEmpty, sEmpty);
				}
			}
		}
		// after the table: garbage whose ints fail the entry shape, so a scan that starts part
		// way into the real table cannot "recover" and find a second one
		for (size_t i = table + 16 * stride; i + 4 < size; i += 4)
			PutU32(buf, i, 0xDEADBEEFu);
		return table;
	}
}

static void TestWeaponScannerStockLayout()
{
	Section("WeaponEsp::ScanRegion - finds a stock 1.32 bg_itemlist in a fake cgame segment");

	enum { kSegSize = 4096 };
	unsigned char buf[kSegSize];
	const size_t tableOff = BuildFakeCgameSegment(buf, kSegSize, false);

	// The pointer values are offsets into the segment - a bytecode VM's masked addressing. The
	// mask must cover the whole segment, so size it as a power of two.
	const uint32_t mask = kSegSize - 1;

	WeaponEsp::ScanResult result;
	CHECK_TRUE(WeaponEsp::ScanRegion(buf, kSegSize, (uintptr_t)buf, mask, false, result),
	           "the table is found");
	CHECK_TRUE(result.found, "found flag set");
	CHECK_INT(result.stride, 52, "the stock gitem_t stride");
	CHECK_INT((int)result.offset, (int)tableOff, "the table's offset in the segment");
	CHECK_INT(result.itemCount, 5, "five entries carry strings (armor + four weapons)");

	CHECK_TRUE(result.table.haveName[1] && strcmp(result.table.weapons[1].name, "Gauntlet") == 0,
	           "weapon 1 name = pickup_name (Gauntlet)");
	CHECK_TRUE(result.table.haveIcon[1] && strcmp(result.table.weapons[1].icon, "icons/iconw_gauntlet") == 0,
	           "weapon 1 icon");
	CHECK_TRUE(result.table.haveName[2] && strcmp(result.table.weapons[2].name, "Machinegun") == 0,
	           "weapon 2 name (Machinegun)");
	CHECK_TRUE(result.table.haveName[5] && strcmp(result.table.weapons[5].name, "weapon_custom") == 0,
	           "weapon 5 falls back to the classname when the pickup name is missing");
	CHECK_TRUE(result.table.haveIcon[5] && strcmp(result.table.weapons[5].icon, "icons/iconw_custom") == 0,
	           "weapon 5 icon");
	// a TC weapon numbered past the stock MAX_WEAPONS 16: the mod's own name and icon survive
	CHECK_TRUE(result.table.haveName[19] && strcmp(result.table.weapons[19].name, "TC Heavy Cannon") == 0,
	           "weapon 19 (past the stock range) keeps the mod's pickup name");
	CHECK_TRUE(result.table.haveIcon[19] && strcmp(result.table.weapons[19].icon, "icons/iconw_tc_heavy") == 0,
	           "weapon 19 keeps the mod's icon");
	CHECK_INT(result.table.weaponCount, 4, "four weapons in the table");

	char name[64];
	CHECK_TRUE(WeaponEsp::WeaponName(result.table, 19, name, sizeof(name)) && strcmp(name, "TC Heavy Cannon") == 0,
	           "WeaponName resolves a TC weapon number through the scanned table");
	CHECK_TRUE(WeaponEsp::WeaponName(result.table, 1, name, sizeof(name)) && strcmp(name, "Gauntlet") == 0,
	           "WeaponName through the scanned table");
}

static void TestWeaponScannerIoqLayout()
{
	Section("WeaponEsp::ScanRegion - finds the ioquake3 72-byte gitem_t layout too");

	enum { kSegSize = 4096 };
	unsigned char buf[kSegSize];
	const size_t tableOff = BuildFakeCgameSegment(buf, kSegSize, true);
	// A nonzero giFlags (the ioq-only field) makes the 48-byte interpretation fail its string
	// checks, so this really exercises the 72-byte path instead of the stock scan reading the
	// shared head and winning first.
	PutU32(buf, tableOff + 1 * 72 + 44, 1);

	WeaponEsp::ScanResult result;
	CHECK_TRUE(WeaponEsp::ScanRegion(buf, kSegSize, (uintptr_t)buf, kSegSize - 1, false, result),
	           "the ioq-layout table is found");
	CHECK_INT(result.stride, 72, "the ioq gitem_t stride");
	CHECK_INT((int)result.offset, (int)tableOff, "the table's offset in the segment");
	CHECK_TRUE(result.table.haveName[1] && strcmp(result.table.weapons[1].name, "Gauntlet") == 0,
	           "weapon 1 name survives the wider layout");
	CHECK_TRUE(result.table.haveName[5] && strcmp(result.table.weapons[5].name, "weapon_custom") == 0,
	           "weapon 5 classname fallback survives the wider layout");
	CHECK_TRUE(result.table.haveName[19] && strcmp(result.table.weapons[19].name, "TC Heavy Cannon") == 0,
	           "a TC weapon number past the stock range survives the wider layout");
	CHECK_INT(result.table.weaponCount, 4, "four weapons in the ioq table");
}

static void TestWeaponScannerNegatives()
{
	Section("WeaponEsp::ScanRegion - rejects what is not a table");

	enum { kSegSize = 4096 };
	unsigned char buf[kSegSize];

	// pure garbage: no entry-0 prefilter can pass, nothing is found
	memset(buf, 0xA5, kSegSize);
	WeaponEsp::ScanResult result;
	CHECK_TRUE(!WeaponEsp::ScanRegion(buf, kSegSize, (uintptr_t)buf, kSegSize - 1, false, result),
	           "garbage is not a table");

	// a table whose later entries are corrupted: the run validation must reject it
	const size_t tableOff = BuildFakeCgameSegment(buf, kSegSize, false);
	PutU32(buf, tableOff + 6 * 52 + 36, 77 /* giType out of range */);
	CHECK_TRUE(!WeaponEsp::ScanRegion(buf, kSegSize, (uintptr_t)buf, kSegSize - 1, false, result),
	           "a corrupted run is not a table");

	// a table with only two weapons: the acceptance threshold must reject it (drop two of the
	// four weapon rows, leaving machinegun + the TC weapon)
	BuildFakeCgameSegment(buf, kSegSize, false);
	PutU32(buf, tableOff + 2 * 52 + 36, 0 /* entry 2: not a weapon anymore */);
	PutU32(buf, tableOff + 4 * 52 + 36, 0 /* entry 4: not a weapon anymore */);
	CHECK_TRUE(!WeaponEsp::ScanRegion(buf, kSegSize, (uintptr_t)buf, kSegSize - 1, false, result),
	           "fewer than three weapons is not a table");

	// a pointer that runs off the end of the segment (no NUL in sight)
	BuildFakeCgameSegment(buf, kSegSize, false);
	PutU32(buf, tableOff + 2 * 52 + 28, kSegSize - 8 /* pickup_name points at the last bytes */);
	CHECK_TRUE(!WeaponEsp::ScanRegion(buf, kSegSize, (uintptr_t)buf, kSegSize - 1, false, result),
	           "a dangling string pointer is not a table");
}

static void TestWeaponLegAnchor()
{
	Section("WeaponEsp::LegAnchor - mid-leg, below the feet the other ESPs anchor above");

	NameEsp::PlayerTag tag;
	memset(&tag, 0, sizeof(tag));
	tag.lerpOrigin[0] = 100.0f;
	tag.lerpOrigin[1] = -50.0f;
	tag.lerpOrigin[2] = 10.0f;

	float leg[3];
	WeaponEsp::LegAnchor(tag, leg);
	CHECK_TRUE(leg[0] == 100.0f && leg[1] == -50.0f, "x/y are the interpolated origin");
	CHECK_TRUE(fabsf(leg[2] - (10.0f + q3::kWeaponEspLegHeight)) < 1e-6f,
	           "z is the origin + the leg height (8 units above the feet)");

	// and the head anchor the other ESPs use sits 28 units above it - a whole model between
	// the two anchors, which is what keeps the two ESP stacks from ever sharing a screen row
	CHECK_TRUE(fabsf((q3::kPlayerTagHeight + 10.0f) - (q3::kWeaponEspLegHeight + 10.0f) - 28.0f) < 1e-6f,
	           "head anchor (+36) and leg anchor (+8) are 28 units apart");
}

static void TestWeaponFadeMatchesOtherEsp()
{
	Section("Weapon ESP fade - the same ramp as the distance ESP");

	float scale = 0.0f, alpha = 0.0f;
	DistanceEsp::DistanceFade(100.0f, scale, alpha);
	CHECK_TRUE(scale == 1.0f && alpha == 1.0f, "full size and opacity close in");
	DistanceEsp::DistanceFade(2500.0f, scale, alpha);
	CHECK_TRUE(scale == DistanceEsp::kMinScale && alpha == 0.0f, "gone at the fade end");
	DistanceEsp::DistanceFade(1450.0f, scale, alpha);   // midpoint of 400..2500
	CHECK_TRUE(alpha > 0.49f && alpha < 0.51f, "half faded at the midpoint");
	CHECK_TRUE(scale > 0.67f && scale < 0.69f, "half scaled at the midpoint");
}

// The deflate fixtures: zlib 1.2.13, raw -15 windows, generated from MakeIcon()'s bytes (so
// a test can inflate one and compare it against the TGA it builds itself). They cover the
// three DEFLATE block types, and TestWeaponIconPakRead() asserts each fixture's block type
// so a regeneration with different settings cannot quietly drop one. Regenerate with
// tests/gen_icon_fixtures.py (kept next to the tests for exactly that).

// 32x32, level 9: what a pak packager produces - a DYNAMIC Huffman block.
static const unsigned char kDeflateIconDynamic[] = {
	0xed, 0x97, 0x1f, 0x97, 0x34, 0xbb, 0x16, 0x87, 0xcf, 0xba, 0x9f, 0xa2,
	0xb1, 0xb0, 0xb0, 0xe0, 0x85, 0x82, 0x81, 0x82, 0x81, 0x82, 0x81, 0x40,
	0x43, 0xa0, 0x21, 0xd0, 0x10, 0x68, 0x08, 0x34, 0x04, 0x1a, 0x02, 0x0d,
	0x81, 0x86, 0x40, 0x43, 0xa0, 0x21, 0xd0, 0x10, 0x68, 0x08, 0x0c, 0x04,
	0x06, 0x0a, 0x06, 0x0a, 0x5e, 0x28, 0x2c, 0x6c, 0x9c, 0x6f, 0xf1, 0xdc,
	0xb5, 0xee, 0x5a, 0xd7, 0xe6, 0xfc, 0xbd, 0xe7, 0xda, 0xf9, 0xed, 0x0f,
	0xf0, 0x3c, 0xb4, 0xf7, 0x6f, 0xff, 0xf2, 0xcb, 0xbf, 0x7e, 0xf9, 0x6f,
	0x9a, 0xff, 0xcc, 0x3f, 0xf9, 0xe3, 0x49, 0x5f, 0x92, 0xfb, 0x97, 0x22,
	0x7f, 0x69, 0x1e, 0x5f, 0x86, 0xf2, 0x65, 0x79, 0xff, 0x72, 0xd4, 0x2f,
	0xcf, 0xc7, 0x57, 0x60, 0xfa, 0x8a, 0x7c, 0x7e, 0x25, 0xe6, 0xaf, 0xcc,
	0xdf, 0xc9, 0xbd, 0x3e, 0x47, 0xe2, 0x53, 0x70, 0x7b, 0x4a, 0xd2, 0x53,
	0x71, 0x7f, 0x6a, 0xf2, 0xd3, 0xf0, 0x78, 0x5a, 0xca, 0xd3, 0xf1, 0xfe,
	0xf4, 0xd4, 0x67, 0xe0, 0xe3, 0x19, 0x99, 0x9e, 0x89, 0xcf, 0x67, 0x66,
	0x7e, 0x16, 0x7e, 0x3e, 0xeb, 0xff, 0xec, 0x71, 0x59, 0x07, 0xc2, 0x3a,
	0x72, 0x5d, 0x05, 0x71, 0x95, 0xdc, 0x56, 0x45, 0x5a, 0x35, 0xf7, 0xd5,
	0x90, 0x57, 0xcb, 0x63, 0x75, 0x94, 0xd5, 0xf3, 0xbe, 0x06, 0xea, 0x1a,
	0xf9, 0x58, 0x13, 0xd3, 0x9a, 0xf9, 0x5c, 0x0b, 0xf3, 0x5a, 0xf9, 0xb9,
	0x4e, 0x7f, 0xd9, 0xe1, 0xbc, 0xf4, 0xf8, 0x65, 0xe0, 0xb2, 0x8c, 0x84,
	0x45, 0x70, 0x5d, 0x24, 0x71, 0x51, 0xdc, 0x16, 0x4d, 0x5a, 0x0c, 0xf7,
	0xc5, 0x92, 0x17, 0xc7, 0x63, 0xf1, 0x94, 0x25, 0xf0, 0xbe, 0x44, 0xea,
	0x92, 0xf8, 0x58, 0x32, 0xd3, 0x52, 0xf8, 0x5c, 0x2a, 0xf3, 0x32, 0xf1,
	0x73, 0x99, 0xff, 0xb4, 0xc3, 0x69, 0xee, 0x70, 0x73, 0xcf, 0x79, 0x1e,
	0xf0, 0xf3, 0xc8, 0x65, 0x16, 0x84, 0x59, 0x72, 0x9d, 0x15, 0x71, 0xd6,
	0xdc, 0x66, 0x43, 0x9a, 0x2d, 0xf7, 0xd9, 0x91, 0x67, 0xcf, 0x63, 0x0e,
	0x94, 0x39, 0xf2, 0x3e, 0x27, 0xea, 0x9c, 0xf9, 0x98, 0x0b, 0xd3, 0x5c,
	0xf9, 0x9c, 0x27, 0xe6, 0x79, 0xe6, 0xe7, 0xbc, 0xfc, 0x61, 0x87, 0xe3,
	0xd4, 0x62, 0xa7, 0x8e, 0xd3, 0xd4, 0xe3, 0xa6, 0x81, 0xf3, 0x34, 0xe2,
	0x27, 0xc1, 0x65, 0x92, 0x84, 0x49, 0x71, 0x9d, 0x34, 0x71, 0x32, 0xdc,
	0x26, 0x4b, 0x9a, 0x1c, 0xf7, 0xc9, 0x93, 0xa7, 0xc0, 0x63, 0x8a, 0x94,
	0x29, 0xf1, 0x3e, 0x65, 0xea, 0x54, 0xf8, 0x98, 0x2a, 0xd3, 0x34, 0xf1,
	0x39, 0xcd, 0xcc, 0xd3, 0xc2, 0xcf, 0x69, 0xfd, 0x5d, 0x87, 0x43, 0x6d,
	0x30, 0xb5, 0xe5, 0x58, 0x3b, 0x6c, 0xed, 0x39, 0xd5, 0x01, 0x57, 0x47,
	0xce, 0x55, 0xe0, 0xab, 0xe4, 0x52, 0x15, 0xa1, 0x6a, 0xae, 0xd5, 0x10,
	0xab, 0xe5, 0x56, 0x1d, 0xa9, 0x7a, 0xee, 0x35, 0x90, 0x6b, 0xe4, 0x51,
	0x13, 0xa5, 0x66, 0xde, 0x6b, 0xa1, 0xd6, 0xca, 0x47, 0x9d, 0x98, 0xea,
	0xcc, 0x67, 0x5d, 0x98, 0xeb, 0xca, 0xcf, 0xfa, 0xfc, 0x4d, 0x07, 0x5d,
	0x1a, 0x0e, 0xa5, 0xc5, 0x94, 0x8e, 0x63, 0xe9, 0xb1, 0x65, 0xe0, 0x54,
	0x46, 0x5c, 0x11, 0x9c, 0x8b, 0xc4, 0x17, 0xc5, 0xa5, 0x68, 0x42, 0x31,
	0x5c, 0x8b, 0x25, 0x16, 0xc7, 0xad, 0x78, 0x52, 0x09, 0xdc, 0x4b, 0x24,
	0x97, 0xc4, 0xa3, 0x64, 0x4a, 0x29, 0xbc, 0x97, 0x4a, 0x2d, 0x13, 0x1f,
	0x65, 0x66, 0x2a, 0x0b, 0x9f, 0x65, 0x65, 0x2e, 0xbf, 0xce, 0x57, 0x79,
	0xc3, 0x3e, 0x37, 0xe8, 0xdc, 0x72, 0xc8, 0x1d, 0x26, 0xf7, 0x1c, 0xf3,
	0x80, 0xcd, 0x23, 0xa7, 0x2c, 0x70, 0x59, 0x72, 0xce, 0x0a, 0x9f, 0x35,
	0x97, 0x6c, 0x08, 0xd9, 0x72, 0xcd, 0x8e, 0x98, 0x3d, 0xb7, 0x1c, 0x48,
	0x39, 0x72, 0xcf, 0x89, 0x9c, 0x33, 0x8f, 0x5c, 0x28, 0xb9, 0xf2, 0x9e,
	0x27, 0x6a, 0x9e, 0xf9, 0xc8, 0x0b, 0x53, 0x5e, 0xf9, 0xcc, 0x4f, 0xe6,
	0xfc, 0xf5, 0xad, 0xc3, 0x2e, 0x6d, 0x50, 0xa9, 0x61, 0x9f, 0x5a, 0x74,
	0xea, 0x38, 0xa4, 0x1e, 0x93, 0x06, 0x8e, 0x69, 0xc4, 0x26, 0xc1, 0x29,
	0x49, 0x5c, 0x52, 0x9c, 0x93, 0xc6, 0x27, 0xc3, 0x25, 0x59, 0x42, 0x72,
	0x5c, 0x93, 0x27, 0xa6, 0xc0, 0x2d, 0x45, 0x52, 0x4a, 0xdc, 0x53, 0x26,
	0xa7, 0xc2, 0x23, 0x55, 0x4a, 0x9a, 0x78, 0x4f, 0x33, 0x35, 0x2d, 0x7c,
	0xa4, 0x95, 0x29, 0x3d, 0xf9, 0x4c, 0xdf, 0xf3, 0x65, 0xdc, 0xb0, 0x8b,
	0x0d, 0x2a, 0xb6, 0xec, 0x63, 0x87, 0x8e, 0x3d, 0x87, 0x38, 0x60, 0xe2,
	0xc8, 0x31, 0x0a, 0x6c, 0x94, 0x9c, 0xa2, 0xc2, 0x45, 0xcd, 0x39, 0x1a,
	0x7c, 0xb4, 0x5c, 0xa2, 0x23, 0x44, 0xcf, 0x35, 0x06, 0x62, 0x8c, 0xdc,
	0x62, 0x22, 0xc5, 0xcc, 0x3d, 0x16, 0x72, 0xac, 0x3c, 0xe2, 0x44, 0x89,
	0x33, 0xef, 0x71, 0xa1, 0xc6, 0x95, 0x8f, 0xf8, 0x64, 0x8a, 0xdf, 0xf3,
	0xb7, 0x61, 0x83, 0x0c, 0x0d, 0xbb, 0xd0, 0xa2, 0x42, 0xc7, 0x3e, 0xf4,
	0xe8, 0x30, 0x70, 0x08, 0x23, 0x26, 0x08, 0x8e, 0x41, 0x62, 0x83, 0xe2,
	0x14, 0x34, 0x2e, 0x18, 0xce, 0xc1, 0xe2, 0x83, 0xe3, 0x12, 0x3c, 0x21,
	0x04, 0xae, 0x21, 0x12, 0x43, 0xe2, 0x16, 0x32, 0x29, 0x14, 0xee, 0xa1,
	0x92, 0xc3, 0xc4, 0x23, 0xcc, 0x94, 0xb0, 0xf0, 0x1e, 0x56, 0x6a, 0x78,
	0xf2, 0x11, 0xbe, 0xe7, 0x0b, 0xbf, 0x61, 0xeb, 0x1b, 0xa4, 0x6f, 0xd9,
	0xf9, 0x0e, 0xe5, 0x7b, 0xf6, 0x7e, 0x40, 0xfb, 0x91, 0x83, 0x17, 0x18,
	0x2f, 0x39, 0x7a, 0x85, 0xf5, 0x9a, 0x93, 0x37, 0x38, 0x6f, 0x39, 0x7b,
	0x87, 0xf7, 0x9e, 0x8b, 0x0f, 0x04, 0x1f, 0xb9, 0xfa, 0x44, 0xf4, 0x99,
	0x9b, 0x2f, 0x24, 0x5f, 0xb9, 0xfb, 0x89, 0xec, 0x67, 0x1e, 0x7e, 0xa1,
	0xf8, 0x95, 0x77, 0xff, 0xa4, 0xfa, 0xef, 0xf9, 0x6f, 0x6e, 0x83, 0x70,
	0x0d, 0x5b, 0xd7, 0x22, 0x5d, 0xc7, 0xce, 0xf5, 0x28, 0x37, 0xb0, 0x77,
	0x23, 0xda, 0x09, 0x0e, 0x4e, 0x62, 0x9c, 0xe2, 0xe8, 0x34, 0xd6, 0x19,
	0x4e, 0xce, 0xe2, 0x9c, 0xe3, 0xec, 0x3c, 0xde, 0x05, 0x2e, 0x2e, 0x12,
	0x5c, 0xe2, 0xea, 0x32, 0xd1, 0x15, 0x6e, 0xae, 0x92, 0xdc, 0xc4, 0xdd,
	0xcd, 0x64, 0xb7, 0xf0, 0x70, 0x2b, 0xc5, 0x3d, 0x79, 0x77, 0xdf, 0xf3,
	0x47, 0xbb, 0xe1, 0xcd, 0x36, 0x08, 0xdb, 0xb2, 0xb5, 0x1d, 0xd2, 0xf6,
	0xec, 0xec, 0x80, 0xb2, 0x23, 0x7b, 0x2b, 0xd0, 0x56, 0x72, 0xb0, 0x0a,
	0x63, 0x35, 0x47, 0x6b, 0xb0, 0xd6, 0x72, 0xb2, 0x0e, 0x67, 0x3d, 0x67,
	0x1b, 0xf0, 0x36, 0x72, 0xb1, 0x89, 0x60, 0x33, 0x57, 0x5b, 0x88, 0xb6,
	0x72, 0xb3, 0x13, 0xc9, 0xce, 0xdc, 0xed, 0x42, 0xb6, 0x2b, 0x0f, 0xfb,
	0xa4, 0xd8, 0xef, 0xf9, 0xaf, 0x66, 0xc3, 0x68, 0x1a, 0xde, 0x4c, 0x8b,
	0x30, 0x1d, 0x5b, 0xd3, 0x23, 0xcd, 0xc0, 0xce, 0x8c, 0x28, 0x23, 0xd8,
	0x1b, 0x89, 0x36, 0x8a, 0x83, 0xd1, 0x18, 0x63, 0x38, 0x1a, 0x8b, 0x35,
	0x8e, 0x93, 0xf1, 0x38, 0x13, 0x38, 0x9b, 0x88, 0x37, 0x89, 0x8b, 0xc9,
	0x04, 0x53, 0xb8, 0x9a, 0x4a, 0x34, 0x13, 0x37, 0x33, 0x93, 0xcc, 0xc2,
	0xdd, 0xac, 0x64, 0xf3, 0xe4, 0x61, 0xbe, 0xe7, 0x0f, 0x7a, 0xc3, 0xab,
	0x6e, 0x18, 0x75, 0xcb, 0x9b, 0xee, 0x10, 0xba, 0x67, 0xab, 0x07, 0xa4,
	0x1e, 0xd9, 0x69, 0x81, 0xd2, 0x92, 0xbd, 0x56, 0x68, 0xad, 0x39, 0x68,
	0x83, 0xd1, 0x96, 0xa3, 0x76, 0x58, 0xed, 0x39, 0xe9, 0x80, 0xd3, 0x91,
	0xb3, 0x4e, 0x78, 0x9d, 0xb9, 0xe8, 0x42, 0xd0, 0x95, 0xab, 0x9e, 0x88,
	0x7a, 0xe6, 0xa6, 0x17, 0x92, 0x5e, 0xb9, 0xeb, 0x27, 0x59, 0x7f, 0xcf,
	0x7f, 0x51, 0x1b, 0x06, 0xd5, 0xf0, 0xaa, 0x5a, 0x46, 0xd5, 0xf1, 0xa6,
	0x7a, 0x84, 0x1a, 0xd8, 0xaa, 0x11, 0xa9, 0x04, 0x3b, 0x25, 0x51, 0x4a,
	0xb1, 0x57, 0x1a, 0xad, 0x0c, 0x07, 0x65, 0x31, 0xca, 0x71, 0x54, 0x1e,
	0xab, 0x02, 0x27, 0x15, 0x71, 0x2a, 0x71, 0x56, 0x19, 0xaf, 0x0a, 0x17,
	0x55, 0x09, 0x6a, 0xe2, 0xaa, 0x66, 0xa2, 0x5a, 0xb8, 0xa9, 0x95, 0xa4,
	0x9e, 0xdc, 0xd5, 0xf7, 0xfc, 0x5e, 0x6e, 0x78, 0x91, 0x0d, 0x83, 0x6c,
	0x79, 0x95, 0x1d, 0xa3, 0xec, 0x79, 0x93, 0x03, 0x42, 0x8e, 0x6c, 0xa5,
	0x40, 0x4a, 0xc9, 0x4e, 0x2a, 0x94, 0xd4, 0xec, 0xa5, 0x41, 0x4b, 0xcb,
	0x41, 0x3a, 0x8c, 0xf4, 0x1c, 0x65, 0xc0, 0xca, 0xc8, 0x49, 0x26, 0x9c,
	0xcc, 0x9c, 0x65, 0xc1, 0xcb, 0xca, 0x45, 0x4e, 0x04, 0x39, 0x73, 0x95,
	0x0b, 0x51, 0xae, 0xdc, 0xe4, 0x93, 0x24, 0xbf, 0x7e, 0x75, 0x07, 0xf7,
	0xa2, 0xe1, 0x45, 0xb4, 0x0c, 0xa2, 0xe3, 0x55, 0xf4, 0x8c, 0x62, 0xe0,
	0x4d, 0x8c, 0x08, 0x21, 0xd8, 0x0a, 0x89, 0x14, 0x8a, 0x9d, 0xd0, 0x28,
	0x61, 0xd8, 0x0b, 0x8b, 0x16, 0x8e, 0x83, 0xf0, 0x18, 0x11, 0x38, 0x8a,
	0x88, 0x15, 0x89, 0x93, 0xc8, 0x38, 0x51, 0x38, 0x8b, 0x8a, 0x17, 0x13,
	0x17, 0x31, 0x13, 0xc4, 0xc2, 0x55, 0xac, 0x44, 0xf1, 0xdb, 0xf7, 0xe7,
	0xc7, 0xd8, 0xd0, 0x8f, 0x2d, 0x2f, 0x63, 0xc7, 0x30, 0xf6, 0xbc, 0x8e,
	0x03, 0xe3, 0x38, 0xf2, 0x36, 0x0a, 0xc4, 0x28, 0xd9, 0x8e, 0x0a, 0x39,
	0x6a, 0x76, 0xa3, 0x41, 0x8d, 0x96, 0xfd, 0xe8, 0xd0, 0xa3, 0xe7, 0x30,
	0x06, 0xcc, 0x18, 0x39, 0x8e, 0x09, 0x3b, 0x66, 0x4e, 0x63, 0xc1, 0x8d,
	0x95, 0xf3, 0x38, 0xe1, 0xc7, 0x99, 0xcb, 0xb8, 0x10, 0xc6, 0x95, 0xeb,
	0xf8, 0xfc, 0xdd, 0x1b, 0xfc, 0x63, 0x68, 0xe9, 0x87, 0x8e, 0x97, 0xa1,
	0x67, 0x18, 0x06, 0x5e, 0x87, 0x91, 0x71, 0x10, 0xbc, 0x0d, 0x12, 0x31,
	0x28, 0xb6, 0x83, 0x46, 0x0e, 0x86, 0xdd, 0x60, 0x51, 0x83, 0x63, 0x3f,
	0x78, 0xf4, 0x10, 0x38, 0x0c, 0x11, 0x33, 0x24, 0x8e, 0x43, 0xc6, 0x0e,
	0x85, 0xd3, 0x50, 0x71, 0xc3, 0xc4, 0x79, 0x98, 0xf1, 0xc3, 0xc2, 0x65,
	0x58, 0xff, 0x70, 0x07, 0xf9, 0xd1, 0x77, 0xf4, 0x7d, 0xcf, 0x4b, 0x3f,
	0x30, 0xf4, 0x23, 0xaf, 0xbd, 0x60, 0xec, 0x25, 0x6f, 0xbd, 0x42, 0xf4,
	0x9a, 0x6d, 0x6f, 0x90, 0xbd, 0x65, 0xd7, 0x3b, 0x54, 0xef, 0xd9, 0xf7,
	0x01, 0xdd, 0x47, 0x0e, 0x7d, 0xc2, 0xf4, 0x99, 0x63, 0x5f, 0xb0, 0x7d,
	0xe5, 0xd4, 0x4f, 0xb8, 0x7e, 0xe6, 0xdc, 0x2f, 0x7f, 0xba, 0x83, 0xfd,
	0xe8, 0x7a, 0xfa, 0x6e, 0xe0, 0xa5, 0x1b, 0x19, 0x3a, 0xc1, 0x6b, 0x27,
	0x19, 0x3b, 0xc5, 0x5b, 0xa7, 0x11, 0x9d, 0x61, 0xdb, 0x59, 0x64, 0xe7,
	0xd8, 0x75, 0x1e, 0xd5, 0x05, 0xf6, 0x5d, 0x44, 0x77, 0x89, 0x43, 0x97,
	0x31, 0x5d, 0xe1, 0xd8, 0x55, 0x6c, 0x37, 0x71, 0xea, 0xe6, 0xbf, 0xdc,
	0x41, 0x7f, 0xb4, 0x03, 0x7d, 0x3b, 0xf2, 0xd2, 0x0a, 0x86, 0x56, 0xf2,
	0xda, 0x2a, 0xc6, 0x56, 0xf3, 0xd6, 0x1a, 0x44, 0x6b, 0xd9, 0xb6, 0x0e,
	0xd9, 0x7a, 0x76, 0x6d, 0x40, 0xb5, 0x91, 0x7d, 0x9b, 0xd0, 0x6d, 0xe6,
	0xd0, 0x16, 0x4c, 0x5b, 0x39, 0xb6, 0xd3, 0xff, 0xdc, 0xc1, 0x7f, 0x34,
	0x23, 0x7d, 0x23, 0x78, 0x69, 0x24, 0x43, 0xa3, 0x78, 0x6d, 0x34, 0x63,
	0x63, 0x78, 0x6b, 0x2c, 0xa2, 0x71, 0x6c, 0x1b, 0x8f, 0x6c, 0x02, 0xbb,
	0x26, 0xa2, 0x9a, 0xc4, 0xbe, 0xc9, 0xe8, 0xa6, 0x70, 0x68, 0xea, 0xdf,
	0xfa, 0x87, 0xf4, 0x1b, 0xc9, 0xcb, 0x46, 0x31, 0x6c, 0x34, 0xaf, 0x1b,
	0xc3, 0xb8, 0xb1, 0xbc, 0x6d, 0x1c, 0x62, 0xe3, 0xd9, 0x6e, 0x02, 0x72,
	0x13, 0xd9, 0x6d, 0x12, 0x6a, 0xf3, 0xf7, 0xfe, 0x3f, 0xff, 0xe4, 0xff,
	0x97, 0x7f, 0x03,
};

// 8x8, level 0: STORED deflate blocks - a method-8 entry that is not Huffman coded at all.
static const unsigned char kDeflateIconStored[] = {
	0x01, 0x12, 0x01, 0xed, 0xfe, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x20, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x20, 0x28, 0x18, 0xff, 0x24, 0x28, 0x20, 0xff, 0x28,
	0x28, 0x28, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c,
	0x20, 0x18, 0xff, 0x20, 0x20, 0x20, 0xff, 0x24, 0x20, 0x28, 0xff, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0xff, 0x1c,
	0x18, 0x20, 0xff, 0x20, 0x18, 0x28, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00,
};

// 4x4, level 1: small enough that zlib picks the FIXED Huffman table.
static const unsigned char kDeflateIconFixed[] = {
	0x63, 0x60, 0x60, 0x62, 0x80, 0x01, 0x16, 0x06, 0x16, 0x06, 0x05, 0x18,
	0x07, 0x83, 0x16, 0x10, 0x10, 0xf8, 0x8f, 0x21, 0x88, 0x45, 0x00, 0x00,
};

// =============================================================================================== //
// WEAPON ESP Icon mode - the pak (ZIP) reader and the TGA decode
//
// Both live in weaponEspCore.cpp, which is what this file links; they used to sit in
// weaponEsp.cpp's _WIN32 half, where off Windows they could only be compiled, never run - and that
// is exactly where the bugs that made every stock icon draw as a chip hid. So the tests below
// drive the code the DLL runs: real ZIP layouts, real zlib streams, and the shape Q3's own icon
// art has.
// =============================================================================================== //

// The block type of a stream's first deflate block: 0 stored, 1 fixed Huffman, 2 dynamic Huffman.
static int DeflateBlockType(const unsigned char* stream)
{
	return (stream[0] >> 1) & 3;
}

// One pixel of a w x h icon: opaque inside the circle, fully transparent outside - the shape a Q3
// weapon icon has (which is also what makes its alpha channel worth carrying) - with the colour a
// function of the pixel, so a decode that lost a row, flipped the image or swapped a channel
// cannot happen to match.
static void IconPixel(int x, int y, int w, int h, unsigned char& r, unsigned char& g,
                      unsigned char& b, unsigned char& a)
{
	const int dx = x - w / 2;
	const int dy = y - h / 2;
	int rad = w / 2 - 2;
	if (rad < 1)
		rad = 1;
	if (dx * dx + dy * dy < rad * rad)
	{
		r = (unsigned char)((x * 8) & 0xFF);
		g = (unsigned char)((y * 8) & 0xFF);
		b = (unsigned char)(((x + y) * 4) & 0xFF);
		a = 255;
	}
	else
	{
		r = g = b = 0;
		a = 0;
	}
}

// The artwork as a plain top-down RGBA image - what every decode below has to come out as.
static void MakeIconArtwork(unsigned char* rgba, int w, int h)
{
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			unsigned char r, g, b, a;
			IconPixel(x, y, w, h, r, g, b, a);
			unsigned char* o = rgba + ((size_t)y * w + x) * 4;
			o[0] = r;
			o[1] = g;
			o[2] = b;
			o[3] = a;
		}
	}
}

// The file row that carries image row `y`: bit 5 of the descriptor says the rows come top-down,
// so without it - what Q3 writes - the image's first row is the file's LAST one.
static int FileRow(int y, int h, bool topDown)
{
	return topDown ? y : h - 1 - y;
}

static void TgaHeader(unsigned char* out, int w, int h, int imageType, int bpp, int desc)
{
	memset(out, 0, 18);
	out[2]  = (unsigned char)imageType;
	out[12] = (unsigned char)(w & 0xFF);
	out[13] = (unsigned char)(w >> 8);
	out[14] = (unsigned char)(h & 0xFF);
	out[15] = (unsigned char)(h >> 8);
	out[16] = (unsigned char)bpp;       // pixel depth: header byte 16, not 14
	out[17] = (unsigned char)desc;      // descriptor: byte 17 (0x20 = top-down)
}

// type 2, 32bpp, Q3's byte order (B,G,R,A). `topDown` only changes where the rows sit.
static size_t MakeIconTga32(unsigned char* out, int w, int h, bool topDown)
{
	TgaHeader(out, w, h, 2, 32, topDown ? 0x20 : 0);
	unsigned char* px = out + 18;
	for (int y = 0; y < h; ++y)
	{
		unsigned char* row = px + (size_t)FileRow(y, h, topDown) * w * 4;
		for (int x = 0; x < w; ++x)
		{
			unsigned char r, g, b, a;
			IconPixel(x, y, w, h, r, g, b, a);
			row[x * 4 + 0] = b;
			row[x * 4 + 1] = g;
			row[x * 4 + 2] = r;
			row[x * 4 + 3] = a;
		}
	}
	return 18 + (size_t)w * h * 4;
}

// type 2, 24bpp, bottom-up: B,G,R with no alpha channel at all (so every pixel is opaque).
static size_t MakeIconTga24(unsigned char* out, int w, int h)
{
	TgaHeader(out, w, h, 2, 24, 0);
	unsigned char* px = out + 18;
	for (int y = 0; y < h; ++y)
	{
		unsigned char* row = px + (size_t)FileRow(y, h, false) * w * 3;
		for (int x = 0; x < w; ++x)
		{
			unsigned char r, g, b, a;
			IconPixel(x, y, w, h, r, g, b, a);
			row[x * 3 + 0] = b;
			row[x * 3 + 1] = g;
			row[x * 3 + 2] = r;
		}
	}
	return 18 + (size_t)w * h * 3;
}

// The gray level a pixel gets in the 8bpp fixture: the artwork's channels averaged, so the test
// can recompute the expected byte exactly.
static unsigned char GrayLevel(int x, int y, int w, int h)
{
	unsigned char r, g, b, a;
	IconPixel(x, y, w, h, r, g, b, a);
	return (unsigned char)(((unsigned int)r + g + b) / 3);
}

// type 3, 8bpp gray, bottom-up: one byte per pixel.
static size_t MakeIconTgaGray(unsigned char* out, int w, int h)
{
	TgaHeader(out, w, h, 3, 8, 0);
	unsigned char* px = out + 18;
	for (int y = 0; y < h; ++y)
	{
		unsigned char* row = px + (size_t)FileRow(y, h, false) * w;
		for (int x = 0; x < w; ++x)
			row[x] = GrayLevel(x, y, w, h);
	}
	return 18 + (size_t)w * h;
}

// type 10, run-length encoded 32bpp, bottom-up. A row of identical pixels becomes a run packet
// (rows above and below the circle are entirely transparent, so those are real runs), anything
// else a raw packet - both packet kinds, as a pak's icon art exercises them.
static size_t MakeIconTgaRle(unsigned char* out, int w, int h)
{
	TgaHeader(out, w, h, 10, 32, 0);
	unsigned char* px = out + 18;
	unsigned char row[64 * 4];
	size_t n = 0;
	if (w > 64)
		return 0;
	for (int f = 0; f < h; ++f)                       // the file's row order (bottom-up)
	{
		const int y = h - 1 - f;                      // the image row it carries
		for (int x = 0; x < w; ++x)
		{
			unsigned char r, g, b, a;
			IconPixel(x, y, w, h, r, g, b, a);
			row[x * 4 + 0] = b;
			row[x * 4 + 1] = g;
			row[x * 4 + 2] = r;
			row[x * 4 + 3] = a;
		}

		bool same = true;
		for (int x = 1; x < w; ++x)
			if (memcmp(row, row + x * 4, 4) != 0)
				same = false;

		if (same)
		{
			px[n++] = (unsigned char)(0x80 | (w - 1));
			memcpy(px + n, row, 4);
			n += 4;
		}
		else
		{
			px[n++] = (unsigned char)(w - 1);
			memcpy(px + n, row, (size_t)w * 4);
			n += (size_t)w * 4;
		}
	}
	return 18 + n;
}

static void TestWeaponIconTgaDecode()
{
	Section("WEAPON ESP icons - DecodeTga (header bytes, types 2/3/10, byte order, row order)");

	static unsigned char tga[18 + 128 * 128 * 4];     // fits every fixture below
	static unsigned char artwork[128 * 128 * 4];
	static unsigned char want[18 + 128 * 128 * 4];
	unsigned char* rgba = 0;
	int w = 0, h = 0;

	// --- 32bpp, bottom-up: the stock icon case ---------------------------------------------- //
	const int side = 32;
	MakeIconArtwork(artwork, side, side);
	size_t len = MakeIconTga32(tga, side, side, false);
	CHECK_UINT(tga[16], 32, "the pixel depth sits in header byte 16 ...");
	CHECK_UINT(tga[14], 32, "... while byte 14 is the height's low byte - which is where the "
	                       "decode used to read the depth from (so a 64x64 icon looked like a "
	                       "64bpp one and was rejected, and every stock icon drew as a chip)");
	CHECK_TRUE(WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "a 32x32 icon decodes");
	CHECK_INT(w, side, "width");
	CHECK_INT(h, side, "height");
	if (rgba)
	{
		CHECK_TRUE(memcmp(rgba, artwork, (size_t)side * side * 4) == 0,
		           "... to the artwork, top row first and B,G,R,A read back as RGB(A)");
		free(rgba);
		rgba = 0;
	}

	// --- the size the old header read mangled: a 64x64 icon (height's low byte 0x40) -------- //
	const int big = 64;
	MakeIconArtwork(artwork, big, big);
	len = MakeIconTga32(tga, big, big, false);
	CHECK_UINT(tga[14], 0x40, "a 64x64 icon's header byte 14 reads 0x40 - not a pixel depth");
	CHECK_TRUE(WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "a 64x64 icon decodes");
	CHECK_INT(w, big, "64x64 width");
	if (rgba)
	{
		CHECK_TRUE(memcmp(rgba, artwork, (size_t)big * big * 4) == 0, "... to its artwork");
		free(rgba);
		rgba = 0;
	}

	// --- 128x128: the height's low byte (0x80) is not even a plausible depth ---------------- //
	const int huge = 128;
	MakeIconArtwork(artwork, huge, huge);
	len = MakeIconTga32(tga, huge, huge, false);
	CHECK_TRUE(WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "a 128x128 icon decodes");
	CHECK_INT(w, huge, "128x128 width");
	if (rgba)
	{
		CHECK_TRUE(memcmp(rgba, artwork, (size_t)huge * huge * 4) == 0, "... to its artwork");
		free(rgba);
		rgba = 0;
	}

	// --- top-down (descriptor bit 5): the same artwork, the other row order ----------------- //
	MakeIconArtwork(artwork, side, side);
	len = MakeIconTga32(tga, side, side, true);
	CHECK_TRUE(WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "a top-down icon decodes");
	if (rgba)
	{
		CHECK_TRUE(memcmp(rgba, artwork, (size_t)side * side * 4) == 0,
		           "... to the same artwork (the flip is applied exactly once)");
		free(rgba);
		rgba = 0;
	}

	// --- 24bpp: no alpha channel in the file, so every pixel is opaque ---------------------- //
	MakeIconArtwork(artwork, side, side);
	len = MakeIconTga24(tga, side, side);
	CHECK_TRUE(WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "a 24bpp icon decodes");
	if (rgba)
	{
		bool ok = true;
		for (int i = 0; i < side * side; ++i)
		{
			if (rgba[i * 4 + 0] != artwork[i * 4 + 0] || rgba[i * 4 + 1] != artwork[i * 4 + 1] ||
			    rgba[i * 4 + 2] != artwork[i * 4 + 2] || rgba[i * 4 + 3] != 255)
				ok = false;
		}
		CHECK_TRUE(ok, "... as the artwork with alpha forced opaque");
		free(rgba);
		rgba = 0;
	}

	// --- 8bpp gray (type 3) ----------------------------------------------------------------- //
	len = MakeIconTgaGray(tga, side, side);
	CHECK_TRUE(WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "an 8bpp gray icon decodes");
	if (rgba)
	{
		bool ok = true;
		for (int y = 0; y < side && ok; ++y)
		{
			for (int x = 0; x < side; ++x)
			{
				const unsigned char v = GrayLevel(x, y, side, side);
				const unsigned char* o = rgba + ((size_t)y * side + x) * 4;
				if (o[0] != v || o[1] != v || o[2] != v || o[3] != 255)
					ok = false;
			}
		}
		CHECK_TRUE(ok, "... as the gray level in all three channels, opaque");
		free(rgba);
		rgba = 0;
	}

	// --- 16bpp: 5-5-5, expanded and forced opaque (within a 5-bit step of the artwork) ------ //
	{
		TgaHeader(tga, 4, 4, 2, 16, 0);
		for (int y = 0; y < 4; ++y)
		{
			for (int x = 0; x < 4; ++x)
			{
				unsigned char r, g, b, a;
				IconPixel(x, y, 4, 4, r, g, b, a);
				const unsigned int v555 = (unsigned int)(r >> 3) << 10 |
				                          (unsigned int)(g >> 3) << 5 |
				                          (unsigned int)(b >> 3);
				tga[18 + (y * 4 + x) * 2 + 0] = (unsigned char)(v555 & 0xFF);
				tga[18 + (y * 4 + x) * 2 + 1] = (unsigned char)(v555 >> 8);
			}
		}
		CHECK_TRUE(WeaponEsp::DecodeTga(tga, 18 + 4 * 4 * 2, &rgba, &w, &h), "a 16bpp icon decodes");
		CHECK_INT(w, 4, "16bpp width");
		if (rgba)
		{
			unsigned char r, g, b, a;
			IconPixel(0, 0, 4, 4, r, g, b, a);
			CHECK_UINT(rgba[3], 255, "16bpp: opaque");
			CHECK_TRUE(rgba[0] >= r - 8 && rgba[0] <= r + 8, "16bpp: red within a 5-bit step");
			free(rgba);
			rgba = 0;
		}
	}

	// --- type 10 (RLE): must come out as the uncompressed read of the same artwork ---------- //
	MakeIconArtwork(artwork, side, side);
	const size_t plainLen = MakeIconTga32(tga, side, side, false);
	memcpy(want, tga, plainLen);                      // keep the uncompressed file for comparison
	const size_t rleLen = MakeIconTgaRle(tga, side, side);
	CHECK_TRUE(rleLen < plainLen, "the RLE fixture is smaller than the uncompressed one");
	CHECK_TRUE(WeaponEsp::DecodeTga(tga, rleLen, &rgba, &w, &h), "an RLE icon decodes");
	if (rgba)
	{
		CHECK_TRUE(memcmp(rgba, artwork, (size_t)side * side * 4) == 0,
		           "... to the uncompressed artwork");
		free(rgba);
		rgba = 0;
	}

	// an RLE packet that runs past the last pixel: the engine lets it stop there, and so must the
	// decode (a 4x4 all-opaque image, whose last packet claims 128 pixels)
	{
		static unsigned char one[18 + 4 * 4 * 4 + 8];
		memset(one, 0, sizeof(one));
		TgaHeader(one, 4, 4, 10, 32, 0);
		size_t n = 18;
		one[n++] = 0x8F;                              // a run of 16 pixels ...
		one[n++] = 0x00; one[n++] = 0x00; one[n++] = 0x00; one[n++] = 0xFF;   // ... of opaque black
		one[n++] = 0xFF;                              // then a run of 128 with nothing left
		one[n++] = 0x01; one[n++] = 0x02; one[n++] = 0x03; one[n++] = 0xFF;
		CHECK_TRUE(WeaponEsp::DecodeTga(one, n, &rgba, &w, &h),
		           "an RLE packet that overshoots the last pixel still decodes");
		if (rgba)
		{
			CHECK_UINT(rgba[3], 255, "... to the pixels the earlier packets described");
			free(rgba);
			rgba = 0;
		}
	}

	// --- the refusals ----------------------------------------------------------------------- //
	CHECK_TRUE(!WeaponEsp::DecodeTga(0, 100, &rgba, &w, &h), "no data: refused");
	CHECK_TRUE(!WeaponEsp::DecodeTga(tga, 17, &rgba, &w, &h), "a 17-byte header: refused");
	CHECK_TRUE(rgba == 0 && w == 0 && h == 0, "... with nothing handed back");

	len = MakeIconTga32(tga, side, side, false);
	CHECK_TRUE(!WeaponEsp::DecodeTga(tga, len - 10, &rgba, &w, &h), "pixels cut short: refused");
	CHECK_TRUE(rgba == 0, "... with nothing handed back");

	tga[0] = 8;                                       // an id field the bytes do not hold
	CHECK_TRUE(!WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "an id field past the end: refused");
	tga[0] = 0;

	TgaHeader(tga, side, side, 5, 32, 0);             // 5 is not an image type TGA defines
	CHECK_TRUE(!WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "an unknown image type: refused");
	TgaHeader(tga, side, side, 2, 12, 0);             // nor is 12 a pixel depth
	CHECK_TRUE(!WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "an unknown pixel depth: refused");
	TgaHeader(tga, side, side, 2, 32, 0);
	tga[1] = 1;                                       // a colour-mapped image
	CHECK_TRUE(!WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "a colour-mapped image: refused");
	TgaHeader(tga, 0, side, 2, 32, 0);
	CHECK_TRUE(!WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "a zero-sized image: refused");
	TgaHeader(tga, 513, side, 2, 32, 0);
	CHECK_TRUE(!WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h), "a 513-pixel image: refused");
	TgaHeader(tga, side, side, 10, 32, 0);            // an RLE header with no packets behind it
	CHECK_TRUE(!WeaponEsp::DecodeTga(tga, 18, &rgba, &w, &h),
	           "an RLE stream with no packets behind it: refused");

	MakeIconTgaRle(tga, side, side);
	const size_t shortRle = 18 + 1 + 4 + 1;           // the first packet plus a cut-off one
	CHECK_TRUE(!WeaponEsp::DecodeTga(tga, shortRle, &rgba, &w, &h), "a cut-off RLE stream: refused");
}

// =============================================================================================== //
// the pak side: a ZIP laid out the way a pak tool lays one out
// =============================================================================================== //

static void Put16(unsigned char* p, unsigned int v)
{
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void Put32(unsigned char* p, unsigned int v)
{
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
	p[2] = (unsigned char)((v >> 16) & 0xFF);
	p[3] = (unsigned char)((v >> 24) & 0xFF);
}

struct PakEntry
{
	const char*          name;       // the name as stored in the archive
	unsigned int         method;     // 0 stored, 8 deflated, anything else: not read
	const unsigned char* packed;     // the bytes as they sit in the archive
	unsigned int         packedLen;  // ... their length
	unsigned int         rawLen;     // the uncompressed size the entry declares
	unsigned int         localExtra; // junk in the LOCAL header's extra field only - the data
	                                 // offset has to come from the local header, not the index
};

// Local header + data per entry, then the central directory, then the end-of-central-directory
// record with `commentLen` junk bytes behind it (which the loader has to scan past). The CRC
// fields stay zero - the loader does not check them.
static size_t BuildPak(unsigned char* out, size_t outCap, const PakEntry* entries, int count,
                       unsigned int commentLen)
{
	unsigned int offsets[8];
	if (count > 8)
		return 0;

	size_t n = 0;
	for (int i = 0; i < count; ++i)
	{
		const size_t nameLen = strlen(entries[i].name);
		if (n + 30 + nameLen + entries[i].localExtra + entries[i].packedLen > outCap)
			return 0;
		unsigned char* h = out + n;
		memset(h, 0, 30);
		h[0] = 'P'; h[1] = 'K'; h[2] = 3; h[3] = 4;
		Put16(h + 4, 20);                                 // version needed to extract
		Put16(h + 8, entries[i].method);
		Put32(h + 18, entries[i].packedLen);              // compressed size
		Put32(h + 22, entries[i].rawLen);                 // uncompressed size
		Put16(h + 26, (unsigned int)nameLen);
		Put16(h + 28, entries[i].localExtra);
		memcpy(h + 30, entries[i].name, nameLen);
		memset(h + 30 + nameLen, 0xEE, entries[i].localExtra);
		memcpy(h + 30 + nameLen + entries[i].localExtra, entries[i].packed, entries[i].packedLen);
		offsets[i] = (unsigned int)n;
		n += 30 + nameLen + entries[i].localExtra + entries[i].packedLen;
	}

	const size_t cdStart = n;
	for (int i = 0; i < count; ++i)
	{
		const size_t nameLen = strlen(entries[i].name);
		if (n + 46 + nameLen > outCap)
			return 0;
		unsigned char* r = out + n;
		memset(r, 0, 46);
		r[0] = 'P'; r[1] = 'K'; r[2] = 1; r[3] = 2;
		Put16(r + 4, 20);                                 // version made by
		Put16(r + 6, 20);                                 // version needed
		Put16(r + 10, entries[i].method);
		Put32(r + 20, entries[i].packedLen);
		Put32(r + 24, entries[i].rawLen);
		Put16(r + 28, (unsigned int)nameLen);
		Put32(r + 42, offsets[i]);                        // where the local header starts
		memcpy(r + 46, entries[i].name, nameLen);
		n += 46 + nameLen;
	}

	if (n + 22 + commentLen > outCap)
		return 0;
	unsigned char* e = out + n;
	memset(e, 0, 22);
	e[0] = 'P'; e[1] = 'K'; e[2] = 5; e[3] = 6;
	Put16(e + 8, (unsigned int)count);
	Put16(e + 10, (unsigned int)count);
	Put32(e + 12, (unsigned int)(n - cdStart));           // size of the central directory
	Put32(e + 16, (unsigned int)cdStart);                 // ... and where it starts
	Put16(e + 20, commentLen);
	n += 22;
	if (commentLen)
	{
		memset(out + n, 'x', commentLen);
		n += commentLen;
	}
	return n;
}

// What weaponEsp.cpp hands PakReadEntry on Windows is SetFilePointer + ReadFile over the pak; a
// test hands it a buffer.
struct MemoryPak
{
	const unsigned char* data;
	size_t               size;
};

static bool MemoryPakRead(void* user, unsigned long long offset, void* dest, size_t count)
{
	const MemoryPak& pak = *(const MemoryPak*)user;
	if (offset > (unsigned long long)pak.size ||
	    (size_t)((unsigned long long)pak.size - offset) < count)
		return false;
	memcpy(dest, pak.data + (size_t)offset, count);
	return true;
}

static void TestWeaponIconPakRead()
{
	Section("WEAPON ESP icons - PakReadEntry (ZIP central directory, DEFLATE, real zlib streams)");

	static unsigned char pak[128 * 1024];
	static unsigned char icon[18 + 32 * 32 * 4];       // the 32x32 TGA the fixtures were made from
	static unsigned char artwork[32 * 32 * 4];
	MakeIconArtwork(artwork, 32, 32);
	const size_t iconLen = MakeIconTga32(icon, 32, 32, false);

	// the fixtures really do cover the three block types - regenerating them wrong shows up here
	CHECK_INT(DeflateBlockType(kDeflateIconDynamic), 2, "the dynamic fixture is a dynamic block");
	CHECK_INT(DeflateBlockType(kDeflateIconStored), 0, "the stored fixture is a stored block");
	CHECK_INT(DeflateBlockType(kDeflateIconFixed), 1, "the fixed fixture is a fixed-Huffman block");

	PakEntry entry;
	memset(&entry, 0, sizeof(entry));

	// --- a stored entry: the plain copy path ------------------------------------------------- //
	entry.name      = "icons/Iconw_Test.tga";          // the archive's casing, like the engine's
	entry.method    = 0;
	entry.packed    = icon;
	entry.packedLen = (unsigned int)iconLen;
	entry.rawLen    = (unsigned int)iconLen;
	size_t pakLen = BuildPak(pak, sizeof(pak), &entry, 1, 0);
	CHECK_TRUE(pakLen > 0, "a pak with one stored icon was built");

	MemoryPak file;
	file.data = pak;
	file.size = pakLen;

	unsigned char* bytes = 0;
	size_t outLen = 0;
	CHECK_TRUE(WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, "icons/iconw_test.tga",
	                                   &bytes, &outLen),
	           "a stored entry is found by a differently-cased name (the engine's search)");
	CHECK_UINT(outLen, iconLen, "... with the size the file has");
	if (bytes)
	{
		CHECK_TRUE(memcmp(bytes, icon, iconLen) == 0, "... byte for byte");
		unsigned char* rgba = 0;
		int w = 0, h = 0;
		CHECK_TRUE(WeaponEsp::DecodeTga(bytes, outLen, &rgba, &w, &h), "... and decodes as the icon");
		if (rgba)
		{
			CHECK_TRUE(memcmp(rgba, artwork, sizeof(artwork)) == 0, "... to its artwork");
			free(rgba);
		}
		free(bytes);
	}

	// --- a deflated entry per block type ----------------------------------------------------- //
	{
		struct
		{
			const char*          name;
			const unsigned char* stream;
			unsigned int         streamLen;
			size_t               rawLen;
			const char*          what;
		} cases[] =
		{
			{ "icons/iconw_machinegun.tga", kDeflateIconDynamic, (unsigned int)sizeof(kDeflateIconDynamic), iconLen, "dynamic Huffman" },
			{ "icons/iconw_shotgun.tga",    kDeflateIconStored,  (unsigned int)sizeof(kDeflateIconStored),  18 + 8 * 8 * 4, "stored blocks" },
			{ "icons/iconw_grenade.tga",    kDeflateIconFixed,   (unsigned int)sizeof(kDeflateIconFixed),   18 + 4 * 4 * 4, "fixed Huffman" },
		};

		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
		{
			PakEntry deflated;
			memset(&deflated, 0, sizeof(deflated));
			deflated.name      = cases[i].name;
			deflated.method    = 8;
			deflated.packed    = cases[i].stream;
			deflated.packedLen = cases[i].streamLen;
			deflated.rawLen    = (unsigned int)cases[i].rawLen;
			pakLen = BuildPak(pak, sizeof(pak), &deflated, 1, 0);
			file.size = pakLen;
			bytes = 0;
			outLen = 0;

			char what[128];
			snprintf(what, sizeof(what), "a deflated entry (%s) is inflated", cases[i].what);
			CHECK_TRUE(WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, cases[i].name, &bytes,
			                                   &outLen), what);
			CHECK_UINT(outLen, cases[i].rawLen, "... to the declared size");
			if (bytes)
			{
				if (i == 0)
					CHECK_TRUE(memcmp(bytes, icon, iconLen) == 0,
					           "... byte for byte - the 32x32 icon the fixture was made from");
				unsigned char* rgba = 0;
				int w = 0, h = 0;
				snprintf(what, sizeof(what), "... and decodes as a TGA (%s)", cases[i].what);
				CHECK_TRUE(WeaponEsp::DecodeTga(bytes, outLen, &rgba, &w, &h), what);
				free(rgba);
				free(bytes);
			}
		}
	}

	// --- a local header carrying an extra field the central directory does not know about ----- //
	entry.name       = "icons/iconw_railgun.tga";
	entry.localExtra = 12;
	pakLen = BuildPak(pak, sizeof(pak), &entry, 1, 0);
	file.size = pakLen;
	bytes = 0;
	outLen = 0;
	CHECK_TRUE(WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, "icons/iconw_railgun.tga",
	                                   &bytes, &outLen),
	           "an entry with a local-only extra field is found");
	CHECK_UINT(outLen, iconLen, "... with the size the file has");
	if (bytes)
	{
		CHECK_TRUE(memcmp(bytes, icon, iconLen) == 0,
		           "... and its data starts after the LOCAL header's name/extra lengths");
		free(bytes);
	}
	entry.localExtra = 0;

	// --- an exact-case match wins over an earlier case-insensitive one ----------------------- //
	{
		static unsigned char small[18 + 4 * 4 * 4];
		const size_t smallLen = MakeIconTga32(small, 4, 4, false);
		PakEntry twins[2];
		memset(twins, 0, sizeof(twins));
		twins[0].name      = "icons/iconw_rocket.tga";     // the 4x4 icon, first in the archive
		twins[0].method    = 0;
		twins[0].packed    = small;
		twins[0].packedLen = (unsigned int)smallLen;
		twins[0].rawLen    = (unsigned int)smallLen;
		twins[1].name      = "icons/iconw_rocket.TGA";     // the 32x32 one, exact-cased
		twins[1].method    = 0;
		twins[1].packed    = icon;
		twins[1].packedLen = (unsigned int)iconLen;
		twins[1].rawLen    = (unsigned int)iconLen;
		pakLen = BuildPak(pak, sizeof(pak), twins, 2, 0);
		file.size = pakLen;
		bytes = 0;
		outLen = 0;
		CHECK_TRUE(WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, "icons/iconw_rocket.TGA",
		                                   &bytes, &outLen),
		           "the exact-cased entry is found");
		CHECK_UINT(outLen, iconLen, "... and it is the exact match that won, not the earlier entry");
		if (bytes)
			free(bytes);

		bytes = 0;
		outLen = 0;
		CHECK_TRUE(WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, "ICONS/ICONW_ROCKET.tga",
		                                   &bytes, &outLen),
		           "with no exact match, the first case-insensitive one is used");
		CHECK_UINT(outLen, smallLen, "... which is the entry that comes first in the archive");
		if (bytes)
			free(bytes);
	}

	// --- an archive comment behind the end-of-central-directory record ----------------------- //
	entry.name      = "icons/iconw_railgun.tga";
	entry.method    = 0;
	entry.packed    = icon;
	entry.packedLen = (unsigned int)iconLen;
	entry.rawLen    = (unsigned int)iconLen;
	pakLen = BuildPak(pak, sizeof(pak), &entry, 1, 300);
	file.size = pakLen;
	bytes = 0;
	outLen = 0;
	CHECK_TRUE(WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, "icons/iconw_railgun.tga",
	                                   &bytes, &outLen),
	           "the record is found with 300 bytes of comment behind it");
	if (bytes)
		free(bytes);

	// --- the refusals ------------------------------------------------------------------------ //
	pakLen = BuildPak(pak, sizeof(pak), &entry, 1, 0);
	file.size = pakLen;
	bytes = 0;
	outLen = 0;
	CHECK_TRUE(!WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, "icons/iconw_bfg.tga", &bytes,
	                                    &outLen),
	           "a name the pak does not hold: refused");
	CHECK_TRUE(bytes == 0 && outLen == 0, "... with nothing handed back");

	MemoryPak cut;
	cut.data = pak;
	cut.size = pakLen / 2;                             // the tail cannot even be read
	CHECK_TRUE(!WeaponEsp::PakReadEntry(MemoryPakRead, &cut, cut.size, "icons/iconw_railgun.tga",
	                                    &bytes, &outLen),
	           "a pak whose reads run off the end: refused");

	static unsigned char junk[512];
	memset(junk, 0xAA, sizeof(junk));
	cut.data = junk;
	cut.size = sizeof(junk);
	CHECK_TRUE(!WeaponEsp::PakReadEntry(MemoryPakRead, &cut, cut.size, "icons/iconw_test.tga",
	                                    &bytes, &outLen),
	           "a file that is not a ZIP at all: refused");

	// a method a pak could hold but this loader does not read (bzip2)
	PakEntry other;
	memset(&other, 0, sizeof(other));
	other.name      = "icons/iconw_plasma.tga";
	other.method    = 12;
	other.packed    = icon;
	other.packedLen = (unsigned int)iconLen;
	other.rawLen    = (unsigned int)iconLen;
	pakLen = BuildPak(pak, sizeof(pak), &other, 1, 0);
	file.size = pakLen;
	bytes = 0;
	outLen = 0;
	CHECK_TRUE(!WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, "icons/iconw_plasma.tga",
	                                    &bytes, &outLen),
	           "a bzip2 entry (method 12): refused");

	// a deflated entry whose stream is cut short (a half-written file)
	static unsigned char damaged[sizeof(kDeflateIconDynamic) - 6];
	memcpy(damaged, kDeflateIconDynamic, sizeof(damaged));
	damaged[sizeof(damaged) / 2] ^= 0xFF;
	other.name      = "icons/iconw_lightning.tga";
	other.method    = 8;
	other.packed    = damaged;
	other.packedLen = (unsigned int)sizeof(damaged);
	other.rawLen    = (unsigned int)iconLen;
	pakLen = BuildPak(pak, sizeof(pak), &other, 1, 0);
	file.size = pakLen;
	bytes = 0;
	outLen = 0;
	CHECK_TRUE(!WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, "icons/iconw_lightning.tga",
	                                    &bytes, &outLen),
	           "a deflate stream cut short: refused");
	CHECK_TRUE(bytes == 0, "... with nothing handed back");

	// a deflated entry whose declared size is smaller than its stream inflates to: the inflater
	// has to fail rather than write past what the caller sized the buffer for
	other.name      = "icons/iconw_chaingun.tga";
	other.method    = 8;
	other.packed    = kDeflateIconDynamic;
	other.packedLen = (unsigned int)sizeof(kDeflateIconDynamic);
	other.rawLen    = 100;
	pakLen = BuildPak(pak, sizeof(pak), &other, 1, 0);
	file.size = pakLen;
	bytes = 0;
	outLen = 0;
	CHECK_TRUE(!WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, "icons/iconw_chaingun.tga",
	                                    &bytes, &outLen),
	           "a declared size the stream overruns: refused");

	// a pak with no entries at all
	pakLen = BuildPak(pak, sizeof(pak), &entry, 0, 0);
	file.size = pakLen;
	bytes = 0;
	outLen = 0;
	CHECK_TRUE(!WeaponEsp::PakReadEntry(MemoryPakRead, &file, pakLen, "icons/iconw_test.tga", &bytes,
	                                    &outLen),
	           "an empty pak: refused");
}

int main(void)
{
	printf("kutaQ3 hook tests - NAME ESP core (nameEspCore.cpp)\n");
	printf("  sizeof(q3::snapshot_t) = %u, sizeof(q3::gameState_t) = %u\n",
	       (unsigned)sizeof(q3::snapshot_t), (unsigned)sizeof(q3::gameState_t));

	TestInfoStringParsing();
	TestModuleNameMatching();
	TestGatherGuards();
	TestGatherPlayers();
	TestView();
	TestFrozenView();
	TestRefdefView();
	TestInterpolation();
	TestInterpolationLookback();
	TestRenderLagCarryOver();
	TestReset();
	TestProjection();
	TestTeamColors();
	TestWorldThenHudCapture();
	TestDistanceEsp();
	TestWeaponNumberGather();
	TestWeaponStockTable();
	TestWeaponScannerStockLayout();
	TestWeaponScannerIoqLayout();
	TestWeaponScannerNegatives();
	TestWeaponLegAnchor();
	TestWeaponFadeMatchesOtherEsp();
	TestWeaponIconTgaDecode();
	TestWeaponIconPakRead();

	printf("\n%d checks, %d failed - %s\n", g_checks, g_failed, g_failed ? "FAILED" : "all passed");
	return g_failed ? 1 : 0;
}
