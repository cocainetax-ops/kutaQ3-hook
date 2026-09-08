

#pragma once
#include <windows.h>
#include <stdio.h>
#include <gl/GL.h>

//https://guidedhacking.com/threads/opengl-swapbuffers-hook-template-source-code.11476/

struct vec3 { float x, y, z; };

//#define FONT_HEIGHT 18
#define FONT_HEIGHT 14



namespace GL
{
	class Font
	{
	public:
		bool bBuilt = false;
		unsigned int base = 0;
		HDC hdc = nullptr;
		HFONT hFont = nullptr;   // kept alive after Build() so TextWidth() can measure with the
		                         // very face the display lists were baked from
		int m_height = 0;
		int m_width = 0;

		void Build(int height);
		void Print(float x, float y, const unsigned char color[3], const char *format, ...);

		// width in pixels of text rendered in this font - what centring a label needs.
		// Returns 0 when the font is not built or the string is empty.
		float TextWidth(const char *text);

		//center on X and Y axes
		vec3 centerText(float x, float y, float width, float height, float textWidth, float textHeight);

		//center on X axis only
		float centerText(float x, float width, float textWidth);
	};
};

