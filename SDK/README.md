# SDK — Quake III Arena 1.32b source reference

The files a `kutaQ3` client hook needs in order to read Quake 3's own data
structures instead of guessing at them. Everything under `code/` except the two
`cl_sdk*.c` files is **unmodified id Software source**.

## Provenance and licence

| | |
|---|---|
| Upstream | `https://github.com/id-Software/Quake-III-Arena` (note: roman numerals) |
| Commit | `dbe4ddb10315479fc00086f08e25d968b4b43c49` |
| Commit subject | *"The Quake III Arena sources as originally released under the GPL license on August 20, 2005."* |
| Licence | **GPL v2 only** — see `COPYING.txt` |
| Version stamp | `Q3_VERSION "Q3 1.32b"` (`code/game/q_shared.h:29`) |
| Protocol | `PROTOCOL_VERSION 68` (`code/qcommon/qcommon.h:222`) |

This is the original id Software release, **not** ioquake3. That matters:
ioquake3 has diverged for twenty years and its struct layouts no longer match
retail `quake3.exe`, so offsets read out of an ioq3 header can be wrong against
the 1.32b binary this hook injects into. `UPSTREAM-README.txt` is id's own
release note.

**GPL v2 obligations.** These files are copied verbatim, so `COPYING.txt` must
travel with them and any derivative of them stays GPL. The two `cl_sdk*.c`
harness files are kutaQ3 additions that only `#include` the engine headers and
print facts about them; they contain no id code.

Nothing under `SDK/` is referenced by `kutaQ3.vcxproj` — that project lists its
sources explicitly, with no wildcards — so the SDK is inert as far as the DLL
build is concerned. If you later add any of these files (or a header that
`#include`s them) to the hook project, the resulting DLL becomes a GPL
derivative and has to be distributed under GPL v2. Reading the headers to learn
an offset and hard-coding that offset is a different matter from compiling the
code in, but that is a judgement call to make deliberately, not by accident.

## What is here and why

22 headers and 12 `.c` files — 8 engine sources that compile anywhere, 2 win32
sources kept as reference, and the 2 kutaQ3 layout harnesses. The directory
layout under `code/` is upstream's own — do not flatten it, because the engine
headers `#include` each other by relative path (`"../game/q_shared.h"`).

### `code/game/` — the shared foundation
| File | Why you need it |
|---|---|
| `q_shared.h` | **The** header. `entityState_t`, `playerState_t`, `usercmd_t`, `trajectory_t`, `cvar_t`, `vmCvar_t`, `vec3_t`, `MAX_CLIENTS`, `MAX_GENTITIES`. Guarded (`__Q_SHARED_H`). |
| `q_shared.c`, `q_math.c`, `bg_lib.c` | Matching implementations (`Q_strncpyz`, `Com_sprintf`, `VectorNormalize`, …). Compile these instead of hand-rolling them, or your string/vector maths will differ from the engine's in the edge cases. |
| `bg_public.h` | `pmove_t`, `weapon_t`, `gitem_t`, `animation_t`, `entityType_t`, `statIndex_t`, `MAX_ITEMS 256`. |
| `bg_pmove.c`, `bg_misc.c`, `bg_slidemove.c`, `bg_local.h` | The shared movement/item code. `bg_pmove.c` is how the engine turns a `usercmd_t` into a position — the reference for any client-side prediction. |
| `g_public.h` | `gameImport_t` / `gameExport_t`, `entityShared_t`, `GAME_API_VERSION 8`. |
| `surfaceflags.h` | `CONTENTS_*` / `SURF_*` — what a trace hit actually is. |
| `botlib.h` | Pulled in by `client/cl_cgame.c`; also `BOTLIB_API_VERSION 2`. |

### `code/qcommon/` — engine internals
`qcommon.h` (guarded `_QCOMMON_H_`, defines `PROTOCOL_VERSION`), `qfiles.h`
(`.bsp` / `.md3` on-disk formats), `cm_public.h` (the trace/clipmodel API),
`vm_local.h` (`vm_t`, the bytecode-VM state).

### `code/client/` — the client
`client.h` is the whole client: it defines `clientActive_t` (the `cl` global),
`clientStatic_t` (`cls`), `clSnapshot_t` and `gameState_t`, and transitively
includes the shared, renderer, ui and cgame public headers. `keys.h` +
`snd_public.h` come with it. `cl_cgame.c` is **the** file for understanding the
client ↔ cgame boundary — it contains `CL_CgameSystemCalls()`, the table the
engine answers cgame syscalls through.

### `code/cgame/` — the client game module
`cg_local.h` defines `cg_t` (117 fields: the cgame's entire per-frame world),
`cgs_t` (per-level state), `centity_t`, `clientInfo_t`, `weaponInfo_t`.
`cg_public.h` defines the `snapshot_t` the cgame sees and the `CG_*` syscall
enums. `tr_types.h` defines `refdef_t`, `refEntity_t` and `glconfig_t` — the
renderer ABI. `cg_main.c` shows the `vmMain()` dispatch.

### `code/renderer/` + `code/ui/`
`tr_public.h` (`refimport_t` / `refexport_t`, `REF_API_VERSION 8`) and
`ui_public.h` (`uiImport_t` / `uiExport_t`, `UI_API_VERSION 6`).

### `code/win32/` — directly relevant to this repo's `dinputHook`
`win_input.c` is the authoritative description of the legacy DirectInput mouse
path that `dinputHook.cpp` hooks, and it confirms several things the repo README
can only guess at:

- **`win_local.h:34` is `#define DIRECTINPUT_VERSION 0x0300`.** The GPL source
  compiles the game against legacy DirectInput version `0x0300` specifically —
  one definite answer out of the four candidates (`0x0300 / 0x0500 / 0x05A0 /
  0x0700`) that `dinputHook.cpp` enumerates and probes.
- The engine calls `LoadLibrary("dinput.dll")` → `DirectInputCreateA` →
  `CreateDevice(&GUID_SysMouse)` → `SetDataFormat` → `GetDeviceData` /
  `GetDeviceState`, exactly the sequence the README describes.
- **The engine does *not* use `c_dfDIMouse`.** It builds a private
  `DIDATAFORMAT` over its own `MYDATA` struct, with an in-source note at
  `win_input.c:237`: *"NOTE TTimo: would be easier using c_dfDIMouse or
  c_dfDIMouse2"*. It also sets `DIDF_RELAXIS` in that format while declaring the
  axes `DIDFT_AXIS`.
- `MYDATA` is `LONG lX, lY, lZ; BYTE bButtonA..D` — on 32-bit Win32 that is
  **16 bytes with the buttons at offset 12**, byte-identical in size to
  `DIMOUSESTATE`. So `dinputHook.cpp`'s "16 bytes ⇒ mouse" filter does match the
  engine's real format, but it matches *by coincidence of size*, not because the
  engine installs `DIMOUSESTATE`. Worth knowing before you rely on the
  assumption. (Verified with `_Static_assert`; on x86-64 `LONG` is 8 bytes and
  `MYDATA` becomes 32, which is why this only holds for the 32-bit build.)

`win_wndproc.c` is the window procedure and message routing; `win_local.h` holds
`winvar_t` / `glwstate_t`; `glw_win.h` the GL window state.

## Two rules that will bite you

### 1. Most of these headers have no include guard

Guarded: `q_shared.h`, `tr_types.h`, `tr_public.h`, `qcommon.h`, `qfiles.h`,
`ui_public.h`, `keycodes.h`.

**Unguarded:** `client.h`, `cgame/cg_public.h`, `cgame/cg_local.h`,
`game/bg_public.h`, `game/g_public.h`, `qcommon/cm_public.h`,
`qcommon/vm_local.h`, `game/bg_lib.h`, `game/bg_local.h`, `game/surfaceflags.h`,
`client/keys.h`, `client/snd_public.h`.

The engine survives this because **no translation unit ever mixes the client-side
and cgame-side header families.** Break that rule and `cg_public.h` /
`bg_public.h` arrive twice by two different relative paths and you get a wall of
"redeclaration of enumerator" errors. This is exactly why the layout harness is
split into two `.c` files.

### 2. Field order is not layout — build the harness on x86

The field order here matches retail 1.32b, but *padding* depends on the ABI, and
retail Quake 3 is 32-bit. Build the harness 64-bit and every pointer doubles in
size, silently shifting every offset after it.

```
code\client\cl_sdkclstate.c   MSVC (x86 Native Tools prompt, at the SDK root):
code\client\cl_sdkcgstate.c       cl /nologo /W3 code\client\cl_sdkclstate.c /Fe:cl_sdkclstate.exe
                                  cl /nologo /W3 code\client\cl_sdkcgstate.c /Fe:cl_sdkcgstate.exe
                              MinGW-w64 / gcc:
                                  gcc -m32 -o cl_sdkclstate code/client/cl_sdkclstate.c
                                  gcc -m32 -o cl_sdkcgstate code/client/cl_sdkcgstate.c
```

Both programs refuse quietly: they print a warning banner whenever
`sizeof(void*) != 4`. Run them and read the two outputs together.

## Naming traps in 1.32b

- **There is no `cl_t`** anywhere in the tree. The client's state is the
  `clientActive_t` global `cl` and the `clientStatic_t` global `cls`.
- **`clientSnapshot_t` is the server's type** (`code/server/server.h`, not in this
  SDK). The client's is `clSnapshot_t`; the cgame's is `snapshot_t`.
- **`cg_t` has no `entities` member.** The per-entity array is the standalone
  global `cg_entities[MAX_GENTITIES]`, so `sizeof(centity_t)` is its stride.
- **`fraglimit` / `timelimit` / `maxclients` / `mapname` are in `cgs_t`, not
  `cg_t`.** `cg_t` only has `fraglimitWarnings` / `timelimitWarnings`.
- **`cgameImport_t`, `cgameExport_t`, `uiImport_t`, `uiExport_t`,
  `gameImport_t`, `gameExport_t` are enums, not structs of function pointers.**
  They number the syscalls; the functions themselves are passed flat to
  `vmMain()` / `dllEntry()`. `sizeof()` on them, never `offsetof()`.
- **There is no `CGAME_API_VERSION`.** `GAME_API_VERSION` is 8, `UI_API_VERSION`
  6, `REF_API_VERSION` 8 — the cgame module has no version handshake at all.
- **`refdef_t` carries no entity list.** Entities are submitted to the renderer
  one at a time via `re.AddRefEntityToScene()`.

## Verified and not verified

Checked on a Linux x86-64 host with gcc 12.2 (`gcc -c -I. <file>` from the SDK
root):

- **All 8 engine `.c` files compile clean:** `q_shared.c`, `q_math.c`,
  `bg_lib.c`, `bg_pmove.c`, `bg_misc.c`, `bg_slidemove.c`, `cl_cgame.c`,
  `cg_main.c`.
- **Both harness programs compile, link and run.**
- `win_input.c` / `win_wndproc.c` fail with `fatal error: windows.h` — expected,
  they are reference-only off Windows.
- The header closure is complete: every `#include "..."` in the 22 headers
  resolves inside `code/`, with nothing missing.

**Not** checked, because this sandbox has no 32-bit toolchain and no MSVC:

- The actual x86 sizes and offsets — build the harness on your x86 machine and
  read them off. Everything printed from a 64-bit build is labelled wrong by the
  programs themselves.
- That the layouts match a specific `quake3.exe`. The source is 1.32b and the
  hook targets 1.32, but confirming that against your binary is your step.
