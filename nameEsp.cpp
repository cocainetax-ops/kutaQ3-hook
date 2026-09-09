// =============================================================================================== //
// kutaQ3 hook - NAME ESP, GL half (see nameEsp.h for the overview)
//
// Renders the tags the portable half (nameEspCore.cpp) gathered from the cgame VM, using
// the GL::Font display-list text renderer from glText.h. Runs from the hooked SwapBuffers in
// main.cpp: that is the one point in the frame where the GL context is current and the game scene
// is already drawn, so the text lands on top of it.
// =============================================================================================== //

#include "nameEsp.h"
#include "config.h"        // Config::g_Settings.nameEsp
#include "glText.h"        // GL::Font
#include "glDraw.h"        // GL::SetupOrtho / GL::RestoreGL
#include "glStateGuard.h"  // KUTAQ3_LEGACY_GL_STATE_GUARD

#include <gl/GL.h>

namespace
{
	// Display lists are owned by the GL context they were created on, and Quake 3 throws the
	// context away on vid_restart - so the font is rebuilt whenever the DC it was built against is
	// no longer the current one.
	GL::Font s_font;

	// Off-screen tags are clamped to the viewport edge; dimming them says "this one is not where
	// the tag is" without needing an arrow.
	void Dim(unsigned char rgb[3])
	{
		rgb[0] = (unsigned char)(rgb[0] * 55 / 100);
		rgb[1] = (unsigned char)(rgb[1] * 55 / 100);
		rgb[2] = (unsigned char)(rgb[2] * 55 / 100);
	}
}

void NameEsp::Draw()
{
	if (!Config::g_Settings.nameEsp)
		return;

	const Frame& frame = Current();
	if (!frame.valid || frame.playerCount <= 0)
		return;

	// The game's own viewport: where the 3D scene actually was, which is what the projection maps
	// out of (a letterboxed r_mode puts it somewhere other than 0,0).
	GLint glViewportRect[4] = { 0, 0, 0, 0 };
	glGetIntegerv(GL_VIEWPORT, glViewportRect);

	const Viewport vp = { glViewportRect[0], glViewportRect[1], glViewportRect[2], glViewportRect[3] };
	if (vp.width <= 0 || vp.height <= 0)
		return;

	// Build (or rebuild after a context change) before touching the overlay state.
	HDC hdc = wglGetCurrentDC();
	if (!hdc)
		return;
	if (!s_font.bBuilt || s_font.hdc != hdc)
		s_font.Build(FONT_HEIGHT);
	if (!s_font.bBuilt)
		return;

	{
		// The overlay changes plenty of legacy state Quake 3 caches in its own glState shadow;
		// the guard puts all of it back on the way out (same wrapper the menu renders in).
		KUTAQ3_LEGACY_GL_STATE_GUARD();
		GL::SetupOrtho();

		for (int i = 0; i < frame.playerCount; ++i)
		{
			const PlayerTag& tag = frame.players[i];

			ScreenPoint p;
			if (!ProjectWorldToScreen(frame.view, vp, tag.origin, p))
				continue;                              // behind the viewer

			unsigned char rgb[3];
			TeamColor(tag.team, rgb);
			if (!p.inView)
				Dim(rgb);

			// centre the name on the projected point: with width 0 the helper returns
			// x - textWidth / 2, which is what "centred on x" means here
			const float textWidth = s_font.TextWidth(tag.name);
			float x = s_font.centerText(p.x, 0.0f, textWidth);
			float y = p.y;

			// glRasterPos outside the ortho rect invalidates the raster position and the
			// display-list font then draws NOTHING - it does not clip. The projection
			// already clamped the anchor point p to the viewport, but centring can still
			// push the string's left edge off-screen (a tag pinned to the left edge would
			// start at x = -textWidth / 2 and never draw). Keep the whole string - shadow
			// included - inside the overlay so edge tags stay visible instead of vanishing.
			// The overlay's ortho is (0..vp.width, 0..vp.height); see GL::SetupOrtho().
			const float overlayW = (float)vp.width;
			const float overlayH = (float)vp.height;
			if (x < 0.0f)
				x = 0.0f;
			if (x + textWidth + 1.0f > overlayW)
				x = overlayW - textWidth - 1.0f;
			if (x < 0.0f)                              // text wider than the viewport
				x = 0.0f;
			if (y < 0.0f)
				y = 0.0f;
			if (y + (float)FONT_HEIGHT + 1.0f > overlayH)
				y = overlayH - (float)FONT_HEIGHT - 1.0f;
			if (y < 0.0f)
				y = 0.0f;

			// No blending in the overlay (GL::SetupOrtho disables it and the display-list font has
			// no alpha of its own), so readability comes from a 1px black drop shadow instead of a
			// translucent backing plate. Note "%s": a player name may contain '%'.
			static const unsigned char black[3] = { 0, 0, 0 };
			s_font.Print(x + 1.0f, y + 1.0f, black, "%s", tag.name);
			s_font.Print(x, y, rgb, "%s", tag.name);
		}

		GL::RestoreGL();
	}
}
