// =============================================================================================== //
// kutaQ3 hook - DirectInput8 mouse routing
//
// Why this exists:
//   Quake 3 with in_mouse 1 reads the mouse through DirectInput8 (IDirectInputDevice8). While that
//   device is acquired (foreground/exclusive) the OS cursor is frozen and NO WM_MOUSEMOVE / mouse
//   button messages reach the game window - DirectInput consumes them at the HID level. Dear ImGui's
//   Win32 backend only ever learns about the cursor through those window messages, so while the
//   device is grabbed the ImGui cursor never moves: "when IMGUI menu is open I cannot move the mouse
//   cursor to control IMGUI menu".
//
//   Hooking GetCursorPos (the Win32 mouse path) does nothing here because the game never calls it -
//   it polls IDirectInputDevice8::GetDeviceState / ::GetDeviceData directly for relative deltas.
//
// What this does:
//   - Resolves the one shared IDirectInputDevice8 vtable inside dinput8.dll (every device instance
//     in the process points at it) and patches two entries on it - GetDeviceState (vtable[9]) and
//     GetDeviceData (vtable[10]) - using VirtualProtect.
//   - While the kutaQ3 menu is open it reads the real mouse deltas / wheel / buttons out of those
//     calls, feeds them into Dear ImGui (AddMousePosEvent / AddMouseButtonEvent / AddMouseWheelEvent)
//     so the menu cursor moves and clicks register, and then ZEROES the deltas / buttons before they
//     are returned to Quake 3 so the game does not look around / shoot while you drive the menu.
//   - While the menu is closed the calls pass straight through to DirectInput untouched.
//
//   The shared vtable is patched by creating our own throwaway IDirectInput8 + mouse device purely
//   to read the vtable address out of it, so this works no matter whether the game created its mouse
//   device before or after the hook is installed (same static vtable either way).
//
//   Quake 3 drives only the mouse through DirectInput8 (the keyboard stays on Win32 messages), so
//   intercepting the shared device vtable is safe; GetDeviceState additionally self-filters on the
//   data format size so it only ever treats 16/20-byte mouse states as mouse data.
// =============================================================================================== //
#pragma once

namespace DInput
{
	// Patch the IDirectInputDevice8 vtable the first time dinput8.dll is resident in the process.
	// Safe to call every frame - it is a no-op once already installed. Returns true once installed.
	bool Install();

	// Restore the original vtable entries. Called on DLL unload so Quake 3 never ends up calling into
	// unmapped hook code after this DLL is freed.
	void Shutdown();

	// Driven from the SwapBuffers hook each frame. When true, the GetDeviceState / GetDeviceData
	// detours read the real mouse data, route it into ImGui and return zeroes to Quake 3.
	void SetMenuOpen(bool open);

	// Re-queue the routed cursor position to ImGui. MUST be called AFTER ImGui_ImplWin32_NewFrame()
	// and before ImGui::NewFrame(): the Win32 backend re-feeds the (DirectInput-frozen) OS cursor
	// from GetCursorPos() every NewFrame when the mouse is not tracked via WM_MOUSEMOVE - which is
	// always the case under DirectInput - so our routed position has to be the LAST mouse-position
	// event in the queue or the frozen one wins.
	void RefeedMousePos();
}
