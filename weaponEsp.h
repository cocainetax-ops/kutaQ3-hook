#pragma once

// =============================================================================================== //
// kutaQ3 hook - WEAPON ESP
//
// Every other player's current weapon, at their LEG position (not the head), through walls.
// Toggled from the VISUALS tab with its own checkbox; while on, one of two styles:
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
// Where the data comes from
// -------------------------
// The weapon NUMBER is the stock networked field: the ET_PLAYER entity's entityState_t::weapon
// (BG_PlayerStateToEntityState copies ps->weapon into it, bg_misc.c). That is the very index the
// cgame uses to index its own native cg_weapons[] array - so whatever a mod numbers its weapons
// as, the ESP follows it; nothing about the ESP's weapon handling is vanilla-specific.
//
// The weapon NAME and ICON come from the cgame's own native item table:
// bg_itemlist[], the gitem_t[] global compiled into the cgame (bg_misc.c). That table is what
// the cgame's cg_weapons[] is built from - cg_weapons[W].item is the gitem_t entry of the
// weapon item, and its icon (cg_items[].icon) is registered from the entry's "icon" field.
// So the ESP reads the same names, classes and icon shader paths the cgame itself uses, and a
// total conversion with its own weapon list (renamed, renumbered or new weapons) is picked up
// as-is: the ESP resolves the snapshot's weapon index through the MOD's table, not through a
// hardcoded vanilla one.
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
	struct WeaponInfo
	{
		char name[64];
		char icon[64];
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
		int        weaponCount;
	};

	// The VISUALS-tab style radio values.
	enum Style
	{
		StyleText = 0,
		StyleIcon = 1
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

	// What Draw() did with the frame it was given (read by the menu).
	struct DrawStats
	{
		int drawn;         // tags issued to GL (text: shadow + glyphs, icon: outline + quad)
		int inView;        // ... projected unclamped
		int edge;          // ... clamped to the viewport edge, dimmed
		int behind;        // skipped as behind the viewer
		int faded;         // skipped, faded to nothing by the distance fade
		int iconsMissing;  // icon mode: the texture could not be loaded, a chip was drawn instead
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

	// Draw() - the GL half, in weaponEsp.cpp. Called from the hooked SwapBuffers every frame,
	// after NameEsp::Draw() / DistanceEsp::Draw() / HealthEsp::Draw(); a no-op while the
	// feature is off or nothing is gathered.
	void Draw();
}
