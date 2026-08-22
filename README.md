# kutaQ3 hook

OpenGL hook DLL for Quake III Arena with a Dear ImGui in-game menu.

The DLL hooks `SwapBuffers` (with a `wglSwapBuffers` fallback), `glBindTexture`, `glDrawElements`,
`glVertexPointer`, `CreateWindowExA` and `LoadLibraryExA` with Microsoft Detours. The **"kutaQ3 hook"** menu
is rendered on top of the game every frame using Dear ImGui (the bloat-free immediate mode
GUI for C++) with the fixed-function OpenGL2 backend (`imgui/imgui_impl_opengl2.cpp` +
`imgui/imgui_impl_opengl2.h`), which fits Quake 3's legacy GL context.

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
- Player shader logger - hold `F10` in-game to dump player model shader names to `log.txt`
- Dear ImGui menu window called **"kutaQ3 hook"**
  - `INSERT` toggles the menu
  - Mouse/keyboard input is captured while the menu is open
  - No files written to the game folder (`imgui.ini` / `imgui_log.txt` disabled)

## Building

1. Open `kutaQ3.sln` in Visual Studio (2015 or newer - let VS retarget the v140 toolset if it asks).
2. Build the **Win32 / Release** configuration.

> The shader-detection code relies on inline x86 assembly, so only 32-bit builds are supported.
> `detours.lib` in the repository root is the x86 Detours 3.0 library.

The output DLL is `Release\kutaQ3.dll`.

## Usage

Inject `kutaQ3.dll` into `quake3.exe` with your favourite DLL injector. The hooks are installed
in `DllMain`, the menu appears as soon as the first frame is swapped, and `INSERT` shows/hides it.

## Third-party

- [Dear ImGui](https://github.com/ocornut/imgui) (MIT license) - `imgui/`
- [Microsoft Detours 3.0](https://github.com/microsoft/Detours) - `detours.h`, `detours.lib`, `detours.pdb`, `detver.h`
