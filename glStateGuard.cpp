// =============================================================================================== //
// kutaQ3 hook - legacy (fixed function) OpenGL state guard - implementation
//
// See glStateGuard.h for the rationale. Short version: Dear ImGui's OpenGL2 backend leaves legacy
// state behind that Quake 3 assumes it owns, which makes the game flicker every other frame while
// the menu is up. This guard snapshots the state before the menu draws and puts it back afterwards.
// =============================================================================================== //

#include "glStateGuard.h"

// ----------------------------------------------------------------------------------------------- //
// GL 1.2+/ARB tokens and entry points that are missing from the ancient <gl/GL.h> shipped with the
// Windows SDK. Quake 3 resolves the very same ones through wglGetProcAddress at startup.
// ----------------------------------------------------------------------------------------------- //
#ifndef GL_TEXTURE0
#define GL_TEXTURE0						0x84C0
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE				0x84E0
#endif
#ifndef GL_CLIENT_ACTIVE_TEXTURE
#define GL_CLIENT_ACTIVE_TEXTURE		0x84E1
#endif
#ifndef GL_MAX_TEXTURE_UNITS
#define GL_MAX_TEXTURE_UNITS			0x84E2
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING			0x8894
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER_BINDING
#define GL_ELEMENT_ARRAY_BUFFER_BINDING	0x8895
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER					0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER			0x8893
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM				0x8B8D
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH			0x0CF2
#endif

typedef void (APIENTRY *PFN_glActiveTexture)(GLenum texture);
typedef void (APIENTRY *PFN_glClientActiveTexture)(GLenum texture);
typedef void (APIENTRY *PFN_glBindBuffer)(GLenum target, GLuint buffer);
typedef void (APIENTRY *PFN_glUseProgram)(GLuint program);

namespace
{
	// ------------------------------------------------------------------------------------------- //
	// Lazily resolved multitexture / buffer / shader entry points.
	// Everything is optional: on a pure GL 1.1 context the pointers stay NULL and the guard simply
	// skips the parts of the state that cannot exist there.
	// ------------------------------------------------------------------------------------------- //
	struct LegacyGLExt
	{
		bool						resolved;
		PFN_glActiveTexture			ActiveTexture;
		PFN_glClientActiveTexture	ClientActiveTexture;
		PFN_glBindBuffer			BindBuffer;
		PFN_glUseProgram			UseProgram;
		GLint						maxTextureUnits;
	};

	LegacyGLExt g_ext = { false, NULL, NULL, NULL, NULL, 1 };

	// nesting depth of live guards on this thread (the menu never renders re-entrantly, but the
	// counter keeps IsActive() honest if a future caller nests two guards)
	long g_guardDepth = 0;

	void* GetGLProc(const char* name)
	{
		PROC p = wglGetProcAddress(name);
		// wglGetProcAddress returns these sentinel values for "not supported"
		if (p == NULL || p == (PROC)1 || p == (PROC)2 || p == (PROC)3 || p == (PROC)-1)
			return NULL;
		return (void*)p;
	}

	void ResolveGLExt()
	{
		if (g_ext.resolved)
			return;

		g_ext.ActiveTexture			= (PFN_glActiveTexture)GetGLProc("glActiveTexture");
		if (!g_ext.ActiveTexture)
			g_ext.ActiveTexture		= (PFN_glActiveTexture)GetGLProc("glActiveTextureARB");

		g_ext.ClientActiveTexture	= (PFN_glClientActiveTexture)GetGLProc("glClientActiveTexture");
		if (!g_ext.ClientActiveTexture)
			g_ext.ClientActiveTexture = (PFN_glClientActiveTexture)GetGLProc("glClientActiveTextureARB");

		g_ext.BindBuffer			= (PFN_glBindBuffer)GetGLProc("glBindBuffer");
		if (!g_ext.BindBuffer)
			g_ext.BindBuffer		= (PFN_glBindBuffer)GetGLProc("glBindBufferARB");

		g_ext.UseProgram			= (PFN_glUseProgram)GetGLProc("glUseProgram");
		if (!g_ext.UseProgram)
			g_ext.UseProgram		= (PFN_glUseProgram)GetGLProc("glUseProgramObjectARB");

		g_ext.maxTextureUnits = 1;
		if (g_ext.ActiveTexture)
		{
			GLint units = 1;
			glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
			if (units < 1)								units = 1;
			if (units > GL::LEGACY_MAX_TEXTURE_UNITS)	units = GL::LEGACY_MAX_TEXTURE_UNITS;
			g_ext.maxTextureUnits = units;
		}

		// a driver that does not know GL_MAX_TEXTURE_UNITS raises GL_INVALID_ENUM - eat it
		while (glGetError() != GL_NO_ERROR) {}

		g_ext.resolved = true;
	}

	// the enable bits Quake 3 (and the OpenGL2 backend) actually care about
	const GLenum kCaps[] =
	{
		GL_BLEND,
		GL_DEPTH_TEST,
		GL_CULL_FACE,
		GL_TEXTURE_2D,
		GL_LIGHTING,
		GL_COLOR_MATERIAL,
		GL_SCISSOR_TEST,
		GL_STENCIL_TEST,
		GL_ALPHA_TEST,
		GL_FOG,
		GL_POLYGON_OFFSET_FILL,
		GL_POLYGON_OFFSET_LINE,
		GL_LINE_SMOOTH,
		GL_LINE_STIPPLE,
		GL_NORMALIZE,
		GL_CLIP_PLANE0
	};
	const int kNumCaps = sizeof(kCaps) / sizeof(kCaps[0]);

	void SetCap(GLenum cap, GLboolean on)
	{
		if (on) glEnable(cap);
		else	glDisable(cap);
	}
}

namespace GL
{

bool LegacyStateGuard::IsActive()
{
	return g_guardDepth > 0;
}

LegacyStateGuard::LegacyStateGuard()
	: m_bValid(false)
	, m_bPushedServer(false)
	, m_bPushedClient(false)
	, m_numTextureUnits(1)
	, m_activeTexture(GL_TEXTURE0)
	, m_clientActiveTexture(GL_TEXTURE0)
	, m_arrayBuffer(0)
	, m_elementArrayBuffer(0)
	, m_program(0)
{
	// no context = nothing to guard (can happen if SwapBuffers is called on a foreign DC)
	if (wglGetCurrentContext() == NULL)
		return;

	m_bValid = true;
	g_guardDepth++;

	ResolveGLExt();
	Capture();
	Neutralise();
}

LegacyStateGuard::~LegacyStateGuard()
{
	if (!m_bValid)
		return;

	Restore();

	// The menu must never make Quake 3's own glGetError() checks (GL_CheckErrors) trip.
	while (glGetError() != GL_NO_ERROR) {}

	g_guardDepth--;
	if (g_guardDepth < 0)
		g_guardDepth = 0;
}

// ----------------------------------------------------------------------------------------------- //
// 1. capture
// ----------------------------------------------------------------------------------------------- //
void LegacyStateGuard::Capture()
{
	// ---- server side state -------------------------------------------------------------------- //
	// glPushAttrib(GL_ALL_ATTRIB_BITS) is the cheap, complete way of saving enables, blending,
	// depth/stencil, fog, polygon, scissor, viewport, current colour, texture env, ... in one go.
	// It can fail if the game already filled the 16-deep attribute stack, so the result is checked
	// and a manual capture is used as a fallback.
	while (glGetError() != GL_NO_ERROR) {}
	glPushAttrib(GL_ALL_ATTRIB_BITS);
	m_bPushedServer = (glGetError() == GL_NO_ERROR);

	glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
	m_bPushedClient = (glGetError() == GL_NO_ERROR);

	// ---- matrices (never covered by glPushAttrib) --------------------------------------------- //
	glGetIntegerv(GL_MATRIX_MODE, &m_matrixMode);
	glGetFloatv(GL_PROJECTION_MATRIX, m_matProjection);
	glGetFloatv(GL_MODELVIEW_MATRIX, m_matModelView);

	// ---- per texture unit state ---------------------------------------------------------------- //
	m_numTextureUnits = g_ext.maxTextureUnits;

	if (g_ext.ActiveTexture)
		glGetIntegerv(GL_ACTIVE_TEXTURE, &m_activeTexture);
	if (g_ext.ClientActiveTexture)
		glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &m_clientActiveTexture);

	for (GLint unit = 0; unit < m_numTextureUnits; unit++)
	{
		if (g_ext.ActiveTexture)
			g_ext.ActiveTexture(GL_TEXTURE0 + unit);

		glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_texBinding2D[unit]);
		m_texEnabled2D[unit] = glIsEnabled(GL_TEXTURE_2D);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &m_texEnvMode[unit]);
		glGetFloatv(GL_TEXTURE_MATRIX, m_matTexture[unit]);

		if (g_ext.ClientActiveTexture)
		{
			g_ext.ClientActiveTexture(GL_TEXTURE0 + unit);
			m_texCoordArray[unit].enabled = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
			glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE,		&m_texCoordArray[unit].size);
			glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE,		&m_texCoordArray[unit].type);
			glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE,	&m_texCoordArray[unit].stride);
			glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER,	&m_texCoordArray[unit].pointer);
		}
		else
		{
			m_texCoordArray[unit].enabled = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
			glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE,		&m_texCoordArray[unit].size);
			glGetIntegerv(GL_TEXTURE_COORD_ARRAY_TYPE,		&m_texCoordArray[unit].type);
			glGetIntegerv(GL_TEXTURE_COORD_ARRAY_STRIDE,	&m_texCoordArray[unit].stride);
			glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER,	&m_texCoordArray[unit].pointer);
		}
	}

	// ---- modern objects that would break the fixed function draw ------------------------------ //
	if (g_ext.BindBuffer)
	{
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_arrayBuffer);
		glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &m_elementArrayBuffer);
	}
	if (g_ext.UseProgram)
		glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);

	// ---- manual fallback (only meaningful when the stacks were full) --------------------------- //
	if (!m_bPushedServer)
	{
		for (int i = 0; i < kNumCaps; i++)
			m_capEnabled[i] = glIsEnabled(kCaps[i]);

		glGetIntegerv(GL_BLEND_SRC, &m_blendSrc);
		glGetIntegerv(GL_BLEND_DST, &m_blendDst);
		glGetIntegerv(GL_DEPTH_FUNC, &m_depthFunc);
		glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depthMask);
		glGetBooleanv(GL_COLOR_WRITEMASK, m_colorMask);
		glGetIntegerv(GL_ALPHA_TEST_FUNC, &m_alphaFunc);
		glGetFloatv(GL_ALPHA_TEST_REF, &m_alphaRef);
		glGetIntegerv(GL_CULL_FACE_MODE, &m_cullFaceMode);
		glGetIntegerv(GL_FRONT_FACE, &m_frontFace);
		glGetIntegerv(GL_SHADE_MODEL, &m_shadeModel);
		glGetIntegerv(GL_POLYGON_MODE, m_polygonMode);
		glGetFloatv(GL_LINE_WIDTH, &m_lineWidth);
		glGetFloatv(GL_CURRENT_COLOR, m_currentColor);
		glGetIntegerv(GL_VIEWPORT, m_viewport);
		glGetIntegerv(GL_SCISSOR_BOX, m_scissorBox);
	}

	if (!m_bPushedClient)
	{
		m_vertexArray.enabled = glIsEnabled(GL_VERTEX_ARRAY);
		glGetIntegerv(GL_VERTEX_ARRAY_SIZE,		&m_vertexArray.size);
		glGetIntegerv(GL_VERTEX_ARRAY_TYPE,		&m_vertexArray.type);
		glGetIntegerv(GL_VERTEX_ARRAY_STRIDE,	&m_vertexArray.stride);
		glGetPointerv(GL_VERTEX_ARRAY_POINTER,	&m_vertexArray.pointer);

		m_colorArray.enabled = glIsEnabled(GL_COLOR_ARRAY);
		glGetIntegerv(GL_COLOR_ARRAY_SIZE,		&m_colorArray.size);
		glGetIntegerv(GL_COLOR_ARRAY_TYPE,		&m_colorArray.type);
		glGetIntegerv(GL_COLOR_ARRAY_STRIDE,	&m_colorArray.stride);
		glGetPointerv(GL_COLOR_ARRAY_POINTER,	&m_colorArray.pointer);

		m_normalArray.enabled = glIsEnabled(GL_NORMAL_ARRAY);
		m_normalArray.size = 3;
		glGetIntegerv(GL_NORMAL_ARRAY_TYPE,		&m_normalArray.type);
		glGetIntegerv(GL_NORMAL_ARRAY_STRIDE,	&m_normalArray.stride);
		glGetPointerv(GL_NORMAL_ARRAY_POINTER,	&m_normalArray.pointer);

		glGetIntegerv(GL_UNPACK_ALIGNMENT, &m_unpackAlignment);
		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &m_unpackRowLength);
	}

	// swallow anything the driver disliked while we were interrogating it
	while (glGetError() != GL_NO_ERROR) {}
}

// ----------------------------------------------------------------------------------------------- //
// 2. neutralise - put the context into the plain state the OpenGL2 backend expects
// ----------------------------------------------------------------------------------------------- //
void LegacyStateGuard::Neutralise()
{
	// ImGui draws with a single texture unit and no texture matrix. Quake 3 leaves unit 1 (and
	// sometimes 2) enabled with a GL_ADD/GL_MODULATE combiner and a scrolling texture matrix, so
	// the menu would come out double-textured / scrolled if we did not shut the extra units down.
	if (g_ext.ActiveTexture)
	{
		for (GLint unit = m_numTextureUnits - 1; unit >= 0; unit--)
		{
			g_ext.ActiveTexture(GL_TEXTURE0 + unit);

			glMatrixMode(GL_TEXTURE);
			glLoadIdentity();

			if (unit > 0)
			{
				glDisable(GL_TEXTURE_2D);
				glBindTexture(GL_TEXTURE_2D, 0);
			}

			if (g_ext.ClientActiveTexture)
			{
				g_ext.ClientActiveTexture(GL_TEXTURE0 + unit);
				if (unit > 0)
					glDisableClientState(GL_TEXTURE_COORD_ARRAY);
			}
		}

		// leave everything pointed at unit 0 - that is the only unit ImGui talks to
		g_ext.ActiveTexture(GL_TEXTURE0);
		if (g_ext.ClientActiveTexture)
			g_ext.ClientActiveTexture(GL_TEXTURE0);
	}
	else
	{
		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
	}

	// The OpenGL2 backend cannot express "no shader / no VBO" (those calls don't exist in GL2), so
	// the docs tell the integrator to do it. If a wrapper/ioquake3 renderer left a program or a
	// buffer bound, ImGui's client side vertex pointers would read from the VBO instead of RAM.
	if (g_ext.UseProgram && m_program != 0)
		g_ext.UseProgram(0);
	if (g_ext.BindBuffer)
	{
		if (m_arrayBuffer != 0)			g_ext.BindBuffer(GL_ARRAY_BUFFER, 0);
		if (m_elementArrayBuffer != 0)	g_ext.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}

	// State the backend assumes is off but never touches itself. GL_ALPHA_TEST in particular is
	// what eats the anti-aliased edges of the ImGui font and makes the menu strobe with Q3's
	// alpha-tested surfaces.
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_FOG);
	glDisable(GL_NORMALIZE);
	glDisable(GL_LINE_STIPPLE);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDisable(GL_POLYGON_OFFSET_LINE);
	glDisable(GL_CLIP_PLANE0);

	// Q3 turns depth writes off for translucent passes and colour writes off during its
	// shadow/fog passes - with those left over the menu is invisible or trails smearing.
	glDepthMask(GL_TRUE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthFunc(GL_LEQUAL);
	glFrontFace(GL_CCW);
	glLineWidth(1.0f);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

	// pixel store defaults - the font atlas upload in ImGui_ImplOpenGL2_UpdateTexture assumes them
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

	// leave the matrix mode where the backend expects to find it
	glMatrixMode(GL_MODELVIEW);

	while (glGetError() != GL_NO_ERROR) {}
}

// ----------------------------------------------------------------------------------------------- //
// 3. restore - hand the context back exactly as Quake 3 left it
// ----------------------------------------------------------------------------------------------- //
void LegacyStateGuard::Restore()
{
	// ---- per texture unit state (must be restored before the attrib pops) --------------------- //
	for (GLint unit = 0; unit < m_numTextureUnits; unit++)
	{
		if (g_ext.ActiveTexture)
			g_ext.ActiveTexture(GL_TEXTURE0 + unit);

		glMatrixMode(GL_TEXTURE);
		glLoadMatrixf(m_matTexture[unit]);

		glBindTexture(GL_TEXTURE_2D, (GLuint)m_texBinding2D[unit]);
		SetCap(GL_TEXTURE_2D, m_texEnabled2D[unit]);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, m_texEnvMode[unit]);

		if (g_ext.ClientActiveTexture)
			g_ext.ClientActiveTexture(GL_TEXTURE0 + unit);

		// the pointer itself is not part of the client attrib stack on every driver, so always
		// re-arm it from our own snapshot
		if (m_texCoordArray[unit].pointer || m_texCoordArray[unit].enabled)
			glTexCoordPointer(m_texCoordArray[unit].size, (GLenum)m_texCoordArray[unit].type,
							  m_texCoordArray[unit].stride, m_texCoordArray[unit].pointer);
		if (m_texCoordArray[unit].enabled)	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		else								glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}

	// put the selectors back the way the game had them
	if (g_ext.ActiveTexture)
		g_ext.ActiveTexture((GLenum)m_activeTexture);
	if (g_ext.ClientActiveTexture)
		g_ext.ClientActiveTexture((GLenum)m_clientActiveTexture);

	// ---- modern objects ------------------------------------------------------------------------ //
	if (g_ext.UseProgram && m_program != 0)
		g_ext.UseProgram((GLuint)m_program);
	if (g_ext.BindBuffer)
	{
		if (m_arrayBuffer != 0)			g_ext.BindBuffer(GL_ARRAY_BUFFER, (GLuint)m_arrayBuffer);
		if (m_elementArrayBuffer != 0)	g_ext.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)m_elementArrayBuffer);
	}

	// ---- matrices ------------------------------------------------------------------------------ //
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(m_matProjection);
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(m_matModelView);

	// ---- client + server attribute stacks ------------------------------------------------------ //
	if (m_bPushedClient)
	{
		glPopClientAttrib();
	}
	else
	{
		if (m_vertexArray.pointer || m_vertexArray.enabled)
			glVertexPointer(m_vertexArray.size, (GLenum)m_vertexArray.type, m_vertexArray.stride, m_vertexArray.pointer);
		if (m_vertexArray.enabled)	glEnableClientState(GL_VERTEX_ARRAY);
		else						glDisableClientState(GL_VERTEX_ARRAY);

		if (m_colorArray.pointer || m_colorArray.enabled)
			glColorPointer(m_colorArray.size, (GLenum)m_colorArray.type, m_colorArray.stride, m_colorArray.pointer);
		if (m_colorArray.enabled)	glEnableClientState(GL_COLOR_ARRAY);
		else						glDisableClientState(GL_COLOR_ARRAY);

		if (m_normalArray.pointer || m_normalArray.enabled)
			glNormalPointer((GLenum)m_normalArray.type, m_normalArray.stride, m_normalArray.pointer);
		if (m_normalArray.enabled)	glEnableClientState(GL_NORMAL_ARRAY);
		else						glDisableClientState(GL_NORMAL_ARRAY);

		glPixelStorei(GL_UNPACK_ALIGNMENT, m_unpackAlignment);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, m_unpackRowLength);
	}

	if (m_bPushedServer)
	{
		glPopAttrib();
	}
	else
	{
		for (int i = 0; i < kNumCaps; i++)
			SetCap(kCaps[i], m_capEnabled[i]);

		glBlendFunc((GLenum)m_blendSrc, (GLenum)m_blendDst);
		glDepthFunc((GLenum)m_depthFunc);
		glDepthMask(m_depthMask);
		glColorMask(m_colorMask[0], m_colorMask[1], m_colorMask[2], m_colorMask[3]);
		glAlphaFunc((GLenum)m_alphaFunc, m_alphaRef);
		glCullFace((GLenum)m_cullFaceMode);
		glFrontFace((GLenum)m_frontFace);
		glShadeModel((GLenum)m_shadeModel);
		glPolygonMode(GL_FRONT, (GLenum)m_polygonMode[0]);
		glPolygonMode(GL_BACK, (GLenum)m_polygonMode[1]);
		glLineWidth(m_lineWidth);
		glColor4fv(m_currentColor);
		glViewport(m_viewport[0], m_viewport[1], (GLsizei)m_viewport[2], (GLsizei)m_viewport[3]);
		glScissor(m_scissorBox[0], m_scissorBox[1], (GLsizei)m_scissorBox[2], (GLsizei)m_scissorBox[3]);
	}

	// ---- matrix mode last: glPopAttrib(GL_TRANSFORM_BIT) restores it too, but the manual path
	//      above does not, so set it explicitly either way.
	glMatrixMode((GLenum)m_matrixMode);
}

} // namespace GL
