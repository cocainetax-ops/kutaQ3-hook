// =============================================================================================== //
// kutaQ3 hook - DISTANCE ESP, GL half (see distanceEsp.h for the overview)
//
// Renders the distance tags the portable half (nameEspCore.cpp) gathered, with the same
// GL::Font display-list renderer NAME ESP uses. Runs from the hooked SwapBuffers in
// main.cpp, after NameEsp::Draw() and before WeaponEsp::Draw(); the row offset comes from
// NameEsp::ComputeEspRows(), so the head-anchored ESP overlays stack without overlapping.
//
// Scaling: a GL::Font bakes ONE fixed face into its display list, and glBitmap glyphs render
// in window pixels - they are not affected by the modelview transform, so glScalef would move
// the tag but never resize its letters. A "scaled" tag therefore means picking the pre-baked
// face nearest FONT_HEIGHT * scale; the buckets below span the fade's kMinScale..1, and 5px
// == 14 * 0.357 ~= kMinScale, so the smallest face is exactly what a fully faded tag sits at.
// =============================================================================================== //

#include "distanceEsp.h"
#include "config.h"        // Config::g_Settings.distanceEsp
#include "glText.h"        // GL::Font, FONT_HEIGHT
#include "glDraw.h"        // GL::SetupOrtho / GL::RestoreGL
#include "glStateGuard.h"  // KUTAQ3_LEGACY_GL_STATE_GUARD

#include <gl/GL.h>

namespace
{
	// The face a tag at `scale` is drawn in: the bucket whose height is nearest
	// FONT_HEIGHT * scale. FONT_HEIGHT is 14 (glText.h), the fade never scales below
	// kMinScale, so 5px is the smallest face a tag can ever need.
	const float kFontBuckets[] = { 14.0f, 12.0f, 10.0f, 8.0f, 6.0f, 5.0f };
	const int   kBucketCount   = 6;

	// Display lists are owned by the GL context they were created on, and Quake 3 throws the
	// context away on vid_restart - so every bucket is rebuilt whenever the DC it was built
	// against is no longer the current one (same rule as the name ESP's font).
	GL::Font s_fonts[kBucketCount];

	// What the last Draw() did with its frame (see DrawStats in distanceEsp.h).
	DistanceEsp::DrawStats s_stats;

	// ---- per-client draw state ----------------------------------------------------------------
	// The tags themselves are rebuilt from scratch every frame; what has to survive across
	// frames is how far a tag has faded in and which anchor it is currently using. Same shape
	// and intent as nameEsp.cpp / weaponEsp.cpp: keyed by client number, fixed size, no
	// allocation.
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
	// the tags are positioned with. Clamped both ways, and a clock that runs backwards is a new
	// level, so every fade restarts with it (same rules as nameEsp.cpp).
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

	int BucketForScale(float scale)
	{
		const float target = (float)FONT_HEIGHT * scale;
		int best = 0;
		for (int i = 1; i < kBucketCount; ++i)
			if (fabsf(kFontBuckets[i] - target) < fabsf(kFontBuckets[best] - target))
				best = i;
		return best;
	}

	// Off-screen tags are clamped to the viewport edge; dimming them says "this one is not
	// where the tag is" without needing an arrow (same 55% dim as the name ESP).
	void Dim(unsigned char rgb[3])
	{
		rgb[0] = (unsigned char)(rgb[0] * 55 / 100);
		rgb[1] = (unsigned char)(rgb[1] * 55 / 100);
		rgb[2] = (unsigned char)(rgb[2] * 55 / 100);
	}
}

const DistanceEsp::DrawStats& DistanceEsp::LastDrawStats()
{
	return s_stats;
}

void DistanceEsp::ResetDrawState()
{
	ResetTagState();
}

void DistanceEsp::Draw()
{
	s_stats.drawn = s_stats.inView = s_stats.edge = s_stats.behind = s_stats.faded = 0;
	if (!Config::g_Settings.distanceEsp)
	{
		// off: forget every fade, so turning the feature back on ramps the tags in again
		// instead of popping them in at full brightness
		ResetTagState();
		return;
	}

	const NameEsp::Frame& frame = NameEsp::Current();
	if (!frame.valid || frame.playerCount <= 0)
		return;

	// The game's own viewport: where the 3D scene actually was, which is what the projection
	// maps out of (a letterboxed r_mode puts it somewhere other than 0,0).
	GLint glViewportRect[4] = { 0, 0, 0, 0 };
	glGetIntegerv(GL_VIEWPORT, glViewportRect);

	const NameEsp::Viewport vp = { glViewportRect[0], glViewportRect[1],
	                               glViewportRect[2], glViewportRect[3] };
	if (vp.width <= 0 || vp.height <= 0)
		return;

	// Build (or rebuild after a context change) before touching the overlay state.
	HDC hdc = wglGetCurrentDC();
	if (!hdc)
		return;
	for (int i = 0; i < kBucketCount; ++i)
	{
		if (!s_fonts[i].bBuilt || s_fonts[i].hdc != hdc)
			s_fonts[i].Build((int)kFontBuckets[i]);
	}
	if (!s_fonts[0].bBuilt)
		return;

	// One clock step for the whole frame: every tag fades in over the same interval.
	const int stepMs = ClockStepMs(frame.serverTime);
	++s_frameSerial;

	// The row the distance text sits in: under the name when NAME ESP is on, on the head
	// anchor itself when it is not.
	const NameEsp::EspRows rows =
		NameEsp::ComputeEspRows(Config::g_Settings.nameEsp);

	{
		// The overlay changes plenty of legacy state Quake 3 caches in its own glState shadow;
		// the guard puts all of it back on the way out (same wrapper the menu renders in).
		KUTAQ3_LEGACY_GL_STATE_GUARD();
		GL::SetupOrtho();

		// Blending is what makes the fade a fade: SetupOrtho() turns it off, the display-list
		// font has no alpha of its own, and glBitmap fragments take the current raster colour
		// - so SRC_ALPHA over the scene is the only thing that can soften a tag in.
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		// Neutral white: readable on any background, and distinct from the name ESP's team
		// colours.
		static const unsigned char kColor[3]  = { 255, 255, 255 };
		static const unsigned char black[3]   = { 0, 0, 0 };

		for (int i = 0; i < frame.playerCount; ++i)
		{
			const NameEsp::PlayerTag& tag = frame.players[i];

			// Distance-based scale & alpha fading instead of full size and opacity:
			// |cg.refdef.vieworg - cent->lerpOrigin| drives both (see distanceEsp.h).
			float scale = 1.0f, fade = 1.0f;
			DistanceEsp::DistanceFade(tag.distance, scale, fade);
			if (fade < 0.04f)
			{
				++s_stats.faded;
				continue;                            // faded to nothing: nothing to draw
			}
			const int bucket = BucketForScale(scale);

			// The same anchor handling as NAME ESP: above the head, re-anchored to the chest
			// up close where aiming up/down would push the head anchor off screen. The two
			// features share the row stack, so they must follow the same anchor.
			NameEsp::ScreenPoint p;
			const bool headOk = NameEsp::ProjectWorldToScreen(frame.view, vp, tag.origin, p);
			// ProjectWorldToScreen returns coordinates in desktop/window space; SetupOrtho()
			// resets the GL viewport to (0, 0, width, height), so convert to the overlay's
			// local coordinates before drawing (same conversion as nameEsp.cpp).
			if (headOk)
			{
				p.x -= (float)vp.x;
				p.y += (float)vp.y;
			}
			const bool headOnScreen = headOk && p.inView;

			float chest[3] = { tag.origin[0], tag.origin[1],
			                  tag.origin[2] - q3::kPlayerTagHeight + q3::kChestHeight };
			NameEsp::ScreenPoint pc;
			const bool chestOk = NameEsp::ProjectWorldToScreen(frame.view, vp, chest, pc);
			if (chestOk)
			{
				pc.x -= (float)vp.x;
				pc.y += (float)vp.y;
			}
			const bool chestOnScreen = chestOk && pc.inView;

			TagState& st = TagSlot(tag.clientNum);
			const bool fresh = (st.steppedAt != s_frameSerial);
			if (fresh)
				st.steppedAt = s_frameSerial;

			if (headOnScreen)
			{
				if (st.onChest)
				{
					// Moving back to the head is held for a few frames: without that, aiming
					// around the boundary swaps the anchor every other frame and the tag hops
					// between two points that are a head apart.
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
					continue;                        // behind the viewer
				}
				if (chestOnScreen)
				{
					st.onChest = true;
					p = pc;                          // on the visible body, full brightness
				}
				// else: ahead of the viewer but off screen; keep the edge-clamped point
				// (dimmed below).
			}

			// Fade in (a tag that just entered the PVS ramps up over kFadeInMs) times the
			// distance fade.
			if (fresh)
			{
				st.alpha += (float)stepMs / (float)kFadeInMs;
				if (st.alpha > 1.0f)
					st.alpha = 1.0f;
			}
			const float alpha = st.alpha * fade;
			if (alpha < 0.04f)
			{
				++s_stats.faded;
				continue;
			}

			char text[8];
			if (!DistanceEsp::FormatDistance(tag.distance, text))
			{
				++s_stats.faded;
				continue;
			}

			unsigned char rgb[3] = { kColor[0], kColor[1], kColor[2] };
			if (!p.inView)
				Dim(rgb);

			// Centre the tag on the projected point, in its row below the head anchor. With
			// width 0 the helper returns x - textWidth / 2, which is what "centred on x"
			// means here; TextWidth is measured with the very face this tag is drawn in.
			GL::Font& font = s_fonts[bucket];
			const float textW   = font.TextWidth(text);
			const float fontH   = kFontBuckets[bucket];
			float x = font.centerText(p.x, 0.0f, textW);
			float y = p.y + rows.distance;

			// glRasterPos outside the ortho rect invalidates the raster position and the
			// display-list font then draws NOTHING - it does not clip. The projection already
			// clamped the anchor point to the viewport, but centring can still push the
			// string's left edge off-screen. Keep the whole string - shadow included - inside
			// the overlay so edge tags stay visible instead of vanishing (same rule as
			// NameEsp::Draw). The overlay's ortho is (0..vp.width, 0..vp.height).
			const float overlayW = (float)vp.width;
			const float overlayH = (float)vp.height;
			if (x < 0.0f)
				x = 0.0f;
			if (x + textW + 1.0f > overlayW)
				x = overlayW - textW - 1.0f;
			if (x < 0.0f)                            // text wider than the viewport
				x = 0.0f;
			if (y < 0.0f)
				y = 0.0f;
			if (y + fontH + 1.0f > overlayH)
				y = overlayH - fontH - 1.0f;
			if (y < 0.0f)
				y = 0.0f;

			// Readability comes from a 1px black drop shadow (no backing plate), same as the
			// name ESP. Note "%s": a fixed string here, but the habit costs nothing.
			font.PrintAlpha(x + 1.0f, y + 1.0f, black, alpha, "%s", text);
			font.PrintAlpha(x, y, rgb, alpha, "%s", text);
			++s_stats.drawn;
			if (p.inView)
				++s_stats.inView;
			else
				++s_stats.edge;
		}

		GL::RestoreGL();
	}
}
