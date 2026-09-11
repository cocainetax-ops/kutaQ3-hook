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

	// ---- per-client draw state -----------------------------------------------------------------
	// The tags themselves are rebuilt from scratch every frame; what has to survive across frames
	// is how far a tag has faded in and which anchor it is currently using. Keyed by client
	// number, fixed size (the ESP can never carry more than kMaxClients tags), no allocation.
	const int kFadeInMs         = 220;   // a tag ramps 0 -> full over this
	const int kHeadFramesToBack = 3;     // head anchor must hold this long before the tag returns
	const int kMaxClockStepMs   = 250;   // a hitch is not a slow-motion ramp
	const int kMinClockStepMs   = 16;    // ... and a stalled clock must not stall the ramp

	struct TagState
	{
		int   clientNum;      // -1: slot free
		float alpha;          // 0..1 fade-in
		int   headHeld;       // frames in a row the above-the-head anchor has been on screen
		bool  onChest;        // currently anchored to the chest instead
		int   steppedAt;      // the Draw() that last advanced this slot (s_frameSerial)
	};

	TagState s_tags[q3::kMaxClients];
	int      s_clock       = 0;
	bool     s_haveClock   = false;
	int      s_frameSerial = 0;   // one Draw() == one step, however many tags share a client number

	void ResetTagState()
	{
		for (int i = 0; i < q3::kMaxClients; ++i)
		{
			s_tags[i].clientNum = -1;
			s_tags[i].alpha     = 0.0f;
			s_tags[i].headHeld  = 0;
			s_tags[i].onChest   = false;
			s_tags[i].steppedAt = 0;
		}
		s_clock     = 0;
		s_haveClock = false;
	}

	// The slot for a client, reusing a free one when it has not been seen since the last reset.
	// A client that drops out of the PVS and comes back keeps its slot, so a tag that blinks does
	// not restart its fade - which is most of what the fade is for.
	TagState& TagSlot(int clientNum)
	{
		int freeSlot = -1;
		for (int i = 0; i < q3::kMaxClients; ++i)
		{
			if (s_tags[i].clientNum == clientNum)
				return s_tags[i];
			if (s_tags[i].clientNum < 0 && freeSlot < 0)
				freeSlot = i;
		}
		// Every slot belongs to another client: with kMaxClients clients and kMaxClients slots
		// that can only happen if the client list changed wholesale, so take slot 0 over the
		// oldest entry (the tags are drawn in one pass, so a shared slot costs one fade).
		if (freeSlot < 0)
			freeSlot = 0;
		s_tags[freeSlot].clientNum = clientNum;
		s_tags[freeSlot].alpha     = 0.0f;
		s_tags[freeSlot].headHeld  = 0;
		s_tags[freeSlot].onChest   = false;
		s_tags[freeSlot].steppedAt = 0;
		return s_tags[freeSlot];
	}

	// Milliseconds since the previous Draw(), from the frame's own server time - the same clock
	// the tags are positioned with. Clamped both ways: a long hitch must not ramp a tag in over
	// the whole pause, and a clock that has stopped (demo pause, a frozen refdef) must not leave
	// every tag invisible forever. A clock that runs BACKWARDS is a new level - the tags are all
	// new players in a new map, so every fade restarts with it.
	int ClockStepMs(int serverTime)
	{
		if (s_haveClock && serverTime < s_clock)
		{
			ResetTagState();
			s_clock     = serverTime;
			s_haveClock = true;
			return kMinClockStepMs;
		}
		int step = s_haveClock ? (serverTime - s_clock) : 0;
		s_clock     = serverTime;
		s_haveClock = true;
		if (step < kMinClockStepMs) step = kMinClockStepMs;
		if (step > kMaxClockStepMs) step = kMaxClockStepMs;
		return step;
	}

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

void NameEsp::ResetDrawState()
{
	ResetTagState();
}

void NameEsp::Draw()
{
	s_stats.drawn = s_stats.inView = s_stats.edge = s_stats.behind = 0;
	if (!Config::g_Settings.nameEsp)
	{
		// off: forget every fade, so turning the feature back on ramps the tags in again instead
		// of popping them in at full brightness
		ResetTagState();
		return;
	}

	const Frame& frame = Current();
	// Note this deliberately does NOT clear the fade state when there is nobody to draw: the PVS
	// drops every player for a frame or two at times, and restarting the fades there would blink
	// the whole set. A player who is genuinely gone keeps a slot that costs nothing.
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

	// One clock step for the whole frame: every tag fades in over the same interval.
	const int stepMs = ClockStepMs(frame.serverTime);
	++s_frameSerial;

	{
		// The overlay changes plenty of legacy state Quake 3 caches in its own glState shadow;
		// the guard puts all of it back on the way out (same wrapper the menu renders in).
		KUTAQ3_LEGACY_GL_STATE_GUARD();
		GL::SetupOrtho();

		// Blending is what makes the fade a fade: SetupOrtho() turns it off, the display-list font
		// has no alpha of its own, and glBitmap fragments take the current raster colour - so
		// SRC_ALPHA over the scene is the only thing that can soften a tag in. At alpha 1 the
		// blend is a no-op, so a fully faded tag is pixel-identical to the old opaque draw.
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		for (int i = 0; i < frame.playerCount; ++i)
		{
			const PlayerTag& tag = frame.players[i];

			ScreenPoint p;
			const bool headOk = ProjectWorldToScreen(frame.view, vp, tag.origin, p);
			// ProjectWorldToScreen returns coordinates in desktop/window space so callers can
			// reason about letterboxed viewports. SetupOrtho(), however, deliberately resets the
			// GL viewport to (0, 0, width, height). Convert the result to that overlay's local
			// coordinates before drawing. Without this, a non-zero viewport origin (common with
			// letterbox/split-screen views) makes every raster position land against the left/top
			// edge even though the world projection itself is correct.
			if (headOk)
			{
				p.x -= (float)vp.x;
				p.y += (float)vp.y;
			}
			const bool headOnScreen = headOk && p.inView;

			// The fallback anchor, mid-torso, projected every frame: the hysteresis below needs to
			// know whether the chest is usable even while the head anchor is the one being drawn.
			float chest[3] = { tag.origin[0], tag.origin[1],
			                   tag.origin[2] - q3::kPlayerTagHeight + q3::kChestHeight };
			ScreenPoint pc;
			const bool chestOk = ProjectWorldToScreen(frame.view, vp, chest, pc);
			if (chestOk)
			{
				pc.x -= (float)vp.x;
				pc.y += (float)vp.y;
			}
			const bool chestOnScreen = chestOk && pc.inView;

			TagState& st = TagSlot(tag.clientNum);
			// One step per Draw(), not per tag: a frame is one unit of time for the ramp and the
			// anchor hold however many tags in it name the same client.
			const bool fresh = (st.steppedAt != s_frameSerial);
			if (fresh)
				st.steppedAt = s_frameSerial;

			if (headOnScreen)
			{
				if (st.onChest)
				{
					// The anchor sits 36 units above the player's feet, so up close plus aiming
					// up/down it leaves the screen while the player is still plainly visible -
					// which is when the tag re-anchors to the chest. Moving back is held for a few
					// frames: without that, aiming around the boundary swaps the anchor every
					// other frame and the name hops between two points that are a head apart.
					if (fresh)
						++st.headHeld;
					if (st.headHeld >= kHeadFramesToBack || !chestOnScreen)
					{
						st.onChest  = false;
						st.headHeld = 0;
					}
					else
					{
						p = pc;
					}
				}
			}
			else
			{
				if (fresh)
					st.headHeld = 0;
				if (!headOk && !chestOnScreen)
				{
					++s_stats.behind;
					continue;                          // behind the viewer
				}
				if (chestOnScreen)
				{
					st.onChest = true;
					p = pc;                            // on the visible body, full brightness
				}
				// else: ahead of the viewer but off screen; keep the edge-clamped point
				// (dimmed below).
			}

			// Fade in. A tag that just entered the PVS (or the level) ramps up over kFadeInMs
			// instead of popping into existence; a client that drops out and back inside a few
			// frames keeps its alpha, so the PVS flicker stops being a flicker.
			if (fresh)
			{
				st.alpha += (float)stepMs / (float)kFadeInMs;
				if (st.alpha > 1.0f)
					st.alpha = 1.0f;
			}
			const float alpha = st.alpha;

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
			s_font.PrintAlpha(x + 1.0f, y + 1.0f, black, alpha, "%s", tag.name);
			s_font.PrintAlpha(x, y, rgb, alpha, "%s", tag.name);
			++s_stats.drawn;
			if (p.inView)
				++s_stats.inView;
			else
				++s_stats.edge;
		}

		GL::RestoreGL();
	}
}
