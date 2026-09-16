#pragma once

// =============================================================================================== //
// kutaQ3 hook tests - a stand-in for <gl/GL.h>
//
// Every entry point the NAME ESP overlay touches, recording into glrec.h instead of calling a
// driver. GL_VIEWPORT is served out of the recorder's state so a test can pick the viewport the
// projection is checked against.
// =============================================================================================== //

#include "glrec.h"

// ---- types and constants -----------------------------------------------------------------------
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;

#define GL_DEPTH_TEST          0x0B71
#define GL_TEXTURE_2D          0x0DE1
#define GL_BLEND               0x0BE2
#define GL_VIEWPORT            0x0BA2
#define GL_ALL_ATTRIB_BITS     0x000FFFFF
#define GL_LIST_BIT            0x00020000
#define GL_UNSIGNED_BYTE       0x1401
#define GL_LINES               0x0001
#define GL_LINE_STRIP          0x0003
#define GL_QUADS               0x0007
#define GL_MODELVIEW           0x1700
#define GL_PROJECTION          0x1701
#define GL_SRC_ALPHA           0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_RGBA                0x1908
#define GL_TEXTURE_MAG_FILTER  0x2800
#define GL_TEXTURE_MIN_FILTER  0x2801
#define GL_LINEAR              0x2601
#define GL_TEXTURE_ENV         0x2300
#define GL_TEXTURE_ENV_MODE    0x2200
#define GL_MODULATE            0x2100

// ---- recording entry points --------------------------------------------------------------------
inline void glGetIntegerv(GLenum pname, GLint* params)
{
	if (pname == GL_VIEWPORT && params)
	{
		for (int i = 0; i < 4; ++i)
			params[i] = Rec::StateRef().viewport[i];
	}
	Rec::Push1("glGetIntegerv", (double)pname);
}

inline void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	Rec::StateRef().viewport[0] = x;
	Rec::StateRef().viewport[1] = y;
	Rec::StateRef().viewport[2] = w;
	Rec::StateRef().viewport[3] = h;
	Rec::Push4("glViewport", x, y, w, h);
}

inline void glPushAttrib(GLbitfield mask) { Rec::Push1("glPushAttrib", (double)mask); }
inline void glPopAttrib(void)             { Rec::Push0("glPopAttrib"); }
inline void glPushMatrix(void)            { Rec::Push0("glPushMatrix"); }
inline void glPopMatrix(void)             { Rec::Push0("glPopMatrix"); }
inline void glLoadIdentity(void)          { Rec::Push0("glLoadIdentity"); }
inline void glMatrixMode(GLenum mode)     { Rec::Push1("glMatrixMode", (double)mode); }
inline void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
	Rec::Push4("glOrtho", l, r, b, t);
}
inline void glEnable(GLenum cap)          { Rec::Push1("glEnable", (double)cap); }
inline void glDisable(GLenum cap)         { Rec::Push1("glDisable", (double)cap); }
inline void glLineWidth(GLfloat w)        { Rec::Push1("glLineWidth", (double)w); }
inline void glBegin(GLenum mode)          { Rec::Push1("glBegin", (double)mode); }
inline void glEnd(void)                   { Rec::Push0("glEnd"); }
inline void glVertex2f(GLfloat x, GLfloat y) { Rec::Push2("glVertex2f", x, y); }
inline void glVertex2i(GLint x, GLint y)     { Rec::Push2("glVertex2i", x, y); }

inline void glColor3f(GLfloat r, GLfloat g, GLfloat b)
{
	Rec::StateRef().rgb   = ((unsigned int)(r * 255.0f) << 16) |
	                        ((unsigned int)(g * 255.0f) << 8) |
	                        ((unsigned int)(b * 255.0f));
	Rec::StateRef().alpha = 1.0f;
	Rec::Push3("glColor3f", r, g, b);
}

inline void glColor3ub(GLubyte r, GLubyte g, GLubyte b)
{
	Rec::StateRef().rgb   = ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
	Rec::StateRef().alpha = 1.0f;
	Rec::Push3("glColor3ub", r, g, b);
}

inline void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
	Rec::StateRef().rgb   = ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
	Rec::StateRef().alpha = (float)a / 255.0f;
	Rec::Push4("glColor4ub", r, g, b, a);
}

inline void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	Rec::StateRef().rgb   = ((unsigned int)(r * 255.0f) << 16) |
	                        ((unsigned int)(g * 255.0f) << 8) |
	                        ((unsigned int)(b * 255.0f));
	Rec::StateRef().alpha = a;
	Rec::Push4("glColor4f", r, g, b, a);
}

inline void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
	Rec::Push2("glBlendFunc", (double)sfactor, (double)dfactor);
}

inline void glRasterPos2f(GLfloat x, GLfloat y) { Rec::Push2("glRasterPos2f", x, y); }

inline void glListBase(GLuint base)
{
	Rec::StateRef().listBase = base;
	Rec::Push1("glListBase", (double)base);
}

// The text is recovered exactly the way the driver would. glText.cpp sets the list base to
// base-32, so the driver calls list (base-32)+byte for byte b - which is the list wglUseFontBitmaps
// baked for ASCII character b. The byte IS the character.
inline void glCallLists(GLsizei n, GLenum type, const GLvoid* lists)
{
	const double v[1] = { (double)n };
	Rec::Push("glCallLists", v, 1);

	Rec::Call& c = Rec::Calls().back();
	if (type == GL_UNSIGNED_BYTE && lists)
	{
		const unsigned char* bytes = (const unsigned char*)lists;
		for (GLsizei i = 0; i < n; ++i)
			c.text += (char)bytes[i];
	}
}

inline GLuint glGenLists(GLsizei range)
{
	static GLuint nextBase = 1000;
	const GLuint base = nextBase;
	nextBase += (GLuint)range;
	Rec::Push1("glGenLists", (double)range);
	return base;
}

// ---- the texture surface the WEAPON ESP icon path touches (weaponEsp.cpp) -------------------
// glGenTextures hands out ids the way a driver would, so a test can tell "bound the loaded
// icon" from "bound nothing" in the recorded calls. Same void-out signature as the real
// <GL/gl.h>, so weaponEsp.cpp cannot drift onto a call that only compiles against this stub.
inline void glGenTextures(GLsizei n, GLuint* textures)
{
	static GLuint nextTex = 5000;
	for (GLsizei i = 0; i < n; ++i)
		textures[i] = nextTex++;
	Rec::Push1("glGenTextures", (double)n);
}

inline void glBindTexture(GLenum target, GLuint texture)
{
	Rec::Push2("glBindTexture", (double)target, (double)texture);
}

inline void glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width,
                         GLsizei height, GLint border, GLenum format, GLenum type,
                         const GLvoid* data)
{
	Rec::Push4("glTexImage2D", (double)internalFormat, (double)width, (double)height,
	           (double)(data != 0 ? 1 : 0));
}

inline void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
	Rec::Push3("glTexParameteri", (double)target, (double)pname, (double)param);
}

inline void glTexEnvi(GLenum target, GLenum pname, GLint param)
{
	Rec::Push3("glTexEnvi", (double)target, (double)pname, (double)param);
}

inline void glTexCoord2f(GLfloat u, GLfloat v) { Rec::Push2("glTexCoord2f", u, v); }
