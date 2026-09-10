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

	// -------------------------------------------------------------------------------------------
	// A gameState_t is 1024 string offsets, a 16000 byte packed string pool and a used byte count.
	// CL_ParseGamestate() appends each configstring the server enumerates (in ascending index
	// order, SV_SendClientGameState loops i = 0..MAX_CONFIGSTRINGS), and CL_SetConfigstring()
	// rebuilds the pool the same way when one changes at runtime; the offsets of the indices that
	// were actually set are therefore strictly increasing and all index the pool. Indices the
	// server never sends (unused model/sound slots, empty player slots, ...) stay zero, and the
	// majority of the 1024 are zero on a live level. Skipping those zero gaps is what recognises
	// a real gameState in a megabyte of VM data - an earlier check required every one of the
	// 1024 offsets to be nonzero and increasing, which no shipped server ever produces, so the
	// scan failed until the first runtime "cs" command happened to re-fire CG_GETGAMESTATE.
	// -------------------------------------------------------------------------------------------
	bool LooksLikeGameState(const q3::gameState_t* gs)
	{
		// cheapest discriminator first - this alone rejects essentially every other address in the
		// segment, which is what keeps the scan to a few milliseconds
		if (!VmFind::GameStateLooksLive(gs))
			return false;

		const int count = gs->dataCount;

		// Walk the set slots (nonzero offsets) in ascending index order: their offsets were
		// appended in that order, so they must be strictly increasing and inside the pool.
		int prev = 0;
		int setCount = 0;
		for (int i = 0; i < q3::kMaxConfigStrings; ++i)
		{
			const int offset = gs->stringOffsets[i];
			if (offset == 0)
				continue;                               // index never set: a gap, not corruption
			if (offset <= prev || offset >= count)
				return false;
			prev = offset;
			++setCount;
		}
		if (setCount < 2)
			return false;                              // serverinfo plus at least one more string

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
	// - and assigns stringOffsets[0] before appending the first string, so it is 1 too.
	const int count = gs->dataCount;
	if (count <= 1 || count > q3::kMaxGamestateChars)
		return false;
	return gs->stringOffsets[0] == 1;
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
