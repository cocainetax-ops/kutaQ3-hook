
#include "glText.h"



//https://guidedhacking.com/threads/opengl-swapbuffers-hook-template-source-code.11476/

void GL::Font::Build(int height)
{
	// Rebuilding (Quake 3 destroys the GL context on vid_restart) throws away the previous face.
	// The old display list range is deliberately NOT glDeleteLists'd here: after a context switch
	// the stale base no longer belongs to this context, and deleting that range could destroy
	// Quake 3's own lists. Leaking 96 list ids per vid_restart is the cheaper mistake.
	if (hFont)
	{
		DeleteObject(hFont);
		hFont = nullptr;
	}

	hdc = wglGetCurrentDC();
	base = glGenLists(96);
	if (!hdc || !base)
	{
		bBuilt = false;
		return;
	}

	m_height = height;
	hFont = CreateFontA(-(height), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, PROOF_QUALITY, FF_DONTCARE | DEFAULT_PITCH, "Calibri");
	//HFONT hFont = CreateFontA(-(height), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, PROOF_QUALITY, FF_DONTCARE | DEFAULT_PITCH, "Consolas");
	HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
	wglUseFontBitmaps(hdc, 32, 96, base);
	SelectObject(hdc, hOldFont);

	// hFont stays alive: TextWidth() measures strings with this exact face.

	bBuilt = true;
}


//width in pixels of text rendered in this font - GDI measures it with the same face
//wglUseFontBitmaps baked the display lists from, so this matches what glCallLists advances by
float GL::Font::TextWidth(const char *text)
{
	if (!bBuilt || !hdc || !hFont || !text || !text[0])
		return 0.0f;

	HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
	SIZE size = { 0, 0 };
	GetTextExtentPoint32A(hdc, text, (int)strlen(text), &size);
	SelectObject(hdc, hOldFont);

	return (float)size.cx;
}


//replaced glRasterPos2f  with glWindowPos2d
void GL::Font::Print(float x, float y, const unsigned char color[3], const char *format, ...)
{

	glColor3ub(color[0], color[1], color[2]);
	glRasterPos2f(x, y);
 //read https://stackoverflow.com/questions/34780950/text-wont-output-on-screen-c-opengl


	char text[100];
	va_list    args;

	va_start(args, format);
	vsprintf_s(text, 100, format, args);
	va_end(args);

	glPushAttrib(GL_LIST_BIT);
	glListBase(base - 32);
	glCallLists(strlen(text), GL_UNSIGNED_BYTE, text);

	glPopAttrib();
}

vec3 GL::Font::centerText(float x, float y, float width, float height, float textWidth, float textHeight)
{
	UNREFERENCED_PARAMETER(height);
	vec3 text;
	text.x = x + (width - textWidth) / 2;
	text.y = y + textHeight;
	return text;
}

float GL::Font::centerText(float x, float width, float textWidth)
{
	if (width > textWidth)
	{
		float difference = width - textWidth;
		return (x + (difference / 2));
	}

	else
	{
		float difference = textWidth - width;
		return (x - (difference / 2));
	}
}

