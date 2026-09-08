#pragma once

// =============================================================================================== //
// kutaQ3 hook tests - call recorder shared by the stub <windows.h> / <gl/GL.h>
//
// Every stubbed GL / GDI entry point pushes one Call here, so a test can assert on what the real
// nameEsp.cpp + glText.cpp + glDraw.cpp actually issued: which raster positions, which colours and
// which text went out for which player.
//
// Header-only with function-local statics, so the stubs need no .cpp of their own and every
// translation unit in the test shares one recorder.
// =============================================================================================== //

#include <string>
#include <vector>

namespace Rec
{
	struct Call
	{
		std::string  fn;
		double       a[4];
		int          n;         // how many of a[] are meaningful
		std::string  text;      // glCallLists payload, decoded through the current list base
		unsigned int rgb;       // colour in effect when the call was made
		unsigned int listBase;
	};

	struct State
	{
		unsigned int rgb;
		unsigned int listBase;
		int viewport[4];
	};

	inline std::vector<Call>& Calls()
	{
		static std::vector<Call> calls;
		return calls;
	}

	inline State& StateRef()
	{
		static State s = { 0xffffffffu, 0u, { 0, 0, 0, 0 } };
		return s;
	}

	// wglGetCurrentDC() serves this, so a test can simulate Quake 3 destroying and recreating the
	// GL context (vid_restart) and check that the font gets rebuilt.
	inline void*& CurrentDC()
	{
		static void* dc = 0;
		return dc;
	}

	inline void Reset(int viewportX, int viewportY, int viewportW, int viewportH)
	{
		Calls().clear();
		StateRef().rgb      = 0xffffffffu;
		StateRef().listBase = 0u;
		StateRef().viewport[0] = viewportX;
		StateRef().viewport[1] = viewportY;
		StateRef().viewport[2] = viewportW;
		StateRef().viewport[3] = viewportH;
	}

	inline void Push(const char* fn, const double* a, int n)
	{
		Call c;
		c.fn       = fn;
		c.n        = n;
		c.rgb      = StateRef().rgb;
		c.listBase = StateRef().listBase;
		for (int i = 0; i < 4; ++i)
			c.a[i] = (i < n) ? a[i] : 0.0;
		Calls().push_back(c);
	}

	inline void Push0(const char* fn)                      { Push(fn, 0, 0); }
	inline void Push1(const char* fn, double a)            { const double v[1] = { a }; Push(fn, v, 1); }
	inline void Push2(const char* fn, double a, double b)  { const double v[2] = { a, b }; Push(fn, v, 2); }
	inline void Push3(const char* fn, double a, double b, double c)
	{
		const double v[3] = { a, b, c };
		Push(fn, v, 3);
	}
	inline void Push4(const char* fn, double a, double b, double c, double d)
	{
		const double v[4] = { a, b, c, d };
		Push(fn, v, 4);
	}

	inline int Count(const char* fn)
	{
		int n = 0;
		for (size_t i = 0; i < Calls().size(); ++i)
			if (Calls()[i].fn == fn)
				++n;
		return n;
	}

	inline std::vector<const Call*> All(const char* fn)
	{
		std::vector<const Call*> out;
		for (size_t i = 0; i < Calls().size(); ++i)
			if (Calls()[i].fn == fn)
				out.push_back(&Calls()[i]);
		return out;
	}

	inline const Call* Last(const char* fn)
	{
		for (size_t i = Calls().size(); i-- > 0; )
			if (Calls()[i].fn == fn)
				return &Calls()[i];
		return 0;
	}

	// index of a call within the whole recorded stream, for order checks
	inline int IndexOf(const Call* c)
	{
		for (size_t i = 0; i < Calls().size(); ++i)
			if (&Calls()[i] == c)
				return (int)i;
		return -1;
	}

	// the call immediately before c with the given name (0 if none)
	inline const Call* Prev(const Call* c, const char* fn)
	{
		const int at = IndexOf(c);
		for (int i = at - 1; i >= 0; --i)
			if (Calls()[(size_t)i].fn == fn)
				return &Calls()[(size_t)i];
		return 0;
	}
}
