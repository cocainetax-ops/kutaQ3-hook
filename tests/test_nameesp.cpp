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
#include "fake_engine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// the engine's own angle maths, from SDK/code/game/q_math.c
extern "C" void AngleVectors(const float angles[3], float forward[3], float right[3], float up[3]);
extern "C" void AnglesToAxis(const float angles[3], float axis[3][3]);

#include "check.h"

// =============================================================================================== //

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

	CHECK_INT(FakeEngine::SnapshotRequests(), 1, "one snapshot read");
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

	// no refdef at all still works - the fallback view above is what the tests before this use
	NameEsp::Reset();
	CHECK_TRUE(NameEsp::Gather(5000, FakeEngine::Syscall(), NULL), "gathered without a refdef");
	CHECK_TRUE(NameEsp::Current().view.valid, "fallback view still valid");
	CHECK_TRUE(!NameEsp::Current().usedRefdef, "rebuilt view reported as the view source");
}

static void TestSmoothing()
{
	Section("NameEsp::Gather - carrying moving players forward");

	const float here[3] = { 0.0f, 0.0f, 0.0f };
	const float none[3] = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Runner", (const float[]){ 0.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "first sample");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 0.0f, 0.001, "first sample is not extrapolated");

	// 50 ms later the player is 5 units along +x: 100 u/s. The snapshot is then 100 ms old when we
	// are asked, so the tag should sit 10 units past it. No NameEsp::Reset() here - the whole point
	// is that the previous sample is still in the smoothing history.
	FakeEngine::Reset();
	FakeEngine::SetSnapshotTime(1050);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Runner", (const float[]){ 5.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(1150, FakeEngine::Syscall()), "second sample");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 5.0f + 100.0f * 0.1f, 0.01,
	           "velocity from the previous snapshot, carried forward by the snapshot age");

	// a teleport-sized jump must not be extrapolated
	FakeEngine::Reset();
	FakeEngine::SetSnapshotTime(1100);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Runner", (const float[]){ 9000.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(1150, FakeEngine::Syscall()), "after a teleport");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 9000.0f, 0.001,
	           "teleport-sized jump is used as-is");

	// smoothing overshoot: a fast player running at the camera on an old snapshot extrapolates
	// to BEHIND the viewer while the snapshot position is still in front. The tag must fall back
	// to the raw snapshot position instead of vanishing with the "behind the viewer" reject.
	NameEsp::Reset();
	FakeEngine::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);   // eye at z=26, looking along +X
	FakeEngine::SetPlayer(1, "\\n\\Charger", (const float[]){ 70.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "overshoot: first sample");
	FakeEngine::Reset();
	FakeEngine::SetSnapshotTime(1050);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Charger", (const float[]){ 30.0f, 0.0f, 0.0f });
	// velocity is now -800 u/s along X; at +250 ms the smoothing lands at 30-200 = -170, i.e.
	// behind the viewer (near plane 4), while the raw 30 is still in front of it
	CHECK_TRUE(NameEsp::Gather(1300, FakeEngine::Syscall()), "overshoot: second sample");
	CHECK_NEAR(NameEsp::Current().players[0].origin[0], 30.0f, 0.001,
	           "overshoot falls back to the raw snapshot position");
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
	TestSmoothing();
	TestReset();
	TestProjection();
	TestTeamColors();

	printf("\n%d checks, %d failed - %s\n", g_checks, g_failed, g_failed ? "FAILED" : "all passed");
	return g_failed ? 1 : 0;
}
