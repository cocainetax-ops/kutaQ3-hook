#pragma once

// =============================================================================================== //
// kutaQ3 hook - DISTANCE ESP
//
// The distance to every other player, drawn in metres with an "M" after the number ("128M")
// above their head, in the same SwapBuffers overlay as NAME ESP. It uses the GL::Font
// display-list renderer from glText.h, the same face and the same full size (FONT_HEIGHT 14)
// as NAME ESP, centred on the same head anchor.
//
// Stacking
// --------
// The three ESP overlays each take one 16px row above the head anchor - the name, then the
// distance, then the health bar - from NameEsp::ComputeEspRows(), so no combination of the
// three features overlaps on screen: with all three on, the HEALTH ESP bar is on the last
// row, the distance sits right under the name, and name + distance alone stack with nothing
// else in between.
//
// Data
// ----
// The value is tag.distance: |cg.refdef.vieworg - cent->lerpOrigin| in world units - the
// local player's view origin from the refdef NameEsp::Gather() projects with, and the
// target's interpolated origin, exactly like the cgame lerps the model. NameEsp::Gather()
// already computes it per tag (the HEALTH ESP fade is driven by the same value), so this
// feature adds no extra read of the cgame. World units are displayed as metres - the
// standard Q3 ESP convention - rounded to the nearest whole number so the string stays as
// short as a distance can be.
//
// Distance-based scale & alpha fading
// -----------------------------------
// Instead of drawing everyone's distance at full size and opacity, the text scales down
// (FONT_HEIGHT 14 -> 5px) and fades to transparent as the player moves from kFadeStartDist
// to kFadeEndDist away - the same ramp the HEALTH ESP bars use, so the two overlays agree
// about what "far" looks like.
//
// The GL half lives in distanceEsp.cpp; the layout maths below is header-only so the tests
// can exercise the row stacking / fade / text without a GL context.
// =============================================================================================== //

#include "nameEsp.h"
#include "q3sdk.h"

#include <math.h>
#include <stdio.h>

namespace DistanceEsp
{
	// Full size and opacity inside this many world units; fade toward kMinScale / alpha 0 by
	// kFadeEndDist. Same ramp as HealthEsp::DistanceFade, so both overlays fade together.
	const float kFadeStartDist = 400.0f;
	const float kFadeEndDist   = 2500.0f;
	const float kMinScale      = 0.35f;

	// scale 1..kMinScale, alpha 1..0 as distance goes kFadeStartDist..kFadeEndDist.
	inline void DistanceFade(float distance, float& scale, float& alpha)
	{
		if (distance <= kFadeStartDist)
		{
			scale = 1.0f;
			alpha = 1.0f;
			return;
		}
		if (distance >= kFadeEndDist)
		{
			scale = kMinScale;
			alpha = 0.0f;
			return;
		}
		const float t = (distance - kFadeStartDist) / (kFadeEndDist - kFadeStartDist);
		scale = 1.0f - t * (1.0f - kMinScale);
		alpha = 1.0f - t;
	}

	// "128M": the distance rounded to the nearest metre with an "M" after it - digits and M
	// only, so the GL::Font display lists (glyphs 32..127) always hold it. Returns false for
	// a NaN or negative distance, which there is nothing to display. out needs 8 bytes
	// ("99999M" + NUL is the worst case).
	inline bool FormatDistance(float distance, char out[8])
	{
		if (!(distance >= 0.0f))                 // NaN fails the compare, negative is bogus
			return false;
		if (distance > 99999.0f)                 // a map is not that big; keeps the buffer honest
			distance = 99999.0f;
		const int metres = (int)(distance + 0.5f);
		snprintf(out, 8, "%dM", metres);
		return true;
	}

	// What Draw() did with the frame it was given: how many tags were drawn at their unclamped
	// position, how many were clamped to the edge, how many were skipped as behind the viewer,
	// and how many the distance fade took to nothing. Read by the menu.
	struct DrawStats
	{
		int drawn;    // tags issued to GL::Font (shadow + text each)
		int inView;   // ... drawn unclamped, full brightness
		int edge;     // ... clamped to the viewport edge, dimmed
		int behind;   // skipped as behind the viewer
		int faded;    // skipped, faded to nothing by the distance fade
	};

	// The stats for the last Draw() call - zeros when that call drew nothing.
	const DrawStats& LastDrawStats();

	// Drop the GL half's per-client state (the fade-in ramp and which anchor each tag is
	// using). Mirrors NameEsp::ResetDrawState(); called from the menu side, never from the
	// portable half.
	void ResetDrawState();

	// Draw() - the GL half, in distanceEsp.cpp. Called from the hooked SwapBuffers every
	// frame, between NameEsp::Draw() and HealthEsp::Draw().
	void Draw();
}
