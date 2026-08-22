// =============================================================================================== //
// kutaQ3 hook - legacy DirectInput mouse routing
//
// Why this exists:
//   Quake 3 Arena original retail 1.32 quake3.exe reads the mouse through the LEGACY DirectInput
//   path in dinput.dll: LoadLibrary("dinput.dll") -> GetProcAddress("DirectInputCreateA") ->
//   DirectInputCreate(hInstance, DIRECTINPUT_VERSION, &IDirectInput, NULL) ->
//   IDirectInput::CreateDevice(&GUID_SysMouse, &IDirectInputDevice, NULL). It is NOT the DirectInput8
//   (dinput8.dll / IDirectInputDevice8) interface - a vtable hook on IDirectInputDevice8 would never
//   be called by the retail executable. While the legacy device is acquired (DISCL_EXCLUSIVE |
//   DISCL_FOREGROUND) the OS cursor is frozen and NO WM_MOUSEMOVE / mouse button messages reach the
//   game window - DirectInput consumes them at the HID level. Dear ImGui's Win32 backend only ever
//   learns about the cursor through those window messages, so while the device is grabbed the ImGui
//   cursor never moves: "when IMGUI menu is open I cannot move the mouse cursor to control IMGUI
//   menu". Hooking GetCursorPos (the in_mouse -1 Win32 path) does nothing here because the game
//   never calls it - it drains IDirectInputDevice::GetDeviceData one sample at a time and then reads
//   the remaining counters through IDirectInputDevice::GetDeviceState every frame.
//
// What this does:
//   - Resolves the shared IDirectInputDevice vtables inside dinput.dll and patches three entries on
//     each of them - GetDeviceState (vtable[9]), GetDeviceData (vtable[10]) and SetDataFormat
//     (vtable[11]) - using VirtualProtect. The legacy device vtable is shared by mouse, keyboard and
//     joystick devices alike (unlike DirectInput8, which the game only ever uses for the mouse), so
//     the SetDataFormat hook additionally tracks which device instances run in mouse data format;
//     only those get their data routed / suppressed, keyboards and joysticks pass through untouched.
//   - dinput.dll keeps a separate static vtable per device interface generation, and the retail
//     executable may have been compiled against any legacy DIRECTINPUT_VERSION (0x0300 / 0x0500 /
//     0x05A0 / 0x0700), so Install() creates throwaway DirectInput + system mouse devices for each
//     of those versions and patches every distinct vtable it finds. IDirectInput::CreateDevice is
//     vtable[3] and the GetDeviceState / GetDeviceData / SetDataFormat slots are identical across
//     all generations, so one set of hooks covers them all.
//   - While the kutaQ3 menu is open the hooks read the real mouse deltas / wheel / buttons out of
//     those calls, feed them into Dear ImGui (AddMousePosEvent / AddMouseButtonEvent /
//     AddMouseWheelEvent) so the menu cursor moves and clicks register, and then ZERO the deltas /
//     buttons before they are returned to Quake 3 so the game does not look around / shoot while you
//     drive the menu. While the menu is closed the calls pass straight through to DirectInput
//     untouched.
//
//   The vtables are patched by creating our own throwaway IDirectInput + mouse devices purely to
//   read the vtable addresses out of them, so this works no matter whether the game created its
//   mouse device before or after the hook is installed (static vtables either way).
//
//   Quake 3 drives only the mouse through DirectInput (the keyboard stays on Win32 messages), so
//   intercepting the shared device vtables is safe; GetDeviceState additionally self-filters on the
//   data format size so it only ever treats 16/20-byte mouse states as mouse data.
// =============================================================================================== //
#pragma once

namespace DInput
{
	// Patch the IDirectInputDevice vtables the first time dinput.dll is resident in the process.
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
