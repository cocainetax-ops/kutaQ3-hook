# kutaQ3 hook

OpenGL hook DLL for Quake III Arena with a Dear ImGui in-game menu.

The DLL hooks `SwapBuffers` (with a `wglSwapBuffers` fallback), `glBindTexture`, `glDrawElements`,
`glVertexPointer`, `CreateWindowExA` and `LoadLibraryExA` with Microsoft Detours. The **"kutaQ3 hook"** menu
is rendered on top of the game every frame using Dear ImGui (the bloat-free immediate mode
GUI for C++) with the fixed-function OpenGL2 backend, which fits Quake 3's legacy GL context.

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
