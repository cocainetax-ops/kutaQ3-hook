// =============================================================================================== //
// kutaQ3 hook - DirectInput8 mouse routing - implementation
//
// See dinput8Hook.h for the rationale. Short version: with in_mouse 1 Quake 3 grabs the mouse
// through DirectInput8, which freezes the OS cursor and swallows WM_MOUSEMOVE / button messages,
// so ImGui's Win32 backend never sees the mouse move. We hook the device vtable to read the real
// deltas, feed them to ImGui and hand zeroes back to the game while the menu is up.
// =============================================================================================== //

#include "dinput8Hook.h"

#include <windows.h>
#include "imgui/imgui.h"

// the game window the menu is attached to (owned by main.cpp); used to keep the routed cursor
// inside the client area
extern HWND g_GameHwnd;

// ----------------------------------------------------------------------------------------------- //
// minimal DirectInput8 declarations - we deliberately do not include <dinput.h> / the DirectX SDK.
// DirectInput8 is plain COM: __stdcall methods with the object pointer as the first parameter and a
// single static vtable per interface living in dinput8.dll's read-only data.
// ----------------------------------------------------------------------------------------------- //

// HRESULT success / failure test without pulling in winerror's SUCCEEDED in a weird include order.
#define KUTAQ3_SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)

// IDirectInputDevice8 layout the mouse uses. lX/lY/lZ are 32-bit relative deltas (lZ is the wheel,
// in WHEEL_DELTA units); rgbButtons[] holds one byte per button with the high bit (0x80) set while
// pressed. DIMOUSESTATE is 16 bytes (4 buttons), DIMOUSESTATE2 is 20 bytes (8 buttons).
struct KUTA_DIMOUSESTATE  { long lX; long lY; long lZ; unsigned char rgbButtons[4]; };
struct KUTA_DIMOUSESTATE2 { long lX; long lY; long lZ; unsigned char rgbButtons[8]; };

// one buffered sample returned by GetDeviceData. dwOfs is a field offset into the device state
// (0/4/8 = X/Y/Z, 12..19 = button 0..7); dwData is the delta for an axis or 0x80-masked for a button.
struct KUTA_DIDEVICEOBJECTDATA { unsigned long dwOfs; unsigned long dwData; unsigned long dwTimeStamp; unsigned long dwSequence; };

// GUIDs we need (well-known constants, so no dinput.h / dxguid.lib dependency).
//   GUID_SysMouse            = {6F1D2B60-D5A0-11CF-BFC7-444553540000}
//   IID_IDirectInput8A       = {BF798030-483A-4DA2-AA99-5D64ED369700}  (Quake 3 is a MultiByte/ANSI build)
static const GUID g_GUID_SysMouse      = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID g_IID_IDirectInput8A = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };

// DIRECTINPUT_VERSION for DirectInput 8.
#define KUTAQ3_DIRECTINPUT_VERSION 0x0800

// COM method signatures (x86 COM is __stdcall, with the object pointer as the first argument).
typedef HRESULT (WINAPI  *PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
typedef HRESULT (__stdcall *PFN_DI8_CreateDevice)(void* This, REFGUID rguid, void** ppDevice, IUnknown* pUnkOuter);
typedef ULONG   (__stdcall *PFN_IUnknown_Release)(void* This);
typedef HRESULT (__stdcall *PFN_GetDeviceState)(void* This, DWORD cbData, void* lpvData);
typedef HRESULT (__stdcall *PFN_GetDeviceData)(void* This, DWORD cbObjectData, void* rgdod, DWORD* pdwInOut, DWORD dwFlags);

// vtable slot indices (IUnknown occupies 0/1/2).
//   IDirectInput8::          CreateDevice   = 3
//   IDirectInputDevice8::    GetDeviceState = 9
//   IDirectInputDevice8::    GetDeviceData  = 10
enum { VTBL_DI8_CreateDevice = 3 };
enum { VTBL_DIDev_GetDeviceState = 9, VTBL_DIDev_GetDeviceData = 10 };

// DIMOFS_* field offsets inside the mouse device state.
enum { DIMOFS_X = 0, DIMOFS_Y = 4, DIMOFS_Z = 8, DIMOFS_BUTTON0 = 12, DIMOFS_BUTTON_LAST = 19 };

// ----------------------------------------------------------------------------------------------- //
// module state
// ----------------------------------------------------------------------------------------------- //
static PFN_DirectInput8Create g_pDirectInput8Create = NULL;  // resolved export, never detoured
static PFN_GetDeviceState     g_origGetDeviceState  = NULL;  // saved before the vtable is patched
static PFN_GetDeviceData      g_origGetDeviceData   = NULL;
static void**                  g_pDevVtable          = NULL; // address of the shared device vtable (for Shutdown)
static bool                    g_bInstalled          = false;

static bool  g_bMenuOpen   = false;          // driven by DInput::SetMenuOpen - intercept only while true
static long  g_iCursorX    = 0, g_iCursorY = 0; // ImGui cursor position, in client pixels
static bool  g_bCursorInit = false;            // false => recentre the cursor on the next sample
static bool  g_bMouseBtn[8];                   // last button state we fed to ImGui (edge detection)

// ----------------------------------------------------------------------------------------------- //
// ImGui routing helpers
// ----------------------------------------------------------------------------------------------- //
static bool ImGuiIsReady()
{
	return ImGui::GetCurrentContext() != NULL;
}

// apply a mouse delta to the tracked cursor, clamp it to the game window and push it to ImGui.
static void FeedImGuiMouseDelta(long dx, long dy)
{
	if (!ImGuiIsReady() || (!dx && !dy))
		return;

	RECT rc;
	if (!g_GameHwnd || !GetClientRect(g_GameHwnd, &rc))
		return;

	if (!g_bCursorInit)
	{
		g_iCursorX = (rc.right - rc.left) / 2;
		g_iCursorY = (rc.bottom - rc.top) / 2;
		g_bCursorInit = true;
	}

	g_iCursorX += dx;
	g_iCursorY += dy;
	if (g_iCursorX < rc.left)  g_iCursorX = rc.left;
	if (g_iCursorX > rc.right) g_iCursorX = rc.right;
	if (g_iCursorY < rc.top)    g_iCursorY = rc.top;
	if (g_iCursorY > rc.bottom) g_iCursorY = rc.bottom;

	ImGui::GetIO().AddMousePosEvent((float)g_iCursorX, (float)g_iCursorY);
}

static void FeedImGuiButton(int button, bool down)
{
	if (!ImGuiIsReady())
		return;
	ImGui::GetIO().AddMouseButtonEvent(button, down);
}

static void FeedImGuiWheel(long dz)
{
	if (!ImGuiIsReady() || !dz)
		return;
	// DirectInput reports the wheel in WHEEL_DELTA units; ImGui wants float notches.
	ImGui::GetIO().AddMouseWheelEvent(0.0f, (float)dz / (float)WHEEL_DELTA);
}

// ----------------------------------------------------------------------------------------------- //
// vtable patching
// ----------------------------------------------------------------------------------------------- //
static void PatchVtableEntry(void* comObject, int slot, void* hookFn, void** origOut)
{
	void** vtbl = *(void***)comObject;        // first DWORD of a COM object is its vtable pointer
	if (vtbl[slot] == hookFn)
		return;                                // already patched - do not clobber the saved original

	DWORD oldProt = 0;
	if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &oldProt))
		return;
	if (origOut)
		*origOut = vtbl[slot];
	vtbl[slot] = hookFn;
	DWORD prot = 0;
	VirtualProtect(&vtbl[slot], sizeof(void*), oldProt, &prot);
	FlushInstructionCache(GetCurrentProcess(), &vtbl[slot], sizeof(void*));
}

// ----------------------------------------------------------------------------------------------- //
// hooked IDirectInputDevice8 methods
// ----------------------------------------------------------------------------------------------- //

// Immediate mode: the game asks for the whole current device state in one buffer.
static HRESULT __stdcall hkGetDeviceState(void* This, DWORD cbData, void* lpvData)
{
	HRESULT hr = g_origGetDeviceState ? g_origGetDeviceState(This, cbData, lpvData) : E_FAIL;
	if (!KUTAQ3_SUCCEEDED(hr) || !lpvData)
		return hr;

	// Menu closed - hand the real state straight back to the game untouched.
	if (!g_bMenuOpen)
		return hr;

	// Only treat mouse-sized states as mouse data (16 = DIMOUSESTATE, 20 = DIMOUSESTATE2). A keyboard
	// device state is 256 bytes and a joystick state is larger again, so this keeps us out of those.
	const int nButtons = (cbData >= sizeof(KUTA_DIMOUSESTATE2)) ? 8
	                   : (cbData >= sizeof(KUTA_DIMOUSESTATE))  ? 4 : 0;
	if (!nButtons)
		return hr;

	unsigned char* base = (unsigned char*)lpvData;
	long  dx = *(long*)(base + 0);   // lX
	long  dy = *(long*)(base + 4);   // lY
	long  dz = *(long*)(base + 8);   // lZ (wheel)
	unsigned char* btns = base + 12; // rgbButtons

	FeedImGuiMouseDelta(dx, dy);
	FeedImGuiWheel(dz);
	for (int i = 0; i < nButtons; ++i)
	{
		bool down = (btns[i] & 0x80) != 0;
		if (down != g_bMouseBtn[i])
		{
			FeedImGuiButton(i, down);
			g_bMouseBtn[i] = down;
		}
	}

	// Hand Quake 3 a clean, empty state so it does not look around / fire while the menu is up.
	memset(lpvData, 0, cbData);
	return hr;
}

// Buffered mode: the game drains one DIDEVICEOBJECTDATA record per event since the last poll.
static HRESULT __stdcall hkGetDeviceData(void* This, DWORD cbObjectData, void* rgdod, DWORD* pdwInOut, DWORD dwFlags)
{
	HRESULT hr = g_origGetDeviceData ? g_origGetDeviceData(This, cbObjectData, rgdod, pdwInOut, dwFlags) : E_FAIL;
	if (!KUTAQ3_SUCCEEDED(hr) || !rgdod || !pdwInOut || *pdwInOut == 0)
		return hr;

	// Menu closed - hand the real samples straight back to the game untouched.
	if (!g_bMenuOpen)
		return hr;

	const DWORD count = *pdwInOut;
	long dx = 0, dy = 0, dz = 0;

	for (DWORD i = 0; i < count; ++i)
	{
		KUTA_DIDEVICEOBJECTDATA* od = (KUTA_DIDEVICEOBJECTDATA*)((unsigned char*)rgdod + i * cbObjectData);
		const unsigned long ofs = od->dwOfs;
		const unsigned long dat = od->dwData;

		if (ofs == DIMOFS_X)
			dx += (long)dat;
		else if (ofs == DIMOFS_Y)
			dy += (long)dat;
		else if (ofs == DIMOFS_Z)
			dz += (long)dat;
		else if (ofs >= DIMOFS_BUTTON0 && ofs <= DIMOFS_BUTTON_LAST)
		{
			int btn = (int)(ofs - DIMOFS_BUTTON0);
			bool down = (dat & 0x80) != 0;
			if (down != g_bMouseBtn[btn])
			{
				FeedImGuiButton(btn, down);
				g_bMouseBtn[btn] = down;
			}
		}

		// Strip the sample from what the game will read (movement and clicks alike).
		od->dwData = 0;
	}

	FeedImGuiMouseDelta(dx, dy);
	FeedImGuiWheel(dz);

	return hr;
}

// ----------------------------------------------------------------------------------------------- //
// public API
// ----------------------------------------------------------------------------------------------- //
void DInput::SetMenuOpen(bool open)
{
	if (open && !g_bMenuOpen)
	{
		// just opened - place the cursor at the centre of the game window right away (so it is
		// visible before the first movement) and forget any button state we tracked while closed
		RECT rc;
		if (g_GameHwnd && GetClientRect(g_GameHwnd, &rc))
		{
			g_iCursorX = (rc.right - rc.left) / 2;
			g_iCursorY = (rc.bottom - rc.top) / 2;
			g_bCursorInit = true;
		}
		else
		{
			g_bCursorInit = false;
		}
		for (int i = 0; i < 8; ++i)
			g_bMouseBtn[i] = false;
	}
	g_bMenuOpen = open;
}

void DInput::RefeedMousePos()
{
	if (!g_bMenuOpen || !ImGuiIsReady() || !g_bCursorInit)
		return;
	ImGui::GetIO().AddMousePosEvent((float)g_iCursorX, (float)g_iCursorY);
}

bool DInput::Install()
{
	if (g_bInstalled)
		return true;

	HMODULE hDInput = GetModuleHandleA("dinput8.dll");
	if (!hDInput)
		return false; // not loaded yet - retry next frame (Quake 3 has it resident by first swap)

	g_pDirectInput8Create = (PFN_DirectInput8Create)GetProcAddress(hDInput, "DirectInput8Create");
	if (!g_pDirectInput8Create)
	{
		g_bInstalled = true; // dinput8 present without the export - nothing to hook, stop retrying
		return true;
	}

	// Create our own throwaway IDirectInput8 + system mouse device purely to read the address of the
	// shared IDirectInputDevice8 vtable out of it. Every device instance points at that one static
	// vtable, so patching it once reaches the game's mouse device too, regardless of when it was
	// created. DirectInput8Create does not require prior CoInitialize().
	void* pDI = NULL;
	HRESULT hr = g_pDirectInput8Create(GetModuleHandleA(NULL), KUTAQ3_DIRECTINPUT_VERSION,
	                                   g_IID_IDirectInput8A, &pDI, NULL);
	if (!KUTAQ3_SUCCEEDED(hr) || !pDI)
		return false;

	void** vtblDI8 = *(void***)pDI;
	PFN_DI8_CreateDevice pCreateDevice = (PFN_DI8_CreateDevice)vtblDI8[VTBL_DI8_CreateDevice];

	void* pDev = NULL;
	hr = pCreateDevice(pDI, g_GUID_SysMouse, &pDev, NULL);
	if (KUTAQ3_SUCCEEDED(hr) && pDev)
	{
		g_pDevVtable = *(void***)pDev; // remember the shared vtable address so Shutdown can restore it
		PatchVtableEntry(pDev, VTBL_DIDev_GetDeviceState, (void*)hkGetDeviceState, (void**)&g_origGetDeviceState);
		PatchVtableEntry(pDev, VTBL_DIDev_GetDeviceData,  (void*)hkGetDeviceData,  (void**)&g_origGetDeviceData);

		PFN_IUnknown_Release pRelease = (PFN_IUnknown_Release)(*(void***)pDev)[2];
		pRelease(pDev); // our throwaway device goes away; the patched static vtable stays
	}

	PFN_IUnknown_Release pReleaseDI = (PFN_IUnknown_Release)vtblDI8[2];
	pReleaseDI(pDI);

	g_bInstalled = true;
	return true;
}

void DInput::Shutdown()
{
	if (!g_bInstalled)
		return;

	// Put the real DirectInput methods back so the game keeps working after this DLL is freed.
	if (g_pDevVtable)
	{
		DWORD oldProt = 0;
		if (VirtualProtect(g_pDevVtable + VTBL_DIDev_GetDeviceState, sizeof(void*) * 2, PAGE_READWRITE, &oldProt))
		{
			if (g_origGetDeviceState)
				g_pDevVtable[VTBL_DIDev_GetDeviceState] = (void*)g_origGetDeviceState;
			if (g_origGetDeviceData)
				g_pDevVtable[VTBL_DIDev_GetDeviceData] = (void*)g_origGetDeviceData;
			DWORD prot = 0;
			VirtualProtect(g_pDevVtable + VTBL_DIDev_GetDeviceState, sizeof(void*) * 2, oldProt, &prot);
		}
		g_pDevVtable = NULL;
	}
	g_origGetDeviceState = NULL;
	g_origGetDeviceData = NULL;
	g_bInstalled = false;
	g_bMenuOpen = false;
}
