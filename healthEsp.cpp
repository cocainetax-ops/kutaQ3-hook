// =============================================================================================== //
// kutaQ3 hook - HEALTH ESP, GL half (see healthEsp.h for the overview)
//
// Renders a green-to-red 2D health bar above every other player, using the tags NameEsp::Gather()
// already built. Runs from the hooked SwapBuffers, after NameEsp::Draw() so the bar can sit
// underneath the name when both features are on.
// =============================================================================================== //

#include "healthEsp.h"
#include "config.h"
#include "glDraw.h"
#include "glStateGuard.h"

#include <gl/GL.h>

namespace
{
	HealthEsp::DrawStats s_stats;

	const int kHeadFramesToBack = 3;

	struct TagState
	{
		int  clientNum;
		int  headHeld;
		bool onChest;
		int  steppedAt;
	};

	TagState s_tags[q3::kMaxClients];
	int      s_frameSerial = 0;

	void ResetTagState()
	{
		for (int i = 0; i < q3::kMaxClients; ++i)
		{
			s_tags[i].clientNum = -1;
			s_tags[i].headHeld  = 0;
			s_tags[i].onChest   = false;
			s_tags[i].steppedAt = 0;
		}
	}

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
		if (freeSlot < 0)
			freeSlot = 0;
		s_tags[freeSlot].clientNum = clientNum;
		s_tags[freeSlot].headHeld  = 0;
		s_tags[freeSlot].onChest   = false;
		s_tags[freeSlot].steppedAt = 0;
		return s_tags[freeSlot];
	}

	void ToOverlay(const NameEsp::Viewport& vp, NameEsp::ScreenPoint& p)
	{
		p.x -= (float)vp.x;
		p.y += (float)vp.y;
	}
}

const HealthEsp::DrawStats& HealthEsp::LastDrawStats()
{
	return s_stats;
}

void HealthEsp::ResetDrawState()
{
	ResetTagState();
}

void HealthEsp::Draw()
{
	s_stats.drawn = s_stats.inView = s_stats.skipped = 0;
	if (!Config::g_Settings.healthEsp)
	{
		ResetTagState();
		return;
	}

	const NameEsp::Frame& frame = NameEsp::Current();
	if (!frame.valid || frame.playerCount <= 0)
		return;

	GLint glViewportRect[4] = { 0, 0, 0, 0 };
	glGetIntegerv(GL_VIEWPORT, glViewportRect);

	const NameEsp::Viewport vp = { glViewportRect[0], glViewportRect[1],
	                               glViewportRect[2], glViewportRect[3] };
	if (vp.width <= 0 || vp.height <= 0)
		return;

	++s_frameSerial;
	const bool nameEspOn = Config::g_Settings.nameEsp;

	{
		KUTAQ3_LEGACY_GL_STATE_GUARD();
		GL::SetupOrtho();

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		static const unsigned char kBg[3]     = { 20, 20, 20 };
		static const unsigned char kOutline[3] = { 0, 0, 0 };

		for (int i = 0; i < frame.playerCount; ++i)
		{
			const NameEsp::PlayerTag& tag = frame.players[i];

			NameEsp::ScreenPoint p;
			const bool headOk = NameEsp::ProjectWorldToScreen(frame.view, vp, tag.origin, p);
			if (headOk)
				ToOverlay(vp, p);
			const bool headOnScreen = headOk && p.inView;

			float chest[3] = { tag.origin[0], tag.origin[1],
			                   tag.origin[2] - q3::kPlayerTagHeight + q3::kChestHeight };
			NameEsp::ScreenPoint pc;
			const bool chestOk = NameEsp::ProjectWorldToScreen(frame.view, vp, chest, pc);
			if (chestOk)
				ToOverlay(vp, pc);
			const bool chestOnScreen = chestOk && pc.inView;

			TagState& st = TagSlot(tag.clientNum);
			const bool fresh = (st.steppedAt != s_frameSerial);
			if (fresh)
				st.steppedAt = s_frameSerial;

			if (headOnScreen)
			{
				if (st.onChest)
				{
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
					++s_stats.skipped;
					continue;
				}
				if (chestOnScreen)
				{
					st.onChest = true;
					p = pc;
				}
			}

			if (!p.inView)
			{
				++s_stats.skipped;
				continue;
			}

			BarGeom bar;
			if (!ComputeBar(tag, frame.view, vp, p, nameEspOn, bar) || !bar.visible)
			{
				++s_stats.skipped;
				continue;
			}

			GL::DrawFilledRectAlpha(bar.x, bar.y, bar.w, bar.h, kBg, bar.alpha * 0.85f);
			if (bar.fillW > 0.5f)
				GL::DrawFilledRectAlpha(bar.x, bar.y, bar.fillW, bar.h, bar.fill, bar.alpha);
			GL::DrawOutlineAlpha(bar.x, bar.y, bar.w, bar.h, 1.0f, kOutline, bar.alpha);

			++s_stats.drawn;
			++s_stats.inView;
		}

		GL::RestoreGL();
	}
}
