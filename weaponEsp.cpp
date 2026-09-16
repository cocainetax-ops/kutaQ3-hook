// =============================================================================================== //
// kutaQ3 hook - WEAPON ESP, GL half (see weaponEsp.h for the overview)
//
// Renders every other player's current weapon at their leg position, from the hooked
// SwapBuffers in main.cpp, after the name / distance / health overlays. Text mode draws the
// weapon's name in the GL::Font display-list faces (glText.h) the other ESPs use; icon mode
// draws the cgame's own item icon for that weapon, loaded out of the game's pak files and
// projected onto the 3D leg anchor. Both scale down and fade out with |vieworg - lerpOrigin|,
// the same ramp as the DISTANCE and HEALTH ESPs (DistanceEsp::DistanceFade).
//
// This file is the Win32 half, like nameEsp.cpp: the portable table maths (the bg_itemlist
// shape scan, the stock fallback, the anchor) lives in weaponEspCore.cpp. Everything that
// touches Windows (the cgame data segment walk, the pak / ZIP / TGA icon loading) is guarded
// by _WIN32; the test build (tests/stub) compiles the same Draw() against a stock table with
// no icons available, so the drawing path - projection, anchor, stacking, fade, stats - is
// exercised off Windows exactly like the other ESPs.
// =============================================================================================== //

#include "weaponEsp.h"
#include "config.h"        // Config::g_Settings.weaponEsp / weaponEspStyle
#include "glText.h"        // GL::Font, FONT_HEIGHT
#include "glDraw.h"        // GL::SetupOrtho / GL::RestoreGL / the rect helpers
#include "glStateGuard.h"  // KUTAQ3_LEGACY_GL_STATE_GUARD
#include <gl/GL.h>
#include <stdlib.h>        // malloc / free (the pak + TGA buffers)
#include <stdint.h>
#include <string.h>        // memset / strcmp (the model cache)

#if defined(_WIN32)
#include "main.h"          // Log()
#include "vmHook.h"        // Vm::DataSegment / VmIdentity - the cgame data to scan
#endif

namespace
{
	// =========================================================================================== //
	// text faces - the same fixed-face trick as the distance ESP: a GL::Font bakes ONE face into
	// its display list and glBitmap glyphs render in window pixels, so "scaling" means picking
	// the pre-baked face nearest FONT_HEIGHT * scale. The buckets span the fade's kMinScale..1
	// (5px == 14 * 0.357 ~= kMinScale). Rebuilt after a GL context change like the other fonts.
	// =========================================================================================== //
	const float kFontBuckets[] = { 14.0f, 12.0f, 10.0f, 8.0f, 6.0f, 5.0f };
	const int   kBucketCount   = 6;

	int BucketForScale(float scale)
	{
		const float target = (float)FONT_HEIGHT * scale;
		int best = 0;
		for (int i = 1; i < kBucketCount; ++i)
			if (fabsf(kFontBuckets[i] - target) < fabsf(kFontBuckets[best] - target))
				best = i;
		return best;
	}

	// One pre-baked face per bucket, rebuilt when the GL context changes (hdc) - the same
	// invalidation the distance ESP's faces use.
	GL::Font s_fonts[kBucketCount];

	// =========================================================================================== //
	// the weapon table
	// =========================================================================================== //
	WeaponEsp::WeaponTable s_table;
	bool s_tableNative = false;   // true: extracted from the cgame's own bg_itemlist (the table
	                             // cg_weapons[] is built from); false: built-in stock 1.32
	bool      s_tableForVm = false;    // a scan (or fallback) has been applied for that instance
#if defined(_WIN32)
	// The cgame VM instance the scan result belongs to. A level change reuses the instance
	// (same table), a reconnect or mod change gets a new one and triggers a rescan.
	uintptr_t s_tableVmBase = 0;
	uint32_t  s_tableVmMask = 0;
	uintptr_t s_tableVmDll  = 0;
#endif

	// =========================================================================================== //
	// icon textures (icon mode)
	// =========================================================================================== //
	struct IconTex
	{
		char   icon[64];   // the icon shader name this texture was loaded from ("" = empty)
		GLuint tex;
		int    w, h;
		bool   valid;
		int    triedGen;   // the table generation this slot was last loaded / checked under - a
		                   // failed lookup is not repeated every frame (the file does not
		                   // appear by itself); a new table or a new GL context earns a retry
	};
	IconTex s_icons[WeaponEsp::kTableWeapons];
	int s_tableGen = 0;    // bumped whenever the weapon table is (re)applied - the cgame changed
	                     // (the model handle cache keys off it too, so it lives in the shared
	                     // half even though only the _WIN32 code bumps it)
#if defined(_WIN32)
	HDC s_iconHdc = 0;   // the GL context the textures live on; a new one (vid_restart)
	                     // invalidates every id - the game destroys the context, not the ids

	void CopyIconName(char* dest, size_t destSize, const char* src)
	{
		size_t n = 0;
		for (; src && src[n] && n + 1 < destSize; ++n)
			dest[n] = src[n];
		dest[n] = 0;
	}
#endif

	// =========================================================================================== //
	// per-client draw state - the fade-in ramp. Same shape and intent as the other ESPs: the
	// tags are rebuilt every frame; what survives is how far a tag has ramped up, keyed by
	// client number, dropped with the level.
	// =========================================================================================== //
	const int kFadeInMs         = 220;
	const int kMaxClockStepMs   = 250;   // a hitch is not a slow-motion ramp
	const int kMinClockStepMs   = 16;    // ... and a stalled clock must not stall the ramp

	struct TagState
	{
		int   clientNum;      // -1: slot free
		float alpha;          // 0..1 fade-in
		int   steppedAt;      // the Draw() that last advanced this slot (s_frameSerial)
	};

	TagState s_tags[q3::kMaxClients];
	int      s_clock       = 0;
	bool     s_haveClock   = false;
	int      s_frameSerial = 0;

	WeaponEsp::DrawStats s_stats;

	// =========================================================================================== //
	// model mode (3D): the registered weapon models + the per-frame injection state.
	//
	// s_models[w].handle is a qhandle from the ENGINE's renderer (RE_RegisterModel through the
	// located refexport_t), for the model path s_models[w].path, taken under table generation
	// s_models[w].triedGen. A handle is only ever pushed into a scene when it was taken under
	// the CURRENT table generation - a dead handle is an out-of-bounds model index inside the
	// renderer. The handles die when the renderer tears down its media (a vid_restart), which
	// is detected the same way the icon textures detect it: the GL context changed.
	// =========================================================================================== //
	struct ModelRec
	{
		char path[128];   // the table's model path this handle was registered for ("" = none)
		int  handle;      // qhandle, 0 = none
		int  triedGen;    // the table generation the lookup for this weapon last ran under
	};
	ModelRec s_models[WeaponEsp::kTableWeapons];

	// Set by OnWorldRenderScene() once this frame's models have been pushed into the scene;
	// Draw() (frame end) clears it for the next frame.
	bool s_injectedThisFrame = false;

	// What the last frame's scene received (the menu's "N weapon models in the scene").
	int  s_modelInjected    = 0;

	bool ModelReady(int weapon)
	{
		// The overlay draws nothing for a player whose model is handled by the scene: the tag
		// is "up" when the scene has (or had, this frame) its model.
		if (weapon < 0 || weapon >= WeaponEsp::kTableWeapons)
			return false;
		const ModelRec& rec = s_models[weapon];
		return rec.handle > 0 && rec.triedGen == s_tableGen;
	}

	void ResetTagState()
	{
		for (int i = 0; i < q3::kMaxClients; ++i)
		{
			s_tags[i].clientNum = -1;
			s_tags[i].alpha     = 0.0f;
			s_tags[i].steppedAt = 0;
		}
		s_clock     = 0;
		s_haveClock = false;
	}

	TagState& TagSlot(int clientNum)
	{
		int freeSlot = -1;
		for (int i = 0; i < q3::kMaxClients; ++i)
		{
			if (s_tags[i].clientNum == clientNum)
				return s_tags[i];
			if (s_tags[i].clientNum < 0 && freeSlot < 0)
				freeSlot = i;
		}
		if (freeSlot < 0)
			freeSlot = 0;
		s_tags[freeSlot].clientNum = clientNum;
		s_tags[freeSlot].alpha     = 0.0f;
		s_tags[freeSlot].steppedAt = 0;
		return s_tags[freeSlot];
	}

	int ClockStepMs(int serverTime)
	{
		if (s_haveClock && serverTime < s_clock)   // backwards clock: a new level
		{
			ResetTagState();
			s_clock     = serverTime;
			s_haveClock = true;
			return kMinClockStepMs;
		}
		int step = s_haveClock ? (serverTime - s_clock) : 0;
		s_clock     = serverTime;
		s_haveClock = true;
		if (step < kMinClockStepMs) step = kMinClockStepMs;
		if (step > kMaxClockStepMs) step = kMaxClockStepMs;
		return step;
	}

	// Off-screen tags are clamped to the viewport edge; dimming them says "this one is not
	// where the tag is" (same 55% dim as the other ESPs).
	void Dim(unsigned char rgb[3])
	{
		rgb[0] = (unsigned char)(rgb[0] * 55 / 100);
		rgb[1] = (unsigned char)(rgb[1] * 55 / 100);
		rgb[2] = (unsigned char)(rgb[2] * 55 / 100);
	}
}

// =============================================================================================== //
#if defined(_WIN32)
namespace
{
	// =========================================================================================== //
	// the cgame's writable data, walked for bg_itemlist[]
	//
	// Same memory-walking rules as vmHook.cpp's scans: only committed, readable, writable
	// regions inside the cgame's own range (its hunk segment for a bytecode VM, its image for a
	// native DLL) are touched. The walk runs ONCE per cgame VM instance - the first frame the
	// feature draws - and costs a few milliseconds; when it finds nothing the built-in stock
	// table is used instead, so there is no retry loop.
	// =========================================================================================== //
	template <typename Visitor>
	void ForEachWritableRegion(uintptr_t low, uintptr_t high, Visitor visit)
	{
		MEMORY_BASIC_INFORMATION info;
		uintptr_t address = low;
		while (address < high)
		{
			if (!VirtualQuery((LPCVOID)address, &info, sizeof(info)))
				return;

			const uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize;
			const bool writable = (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
			                                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
			const bool guarded  = (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0;

			const uintptr_t useStart = ((uintptr_t)info.BaseAddress > low) ? (uintptr_t)info.BaseAddress : low;
			const uintptr_t useEnd   = (end < high) ? end : high;

			if (info.State == MEM_COMMIT && writable && !guarded && useEnd > useStart)
				visit((const unsigned char*)useStart, (size_t)(useEnd - useStart));

			if (end <= address)
				return;
			address = end;
		}
	}

	void EnsureWeaponTable()
	{
		uintptr_t dataBase = 0, dllHandle = 0;
		uint32_t  dataMask = 0;
		const bool haveVm = Vm::VmIdentity(dataBase, dataMask, dllHandle);
		if (haveVm && s_tableForVm &&
		    s_tableVmBase == dataBase && s_tableVmMask == dataMask && s_tableVmDll == dllHandle)
			return;                          // already scanned for this cgame instance

		bool found = false;
		if (haveVm)
		{
			uintptr_t low = 0, high = 0;
			if (Vm::DataSegment(low, high))
			{
				ForEachWritableRegion(low, high, [&](const unsigned char* region, size_t size)
				{
					if (found)
						return;
					WeaponEsp::ScanResult result;
					if (WeaponEsp::ScanRegion(region, size, dataBase, dataMask, dllHandle != 0, result))
					{
						s_table          = result.table;
						s_tableNative    = true;
						Log("[kutaQ3] weapon table: cgame bg_itemlist at %p (stride %d, %d weapons)",
						    (const void*)(region + result.offset), result.stride, s_table.weaponCount);
						found = true;
					}
				});
			}
		}

		if (found)
		{
			s_tableVmBase  = dataBase;
			s_tableVmMask  = dataMask;
			s_tableVmDll   = dllHandle;
			s_tableForVm   = true;
			++s_tableGen;                    // the icon cache now resolves against a new table
			return;
		}

		// No cgame VM, or the table is not recognisable (a cgame built against a different
		// bg_public.h): the ESP still works on vanilla servers through the built-in table.
		WeaponEsp::StockTable(s_table);
		s_tableNative = false;
		s_tableVmBase = dataBase;
		s_tableVmMask = dataMask;
		s_tableVmDll  = dllHandle;
		s_tableForVm  = true;
		++s_tableGen;
	}

	// =========================================================================================== //
	// pak (ZIP) + TGA icon loading
	//
	// The icon shader name the table carries ("icons/iconw_gauntlet") is a path relative to the
	// game directory; the cgame registered the same path as a shader, so the file it resolves
	// to is exactly the artwork the mod uses. The paks (baseq3/pak0.pk3, the mod's paks) are
	// plain ZIP archives - central directory first, then one read of the entry.
	// =========================================================================================== //
	uint16_t Rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
	uint32_t Rd32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	                                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

	struct PakFile
	{
		HANDLE hFile;
		bool   open;
	};

	bool PakOpen(const char* path, PakFile& pak)
	{
		pak.hFile = INVALID_HANDLE_VALUE;
		pak.open  = false;
		pak.hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (pak.hFile == INVALID_HANDLE_VALUE)
			return false;
		pak.open = true;
		return true;
	}

	void PakClose(PakFile& pak)
	{
		if (pak.open)
			CloseHandle(pak.hFile);
		pak.open = false;
	}

	bool PakSeek(PakFile& pak, DWORD offset)
	{
		return SetFilePointer(pak.hFile, offset, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
	}

	bool PakRead(PakFile& pak, void* dest, DWORD count)
	{
		DWORD got = 0;
		const BOOL ok = ReadFile(pak.hFile, dest, count, &got, NULL);
		return ok && got == count;
	}

	// RtlDecompressBuffer from ntdll (no import needed): ZIP method 8 is a zlib stream, which
	// is exactly what the NATIVE compress format expects.
	bool InflateDeflate(const unsigned char* src, size_t srcLen, unsigned char* dest, size_t destLen)
	{
		typedef NTSTATUS (WINAPI *RtlDecompressBuffer_t)(int, PCHAR, ULONG, PCHAR, ULONG, PULONG);
		static RtlDecompressBuffer_t decompress = 0;
		if (!decompress)
		{
			HMODULE ntdll = GetModuleHandleA("ntdll.dll");
			if (!ntdll)
				return false;
			decompress = (RtlDecompressBuffer_t)GetProcAddress(ntdll, "RtlDecompressBuffer");
		}
		if (!decompress)
			return false;

		ULONG outSize = 0;
		const NTSTATUS status = decompress(0 /* CompressFormatNative */, (PCHAR)dest, (ULONG)destLen,
		                                   (PCHAR)src, (ULONG)srcLen, &outSize);
		if (status != 0)
			return false;
		return outSize <= destLen;
	}

	// Find `entry` (e.g. "icons/iconw_gauntlet.tga") in the pak's central directory and read
	// its bytes into *out (malloc'd; the caller frees). An exact name match wins, else the
	// first case-insensitive one - like the engine's fs search.
	bool PakFindEntry(PakFile& pak, const char* entry, unsigned char** out, size_t* outLen)
	{
		*out   = 0;
		*outLen = 0;

		LARGE_INTEGER sizeLi;
		if (!GetFileSizeEx(pak.hFile, &sizeLi) || sizeLi.QuadPart < 22)
			return false;
		const uint64_t fileSize = (uint64_t)sizeLi.QuadPart;

		// end of central directory: 22 bytes + a comment < 65536 bytes long, at the tail
		const size_t tail = (size_t)(fileSize < (uint64_t)(65535 + 22)
		                            ? fileSize : (uint64_t)(65535 + 22));
		unsigned char tailBuf[65535 + 22];
		if (!PakSeek(pak, (DWORD)(fileSize - tail)) || !PakRead(pak, tailBuf, (DWORD)tail))
			return false;

		const unsigned char* eocd = 0;
		for (size_t off = tail - 22; off > 0; --off)
		{
			if (tailBuf[off] == 'P' && tailBuf[off + 1] == 'K' &&
			    tailBuf[off + 2] == 0x05 && tailBuf[off + 3] == 0x06)
			{
				eocd = tailBuf + off;
				break;
			}
		}
		if (!eocd)
			return false;

		const uint16_t entryCount = Rd16(eocd + 10);
		const uint32_t cdSize     = Rd32(eocd + 12);
		const uint32_t cdOffset   = Rd32(eocd + 16);
		if (entryCount == 0 || cdSize == 0)
			return false;
		if ((uint64_t)cdOffset + cdSize > fileSize)
			return false;
		if (cdSize > (4u * 1024u * 1024u))
			return false;                      // no real pak's central directory is that big
		unsigned char* cd = (unsigned char*)malloc(cdSize);
		if (!cd)
			return false;
		const bool gotCd = PakSeek(pak, cdOffset) && PakRead(pak, cd, cdSize);
		if (!gotCd)
		{
			free(cd);
			return false;
		}

		const size_t wantLen = strlen(entry);
		const unsigned char* bestRec  = 0;      // exact match
		const unsigned char* bestCiRec = 0;     // first full case-insensitive match

		const unsigned char* p = cd;
		const unsigned char* cdEndPtr = cd + cdSize;
		for (uint16_t i = 0; i < entryCount && p + 46 <= cdEndPtr; ++i)
		{
			if (p[0] != 'P' || p[1] != 'K' || p[2] != 0x01 || p[3] != 0x02)
				break;
			const uint16_t nameLen    = Rd16(p + 28);
			const uint16_t extraLen   = Rd16(p + 30);
			const uint16_t commentLen = Rd16(p + 32);
			const unsigned char* name = p + 46;
			if (name + nameLen > cdEndPtr)
				break;

			bool exact = (nameLen == (uint16_t)wantLen);
			bool ci    = (nameLen == (uint16_t)wantLen);
			for (uint16_t k = 0; exact && k < nameLen; ++k)
				if (name[k] != entry[k])
					exact = false;
			for (uint16_t k = 0; ci && k < nameLen; ++k)
			{
				const char a = name[k];
				const char b = entry[k];
				const char la = (a >= 'A' && a <= 'Z') ? (char)(a + 32) : a;
				const char lb = (b >= 'A' && b <= 'Z') ? (char)(b + 32) : b;
				if (la != lb)
					ci = false;
			}
			if (exact)
			{
				bestRec = p;
				break;
			}
			if (ci && !bestCiRec)
				bestCiRec = p;

			p += 46 + nameLen + extraLen + commentLen;
		}

		const unsigned char* rec = bestRec ? bestRec : bestCiRec;
		if (!rec)
		{
			free(cd);
			return false;
		}

		const uint16_t method     = Rd16(rec + 10);
		const uint32_t compSize   = Rd32(rec + 20);
		const uint32_t uncompSize = Rd32(rec + 24);
		const uint32_t localOff   = Rd32(rec + 42);
		free(cd);
		if (uncompSize == 0 || uncompSize > (16u * 1024u * 1024u) ||
		    (uint64_t)localOff + 30 >= fileSize)
			return false;

		// the local header repeats the name/extra lengths - the data starts after them
		unsigned char local[30];
		if (!PakSeek(pak, localOff) || !PakRead(pak, local, 30) ||
		    local[0] != 'P' || local[1] != 'K' || local[2] != 0x03 || local[3] != 0x04)
			return false;
		const DWORD dataOff = localOff + 30 + Rd16(local + 26) + Rd16(local + 28);
		if ((uint64_t)dataOff + compSize > fileSize)
			return false;

		unsigned char* raw = (unsigned char*)malloc(compSize ? compSize : 1);
		if (!raw)
			return false;
		if (!PakSeek(pak, dataOff) || !PakRead(pak, raw, compSize))
		{
			free(raw);
			return false;
		}

		bool ok = false;
		if (method == 0)
		{
			*out = raw;
			*outLen = compSize;
			ok = (compSize == uncompSize);
		}
		else if (method == 8)
		{
			unsigned char* inflated = (unsigned char*)malloc(uncompSize);
			if (inflated && InflateDeflate(raw, compSize, inflated, uncompSize))
			{
				*out   = inflated;
				*outLen = uncompSize;
				ok = true;
			}
			else
			{
				free(inflated);
			}
		}
		free(raw);
		if (!ok)
		{
			free(*out);
			*out   = 0;
			*outLen = 0;
		}
		return ok;
	}

	// =========================================================================================== //
	// TGA decode: the .tga artwork the cgame registered for the icon. Q3 icons are 32bpp (the
	// classic ARGB byte order) or 24bpp, top-down or bottom-up; 8/16bpp are accepted too. The
	// output is always a top-down RGBA buffer for glTexImage2D.
	// =========================================================================================== //
	bool DecodeTga(const unsigned char* d, size_t len, unsigned char** out, int* outW, int* outH)
	{
		*out = 0;
		if (len < 18)
			return false;

		const int idLen    = d[0];
		const int cmapType = d[1];
		const int imgType  = d[2];
		const int bpp      = d[14];
		const int desc     = d[15];
		if (cmapType != 0 || (imgType != 2 && imgType != 3) ||
		    (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
			return false;

		const int w = (int)(d[12] | (d[13] << 8));
		const int h = (int)(d[14] | (d[15] << 8));
		if (w < 1 || w > 512 || h < 1 || h > 512)
			return false;
		if ((size_t)(18 + idLen) + (size_t)w * h * (bpp / 8) > len)
			return false;

		const bool topDown = (desc & 0x20) != 0;
		const unsigned char* px = d + 18 + idLen;

		unsigned char* rgba = (unsigned char*)malloc((size_t)w * h * 4);
		if (!rgba)
			return false;

		// 32bpp: Q3 TGA writers differ on the channel order - the classic Q3 convention is
		// ARGB, some mod tools write RGBA. Detect it statistically: on an icon most pixels
		// are transparent, so which byte is 0 for the majority decides.
		int alphaFirst = 0, rgbaFirst = 0;
		if (bpp == 32)
		{
			const int probe = w * h < 4096 ? w * h : 4096;
			for (int i = 0; i < probe; ++i)
			{
				const unsigned char* s = px + i * 4;
				if (s[0] == 0 && s[3] != 0) ++alphaFirst;
				else if (s[3] == 0 && s[0] != 0) ++rgbaFirst;
			}
		}
		const bool argb = (bpp == 32) && (alphaFirst > rgbaFirst);

		for (int y = 0; y < h; ++y)
		{
			const int row = topDown ? y : (h - 1 - y);
			const unsigned char* s = px + (size_t)y * w * (bpp / 8);
			unsigned char* o = rgba + (size_t)row * w * 4;
			for (int x = 0; x < w; ++x, ++o)
			{
				switch (bpp)
				{
				case 8:
				{
					const int v = s[x];
					o[0] = o[1] = o[2] = (unsigned char)v;
					o[3] = 255;
					break;
				}
				case 16:
				{
					const uint16_t v = (uint16_t)(s[x * 2] | (s[x * 2 + 1] << 8));
					o[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
					o[1] = (unsigned char)(((v >> 5) & 31) * 255 / 31);
					o[2] = (unsigned char)((v & 31) * 255 / 31);
					o[3] = 255;
					break;
				}
				case 24:
				{
					o[0] = s[x * 3 + 2];   // stored BGR
					o[1] = s[x * 3 + 1];
					o[2] = s[x * 3];
					o[3] = 255;
					break;
				}
				case 32:
				{
					if (argb)
					{
						o[0] = s[x * 4 + 1];
						o[1] = s[x * 4 + 2];
						o[2] = s[x * 4 + 3];
						o[3] = s[x * 4];
					}
					else
					{
						o[0] = s[x * 4];
						o[1] = s[x * 4 + 1];
						o[2] = s[x * 4 + 2];
						o[3] = s[x * 4 + 3];
					}
					break;
				}
				}
			}
		}

		*out  = rgba;
		*outW = w;
		*outH = h;
		return true;
	}

	// =========================================================================================== //
	// finding the icon file: the mod's paks first (they override baseq3, the same load order
	// the engine's fs uses), then baseq3, then loose .tga files.
	// =========================================================================================== //
	bool LoadPakBytes(const char* dir, const char* file, unsigned char** out, size_t* outLen)
	{
		PakFile pak;
		char path[800];
		if (snprintf(path, sizeof(path), "%s\\%s", dir, file) < 0)
			return false;
		if (!PakOpen(path, pak))
			return false;
		const bool ok = PakFindEntry(pak, file, out, outLen);
		PakClose(pak);
		return ok;
	}

	bool LoadLooseFile(const char* path, unsigned char** out, size_t* outLen)
	{
		*out   = 0;
		*outLen = 0;
		HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		LARGE_INTEGER sizeLi;
		bool ok = false;
		if (GetFileSizeEx(h, &sizeLi) && sizeLi.QuadPart > 0 &&
		    sizeLi.QuadPart < (16u * 1024u * 1024u))
		{
			unsigned char* buf = (unsigned char*)malloc((size_t)sizeLi.QuadPart);
			if (buf)
			{
				DWORD got = 0;
				if (SetFilePointer(h, 0, NULL, FILE_BEGIN) == 0 &&
				    ReadFile(h, buf, (DWORD)sizeLi.QuadPart, &got, NULL) &&
				    got == (DWORD)sizeLi.QuadPart)
				{
					*out   = buf;
					*outLen = (size_t)sizeLi.QuadPart;
					ok = true;
				}
				else
				{
					free(buf);
				}
			}
		}
		CloseHandle(h);
		return ok;
	}

	// Load the .tga for an icon shader name ("icons/iconw_gauntlet") out of the game's paks or
	// loose files. malloc'd on success; the caller frees.
	bool LoadIconFile(const char* shaderName, unsigned char** out, size_t* outLen)
	{
		char file[160];
		if (strrchr(shaderName, '.'))
		{
			// already a filename (a mod spells the icon with an extension)
			if (snprintf(file, sizeof(file), "%s", shaderName) < 0)
				return false;
		}
		else
		{
			if (snprintf(file, sizeof(file), "%s.tga", shaderName) < 0)
				return false;
		}

		static char gameDir[520];
		if (gameDir[0] == 0)
		{
			GetModuleFileNameA(NULL, gameDir, sizeof(gameDir) - 1);
			for (char* p = gameDir; *p; ++p)
				if (*p == '\\')
				{
					p[1] = 0;
					break;
				}
		}

		// every subdirectory of the game dir is a candidate mod dir; the engine loads them on
		// top of baseq3, so they take precedence
		struct ModDir
		{
			char path[560];
		}
		mods[32];
		int modCount = 0;
		{
			char pattern[544];
			snprintf(pattern, sizeof(pattern), "%s\\*", gameDir);
			WIN32_FIND_DATAA fd;
			HANDLE find = FindFirstFileA(pattern, &fd);
			if (find != INVALID_HANDLE_VALUE)
			{
				do
				{
					if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
						continue;
					if (modCount < 32 && _stricmp(fd.cFileName, "baseq3") != 0)
					{
						if (snprintf(mods[modCount].path, sizeof(mods[modCount].path),
						              "%s%s", gameDir, fd.cFileName) >= 0)
							++modCount;
					}
				}
				while (FindNextFileA(find, &fd) && modCount < 32);
				FindClose(find);
			}
		}

		unsigned char* bytes = 0;
		size_t len = 0;

		for (int m = 0; m < modCount; ++m)
		{
			if (LoadPakBytes(mods[m].path, file, &bytes, &len))
				return true;
		}

		// baseq3, then the game dir itself (loose files at the quake3.exe level)
		const char* roots[2] = { "baseq3", "" };
		for (int r = 0; r < 2; ++r)
		{
			char root[600];
			snprintf(root, sizeof(root), "%s%s", gameDir, roots[r]);

			// paks inside the dir, highest number first: the engine loads pakN in ascending
			// order, so the highest-numbered pak is the last one loaded and wins
			char paks[16][800];
			int pakCount = 0;
			{
				char pattern[640];
				snprintf(pattern, sizeof(pattern), "%s\\*.pk3", root);
				WIN32_FIND_DATAA fd;
				HANDLE find = FindFirstFileA(pattern, &fd);
				if (find != INVALID_HANDLE_VALUE)
				{
					do
					{
						if (pakCount < 16)
						{
							if (snprintf(paks[pakCount], sizeof(paks[pakCount]), "%s\\%s",
							            root, fd.cFileName) >= 0)
								++pakCount;
						}
					}
					while (FindNextFileA(find, &fd) && pakCount < 16);
					FindClose(find);
				}
			}
			for (int p = pakCount - 1; p >= 0; --p)
			{
				if (LoadPakBytes(paks[p], file, &bytes, &len))
					return true;
			}

			{
				char loose[800];
				snprintf(loose, sizeof(loose), "%s\\%s", root, file);
				if (LoadLooseFile(loose, &bytes, &len))
					return true;
			}
		}

		return false;
	}

	// Upload the decoded icon as a GL texture on the current context.
	bool UploadIcon(int weapon, const char* icon, const unsigned char* rgba, int w, int h)
	{
		IconTex& slot = s_icons[weapon];

		GLuint tex = 0;
		{
			KUTAQ3_LEGACY_GL_STATE_GUARD();
			// Real <GL/gl.h> signature: void glGenTextures(GLsizei, GLuint*) - the id comes out
			// through the pointer, 0 on failure (the stub in tests/stub/gl/GL.h mirrors this).
			glGenTextures(1, &tex);
			if (tex)
			{
				glBindTexture(GL_TEXTURE_2D, tex);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
			}
		}
		if (!tex)
			return false;

		CopyIconName(slot.icon, sizeof(slot.icon), icon);
		slot.tex      = tex;
		slot.w        = w;
		slot.h        = h;
		slot.valid    = true;
		slot.triedGen = s_tableGen;
		s_iconHdc     = wglGetCurrentDC();
		return true;
	}

	// =========================================================================================== //
	// EnsureIconTexture(weapon): the loaded texture for this weapon's icon, or false when the
	// icon does not exist (the caller draws the chip). Loaded once per weapon per table per
	// context - a failed lookup is remembered (triedGen) so a missing file does not cost a
	// directory walk and a pak open every frame.
	// =========================================================================================== //
	bool EnsureIconTexture(int weapon, IconTex& out)
	{
		out = s_icons[weapon];
		if (s_iconHdc && s_iconHdc != wglGetCurrentDC())
		{
			// vid_restart destroyed the context: every id is stale, and a fresh retry is earned
			for (int i = 0; i < WeaponEsp::kTableWeapons; ++i)
			{
				s_icons[i].valid    = false;
				s_icons[i].triedGen = 0;
			}
			s_iconHdc = 0;
			out.valid    = false;
			out.triedGen = 0;
		}

		char icon[64];
		if (!WeaponEsp::WeaponIcon(s_table, weapon, icon, sizeof(icon)))
			return false;
		if (out.valid && _stricmp(out.icon, icon) == 0)
			return true;               // already loaded for this exact icon, this table, this context
		if (out.triedGen == s_tableGen)
			return false;              // looked for it under this table already: it is not there

		unsigned char* tga = 0;
		size_t len = 0;
		if (!LoadIconFile(icon, &tga, &len))
		{
			s_icons[weapon].triedGen = s_tableGen;
			return false;
		}

		unsigned char* rgba = 0;
		int w = 0, h = 0;
		const bool decoded = DecodeTga(tga, len, &rgba, &w, &h);
		free(tga);
		if (!decoded)
		{
			s_icons[weapon].triedGen = s_tableGen;
			return false;
		}

		const bool uploaded = UploadIcon(weapon, icon, rgba, w, h);
		if (!uploaded)
			s_icons[weapon].triedGen = s_tableGen;
		free(rgba);
		return uploaded;
	}

	// =========================================================================================== //
	// model mode (3D) - the engine's renderer, reached the same way everything else is: by
	// shape, from the engine's own memory, with nothing called into the cgame.
	//
	// refexport_t ("re") - tr_public.h, a global in quake3.exe - is the struct of 29 renderer
	// function pointers the cgame's traps call through (RE_RegisterModel,
	// RE_AddRefEntityToScene, ...). It is located with FindRefExportInBytes(): a run of 29
	// consecutive main-module code pointers, accepted only when the cgame's own syscall
	// dispatcher (already in hand: vm->systemCall) references the candidate's slots by address.
	// =========================================================================================== //
	struct RefExport
	{
		uint32_t slot[WeaponEsp::kRefExportSlots];
	};
	RefExport* s_refExport        = NULL;   // the located table (engine memory, outlives the VM)
	bool       s_refExportTried   = false;  // a scan ran and found nothing (retried, throttled)
	DWORD      s_refExportNextTry = 0;

	// The main module's own range (quake3.exe): where the table must live and where every
	// slot must point.
	bool MainModuleRange(uintptr_t& low, uintptr_t& high)
	{
		HMODULE module = GetModuleHandle(NULL);
		if (!module)
			return false;
		const BYTE* base = (const BYTE*)module;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		low  = (uintptr_t)base;
		high = low + nt->OptionalHeader.SizeOfImage;
		return high > low;
	}

	// The first `want` bytes of the cgame's syscall dispatcher, for the slot-address check.
	// The address vm->systemCall carries is where the detour now lives (a 5-byte jump plus the
	// original prologue), but the dispatcher's body - every case thunk, with the refexport
	// slot addresses as immediates - is still in place after it, so a plain forward read is
	// exactly the code we want. Read-only walk: .text is committed, but never trust the tail.
	size_t DispatcherCode(unsigned char* buf, size_t want)
	{
		uintptr_t start = 0;
		if (!Vm::DispatcherAddress(start) || !start)
			return 0;

		size_t got = 0;
		MEMORY_BASIC_INFORMATION info;
		uintptr_t address = start;
		const uintptr_t limit = start + want;
		while (address < limit && got < want)
		{
			if (!VirtualQuery((LPCVOID)address, &info, sizeof(info)))
				break;
			const uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize;
			if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
				break;
			const uintptr_t readEnd = (end < limit) ? end : limit;
			const size_t n = (size_t)(readEnd - address);
			if (n + got > want)
				break;
#ifdef _MSC_VER
			__try
			{
				memcpy(buf + got, (const void*)address, n);
				got += n;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				break;
			}
#else
			memcpy(buf + got, (const void*)address, n);
			got += n;
#endif
			address = readEnd;
			if (end <= address)
				break;
		}
		return got;
	}

	// Locate the refexport_t once (and remember it forever: it is a global in the executable,
	// which cannot change under us). Throttled retries while nothing has been found.
	bool EnsureRefExport()
	{
		if (s_refExport)
			return true;
		const DWORD now = timeGetTime();
		if (s_refExportTried && now < s_refExportNextTry)
			return false;

		uintptr_t low = 0, high = 0;
		if (!MainModuleRange(low, high))
			return false;

		// The dispatcher's bytes, once per scan (the slot-address check is the expensive half).
		const size_t kDispBytes = 128 * 1024;
		unsigned char* disp = (unsigned char*)malloc(kDispBytes);
		if (!disp)
			return false;
		const size_t dispLen = DispatcherCode(disp, kDispBytes);
		if (dispLen < 4096)
		{
			free(disp);
			return false;
		}

		bool found = false;
		ForEachWritableRegion(low, high, [&](const unsigned char* region, size_t size)
		{
			if (found)
				return;
			uintptr_t base = 0;
			if (WeaponEsp::FindRefExportInBytes(region, size, low, high, disp, dispLen, &base))
			{
				s_refExport = (RefExport*)base;
				found = true;
				Log("[kutaQ3] renderer refexport table at %p (RegisterModel %p, AddRefEntityToScene %p)",
				    (const void*)base,
				    (void*)(uintptr_t)s_refExport->slot[WeaponEsp::kSlotRegisterModel],
				    (void*)(uintptr_t)s_refExport->slot[WeaponEsp::kSlotAddRefEntityToScene]);
			}
		});
		free(disp);

		if (found)
			return true;
		s_refExportTried   = true;
		s_refExportNextTry = now + 2000;
		return false;
	}
}

#else // !defined(_WIN32) - the test build: no VM to scan, no paks to read

	// The stock table is the only table the non-Windows build can have, and no icon texture can
	// be loaded: icon mode falls through to the chip, which is what the GL tests assert on.
	void EnsureWeaponTable()
	{
		if (!s_tableForVm)
		{
			WeaponEsp::StockTable(s_table);
			s_tableNative = false;
			s_tableForVm  = true;
		}
	}

	bool EnsureIconTexture(int weapon, IconTex& out)
	{
		out = s_icons[weapon];
		return out.valid;
	}

#endif // _WIN32

// =============================================================================================== //
// model mode (3D) - the public half
// =============================================================================================== //
#if defined(_WIN32)

int WeaponEsp::EnsureModelHandles()
{
	// A recreated GL context (vid_restart) tore the renderer down with it: every model handle
	// is dead. (The icon textures watch the same context change and invalidate themselves in
	// EnsureIconTexture; the models do it here, against the same tracked DC.)
	if (s_iconHdc && s_iconHdc != wglGetCurrentDC())
		memset(s_models, 0, sizeof(s_models));

	EnsureWeaponTable();                       // s_table / s_tableGen current for this VM
	if (!EnsureRefExport())
		return 0;                              // never located the renderer table: nothing to register

	int ready = 0;
	for (int w = 1; w < WeaponEsp::kTableWeapons; ++w)
	{
		ModelRec& rec = s_models[w];
		char path[128];
		if (!WeaponEsp::WeaponModel(s_table, w, path, sizeof(path)))
		{
			// the table has no model for this weapon (or it changed since): forget any handle
			if (rec.handle != 0 || rec.path[0] != 0)
			{
				rec.handle   = 0;
				rec.path[0]  = 0;
				rec.triedGen = s_tableGen;
			}
			continue;
		}
		if (rec.handle > 0 && rec.triedGen == s_tableGen && strcmp(rec.path, path) == 0)
		{
			++ready;
			continue;                            // this exact path, this table generation
		}
		if (rec.triedGen == s_tableGen)
			continue;                            // looked for it under this table already: no model
		rec.triedGen = s_tableGen;
		CopyIconName(rec.path, sizeof(rec.path), path);

		int h = 0;
#ifdef _MSC_VER
		__try
		{
			h = ((int (Q3SDK_CDECL *)(const char*))
			        s_refExport->slot[WeaponEsp::kSlotRegisterModel])(path);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Log("[kutaQ3] RE_RegisterModel faulted on %s - model mode falls back to the icon", path);
			s_refExport        = NULL;           // allow a throttled rescan of the table
			s_refExportTried   = false;
			s_refExportNextTry = 0;
		}
#else
		h = ((int (*)(const char*))
		        s_refExport->slot[WeaponEsp::kSlotRegisterModel])(path);
#endif
		rec.handle = (h > 0) ? h : 0;
		if (rec.handle > 0)
			++ready;
	}
	if (ready > 0)
		Log("[kutaQ3] model mode: %d weapon model(s) registered", ready);
	return ready;
}

void WeaponEsp::OnWorldRenderScene(const int* args)
{
	if (!Config::g_Settings.weaponEsp ||
	    Config::g_Settings.weaponEspStyle != WeaponEsp::StyleModel)
		return;
	if (s_injectedThisFrame)
		return;                                 // the scene is built once per frame

	// The refdef the scene is about to be rendered with - read while the cgame still owns it.
	q3::refdef_t fd;
	if (!Vm::TrapRefdef(args, &fd))
		return;
	if ((fd.rdflags & q3::kRdfNoWorldModel) != 0)
		return;                                 // a HUD / model-view scene: no world to sit in

	EnsureWeaponTable();                        // cheap: a no-op once the table is applied

	float scale = Config::g_Settings.weaponEspModelScale;
	if (!(scale > 0.0f))
		scale = 1.0f;

	// The players' positions at the exact instant this frame renders (fd.time is cg.time):
	// the same lerp the cgame applies to the bodies, evaluated at the moment the scene will
	// capture them - so the model sits on the body instead of trailing a frame behind it.
	if (!NameEsp::Gather(fd.time, Vm::Syscall(), &fd))
	{
		s_injectedThisFrame = true;
		s_modelInjected     = 0;
		return;
	}
	const NameEsp::Frame& frame = NameEsp::Current();

	int pushed = 0;
	if (frame.valid)
	{
		for (int i = 0; i < frame.playerCount; ++i)
		{
			const NameEsp::PlayerTag& tag = frame.players[i];
			if (!ModelReady(tag.weapon))
				continue;                        // no model: the overlay falls back to the icon
			q3::refEntity_t re;
			if (!PlanModelEntity(tag, scale, s_models[tag.weapon].handle, re))
				continue;
			if (!s_refExport)
				break;
#ifdef _MSC_VER
			__try
			{
				((void (Q3SDK_CDECL *)(const q3::refEntity_t*))
				 s_refExport->slot[WeaponEsp::kSlotAddRefEntityToScene])(&re);
				++pushed;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				Log("[kutaQ3] RE_AddRefEntityToScene faulted - model injection stopped (icon fallback stays)");
				s_refExport        = NULL;       // allow a throttled rescan of the table
				s_refExportTried   = false;
				s_refExportNextTry = 0;
				break;
			}
#else
			((void (*)(const q3::refEntity_t*))
			 s_refExport->slot[WeaponEsp::kSlotAddRefEntityToScene])(&re);
			++pushed;
#endif
		}
	}
	s_modelInjected     = pushed;
	s_injectedThisFrame = true;
}

void WeaponEsp::ResetModelHandles()
{
	// A level change (or the VM going away): the renderer re-registers all its media, so the
	// qhandles from the old level are dead. The refexport TABLE itself is a global in the
	// executable and survives.
	memset(s_models, 0, sizeof(s_models));
	s_modelInjected = 0;
}

const char* WeaponEsp::ModelStatus()
{
	static char line[160];
	if (!s_refExport)
	{
		snprintf(line, sizeof(line), "renderer table: %s",
		         s_refExportTried ? "not found (Model mode falls back to the icon)"
		                         : "not located yet");
		return line;
	}
	int ready = 0;
	for (int w = 0; w < WeaponEsp::kTableWeapons; ++w)
		if (s_models[w].triedGen == s_tableGen && s_models[w].handle > 0)
			++ready;
	snprintf(line, sizeof(line), "renderer table found - %d model(s) registered", ready);
	return line;
}

#else // !defined(_WIN32) - the test build: no renderer to talk to, model mode is icon mode

int WeaponEsp::EnsureModelHandles()
{
	return 0;                                   // no models off Windows: every tag falls back to the icon
}

void WeaponEsp::OnWorldRenderScene(const int*)
{
}

void WeaponEsp::ResetModelHandles()
{
	memset(s_models, 0, sizeof(s_models));
	s_modelInjected = 0;
}

const char* WeaponEsp::ModelStatus()
{
	return "not available off Windows (icon fallback)";
}

#endif // _WIN32

// =============================================================================================== //

const WeaponEsp::DrawStats& WeaponEsp::LastDrawStats()
{
	return s_stats;
}

void WeaponEsp::ResetDrawState()
{
	ResetTagState();
}

const char* WeaponEsp::TableSource()
{
	return s_tableNative ? "cgame native table (scanned)" : "built-in stock 1.32 names";
}

int WeaponEsp::TableWeaponCount()
{
	return s_table.weaponCount;
}

void WeaponEsp::Draw()
{
	s_stats.drawn = s_stats.inView = s_stats.edge = s_stats.behind = s_stats.faded = 0;
	s_stats.iconsMissing = 0;
	s_stats.modelsMissing = 0;
	// Draw() runs at the end of every frame (the SwapBuffers hook), after the scene has been
	// rendered - which is when the next frame's model injection earns its one push.
	s_injectedThisFrame = false;
	if (!Config::g_Settings.weaponEsp)
	{
		// off: forget every fade, so turning the feature back on ramps the tags in again
		// instead of popping them in at full brightness
		ResetTagState();
		return;
	}

	const NameEsp::Frame& frame = NameEsp::Current();
	// Note this deliberately does NOT clear the fade state when there is nobody to draw: the PVS
	// drops every player for a frame or two at times, and restarting the fades there would blink
	// the whole set. A player who is genuinely gone keeps a slot that costs nothing.
	if (!frame.valid || frame.playerCount <= 0)
		return;

	// The game's own viewport: where the 3D scene actually was, which is what the projection
	// maps out of (a letterboxed r_mode puts it somewhere other than 0,0).
	GLint glViewportRect[4] = { 0, 0, 0, 0 };
	glGetIntegerv(GL_VIEWPORT, glViewportRect);

	const NameEsp::Viewport vp = { glViewportRect[0], glViewportRect[1],
	                               glViewportRect[2], glViewportRect[3] };
	if (vp.width <= 0 || vp.height <= 0)
		return;

	// Build (or rebuild after a context change) before touching the overlay state.
	HDC hdc = wglGetCurrentDC();
	if (!hdc)
		return;
	for (int i = 0; i < kBucketCount; ++i)
	{
		if (!s_fonts[i].bBuilt || s_fonts[i].hdc != hdc)
			s_fonts[i].Build((int)kFontBuckets[i]);
	}
	if (!s_fonts[0].bBuilt)
		return;

	EnsureWeaponTable();
	if (Config::g_Settings.weaponEspStyle == WeaponEsp::StyleModel)
		EnsureModelHandles();   // at frame end the renderer is idle: register missing models

	// One clock step for the whole frame: every tag fades in over the same interval.
	const int stepMs = ClockStepMs(frame.serverTime);
	++s_frameSerial;

	{
		// The overlay changes plenty of legacy state Quake 3 caches in its own glState shadow;
		// the guard puts all of it back on the way out (same wrapper the menu renders in).
		KUTAQ3_LEGACY_GL_STATE_GUARD();
		GL::SetupOrtho();

		// Blending is what makes the fade a fade (same as the other ESP overlays).
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		const bool iconMode = (Config::g_Settings.weaponEspStyle != 0);
		static const unsigned char kTextRgb[3] = { 255, 140, 0 };   // saturated orange: readable
		                                                            // on any background, distinct
		                                                            // from the team colours
		static const unsigned char kChipRgb[3] = { 90, 220, 235 };  // icon-missing chip
		static const unsigned char black[3]   = { 0, 0, 0 };

		for (int i = 0; i < frame.playerCount; ++i)
		{
			const NameEsp::PlayerTag& tag = frame.players[i];

			// Distance-based scale & alpha fading instead of full size and opacity:
			// |cg.refdef.vieworg - cent->lerpOrigin| drives both - the same value and the same
			// ramp as the DISTANCE and HEALTH ESPs (see weaponEsp.h).
			// WP_NONE: the player is carrying nothing - the game shows no weapon there
			// either, so neither style draws a tag (text mode would hit the same guard in
			// WeaponName()).
			if (tag.weapon <= 0)
				continue;

			float scale = 1.0f, fade = 1.0f;
			DistanceEsp::DistanceFade(tag.distance, scale, fade);
			// Model mode with a registered model: the 3D weapon model is the tag itself, pushed
			// into the scene by OnWorldRenderScene at this player's exact rendered position.
			// A real 3D object shrinks with distance on its own, so the 2D ramp does not apply.
			const bool modelReady =
			    (Config::g_Settings.weaponEspStyle == WeaponEsp::StyleModel) &&
			    ModelReady(tag.weapon);
			if (!modelReady && fade < 0.04f)
			{
				++s_stats.faded;
				continue;                           // faded to nothing: nothing to draw
			}

			// The leg anchor (mid-leg, not the head - see the header for why that is what keeps
			// this ESP clear of the head-anchored stack).
			float leg[3];
			LegAnchor(tag, leg);
			NameEsp::ScreenPoint p;
			const bool ok = NameEsp::ProjectWorldToScreen(frame.view, vp, leg, p);
			// ProjectWorldToScreen returns coordinates in desktop/window space; SetupOrtho()
			// resets the GL viewport to (0, 0, width, height) - convert like the other ESPs.
			if (ok)
			{
				p.x -= (float)vp.x;
				p.y += (float)vp.y;
			}
			if (!ok)
			{
				++s_stats.behind;                   // behind the viewer
				continue;
			}

			if (modelReady)
			{
				// No overlay for this one: the weapon's 3D model is in the world already.
				++s_stats.drawn;
				if (p.inView)
					++s_stats.inView;
				else
					++s_stats.edge;
				continue;
			}
			if (Config::g_Settings.weaponEspStyle == WeaponEsp::StyleModel)
				++s_stats.modelsMissing;            // model mode, no model: the icon below is the fallback

			TagState& st = TagSlot(tag.clientNum);
			const bool fresh = (st.steppedAt != s_frameSerial);
			if (fresh)
				st.steppedAt = s_frameSerial;

			// Fade in (a tag that just entered the PVS ramps up over kFadeInMs) times the
			// distance fade.
			if (fresh)
			{
				st.alpha += (float)stepMs / (float)kFadeInMs;
				if (st.alpha > 1.0f)
					st.alpha = 1.0f;
			}
			const float alpha = st.alpha * fade;
			if (alpha < 0.04f)
			{
				++s_stats.faded;
				continue;
			}

			if (iconMode)
			{
				// The icon, centred on the leg point. Size scales with distance exactly like
				// the text's face does.
				float size = kIconBaseSizePx * scale;
				if (size < 8.0f)
					size = 8.0f;
				float x = p.x - size * 0.5f;
				float y = p.y - size * 0.5f;

				// Keep the whole quad inside the overlay so edge tags stay visible.
				const float overlayW = (float)vp.width;
				const float overlayH = (float)vp.height;
				if (x < 0.0f)
					x = 0.0f;
				if (x + size + 1.0f > overlayW)
					x = overlayW - size - 1.0f;
				if (x < 0.0f)
					x = 0.0f;
				if (y < 0.0f)
					y = 0.0f;
				if (y + size + 1.0f > overlayH)
					y = overlayH - size - 1.0f;
				if (y < 0.0f)
					y = 0.0f;

				IconTex tex;
				if (EnsureIconTexture(tag.weapon, tex))
				{
					// 1px black outline for readability, then the icon itself
					GL::DrawOutlineAlpha(x, y, size, size, 1.0f, black, alpha);

					glEnable(GL_TEXTURE_2D);
					glBindTexture(GL_TEXTURE_2D, tex.tex);
					glColor4f(1.0f, 1.0f, 1.0f, alpha * (p.inView ? 1.0f : 0.55f));
					glBegin(GL_QUADS);
					glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
					glTexCoord2f(1.0f, 0.0f); glVertex2f(x + size, y);
					glTexCoord2f(1.0f, 1.0f); glVertex2f(x + size, y + size);
					glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + size);
					glEnd();
					glDisable(GL_TEXTURE_2D);
				}
				else
				{
					// no texture for this weapon (icon not in the paks, or a mod weapon the
					// table has no icon for): mark the position with a neutral chip
					GL::DrawOutlineAlpha(x, y, size, size, 1.0f, black, alpha);
					unsigned char chipRgb[3] = { kChipRgb[0], kChipRgb[1], kChipRgb[2] };
					if (!p.inView)
						Dim(chipRgb);
					GL::DrawFilledRectAlpha(x, y, size, size, chipRgb, alpha);
					++s_stats.iconsMissing;
				}

				++s_stats.drawn;
				if (p.inView)
					++s_stats.inView;
				else
					++s_stats.edge;
			}
			else
			{
				// The weapon's name string, in the pre-baked face nearest FONT_HEIGHT * scale
				// (same bucket trick as the distance ESP).
				char name[64];
				if (!WeaponName(s_table, tag.weapon, name, sizeof(name)))
					continue;                        // WP_NONE: no weapon to show
				const int bucket = BucketForScale(scale);
				if (!s_fonts[bucket].bBuilt)
					continue;

				GL::Font& font = s_fonts[bucket];
				const float textW = font.TextWidth(name);
				if (textW <= 0.0f)
					continue;
				float x = font.centerText(p.x, 0.0f, textW);
				float y = p.y;
				const float fontH = kFontBuckets[bucket];

				// Keep the whole string inside the overlay so edge tags stay visible instead of
				// vanishing (same rule as NameEsp::Draw / DistanceEsp::Draw).
				const float overlayW = (float)vp.width;
				const float overlayH = (float)vp.height;
				if (x < 0.0f)
					x = 0.0f;
				if (x + textW + 1.0f > overlayW)
					x = overlayW - textW - 1.0f;
				if (x < 0.0f)
					x = 0.0f;
				if (y < 0.0f)
					y = 0.0f;
				if (y + fontH + 1.0f > overlayH)
					y = overlayH - fontH - 1.0f;
				if (y < 0.0f)
					y = 0.0f;

				unsigned char rgb[3] = { kTextRgb[0], kTextRgb[1], kTextRgb[2] };
				if (!p.inView)
					Dim(rgb);

				// 1px black drop shadow for readability, same as the other text ESPs.
				font.PrintAlpha(x + 1.0f, y + 1.0f, black, alpha, "%s", name);
				font.PrintAlpha(x, y, rgb, alpha, "%s", name);
				++s_stats.drawn;
				if (p.inView)
					++s_stats.inView;
				else
					++s_stats.edge;
			}
		}

		GL::RestoreGL();
	}
}
