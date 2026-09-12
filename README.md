# kutaQ3 hook

OpenGL hook DLL for Quake III Arena with a Dear ImGui in-game menu.

The DLL hooks `SwapBuffers` (with a `wglSwapBuffers` fallback), `glBindTexture`, `glDrawElements`,
`glVertexPointer`, `CreateWindowExA` and - inside `quake3.exe` itself - the cgame VM's syscall
dispatcher, with Microsoft Detours. The **"kutaQ3 hook"** menu is rendered on top of the game every
frame using Dear ImGui (the bloat-free immediate mode GUI for C++) with the fixed-function OpenGL2
backend (`imgui/imgui_impl_opengl2.cpp` + `imgui/imgui_impl_opengl2.h`), which fits Quake 3's legacy
GL context.

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

## NAME ESP and the cgame VM hook

Player names above every other player's head, through walls, drawn with the `GL::Font`
display-list text renderer in `glText.h` / `glText.cpp`. Toggled with the **Name ESP
(OpenGL)** tickbox in the VISUALS tab (`NameEspEnabled` in `kutaQ3.cfg`); the colour is the
team from the clientinfo, and a tag clamped to the screen edge is dimmed.

It works on a stock install. No `vm_cgame`, no cgame DLL, no module to wait for.

### Why it does not hook a cgame module

Retail Quake 3 does not load one. `CL_InitCGame()` picks an interpreter and creates a VM
(`SDK/code/client/cl_cgame.c:732`):

```c
// load the dll or bytecode
if ( cl_connectedToPureServer != 0 ) {
    // if sv_pure is set we only allow qvms to be loaded
    interpret = VMI_COMPILED;
}
else {
    interpret = Cvar_VariableValue( "vm_cgame" );
}
cgvm = VM_Create( "cgame", CL_CgameSystemCalls, interpret );
```

Three things follow, all of them checkable in the sources under `SDK/` and in the GPL 1.32b
release:

- `vmInterpret_t` is `{ VMI_NATIVE, VMI_BYTECODE, VMI_COMPILED }` (`qcommon.h:289`), so a **native
  DLL cgame is `vm_cgame 0`, not 1** - `1` is the *interpreted* bytecode VM.
- `VM_Init()` defaults the cvar to `"2"` (`vm.c`, *"!@# SHIP WITH SET TO 2"*), i.e. the JIT
  bytecode VM. That is what a stock client runs: `vm/cgame.mp.qvm` out of `baseq3/pak0.pk3`,
  loaded into `quake3.exe` and executed there. There is no `vmMain` export and no `dllEntry` to
  hook, because there is no module.
- On a pure server the engine **ignores `vm_cgame` entirely** and forces `VMI_COMPILED`, so even
  setting the cvar cannot produce a DLL cgame there. (`sv_pure` is documented as defaulting to 1.)

`VM_Create()` only reaches `Sys_LoadDll()` for `VMI_NATIVE`, and falls back to the QVM if that
fails. So a native cgame is a real thing - mods ship `cgamex86.dll`, retail would load
`cgame_mp_x86.dll` - it is just never the default, and never on a pure server. A hook that needs
it needs the user to change a cvar and silently does nothing otherwise.

### What it hooks instead

Every call the cgame makes into the engine funnels through one function pointer the engine stored
in the VM record when it created it (`vm.c`, `vm_local.h:130`):

```c
vm->systemCall = systemCalls;      // CL_CgameSystemCalls for the cgame
```

The interpreter calls it directly for a bytecode VM; a native DLL reaches the same pointer through
`VM_DllSyscall()` (`return currentVM->systemCall(&arg)`). **One Detours hook on that address covers
both**, and because it is engine code in `quake3.exe` it is never unmapped - the hook survives map
changes and reconnects, and can always be cleanly detached. (The old design detoured the exports of
a cgame DLL, which meant racing `LoadLibrary`, and abandoning rather than detaching the trampoline
once the engine freed the module. Neither problem exists any more, and the `LoadLibraryExA` /
`LoadLibraryA` hooks are gone.)

`vmFind.h` / `vmFind.cpp` locate the `vm_t` without hardcoding an address. `vmTable[]` is a static
array inside the executable, so the writable sections of the main module are scanned for a record
matching every invariant `VM_Create()` leaves behind at once: `name == "cgame"`, `systemCall`
inside the module, and - for a bytecode VM - `dataMask == (1<<n)-1`, `programStack == dataMask+1`,
`stackBottom == programStack - STACK_SIZE`. The first two `vm_t` fields are ABI-locked by the
engine itself (`VM_OFFSET_PROGRAM_STACK 0` / `VM_OFFSET_SYSTEM_CALL 4`, because the x86 interpreter
is written in assembly).

The detour then watches the cgame's own traps and keeps what the ESP needs. Nothing in it calls
back into the engine, so none of it depends on being inside a VM call - which is the constraint
that used to force the read into `vmMain`:

| trap | what the hook takes |
|---|---|
| `CG_GETSNAPSHOT` | every snapshot the cgame requests, kept in a small ring keyed by message number (the newest **and the one before it**): player entity positions + the local `playerState_t` |
| `CG_GETGAMESTATE` | the address of the cgame's `cgs.gameState`, read live for the `CS_PLAYERS` configstrings |
| `CG_R_RENDERSCENE` | the `refdef_t` the cgame rendered this frame: the exact `vieworg`, `viewaxis` and `fov_x` |
| `CG_CM_LOADMAP` | level boundary - the per-*level* captures are dropped (see below: **not** the configstrings pointer) |

`NameEsp::Gather()` is handed a trampoline (`Vm::Syscall()`) that answers those same trap numbers
out of the copies, so the portable half of the ESP is unchanged. Pointers resolve the way
`VM_ArgPtr()` resolves them (`vm.c`): a native VM passes real host pointers and has `dataBase` 0, a
bytecode VM passes offsets that get masked into its hunk segment.

### Every `vm_cgame` value, including the one you never set

The three values are `VMI_NATIVE 0` / `VMI_BYTECODE 1` / `VMI_COMPILED 2`, and all three work. `1`
and `2` both take the same QVM path through `VM_Create()`, which allocates the data segment and sets
`dataMask` / `programStack` / `stackBottom` **after** choosing who fills in `codeBase`:

```c
if ( interpret >= VMI_COMPILED ) {
    VM_Compile( vm, header );              // vm_cgame 2 - emit x86
} else {
    VM_PrepareInterpreter( vm, header );   // vm_cgame 1 - translate the opcode stream
}
vm->programStack = vm->dataMask + 1;
vm->stackBottom  = vm->programStack - STACK_SIZE;
```

So the two records differ only in the mode flags and in what the code fields describe:
`VM_Compile()` ends with `vm->codeLength = compiledOfs;` and `Hunk_Alloc( compiledOfs, h_low )`,
while `VM_PrepareInterpreter()` leaves `codeLength` at `header->codeLength` and does
`vm->codeBase = Hunk_Alloc( vm->codeLength*4, h_high )`. Neither is part of the acceptance test -
both satisfy `codeBase != 0` and `codeLength > 0`, and both modes reach the traps through the same
pointer: `vm_interpreted.c` calls `vm->systemCall( (int *)&image[programStack+4] )` from
`VM_CallInterpreted()`, and `vm_x86.c` emits `currentVM->systemCall( ... )` in `AsmCall`, a field
load rather than an address baked in at compile time. `tests/test_vmfind.cpp` pins this: records
shaped for `1`, `2` and `0` are each accepted, `compiled` and `currentlyInterpreting` are flipped
without changing the outcome, and the shared invariants still reject a bad record in either mode.
The status line in the menu reports which one it found.

### Where the configstrings come from, and why names used to take a minute to appear

The pointer to the cgame's `gameState_t` copy (`cgs.gameState`, read live for `CS_PLAYERS`) is
captured from the `CG_GETGAMESTATE` trap. That trap fires in `CG_Init` and again whenever a `"cs"`
server command changes a configstring - the retail cgame's `CG_ConfigStringModified()`
(cg_servercmds.c) answers one by re-fetching the *whole* gamestate through
`trap_GetGameState(&cgs.gameState)`. The trap is the *authoritative* source - it is the cgame
handing over the address of its own global - and memory scans are only the fallback for when no
trap has fired since the hook attached.

Keeping it that way took four fixes; each one on its own still left a window where every name was
skipped, and `Gather()` skips a player with no name rather than guessing one.

1. **The level boundary used to throw the pointer away.** `CG_Init` calls `trap_GetGameState` and
   *then* `trap_CM_LoadMap` (SDK/code/cgame/cg_main.c), and the `CG_CM_LOADMAP` case dropped
   *everything* captured so far - on the reasoning that all of it belonged to the level that had
   just ended. That is true of the snapshots, the camera and the last usercmd, and false of the
   address `CG_Init` had handed over microseconds earlier: `cgs.gameState` is a global inside the
   cgame, so a level change re-fills it in place rather than moving it. So every map change started
   with no configstrings at all, and names only came back when something re-fired the trap. The
   level boundary now drops the per-level captures (`DropLevelState()`) and leaves the pointer
   alone; it is dropped only when the VM instance that owns it is gone, which is compared against
   the identity the pointer was *taken* under (`VmFind::SameVmInstance`) rather than against the last
   one seen - a level change inside one instance changes neither `dataBase` nor `dllHandle`.
2. **The liveness check asserted an invariant the engine breaks.** `GameStateLooksLive()` required
   `stringOffsets[CS_SERVERINFO] == 1`, true in a freshly parsed gamestate because the serverinfo is
   the first string appended. A runtime update moves that offset to the highest in the table - and
   from then on the bridge reported a perfectly good copy as dead, so names stopped *and stayed
   stopped* until the next trap, and the scan could never find the copy again either. The check is
   now "the used pool is inside the 16000-byte limit and `CS_SERVERINFO` points into it", which
   holds for a live copy at any time.
3. **The scan's shape check assumed the offsets stay in index order.** They do not under every
   engine: ioquake3 appends a runtime `"cs"` at the end of the pool whatever its index (retail 1.32
   rebuilds the whole pool in index order instead, `CL_ConfigstringModified`). The checks are now
   order independent - every set offset lands inside the pool, no two set indices share one, every
   set string is terminated and control-character free, `CS_SERVERINFO` carries `\mapname\`, and at
   least one `CS_PLAYERS` slot holds an infostring - so the fallback recognises a live copy whenever
   it runs instead of only in the first seconds after a parse.
4. **A fresh join always missed the one trap that mattered, and the first `cs` was your first
   death.** `CL_InitCGame()` runs `VM_Create("cgame")` and `CG_Init()` back to back, mid-frame;
   the dispatcher detour is installed from the SwapBuffers poll at frame end. There is no VM record
   (and so no dispatcher address) until `VM_Create` finishes, and `CG_Init` fires its
   `trap_GetGameState` microseconds after that - so joining from the main menu *always* misses the
   capture. On stock 1.32 nothing re-fires that trap until the first runtime `"cs"`, and on a quiet
   FFA server the first runtime configstring change is the first score event of the match: your own
   first death (a suicide scores -1, the bot's frag +1, both update `CS_SCORES1` → the server sends
   `cs 7 …`). That is the whole "names only show up after I kill myself" symptom - the death was
   never the trigger, the configstring change it causes was.

   The fix is a source that needs no trap at all: the engine's **own** copy,
   `clientActive_t::gameState`, a global in quake3.exe. `CL_ParseGamestate()` fills it before
   `CL_InitCGame()` runs - before the cgame VM even exists - and `CL_GetGameState()` (cl_cgame.c)
   is nothing but `*gs = cl.gameState`, exactly what the trap hands over. A shape scan of
   quake3.exe's data (`ScanEngineForGameState()` in vmHook.cpp) finds it with the same verifier the
   other scans use, so a fresh join has configstrings within a poll tick, without waiting for any
   `cs`. Precedence: the trap capture, then the engine copy, then the cgame VM's own copy; the
   first runtime `"cs"` re-fires the trap and upgrades either scan on its own. An engine-copy
   pointer is deliberately exempt from the VM-instance identity check - `cl.gameState` belongs to
   quake3.exe and outlives every cgame VM; the shape checks are what keep it honest.

The menu status line says which source the names are coming from (`configstrings: cgame trap, 12s
old` / `engine cl.gameState (scan)` / `cgame data (scan)` / `not found yet`). Read that first if
names ever go missing: a `cgame trap` line with names missing is a gathering problem, a scan line
means the authoritative capture has not arrived (yet - normal until the first runtime `"cs"`), and
`not found yet` means neither has any copy.


### The view

`refdef_t` is preferred: it is the view the frame was actually drawn with. It is only used once it
passes a shape check (`RefdefUsable` in `nameEspCore.cpp`), because it is read out of the cgame's
data segment by address. That check measures the handedness of `viewaxis` rather than assuming it:
`AnglesToAxis()` gives a **right-handed** triple (`cross(viewaxis[0], viewaxis[1]) == +viewaxis[2]`,
measured 1.000 against `SDK/code/game/q_math.c`), because `AngleVectors()` reports *right* as
`(0,-1,0)` at zero angles and `AnglesToAxis()` negates it - so `viewaxis[1]` is the world LEFT
vector, which is what the projection relies on when it flips the sign to make screen x grow right.
A renderer that handed over the un-negated vector would measure -1 and be rejected instead of
mirroring every tag.

Without a refdef the view is rebuilt the way it always was: the newest `usercmd_t` plus
`playerState_t::delta_angles` (the engine's `PM_UpdateViewAngles()`, including its frozen view
while dead-and-playing or in intermission and its +/-87.9 degree pitch clamp), the snapshot
origin pushed forward by the snapshot's age, `cg_fov` for `fov_x`, and `fov_y` plus the screen
rectangle from the GL viewport at draw time.

Remote players are placed **exactly where the renderer places their models**. The cgame never
velocity-extrapolates players: `CG_InterpolateEntityPosition()` (cg_ents.c) lerps each player
between the previous and the newest server snapshot with `frameInterpolation = (cg.time -
old.serverTime) / (new.serverTime - old.serverTime)` clamped to 0..1, and holds the newest
position while waiting for the next snapshot. The hook does the same: the VM-hook snapshot ring
gives Gather() both endpoints, the captured refdef's time (`cg.time`) gives the fraction, and a
player missing from the previous snapshot (just entered the PVS / respawned) or carrying a
toggled `EF_TELEPORT_BIT` is snapped to the newest position just like `CG_ResetEntity()`. An
earlier build held only the newest snapshot and velocity-extrapolated from it, which parked the
tag one server frame ahead of a moving player and stepped it every ~50 ms - the visible
"jitter"; lerping the engine's own two endpoints glues the tag to the model on every frame.

### Smoothness: what makes a tag glide instead of step

Three things between "the tag tracks the model" and "the tag looks like part of the scene":

- **The previous sample is searched for, not assumed.** The lerp needs the snapshot the engine
  interpolates *from*, and `Gather()` used to ask for exactly `newest - 1`. The bridge answers a
  message number its ring no longer holds with the *newest* snapshot, so a miss is
  indistinguishable from "that is the newest sample" except by its `serverTime` - and the exact
  previous number goes missing whenever the cgame advanced by more than one server frame since the
  ring was last filled (a hitch, the first frames after a level load). Those frames lost the
  interpolation entirely: the tag held still, then jumped a whole server frame. `Gather()` now walks
  back up to 4 numbers until a strictly older sample turns up, which covers both the bridge's
  fallback and the real engine's "aged out of the buffer" answer.
- **The interpolation clock survives a missing refdef.** The fraction is measured at `cg.time`,
  which only exists in a captured `refdef_t`. Without one, `Vm::ServerTime()` hands over the newest
  snapshot's own time, which clamps the fraction to 1.0 - the tag stops dead and steps. The lag
  between `cg.time` and the newest snapshot is measured on every frame that has a refdef and reused
  on the ones that do not (clamped to +/-250 ms, and dropped by `Reset()`), so a frame without a
  refdef keeps the tag where the model is instead of snapping it to the newest server position.
- **Tags fade in, and the chest anchor holds.** A tag ramps its alpha up over 220 ms rather than
  popping into existence, keyed by client number so a player who flickers out of the PVS and back
  inside a few frames keeps their alpha instead of blinking (the overlay turns blending on for it;
  at full alpha the blend is a no-op, so a settled tag is pixel-identical to the old opaque draw).
  And the head-to-chest re-anchor, which kicks in up close when the anchor above the head leaves the
  screen, no longer gives the chest back on the first frame the head anchor reappears: aiming up
  and down across that boundary used to swap the anchor every other frame and hop the name between
  two points a head apart. Moving *to* the chest is still immediate; moving back waits three frames.

Both the fade and the anchor choice are per-client state that outlives a frame, so they are dropped
with the level (`NameEsp::ResetDrawState()`, called from the VM hook's level-boundary drop) and
advance once per `Draw()` rather than once per tag.

### What the server sends you (PVS)

A tag can only be drawn for a player the server put in your snapshot, and the server only sends
entities in your *potentially visible set* - roughly, the parts of the map your client could
currently see. A spectator floating above the map is in sight of nearly everything and gets a
snapshot full of players; a player down in a corridor only gets whoever shares their PVS. That
is not the ESP failing. The Name ESP status lines in the menu make it unambiguous on a live
client: "N player entities in snapshot; self #k (normal, 100 hp)" is what the server actually
sent you, with separate counters for dead/self/unreadable entries. In first person a player
outside your visible set simply is not there (the "no live others" line explains the PVS
limit); the same map seen from a spectator perch reports every player. The flip side is
that a tag for a player behind a wall only appears while that wall's far side is still in your
PVS; "through walls" reaches exactly as far as the server's visibility reaches, on any client
(and the chams have the same limit - a model the server never sent cannot be drawn).

The structures crossing that boundary are mirrored by hand in `q3sdk.h` and `vmFind.h` (the GPL
headers in `SDK/` stay out of the build - see `SDK/README.md`), and
`SDK/code/client/cl_sdkmirror.cpp` asserts every mirrored size, offset and syscall number against
the real 1.32b headers.

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
- **NAME ESP** (`nameEsp.h`) - every other player's name drawn above their head through walls,
  in their team colour, with a 1px drop shadow so it stays readable on any background.
  Toggled with the **Name ESP (OpenGL)** tickbox in the VISUALS tab (`NameEspEnabled` in
  `kutaQ3.cfg`), driven by the cgame VM hook described above. Your own name is not drawn,
  dead players (corpses) are skipped, and tags for players outside the frustum are clamped to
  the screen edge and dimmed. Up close, where aiming up or down would push the above-the-head
  anchor off the screen while the player is still visible, the tag re-anchors to the chest.
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
NeonEnabled=0         ; 1 = neon bloom chams override the style above
NameEspEnabled=1      ; 1 = player names on screen (reads the cgame VM directly)
LogShaders=1
```

A missing file keeps the compiled defaults. Layout is also flushed a few seconds
after you move/resize/collapse a window, and both files are written on DLL unload.

This ImGui snapshot is **master** (no docking). Dock-space layouts are not stored
until the docking branch is used; the same `kutaQ3_imgui.ini` path will then include them.

## Tests

The DLL is a Win32/MSVC build, so it cannot be linked on a Linux host - but everything in the
NAME ESP and the VM hook that is not Win32 API can be compiled and run here, and is:

```
make -C tests check
```

| target | what it runs |
|---|---|
| `mirror` | `SDK/code/client/cl_sdkmirror.cpp`: every size, offset and syscall number in `q3sdk.h`, and the `vm_t` mirror in `vmFind.h`, as a `static_assert` against the real 1.32b headers. Drift fails the *compile*. |
| `core` | the real `nameEspCore.cpp`, driven by a fake engine syscall trampoline (`tests/fake_engine.cpp`): infostring parsing, which entities become tags, the view rebuild (including the captured `refdef_t` and its shape checks), the smoothing - including finding the sample it interpolates from when the exact previous message number is gone, and the interpolation clock surviving a missing refdef - and the projection, checked against the engine's own `AngleVectors()` / `AnglesToAxis()` compiled out of `SDK/code/game/q_math.c`. |
| `vm` | the real `vmFind.cpp`: the scanners that find the cgame `vm_t` and the cgame's `gameState_t` copy, driven with records built the way `VM_Create()` and `CL_ParseGamestate()` build them, plus every near-miss they have to reject - and the copy of a level that has been *running*, whose offsets a runtime `"cs"` has put out of index order. Also `VmFind::SameVmInstance`, the rule that decides whether a captured pointer survives a map change. |
| `gl` | the real `nameEsp.cpp` + `glText.cpp` + `glDraw.cpp` against a stub `<windows.h>` / `<gl/GL.h>` (`tests/stub/`) that records every call, so the raster positions, colours, alphas and strings actually issued for a frame can be asserted on - including the fade-in ramp across frames and the chest anchor holding its ground. |
| `vmhook.o` | the real `vmHook.cpp`, compiled only - it is the Win32 half (PE headers, `VirtualQuery`, Detours) and cannot run off Windows. `tests/stub_win/` declares just the Win32 surface it touches, so a typo or a type mismatch fails here rather than in Visual Studio. |

They need nothing but a C++11 compiler; `tests/build/` is ignored.

Two things this host genuinely cannot check, so they are not quietly assumed to be fine:

- **The `vm_t` field offsets behind `vm_t::name`.** `struct vm_s` is full of pointers, so on an
  LP64 host the engine's copy is wider than the x86-only mirror and only the ABI-locked first two
  fields compare (`VM_OFFSET_PROGRAM_STACK` / `VM_OFFSET_SYSTEM_CALL`, which the mirror is asserted
  against on any host). The full field-by-field comparison is inside `#if UINTPTR_MAX == 0xffffffff`
  in `cl_sdkmirror.cpp`, so an x86 build of the harness checks all of it. This sandbox has no 32-bit
  libc headers (`g++ -m32` cannot include `<stdint.h>`), so that block does not run here.
- **`vmHook.cpp` against a real `quake3.exe`.** The scanners and the trap decoding are unit-tested;
  that they match the shipped 1.32b binary is the one thing that needs the game.

## Third-party

- [Dear ImGui](https://github.com/ocornut/imgui) (MIT license) - `imgui/`
- [Microsoft Detours 3.0](https://github.com/microsoft/Detours) - `detours.h`, `detours.lib`, `detours.pdb`, `detver.h`
- [Quake III Arena 1.32b source](https://github.com/id-Software/Quake-III-Arena)
  (GPL v2, commit `dbe4ddb`) - `SDK/`, the engine's own data structures for
  reading client state instead of guessing at offsets. See `SDK/README.md`;
  `SDK/COPYING.txt` carries the licence.
