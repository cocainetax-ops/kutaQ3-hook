# kutaQ3 hook

OpenGL hook DLL for Quake III Arena with a Dear ImGui in-game menu.

The DLL hooks `SwapBuffers` (with a `wglSwapBuffers` fallback), `glBindTexture`, `glDrawElements`,
`glVertexPointer`, `CreateWindowExA` and `LoadLibraryExA` with Microsoft Detours. The **"kutaQ3 hook"** menu
is rendered on top of the game every frame using Dear ImGui (the bloat-free immediate mode
GUI for C++) with the fixed-function OpenGL2 backend (`imgui/imgui_impl_opengl2.cpp` +
`imgui/imgui_impl_opengl2.h`), which fits Quake 3's legacy GL context.

## Legacy DirectInput mouse routing (in_mouse 1)

With its default `in_mouse 1`, retail Quake 3 Arena 1.32 reads the mouse through the **legacy**
DirectInput path in `dinput.dll`: `LoadLibrary("dinput.dll")` → `DirectInputCreateA` →
`IDirectInput::CreateDevice(&GUID_SysMouse, ...)` → `IDirectInputDevice`. It is *not* the
DirectInput8 (`dinput8.dll` / `IDirectInputDevice8`) interface, so a vtable hook on the DirectInput
8 device interface would never fire. While that legacy device is acquired (foreground/exclusive) the
OS cursor is frozen and **no** `WM_MOUSEMOVE` / mouse-button messages reach the game window -
DirectInput consumes them at the HID level. The ImGui Win32 backend only ever learns about the
cursor from those window messages, so with the menu open the cursor would be stuck in place.
Hooking `GetCursorPos` (the `in_mouse -1` Win32 path) is useless here because the game never calls
it.

`dinputHook.h` / `dinputHook.cpp` solve this by hooking the **legacy device vtable**:

- `Install()` resolves `dinput.dll` and `DirectInputCreateA`, then creates throwaway `IDirectInput`
  + system mouse devices for each legacy `DIRECTINPUT_VERSION` the retail executable may have been
  compiled against (0x0300 / 0x0500 / 0x05A0 / 0x0700 - `dinput.dll` keeps one static device vtable
  per interface generation) purely to read the shared vtable addresses out of them. On each distinct
  vtable it patches three entries with `VirtualProtect`: `GetDeviceState` (vtable[9]),
  `GetDeviceData` (vtable[10]) and `SetDataFormat` (vtable[11]). Patching the shared static vtables
  reaches the game's own mouse device regardless of when it was created, so it works for both early
  and late injection. No DirectX SDK headers or `dxguid.lib` are needed - the COM signatures and
  GUIDs are declared locally.
- Unlike DirectInput 8, the legacy device vtable is shared by mouse, keyboard and joystick devices
  alike. The `SetDataFormat` hook therefore tracks which device instances install a mouse-sized
  format (16-byte `DIMOUSESTATE` / 20-byte `DIMOUSESTATE2`); only those are routed/suppressed.
  `GetDeviceState` additionally self-filters on the buffer size, so non-mouse devices are left
  untouched.
- While the menu is open, the hooks read the real mouse deltas / wheel / buttons out of the device
  data, route them into ImGui (`AddMousePosEvent` / `AddMouseButtonEvent` / `AddMouseWheelEvent`)
  and then **zero** the data before returning it to Quake 3 - so the game neither looks around nor
  fires while you drive the menu. While the menu is closed, both calls pass straight through
  untouched (in-game mouse is unaffected).
- One subtlety: the ImGui Win32 backend re-feeds the (frozen) OS cursor from `GetCursorPos` inside
  `ImGui_ImplWin32_NewFrame()` whenever the mouse is not tracked via `WM_MOUSEMOVE` - which is always
  under DirectInput. So `DInput::RefeedMousePos()` re-queues the routed position **after** the
  backend's `NewFrame` and before `ImGui::NewFrame()` consumes the input queue, making the routed
  position the last (winning) mouse-position event of the frame.
- `Shutdown()` restores the original vtable entries on DLL unload so the game never calls into freed
  hook code.

The keyboard is unaffected - Quake 3 keeps the keyboard on Win32 messages, so the existing
`INSERT` toggle and `F10` shader logging keep working.

## Legacy GL state guard (flicker fix)

Quake 3 drives a legacy OpenGL 1.1 fixed-function pipeline and keeps its **own shadow copy** of the
GL state (`glState` / `GL_State()`). The ImGui OpenGL2 backend only backs up
`GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT` plus a handful of manual `glGet`s, so
everything else it changes is still changed when the game draws its next frame:

- active / client-active texture unit, and the bindings, enables, env mode and texture matrices of
  units 1+ (Quake 3 is a multitexture renderer)
- vertex / colour / texcoord array pointers and their enables
- `GL_ALPHA_TEST`, `GL_FOG`, `glDepthMask`, `glColorMask`, polygon offset, line stipple, clip planes
- a bound VBO or GLSL program on ioquake3-style renderers

Quake 3 only notices and repairs those on the *following* frame, which is the one-frame-wrong /
one-frame-right ping-pong that shows up as flickering while the menu is open.

`glStateGuard.h` / `glStateGuard.cpp` add `GL::LegacyStateGuard`, a scoped RAII guard used via the
`KUTAQ3_LEGACY_GL_STATE_GUARD()` macro. It wraps the whole ImGui frame inside the hooked
`SwapBuffers` (plus backend init and shutdown) and:

1. captures the full legacy state (server + client attribute stacks, all matrices, per-texture-unit
   state, bound buffers/program),
2. neutralises the state the OpenGL2 backend assumes but never sets itself,
3. restores everything exactly when it goes out of scope,
4. drains `glGetError()` so Quake 3's own `GL_CheckErrors()` stays quiet.

While a guard is alive, `GL::LegacyStateGuard::IsActive()` is true and the hooked `glBindTexture` /
`glDrawElements` detours pass ImGui's own draw calls straight through to the originals (no chams,
no shader sniffing). Multitexture / buffer / program entry points are resolved lazily through
`wglGetProcAddress` and simply skipped on a pure GL 1.1 context.

## Features

- Chams (wallhack) on player models - FFA, red team and blue team models
  - Solid style (flat colour behind walls / flat colour in front of walls)
  - Wireframe style (wireframe outline behind walls / solid colour in front of walls)
- **Neon** bloom chams (`neonChams.h`) - a separate feature on the same player-model detection.
  Toggled with the **Neon** button in the VISUALS tab. Three slightly inflated silhouette passes
  are drawn with the depth test off and **additive blending** (`GL_SRC_ALPHA, GL_ONE`), so the
  layered passes accumulate into an intense neon brightness halo that bleeds through walls, topped
  by a white-hot depth-tested core. A `timeGetTime()` pulse makes the glow breathe. Fixed-function
  GL 1.1 has no shaders/FBOs, so this additive over-draw trick is the era-correct "bloom".
  While enabled it overrides the Solid/Wireframe styles (`NeonEnabled` in `kutaQ3.cfg`).
- Player shader logger - hold `F10` in-game to dump player model shader names to `log.txt`
- Dear ImGui menu window called **"kutaQ3 hook"**
  - `INSERT` toggles the menu
  - Mouse/keyboard input is captured while the menu is open
  - **Save settings** / **Load settings** persist cheat options to a dedicated `kutaQ3.cfg`
    next to the DLL (not `imgui.ini` - that file is only ImGui's own window-layout cache
    and stays disabled). Settings are also loaded on inject and written on unload.
  - No ImGui files written to the game folder (`imgui.ini` / `imgui_log.txt` disabled)

## Building

1. Open `kutaQ3.sln` in Visual Studio (2015 or newer - let VS retarget the v140 toolset if it asks).
2. Build the **Win32 / Release** configuration.

> The shader-detection code relies on inline x86 assembly, so only 32-bit builds are supported.
> `detours.lib` in the repository root is the x86 Detours 3.0 library.

The output DLL is `Release\kutaQ3.dll`.

## Usage

Inject `kutaQ3.dll` into `quake3.exe` with your favourite DLL injector. The hooks are installed
in `DllMain`, the menu appears as soon as the first frame is swapped, and `INSERT` shows/hides it.

Settings use two dedicated files next to the DLL (**not** the game-folder `imgui.ini`):

- `kutaQ3.cfg` — cheat feature toggles (hand-editable).
- `kutaQ3_imgui.ini` — ImGui window layout via `SaveIniSettingsToMemory` /
  `LoadIniSettingsFromMemory` (position, size, collapsed flag, table state).

Use **Save settings** / **Load settings** in the menu, or edit `kutaQ3.cfg` by hand:

```
[Features]
ChamsEnabled=1
ChamsStyle=0          ; 0 = solid, 1 = wireframe
LogShaders=1
```

A missing file keeps the compiled defaults. Layout is also flushed a few seconds
after you move/resize/collapse a window, and both files are written on DLL unload.

This ImGui snapshot is **master** (no docking). Dock-space layouts are not stored
until the docking branch is used; the same `kutaQ3_imgui.ini` path will then include them.

## Third-party

- [Dear ImGui](https://github.com/ocornut/imgui) (MIT license) - `imgui/`
- [Microsoft Detours 3.0](https://github.com/microsoft/Detours) - `detours.h`, `detours.lib`, `detours.pdb`, `detver.h`
