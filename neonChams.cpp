#include "main.h"   // team model flags + origglDrawElements (extern, defined in main.cpp)
#include "neonChams.h"

#include <math.h>   // sinf for the brightness pulse

// =============================================================================================== //
// kutaQ3 hook - "Neon" player chams implementation (see neonChams.h for the overview)
// =============================================================================================== //

namespace
{
	// per-team neon palette: through-wall bloom colour + white-hot core colour
	struct NeonPalette
	{
		int haloR, haloG, haloB; // additive bloom halo (glows through walls)
		int coreR, coreG, coreB; // white-hot core (depth tested, only the visible part)
	};

	const NeonPalette& PaletteForTeam()
	{
		static const NeonPalette ffa  = { 255,   0, 255, 255, 228, 255 }; // neon magenta / hot pink core
		static const NeonPalette red  = { 255,   0,  64, 255, 236, 236 }; // neon red      / hot white core
		static const NeonPalette blue = {   0, 255, 255, 228, 255, 255 }; // neon cyan     / hot white core

		if (red_team_player_models)
			return red;
		if (blue_team_player_models)
			return blue;
		return ffa; // free for all (and anything else) glows magenta
	}

	// One additive, slightly inflated silhouette pass. Each pass is wrapped in its own
	// glPushMatrix/glScalef/glPopMatrix pair so the inflations never accumulate.
	void HaloPass(float inflate, int r, int g, int b, int a,
	              GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
	{
		glPushMatrix();
		glScalef(inflate, inflate, inflate);
		glDisableClientState(GL_COLOR_ARRAY);
		glEnable(GL_COLOR_MATERIAL);
		glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, (GLubyte)a);
		origglDrawElements(mode, count, type, indices);
		glPopMatrix();
	}
}

// =============================================================================================== //

void Neon::DrawChams(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	const NeonPalette& pal = PaletteForTeam();

	// brightness pulse: the bloom "breathes" between ~72% and 100% every 1.1 seconds
	const float  twoPi = 6.28318530718f;
	const float  phase = (float)(timeGetTime() % 1100) * (twoPi / 1100.0f);
	const float  pulse = 0.72f + 0.28f * sinf(phase);

	glPushMatrix();

	// ---- bloom halo: depth test OFF (glows through walls), additive blending (passes ADD light) ----
	glDisable(GL_TEXTURE_2D);
	glDisableClientState(GL_COLOR_ARRAY);
	glEnable(GL_COLOR_MATERIAL);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glDisable(GL_DEPTH_TEST);

	HaloPass(1.100f, pal.haloR, pal.haloG, pal.haloB, (int)( 52.0f * pulse), mode, count, type, indices); // faint outer aura
	HaloPass(1.055f, pal.haloR, pal.haloG, pal.haloB, (int)( 92.0f * pulse), mode, count, type, indices); // mid halo
	HaloPass(1.025f, pal.haloR, pal.haloG, pal.haloB, (int)(140.0f * pulse), mode, count, type, indices); // inner halo

	// ---- white-hot core: depth test back ON, exact silhouette, full-bright ----
	glEnable(GL_DEPTH_TEST);
	glColor4ub((GLubyte)pal.coreR, (GLubyte)pal.coreG, (GLubyte)pal.coreB, 255);
	origglDrawElements(mode, count, type, indices);

	// ---- leave the GL state the way the classic chams styles leave it ----
	// (blend back to standard alpha, texturing back on, depth test on - Quake 3 re-syncs the rest
	// from its own glState shadow on the next draw call)
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_2D);
	glPopMatrix();
}
