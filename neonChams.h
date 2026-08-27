#pragma once

// =============================================================================================== //
// kutaQ3 hook - "Neon" player chams (intense neon brightness bloom)
//
// A SEPARATE visual feature built on top of the same player-model detection the classic chams
// use (the models/players/... shader sniffing in newglBindTexture). Instead of flat-filling the
// model like Solid / Wireframe, this style makes it GLOW like a neon tube:
//
//   - Three halo passes, drawn with the depth test OFF (the glow bleeds through walls - that is
//     the wallhack part) at slightly inflated scales, all blended ADDITIVELY
//     (glBlendFunc(GL_SRC_ALPHA, GL_ONE)). Every pass ADDS light on top of the previous one, so
//     the layered silhouettes accumulate into a soft brightness bloom around the model. That is
//     the classic fixed-function trick - Quake 3's GL 1.1 context has no shaders or FBOs, so a
//     real post-process bloom is impossible; additive over-draw is how it was done back then.
//   - One white-hot core pass, drawn with the depth test back ON at exact scale (only the part
//     of the model that is actually visible), so the player reads as a saturated neon tube with
//     a hot centre.
//   - A timeGetTime() pulse modulates the halo brightness, so the bloom "breathes" (~72%-100%
//     every 1.1 s) instead of sitting static.
//
// Toggled from the VISUALS tab of the "kutaQ3 hook" menu with the "Neon" button
// (Config::g_Settings.neonEnabled, persisted in kutaQ3.cfg). While enabled it takes priority over
// the classic Solid / Wireframe styles so the two never stack passes on the same draw call.
// =============================================================================================== //

#include <gl/GL.h>

namespace Neon
{
	// called from newglDrawElements for every player-model draw call
	void DrawChams(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
}
