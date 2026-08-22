// =============================================================================================== //
// kutaQ3 hook - legacy DirectInput mouse routing - implementation
//
// See dinputHook.h for the rationale. Short version: with in_mouse 1 retail Quake 3 (1.32) grabs
// the mouse through the legacy DirectInput path (dinput.dll: DirectInputCreate ->
// IDirectInput::CreateDevice -> IDirectInputDevice), which freezes the OS cursor and swallows
// WM_MOUSEMOVE / button messages, so ImGui's Win32 backend never sees the mouse move. We hook the
// legacy device vtables to read the real deltas, feed them to ImGui and hand zeroes back to the
// game while the menu is up.
// =============================================================================================== //

#include "dinputHook.h"

#include <windows.h>
#include <string.h>
#include "imgui/imgui.h"

// the game window the menu is attached to (owned by main.cpp); used to keep the routed cursor
// inside the client area
extern HWND g_GameHwnd;

// ----------------------------------------------------------------------------------------------- //
// minimal legacy DirectInput declarations - we deliberately do not include <dinput.h> / the DirectX
// SDK. Legacy DirectInput is plain COM too: __stdcall methods with the object pointer as the first
// parameter and one static vtable per interface generation living in dinput.dll's read-only data.
// ----------------------------------------------------------------------------------------------- //

// HRESULT success / failure test without pulling in winerror's SUCCEEDED in a weird include order.
#define KUTAQ3_SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)

// IDirectInputDevice mouse state layouts. lX/lY/lZ are 32-bit relative deltas (lZ is the wheel, in
// WHEEL_DELTA units); rgbButtons[] holds one byte per button with the high bit (0x80) set while
// pressed. DIMOUSESTATE is 16 bytes (4 buttons), DIMOUSESTATE2 is 20 bytes (8 buttons).
struct KUTA_DIMOUSESTATE  { long lX; long lY; long lZ; unsigned char rgbButtons[4]; };
struct KUTA_DIMOUSESTATE2 { long lX; long lY; long lZ; unsigned char rgbButtons[8]; };

// one buffered sample returned by GetDeviceData. dwOfs is a field offset into the device state
// (0/4/8 = X/Y/Z, 12..19 = button 0..7); dwData is the delta for an axis or 0x80-masked for a
// button. Identical layout in the legacy interface and in DirectInput 8.
struct KUTA_DIDEVICEOBJECTDATA { unsigned long dwOfs; unsigned long dwData; unsigned long dwTimeStamp; unsigned long dwSequence; };

// GUID_SysMouse = {6F1D2B60-D5A0-11CF-BFC7-444553540000} (well-known constant, no dxguid.lib).
static const GUID g_GUID_SysMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

// DIRECTINPUT_VERSION constants the retail executable may have been compiled against. dinput.dll
// keeps a separate static device vtable per interface generation, so we probe each version and
// patch every distinct vtable we get back.
static const unsigned long g_diVersions[] = { 0x0300, 0x0500, 0x05A0, 0x0700 };

// COM method signatures (x86 COM is __stdcall, with the object pointer as the first argument).
typedef HRESULT (WINAPI    *PFN_DirectInputCreate)(HINSTANCE, DWORD, void**, IUnknown*);
typedef HRESULT (__stdcall *PFN_DI_CreateDevice)(void* This, const GUID* rguid, void** ppDevice, IUnknown* pUnkOuter);
typedef ULONG   (__stdcall *PFN_IUnknown_Release)(void* This);
typedef HRESULT (__stdcall *PFN_GetDeviceState)(void* This, DWORD cbData, void* lpvData);
typedef HRESULT (__stdcall *PFN_GetDeviceData)(void* This, DWORD cbObjectData, void* rgdod, DWORD* pdwInOut, DWORD dwFlags);
typedef HRESULT (__stdcall *PFN_SetDataFormat)(void* This, const void* lpdfDIDF);

// vtable slot indices (IUnknown occupies 0/1/2 in both interfaces).
//   IDirectInput::        CreateDevice  = 3   (identical in IDirectInput/2/7 - methods are appended)
//   IDirectInputDevice::  GetDeviceState = 9
//   IDirectInputDevice::  GetDeviceData  = 10
//   IDirectInputDevice::  SetDataFormat  = 11 (identical in IDirectInputDevice/2/5/7)
enum { VTBL_DI_CreateDevice = 3 };
enum { VTBL_DIDev_GetDeviceState = 9, VTBL_DIDev_GetDeviceData = 10, VTBL_DIDev_SetDataFormat = 11 };

// DIMOFS_* field offsets inside the mouse device state.
enum { DIMOFS_X = 0, DIMOFS_Y = 4, DIMOFS_Z = 8, DIMOFS_BUTTON0 = 12, DIMOFS_BUTTON_LAST = 19 };

// DIDATAFORMAT::dwDataSize offset - the only member we need from the format the game installs.
enum { DIDATAFORMAT_dwDataSize_OFFSET = 12 };

// ----------------------------------------------------------------------------------------------- //
// module state
// ----------------------------------------------------------------------------------------------- //
static PFN_DirectInputCreate g_pDirectInputCreate = NULL;  // resolved export, never detoured
static bool                  g_bInstalled         = false;

// one record per distinct legacy device vtable we patched (dinput.dll has one per interface
// generation, so a handful at most).
struct KUTA_PatchedVtable
{
	void**             vtbl;
	PFN_GetDeviceState origGetDeviceState;
	PFN_GetDeviceData  origGetDeviceData;
	PFN_SetDataFormat  origSetDataFormat;
};
static KUTA_PatchedVtable g_patched[8];
static int                g_nPatched = 0;

// device instances seen installing a mouse-sized data format (SetDataFormat self-report). The
// legacy device vtable is shared by mouse / keyboard / joystick devices alike, so this keeps the
// GetDeviceData hook out of non-mouse devices when they exist.
static void* g_mouseDevices[16];
static int   g_nMouseDevices = 0;

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
static void PatchVtableEntry(void** vtbl, int slot, void* hookFn, void** origOut)
{
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

static KUTA_PatchedVtable* FindPatched(void** vtbl)
{
	for (int i = 0; i < g_nPatched; ++i)
		if (g_patched[i].vtbl == vtbl)
			return &g_patched[i];
	return NULL;
}

// ----------------------------------------------------------------------------------------------- //
// device tracking helpers
// ----------------------------------------------------------------------------------------------- //
static void TrackMouseDevice(void* dev)
{
	for (int i = 0; i < g_nMouseDevices; ++i)
		if (g_mouseDevices[i] == dev)
			return;
	if (g_nMouseDevices < (int)(sizeof(g_mouseDevices) / sizeof(g_mouseDevices[0])))
		g_mouseDevices[g_nMouseDevices++] = dev;
}

static bool IsTrackedMouseDevice(void* dev)
{
	for (int i = 0; i < g_nMouseDevices; ++i)
		if (g_mouseDevices[i] == dev)
			return true;
	return false;
}

// ----------------------------------------------------------------------------------------------- //
// hooked IDirectInputDevice methods
// ----------------------------------------------------------------------------------------------- //

// Immediate mode: the game asks for the whole current device state in one buffer. Retail Quake 3
// calls this once per frame (with a 16-byte DIMOUSESTATE) after draining the buffered samples and
// uses lX / lY as the raw look deltas.
static HRESULT __stdcall hkGetDeviceState(void* This, DWORD cbData, void* lpvData)
{
	KUTA_PatchedVtable* rec = FindPatched(*(void***)This);
	PFN_GetDeviceState orig = rec ? rec->origGetDeviceState : NULL;
	HRESULT hr = orig ? orig(This, cbData, lpvData) : E_FAIL;
	if (!KUTAQ3_SUCCEEDED(hr) || !lpvData)
		return hr;

	// Menu closed - hand the real state straight back to the game untouched.
	if (!g_bMenuOpen)
		return hr;

	// Only treat mouse-sized states as mouse data (16 = DIMOUSESTATE, 20 = DIMOUSESTATE2). A
	// keyboard device state is 256 bytes and a joystick state is larger again, so this keeps us
	// out of those.
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

	// Hand Quake 3 a clean, empty state so it does not look around while the menu is up.
	memset(lpvData, 0, cbData);
	return hr;
}

// Buffered mode: the game drains one DIDEVICEOBJECTDATA record per event since the last poll.
// Retail Quake 3 loops over this with dwElements = 1 to pick up mouse buttons and the wheel.
static HRESULT __stdcall hkGetDeviceData(void* This, DWORD cbObjectData, void* rgdod, DWORD* pdwInOut, DWORD dwFlags)
{
	KUTA_PatchedVtable* rec = FindPatched(*(void***)This);
	PFN_GetDeviceData orig = rec ? rec->origGetDeviceData : NULL;
	HRESULT hr = orig ? orig(This, cbObjectData, rgdod, pdwInOut, dwFlags) : E_FAIL;
	if (!KUTAQ3_SUCCEEDED(hr) || !rgdod || !pdwInOut || *pdwInOut == 0)
		return hr;

	// Menu closed - hand the real samples straight back to the game untouched.
	if (!g_bMenuOpen)
		return hr;

	// The legacy device vtable is shared by every device type. Only touch devices that installed a
	// mouse-sized data format (tracked by the SetDataFormat hook below). Devices we never saw set a
	// format (e.g. created before the hook went in) are treated as the mouse - retail Quake 3 only
	// ever drives the mouse through DirectInput, the keyboard stays on Win32 messages.
	if (g_nMouseDevices > 0 && !IsTrackedMouseDevice(This))
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
			if (btn < 8)
			{
				bool down = (dat & 0x80) != 0;
				if (down != g_bMouseBtn[btn])
				{
					FeedImGuiButton(btn, down);
					g_bMouseBtn[btn] = down;
				}
			}
		}

		// Strip the sample from what the game will read (movement and clicks alike).
		od->dwData = 0;
	}

	FeedImGuiMouseDelta(dx, dy);
	FeedImGuiWheel(dz);

	return hr;
}

// Tracks which device instances run in mouse data format. DIDATAFORMAT::dwDataSize sits 12 bytes
// into the structure; 16 = DIMOUSESTATE, 20 = DIMOUSESTATE2. Everything else (keyboard 256, ...)
// simply is not tracked, which keeps the GetDeviceData hook out of it.
static HRESULT __stdcall hkSetDataFormat(void* This, const void* lpdfDIDF)
{
	KUTA_PatchedVtable* rec = FindPatched(*(void***)This);
	PFN_SetDataFormat orig = rec ? rec->origSetDataFormat : NULL;
	HRESULT hr = orig ? orig(This, lpdfDIDF) : E_FAIL;

	if (KUTAQ3_SUCCEEDED(hr) && lpdfDIDF)
	{
		const unsigned long dataSize = *(const unsigned long*)((const unsigned char*)lpdfDIDF + DIDATAFORMAT_dwDataSize_OFFSET);
		if (dataSize == sizeof(KUTA_DIMOUSESTATE) || dataSize == sizeof(KUTA_DIMOUSESTATE2))
			TrackMouseDevice(This);
	}
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

	HMODULE hDInput = GetModuleHandleA("dinput.dll");
	if (!hDInput)
		return false; // not loaded yet - retry next frame (Quake 3 has it resident by first swap)

	// retail Quake 3 resolves "DirectInputCreateA" itself; fall back to the undecorated name just
	// in case some loader exposes that one instead
	g_pDirectInputCreate = (PFN_DirectInputCreate)GetProcAddress(hDInput, "DirectInputCreateA");
	if (!g_pDirectInputCreate)
		g_pDirectInputCreate = (PFN_DirectInputCreate)GetProcAddress(hDInput, "DirectInputCreate");
	if (!g_pDirectInputCreate)
	{
		g_bInstalled = true; // dinput present without the export - nothing to hook, stop retrying
		return true;
	}

	// Create our own throwaway IDirectInput + system mouse device for every legacy
	// DIRECTINPUT_VERSION the retail executable may have been compiled against, purely to read the
	// address of each shared IDirectInputDevice vtable out of it. Every device instance of a given
	// generation points at one static vtable, so patching it once reaches the game's mouse device
	// too, regardless of when it was created. DirectInputCreate does not require prior
	// CoInitialize().
	for (int v = 0; v < (int)(sizeof(g_diVersions) / sizeof(g_diVersions[0])); ++v)
	{
		void* pDI = NULL;
		HRESULT hr = g_pDirectInputCreate(GetModuleHandleA(NULL), g_diVersions[v], &pDI, NULL);
		if (!KUTAQ3_SUCCEEDED(hr) || !pDI)
			continue;

		void** vtblDI = *(void***)pDI;
		PFN_DI_CreateDevice pCreateDevice = (PFN_DI_CreateDevice)vtblDI[VTBL_DI_CreateDevice];

		void* pDev = NULL;
		hr = pCreateDevice(pDI, &g_GUID_SysMouse, &pDev, NULL);
		if (KUTAQ3_SUCCEEDED(hr) && pDev)
		{
			void** vtbl = *(void***)pDev;
			if (!FindPatched(vtbl) && g_nPatched < (int)(sizeof(g_patched) / sizeof(g_patched[0])))
			{
				// remember the shared vtable address + originals so Shutdown can restore it
				KUTA_PatchedVtable* rec = &g_patched[g_nPatched++];
				rec->vtbl              = vtbl;
				rec->origGetDeviceState = NULL;
				rec->origGetDeviceData  = NULL;
				rec->origSetDataFormat  = NULL;

				PatchVtableEntry(vtbl, VTBL_DIDev_GetDeviceState, (void*)hkGetDeviceState, (void**)&rec->origGetDeviceState);
				PatchVtableEntry(vtbl, VTBL_DIDev_GetDeviceData,  (void*)hkGetDeviceData,  (void**)&rec->origGetDeviceData);
				PatchVtableEntry(vtbl, VTBL_DIDev_SetDataFormat,  (void*)hkSetDataFormat,  (void**)&rec->origSetDataFormat);
			}

			PFN_IUnknown_Release pRelease = (PFN_IUnknown_Release)(*(void***)pDev)[2];
			pRelease(pDev); // our throwaway device goes away; the patched static vtable stays
		}

		PFN_IUnknown_Release pReleaseDI = (PFN_IUnknown_Release)vtblDI[2];
		pReleaseDI(pDI);
	}

	g_bInstalled = true;
	return true;
}

void DInput::Shutdown()
{
	if (!g_bInstalled)
		return;

	// Put the real DirectInput methods back so the game keeps working after this DLL is freed.
	for (int i = 0; i < g_nPatched; ++i)
	{
		KUTA_PatchedVtable* rec = &g_patched[i];
		if (!rec->vtbl)
			continue;

		DWORD oldProt = 0;
		if (VirtualProtect(rec->vtbl + VTBL_DIDev_GetDeviceState, sizeof(void*) * 3, PAGE_READWRITE, &oldProt))
		{
			if (rec->origGetDeviceState)
				rec->vtbl[VTBL_DIDev_GetDeviceState] = (void*)rec->origGetDeviceState;
			if (rec->origGetDeviceData)
				rec->vtbl[VTBL_DIDev_GetDeviceData] = (void*)rec->origGetDeviceData;
			if (rec->origSetDataFormat)
				rec->vtbl[VTBL_DIDev_SetDataFormat] = (void*)rec->origSetDataFormat;
			DWORD prot = 0;
			VirtualProtect(rec->vtbl + VTBL_DIDev_GetDeviceState, sizeof(void*) * 3, oldProt, &prot);
		}
		rec->vtbl = NULL;
	}
	g_nPatched = 0;
	g_nMouseDevices = 0;
	g_bInstalled = false;
	g_bMenuOpen = false;
}
