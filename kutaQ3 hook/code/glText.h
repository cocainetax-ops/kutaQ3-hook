

#pragma once
#include <windows.h>
#include <stdio.h>
#include <gl\GL.h>

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
		unsigned int base;
		HDC hdc = nullptr;
		int m_height;
		int m_width;

		void Build(int height);
		void Print(float x, float y, const unsigned char color[3], const char *format, ...);

		//center on X and Y axes
		vec3 centerText(float x, float y, float width, float height, float textWidth, float textHeight);

		//center on X axis only
		float centerText(float x, float width, float textWidth);
	};
};

