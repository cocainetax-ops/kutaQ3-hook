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
//     own weapon list is what the ESP ends up displaying;
//
//   - PakReadEntry(): pulling one entry out of a pak (a plain ZIP archive), including the raw
//     DEFLATE inflater the method-8 entries need;
//
//   - DecodeTga(): the .tga decode behind Icon mode.
//
// The last two used to live in weaponEsp.cpp, where they could only be compiled on Windows and
// never run by the tests - which is exactly where the icon bugs hid (see the comments on the
// header offsets below and on the inflater). They are pure byte maths, so they belong here, next
// to the table scan, where tests/test_nameesp.cpp drives them with real pak layouts and real zlib
// streams.
// =============================================================================================== //

#include "weaponEsp.h"

#include <stdlib.h>
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

	// =========================================================================================== //
	// raw DEFLATE (RFC 1951) - what a ZIP entry with method 8 holds
	//
	// Q3's paks are ordinary ZIP archives and the assets in them are deflated, so an icon cannot be
	// read out of one without an inflater. This is one: stored / fixed-Huffman / dynamic-Huffman
	// blocks, the full length and distance code tables, no allocation, no zlib, nothing outside
	// this file. It is written against the RFC and checked by tests/test_nameesp.cpp against real
	// zlib streams (see the pak fixtures there).
	// =========================================================================================== //
	const int kMaxCodeBits = 15;
	const int kLitSymbols  = 288;   // literal/length alphabet: 286 defined, the fixed table adds 2
	const int kDistSymbols = 32;    // distance alphabet: 30 defined
	const int kCodeSymbols = 19;    // the code-length alphabet a dynamic block describes itself with

	// The code lengths of a dynamic block's code-length alphabet are stored in this order, not
	// in symbol order - the reason a decoder needs this table at all.
	const unsigned char kCodeLengthOrder[kCodeSymbols] =
	{
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
	};

	// length code 257..285 -> base length / extra bits; 29 entries (286 and 287 are not codes)
	const unsigned short kLengthBase[29] =
	{
		3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
		67, 83, 99, 115, 131, 163, 195, 227, 258
	};
	const unsigned char kLengthExtra[29] =
	{
		0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
		4, 4, 4, 4, 5, 5, 5, 5, 0
	};

	// distance code 0..29 -> base distance / extra bits
	const unsigned short kDistBase[30] =
	{
		1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
		1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
	};
	const unsigned char kDistExtra[30] =
	{
		0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
		9, 9, 10, 10, 11, 11, 12, 12, 13, 13
	};

	// DEFLATE packs bits least-significant-first into bytes and only aligns where the format says
	// so, so the decoder pulls bits one at a time through this.
	struct BitReader
	{
		const unsigned char* src;
		size_t               len;
		size_t               pos;    // the next byte to load
		unsigned int         bits;   // bit 0 is the next bit of the stream
		int                  count;  // how many of those bits are valid
		bool                 bad;    // the stream ran out while bits were still being asked for

		void Init(const unsigned char* data, size_t length)
		{
			src = data; len = length; pos = 0; bits = 0; count = 0; bad = false;
		}

		unsigned int Get(int n)
		{
			while (count < n)
			{
				if (pos >= len)
				{
					bad = true;
					return 0;
				}
				bits |= (unsigned int)src[pos++] << count;
				count += 8;
			}
			const unsigned int value = bits & ((1u << n) - 1u);
			bits >>= n;
			count -= n;
			return value;
		}

		void AlignToByte()
		{
			const int drop = count & 7;
			bits >>= drop;
			count -= drop;
		}
	};

	// A canonical Huffman table: counts[len] says how many codes have that length, symbols[] lists
	// the symbols sorted by (length, symbol). That is all a canonical code needs to decode.
	//
	// `allowIncomplete` matches the rule zlib's inflate uses: an incomplete code is an error except
	// for the single one-bit code some encoders emit for a lone symbol (maxLen == 1), and except
	// for the code-length alphabet, which a block may describe with only the symbols it uses.
	struct Huffman
	{
		unsigned short counts[kMaxCodeBits + 1];
		unsigned short symbols[kLitSymbols];

		bool Build(const unsigned char* lengths, int n, bool allowIncomplete = false)
		{
			for (int len = 0; len <= kMaxCodeBits; ++len)
				counts[len] = 0;
			for (int s = 0; s < n; ++s)
			{
				if (lengths[s] > kMaxCodeBits)
					return false;
				++counts[lengths[s]];
			}

			int maxLen = 0;
			for (int len = kMaxCodeBits; len > 0; --len)
			{
				if (counts[len])
				{
					maxLen = len;
					break;
				}
			}
			if (maxLen == 0)
			{
				// A block that never matches carries no distance codes at all. Build then describes
				// an empty table and Decode() always fails - the right answer for a stream that does
				// use a distance after all.
				return true;
			}

			// over-subscribed codes are corrupt; an incomplete set is only legal where zlib's own
			// inflate accepts it (see the note on the struct)
			int left = 1;
			for (int len = 1; len <= kMaxCodeBits; ++len)
			{
				left <<= 1;
				left -= counts[len];
				if (left < 0)
					return false;
			}
			if (left > 0 && maxLen != 1 && !allowIncomplete)
				return false;

			unsigned short offsets[kMaxCodeBits + 2];
			offsets[1] = 0;
			for (int len = 1; len <= kMaxCodeBits; ++len)
				offsets[len + 1] = (unsigned short)(offsets[len] + counts[len]);
			for (int s = 0; s < n; ++s)
			{
				if (lengths[s])
					symbols[offsets[lengths[s]]++] = (unsigned short)s;
			}
			return true;
		}

		int Decode(BitReader& br) const
		{
			int code = 0, first = 0, index = 0;
			for (int len = 1; len <= kMaxCodeBits; ++len)
			{
				code |= (int)br.Get(1);
				if (br.bad)
					return -1;
				const int count = counts[len];
				if (code - count < first)
					return symbols[index + (code - first)];
				index += count;
				first  = (first + count) << 1;
				code <<= 1;
			}
			return -1;                        // no code matched: corrupt stream
		}
	};

	// One raw DEFLATE stream into out. written ends up as the byte count; false means the stream
	// is corrupt, truncated, or does not fit the destination buffer.
	bool InflateRaw(const unsigned char* src, size_t srcLen, unsigned char* out, size_t outCap,
	                size_t& written)
	{
		written = 0;
		if (!src || !out)
			return false;

		BitReader br;
		br.Init(src, srcLen);

		for (;;)
		{
			const unsigned int last = br.Get(1);
			const unsigned int type = br.Get(2);
			if (br.bad)
				return false;

			if (type == 0)
			{
				// stored: align to the next byte, then LEN/NLEN and the bytes themselves
				br.AlignToByte();
				const unsigned int len  = br.Get(16);
				const unsigned int nlen = br.Get(16);
				if (br.bad || (len ^ 0xFFFFu) != nlen)
					return false;
				if (written + len > outCap)
					return false;
				for (unsigned int i = 0; i < len; ++i)
				{
					// the bit buffer may already hold whole bytes of the block
					if (br.count >= 8)
					{
						out[written++] = (unsigned char)(br.bits & 0xFFu);
						br.bits >>= 8;
						br.count -= 8;
					}
					else
					{
						if (br.pos >= br.len)
							return false;
						out[written++] = src[br.pos++];
					}
				}
				if (last)
					break;
				continue;
			}

			if (type == 3)
				return false;                 // reserved block type

			unsigned char litLengths[kLitSymbols];
			unsigned char distLengths[kDistSymbols];
			int litCount = 0, distCount = 0;

			if (type == 1)
			{
				// fixed: the lengths the RFC spells out
				for (int i = 0; i < 144; ++i) litLengths[i] = 8;
				for (int i = 144; i < 256; ++i) litLengths[i] = 9;
				for (int i = 256; i < 280; ++i) litLengths[i] = 7;
				for (int i = 280; i < kLitSymbols; ++i) litLengths[i] = 8;
				for (int i = 0; i < kDistSymbols; ++i) distLengths[i] = 5;
				litCount  = kLitSymbols;
				distCount = kDistSymbols;
			}
			else
			{
				// dynamic: the block describes its own code lengths, run-length encoded
				const unsigned int hlit  = br.Get(5) + 257;
				const unsigned int hdist = br.Get(5) + 1;
				const unsigned int hclen = br.Get(4) + 4;
				if (br.bad || hlit > (unsigned int)kLitSymbols || hdist > (unsigned int)kDistSymbols ||
				    hclen > (unsigned int)kCodeSymbols)
					return false;

				unsigned char codeLengths[kCodeSymbols];
				memset(codeLengths, 0, sizeof(codeLengths));
				for (unsigned int i = 0; i < hclen; ++i)
					codeLengths[kCodeLengthOrder[i]] = (unsigned char)br.Get(3);
				if (br.bad)
					return false;

				Huffman codeTable;
				if (!codeTable.Build(codeLengths, kCodeSymbols, true))
					return false;

				unsigned char lengths[kLitSymbols + kDistSymbols];
				memset(lengths, 0, sizeof(lengths));
				const int total = (int)(hlit + hdist);
				int index = 0;
				while (index < total)
				{
					const int sym = codeTable.Decode(br);
					if (sym < 0)
						return false;
					if (sym < 16)
					{
						lengths[index++] = (unsigned char)sym;
						continue;
					}

					int repeat;
					if (sym == 16)
					{
						if (index == 0)
							return false;     // nothing to repeat
						const unsigned char previous = lengths[index - 1];
						repeat = 3 + (int)br.Get(2);
						if (br.bad || index + repeat > total)
							return false;
						while (repeat-- > 0)
							lengths[index++] = previous;
					}
					else
					{
						// code 17 repeats 3..10 zeros (3 extra bits), code 18 repeats 11..138 (7)
						repeat = (sym == 17 ? 3 : 11) + (int)br.Get(sym == 17 ? 3 : 7);
						if (br.bad || index + repeat > total)
							return false;
						while (repeat-- > 0)
							lengths[index++] = 0;
					}
				}

				memset(litLengths, 0, sizeof(litLengths));
				memset(distLengths, 0, sizeof(distLengths));
				memcpy(litLengths, lengths, (size_t)hlit);
				memcpy(distLengths, lengths + hlit, (size_t)hdist);
				litCount  = (int)hlit;
				distCount = (int)hdist;
			}

			Huffman litTable, distTable;
			if (!litTable.Build(litLengths, litCount) || !distTable.Build(distLengths, distCount))
				return false;

			for (;;)
			{
				if (br.bad)
					return false;             // the stream ran out; do not decode phantom bits
				const int sym = litTable.Decode(br);
				if (sym < 0)
					return false;
				if (sym < 256)
				{
					if (written >= outCap)
						return false;
					out[written++] = (unsigned char)sym;
					continue;
				}
				if (sym == 256)
					break;                    // end of block

				const int lcode = sym - 257;
				if (lcode >= 29)
					return false;             // 286 / 287 are not in the alphabet
				const unsigned int length = (unsigned int)kLengthBase[lcode] + br.Get(kLengthExtra[lcode]);
				const int dcode = distTable.Decode(br);
				if (dcode < 0 || dcode >= 30)
					return false;
				const unsigned int distance = (unsigned int)kDistBase[dcode] + br.Get(kDistExtra[dcode]);
				if (br.bad || distance > written || written + length > outCap)
					return false;
				for (unsigned int i = 0; i < length; ++i)
				{
					out[written] = out[written - distance];
					++written;
				}
			}

			if (last)
				break;
		}

		return true;
	}

	// =========================================================================================== //
	// the pak (ZIP) side
	//
	// Everything is read through a callback, so the same code runs against a real file handle on
	// Windows and against a test's byte buffer off it.
	// =========================================================================================== //
	const unsigned int kMaxCentralDir = 4u * 1024u * 1024u;   // no real pak's index is bigger
	const unsigned int kMaxEntryBytes = 16u * 1024u * 1024u;  // ... nor any asset a pak holds

	unsigned int Rd16(const unsigned char* p)
	{
		return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
	}

	// The end-of-central-directory record sits at the very end of the file, behind an archive
	// comment of up to 64 KiB, so the tail is scanned backwards for its signature.
	bool EndOfCentralDirectory(const unsigned char* tail, size_t tailLen, unsigned int& entryCount,
	                           unsigned int& cdSize, unsigned int& cdOffset)
	{
		entryCount = cdSize = cdOffset = 0;
		if (!tail || tailLen < 22)
			return false;

		size_t off = tailLen - 22;
		for (;;)
		{
			if (tail[off] == 'P' && tail[off + 1] == 'K' && tail[off + 2] == 5 && tail[off + 3] == 6)
			{
				entryCount = Rd16(tail + off + 10);
				cdSize     = ReadU32(tail + off + 12);
				cdOffset   = ReadU32(tail + off + 16);
				return entryCount != 0 && cdSize != 0;
			}
			if (off == 0)
				break;
			--off;
		}
		return false;
	}

	// What a central-directory record has to say about the entry's data (local header offset 42,
	// method 10, compressed size 20, uncompressed size 24 - the ZIP record layout).
	struct ZipRecord
	{
		unsigned int method;
		unsigned int compSize;
		unsigned int uncompSize;
		unsigned int localOffset;
	};

	bool NameMatches(const unsigned char* name, unsigned int nameLen, const char* want,
	                 size_t wantLen, bool caseInsensitive)
	{
		if ((size_t)nameLen != wantLen)
			return false;
		for (size_t i = 0; i < wantLen; ++i)
		{
			unsigned char a = name[i];
			unsigned char b = (unsigned char)want[i];
			if (caseInsensitive)
			{
				if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
				if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
			}
			if (a != b)
				return false;
		}
		return true;
	}

	// Walk the central directory for `entry`. An exact name match wins, else the first
	// case-insensitive one - the same order the engine's own file search uses.
	bool FindRecord(const unsigned char* cd, size_t cdSize, unsigned int entryCount,
	                const char* entry, ZipRecord& out)
	{
		const size_t wantLen = strlen(entry);
		const unsigned char* p = cd;
		const unsigned char* end = cd + cdSize;
		const unsigned char* match = 0;

		for (unsigned int i = 0; i < entryCount && p + 46 <= end; ++i)
		{
			if (p[0] != 'P' || p[1] != 'K' || p[2] != 1 || p[3] != 2)
				break;                        // not a central-directory record: stop walking
			const unsigned int nameLen    = Rd16(p + 28);
			const unsigned int extraLen   = Rd16(p + 30);
			const unsigned int commentLen = Rd16(p + 32);
			const unsigned char* name = p + 46;
			if (name + nameLen > end)
				break;

			if (NameMatches(name, nameLen, entry, wantLen, false))
			{
				match = p;
				break;
			}
			if (!match && NameMatches(name, nameLen, entry, wantLen, true))
				match = p;

			p += 46 + nameLen + extraLen + commentLen;
		}

		if (!match)
			return false;
		out.method      = Rd16(match + 10);
		out.compSize    = ReadU32(match + 20);
		out.uncompSize  = ReadU32(match + 24);
		out.localOffset = ReadU32(match + 42);
		return true;
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

// =============================================================================================== //
// The pak side of Icon mode (weaponEsp.h): one entry out of a ZIP-shaped pak file.
//
// The paks hold most of their assets deflated, so this is also where the inflater above gets
// used. Every failure path returns false with *out left NULL - the caller draws its chip and the
// diagnostics report the reason.
// =============================================================================================== //
bool WeaponEsp::PakReadEntry(PakReadFn read, void* user, unsigned long long fileSize,
                             const char* entry, unsigned char** out, size_t* outLen)
{
	if (out)
		*out = 0;
	if (outLen)
		*outLen = 0;
	if (!read || !entry || !out || !outLen || fileSize < 22)
		return false;

	// the tail: the 22-byte end-of-central-directory record plus a comment of up to 64 KiB
	const unsigned long long kMaxTail = 65535ull + 22ull;
	const size_t tailLen = (size_t)(fileSize < kMaxTail ? fileSize : kMaxTail);
	unsigned char* tail = (unsigned char*)malloc(tailLen);
	if (!tail)
		return false;
	const bool gotTail = read(user, fileSize - tailLen, tail, tailLen);
	unsigned int entryCount = 0, cdSize = 0, cdOffset = 0;
	const bool haveEnd = gotTail && EndOfCentralDirectory(tail, tailLen, entryCount, cdSize, cdOffset);
	free(tail);
	if (!haveEnd || cdSize > kMaxCentralDir || (unsigned long long)cdOffset + cdSize > fileSize)
		return false;

	unsigned char* cd = (unsigned char*)malloc(cdSize);
	if (!cd)
		return false;
	const bool gotCd = read(user, cdOffset, cd, cdSize);
	ZipRecord record;
	const bool found = gotCd && FindRecord(cd, cdSize, entryCount, entry, record);
	free(cd);
	if (!found)
		return false;
	if (record.uncompSize == 0 || record.uncompSize > kMaxEntryBytes ||
	    record.compSize > kMaxEntryBytes)
		return false;
	if (record.method != 0 && record.method != 8)
		return false;                     // only stored and deflated entries

	// the local header repeats the name/extra lengths, and ITS copy is the one that says where
	// this entry's data starts (a ZIP may rewrite the name in the central directory)
	unsigned char local[30];
	if (!read(user, record.localOffset, local, sizeof(local)))
		return false;
	if (local[0] != 'P' || local[1] != 'K' || local[2] != 3 || local[3] != 4)
		return false;
	const unsigned long long dataOffset = (unsigned long long)record.localOffset + 30ull +
	                                      (unsigned long long)Rd16(local + 26) +
	                                      (unsigned long long)Rd16(local + 28);
	if (dataOffset + record.compSize > fileSize)
		return false;

	unsigned char* raw = (unsigned char*)malloc(record.compSize ? record.compSize : 1);
	if (!raw)
		return false;
	if (record.compSize && !read(user, dataOffset, raw, record.compSize))
	{
		free(raw);
		return false;
	}

	if (record.method == 0)
	{
		if (record.compSize != record.uncompSize)   // stored entries are verbatim, sizes equal
		{
			free(raw);
			return false;
		}
		*out    = raw;
		*outLen = record.compSize;
		return true;
	}

	unsigned char* inflated = (unsigned char*)malloc(record.uncompSize);
	size_t written = 0;
	if (inflated && InflateRaw(raw, record.compSize, inflated, record.uncompSize, written) &&
	    written == record.uncompSize)
	{
		*out    = inflated;
		*outLen = written;
		free(raw);
		return true;
	}
	free(inflated);
	free(raw);
	return false;
}

// =============================================================================================== //
// TGA decode: the .tga artwork the cgame registered for the icon.
//
// The 18-byte header is (RFC-adjacent but universally agreed):
//   0 id length, 1 color map type, 2 image type, 12 width, 14 height, 16 PIXEL DEPTH, 17 descriptor
//
// The pixel depth is byte 16 and the descriptor byte 17 - NOT the height's two bytes at 14/15.
// This file used to read d[14]/d[15] there, so the "depth" it saw was the low byte of the image
// height: a 64x64 icon looked like a 64bpp image and was rejected (the chip), a 32x32 one looked
// like a 32bpp image and was accepted by accident. That is the bug that made every stock icon in
// baseq3 draw as a chip.
//
// Accepted, like the engine's own LoadTGA: image type 2 (uncompressed) and 3 (gray), type 10
// (run-length encoded), 8/16/24/32bpp, top-down or bottom-up. The byte order is the one the engine
// reads: 32bpp is B,G,R,A and 24bpp is B,G,R (Q3's own art is written that way; a "detect the
// channel order statistically" heuristic sat here before and could swap blue with alpha on an icon
// whose pixels are mostly blue-less). The output is always a top-down RGBA buffer.
// =============================================================================================== //
bool WeaponEsp::DecodeTga(const unsigned char* d, size_t len, unsigned char** outRgba,
                          int* outW, int* outH)
{
	if (outRgba)
		*outRgba = 0;
	if (outW)
		*outW = 0;
	if (outH)
		*outH = 0;
	if (!d || !outRgba || !outW || !outH || len < 18)
		return false;

	const int idLen    = d[0];
	const int cmapType = d[1];
	const int imgType  = d[2];
	const int bpp      = d[16];
	const int desc     = d[17];
	const bool rle     = (imgType == 10);
	if (cmapType != 0 || (imgType != 2 && imgType != 3 && !rle))
		return false;
	if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
		return false;

	const int w = (int)(d[12] | (d[13] << 8));
	const int h = (int)(d[14] | (d[15] << 8));
	if (w < 1 || w > 512 || h < 1 || h > 512)
		return false;

	const size_t headerEnd = (size_t)18 + (size_t)idLen;
	if (headerEnd > len)
		return false;
	const size_t pixelBytes = (size_t)w * (size_t)h * (size_t)(bpp / 8);
	const unsigned char* px = d + headerEnd;

	// type 10: expand the run-length packets into a plain pixel buffer, so the conversion below
	// has one shape of input to deal with. The packets are per-pixel, not per-row, and - like the
	// engine - a packet that runs past the last pixel just stops there.
	unsigned char* expanded = 0;
	if (rle)
	{
		expanded = (unsigned char*)malloc(pixelBytes);
		if (!expanded)
			return false;
		const size_t step = (size_t)(bpp / 8);
		const unsigned char* p = px;
		const unsigned char* end = d + len;
		size_t done = 0;
		bool ok = true;
		while (done < pixelBytes)
		{
			if (p >= end)
			{
				ok = false;               // the packets ran out before the image was complete
				break;
			}
			const unsigned int packet = *p++;
			const unsigned int count  = 1 + (packet & 0x7fu);
			if (packet & 0x80u)
			{
				if (p + step > end)
				{
					ok = false;
					break;
				}
				for (unsigned int i = 0; i < count && done < pixelBytes; ++i)
				{
					memcpy(expanded + done, p, step);
					done += step;
				}
				p += step;
			}
			else
			{
				if (p + (size_t)count * step > end)
				{
					ok = false;
					break;
				}
				for (unsigned int i = 0; i < count && done < pixelBytes; ++i)
				{
					memcpy(expanded + done, p, step);
					p += step;
					done += step;
				}
			}
		}
		if (!ok || done < pixelBytes)
		{
			free(expanded);
			return false;
		}
		px = expanded;
	}
	else if (pixelBytes > len - headerEnd)
	{
		return false;                     // truncated uncompressed image
	}

	const bool topDown = (desc & 0x20) != 0;   // bit 5: the first row in the file is the top one
	const size_t srcStride = (size_t)w * (size_t)(bpp / 8);

	unsigned char* rgba = (unsigned char*)malloc((size_t)w * (size_t)h * 4);
	if (!rgba)
	{
		free(expanded);
		return false;
	}

	for (int y = 0; y < h; ++y)
	{
		const int row = topDown ? y : (h - 1 - y);
		const unsigned char* s = px + (size_t)y * srcStride;
		unsigned char* o = rgba + (size_t)row * (size_t)w * 4;
		for (int x = 0; x < w; ++x, o += 4)
		{
			switch (bpp)
			{
			case 8:
			{
				// grayscale
				const unsigned char v = s[x];
				o[0] = v;
				o[1] = v;
				o[2] = v;
				o[3] = 255;
				break;
			}
			case 16:
			{
				// 5-5-5 with the top bit as alpha (the TGA 16-bit layout)
				const unsigned int v = (unsigned int)s[x * 2] | ((unsigned int)s[x * 2 + 1] << 8);
				o[0] = (unsigned char)(((v >> 10) & 31u) * 255u / 31u);
				o[1] = (unsigned char)(((v >> 5) & 31u) * 255u / 31u);
				o[2] = (unsigned char)((v & 31u) * 255u / 31u);
				o[3] = 255;
				break;
			}
			case 24:
			{
				o[0] = s[x * 3 + 2];      // stored B,G,R
				o[1] = s[x * 3 + 1];
				o[2] = s[x * 3];
				o[3] = 255;
				break;
			}
			default:
			{
				o[0] = s[x * 4 + 2];      // stored B,G,R,A
				o[1] = s[x * 4 + 1];
				o[2] = s[x * 4];
				o[3] = s[x * 4 + 3];
				break;
			}
			}
		}
	}

	free(expanded);
	*outRgba = rgba;
	*outW    = w;
	*outH    = h;
	return true;
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
