// =============================================================================================== //
// kutaQ3 hook tests - the GL half of the NAME ESP
//
// Compiles and runs the REAL nameEsp.cpp, glText.cpp and glDraw.cpp - the same files the DLL is
// built from - against the stub <windows.h> / <gl/GL.h> in tests/stub, which record every call. So
// what is asserted here is what the overlay actually issues at the GL level for a given frame:
// which names go out, at which raster positions, in which colour, through GL::Font's display lists.
//
// The screen positions themselves are taken from NameEsp::ProjectWorldToScreen, which
// tests/test_nameesp.cpp checks independently against the engine's own matrices; this file is about
// the drawing around them (font build, ortho setup, centring, chest re-anchoring, drop shadow,
// team colour, draw stats, and doing nothing at all when there is nothing to draw).
//
//     make -C tests check
// =============================================================================================== //

#include "nameEsp.h"
#include "distanceEsp.h"
#include "weaponEsp.h"
#include "fake_engine.h"
#include "config.h"
#include "check.h"
#include "glrec.h"
#include "glText.h"   // FONT_HEIGHT - the full-size face the name ESP (and the closest
                      // distance tag) is drawn in

#include <math.h>
#include <stdio.h>
#include <string.h>

// Config::g_Settings lives in config.cpp, which needs the real <windows.h>. The overlay reads only
// nameEsp out of it, so the test provides the storage and sets that one field.
namespace Config
{
	Settings g_Settings;
}

namespace
{
	const int kVpW = 800;
	const int kVpH = 600;
	const float kCharWidth = 7.0f;      // what the stub GetTextExtentPoint32A measures

	unsigned int PackRgb(const unsigned char rgb[3])
	{
		return ((unsigned int)rgb[0] << 16) | ((unsigned int)rgb[1] << 8) | (unsigned int)rgb[2];
	}

	// |a - b|: the distance the fade is driven by, computed here so the check is against an
	// independent value rather than the ESP's own helper.
	float DistanceBetween(const float a[3], const float b[3])
	{
		const float dx = a[0] - b[0];
		const float dy = a[1] - b[1];
		const float dz = a[2] - b[2];
		return sqrtf(dx * dx + dy * dy + dz * dz);
	}

	// Three other players and one behind the viewer. The view is at the origin looking down +X
	// (angles 0,0,0), so +x is ahead of the viewer and -x is behind it.
	bool BuildWorld()
	{
		const float here[3]   = { 0.0f, 0.0f, 0.0f };
		const float none[3]   = { 0.0f, 0.0f, 0.0f };
		const float angles[3] = { 0.0f, 0.0f, 0.0f };

		FakeEngine::Reset();
		NameEsp::Reset();
		FakeEngine::SetSnapshotTime(1000);
		FakeEngine::SetFovString("90");
		FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
		FakeEngine::SetPlayer(1, "\\n\\^1Bitterman^7\\t\\1", (const float[]){ 128.0f,  128.0f, 0.0f });
		FakeEngine::SetPlayer(2, "\\n\\Slash\\t\\2",           (const float[]){ 128.0f, -128.0f, 0.0f });
		FakeEngine::SetPlayer(3, "\\n\\Behind\\t\\0",          (const float[]){ -500.0f,   0.0f, 0.0f });

		return NameEsp::Gather(1000, FakeEngine::Syscall());
	}

	const NameEsp::PlayerTag* FindTag(int clientNum)
	{
		const NameEsp::Frame& frame = NameEsp::Current();
		for (int i = 0; i < frame.playerCount; ++i)
			if (frame.players[i].clientNum == clientNum)
				return &frame.players[i];
		return NULL;
	}

	// where the overlay should have put a tag centred on this screen point - the same centring
	// plus the same on-screen clamp Draw() applies so glRasterPos stays valid (see nameEsp.cpp)
	void ExpectedCentreAt(const NameEsp::ScreenPoint& p, const char* name, float& x, float& y)
	{
		const float textWidth = kCharWidth * (float)strlen(name);
		x = p.x - textWidth * 0.5f;
		y = p.y;
		if (x < 0.0f)
			x = 0.0f;
		if (x + textWidth + 1.0f > (float)kVpW)
			x = (float)kVpW - textWidth - 1.0f;
		if (x < 0.0f)
			x = 0.0f;
		if (y < 0.0f)
			y = 0.0f;
		if (y + 14.0f + 1.0f > (float)kVpH)   // FONT_HEIGHT, see glText.h
			y = (float)kVpH - 14.0f - 1.0f;
		if (y < 0.0f)
			y = 0.0f;
	}

	// ... for this player's head anchor (the chest test projects its own anchor instead)
	bool ExpectedCentre(const NameEsp::PlayerTag& tag, float& x, float& y)
	{
		const NameEsp::Viewport vp = { 0, 0, kVpW, kVpH };
		NameEsp::ScreenPoint p;
		if (!NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, tag.origin, p))
			return false;
		ExpectedCentreAt(p, tag.name, x, y);
		return true;
	}

	int CountTextCalls(const char* name)
	{
		const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
		int n = 0;
		for (size_t i = 0; i < texts.size(); ++i)
			if (texts[i]->text == name)
				++n;
		return n;
	}

	// where the overlay should have put a distance tag centred on this screen point, in the
	// row rowOffset px below the head anchor, in a face FONT_HEIGHT of fontH px - the same
	// centring plus the same on-screen clamp distanceEsp.cpp applies
	void ExpectedDistanceAt(const NameEsp::ScreenPoint& p, const char* text,
	                        float rowOffset, float fontH, float& x, float& y)
	{
		const float textWidth = kCharWidth * (float)strlen(text);
		x = p.x - textWidth * 0.5f;
		y = p.y + rowOffset;
		if (x < 0.0f)
			x = 0.0f;
		if (x + textWidth + 1.0f > (float)kVpW)
			x = (float)kVpW - textWidth - 1.0f;
		if (x < 0.0f)
			x = 0.0f;
		if (y < 0.0f)
			y = 0.0f;
		if (y + fontH + 1.0f > (float)kVpH)
			y = (float)kVpH - fontH - 1.0f;
		if (y < 0.0f)
			y = 0.0f;
	}

	// the display-list base assigned to the i-th font bake (wglUseFontBitmaps) in the current
	// recorded stream - how a test tells which face a string went out in
	unsigned int ListBaseOfBake(int i)
	{
		const std::vector<const Rec::Call*> bakes = Rec::All("wglUseFontBitmaps");
		if (i < 0 || i >= (int)bakes.size())
			return 0;
		return (unsigned int)bakes[i]->a[2];
	}

	// BuildWorld() with the players' current weapons stamped on the snapshot before the gather,
	// and a fourth player who is carrying no weapon at all (WP_NONE).
	bool BuildArmedWorld()
	{
		const float here[3]   = { 0.0f, 0.0f, 0.0f };
		const float none[3]   = { 0.0f, 0.0f, 0.0f };
		const float angles[3] = { 0.0f, 0.0f, 0.0f };

		FakeEngine::Reset();
		NameEsp::Reset();
		FakeEngine::SetSnapshotTime(1000);
		FakeEngine::SetFovString("90");
		FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
		FakeEngine::SetPlayer(1, "\\n\\^1Bitterman^7\\t\\1", (const float[]){ 128.0f,  128.0f, 0.0f });
		FakeEngine::SetPlayer(2, "\\n\\Slash\\t\\2",           (const float[]){ 128.0f, -128.0f, 0.0f });
		FakeEngine::SetPlayer(3, "\\n\\Behind\\t\\0",          (const float[]){ -500.0f,   0.0f, 0.0f });
		FakeEngine::SetPlayer(4, "\\n\\NoWeapon\\t\\0",        (const float[]){ 200.0f,   60.0f, 0.0f });
		FakeEngine::SetPlayerWeapon(1, 4);   // WP_GRENADE_LAUNCHER
		FakeEngine::SetPlayerWeapon(2, 2);   // WP_MACHINEGUN
		FakeEngine::SetPlayerWeapon(3, 7);   // WP_RAILGUN (behind the viewer: counted behind)
		FakeEngine::SetPlayerWeapon(4, 0);   // WP_NONE: the game shows no weapon there either

		return NameEsp::Gather(1000, FakeEngine::Syscall());
	}

	// where the overlay should have put a weapon tag centred on this screen point - the same
	// centring plus the same on-screen clamp weaponEsp.cpp applies
	void ExpectedWeaponAt(const NameEsp::ScreenPoint& p, const char* name, float& x, float& y)
	{
		const float textWidth = kCharWidth * (float)strlen(name);
		x = p.x - textWidth * 0.5f;
		y = p.y;
		if (x < 0.0f)
			x = 0.0f;
		if (x + textWidth + 1.0f > (float)kVpW)
			x = (float)kVpW - textWidth - 1.0f;
		if (x < 0.0f)
			x = 0.0f;
		if (y < 0.0f)
			y = 0.0f;
		if (y + 14.0f + 1.0f > (float)kVpH)   // FONT_HEIGHT
			y = (float)kVpH - 14.0f - 1.0f;
		if (y < 0.0f)
			y = 0.0f;
	}

	// the screen point of a player's LEG anchor (the weapon ESP's anchor, below the feet)
	bool ProjectLeg(const NameEsp::PlayerTag& tag, NameEsp::ScreenPoint& out)
	{
		const NameEsp::Viewport vp = { 0, 0, kVpW, kVpH };
		float leg[3];
		WeaponEsp::LegAnchor(tag, leg);
		return NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, leg, out);
	}

	// the (x, y) of the first vertex of the first GL_QUADS in the stream - with the ESP draws
	// in order, that is the first quad-issuing overlay's first bar/chip
	bool FirstQuadOrigin(float& outX, float& outY)
	{
		std::vector<Rec::Call>& calls = Rec::Calls();
		for (size_t i = 0; i < calls.size(); ++i)
		{
			if (calls[i].fn != "glBegin" || (int)calls[i].a[0] != 0x0007)   // GL_QUADS
				continue;
			for (size_t j = i + 1; j < calls.size(); ++j)
			{
				if (calls[j].fn == "glVertex2f")
				{
					outX = (float)calls[j].a[0];
					outY = (float)calls[j].a[1];
					return true;
				}
				if (calls[j].fn == "glEnd")
					break;
			}
		}
		return false;
	}
}

// =============================================================================================== //

static void TestFontIsBuiltOnce()
{
	Section("GL::Font build");

	CHECK_TRUE(BuildWorld(), "frame gathered");
	Rec::CurrentDC() = NULL;                       // a fresh context, so the font must be built
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp = true;

	NameEsp::Draw();
	CHECK_INT(Rec::Count("glGenLists"), 1, "96 display lists allocated once");
	CHECK_INT(Rec::Count("wglUseFontBitmaps"), 1, "glyph bitmaps baked once");

	const Rec::Call* bake = Rec::Last("wglUseFontBitmaps");
	if (bake)
	{
		CHECK_INT((int)bake->a[0], 32, "bitmaps start at character 32");
		CHECK_INT((int)bake->a[1], 96, "96 characters baked");
	}

	// a second frame on the same context must not rebuild it
	Rec::Reset(0, 0, kVpW, kVpH);
	NameEsp::Draw();
	CHECK_INT(Rec::Count("glGenLists"), 0, "no rebuild on the next frame");

	// ... but a new context (vid_restart) must
	Rec::CurrentDC() = (void*)(size_t)0xBEEF;
	Rec::Reset(0, 0, kVpW, kVpH);
	NameEsp::Draw();
	CHECK_INT(Rec::Count("glGenLists"), 1, "rebuilt after the GL context changed");
	Rec::CurrentDC() = NULL;
}

static void TestOverlayState()
{
	Section("overlay GL state (GL::SetupOrtho / GL::RestoreGL)");

	CHECK_TRUE(BuildWorld(), "frame gathered");
	Rec::CurrentDC() = NULL;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp = true;
	NameEsp::Draw();

	const Rec::Call* vp = Rec::Last("glViewport");
	CHECK_TRUE(vp != NULL, "viewport set for the overlay");
	if (vp)
	{
		CHECK_INT((int)vp->a[0], 0, "viewport x");
		CHECK_INT((int)vp->a[1], 0, "viewport y");
		CHECK_INT((int)vp->a[2], kVpW, "viewport keeps the game's width");
		CHECK_INT((int)vp->a[3], kVpH, "viewport keeps the game's height");
	}

	const Rec::Call* ortho = Rec::Last("glOrtho");
	CHECK_TRUE(ortho != NULL, "ortho projection installed");
	if (ortho)
	{
		CHECK_NEAR(ortho->a[0], 0.0, 0.001, "ortho left");
		CHECK_NEAR(ortho->a[1], (double)kVpW, 0.001, "ortho right = viewport width");
		CHECK_NEAR(ortho->a[2], (double)kVpH, 0.001, "ortho bottom = viewport height (y grows down)");
		CHECK_NEAR(ortho->a[3], 0.0, 0.001, "ortho top");
	}

	// SetupOrtho turns texturing, blending and the depth test off; RestoreGL turns them back on
	CHECK_TRUE(Rec::Count("glDisable") >= 3, "texturing / blending / depth test disabled");
	CHECK_TRUE(Rec::Count("glEnable") >= 3, "and enabled again afterwards");
	CHECK_INT(Rec::Count("glPushAttrib"), Rec::Count("glPopAttrib"), "attribute pushes are balanced");
	CHECK_INT(Rec::Count("glPushMatrix"), Rec::Count("glPopMatrix"), "matrix pushes are balanced");

	// the legacy state guard wraps the whole overlay
	CHECK_INT(Rec::Count("LegacyStateGuard()"), 1, "legacy GL state guard entered");
	CHECK_INT(Rec::Count("~LegacyStateGuard()"), 1, "legacy GL state guard left");
	CHECK_TRUE(Rec::IndexOf(Rec::Last("~LegacyStateGuard()")) >
	           Rec::IndexOf(Rec::Last("glCallLists")),
	           "the guard outlives the drawing");
}

static void TestTagsDrawn()
{
	Section("what is drawn for each player");

	CHECK_TRUE(BuildWorld(), "frame gathered");
	Rec::CurrentDC() = NULL;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp = true;
	NameEsp::Draw();

	// two players in front of the viewer, one behind it
	CHECK_TRUE(FindTag(1) != NULL, "player 1 is in the frame");
	CHECK_TRUE(FindTag(2) != NULL, "player 2 is in the frame");
	CHECK_TRUE(FindTag(3) != NULL, "the player behind us is in the frame");
	CHECK_INT(Rec::Count("glCallLists"), 4, "two tags, each drawn twice (drop shadow + text)");
	CHECK_INT(CountTextCalls("Bitterman"), 2, "\"Bitterman\" drawn twice");
	CHECK_INT(CountTextCalls("Slash"), 2, "\"Slash\" drawn twice");
	CHECK_INT(CountTextCalls("Behind"), 0, "the player behind the viewer is not drawn");
	CHECK_INT(CountTextCalls("^1Bitterman^7"), 0, "colour codes never reach the screen");

	const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
	for (size_t i = 0; i < texts.size(); ++i)
	{
		const Rec::Call* text = texts[i];
		const Rec::Call* pos  = Rec::Prev(text, "glRasterPos2f");
		const bool isShadow = (text->rgb == 0x000000u);

		const NameEsp::PlayerTag* tag = NULL;
		if (text->text == "Bitterman") tag = FindTag(1);
		else if (text->text == "Slash") tag = FindTag(2);

		char what[128];
		snprintf(what, sizeof(what), "\"%s\" has a raster position", text->text.c_str());
		CHECK_TRUE(pos != NULL, what);
		if (!pos || !tag)
			continue;

		float wantX = 0.0f, wantY = 0.0f;
		if (!ExpectedCentre(*tag, wantX, wantY))
		{
			Fail(text->text.c_str(), "player does not project");
			continue;
		}
		if (isShadow)
		{
			wantX += 1.0f;
			wantY += 1.0f;
		}

		snprintf(what, sizeof(what), "\"%s\"%s raster x", text->text.c_str(), isShadow ? " shadow" : "");
		CHECK_NEAR(pos->a[0], wantX, 0.02, what);
		snprintf(what, sizeof(what), "\"%s\"%s raster y", text->text.c_str(), isShadow ? " shadow" : "");
		CHECK_NEAR(pos->a[1], wantY, 0.02, what);

		// colour: black drop shadow, otherwise the team palette
		unsigned char palette[3];
		NameEsp::TeamColor(tag->team, palette);
		const unsigned int wantRgb = isShadow ? 0x000000u : PackRgb(palette);
		snprintf(what, sizeof(what), "\"%s\"%s colour", text->text.c_str(), isShadow ? " shadow" : "");
		CHECK_UINT(text->rgb, wantRgb, what);
	}

	// the two teams really do get different colours
	CHECK_TRUE(FindTag(1) && FindTag(2), "both tagged players present");
	if (FindTag(1) && FindTag(2))
	{
		unsigned char red[3], blue[3];
		NameEsp::TeamColor(FindTag(1)->team, red);
		NameEsp::TeamColor(FindTag(2)->team, blue);
		CHECK_TRUE(PackRgb(red) != PackRgb(blue), "red and blue teams are drawn in different colours");
	}
}

static void TestChestAnchoredUpClose()
{
	Section("the tag re-anchors to the chest when the head anchor leaves the screen");

	// A bot 30 units ahead with the viewer pitched down 20 degrees: the anchor above the head
	// projects off the top of the screen while the body is dead centre (see the comment in
	// Draw()). The tag must sit on the body at full brightness, not dimmed at the top edge.
	const float here[3]   = { 0.0f, 0.0f, 0.0f };
	const float none[3]   = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 20.0f, 0.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetFovString("90");
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Close\\t\\0", (const float[]){ 30.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "frame gathered");
	const NameEsp::PlayerTag* tag = FindTag(1);
	CHECK_TRUE(tag != NULL, "player 1 is in the frame");
	if (!tag)
		return;

	// The head anchor must genuinely be off screen, or the test proves nothing...
	const NameEsp::Viewport vp = { 0, 0, kVpW, kVpH };
	NameEsp::ScreenPoint head = { 0.0f, 0.0f, false };
	const bool headOk = NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, tag->origin, head);
	CHECK_TRUE(headOk, "the head anchor projects");
	if (headOk)
		CHECK_TRUE(!head.inView, "the head anchor is off the top of the screen");

	// ... and the chest anchor on screen.
	float chest[3] = { tag->origin[0], tag->origin[1],
	                   tag->origin[2] - q3::kPlayerTagHeight + q3::kChestHeight };
	NameEsp::ScreenPoint pc = { 0.0f, 0.0f, false };
	const bool chestOk = NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, chest, pc);
	CHECK_TRUE(chestOk, "the chest anchor projects");
	if (chestOk)
		CHECK_TRUE(pc.inView, "the chest anchor is on screen");
	if (!chestOk)
		return;

	Rec::CurrentDC() = NULL;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp = true;
	NameEsp::Draw();

	CHECK_INT(CountTextCalls("Close"), 2, "\"Close\" drawn twice (drop shadow + text)");

	float wantX = 0.0f, wantY = 0.0f;
	ExpectedCentreAt(pc, "Close", wantX, wantY);

	unsigned char palette[3];
	NameEsp::TeamColor(0, palette);
	const unsigned int wantRgb = PackRgb(palette);

	const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
	for (size_t i = 0; i < texts.size(); ++i)
	{
		const Rec::Call* text = texts[i];
		const Rec::Call* pos  = Rec::Prev(text, "glRasterPos2f");
		const bool isShadow = (text->rgb == 0x000000u);

		char what[128];
		snprintf(what, sizeof(what), "\"Close\"%s has a raster position", isShadow ? " shadow" : "");
		CHECK_TRUE(pos != NULL, what);
		if (!pos)
			continue;
		snprintf(what, sizeof(what), "\"Close\"%s raster x", isShadow ? " shadow" : "");
		CHECK_NEAR(pos->a[0], wantX + (isShadow ? 1.0f : 0.0f), 0.02, what);
		snprintf(what, sizeof(what), "\"Close\"%s raster y", isShadow ? " shadow" : "");
		CHECK_NEAR(pos->a[1], wantY + (isShadow ? 1.0f : 0.0f), 0.02, what);

		// full team colour: the chest point is in view, so the tag is NOT dimmed
		snprintf(what, sizeof(what), "\"Close\"%s colour", isShadow ? " shadow" : "");
		CHECK_UINT(text->rgb, isShadow ? 0x000000u : wantRgb, what);
	}

	const NameEsp::DrawStats& st = NameEsp::LastDrawStats();
	CHECK_INT(st.drawn, 1, "one tag drawn");
	CHECK_INT(st.inView, 1, "drawn ahead, full brightness");
	CHECK_INT(st.edge, 0, "none at the edge");
	CHECK_INT(st.behind, 0, "none behind");
}

static void TestDrawStats()
{
	Section("Draw() reports what it did with each tag");

	// One bot dead ahead, one off the side of the screen (its chest is off screen too, so the
	// chest fallback correctly declines and the tag stays a dimmed edge marker), one behind.
	const float here[3]   = { 0.0f, 0.0f, 0.0f };
	const float none[3]   = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetFovString("90");
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Ahead\\t\\0", (const float[]){ 128.0f, 0.0f, 0.0f });
	FakeEngine::SetPlayer(2, "\\n\\Edge\\t\\0",  (const float[]){ 128.0f, 400.0f, 0.0f });
	FakeEngine::SetPlayer(3, "\\n\\Back\\t\\0",  (const float[]){ -128.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "frame gathered");

	Rec::CurrentDC() = NULL;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp = true;
	NameEsp::Draw();

	CHECK_INT(Rec::Count("glCallLists"), 4, "two tags, each drawn twice (drop shadow + text)");
	CHECK_INT(CountTextCalls("Ahead"), 2, "\"Ahead\" drawn twice");
	CHECK_INT(CountTextCalls("Edge"), 2, "\"Edge\" drawn twice");
	CHECK_INT(CountTextCalls("Back"), 0, "the player behind the viewer is not drawn");

	// "Ahead" keeps the full team colour; "Edge" is dimmed 55% like Draw()'s Dim() does.
	unsigned char palette[3];
	NameEsp::TeamColor(0, palette);
	const unsigned int fullRgb = PackRgb(palette);
	const unsigned int dimRgb = ((unsigned int)(palette[0] * 55 / 100) << 16) |
	                            ((unsigned int)(palette[1] * 55 / 100) << 8) |
	                            (unsigned int)(palette[2] * 55 / 100);
	bool aheadFull = false, edgeDim = false;
	const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
	for (size_t i = 0; i < texts.size(); ++i)
	{
		if (texts[i]->rgb == 0x000000u)
			continue;                              // drop shadow
		if (texts[i]->text == "Ahead" && texts[i]->rgb == fullRgb)
			aheadFull = true;
		if (texts[i]->text == "Edge" && texts[i]->rgb == dimRgb)
			edgeDim = true;
	}
	CHECK_TRUE(aheadFull, "\"Ahead\" is drawn in the full team colour");
	CHECK_TRUE(edgeDim, "\"Edge\" is drawn dimmed");

	const NameEsp::DrawStats& st = NameEsp::LastDrawStats();
	CHECK_INT(st.drawn, 2, "two tags drawn");
	CHECK_INT(st.inView, 1, "one drawn ahead");
	CHECK_INT(st.edge, 1, "one clamped to the edge");
	CHECK_INT(st.behind, 1, "one skipped behind the viewer");

	// A frame that draws nothing reports zeros.
	Config::g_Settings.nameEsp = false;
	NameEsp::Draw();
	const NameEsp::DrawStats& z = NameEsp::LastDrawStats();
	CHECK_INT(z.drawn, 0, "feature off -> zero drawn");
	CHECK_INT(z.inView, 0, "feature off -> zero ahead");
	CHECK_INT(z.edge, 0, "feature off -> zero at the edge");
	CHECK_INT(z.behind, 0, "feature off -> zero behind");
}

static void TestDrawsNothingWhenItShouldNot()
{
	Section("no drawing when there is nothing to draw");

	// feature switched off
	CHECK_TRUE(BuildWorld(), "frame gathered");
	Rec::CurrentDC() = NULL;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp = false;
	NameEsp::Draw();
	CHECK_INT(Rec::Count("glCallLists"), 0, "feature off -> no text");
	CHECK_INT(Rec::Count("glViewport"), 0, "feature off -> the GL state is not touched at all");
	CHECK_INT(Rec::Count("glGenLists"), 0, "feature off -> no font built");

	// feature on, but no frame (not connected)
	Config::g_Settings.nameEsp = true;
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetConnected(false);
	CHECK_TRUE(!NameEsp::Gather(1000, FakeEngine::Syscall()), "no snapshot -> no frame");
	Rec::Reset(0, 0, kVpW, kVpH);
	NameEsp::Draw();
	CHECK_INT(Rec::Count("glCallLists"), 0, "not connected -> no text");

	// frame with nobody in it but the local player
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	const float here[3] = { 0.0f, 0.0f, 0.0f };
	const float none[3] = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "frame with only the local player");
	CHECK_INT(NameEsp::Current().playerCount, 0, "no tags");
	Rec::Reset(0, 0, kVpW, kVpH);
	NameEsp::Draw();
	CHECK_INT(Rec::Count("glCallLists"), 0, "nobody to tag -> no text");

	// a degenerate viewport
	CHECK_TRUE(BuildWorld(), "frame gathered");
	Rec::Reset(0, 0, 0, 0);
	NameEsp::Draw();
	CHECK_INT(Rec::Count("glCallLists"), 0, "empty viewport -> no text");
}

static void TestFormatSpecifierName()
{
	Section("a hostile name is printed as text, not as a format string");

	const float here[3] = { 0.0f, 0.0f, 0.0f };
	const float none[3] = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\%s%n%x%p", (const float[]){ 128.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "frame gathered");

	Rec::CurrentDC() = NULL;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp = true;
	NameEsp::Draw();
	CHECK_INT(CountTextCalls("%s%n%x%p"), 2, "the specifiers survive verbatim");
}

// =============================================================================================== //

// =============================================================================================== //
// A tag that just became visible ramps its alpha in over kFadeInMs instead of popping into
// existence. The clock is the frame's own server time, so the test drives it by re-gathering at
// later frame times.
// =============================================================================================== //
static void TestTagFadesIn()
{
	Section("a tag fades in instead of popping");

	CHECK_TRUE(BuildWorld(), "frame gathered");
	Rec::CurrentDC() = NULL;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp = true;
	NameEsp::ResetDrawState();        // a fresh level: no tag has faded in yet

	NameEsp::Draw();
	const Rec::Call* text = Rec::Last("glCallLists");
	CHECK_TRUE(text != NULL, "the tag is drawn on the very first frame");
	if (!text)
		return;
	CHECK_TRUE(text->alpha > 0.0f && text->alpha < 1.0f, "at a partial alpha, not at full brightness");

	// ... and the overlay is actually blending, or an alpha would do nothing at all
	CHECK_TRUE(Rec::Count("glBlendFunc") > 0, "the overlay installs a blend function");
	const Rec::Call* blend = Rec::Last("glBlendFunc");
	if (blend)
	{
		CHECK_INT((int)blend->a[0], 0x0302, "blend source is GL_SRC_ALPHA");
		CHECK_INT((int)blend->a[1], 0x0303, "blend destination is GL_ONE_MINUS_SRC_ALPHA");
	}
	// the drop shadow fades with the text: it is the first of the two calls for this name
	const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
	CHECK_TRUE(texts.size() >= 2, "shadow and text were both issued");
	if (texts.size() >= 2)
		CHECK_NEAR(texts[0]->alpha, text->alpha, 1e-6, "the shadow fades at the same alpha");

	// Drive 30 further frames 16 ms apart (480 ms): the ramp climbs monotonically and reaches
	// full opacity, which is also where it stops - a long-lived tag must stay opaque.
	float previous = text->alpha;
	bool monotonic = true;
	bool reachedFull = false;
	bool stayedFull = true;
	for (int i = 1; i <= 30; ++i)
	{
		Rec::Reset(0, 0, kVpW, kVpH);
		if (!NameEsp::Gather(1000 + i * 16, FakeEngine::Syscall()))
		{
			monotonic = false;
			break;
		}
		NameEsp::Draw();
		const Rec::Call* t = Rec::Last("glCallLists");
		if (!t)
		{
			monotonic = false;
			break;
		}
		if (t->alpha < previous - 1e-6f)
			monotonic = false;
		if (t->alpha >= 1.0f)
			reachedFull = true;
		else if (reachedFull)
			stayedFull = false;
		previous = t->alpha;
	}
	CHECK_TRUE(monotonic, "the alpha never drops while the player stays visible");
	CHECK_TRUE(reachedFull, "and it reaches full opacity inside the ramp");
	CHECK_TRUE(stayedFull, "and stays there");
}

// =============================================================================================== //
// Once the tag has moved to the chest it stays there for a few frames after the head anchor comes
// back on screen. Without that hold, aiming up and down across the boundary swaps the anchor every
// other frame and the name hops between two points a head apart.
// =============================================================================================== //
namespace
{
	const float kHysteresisBotAt[3] = { 30.0f, 0.0f, 0.0f };

	// raster position the tag would be at when anchored to `world`, for the current frame
	bool AnchorAt(const float world[3], const char* name, float& outX, float& outY)
	{
		const NameEsp::Viewport vp = { 0, 0, kVpW, kVpH };
		NameEsp::ScreenPoint p;
		if (!NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, world, p))
			return false;
		ExpectedCentreAt(p, name, outX, outY);
		return true;
	}

	bool GatherHysteresisFrame(float pitch)
	{
		const float here[3] = { 0.0f, 0.0f, 0.0f };
		const float none[3] = { 0.0f, 0.0f, 0.0f };
		const float angles[3] = { pitch, 0.0f, 0.0f };

		// A real snapshot carries each player once, so rebuild the entity list rather than
		// appending another copy of the same client every frame.
		FakeEngine::Reset();
		FakeEngine::SetSnapshotTime(1000);
		FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
		FakeEngine::SetPlayer(1, "\\n\\Close\\t\\0", kHysteresisBotAt);
		return NameEsp::Gather(1000, FakeEngine::Syscall());
	}

	// Where the tag actually landed: the raster position of the first non-shadow text call.
	bool DrawnAt(float& outX, float& outY)
	{
		const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
		for (size_t i = 0; i < texts.size(); ++i)
		{
			if (texts[i]->rgb == 0x000000u)
				continue;                            // the drop shadow
			const Rec::Call* pos = Rec::Prev(texts[i], "glRasterPos2f");
			if (!pos)
				return false;
			outX = (float)pos->a[0];
			outY = (float)pos->a[1];
			return true;
		}
		return false;
	}

	bool CloserTo(float drawnX, float drawnY, float x, float y, float otherX, float otherY)
	{
		const float d  = fabs(drawnX - x) + fabs(drawnY - y);
		const float od = fabs(drawnX - otherX) + fabs(drawnY - otherY);
		return d < od;
	}
}

static void TestChestAnchorHoldsItsGround()
{
	Section("the chest anchor is not given up after a single frame");

	// A bot 30 units ahead: pitched down 20 degrees the anchor above the head leaves the top of the
	// screen while the body is on it (the chest case); pitched level again it comes back. Both
	// anchors are re-projected through the frame's own view each time, because the view is what
	// changed between the frames.
	Rec::CurrentDC() = NULL;
	Config::g_Settings.nameEsp = true;
	NameEsp::ResetDrawState();        // a fresh level: the tag has not settled on an anchor yet

	// frame 1: pitched down, the head anchor is off screen -> the tag goes to the chest
	CHECK_TRUE(GatherHysteresisFrame(20.0f), "pitched-down frame gathered");
	const NameEsp::PlayerTag* tag = FindTag(1);
	CHECK_TRUE(tag != NULL, "player 1 is in the frame");
	if (!tag)
		return;
	const float chest[3] = { tag->origin[0], tag->origin[1],
	                         tag->origin[2] - q3::kPlayerTagHeight + q3::kChestHeight };

	float headX = 0.0f, headY = 0.0f, chestX = 0.0f, chestY = 0.0f;
	CHECK_TRUE(AnchorAt(tag->origin, "Close", headX, headY), "the head anchor projects");
	CHECK_TRUE(AnchorAt(chest, "Close", chestX, chestY), "the chest anchor projects");
	CHECK_TRUE(fabs(chestY - headY) > 5.0, "the two anchors are far enough apart to tell apart");

	Rec::Reset(0, 0, kVpW, kVpH);
	NameEsp::Draw();
	float drawnX = 0.0f, drawnY = 0.0f;
	CHECK_TRUE(DrawnAt(drawnX, drawnY), "the tag was drawn on the pitched-down frame");
	CHECK_TRUE(CloserTo(drawnX, drawnY, chestX, chestY, headX, headY), "it moved to the chest");

	// frames 2 and 3: the view is level again, so the head anchor is back on screen - but the tag
	// must hold the chest for a few frames before it goes back
	for (int i = 0; i < 2; ++i)
	{
		CHECK_TRUE(GatherHysteresisFrame(0.0f), "level frame gathered");
		tag = FindTag(1);
		if (!tag)
			return;
		const float chestNow[3] = { tag->origin[0], tag->origin[1],
		                            tag->origin[2] - q3::kPlayerTagHeight + q3::kChestHeight };
		AnchorAt(tag->origin, "Close", headX, headY);
		AnchorAt(chestNow, "Close", chestX, chestY);

		Rec::Reset(0, 0, kVpW, kVpH);
		NameEsp::Draw();
		char what[96];
		snprintf(what, sizeof(what), "frame %d still holds the chest anchor", i + 2);
		CHECK_TRUE(DrawnAt(drawnX, drawnY), what);
		CHECK_TRUE(CloserTo(drawnX, drawnY, chestX, chestY, headX, headY), what);
	}

	// frame 4: the hold has expired, so the tag goes back above the head
	CHECK_TRUE(GatherHysteresisFrame(0.0f), "level frame gathered");
	Rec::Reset(0, 0, kVpW, kVpH);
	NameEsp::Draw();
	CHECK_TRUE(DrawnAt(drawnX, drawnY), "the tag was drawn on the fourth frame");
	CHECK_TRUE(CloserTo(drawnX, drawnY, headX, headY, chestX, chestY), "and it is back above the head");
}

// =============================================================================================== //
// DISTANCE ESP: the "NM" text, its row in the stack, and the scale + fade with range
// =============================================================================================== //

static void TestDistanceTagsDrawn()
{
	Section("DISTANCE ESP drawing (metres text on the head anchor when nothing else is on)");

	CHECK_TRUE(BuildWorld(), "frame gathered");
	Rec::CurrentDC() = (void*)(size_t)0xD157A;   // a fresh context: every bucket must be baked
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp     = false;
	Config::g_Settings.distanceEsp = true;
	DistanceEsp::ResetDrawState();
	DistanceEsp::Draw();

	// both in-view players are |vieworg - lerpOrigin| = sqrt(128^2 + 128^2 + 26^2) = 182.9
	// away -> "183M"; the player behind the viewer is skipped
	CHECK_INT(CountTextCalls("183M"), 4, "two tags, each drawn twice (drop shadow + text)");
	CHECK_INT(CountTextCalls("Bitterman"), 0, "distance ESP alone prints no names");
	CHECK_INT(CountTextCalls("Behind"), 0, "the player behind the viewer is not drawn");

	// the six faces span FONT_HEIGHT down to kMinScale, baked for the fresh context
	{
		const std::vector<const Rec::Call*> bakes = Rec::All("CreateFontA");
		CHECK_INT((int)bakes.size(), 6, "six font buckets baked");
		const double heights[6] = { -14, -12, -10, -8, -6, -5 };
		for (int i = 0; i < 6 && i < (int)bakes.size(); ++i)
		{
			char what[64];
			snprintf(what, sizeof(what), "bucket %d baked at its height", i);
			CHECK_NEAR(bakes[i]->a[0], heights[i], 0.001, what);
		}
	}

	// position: centred on the head anchor's own row (no name above -> row 0), white with the
	// 1px black drop shadow, clamped to the overlay like the name
	const NameEsp::Viewport vp = { 0, 0, kVpW, kVpH };
	const NameEsp::PlayerTag* b1 = FindTag(1);
	const NameEsp::PlayerTag* b2 = FindTag(2);
	CHECK_TRUE(b1 != NULL && b2 != NULL, "both tagged players present");
	if (b1 && b2)
	{
		NameEsp::ScreenPoint p1, p2;
		CHECK_TRUE(NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, b1->origin, p1),
		           "player 1 projects");
		CHECK_TRUE(NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, b2->origin, p2),
		           "player 2 projects");
		float want1x = 0.0f, want1y = 0.0f, want2x = 0.0f, want2y = 0.0f;
		ExpectedDistanceAt(p1, "183M", 0.0f, (float)FONT_HEIGHT, want1x, want1y);
		ExpectedDistanceAt(p2, "183M", 0.0f, (float)FONT_HEIGHT, want2x, want2y);

		int checked = 0;
		const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
		for (size_t i = 0; i < texts.size(); ++i)
		{
			if (texts[i]->text != "183M")
				continue;
			const Rec::Call* pos = Rec::Prev(texts[i], "glRasterPos2f");
			CHECK_TRUE(pos != NULL, "the distance text has a raster position");
			if (!pos)
				continue;
			const bool isShadow = (texts[i]->rgb == 0x000000u);
			// the two players sit at opposite edges (x 0 and x ~771), so the nearer expected x
			// says which one this text belongs to
			const bool isLeft = fabsf(pos->a[0] - want1x) <= fabsf(pos->a[0] - want2x);
			const float wantX = isLeft ? want1x : want2x;
			const float wantY = isLeft ? want1y : want2y;
			CHECK_NEAR(pos->a[0], wantX + (isShadow ? 1.0f : 0.0f), 0.02, "raster x");
			CHECK_NEAR(pos->a[1], wantY + (isShadow ? 1.0f : 0.0f), 0.02, "raster y");
			if (!isShadow)
			{
				CHECK_UINT(texts[i]->rgb, 0xffffffu, "neutral white, not the team colour");
				CHECK_TRUE(texts[i]->alpha > 0.0f && texts[i]->alpha < 1.0f,
				           "fading in on the first frame");
			}
			++checked;
		}
		CHECK_INT(checked, 4, "all four issued texts were checked");
	}

	const DistanceEsp::DrawStats& st = DistanceEsp::LastDrawStats();
	CHECK_INT(st.drawn, 2, "two tags drawn");
	CHECK_INT(st.inView, 2, "drawn ahead, full brightness");
	CHECK_INT(st.edge, 0, "none at the edge");
	CHECK_INT(st.behind, 1, "one skipped behind the viewer");
	CHECK_INT(st.faded, 0, "nothing faded out");

	// feature off: nothing issued, stats zeroed
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.distanceEsp = false;
	DistanceEsp::Draw();
	CHECK_INT(Rec::Count("glCallLists"), 0, "feature off -> no text");
	const DistanceEsp::DrawStats& z = DistanceEsp::LastDrawStats();
	CHECK_INT(z.drawn, 0, "feature off -> zero drawn");
	CHECK_INT(z.behind, 0, "feature off -> zero behind");
}

static void TestDistanceStacksUnderName()
{
	Section("DISTANCE ESP stacking (the name on the anchor, the distance one row below it)");

	CHECK_TRUE(BuildWorld(), "frame gathered");
	Rec::CurrentDC() = (void*)(size_t)0xD157B;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp     = true;
	Config::g_Settings.distanceEsp = true;
	NameEsp::ResetDrawState();
	DistanceEsp::ResetDrawState();
	NameEsp::Draw();
	DistanceEsp::Draw();

	const NameEsp::PlayerTag* b1 = FindTag(1);
	CHECK_TRUE(b1 != NULL, "player 1 is in the frame");
	if (!b1)
		return;
	const NameEsp::Viewport vp = { 0, 0, kVpW, kVpH };
	NameEsp::ScreenPoint p;
	CHECK_TRUE(NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, b1->origin, p),
	           "head projects");

	// where each row should sit: the name on the anchor, the distance one row below it
	float nameX = 0.0f, nameY = 0.0f, distX = 0.0f, distY = 0.0f;
	ExpectedCentreAt(p, "Bitterman", nameX, nameY);
	ExpectedDistanceAt(p, "183M", NameEsp::kEspRowHeight, (float)FONT_HEIGHT, distX, distY);

	const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
	bool sawName = false, sawDist = false;
	for (size_t i = 0; i < texts.size(); ++i)
	{
		const Rec::Call* text = texts[i];
		if (text->rgb == 0x000000u)
			continue;                            // drop shadow
		const Rec::Call* pos = Rec::Prev(text, "glRasterPos2f");
		if (!pos)
			continue;
		if (text->text == "Bitterman" && !sawName)
		{
			CHECK_NEAR(pos->a[0], nameX, 0.02, "the name sits on the head anchor row");
			CHECK_NEAR(pos->a[1], nameY, 0.02, "the name is at the anchor");
			sawName = true;
		}
		if (text->text == "183M" && !sawDist)
		{
			CHECK_NEAR(pos->a[1], distY, 0.02, "the distance sits one row below the name");
			CHECK_NEAR(pos->a[1] - nameY, (double)NameEsp::kEspRowHeight, 0.02,
			           "exactly one row under the name, never overlapping it");
			sawDist = true;
		}
	}
	CHECK_TRUE(sawName, "the name was drawn");
	CHECK_TRUE(sawDist, "the distance was drawn");
	CHECK_INT(Rec::Count("glBegin"), 0, "no bar/chip quads: nothing but the two text rows");
}

static void TestDistanceScaleAndFade()
{
	Section("DISTANCE ESP scales down and fades out with range");

	// one bot 1500 units straight ahead: mid-fade, so neither the full-size face nor full
	// opacity is what the tag goes out in
	const float here[3]   = { 0.0f, 0.0f, 0.0f };
	const float none[3]   = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetFovString("90");
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Far\\t\\0", (const float[]){ 1500.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "frame gathered");
	const NameEsp::PlayerTag* tag = FindTag(1);
	CHECK_TRUE(tag != NULL, "the far player is in the frame");
	if (!tag)
		return;
	{
		const float want = DistanceBetween(NameEsp::Current().view.origin, tag->lerpOrigin);
		CHECK_NEAR(tag->distance, want, 0.01, "distance is |vieworg - lerpOrigin|");
	}

	Rec::CurrentDC() = (void*)(size_t)0xD157C;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp     = false;
	Config::g_Settings.distanceEsp = true;
	DistanceEsp::ResetDrawState();

	// first frame: the fresh context bakes all six faces, so this is where the tag's face
	// base comes from (later frames reuse the lists)
	DistanceEsp::Draw();
	unsigned int wantBase = 0;
	{
		const std::vector<const Rec::Call*> bakes = Rec::All("CreateFontA");
		CHECK_INT((int)bakes.size(), 6, "six font buckets baked for the fresh context");
		const double heights[6] = { -14, -12, -10, -8, -6, -5 };
		for (int i = 0; i < 6 && i < (int)bakes.size(); ++i)
		{
			char what[64];
			snprintf(what, sizeof(what), "bucket %d baked at its height", i);
			CHECK_NEAR(bakes[i]->a[0], heights[i], 0.001, what);
		}
		// the mid-range tag (scale 9.2px) must go out in the 10px face: the third bucket
		wantBase = ListBaseOfBake(2) - 32;      // EmitText sets the list base to base - 32
		CHECK_TRUE(wantBase != 0, "the 10px face has a list base");
	}

	// drive the fade-in ramp to completion (30 further frames x 16 ms > kFadeInMs), so the
	// alpha left on the final frame is the distance fade alone
	for (int i = 1; i <= 30; ++i)
	{
		CHECK_TRUE(NameEsp::Gather(1000 + i * 16, FakeEngine::Syscall()), "later frame gathered");
		Rec::Reset(0, 0, kVpW, kVpH);
		DistanceEsp::Draw();
	}

	const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
	bool sawText = false;
	for (size_t i = 0; i < texts.size(); ++i)
	{
		const Rec::Call* text = texts[i];
		if (text->text != "1500M")
			continue;
		if (text->rgb == 0x000000u)
			continue;                            // the drop shadow
		sawText = true;
		// scale 14 * (1 - t * 0.65) with t = (1500.2 - 400) / 2100 is 9.2px -> the 10px face
		CHECK_UINT(text->listBase, wantBase, "the mid-range tag uses the scaled-down face");
		// ... and the alpha is the distance fade (the fade-in ramp is finished)
		const float t = (tag->distance - DistanceEsp::kFadeStartDist) /
		               (DistanceEsp::kFadeEndDist - DistanceEsp::kFadeStartDist);
		CHECK_NEAR(text->alpha, 1.0f - t, 0.01, "faded by the distance ramp");
	}
	CHECK_TRUE(sawText, "the far tag was drawn");

	// and a player past the fade end is dropped entirely
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(2000);
	FakeEngine::SetFovString("90");
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Gone\\t\\0", (const float[]){ 3000.0f, 0.0f, 0.0f });
	CHECK_TRUE(NameEsp::Gather(2000, FakeEngine::Syscall()), "far frame gathered");
	const NameEsp::PlayerTag* gone = FindTag(1);
	CHECK_TRUE(gone != NULL, "the very far player is in the frame");
	if (gone)
		CHECK_TRUE(gone->distance > DistanceEsp::kFadeEndDist, "really past the fade end");
	Rec::Reset(0, 0, kVpW, kVpH);
	DistanceEsp::ResetDrawState();
	DistanceEsp::Draw();
	CHECK_INT(Rec::Count("glCallLists"), 0, "past the fade end: nothing issued");
	CHECK_INT(DistanceEsp::LastDrawStats().faded, 1, "counted as faded, not drawn");
}

// =============================================================================================== //
// WEAPON ESP: the weapon name (or icon) at the LEG position, the stacking against the
// head-anchored ESPs, and the scale + fade with range
// =============================================================================================== //

static void TestWeaponTextAtLeg()
{
	Section("WEAPON ESP text mode - the weapon name at the leg position, through walls");

	CHECK_TRUE(BuildArmedWorld(), "frame gathered");
	Rec::CurrentDC() = (void*)(size_t)0xD157D;   // a fresh context: the faces must be baked
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp      = false;
	Config::g_Settings.distanceEsp  = false;
	Config::g_Settings.weaponEsp    = true;
	Config::g_Settings.weaponEspStyle = 0;        // text
	WeaponEsp::ResetDrawState();
	WeaponEsp::Draw();

	// the six faces span FONT_HEIGHT down to kMinScale, baked for the fresh context
	{
		const std::vector<const Rec::Call*> bakes = Rec::All("CreateFontA");
		CHECK_INT((int)bakes.size(), 6, "six font buckets baked");
	}

	// two armed players ahead of the viewer get their weapon's name; the player behind the
	// viewer is skipped; the player with WP_NONE gets no tag at all
	CHECK_INT(CountTextCalls("Grenade Launcher"), 2, "\"Grenade Launcher\" drawn twice (shadow + text)");
	CHECK_INT(CountTextCalls("Machinegun"), 2, "\"Machinegun\" drawn twice");
	CHECK_INT(CountTextCalls("Railgun"), 0, "the player behind the viewer is not drawn");
	CHECK_INT(CountTextCalls("NoWeapon"), 0, "WP_NONE shows no weapon");
	CHECK_INT(CountTextCalls("Bitterman"), 0, "the name ESP is off: no player names");

	// position: centred on the LEG anchor's projection (NOT the head anchor), in the stock
	// 1.32 table's name for the snapshot's weapon number
	const NameEsp::PlayerTag* b1 = FindTag(1);
	const NameEsp::PlayerTag* b2 = FindTag(2);
	CHECK_TRUE(b1 != NULL && b2 != NULL, "both tagged players present");
	if (b1 && b2)
	{
		NameEsp::ScreenPoint leg1, head1;
		CHECK_TRUE(ProjectLeg(*b1, leg1), "player 1's leg projects");
		const NameEsp::Viewport vp = { 0, 0, kVpW, kVpH };
		CHECK_TRUE(NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, b1->origin, head1),
		           "player 1's head projects");
		CHECK_TRUE(fabsf(leg1.y - head1.y) > 20.0f, "the leg anchor is clearly below the head anchor");

		float want1x = 0.0f, want1y = 0.0f, want2x = 0.0f, want2y = 0.0f;
		ExpectedWeaponAt(leg1, "Grenade Launcher", want1x, want1y);
		NameEsp::ScreenPoint leg2;
		CHECK_TRUE(ProjectLeg(*b2, leg2), "player 2's leg projects");
		ExpectedWeaponAt(leg2, "Machinegun", want2x, want2y);

		int checked = 0;
		const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
		for (size_t i = 0; i < texts.size(); ++i)
		{
			if (texts[i]->text != "Grenade Launcher" && texts[i]->text != "Machinegun")
				continue;
			const Rec::Call* pos = Rec::Prev(texts[i], "glRasterPos2f");
			CHECK_TRUE(pos != NULL, "the weapon text has a raster position");
			if (!pos)
				continue;
			const bool isShadow = (texts[i]->rgb == 0x000000u);
			const bool isLeft = texts[i]->text == "Grenade Launcher";
			const float wantX = isLeft ? want1x : want2x;
			const float wantY = isLeft ? want1y : want2y;
			char what[96];
			snprintf(what, sizeof(what), "\"%s\"%s raster x", texts[i]->text.c_str(), isShadow ? " shadow" : "");
			CHECK_NEAR(pos->a[0], wantX + (isShadow ? 1.0f : 0.0f), 0.02, what);
			snprintf(what, sizeof(what), "\"%s\"%s raster y (the LEG anchor)", texts[i]->text.c_str(), isShadow ? " shadow" : "");
			CHECK_NEAR(pos->a[1], wantY + (isShadow ? 1.0f : 0.0f), 0.02, what);
			if (!isShadow)
				CHECK_UINT(texts[i]->rgb, 0xFF8C00u, "saturated orange, not a team colour");
			++checked;
		}
		CHECK_INT(checked, 4, "all four issued texts were checked");
	}

	const WeaponEsp::DrawStats& st = WeaponEsp::LastDrawStats();
	CHECK_INT(st.drawn, 2, "two weapon tags drawn");
	CHECK_INT(st.inView, 2, "both ahead of the viewer");
	CHECK_INT(st.edge, 0, "none at the edge");
	CHECK_INT(st.behind, 1, "one skipped behind the viewer");
	CHECK_INT(st.faded, 0, "nothing faded out");
	CHECK_INT(st.iconsMissing, 0, "text mode uses no icons");

	// feature off: nothing issued, stats zeroed
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.weaponEsp = false;
	WeaponEsp::Draw();
	CHECK_INT(Rec::Count("glCallLists"), 0, "feature off -> no text");
	const WeaponEsp::DrawStats& z = WeaponEsp::LastDrawStats();
	CHECK_INT(z.drawn, 0, "feature off -> zero drawn");
}

static void TestWeaponIconChips()
{
	Section("WEAPON ESP icon mode - the icon quad at the leg position (chip when no texture)");

	// In this build no cgame paks exist, so EnsureIconTexture() always fails and the icon is
	// drawn as its neutral chip - what the check below asserts. On Windows the same path
	// uploads the real TGA and draws a textured quad instead (glBindTexture + glTexCoord2f).
	CHECK_TRUE(BuildArmedWorld(), "frame gathered");
	Rec::CurrentDC() = (void*)(size_t)0xD157E;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp      = false;
	Config::g_Settings.distanceEsp  = false;
	Config::g_Settings.weaponEsp    = true;
	Config::g_Settings.weaponEspStyle = 1;        // icon
	WeaponEsp::ResetDrawState();
	WeaponEsp::Draw();

	// two players ahead, each: one outline (LINE_STRIP) + one filled quad (the chip)
	CHECK_INT(Rec::Count("glBegin"), 4, "two chips: an outline and a filled quad each");
	CHECK_INT(Rec::Count("glCallLists"), 0, "icon mode prints no text");

	// the chip is centred on the leg projection, kIconBaseSizePx across at full scale, with the
	// same on-screen clamp as the rest of the overlay
	const NameEsp::PlayerTag* b1 = FindTag(1);
	CHECK_TRUE(b1 != NULL, "player 1 is in the frame");
	if (b1)
	{
		NameEsp::ScreenPoint leg;
		CHECK_TRUE(ProjectLeg(*b1, leg), "player 1's leg projects");
		const float size = (float)WeaponEsp::kIconBaseSizePx;   // scale 1 at this range
		float wantX = leg.x - size * 0.5f;
		float wantY = leg.y - size * 0.5f;
		if (wantX < 0.0f) wantX = 0.0f;
		if (wantX + size + 1.0f > (float)kVpW) wantX = (float)kVpW - size - 1.0f;
		if (wantX < 0.0f) wantX = 0.0f;
		if (wantY < 0.0f) wantY = 0.0f;
		if (wantY + size + 1.0f > (float)kVpH) wantY = (float)kVpH - size - 1.0f;
		if (wantY < 0.0f) wantY = 0.0f;

		float chipX = 0.0f, chipY = 0.0f;
		CHECK_TRUE(FirstQuadOrigin(chipX, chipY), "a chip quad was issued");
		CHECK_NEAR(chipX, wantX, 0.02, "the chip's x is the leg x minus half its size");
		CHECK_NEAR(chipY, wantY, 0.02, "the chip's y is the leg y minus half its size");

		// and it is the chip colour, not the weapon's team colour
		std::vector<Rec::Call>& calls = Rec::Calls();
		for (size_t i = 0; i < calls.size(); ++i)
		{
			if (calls[i].fn == "glBegin" && (int)calls[i].a[0] == 0x0007)   // the first GL_QUADS
			{
				CHECK_UINT(calls[i].rgb, 0x5ADCEBu, "the chip is the neutral chip colour");
				break;
			}
		}
	}

	const WeaponEsp::DrawStats& st = WeaponEsp::LastDrawStats();
	CHECK_INT(st.drawn, 2, "two icons drawn");
	CHECK_INT(st.inView, 2, "both ahead of the viewer");
	CHECK_INT(st.behind, 1, "one skipped behind the viewer");
	CHECK_INT(st.iconsMissing, 2, "both in-view icons fell back to the chip");
	// and the chip is blamed on the right thing: off Windows there is nowhere to look for the
	// .tga, so both are "not in the paks" - not, say, a decode failure or a GL refusal
	CHECK_INT(st.iconsNotInPak, 2, "... because there is no pak to read on this host");
	CHECK_INT(st.iconsBadData, 0, "... not because a file was unreadable");
	CHECK_INT(st.iconsNoUpload, 0, "... and not because GL refused a texture");

	// the WP_NONE player drew nothing, armed or not
	CHECK_INT(st.drawn, 2, "the WP_NONE player has no icon either");
}

static void TestWeaponStackingBelowHeadStack()
{
	Section("WEAPON ESP stacking - the leg tag sits below the whole head-anchored stack");

	CHECK_TRUE(BuildArmedWorld(), "frame gathered");
	Rec::CurrentDC() = (void*)(size_t)0xD157F;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp      = true;
	Config::g_Settings.distanceEsp  = true;
	Config::g_Settings.weaponEsp    = true;
	Config::g_Settings.weaponEspStyle = 0;        // text
	NameEsp::ResetDrawState();
	DistanceEsp::ResetDrawState();
	WeaponEsp::ResetDrawState();
	NameEsp::Draw();
	DistanceEsp::Draw();
	WeaponEsp::Draw();

	const NameEsp::PlayerTag* b1 = FindTag(1);
	CHECK_TRUE(b1 != NULL, "player 1 is in the frame");
	if (!b1)
		return;
	const NameEsp::Viewport vp = { 0, 0, kVpW, kVpH };
	NameEsp::ScreenPoint head;
	CHECK_TRUE(NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, b1->origin, head),
	           "head projects");

	// the head-anchored stack: the name on the anchor, the distance one row below it (the
	// lowest head-anchored thing that can be drawn)
	float nameX = 0.0f, nameY = 0.0f;
	ExpectedCentreAt(head, "Bitterman", nameX, nameY);
	const float stackBottom = nameY + NameEsp::kEspRowHeight + 14.0f;

	// the weapon tag: centred on the leg projection - a full model height below the head
	NameEsp::ScreenPoint leg;
	CHECK_TRUE(ProjectLeg(*b1, leg), "the leg projects");
	float wantX = 0.0f, wantY = 0.0f;
	ExpectedWeaponAt(leg, "Grenade Launcher", wantX, wantY);

	const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
	bool sawWeapon = false;
	for (size_t i = 0; i < texts.size(); ++i)
	{
		const Rec::Call* text = texts[i];
		if (text->text != "Grenade Launcher" || text->rgb == 0x000000u)
			continue;                       // the non-shadow weapon text
		const Rec::Call* pos = Rec::Prev(text, "glRasterPos2f");
		if (!pos)
			continue;
		sawWeapon = true;
		CHECK_NEAR(pos->a[0], wantX, 0.02, "the weapon tag is centred on the leg x");
		CHECK_NEAR(pos->a[1], wantY, 0.02, "the weapon tag is at the leg y");
		CHECK_TRUE(pos->a[1] > stackBottom,
		           "the weapon tag sits BELOW the name + distance stack, no overlap");
	}
	CHECK_TRUE(sawWeapon, "the weapon tag was drawn with all three ESPs on");
}

static void TestWeaponScaleAndFade()
{
	Section("WEAPON ESP scales down and fades out with range, like the other ESPs");

	// one bot 1500 units straight ahead: mid-fade, so neither the full-size face nor full
	// opacity is what the tag goes out in
	const float here[3]   = { 0.0f, 0.0f, 0.0f };
	const float none[3]   = { 0.0f, 0.0f, 0.0f };
	const float angles[3] = { 0.0f, 0.0f, 0.0f };

	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(1000);
	FakeEngine::SetFovString("90");
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Far\\t\\0", (const float[]){ 1500.0f, 0.0f, 0.0f });
	FakeEngine::SetPlayerWeapon(1, 6);        // WP_LIGHTNING
	CHECK_TRUE(NameEsp::Gather(1000, FakeEngine::Syscall()), "frame gathered");
	const NameEsp::PlayerTag* tag = FindTag(1);
	CHECK_TRUE(tag != NULL, "the far player is in the frame");
	if (!tag)
		return;
	{
		const float want = DistanceBetween(NameEsp::Current().view.origin, tag->lerpOrigin);
		CHECK_NEAR(tag->distance, want, 0.01, "distance is |vieworg - lerpOrigin|");
	}

	Rec::CurrentDC() = (void*)(size_t)0xD1580;
	Rec::Reset(0, 0, kVpW, kVpH);
	Config::g_Settings.nameEsp      = false;
	Config::g_Settings.distanceEsp  = false;
	Config::g_Settings.weaponEsp    = true;
	Config::g_Settings.weaponEspStyle = 0;
	WeaponEsp::ResetDrawState();

	WeaponEsp::Draw();
	unsigned int wantBase = 0;
	{
		const std::vector<const Rec::Call*> bakes = Rec::All("CreateFontA");
		CHECK_INT((int)bakes.size(), 6, "six font buckets baked for the fresh context");
		// the mid-range tag (scale 9.2px) must go out in the 10px face: the third bucket
		wantBase = ListBaseOfBake(2) - 32;      // EmitText sets the list base to base - 32
		CHECK_TRUE(wantBase != 0, "the 10px face has a list base");
	}

	// drive the fade-in ramp to completion, so the alpha left on the final frame is the
	// distance fade alone
	for (int i = 1; i <= 30; ++i)
	{
		CHECK_TRUE(NameEsp::Gather(1000 + i * 16, FakeEngine::Syscall()), "later frame gathered");
		Rec::Reset(0, 0, kVpW, kVpH);
		WeaponEsp::Draw();
	}

	const std::vector<const Rec::Call*> texts = Rec::All("glCallLists");
	bool sawText = false;
	for (size_t i = 0; i < texts.size(); ++i)
	{
		const Rec::Call* text = texts[i];
		if (text->text != "Lightning Gun" || text->rgb == 0x000000u)
			continue;
		sawText = true;
		CHECK_UINT(text->listBase, wantBase, "the mid-range tag uses the scaled-down face");
		const float t = (tag->distance - DistanceEsp::kFadeStartDist) /
		               (DistanceEsp::kFadeEndDist - DistanceEsp::kFadeStartDist);
		CHECK_NEAR(text->alpha, 1.0f - t, 0.01, "faded by the distance ramp");
	}
	CHECK_TRUE(sawText, "the far tag was drawn");

	// and a player past the fade end is dropped entirely
	FakeEngine::Reset();
	NameEsp::Reset();
	FakeEngine::SetSnapshotTime(2000);
	FakeEngine::SetFovString("90");
	FakeEngine::SetLocalPlayer(0, here, none, angles, 26);
	FakeEngine::SetPlayer(1, "\\n\\Gone\\t\\0", (const float[]){ 3000.0f, 0.0f, 0.0f });
	FakeEngine::SetPlayerWeapon(1, 6);
	CHECK_TRUE(NameEsp::Gather(2000, FakeEngine::Syscall()), "far frame gathered");
	const NameEsp::PlayerTag* gone = FindTag(1);
	CHECK_TRUE(gone != NULL, "the very far player is in the frame");
	if (gone)
		CHECK_TRUE(gone->distance > DistanceEsp::kFadeEndDist, "really past the fade end");
	Rec::Reset(0, 0, kVpW, kVpH);
	WeaponEsp::ResetDrawState();
	WeaponEsp::Draw();
	CHECK_INT(Rec::Count("glCallLists"), 0, "past the fade end: nothing issued");
	CHECK_INT(WeaponEsp::LastDrawStats().faded, 1, "counted as faded, not drawn");
}

int main(void)
{
	printf("kutaQ3 hook tests - ESP drawing (nameEsp.cpp + distanceEsp.cpp + weaponEsp.cpp + glText.cpp + glDraw.cpp)\n");

	TestFontIsBuiltOnce();
	TestOverlayState();
	TestTagsDrawn();
	TestChestAnchoredUpClose();
	TestChestAnchorHoldsItsGround();
	TestTagFadesIn();
	TestDrawStats();
	TestDrawsNothingWhenItShouldNot();
	TestFormatSpecifierName();
	TestDistanceTagsDrawn();
	TestDistanceStacksUnderName();
	TestDistanceScaleAndFade();
	TestWeaponTextAtLeg();
	TestWeaponIconChips();
	TestWeaponStackingBelowHeadStack();
	TestWeaponScaleAndFade();

	CHECK_SUMMARY("gl");
	return g_failed ? 1 : 0;
}
