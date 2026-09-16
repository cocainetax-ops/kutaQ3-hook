// =============================================================================================== //
// kutaQ3 hook - WEAPON ESP, portable half (see weaponEsp.h for the overview)
//
// No <windows.h>, no GL, no Detours in here: this file is compiled into kutaQ3.dll AND into
// tests/test_nameesp.cpp, which drives the scanner with a fabricated cgame data segment and
// checks what comes out. It holds the two things the feature needs that are pure data maths:
//
//   - StockTable(): the built-in stock 1.32 (mission pack) weapon names and icon shader names,
//     the fallback when the native scan finds nothing;
//
//   - ScanRegion(): the shape scan that finds bg_itemlist[] inside the cgame's own data
//     segment and extracts, per weapon number, the pickup_name / classname and the icon shader
//     name - the same table the cgame's cg_weapons[] is built from, so a total conversion's
//     own weapon list is what the ESP ends up displaying.
// =============================================================================================== //

#include "weaponEsp.h"

#include <string.h>
#include <stdio.h>

namespace
{
	// =========================================================================================== //
	// gitem_t field offsets
	//
	// The table the scan looks for is bg_itemlist[] (bg_misc.c), an array of gitem_t compiled
	// into the cgame. gitem_t is a shared-ABI struct (bg_public.h) - no pointers-to-pointers,
	// only C-string pointers and ints - so its layout is stable across compilers on x86. Two
	// layouts are accepted:
	//
//   stock 1.32 (stride 52):
//     +0  char *classname        +24 char *icon
//     +4  char *pickup_sound     +28 char *pickup_name
//     +8  char *world_model[4]   +32 int  quantity
//                                +36 int  giType (itemType_t: IT_BAD..IT_TEAM, 0..8)
//                                +40 int  giTag  (for IT_WEAPON: the weapon_t number)
//     +44 char *precaches        +48 char *sounds
//
//   world_model[4] (offset 8, both layouts) is validated like every other pointer field - a
//   real table's entries point at model paths or NULL there - but never extracted: the ESP
//   only displays the name and the icon.
	//
	//   ioquake3 1.36+ (stride 72) - the same head plus the fields the modern SDKs added:
	//     +44 int giFlags  +48 int giFlags2  +52 char *pickup_sound2  +56 char *precaches
	//     +60 char *sounds +64 char *use_func +68 char *pmove_frame
	//
	// The entry 0 the array starts with is "leave index 0 alone" in bg_misc.c: every string
	// pointer NULL, quantity/giType/giTag all zero (precaches/sounds point at empty strings).
	// That is the prefilter the walk uses before verifying a whole run of entries.
	// =========================================================================================== //
	const int kStrideStock = 52;
	const int kStrideIoq   = 72;

	const size_t kPtrStock[] = { 0, 4, 8, 12, 16, 20, 24, 28, 44, 48 };
	const size_t kPtrIoq[]   = { 0, 4, 8, 12, 16, 20, 24, 28, 52, 56, 60, 64, 68 };

	const size_t kOffClassname   = 0;
	const size_t kOffIcon        = 24;
	const size_t kOffPickupName  = 28;
	const size_t kOffQuantity    = 32;
	const size_t kOffGiType      = 36;
	const size_t kOffGiTag       = 40;

	// itemType_t (bg_public.h): IT_BAD=0 ... IT_WEAPON=1 ... IT_TEAM=8; the modern SDKs add
	// IT_POWERUP2=9. Anything outside this range is not an item record.
	const int kGiTypeMax = 9;

	// How many entries a candidate must validate before it is accepted as the table. The stock
	// list has ~50 entries; 16 is well inside any real table and still past a fluke.
	const int kRunLength = 16;

	// A field value this large is not a quantity / tag, it is a misread pointer.
	const int kQuantityMax = 100000;
	const int kGiTagMax    = 255;

	// The longest string the scan will follow through a pointer. A gitem_t string is a
	// classname / sound / model / icon path or a short pickup phrase; MAX_QPATH is 1024, but a
	// scan that follows 1024 bytes per pointer is 10x the cost of one that follows 64.
	const int kMaxScanString = 64;

	uint32_t ReadU32(const unsigned char* p)
	{
		return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	}

	int32_t ReadI32(const unsigned char* p)
	{
		return (int32_t)ReadU32(p);
	}

	// =========================================================================================== //
	// pointer / string resolution - VM_ArgPtr spelled out, plus the string checks
	//
	// A bytecode cgame stores its globals as offsets into its hunk segment (masked by the VM's
	// dataMask); a native cgame DLL stores host pointers; the tests pass a plain host region.
	// =========================================================================================== //
	bool ResolvePtr(const unsigned char* region, size_t size, uintptr_t dataBase, uint32_t dataMask,
	               bool native, uint32_t vmAddr, const unsigned char*& outHost)
	{
		outHost = 0;
		if (vmAddr == 0)
			return true;                        // NULL is a valid gitem_t field

		uintptr_t host;
		if (native)
		{
			host = (uintptr_t)vmAddr;
			if ((uintptr_t)region > host || host + 1 > (uintptr_t)region + size)
				return false;
		}
		else
		{
			host = (uintptr_t)dataBase + (uintptr_t)(vmAddr & (uint32_t)dataMask);
			if (host < (uintptr_t)dataBase || host >= (uintptr_t)dataBase + (uintptr_t)dataMask + 1)
				return false;
			// the scan region is a slice of the segment; the pointer must land in the slice
			const uintptr_t rlo = (uintptr_t)region;
			if (host < rlo || host + 1 > rlo + size)
				return false;
		}
		outHost = (const unsigned char*)host;
		return true;
	}

	// Follow a (resolved) string pointer: NUL-terminated, at most kMaxScanString bytes, every
	// byte a visible character (32..255; high bytes are tolerated - localised pickup names).
	bool StringAt(const unsigned char* region, size_t size, const unsigned char* host)
	{
		const unsigned char* end = region + size;
		const unsigned char* p = host;
		while (p < end && p - host < kMaxScanString)
		{
			if (*p == 0)
				return true;
			if (*p < 32)
				return false;
			++p;
		}
		return false;                        // ran out of slice or hit the length cap without NUL
	}

	// One pointer field of an entry: 0, or a string that StringAt() accepts.
	bool FieldOk(const unsigned char* region, size_t size, uintptr_t dataBase, uint32_t dataMask,
	             bool native, const unsigned char* entry, size_t off)
	{
		const unsigned char* host = 0;
		if (!ResolvePtr(region, size, dataBase, dataMask, native, ReadU32(entry + off), host))
			return false;
		if (host)
			return StringAt(region, size, host);
		return true;
	}

	// =========================================================================================== //
	// entry 0 prefilter, then the full run
	// =========================================================================================== //
	bool Entry0Prefilter(const unsigned char* p, bool ioqLayout)
	{
		// the seven leading string pointers (classname, pickup_sound, world_model[4], icon) and
		// pickup_name are all NULL in the null entry - 28 bytes of zeros, the cheapest possible
		// test and the one that rejects almost every position at once
		for (size_t off = 0; off <= 28; off += 4)
			if (ReadU32(p + off) != 0)
				return false;
		if (ReadI32(p + kOffQuantity) != 0 || ReadI32(p + kOffGiType) != 0 || ReadI32(p + kOffGiTag) != 0)
			return false;
		if (ioqLayout)
		{
			// the modern head: two int flags, then the pickup_sound2 pointer slot
			if (ReadI32(p + 44) != 0 || ReadI32(p + 48) != 0)
				return false;
		}
		return true;
	}

	// Extract a gitem_t string into out, sanitising to the printable ASCII the GL::Font display
	// lists hold (32..126; the icon paths are pure ASCII, a pickup name might not be).
	bool ExtractString(const unsigned char* region, size_t size, uintptr_t dataBase, uint32_t dataMask,
	                   bool native, const unsigned char* entry, size_t off, char* out, size_t outSize)
	{
		out[0] = 0;
		const unsigned char* host = 0;
		if (!ResolvePtr(region, size, dataBase, dataMask, native, ReadU32(entry + off), host))
			return false;
		if (!host)
			return false;
		const unsigned char* end = region + size;
		size_t n = 0;
		while (host < end && *host && n + 1 < outSize && n < kMaxScanString)
		{
			const unsigned char c = *host;
			out[n++] = (c >= 32 && c <= 126) ? (char)c : '?';
			++host;
		}
		out[n] = 0;
		return out[0] != 0;
	}

	// Validate a run of kRunLength entries at `base` with the given layout; when the run passes,
	// extract the weapon entries into `out`. Returns false when any entry fails its shape.
	bool VerifyRun(const unsigned char* region, size_t size, uintptr_t dataBase, uint32_t dataMask,
	               bool native, const unsigned char* base, int stride, const size_t* ptrs,
	               size_t ptrCount, WeaponEsp::WeaponTable& out, int& itemCount)
	{
		itemCount = 0;
		for (int i = 0; i < kRunLength; ++i)
		{
			const size_t entryOff = (size_t)i * (size_t)stride;
			if (entryOff + (size_t)stride > size)
				return false;
			const unsigned char* e = base + entryOff;

			for (size_t k = 0; k < ptrCount; ++k)
				if (!FieldOk(region, size, dataBase, dataMask, native, e, ptrs[k]))
					return false;

			const int quantity = ReadI32(e + kOffQuantity);
			const int giType   = ReadI32(e + kOffGiType);
			const int giTag    = ReadI32(e + kOffGiTag);
			if (quantity < -kQuantityMax || quantity > kQuantityMax)
				return false;
			if (giType < 0 || giType > kGiTypeMax)
				return false;
			if (giTag < 0 || giTag > kGiTagMax)
				return false;

			// entry 0 is the null entry (the prefilter already checked the head of it)
			if (i == 0 && (giType != 0 || giTag != 0 || quantity != 0))
				return false;

			if (ReadU32(e + kOffClassname) != 0 || ReadU32(e + kOffPickupName) != 0)
				++itemCount;
		}

		// extract the weapons: the IT_WEAPON entries carry the weapon_t number in giTag
		int weapons = 0;
		for (int i = 0; i < kRunLength; ++i)
		{
			const unsigned char* e = base + (size_t)i * (size_t)stride;
			if (ReadI32(e + kOffGiType) != 1 /* IT_WEAPON */)
				continue;
			const int weapon = ReadI32(e + kOffGiTag);
			if (weapon < 1 || weapon >= WeaponEsp::kTableWeapons)
				continue;                     // well behaved mods stay inside their MAX_WEAPONS

			if (ExtractString(region, size, dataBase, dataMask, native, e, kOffPickupName,
	                           out.weapons[weapon].name, sizeof(out.weapons[weapon].name)))
				out.haveName[weapon] = true;
			else if (ExtractString(region, size, dataBase, dataMask, native, e, kOffClassname,
			                       out.weapons[weapon].name, sizeof(out.weapons[weapon].name)))
				out.haveName[weapon] = true;   // no pickup name: the classname ("weapon_x") still names it
		if (ExtractString(region, size, dataBase, dataMask, native, e, kOffIcon,
		                   out.weapons[weapon].icon, sizeof(out.weapons[weapon].icon)))
			out.haveIcon[weapon] = true;

		if (out.haveName[weapon] || out.haveIcon[weapon])
				++weapons;
		}

		out.weaponCount = weapons;

		// acceptance: a real table always has several weapons, and several named entries at all
		return weapons >= 3 && itemCount >= 4;
	}
}

// =============================================================================================== //

static void CopyStr(char* dest, size_t destSize, const char* src)
{
	if (!dest || destSize == 0)
		return;
	size_t n = 0;
	for (; src && src[n] && n + 1 < destSize; ++n)
		dest[n] = src[n];
	dest[n] = 0;
}

void WeaponEsp::StockTable(WeaponTable& table)
{
	memset(&table, 0, sizeof(table));

	// Stock 1.32 / mission pack weapon_t -> (pickup name, icon shader), exactly as
	// bg_misc.c's bg_itemlist[] spells them. WP_NONE (0) deliberately has no entry: the game
	// never shows a weapon there either.
	struct
	{
		int   weapon;
		const char* name;
		const char* icon;
	}
	stock[] =
	{
		{ 1,  "Gauntlet",         "icons/iconw_gauntlet" },
		{ 2,  "Machinegun",       "icons/iconw_machinegun" },
		{ 3,  "Shotgun",          "icons/iconw_shotgun" },
		{ 4,  "Grenade Launcher", "icons/iconw_grenade" },
		{ 5,  "Rocket Launcher",  "icons/iconw_rocket" },
		{ 6,  "Lightning Gun",    "icons/iconw_lightning" },
		{ 7,  "Railgun",          "icons/iconw_railgun" },
		{ 8,  "Plasma Gun",       "icons/iconw_plasma" },
		{ 9,  "BFG10K",           "icons/iconw_bfg" },
		{ 10, "Grappling Hook",   "icons/iconw_grapple" },
		{ 11, "Nailgun",          "icons/iconw_nailgun" },
		{ 12, "Prox Launcher",    "icons/iconw_proxlauncher" },
		{ 13, "Chaingun",         "icons/iconw_chaingun" },
	};

	for (size_t i = 0; i < sizeof(stock) / sizeof(stock[0]); ++i)
	{
		CopyStr(table.weapons[stock[i].weapon].name, sizeof(table.weapons[stock[i].weapon].name),
		        stock[i].name);
		CopyStr(table.weapons[stock[i].weapon].icon, sizeof(table.weapons[stock[i].weapon].icon),
		        stock[i].icon);
		table.haveName[stock[i].weapon] = true;
		table.haveIcon[stock[i].weapon] = true;
		++table.weaponCount;
	}
}

bool WeaponEsp::ScanRegion(const unsigned char* region, size_t size, uintptr_t dataBase,
                           uint32_t dataMask, bool native, ScanResult& out)
{
	memset(&out, 0, sizeof(out));
	if (!region || size < (size_t)kRunLength * kStrideStock)
		return false;

	// The stock layout first: retail 1.32 and every 1.32-based total conversion compile
	// bg_misc.c with the 52-byte gitem_t.
	for (uintptr_t off = 0; off + kStrideStock <= size; off += 4)
	{
		if (!Entry0Prefilter(region + off, false))
			continue;
		WeaponTable table;
		memset(&table, 0, sizeof(table));
		int count = 0;
		if (VerifyRun(region, size, dataBase, dataMask, native, region + off, kStrideStock,
		              kPtrStock, sizeof(kPtrStock) / sizeof(kPtrStock[0]), table, count))
		{
			out.found     = true;
			out.stride    = kStrideStock;
			out.itemCount = count;
			out.offset    = off;
			out.table     = table;
			return true;
		}
	}

	// The modern ioquake3 layout (72-byte gitem_t) - cgames built against the 1.36+ SDKs.
	for (uintptr_t off = 0; off + kStrideIoq <= size; off += 4)
	{
		if (!Entry0Prefilter(region + off, true))
			continue;
		WeaponTable table;
		memset(&table, 0, sizeof(table));
		int count = 0;
		if (VerifyRun(region, size, dataBase, dataMask, native, region + off, kStrideIoq,
		              kPtrIoq, sizeof(kPtrIoq) / sizeof(kPtrIoq[0]), table, count))
		{
			out.found     = true;
			out.stride    = kStrideIoq;
			out.itemCount = count;
			out.offset    = off;
			out.table     = table;
			return true;
		}
	}

	return false;
}

bool WeaponEsp::WeaponName(const WeaponTable& table, int weapon, char* out, size_t outSize)
{
	if (out && outSize)
		out[0] = 0;
	if (weapon <= 0)
		return false;                        // WP_NONE: the game shows no weapon there either
	if (weapon >= kTableWeapons)
	{
		snprintf(out, outSize, "W%d", weapon);
		return true;
	}
	if (table.haveName[weapon] && table.weapons[weapon].name[0])
	{
		strncpy(out, table.weapons[weapon].name, outSize - 1);
		out[outSize - 1] = 0;
		return true;
	}
	snprintf(out, outSize, "W%d", weapon);
	return true;
}

bool WeaponEsp::WeaponIcon(const WeaponTable& table, int weapon, char* out, size_t outSize)
{
	if (out && outSize)
		out[0] = 0;
	if (weapon < 0 || weapon >= kTableWeapons)
		return false;
	if (table.haveIcon[weapon] && table.weapons[weapon].icon[0])
	{
		strncpy(out, table.weapons[weapon].icon, outSize - 1);
		out[outSize - 1] = 0;
		return true;
	}
	return false;
}

void WeaponEsp::LegAnchor(const NameEsp::PlayerTag& tag, float out[3])
{
	// mid-leg: the standing bbox spans origin z -24..+32 (MINS_Z / bg_pmove.c), so 8 units up
	// is the knee line - clearly on the legs, and a whole model height away from the head
	// anchor the other ESPs stack above.
	out[0] = tag.lerpOrigin[0];
	out[1] = tag.lerpOrigin[1];
	out[2] = tag.lerpOrigin[2] + q3::kWeaponEspLegHeight;
}
