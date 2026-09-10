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

	// What the last Draw() did with its frame (see DrawStats in nameEsp.h). Reset at the top of
	// every Draw() so a frame that draws nothing reports zeros.
	NameEsp::DrawStats s_stats;

	// Off-screen tags are clamped to the viewport edge; dimming them says "this one is not where
	// the tag is" without needing an arrow.
	void Dim(unsigned char rgb[3])
	{
		rgb[0] = (unsigned char)(rgb[0] * 55 / 100);
		rgb[1] = (unsigned char)(rgb[1] * 55 / 100);
		rgb[2] = (unsigned char)(rgb[2] * 55 / 100);
	}
}

const NameEsp::DrawStats& NameEsp::LastDrawStats()
{
	return s_stats;
}

void NameEsp::Draw()
{
	s_stats.drawn = s_stats.inView = s_stats.edge = s_stats.behind = 0;
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
			const bool headOk = ProjectWorldToScreen(frame.view, vp, tag.origin, p);
			if (!headOk || !p.inView)
			{
				// The anchor sits 36 units above the player's feet, and up close plus aiming
				// up/down that point leaves the screen while the player is still plainly
				// visible. Rather than clamping a visible player's name to the edge (dimmed,
				// easily missed), re-anchor to the chest when the chest is on screen.
				float chest[3] = { tag.origin[0], tag.origin[1],
				                   tag.origin[2] - q3::kPlayerTagHeight + q3::kChestHeight };
				ScreenPoint pc;
				if (ProjectWorldToScreen(frame.view, vp, chest, pc) && pc.inView)
					p = pc;                            // on the visible body, full brightness
				else if (!headOk)
				{
					++s_stats.behind;
					continue;                          // behind the viewer
				}
				// else: ahead of the viewer but off screen; keep the edge-clamped point
				// (dimmed below).
			}

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
			++s_stats.drawn;
			if (p.inView)
				++s_stats.inView;
			else
				++s_stats.edge;
		}

		GL::RestoreGL();
	}
}
