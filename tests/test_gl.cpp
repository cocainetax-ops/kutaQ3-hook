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
// the drawing around them (font build, ortho setup, centring, drop shadow, team colour, and doing
// nothing at all when there is nothing to draw).
//
//     make -C tests check
// =============================================================================================== //

#include "nameEsp.h"
#include "fake_engine.h"
#include "config.h"
#include "check.h"
#include "glrec.h"

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

	// where the overlay should have put a centred tag for this player - the same centring plus
	// the same on-screen clamp Draw() applies so glRasterPos stays valid (see nameEsp.cpp)
	bool ExpectedCentre(const NameEsp::PlayerTag& tag, float& x, float& y)
	{
		const NameEsp::Viewport vp = { 0, 0, kVpW, kVpH };
		NameEsp::ScreenPoint p;
		if (!NameEsp::ProjectWorldToScreen(NameEsp::Current().view, vp, tag.origin, p))
			return false;
		const float textWidth = kCharWidth * (float)strlen(tag.name);
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

int main(void)
{
	printf("kutaQ3 hook tests - NAME ESP drawing (nameEsp.cpp + glText.cpp + glDraw.cpp)\n");

	TestFontIsBuiltOnce();
	TestOverlayState();
	TestTagsDrawn();
	TestDrawsNothingWhenItShouldNot();
	TestFormatSpecifierName();

	CHECK_SUMMARY("gl");
	return g_failed ? 1 : 0;
}
