#pragma once

// =============================================================================================== //
// kutaQ3 hook - legacy (fixed function) OpenGL state guard
//
// Why this exists:
//   Quake 3 drives a legacy OpenGL 1.1 fixed function pipeline and keeps its OWN shadow copy of the
//   GL state (tr.glState / GL_State()). Dear ImGui's OpenGL2 backend changes a fair amount of that
//   state while it draws the menu and only restores a subset of it (GL_ENABLE_BIT, GL_COLOR_BUFFER_BIT
//   and GL_TRANSFORM_BIT plus a handful of manual glGet's).
//
//   Everything the backend does NOT touch or restore stays wrong for the next game frame:
//     - the active / client active texture unit (Q3 is a multitexture renderer, it lives on unit 1 a lot)
//     - texture matrices, texture bindings and GL_TEXTURE_2D enables of the secondary units
//     - the vertex/colour/texcoord array pointers and enables (Q3 re-uses them every frame)
//     - GL_ALPHA_TEST, GL_FOG, glDepthMask, glColorMask, polygon offset, line stipple, clip planes
//     - a bound VBO / GLSL program on ioquake3-style renderers
//
//   The result is the classic "menu makes the game flicker" symptom: one frame is drawn with the
//   state ImGui left behind, then Q3 notices and fixes it, then ImGui breaks it again, every frame.
//
// What this does:
//   LegacyStateGuard is a scoped RAII guard. Construct it right before the ImGui frame is built and
//   let it die right after ImGui_ImplOpenGL2_RenderDrawData() returned. It
//     1. captures the complete legacy state (server + client + matrices + per texture unit state),
//     2. neutralises the state the OpenGL2 backend assumes but never sets itself,
//     3. restores the captured state byte for byte when it goes out of scope,
//     4. swallows any GL error the menu produced so Q3's own glGetError() checks stay quiet.
//
//   While the guard is alive IsActive() returns true - the hooked glBindTexture / glDrawElements /
//   glVertexPointer detours use that to pass ImGui's own draw calls straight through to the
//   original functions (no chams, no shader sniffing).
// =============================================================================================== //

#include <windows.h>
#include <gl/GL.h>

namespace GL
{
	// Highest number of texture units the guard saves/restores. Quake 3 uses 2 (3 with r_ext_multitexture
	// style mods); 8 covers every legacy driver combination we can hit without querying huge unit counts.
	enum { LEGACY_MAX_TEXTURE_UNITS = 8 };

	class LegacyStateGuard
	{
	public:
		LegacyStateGuard();
		~LegacyStateGuard();

		// true while a guard is alive on this thread (i.e. we are inside the ImGui menu rendering).
		static bool IsActive();

	private:
		// non copyable - the guard owns the saved GL state
		LegacyStateGuard(const LegacyStateGuard&);
		LegacyStateGuard& operator=(const LegacyStateGuard&);

		void Capture();
		void Neutralise();
		void Restore();

		struct ClientArray
		{
			GLboolean	enabled;
			GLint		size;
			GLint		type;
			GLint		stride;
			GLvoid*		pointer;
		};

		bool		m_bValid;			// false when there is no current GL context
		bool		m_bPushedServer;	// glPushAttrib(GL_ALL_ATTRIB_BITS) succeeded (stack had room)
		bool		m_bPushedClient;	// glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS) succeeded

		// --- always captured manually (glPushAttrib does not cover these) ---
		GLint		m_matrixMode;
		GLfloat		m_matProjection[16];
		GLfloat		m_matModelView[16];
		GLfloat		m_matTexture[LEGACY_MAX_TEXTURE_UNITS][16];

		GLint		m_numTextureUnits;
		GLint		m_activeTexture;
		GLint		m_clientActiveTexture;
		GLint		m_texBinding2D[LEGACY_MAX_TEXTURE_UNITS];
		GLboolean	m_texEnabled2D[LEGACY_MAX_TEXTURE_UNITS];
		GLint		m_texEnvMode[LEGACY_MAX_TEXTURE_UNITS];
		ClientArray	m_texCoordArray[LEGACY_MAX_TEXTURE_UNITS];

		// modern objects that must not be bound while the fixed function pipeline draws
		GLint		m_arrayBuffer;
		GLint		m_elementArrayBuffer;
		GLint		m_program;

		// --- fallback capture, only used when the attribute stacks were full ---
		GLboolean	m_capEnabled[16];
		GLint		m_blendSrc, m_blendDst;
		GLint		m_depthFunc;
		GLboolean	m_depthMask;
		GLboolean	m_colorMask[4];
		GLint		m_alphaFunc;
		GLfloat		m_alphaRef;
		GLint		m_cullFaceMode;
		GLint		m_frontFace;
		GLint		m_shadeModel;
		GLint		m_polygonMode[2];
		GLfloat		m_lineWidth;
		GLfloat		m_currentColor[4];
		GLint		m_viewport[4];
		GLint		m_scissorBox[4];
		GLint		m_unpackAlignment;
		GLint		m_unpackRowLength;
		ClientArray	m_vertexArray;
		ClientArray	m_colorArray;
		ClientArray	m_normalArray;
	};
}

// convenience: drop one of these at the top of the scope that renders the menu
#define KUTAQ3_LEGACY_GL_STATE_GUARD()	GL::LegacyStateGuard __kutaQ3_gl_state_guard
