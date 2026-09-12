// =============================================================================================== //
// kutaQ3 hook - the scanners behind vmFind.h (see that header for why they exist)
//
// No <windows.h>, no Detours, no engine globals in here: vmHook.cpp hands this file a memory range
// and gets back a pointer into it, which is what makes tests/test_vmfind.cpp able to drive it with
// a vm_t / gameState_t built from the real SDK headers.
// =============================================================================================== //

#include "vmFind.h"

#include <string.h>

namespace
{
	// "cgame" plus its terminator, compared as 6 bytes: vmTable slots are reused, so a record left
	// over from a freed VM must not match on a prefix of a longer name.
	const char kCgameName[] = "cgame";

	// configstrings the shape check leans on - CS_SERVERINFO and the clientinfo block
	const int kCsServerInfo = 0;

	bool InfoStringLooksLive(const char* s, size_t maxLen)
	{
		if (!s || maxLen < 4)
			return false;
		if (s[0] != '\\')
			return false;
		// every infostring the gamestate carries is "\key\value..." and is NUL terminated inside
		// the pool; a run of 0xff bytes would otherwise walk out of the segment
		size_t seen = 0;
		for (; seen < maxLen; ++seen)
		{
			if (s[seen] == 0)
				return seen > 1;
		}
		return false;
	}

	bool HasKeyValue(const char* s, const char* key)
	{
		// "\n\" / "\mapname\" - plain substring match, keys are lower case in 1.32
		char pattern[32];
		size_t n = 0;
		pattern[n++] = '\\';
		for (; *key && n + 2 < sizeof(pattern); ++key)
			pattern[n++] = *key;
		pattern[n++] = '\\';
		pattern[n] = 0;
		return strstr(s, pattern) != NULL;
	}

	// A configstring: printable (player names may carry high bytes), NUL terminated inside the
	// pool. Garbage bytes are overwhelmingly either control characters or an unterminated run, so
	// this is what rejects the false positives the cheap offset check lets through.
	bool StringLooksConfig(const char* s, size_t maxLen)
	{
		if (!s || maxLen == 0)
			return false;
		for (size_t i = 0; i < maxLen; ++i)
		{
			const unsigned char c = (unsigned char)s[i];
			if (c == 0)
				return i > 0;
			if (c < 32)
				return false;                          // control byte: not a configstring
		}
		return false;                                  // runs off the pool without a terminator
	}

	// -------------------------------------------------------------------------------------------
	// A gameState_t is 1024 string offsets, a 16000 byte packed string pool and a used byte count.
	// CL_ParseGamestate() appends each configstring the server enumerates (in ascending index
	// order, SV_SendClientGameState loops i = 0..MAX_CONFIGSTRINGS), so right after a parse the
	// offsets of the indices that were set are strictly increasing. A runtime "cs" server command
	// then updates one string, and the two engines do that differently:
	//
	//   - retail 1.32 (cl_cgame.c CL_ConfigstringModified) rebuilds the WHOLE pool in index order
	//     with the changed string woven in - the offsets stay ascending forever;
	//   - ioquake3 (CL_SetConfigstring) appends the new string at the END of the pool no matter
	//     what its index is - from the first such update on, the offsets are NOT in index order.
	//
	// Every check below is therefore order independent, so the scan recognises a live copy at any
	// moment under either engine instead of only in the first seconds after a gamestate parse:
	//
	//   - every set (nonzero) offset lands inside the pool, and no two set indices share one -
	//     neither engine ever produces two indices with one offset;
	//   - every set string is terminated inside the pool and control-character free;
	//   - CS_SERVERINFO carries \mapname\ and at least one CS_PLAYERS slot holds an infostring.
	//
	// Indices the server never sends (unused model/sound slots, empty player slots, ...) stay
	// zero, and the majority of the 1024 are zero on a live level. Skipping those zero gaps is
	// what recognises a real gameState in a megabyte of VM data - an earlier check required every
	// one of the 1024 offsets to be nonzero and increasing, which no shipped server ever produces,
	// so the scan failed until a runtime "cs" command happened to re-fire CG_GETGAMESTATE.
	// -------------------------------------------------------------------------------------------
	bool LooksLikeGameState(const q3::gameState_t* gs)
	{
		// cheapest discriminator first - this alone rejects essentially every other address in the
		// segment, which is what keeps the scan to a few milliseconds
		if (!VmFind::GameStateLooksLive(gs))
			return false;

		const int count = gs->dataCount;

		int setOffsets[q3::kMaxConfigStrings];
		int setCount = 0;
		for (int i = 0; i < q3::kMaxConfigStrings; ++i)
		{
			const int offset = gs->stringOffsets[i];
			if (offset == 0)
				continue;                               // index never set: a gap, not corruption
			if (offset < 1 || offset >= count)
				return false;                           // outside the pool it cannot index
			for (int j = 0; j < setCount; ++j)
			{
				if (setOffsets[j] == offset)
					return false;                       // two indices, one string: not a pool
			}
			setOffsets[setCount++] = offset;
		}
		if (setCount < 2)
			return false;                              // serverinfo plus at least one more string

		for (int i = 0; i < setCount; ++i)
		{
			if (!StringLooksConfig(gs->stringData + setOffsets[i], (size_t)(count - setOffsets[i])))
				return false;
		}

		// the pool starts with the serverinfo, and the clientinfo block has to hold at least one
		// player: an empty gameState (loading screen, wrong structure) is not what we want
		if (!InfoStringLooksLive(q3::ConfigString(gs, kCsServerInfo), (size_t)count))
			return false;
		if (!HasKeyValue(q3::ConfigString(gs, kCsServerInfo), "mapname"))
			return false;

		for (int i = 0; i < q3::kMaxClients; ++i)
		{
			const char* info = q3::ConfigString(gs, q3::kCsPlayers + i);
			if (info[0] == '\\')
				return true;
		}
		return false;
	}
}

// =============================================================================================== //

bool VmFind::GameStateLooksLive(const q3::gameState_t* gs)
{
	if (!gs)
		return false;

	// CL_ParseGamestate() starts dataCount at 1 - the zero byte every configstring is read against
	// - so a live copy always has a used pool within the 16000 byte limit, and CS_SERVERINFO
	// (which every server sends) points into it. Two int reads, safe to call every frame.
	//
	// The offset of CS_SERVERINFO is deliberately NOT required to be 1. It is 1 in a freshly
	// parsed gamestate, because the serverinfo is the first string CL_SetConfigstring() appends -
	// but that same function appends at the END of the pool for every runtime "cs" server command,
	// so the first update of CS_SERVERINFO moves its offset to the highest in the table. Keying on
	// it made this check - and with it the bridge that serves the configstrings to Gather() -
	// report a perfectly good copy as dead from that moment on, and the scan could never find it
	// again either: names stopped, and stayed stopped until the next CG_GETGAMESTATE happened to
	// re-capture the address.
	const int count = gs->dataCount;
	if (count <= 1 || count > q3::kMaxGamestateChars)
		return false;
	const int serverInfo = gs->stringOffsets[0];
	return serverInfo >= 1 && serverInfo < count;
}

bool VmFind::GameStateIsUsable(const q3::gameState_t* gs)
{
	return LooksLikeGameState(gs);
}

bool VmFind::SameVmInstance(const Record& rec, uint32_t capturedDataBase, uint32_t capturedDllHandle)
{
	// A different cgame module was loaded (or one where there was none): every pointer captured
	// from the old one points into freed / foreign memory.
	if (rec.dllHandle != capturedDllHandle)
		return false;

	// Native DLL cgame: the module is the identity. VM_Create() never touches dataBase on the
	// native path, so it is 0 in both and carries no information.
	if (rec.dllHandle != 0)
		return true;

	// Bytecode: the hunk segment VM_Create() allocated is the identity. Same segment, same
	// instance - which is also the level-change case, and exactly when a captured pointer to the
	// cgame's own globals must survive.
	return rec.dataBase == capturedDataBase;
}

// =============================================================================================== //

bool VmFind::IsCgameVm(const Record* candidate, size_t bytesAvailable,
                       uintptr_t codeLow, uintptr_t codeHigh, bool* native)
{
	if (native)
		*native = false;
	if (!candidate || bytesAvailable < sizeof(Record))
		return false;

	// exactly "cgame", terminator included
	if (memcmp(candidate->name, kCgameName, sizeof(kCgameName)) != 0)
		return false;

	// systemCall is CL_CgameSystemCalls, a function in the main module. With codeLow == codeHigh
	// the caller has no module range to offer (the tests) and the check is skipped.
	if (candidate->systemCall == 0)
		return false;
	if (codeHigh > codeLow)
	{
		if ((uintptr_t)candidate->systemCall < codeLow || (uintptr_t)candidate->systemCall >= codeHigh)
			return false;
	}

	if (candidate->dllHandle != 0)
	{
		// VM_Create() returns from the native path before it touches programStack / dataBase, so a
		// DLL cgame has all four still zeroed and entryPoint pointing into the cgame DLL (which is
		// not the main module, hence no range check here).
		if (candidate->entryPoint == 0)
			return false;
		if (candidate->dataBase != 0 || candidate->dataMask != 0)
			return false;
		if (candidate->programStack != 0 || candidate->stackBottom != 0)
			return false;

		if (native)
			*native = true;
		return true;
	}

	// bytecode: the hunk-backed data segment VM_Create() allocated, rounded up to a power of two
	// so the interpreter can mask every address, with the VM stack parked just above it.
	const int32_t mask = candidate->dataMask;
	if (mask <= 0)
		return false;

	const uint32_t umask = (uint32_t)mask;
	if (((umask + 1u) & umask) != 0)
		return false;                                   // not 2^n - 1

	int bits = 0;
	while ((((uint32_t)1 << bits) - 1u) < umask)
		++bits;
	if (bits < kMinDataBits || bits > 28)
		return false;                                   // not a cgame-sized segment

	if (candidate->dataBase == 0)
		return false;
	if (candidate->codeBase == 0 || candidate->codeLength <= 0)
		return false;
	if (candidate->programStack != (int32_t)(umask + 1u))
		return false;                                   // vm.c: programStack = dataMask + 1
	if (candidate->stackBottom != candidate->programStack - kStackSize)
		return false;                                   // vm.c: stackBottom = programStack - STACK_SIZE

	return true;
}

bool VmFind::FindVm(const void* region, size_t size,
                    uintptr_t codeLow, uintptr_t codeHigh, Found& out)
{
	out.record = NULL;
	out.native = false;

	if (!region || size < sizeof(Record))
		return false;

	const char* base = (const char*)region;
	for (size_t offset = 0; offset + sizeof(Record) <= size; offset += 4)
	{
		const Record* candidate = (const Record*)(base + offset);
		bool native = false;
		if (IsCgameVm(candidate, size - offset, codeLow, codeHigh, &native))
		{
			out.record = candidate;
			out.native = native;
			return true;
		}
	}
	return false;
}

bool VmFind::FindGameState(const void* region, size_t size, const q3::gameState_t** out)
{
	if (out)
		*out = NULL;
	if (!region || size < sizeof(q3::gameState_t))
		return false;

	const char* base = (const char*)region;
	for (size_t offset = 0; offset + sizeof(q3::gameState_t) <= size; offset += 4)
	{
		const q3::gameState_t* candidate = (const q3::gameState_t*)(base + offset);
		if (LooksLikeGameState(candidate))
		{
			if (out)
				*out = candidate;
			return true;
		}
	}
	return false;
}
