#pragma once

// =============================================================================================== //
// kutaQ3 hook tests - a stand-in for <windows.h>
//
// Only the handful of declarations glText.cpp / glDraw.cpp / nameEsp.cpp / glStateGuard.h use, with
// the GDI calls recording into glrec.h. It exists so the real sources can be compiled and run on a
// host that has no Windows SDK; it is NOT a general windows.h replacement.
// =============================================================================================== //

#include "glrec.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// ---- basic types ------------------------------------------------------------------------------
typedef int            BOOL;
typedef unsigned char  BYTE;
typedef unsigned int   UINT;
typedef long           LONG;
typedef unsigned long  DWORD;
typedef void*          HANDLE;
typedef void*          HDC;
typedef void*          HGLRC;
typedef void*          HFONT;
typedef void*          HGDIOBJ;
typedef void*          HWND;
typedef void*          HINSTANCE;
typedef void*          LPVOID;
typedef char*          LPSTR;
typedef const char*    LPCSTR;
typedef char*          LPTSTR;
typedef const char*    LPCTSTR;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif

#define WINAPI
#define APIENTRY
#define UNREFERENCED_PARAMETER(P) ((void)(P))

// The MSVC secure CRT call glText.cpp uses. glibc has no equivalent name, so the stub maps it onto
// vsnprintf with the same bounds check the MSVC version performs.
#ifndef _MSC_VER
inline int vsprintf_s(char* buffer, size_t sizeOfBuffer, const char* format, va_list ap)
{
	return vsnprintf(buffer, sizeOfBuffer, format, ap);
}
#endif

// ---- GDI font constants used by glText.cpp -----------------------------------------------------
#define FW_BOLD                700
#define ANSI_CHARSET           0
#define OUT_TT_PRECIS          4
#define CLIP_DEFAULT_PRECIS    0
#define PROOF_QUALITY          2
#define FF_DONTCARE            0
#define DEFAULT_PITCH          0

typedef struct tagSIZE { long cx; long cy; } SIZE;

// ---- recording stubs ---------------------------------------------------------------------------
// Every fake font handle is distinct so a test can tell "selected the built face" from "restored
// the previous object".
inline HFONT CreateFontA(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight,
                         DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet,
                         DWORD iOutputPrecision, DWORD iClipPrecision, DWORD iQuality,
                         DWORD iPitchAndFamily, LPCSTR pszFaceName)
{
	static int next = 1;
	Rec::Push2("CreateFontA", (double)cHeight, (double)cWeight);
	return (HFONT)(size_t)(next++);
}

inline HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj)
{
	Rec::Push1("SelectObject", (double)(size_t)obj);
	static HGDIOBJ previous = (HGDIOBJ)(size_t)0x1000;
	HGDIOBJ old = previous;
	previous = obj;
	return old;
}

inline BOOL DeleteObject(HGDIOBJ obj)
{
	Rec::Push1("DeleteObject", (double)(size_t)obj);
	return TRUE;
}

// A deterministic 7px per character face: enough to check that centring used the measured width.
inline BOOL GetTextExtentPoint32A(HDC hdc, LPCSTR text, int len, SIZE* size)
{
	if (!size)
		return FALSE;
	size->cx = 7 * len;
	size->cy = 14;
	Rec::Push1("GetTextExtentPoint32A", (double)len);
	return TRUE;
}

inline HDC wglGetCurrentDC(void)
{
	static int dcStorage = 0;
	if (!Rec::CurrentDC())
		Rec::CurrentDC() = &dcStorage;
	return (HDC)Rec::CurrentDC();
}

inline HGLRC wglGetCurrentContext(void)
{
	static int rcStorage = 0;
	return (HGLRC)&rcStorage;
}

inline BOOL wglUseFontBitmapsA(HDC hdc, DWORD first, DWORD count, DWORD listBase)
{
	Rec::Push3("wglUseFontBitmaps", (double)first, (double)count, (double)listBase);
	return TRUE;
}
#define wglUseFontBitmaps wglUseFontBitmapsA
