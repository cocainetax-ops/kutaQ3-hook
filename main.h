#pragma once
#pragma warning(disable:4244) //disable warning C4244
#pragma warning(disable:4996)
#pragma comment(lib,"opengl32.lib")
#pragma comment(lib,"detours.lib")
#pragma comment(lib, "winmm.lib")  //timeGetTime, playsound

#include <Windows.h>
#include <gl/GL.h>
#include <vector> //AimPoint
#include <string> //std::string
#include <fstream> //ofstream
#include <time.h> //initparticles time
#include "detours.h"
using namespace std; //logfile

#include "glText.h"
#include "glDraw.h"
#include <tchar.h>


/*
#ifdef MSC_VER < 1700 //pre 2012
#pragma comment(lib,"Xinput.lib")
#else
#pragma comment(lib,"Xinput9_1_0.lib")
#endif
*/




	
DWORD Shader; // shader
bool shaderfound = false;

//glbindtexture
int CurrentTexture = 0;

bool free_for_all_player_models;
bool red_team_player_models;
bool blue_team_player_models;


void ColorOn()
{
	glEnable(GL_COLOR_MATERIAL);
	glDisableClientState(GL_COLOR_ARRAY);
	if (free_for_all_player_models)
		glColor4ub(255, 80, 0, 0); //behind walls
	//else if (blue_team_player_models)
		//glColor4ub(0, 255, 125, 0); //behind walls
}

void ColorOff()
{
	glEnable(GL_COLOR_MATERIAL);
	glDisableClientState(GL_COLOR_ARRAY);
	if (free_for_all_player_models)
		glColor4ub(255, 0, 0, 0); //infront of walls
	//else if (blue_team_player_models)
		//glColor4ub(0, 255, 0, 0); //infront of  walls
}

void ColorFunc(int r, int g, int b, int a)
{
	glDisableClientState(GL_COLOR_ARRAY);
	glColor4ub(r, g, b, a);
	glEnable(GL_COLOR_MATERIAL);
}


// =============================================================================================== //

typedef void (WINAPI *glBindTexture_t) (GLenum target, GLuint texture);
typedef void (WINAPI *glDrawElements_t) (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
typedef void (WINAPI *glVertexPointer_t) (GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
typedef void (__stdcall *wglSwapBuffers_t) (HDC hDC);


typedef HWND(WINAPI *CreateWindowExA_t) (
	_In_     DWORD     dwExStyle,
	_In_opt_ LPCSTR    lpClassName,
	_In_opt_ LPCSTR    lpWindowName,
	_In_     DWORD     dwStyle,
	_In_     int       x,
	_In_     int       y,
	_In_     int       nWidth,
	_In_     int       nHeight,
	_In_opt_ HWND      hWndParent,
	_In_opt_ HMENU     hMenu,
	_In_opt_ HINSTANCE hInstance,
	_In_opt_ LPVOID    lpParam
	);



typedef HMODULE(WINAPI *LoadLibraryExA_t) (
	_In_       LPCTSTR lpFileName,
	_Reserved_ HANDLE  hFile,
	_In_       DWORD   dwFlags
);



glBindTexture_t origglBindTexture;
glDrawElements_t origglDrawElements;
glVertexPointer_t origglVertexPointer;
wglSwapBuffers_t origwglSwapBuffers;
CreateWindowExA_t origCreateWindowExA;
LoadLibraryExA_t origLoadLibraryExA;

// =============================================================================================== //


// getdir & log
char dlldir[320];
char* GetDirectoryFile(char *filename)
{
	static char path[320];
	strcpy_s(path, dlldir);
	strcat_s(path, filename);
	return path;
}

void Log(const char *fmt, ...)
{
	if (!fmt)	return;

	char		text[4096];
	va_list		ap;
	va_start(ap, fmt);
	vsprintf_s(text, fmt, ap);
	va_end(ap);

	ofstream logfile(GetDirectoryFile("log.txt"), ios::app);
	if (logfile.is_open() && text)	logfile << text << endl;
	logfile.close();
}


/*
HWND GameWnd = NULL;

void Initialise()
{
	GameWnd = FindWindow(NULL, "Quake 3: Arena");
	if (GameWnd == NULL) GameWnd = GetForegroundWindow();
}
/*/


/*
// The main window handle of the game.
HWND game_hwnd = FindWindowA(0, "Quake 3: Arena");
//HWND game_hwnd = FindWindowA(0, "Star Wars Battlefront II");

// The main window handle of the game.
//HWND game_hwnd = NULL;

// Used to find windows belonging to the game process.
BOOL CALLBACK find_game_hwnd(HWND hwnd, LPARAM game_pid) 
{
	// Skip windows not belonging to the game process.
	DWORD hwnd_pid = NULL;

	GetWindowThreadProcessId(hwnd, &hwnd_pid);

	if (hwnd_pid != game_pid)
		return TRUE;

	// Set the target window handle and stop the callback.
	game_hwnd = hwnd;

	return FALSE;
}
*/

// =============================================================================================== //

