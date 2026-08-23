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




	
	// NOTE: these are only DECLARED here (extern). The single definitions live in main.cpp.
	// Defining them in this header produced LNK2005 "already defined in main.obj" because
	// every .cpp that includes main.h (main.cpp, config.cpp) emitted its own copy.
	extern DWORD Shader; // shader
	extern bool shaderfound;

	//glbindtexture
	extern int CurrentTexture;

	extern bool free_for_all_player_models;
	extern bool red_team_player_models;
	extern bool blue_team_player_models;


	// declarations only - bodies are defined once in main.cpp
	void ColorOn();
	void ColorOff();
	void ColorFunc(int r, int g, int b, int a);


// =============================================================================================== //

typedef void (WINAPI *glBindTexture_t) (GLenum target, GLuint texture);
typedef void (WINAPI *glDrawElements_t) (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
typedef void (WINAPI *glVertexPointer_t) (GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
typedef BOOL (WINAPI *SwapBuffers_t) (HDC hDC);


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



extern glBindTexture_t origglBindTexture;
extern glDrawElements_t origglDrawElements;
extern glVertexPointer_t origglVertexPointer;
extern SwapBuffers_t origwglSwapBuffers;
extern CreateWindowExA_t origCreateWindowExA;
extern LoadLibraryExA_t origLoadLibraryExA;

// =============================================================================================== //


// getdir & log - declarations only. Single definitions live in main.cpp
extern char dlldir[320];
char* GetDirectoryFile(char *filename);
void Log(const char *fmt, ...);


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

