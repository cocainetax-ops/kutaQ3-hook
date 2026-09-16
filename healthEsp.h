#pragma once

// =============================================================================================== //
// kutaQ3 hook - HEALTH ESP
//
// Dynamic 2D health bars floating above every other player, drawn in the same SwapBuffers overlay
// as NAME ESP. Traditional fill: remaining HP as a green-to-red bar. Toggled independently from
// the VISUALS tab; when both are on the bar sits underneath the name, thinner and no wider than
// the projected player model.
//
// Data
// ----
// Positions, the view and the player list come from NameEsp::Gather() (same snapshot / refdef
// the names use). Other players' STAT_HEALTH is not in a stock 1.32 snapshot - only EV_PAIN's
// eventParm carries remaining HP - so the bar is a damage-derived ESTIMATE, not a live health
// readout. It has two states, both visible at a glance:
//
//   unconfirmed (no EV_PAIN since spawn/respawn): the value is the assumed spawn level
//     (NameEsp::SpawnHealthAssumption(), 100 on stock servers), drawn as neutral hatching so a
//     "never hit" player is never mistaken for a "measured at 100" one;
//
//   confirmed (at least one EV_PAIN sampled): solid green-to-red fill at the last sample's
//     remaining HP. Nothing between hits is modelled - health packs, health regeneration
//     packs and armor do not move the bar; the next hit re-measures it (eventParm is an
//     absolute sample, so a healed player jumps back up on their next pain).
//
// Distance is |cg.refdef.vieworg - cent->lerpOrigin|; bars scale down and fade out with it
// instead of drawing everyone at full size and opacity.
//
// The GL half lives in healthEsp.cpp; the layout maths below is header-only so the tests can
// exercise stacking / fade / colour without a GL context.
// =============================================================================================== //

#include "nameEsp.h"
#include "q3sdk.h"

#include <math.h>

namespace HealthEsp
{
	// Full size and opacity inside this many world units; fade toward kMinScale / alpha 0 by
	// kFadeEndDist. Matches a typical Q3 corridor-to-open-yard range.
	const float kFadeStartDist = 400.0f;
	const float kFadeEndDist   = 2500.0f;
	const float kMinScale      = 0.35f;

	// FONT_HEIGHT (14) + 2px gap: when NAME ESP is also on, the bar is this many pixels below
	// the name's top-left so the two never overlap.
	const float kNameStackOffset = 16.0f;

	const float kBarHeightSolo    = 4.5f;
	const float kBarHeightStacked = 3.0f;
	const float kWidthFracSolo    = 0.88f;   // of the projected player bbox
	const float kWidthFracStacked = 0.70f;   // thinner / shorter so it stays under the name

	struct DrawStats
	{
		int drawn;
		int inView;
		int skipped;   // behind, off-screen, or faded to nothing
	};

	const DrawStats& LastDrawStats();

	void ResetDrawState();
	void Draw();

	inline float DistanceTo(const float a[3], const float b[3])
	{
		const float dx = a[0] - b[0];
		const float dy = a[1] - b[1];
		const float dz = a[2] - b[2];
		return sqrtf(dx * dx + dy * dy + dz * dz);
	}

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

	// Traditional green (full) -> red (empty). ratio is 0..1.
	inline void HealthColor(float ratio, unsigned char rgb[3])
	{
		if (ratio < 0.0f) ratio = 0.0f;
		if (ratio > 1.0f) ratio = 1.0f;
		rgb[0] = (unsigned char)((1.0f - ratio) * 255.0f + 0.5f);
		rgb[1] = (unsigned char)(ratio * 255.0f + 0.5f);
		rgb[2] = 0;
	}

	inline float HealthRatio(int health)
	{
		if (health <= 0)
			return 0.0f;
		if (health >= q3::kDefaultMaxHealth)
			return 1.0f;
		return (float)health / (float)q3::kDefaultMaxHealth;
	}

	struct BarGeom
	{
		float x, y, w, h;
		float fillW;
		float alpha;
		unsigned char fill[3];
		bool  visible;
		bool  confirmed;    // false = value is the assumed spawn level: Draw() renders the
		                    // fill as neutral hatching instead of the solid green-to-red
	};

	// Overlay-space bar centred on overlayP (already converted from ProjectWorldToScreen the
	// way NameEsp::Draw does: x -= vp.x, y += vp.y). Width is capped to the projected player
	// bbox so the bar never exceeds the model; when nameEspOn the bar is thinner and sits
	// kNameStackOffset pixels below the name.
	inline bool ComputeBar(const NameEsp::PlayerTag& tag, const NameEsp::View& view,
	                       const NameEsp::Viewport& vp, const NameEsp::ScreenPoint& overlayP,
	                       bool nameEspOn, BarGeom& out)
	{
		out.x = out.y = out.w = out.h = out.fillW = 0.0f;
		out.alpha = 0.0f;
		out.fill[0] = out.fill[1] = out.fill[2] = 0;
		out.visible = false;
		out.confirmed = false;

		if (!overlayP.inView)
			return false;

		float scale = 1.0f, fade = 1.0f;
		DistanceFade(tag.distance, scale, fade);
		if (fade < 0.04f)
			return false;

		const float ratio = HealthRatio(tag.health);
		HealthColor(ratio, out.fill);
		out.confirmed = tag.healthConfirmed;

		float modelW = 24.0f;
		{
			float leftW[3], rightW[3];
			for (int i = 0; i < 3; ++i)
			{
				leftW[i]  = tag.origin[i] + view.axis[1][i] * q3::kPlayerBboxHalfWidth;
				rightW[i] = tag.origin[i] - view.axis[1][i] * q3::kPlayerBboxHalfWidth;
			}
			NameEsp::ScreenPoint sl, sr;
			const bool lOk = NameEsp::ProjectWorldToScreen(view, vp, leftW, sl);
			const bool rOk = NameEsp::ProjectWorldToScreen(view, vp, rightW, sr);
			if (lOk && rOk)
				modelW = fabsf(sr.x - sl.x);
			else if (lOk)
				modelW = 2.0f * fabsf(sl.x - (overlayP.x + (float)vp.x));
			else if (rOk)
				modelW = 2.0f * fabsf(sr.x - (overlayP.x + (float)vp.x));
		}
		if (modelW < 6.0f)
			modelW = 6.0f;

		const float frac = nameEspOn ? kWidthFracStacked : kWidthFracSolo;
		float w = modelW * frac * scale;
		if (w > modelW)
			w = modelW;
		if (w < 4.0f)
			w = 4.0f;

		float h = (nameEspOn ? kBarHeightStacked : kBarHeightSolo) * scale;
		if (h < 2.0f)
			h = 2.0f;

		float x = overlayP.x - w * 0.5f;
		float y = overlayP.y;
		if (nameEspOn)
			y = overlayP.y + kNameStackOffset;

		const float overlayW = (float)vp.width;
		const float overlayH = (float)vp.height;
		if (x < 0.0f)
			x = 0.0f;
		if (x + w > overlayW)
			x = overlayW - w;
		if (x < 0.0f)
			x = 0.0f;
		if (y < 0.0f)
			y = 0.0f;
		if (y + h > overlayH)
			y = overlayH - h;
		if (y < 0.0f)
			y = 0.0f;

		out.x = x;
		out.y = y;
		out.w = w;
		out.h = h;
		out.fillW = w * ratio;
		out.alpha = fade;
		out.visible = true;
		return true;
	}
}
