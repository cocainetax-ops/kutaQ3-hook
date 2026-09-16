#pragma once

// =============================================================================================== //
// kutaQ3 hook - WEAPON ESP
//
// Every other player's current weapon, at their LEG position (not the head), through walls.
// Toggled from the VISUALS tab with its own checkbox; while on, one of three styles:
//
//   Text - the weapon's name string, in the GL::Font display-list face (glText.h), centred on
//          the leg anchor, in the SwapBuffers overlay;
//
//   Icon - the weapon's item icon: the cgame's own icon shader for that weapon (the "icon"
//          field of the item table entry), loaded out of the game's pak files and drawn as a
//          quad centred on the leg anchor. The quad is projected from the 3D anchor point, so
//          it sits on the player's legs in world space and follows them exactly like the other
//          ESP overlays.
//
//   3D Model - the weapon's actual 3D world model (the "world_model[0]" field of the same item
//          table entry - the very path the cgame hands to RE_RegisterModel for the weapon),
//          rendered IN the game's scene, not in the 2D overlay: a refEntity_t is pushed through
//          the engine's own renderer (refexport_t::AddRefEntityToScene) during the cgame's own
//          CG_R_RENDERSCENE trap, right before R_RenderScene runs, so it renders in the same
//          frame at the player's exact interpolated position (the same lerp the cgame applies
//          to the body). The model is oriented with the player's own interpolated angles and
//          scaled through its axis matrix (nonNormalizedAxes - the "axis matrix scale" of the
//          refEntity API), and carries the RF_DEPTHHACK | RF_MINLIGHT renderfx - the same
//          flags the cgame's own through-wall name tags use - so it draws through walls like
//          every other ESP and stays visible in unlit corners. Distance scales it the way the
//          3D world does (no 2D ramp); the menu's model-scale slider multiplies the axis
//          matrix. A weapon whose model cannot be registered (no world_model in the table,
//          path not in the paks, or the renderer's export table cannot be located) falls back
//          to the Icon style for that player, so the ESP is never blind in model mode.
//
// Where the data comes from
// -------------------------
// The weapon NUMBER is the stock networked field: the ET_PLAYER entity's entityState_t::weapon
// (BG_PlayerStateToEntityState copies ps->weapon into it, bg_misc.c). That is the very index the
// cgame uses to index its own native cg_weapons[] array - so whatever a mod numbers its weapons
// as, the ESP follows it; nothing about the ESP's weapon handling is vanilla-specific.
//
// The weapon NAME, ICON and MODEL PATH come from the cgame's own native item table:
// bg_itemlist[], the gitem_t[] global compiled into the cgame (bg_misc.c). That table is what
// the cgame's cg_weapons[] is built from - cg_weapons[W].item is the gitem_t entry of the
// weapon item, its icon (cg_items[].icon) is registered from the entry's "icon" field, and its
// world model (the weapon model the cgame loads as cg_weapons[W].weaponModel) from
// "world_model[0]". So the ESP reads the same names, classes, icon shader paths and model
// paths the cgame itself uses, and a total conversion with its own weapon list (renamed,
// renumbered or new weapons) is picked up as-is: the ESP resolves the snapshot's weapon index
// through the MOD's table, not through a hardcoded vanilla one.
//
// weaponEspCore.cpp (the portable half) shape-scans the cgame's data segment for bg_itemlist[]
// and extracts, per weapon number, the pickup_name (falling back to the classname) and the icon
// shader name. When the scan cannot find the table (a cgame built against a different bg
// layout), the ESP falls back to the built-in stock 1.32 table, so the feature still works on
// vanilla servers.
//
// Icon textures
// -------------
// The icon shader name (e.g. "icons/iconw_gauntlet") maps to a .tga in the game's pak files
// (baseq3/pak0.pk3 or the mod's paks). weaponEsp.cpp opens those paks (they are plain ZIP
// archives), decodes the TGA and uploads it as a GL texture on the current context - self
// contained, no renderer-internal memory is touched. A missing icon is drawn as a neutral chip
// so the position is still marked.
//
// Leg anchor and stacking
// -----------------------
// The tag is centred on lerpOrigin + kWeaponEspLegHeight (q3sdk.h): mid-leg, 8 units above the
// feet. The NAME / DISTANCE / HEALTH ESPs all stack in rows ABOVE the head anchor
// (lerpOrigin + kPlayerTagHeight, see NameEsp::ComputeEspRows), which projects above the model;
// the weapon ESP is the only overlay anchored BELOW the model, so the two stacks can never
// overlap for the same player - the full standing player bbox (origin z -24..+32, bg_pmove.c)
// always sits between them on screen.
//
// Distance-based scale & alpha fading
// -----------------------------------
// Instead of drawing everyone's weapon at full size and opacity, the tag scales down and fades
// out with range, driven by |cg.refdef.vieworg - cent->lerpOrigin| - the view origin the frame
// was rendered with and the player's interpolated origin, already computed per tag by
// NameEsp::Gather() as tag.distance. It uses the same ramp as the DISTANCE and HEALTH ESPs
// (DistanceEsp::DistanceFade: full inside kFadeStartDist, scale kMinScale / alpha 0 at
// kFadeEndDist), so all the overlays agree about what "far" looks like.
//
// Split like the other ESPs: the table maths (scanning, name/icon resolution, the anchor) lives
// in weaponEspCore.cpp, which needs no <windows.h> and no GL, so tests/ can compile and run it
// against a fabricated cgame data segment. weaponEsp.cpp holds the GL / pak / texture half.
// =============================================================================================== //

#include "nameEsp.h"
#include "distanceEsp.h"   // DistanceEsp::DistanceFade - the shared scale/alpha ramp
#include "q3sdk.h"

#include <stdint.h>

namespace WeaponEsp
{
	// The icon's side (px) at full distance (scale 1). Q3's own 3D item icons are roughly this
	// size on screen at close range.
	const float kIconBaseSizePx = 36.0f;

	// Table slots: 1..15 is the stock 1.32 range (MAX_WEAPONS 16), extended to 32 for total
	// conversions that recompile bg_public.h with a larger MAX_WEAPONS.
	const int kTableWeapons = 32;

	// One weapon's display data, as resolved for the current cgame:
	//   name   - what Text mode prints (the item's pickup_name, or its classname)
	//   icon   - the cgame's icon shader name ("icons/iconw_gauntlet") Icon mode loads
	//   model  - the item's world_model[0] ("models/weapons2/shotgun/shotgun.md3") - the
	//            exact path the cgame registers as the weapon's 3D model; Model mode pushes
	//            it into the scene
	struct WeaponInfo
	{
		char name[64];
		char icon[64];
		char model[128];
	};

	// The weapon table the ESP resolves through. kTableWeapons slots: the 1.32 ABI numbers
	// weapons 1..15 (MAX_WEAPONS 16), but a total conversion that recompiles with a larger
	// MAX_WEAPONS still ships the number over the wire (entityState_t::weapon is a byte), so
	// the table runs to 32 to cover that; anything above degrades to a "W<number>" tag.
	struct WeaponTable
	{
		WeaponInfo weapons[kTableWeapons];
		bool       haveName[kTableWeapons];
		bool       haveIcon[kTableWeapons];
		bool       haveModel[kTableWeapons];
		int        weaponCount;
	};

	// The VISUALS-tab style radio values.
	enum Style
	{
		StyleText = 0,
		StyleIcon = 1,
		StyleModel = 2
	};

	// Result of one shape scan of a cgame data region (see weaponEspCore.cpp).
	struct ScanResult
	{
		bool found;
		int  stride;       // the gitem_t stride the table was found at: 52 (stock 1.32) or
	                     // 72 (ioquake3 1.36+ layout with the giFlags/use_func additions)
		int  itemCount;    // entries walked
		size_t offset;     // the table's start, relative to the scanned region
		WeaponTable table;
	};

	// Fill `table` with the built-in stock 1.32 (mission pack) weapon names and icons - the
	// fallback when no native table is found. Exposed for the tests.
	void StockTable(WeaponTable& table);

	// Shape-scan one readable region for bg_itemlist[] and, when found, extract the weapon
	// table. region..region+size is readable; dataBase/dataMask/native describe how a pointer
	// VALUE found in it resolves to a host address (VM_ArgPtr: a bytecode VM stores masked
	// offsets into its hunk segment, a native DLL stores host pointers, and a plain host
	// memory scan - the tests - passes native=true with dataBase 0).
	bool ScanRegion(const unsigned char* region, size_t size, uintptr_t dataBase,
	                uint32_t dataMask, bool native, ScanResult& out);

	// The string Text mode prints for a weapon number: the table's name, or "W<number>" when
	// the table has nothing for it (an out-of-range or unlisted mod weapon still gets a tag).
	// Returns false for WP_NONE (0) and negative weapons - the player is carrying nothing,
	// so there is nothing to display.
	bool WeaponName(const WeaponTable& table, int weapon, char* out, size_t outSize);

	// The icon shader name Icon mode loads for a weapon number, or "" when unlisted.
	bool WeaponIcon(const WeaponTable& table, int weapon, char* out, size_t outSize);

	// The world model path Model mode pushes for a weapon number, or "" when unlisted (the
	// stock gauntlet has one too; some mod weapons may not).
	bool WeaponModel(const WeaponTable& table, int weapon, char* out, size_t outSize);

	// =========================================================================================
	// 3D Model mode - the refEntity the scene receives, and the renderer table it is pushed
	// through.
	//
	// The model has to be inside the rendered frame, so it cannot be drawn by the SwapBuffers
	// overlay (that runs after R_RenderScene, one frame late, and would depth-test against the
	// just-rendered world). It is pushed instead while the cgame's own CG_R_RENDERSCENE trap is
	// in flight - the one trap that happens right before R_RenderScene on the engine's side -
	// and it goes through the ENGINE's renderer, not our own GL calls:
	//
	//   - the model qhandle comes from refexport_t::RegisterModel (RE_RegisterModel), the same
	//     trap the cgame uses for every model it loads - the path is the item table's
	//     world_model[0], so a TC weapon's own model is what gets pushed;
	//
	//   - the refEntity is appended with refexport_t::AddRefEntityToScene (RE_AddRefEntityTo-
	//     Scene), the same trap the cgame's effect tags and world items use.
	//
	// refexport_t ("re") is the struct of renderer function pointers the engine fills at
	// startup (tr_public.h; 29 slots on retail 1.32b, no __USEA3D slot). It is a plain global
	// in quake3.exe, found by shape exactly like the vm_t is: a run of 29 consecutive
	// pointers all inside the main module's code range. The engine has one other such run -
	// the sibling refimport_t ("ri", 28 slots) - so a candidate is only accepted when the
	// cgame's own syscall dispatcher (CL_CgameSystemCalls, whose address is already in hand:
	// vm->systemCall) references that candidate's slots by address: the dispatcher's machine
	// code calls re.RegisterModel / re.RenderScene / ... through the struct, so its bytes
	// carry the slot addresses as immediates. No engine bytes are written, no trap is
	// synthesised, nothing calls into the cgame.
	// =========================================================================================

	// refexport_t: 29 function-pointer slots (retail 1.32b). The scan only needs the run to
	// be long enough; the two slots actually used are named here.
	const int kRefExportSlots          = 29;
	const int kSlotRegisterModel       = 2;    // qhandle_t (const char *name)
	const int kSlotAddRefEntityToScene = 10;   // void (const refEntity_t *re)
	const int kSlotHitsNeeded          = 6;    // dispatcher references to the candidate's slots
	                                          // required to accept it (it actually calls ~20)

	// Portable core of the refexport_t scan (tested in tests/): look for kRefExportSlots
	// consecutive 4-byte values in [codeLow, codeHigh) inside region, then count how many of
	// the candidate's 29 slot addresses occur as little-endian 32-bit immediates in dispCode
	// (the dispatcher's own bytes). Accepts the first candidate with >= kSlotHitsNeeded hits;
	// *outBase receives its address. Returns false when nothing passes.
	bool FindRefExportInBytes(const unsigned char* region, size_t size,
	                          uintptr_t codeLow, uintptr_t codeHigh,
	                          const unsigned char* dispCode, size_t dispLen,
	                          uintptr_t* outBase);

	// Build the refEntity Model mode pushes for one player: RT_MODEL, RF_DEPTHHACK |
	// RF_MINLIGHT (through the world + visible in the dark - the cgame's name-tag flags),
	// the leg anchor as origin, the player's own lerpAngles as the axis matrix with every
	// axis multiplied by `scale` (nonNormalizedAxes set - that is how a refEntity carries a
	// scale), frame 0. Returns false for WP_NONE or when `handle` is not a registered model
	// (the caller then falls back to the icon).
	bool PlanModelEntity(const NameEsp::PlayerTag& tag, float scale, int handle,
	                     q3::refEntity_t& out);

	// What Draw() did with the frame it was given (read by the menu).
	struct DrawStats
	{
		int drawn;         // tags issued to GL (text: shadow + glyphs, icon: outline + quad)
		int inView;        // ... projected unclamped
		int edge;          // ... clamped to the viewport edge, dimmed
		int behind;        // skipped as behind the viewer
		int faded;         // skipped, faded to nothing by the distance fade
		int iconsMissing;  // icon mode: the texture could not be loaded, a chip was drawn instead
		int modelsMissing; // model mode: no registered model for the weapon, the icon style was
		                   // drawn instead for that player
	};

	// The tag's world anchor: the player's interpolated feet/origin plus kWeaponEspLegHeight -
	// mid-leg, where this ESP deliberately sits (see the header comment for why that is the
	// anchor that keeps it clear of the head-anchored ESP stack).
	void LegAnchor(const NameEsp::PlayerTag& tag, float out[3]);

	// The stats for the last Draw() call - zeros when that call drew nothing.
	const DrawStats& LastDrawStats();

	// Drop the GL half's per-client state (the fade-in ramp): the cgame shut down or the level
	// changed, so the last gathered data is about a map that no longer exists. Mirrors
	// NameEsp::ResetDrawState(); called from vmHook.cpp's level-boundary drop.
	void ResetDrawState();

	// Where the current weapon table came from, for the menu status line. Never NULL:
	// "cgame native table (scanned)" or "built-in stock 1.32 names".
	const char* TableSource();

	// How many weapons the current table lists (the menu shows it next to the source).
	int TableWeaponCount();

	// Model mode - the GL/Win32 half (weaponEsp.cpp). A no-op off Windows (the tests): the
	// portable maths above (PlanModelEntity, FindRefExportInBytes) is all the non-Windows
	// build can run, and Model mode degrades to the icon style when it cannot do its job.

	// Lazily register the current table's weapon models with the engine's renderer
	// (RE_RegisterModel through the located refexport_t), once per table generation per
	// context. Called from Draw() - at frame end, when the renderer is idle. Returns the
	// number of weapons that have a live model handle.
	int EnsureModelHandles();

	// Called from the VM dispatcher detour while a CG_R_RENDERSCENE trap is in flight,
	// BEFORE the trap runs (i.e. before R_RenderScene): pushes this frame's weapon models
	// into the scene. args is the dispatcher's argument array (args[1] = the cgame's
	// refdef). A no-op unless the feature is on in Model style, for the world scene, and a
	// model handle exists; at most once per frame.
	void OnWorldRenderScene(const int* args);

	// Drop the registered model handles: the renderer re-registers all media at a level
	// change (RE_BeginRegistration invalidates existing handles), so a handle from the
	// previous level must not be pushed. Called from vmHook.cpp's level-boundary drop.
	void ResetModelHandles();

	// One line for the menu under Model style: where the renderer table came from and how
	// many models are registered. Never NULL.
	const char* ModelStatus();

	// Draw() - the GL half, in weaponEsp.cpp. Called from the hooked SwapBuffers every frame,
	// after NameEsp::Draw() / DistanceEsp::Draw() / HealthEsp::Draw(); a no-op while the
	// feature is off or nothing is gathered. In Model style it also drives
	// EnsureModelHandles() and falls back to the icon for weapons without a model.
	void Draw();
}
