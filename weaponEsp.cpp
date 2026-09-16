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
// shape scan, the stock fallback, the anchor) and the whole icon pipeline (the pak archive
// parsing with its own DEFLATE inflater, the TGA decode) live in weaponEspCore.cpp, where the
// tests run them against fabricated archives and artwork. What is guarded by _WIN32 here is the
// part that needs Windows: the cgame data segment walk, the file system search for the icon
// files (GetModuleFileNameA / FindFirstFileA / CreateFileA) and the GL texture upload. The test
// build (tests/stub) compiles the same Draw() against a stock table with no icons available, so
// the drawing path - projection, anchor, stacking, fade, stats - is exercised off Windows exactly
// like the other ESPs.
//
// Icon failures carry their reason (WeaponEsp::IconResult): the menu and log.txt say whether the
// file was not in the paks, was there but unreadable as a TGA, or was fine and GL refused it -
// "not in the paks" for every one of them is what made a loader bug look like a missing asset.
// =============================================================================================== //

#include "weaponEsp.h"
#include "config.h"        // Config::g_Settings.weaponEsp / weaponEspStyle
#include "glText.h"        // GL::Font, FONT_HEIGHT
#include "glDraw.h"        // GL::SetupOrtho / GL::RestoreGL / the rect helpers
#include "glStateGuard.h"  // KUTAQ3_LEGACY_GL_STATE_GUARD
#include <gl/GL.h>
#include <stdlib.h>        // malloc / free (the pak + TGA buffers)
#include <stdint.h>
#include <string.h>        // strlen (the pak entry lookup)

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
		int    result;     // the WeaponEsp::IconResult behind `triedGen`: why there is no texture,
		                   // so a cached failure still reports what went wrong
	};
	IconTex s_icons[WeaponEsp::kTableWeapons];

	// One line describing the last icon failure (WeaponEsp::LastIconNote()): which shader, where
	// the search looked, what was wrong with what it found. Written by the Win32 half.
	char s_iconNote[512];
#if defined(_WIN32)
	int s_tableGen = 0;  // bumped whenever the weapon table is (re)applied - the cgame changed.
	                     // (the icon slots remember the generation they were last tried under,
	                     // so a table from a new cgame earns a retry of every missing icon)
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
	// the icon search, and the GL upload
	//
	// The icon shader name the table carries ("icons/iconw_gauntlet") is a path relative to the
	// game directory; the cgame registered the same path as a shader, so the file it resolves to is
	// exactly the artwork the mod uses. This half only opens files and reads ranges out of them -
	// the pak parsing (ZIP central directory, its own DEFLATE inflater) and the TGA decode live in
	// weaponEspCore.cpp, where the tests drive them with real archives and real zlib streams.
	//
	// Every failure keeps its reason. "The file is not in the paks" and "the file is there but is
	// not a TGA this loader reads" are different bugs, and log.txt now says which one happened
	// instead of blaming the paks for both.
	// =========================================================================================== //
	struct PakFile
	{
		HANDLE hFile;
		bool   open;
	};

	bool PakOpen(const char* path, PakFile& pak)
	{
		pak.hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		pak.open  = (pak.hFile != INVALID_HANDLE_VALUE);
		return pak.open;
	}

	void PakClose(PakFile& pak)
	{
		if (pak.open)
			CloseHandle(pak.hFile);
		pak.open = false;
	}

	// The range reader WeaponEsp::PakReadEntry pulls a pak through.
	bool PakReadRange(void* user, unsigned long long offset, void* dest, size_t count)
	{
		PakFile& pak = *(PakFile*)user;
		if (!pak.open)
			return false;
		LONG high = (LONG)(offset >> 32);
		SetFilePointer(pak.hFile, (LONG)(offset & 0xFFFFFFFFu), &high, FILE_BEGIN);
		DWORD got = 0;
		const BOOL ok = ReadFile(pak.hFile, dest, (DWORD)count, &got, NULL);
		return ok && got == (DWORD)count;
	}

	// One entry out of one pak (malloc'd on success, the caller frees).
	bool LoadPakBytes(const char* path, const char* file, unsigned char** out, size_t* outLen)
	{
		PakFile pak;
		if (!PakOpen(path, pak))
			return false;
		LARGE_INTEGER sizeLi;
		bool ok = false;
		if (GetFileSizeEx(pak.hFile, &sizeLi) && sizeLi.QuadPart > 0)
			ok = WeaponEsp::PakReadEntry(PakReadRange, &pak, (unsigned long long)sizeLi.QuadPart,
			                             file, out, outLen);
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

	// The game directory: the folder quake3.exe sits in, where baseq3 and the mod dirs live.
	const char* GameDir()
	{
		static char gameDir[520];
		if (gameDir[0] == 0)
		{
			GetModuleFileNameA(NULL, gameDir, sizeof(gameDir) - 1);
			for (char* p = gameDir; *p; ++p)
			{
				if (*p == '\\')
				{
					p[1] = 0;
					break;
				}
			}
		}
		return gameDir;
	}

	// The directories the search walks, in the engine's own order: every subdirectory of the game
	// dir first (a mod's paks are loaded on top of baseq3 and override it), then baseq3, then the
	// game dir itself (loose .tga files next to quake3.exe). The game dir is the empty string, the
	// marker the walk reads as "use the game dir".
	const int kMaxIconRoots = 34;   // 32 mod dirs + baseq3 + the game dir itself

	int CollectRoots(char roots[][560], int& modCount)
	{
		const char* gameDir = GameDir();
		int count = 0;
		modCount  = 0;

		char pattern[600];
		snprintf(pattern, sizeof(pattern), "%s\\*", gameDir);
		WIN32_FIND_DATAA fd;
		HANDLE find = FindFirstFileA(pattern, &fd);
		if (find != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
					continue;
				if (_stricmp(fd.cFileName, "baseq3") == 0)
					continue;
				if (count >= kMaxIconRoots - 2)
					break;
				if (snprintf(roots[count], 560, "%s%s", gameDir, fd.cFileName) >= 0)
					++count;
			}
			while (FindNextFileA(find, &fd) && count < kMaxIconRoots - 2);
			FindClose(find);
		}
		modCount = count;

		if (snprintf(roots[count], 560, "%sbaseq3", gameDir) >= 0)
			++count;
		roots[count][0] = 0;              // the game dir itself: loose files
		++count;
		return count;
	}

	// Written once, the first time an icon is looked for: what the search has to work with. A
	// "0 icons in the paks" report is much easier to read next to this line - it says whether
	// baseq3 was found at all, and how many paks were in it.
	void LogIconEnvironment(int modCount)
	{
		static bool logged = false;
		if (logged)
			return;
		logged = true;

		const char* gameDir = GameDir();
		char pattern[600];
		snprintf(pattern, sizeof(pattern), "%sbaseq3\\*.pk3", gameDir);
		int paks = 0;
		WIN32_FIND_DATAA fd;
		HANDLE find = FindFirstFileA(pattern, &fd);
		if (find != INVALID_HANDLE_VALUE)
		{
			do
			{
				++paks;
			}
			while (FindNextFileA(find, &fd));
			FindClose(find);
		}
		Log("[kutaQ3] weapon icons: game dir %s, %d mod dir(s), %d pak(s) in baseq3",
		    gameDir, modCount, paks);
	}

	// "<dir><name>" into a fixed buffer - the same bounds the icon search's paths have. (snprintf
	// would do, but the compiler then has to assume the 560-byte dir buffers could overflow the
	// smaller path buffers it is joining them into.)
	void JoinPath(char* out, size_t outSize, const char* dir, const char* name)
	{
		size_t n = 0;
		for (size_t i = 0; dir && dir[i] && n + 1 < outSize; ++i)
			out[n++] = dir[i];
		for (size_t i = 0; name && name[i] && n + 1 < outSize; ++i)
			out[n++] = name[i];
		out[n] = 0;
	}

	// Where a lookup looked and what it found - the diagnostic behind a chip.
	struct IconSearch
	{
		char foundIn[600];     // the pak or loose file the .tga came from, "" when nowhere
		int  dirsLooked;       // mod dirs + baseq3 + the game dir
		int  paksLooked;       // pak files opened
	};

	// Load the .tga for an icon shader name ("icons/iconw_gauntlet") out of the game's paks or
	// loose files. malloc'd on success; the caller frees.
	bool LoadIconFile(const char* shaderName, unsigned char** out, size_t* outLen, IconSearch& search)
	{
		search.foundIn[0] = 0;
		search.dirsLooked = 0;
		search.paksLooked = 0;
		*out   = 0;
		*outLen = 0;

		char file[160];
		if (strrchr(shaderName, '.'))
		{
			// already a filename (a mod spells the icon with an extension)
			snprintf(file, sizeof(file), "%s", shaderName);
		}
		else
		{
			snprintf(file, sizeof(file), "%s.tga", shaderName);
		}

		char roots[kMaxIconRoots][560];
		int modCount = 0;
		const int rootCount = CollectRoots(roots, modCount);
		LogIconEnvironment(modCount);

		for (int r = 0; r < rootCount; ++r)
		{
			const bool gameRoot = (roots[r][0] == 0);
			const char* root = gameRoot ? GameDir() : roots[r];
			++search.dirsLooked;

			// paks inside the dir, highest number first: the engine loads pakN ascending, so the
			// highest-numbered pak is the last one loaded and wins
			char paks[16][800];
			int pakCount = 0;
			{
				char pattern[720];
				JoinPath(pattern, sizeof(pattern), root, "\\*.pk3");
				WIN32_FIND_DATAA fd;
				HANDLE find = FindFirstFileA(pattern, &fd);
				if (find != INVALID_HANDLE_VALUE)
				{
					do
					{
						if (pakCount >= 16)
							break;
						if (fd.cFileName[0])
						{
							JoinPath(paks[pakCount], sizeof(paks[pakCount]), root, "\\");
							JoinPath(paks[pakCount], sizeof(paks[pakCount]), paks[pakCount], fd.cFileName);
							++pakCount;
						}
					}
					while (FindNextFileA(find, &fd) && pakCount < 16);
					FindClose(find);
				}
			}
			for (int p = pakCount - 1; p >= 0; --p)
			{
				++search.paksLooked;
				if (LoadPakBytes(paks[p], file, out, outLen))
				{
					CopyIconName(search.foundIn, sizeof(search.foundIn), paks[p]);
					return true;
				}
			}

			char loose[800];
			JoinPath(loose, sizeof(loose), root, "\\");
			JoinPath(loose, sizeof(loose), loose, file);
			if (LoadLooseFile(loose, out, outLen))
			{
				CopyIconName(search.foundIn, sizeof(search.foundIn), loose);
				return true;
			}
		}

		return false;
	}

	void SetIconNote(const char* icon, const char* reason, const char* detail)
	{
		if (detail && detail[0])
			snprintf(s_iconNote, sizeof(s_iconNote), "%s: %s (%s)", icon, reason, detail);
		else
			snprintf(s_iconNote, sizeof(s_iconNote), "%s: %s", icon, reason);
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
				// GL's default minification filter wants a mipmap chain, and a texture without one
				// is incomplete: it samples as opaque black, so the icon would come out as a black
				// square. Ask for a plain linear filter (the icons are drawn near 1:1 scaled).
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
			}
		}
		if (!tex)
		{
			slot.triedGen = s_tableGen;
			slot.result   = WeaponEsp::IconNoTexture;
			return false;
		}

		CopyIconName(slot.icon, sizeof(slot.icon), icon);
		slot.tex      = tex;
		slot.w        = w;
		slot.h        = h;
		slot.valid    = true;
		slot.triedGen = s_tableGen;
		slot.result   = WeaponEsp::IconOk;
		s_iconHdc     = wglGetCurrentDC();
		return true;
	}

	// =========================================================================================== //
	// EnsureIconTexture(weapon): the loaded texture for this weapon's icon, or the reason there is
	// none (the caller draws the chip). Loaded once per weapon per table per GL context - a failure
	// is remembered with its reason, so a missing file does not cost a directory walk and a pak
	// open every frame, and the next frame still reports the same reason instead of re-probing.
	// A new table (new cgame) or a new GL context (vid_restart) earns a retry.
	// =========================================================================================== //
	WeaponEsp::IconResult EnsureIconTexture(int weapon, IconTex& out)
	{
		memset(&out, 0, sizeof(out));
		if (weapon < 0 || weapon >= WeaponEsp::kTableWeapons)
			return WeaponEsp::IconNoName;  // outside the table: there is no icon to look for

		out = s_icons[weapon];
		if (s_iconHdc && s_iconHdc != wglGetCurrentDC())
		{
			// vid_restart destroyed the context: every id is stale, and a fresh retry is earned
			for (int i = 0; i < WeaponEsp::kTableWeapons; ++i)
			{
				s_icons[i].valid    = false;
				s_icons[i].triedGen = 0;
				s_icons[i].result   = WeaponEsp::IconNotFound;
			}
			s_iconHdc = 0;
			out.valid    = false;
			out.triedGen = 0;
		}

		char icon[64];
		if (!WeaponEsp::WeaponIcon(s_table, weapon, icon, sizeof(icon)))
		{
			// the table has no icon shader for this weapon number: not a file problem
			SetIconNote("the weapon table", "lists no icon shader for this weapon", 0);
			return WeaponEsp::IconNoName;
		}
		if (out.valid && _stricmp(out.icon, icon) == 0)
			return WeaponEsp::IconOk;      // already loaded for this exact icon, this table, this context
		if (out.triedGen == s_tableGen)
			return (WeaponEsp::IconResult)out.result;   // looked for it already: same answer, no probing

		unsigned char* tga = 0;
		size_t len = 0;
		IconSearch search;
		if (!LoadIconFile(icon, &tga, &len, search))
		{
			char detail[160];
			snprintf(detail, sizeof(detail), "%d dir(s), %d pak(s) searched, none holds it",
			         search.dirsLooked, search.paksLooked);
			SetIconNote(icon, "not in the game's paks", detail);
			Log("[kutaQ3] weapon icon %s", s_iconNote);
			s_icons[weapon].triedGen = s_tableGen;
			s_icons[weapon].result   = WeaponEsp::IconNotFound;
			return WeaponEsp::IconNotFound;
		}

		unsigned char* rgba = 0;
		int w = 0, h = 0;
		const bool decoded = WeaponEsp::DecodeTga(tga, len, &rgba, &w, &h);
		const unsigned int tgaLen = (unsigned int)len;
		free(tga);
		if (!decoded)
		{
			char detail[280];
			snprintf(detail, sizeof(detail), "%.200s, %u bytes", search.foundIn, tgaLen);
			SetIconNote(icon, "found, but not a TGA this loader reads", detail);
			Log("[kutaQ3] weapon icon %s", s_iconNote);
			s_icons[weapon].triedGen = s_tableGen;
			s_icons[weapon].result   = WeaponEsp::IconUnreadable;
			return WeaponEsp::IconUnreadable;
		}

		const bool uploaded = UploadIcon(weapon, icon, rgba, w, h);
		free(rgba);
		if (!uploaded)
		{
			SetIconNote(icon, "decoded, but GL would not take the texture", search.foundIn);
			Log("[kutaQ3] weapon icon %s", s_iconNote);
			return WeaponEsp::IconNoTexture;
		}

		out = s_icons[weapon];             // the freshly uploaded texture, for this frame's quad
		return WeaponEsp::IconOk;
	}

}

#else // !defined(_WIN32) - the test build: no VM to scan, no paks to read

	// The stock table is the only table the non-Windows build can have, and the paks cannot be read
	// on this host: icon mode always falls through to the chip. The reason is IconNotFound - there
	// is nowhere to look for the file - which is what the GL tests assert on.
	void EnsureWeaponTable()
	{
		if (!s_tableForVm)
		{
			WeaponEsp::StockTable(s_table);
			s_tableNative = false;
			s_tableForVm  = true;
		}
	}

	WeaponEsp::IconResult EnsureIconTexture(int weapon, IconTex& out)
	{
		memset(&out, 0, sizeof(out));
		if (weapon < 0 || weapon >= WeaponEsp::kTableWeapons)
			return WeaponEsp::IconNoName;
		out = s_icons[weapon];
		return out.valid ? WeaponEsp::IconOk : WeaponEsp::IconNotFound;
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

const char* WeaponEsp::LastIconNote()
{
	return s_iconNote;
}

void WeaponEsp::Draw()
{
	s_stats.drawn = s_stats.inView = s_stats.edge = s_stats.behind = s_stats.faded = 0;
	s_stats.iconsMissing = 0;
	s_stats.iconsNotInPak = s_stats.iconsBadData = s_stats.iconsNoUpload = 0;
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

		const bool iconMode = (Config::g_Settings.weaponEspStyle == WeaponEsp::StyleIcon);
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
			if (fade < 0.04f)
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
				const WeaponEsp::IconResult icon = EnsureIconTexture(tag.weapon, tex);
				if (icon == WeaponEsp::IconOk)
				{
					// 1px black outline for readability, then the icon itself
					GL::DrawOutlineAlpha(x, y, size, size, 1.0f, black, alpha);

					glEnable(GL_TEXTURE_2D);
					glBindTexture(GL_TEXTURE_2D, tex.tex);
					// the fade and the dim ride on the current colour, so the icon's own alpha
					// has to be multiplied into it rather than added
					glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
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
					// No texture for this weapon: mark the position with a neutral chip, and count
					// WHY there is none - "the paks do not have it", "the file is not readable" and
					// "GL refused the upload" are three very different reports in the menu.
					GL::DrawOutlineAlpha(x, y, size, size, 1.0f, black, alpha);
					unsigned char chipRgb[3] = { kChipRgb[0], kChipRgb[1], kChipRgb[2] };
					if (!p.inView)
						Dim(chipRgb);
					GL::DrawFilledRectAlpha(x, y, size, size, chipRgb, alpha);
					++s_stats.iconsMissing;
					if (icon == WeaponEsp::IconNotFound)
						++s_stats.iconsNotInPak;
					else if (icon == WeaponEsp::IconUnreadable)
						++s_stats.iconsBadData;
					else if (icon == WeaponEsp::IconNoTexture)
						++s_stats.iconsNoUpload;
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
