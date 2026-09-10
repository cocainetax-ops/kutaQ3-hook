// =============================================================================================== //
// kutaQ3 hook tests - the scanners in vmFind.cpp
//
// vmFind.cpp is what turns "there is a cgame VM somewhere in quake3.exe" into a pointer, and it is
// the piece of the VM hook that is pure logic - so it is compiled and run here, the real file, with
// records and gameStates built the way VM_Create() / CL_ParseGamestate() build them.
//
// The scanner consumes VmFind::Record (the x86 layout of struct vm_s, checked against vm_local.h by
// SDK/code/client/cl_sdkmirror.cpp), so the tests build records of that type directly. The
// addresses inside them are fake on purpose: vmFind only ever compares systemCall / dataBase, it
// never dereferences them, which keeps the tests host independent.
//
//     make -C tests check
// =============================================================================================== //

#include "vmFind.h"
#include "check.h"

#include <string.h>
#include <stdio.h>

namespace
{
	// stand-ins for the real addresses. vmFind compares, never follows.
	const uint32_t  kSystemCall = 0x004a1000u;   // CL_CgameSystemCalls
	const uintptr_t kCodeLow    = 0x00400000u;   // the main module
	const uintptr_t kCodeHigh   = 0x00600000u;
	const uint32_t  kDataBase   = 0x10000000u;   // a Hunk_Alloc block
	const int       kDataBits   = 21;            // 2 MB data segment

	VmFind::Record MakeBytecodeVm(const char* name = "cgame")
	{
		VmFind::Record r;
		memset(&r, 0, sizeof(r));
		memcpy(r.name, name, strlen(name) + 1);
		r.systemCall   = kSystemCall;
		r.dataBase     = kDataBase;
		r.dataMask     = (int32_t)((1u << kDataBits) - 1u);
		r.programStack = r.dataMask + 1;               // vm.c: programStack = dataMask + 1
		r.stackBottom  = r.programStack - VmFind::kStackSize;
		r.codeBase     = kDataBase - 0x100000;
		r.codeLength   = 393216;
		r.compiled     = 1;
		return r;
	}

	VmFind::Record MakeNativeVm(const char* name = "cgame")
	{
		VmFind::Record r;
		memset(&r, 0, sizeof(r));
		memcpy(r.name, name, strlen(name) + 1);
		r.systemCall = kSystemCall;
		r.dllHandle  = 0x60000000u;                    // cgame_mp_x86.dll
		r.entryPoint = 0x60001234u;                    // its vmMain
		return r;
	}

	// a stand-in for the executable's .data: junk, the record, more junk
	struct FakeDataSection
	{
		unsigned char bytes[8192];
		static const size_t kRecordAt = 3072;

		FakeDataSection()
		{
			for (size_t i = 0; i < sizeof(bytes); ++i)
				bytes[i] = (unsigned char)(i * 7 + 3);
		}

		void Place(const VmFind::Record& r)
		{
			memcpy(bytes + kRecordAt, &r, sizeof(r));
		}

		const void* base() const { return bytes; }
		size_t size() const { return sizeof(bytes); }
	};

	bool FindIn(const FakeDataSection& data, VmFind::Found& out)
	{
		return VmFind::FindVm(data.base(), data.size(), kCodeLow, kCodeHigh, out);
	}

	bool Accepted(const VmFind::Record& r, bool* native = NULL)
	{
		return VmFind::IsCgameVm(&r, sizeof(r), kCodeLow, kCodeHigh, native);
	}

	// -------------------------------------------------------------------------------------------
	// a gameState_t filled exactly the way CL_ParseGamestate() fills the engine's copy: dataCount
	// starts at 1 (the zero byte every configstring is read against), then each configstring is
	// appended and its offset recorded before the append.
	// -------------------------------------------------------------------------------------------
	void AddConfigString(q3::gameState_t& gs, int index, const char* value)
	{
		gs.stringOffsets[index] = gs.dataCount;
		const size_t len = strlen(value) + 1;
		memcpy(gs.stringData + gs.dataCount, value, len);
		gs.dataCount += (int)len;
	}

	void MakeGameState(q3::gameState_t& gs, bool withPlayers = true, bool withMapname = true)
	{
		memset(&gs, 0, sizeof(gs));
		gs.dataCount = 1;                         // stringData[0] = 0

		// index 0 is always CS_SERVERINFO and is the very first string appended, so its
		// offset is 1 - the invariant GameStateLooksLive() keys on.
		AddConfigString(gs, 0, withMapname ? "\\mapname\\q3dm1\\g_gametype\\0\\sv_hostname\\test"
		                                   : "\\hostname\\test");
		AddConfigString(gs, 6, "\\protocol\\68");

		for (int i = 0; i < 4; ++i)              // inline models: *1..*4 at CS_MODELS (32)
		{
			char model[16];
			snprintf(model, sizeof(model), "*%d", i + 1);
			AddConfigString(gs, q3::kCsModels + i, model);
		}
		AddConfigString(gs, q3::kCsSounds + 0, "sound/world/alarmlp1.wav");
		AddConfigString(gs, q3::kCsSounds + 1, "sound/player/footsteps/step1.wav");

		if (withPlayers)
		{
			AddConfigString(gs, q3::kCsPlayers + 1, "\\n\\Bitterman\\t\\1\\model\\sarge");
			AddConfigString(gs, q3::kCsPlayers + 3, "\\n\\Keel\\t\\2\\model\\keel");
		}
		// every other slot stays at offset 0, as it does in the engine for configstrings the
		// server never set
	}

	// The (unrealistic) fully packed pattern every older test assumed: all 1024 slots set and
	// strictly increasing. The scanner must keep accepting this too - the fix was to legalise
	// zero gaps, not to require them.
	void MakeFullyPackedGameState(q3::gameState_t& gs)
	{
		memset(&gs, 0, sizeof(gs));
		gs.dataCount = 1;
		for (int i = 0; i < q3::kMaxConfigStrings; ++i)
		{
			char value[40];
			if (i == 0)
				snprintf(value, sizeof(value), "\\mapname\\q3dm1\\i\\%d", i);
			else
				snprintf(value, sizeof(value), "\\k\\v%d", i);
			AddConfigString(gs, i, value);
		}
	}
}

// =============================================================================================== //

static void TestRecordLayout()
{
	Section("VmFind::Record - the vm_t mirror");

	CHECK_INT(sizeof(VmFind::Record), 204, "sizeof(vm_t) on x86");
	CHECK_INT(offsetof(VmFind::Record, systemCall), 4, "systemCall at VM_OFFSET_SYSTEM_CALL");
	CHECK_INT(offsetof(VmFind::Record, name), 8, "name right after the function pointer");
	CHECK_INT(offsetof(VmFind::Record, dataBase), 104, "dataBase");
	CHECK_INT(offsetof(VmFind::Record, dataMask), 108, "dataMask");
	CHECK_INT(offsetof(VmFind::Record, stackBottom), 112, "stackBottom");
	CHECK_INT(VmFind::kStackSize, 0x20000, "STACK_SIZE from vm.c");
}

static void TestFindVm()
{
	Section("VmFind::FindVm - locating the cgame VM record");

	FakeDataSection data;
	VmFind::Record vm = MakeBytecodeVm();
	data.Place(vm);

	VmFind::Found found;
	CHECK_TRUE(FindIn(data, found), "the cgame record is found");
	CHECK_TRUE(found.record == (const VmFind::Record*)(data.bytes + FakeDataSection::kRecordAt),
	           "found at the offset it was placed at");
	CHECK_TRUE(!found.native, "a bytecode VM is not native");
	CHECK_UINT(found.record->systemCall, kSystemCall, "systemCall is the dispatcher we hook");
	CHECK_UINT(found.record->dataMask, (1u << kDataBits) - 1u, "dataMask read back");

	// native cgame (vm_cgame 0, off pure servers): same hook, different record shape
	FakeDataSection nativeData;
	nativeData.Place(MakeNativeVm());
	bool native = false;
	CHECK_TRUE(FindIn(nativeData, found), "a native cgame record is found");
	CHECK_TRUE(found.native, "reported as native");
	CHECK_TRUE(Accepted(MakeNativeVm(), &native) && native, "IsCgameVm says native too");

	// a record the engine has just freed (VM_Free memsets it) must not match
	FakeDataSection freed;
	VmFind::Record zeroed;
	memset(&zeroed, 0, sizeof(zeroed));
	freed.Place(zeroed);
	CHECK_TRUE(!FindIn(freed, found), "a zeroed (VM_Free'd) record is not a VM");

	// nothing that looks like a VM anywhere
	FakeDataSection empty;
	CHECK_TRUE(!FindIn(empty, found), "junk alone finds nothing");
	CHECK_TRUE(!VmFind::FindVm(NULL, 0, kCodeLow, kCodeHigh, found), "NULL region is rejected");
	CHECK_TRUE(!VmFind::FindVm(data.base(), sizeof(VmFind::Record) - 1, kCodeLow, kCodeHigh, found),
	           "a region too small for a record is rejected");
}

static void TestVmRejection()
{
	Section("VmFind::IsCgameVm - what is rejected");

	VmFind::Record vm = MakeBytecodeVm();
	CHECK_TRUE(Accepted(vm), "the well formed record passes");

	// ---- the name is the primary discriminator: vmTable slots are reused ------------------------
	VmFind::Record other = vm;
	memcpy(other.name, "ui", 3);
	CHECK_TRUE(!Accepted(other), "the UI VM is not the cgame");
	other = vm;
	memcpy(other.name, "qagame", 7);
	CHECK_TRUE(!Accepted(other), "the server VM is not the cgame");
	other = vm;
	memcpy(other.name, "cgame2", 7);
	CHECK_TRUE(!Accepted(other), "a name that merely starts with cgame is rejected");
	other = vm;
	other.name[5] = 'x';                       // "cgame" without its terminator
	CHECK_TRUE(!Accepted(other), "an unterminated cgame is rejected");

	// ---- systemCall has to be engine code -------------------------------------------------------
	other = vm;
	other.systemCall = 0;
	CHECK_TRUE(!Accepted(other), "no dispatcher, no hook");
	other = vm;
	other.systemCall = 0x70000000u;            // outside the main module
	CHECK_TRUE(!Accepted(other), "a dispatcher outside the module is rejected");
	other = vm;
	CHECK_TRUE(Accepted(other, NULL) && Accepted(other), "a NULL native flag is allowed");

	// ---- the bytecode invariants VM_Create() leaves behind --------------------------------------
	other = vm;
	other.dataBase = 0;
	CHECK_TRUE(!Accepted(other), "no data segment");
	other = vm;
	other.dataMask = 0x12345;                  // not 2^n - 1
	CHECK_TRUE(!Accepted(other), "a dataMask that is not 2^n-1");
	other = vm;
	other.dataMask = (1 << 12) - 1;            // too small for a cgame
	other.programStack = other.dataMask + 1;
	other.stackBottom = other.programStack - VmFind::kStackSize;
	CHECK_TRUE(!Accepted(other), "a 4 KB data segment is not a cgame");
	other = vm;
	other.programStack = other.dataMask;       // off by one
	CHECK_TRUE(!Accepted(other), "programStack must be dataMask + 1");
	other = vm;
	other.stackBottom = other.programStack - 0x10000;
	CHECK_TRUE(!Accepted(other), "stackBottom must be programStack - STACK_SIZE");
	other = vm;
	other.codeBase = 0;
	CHECK_TRUE(!Accepted(other), "no code");
	other = vm;
	other.codeLength = 0;
	CHECK_TRUE(!Accepted(other), "an empty code segment");

	// ---- the native shape -----------------------------------------------------------------------
	other = MakeNativeVm();
	other.entryPoint = 0;
	CHECK_TRUE(!Accepted(other), "a DLL with no vmMain");
	other = MakeNativeVm();
	other.dataMask = 4095;
	CHECK_TRUE(!Accepted(other), "a DLL VM has no data segment");
	other = MakeNativeVm();
	other.programStack = 0x20000;
	CHECK_TRUE(!Accepted(other), "a DLL VM never gets a program stack");

	// ---- a truncated record ---------------------------------------------------------------------
	CHECK_TRUE(!VmFind::IsCgameVm(&vm, sizeof(vm) - 1, kCodeLow, kCodeHigh, NULL),
	           "a record that does not fit is rejected");
	CHECK_TRUE(!VmFind::IsCgameVm(NULL, sizeof(vm), kCodeLow, kCodeHigh, NULL),
	           "NULL is rejected");
}

// vm_cgame is a three-way cvar (qcommon.h: VMI_NATIVE 0 / VMI_BYTECODE 1 / VMI_COMPILED 2) and the
// hook has to find the record for whichever the player is running. 2 is the shipped default, but 1
// is what you get with the JIT off, and both take the *same* QVM path in VM_Create(): the only thing
// that differs is who filled in codeBase - VM_Compile() emits x86, VM_PrepareInterpreter() does
// `vm->codeBase = Hunk_Alloc( vm->codeLength*4, h_high )` and translates the opcode stream. Both
// leave codeBase != 0, codeLength > 0 and the identical dataBase / dataMask / programStack /
// stackBottom, and both reach the traps through the same vm->systemCall pointer:
//
//   vm_interpreted.c  VM_CallInterpreted : r = vm->systemCall( (int *)&image[ programStack + 4 ] );
//   vm_x86.c          AsmCall            : currentVM->systemCall( (int *)((byte *)currentVM->dataBase + programStack + 4) )
//
// so acceptance must not depend on the mode flags at all.
static void TestInterpretModes()
{
	Section("VmFind::IsCgameVm - vm_cgame 1 and 2 both qualify");

	// vm_cgame 2, VMI_COMPILED: the default. VM_Compile() set compiled and cleared the flag.
	VmFind::Record jit = MakeBytecodeVm();
	jit.compiled = 1;
	jit.currentlyInterpreting = 0;
	CHECK_TRUE(Accepted(jit), "vm_cgame 2 (JIT bytecode, the shipped default) is accepted");

	// vm_cgame 1, VMI_BYTECODE: VM_PrepareInterpreter() translates in place and the interpreter
	// loop sets currentlyInterpreting on every VM_CallInterpreted().
	VmFind::Record interp = MakeBytecodeVm();
	interp.compiled = 0;
	interp.currentlyInterpreting = 1;
	interp.codeLength = 393216 / 4;            // the raw qvm stream, not the x86 expansion
	CHECK_TRUE(Accepted(interp), "vm_cgame 1 (interpreted bytecode) is accepted");

	// vm_cgame 0, VMI_NATIVE: a mod DLL, the odd one out, still accepted so the hook does not
	// silently do nothing for the people who run it.
	VmFind::Record native = MakeNativeVm();
	bool nativeOut = false;
	CHECK_TRUE(VmFind::IsCgameVm(&native, sizeof(native), kCodeLow, kCodeHigh, &nativeOut),
	           "vm_cgame 0 (native DLL) is accepted");
	CHECK_TRUE(nativeOut, "and is reported as native");

	// ---- the mode flags must not be load-bearing -----------------------------------------------
	bool jitNative = true;
	CHECK_TRUE(VmFind::IsCgameVm(&jit, sizeof(jit), kCodeLow, kCodeHigh, &jitNative) && !jitNative,
	           "the JIT record is not mistaken for a native one");
	VmFind::Record flipped = interp;
	flipped.compiled = 1;                      // VM_Compile() shape, interpreter flag still set
	CHECK_TRUE(Accepted(flipped), "compiled does not gate acceptance");
	flipped = interp;
	flipped.currentlyInterpreting = 0;         // between frames the interpreter is not running
	CHECK_TRUE(Accepted(flipped), "currentlyInterpreting does not gate acceptance");

	// ... but the QVM invariants they share still do, in both modes.
	VmFind::Record broken = interp;
	broken.dataBase = 0;
	CHECK_TRUE(!Accepted(broken), "an interpreted VM without a data segment is still rejected");
	broken = jit;
	broken.programStack = broken.dataMask;
	CHECK_TRUE(!Accepted(broken), "a JIT VM with a bad programStack is still rejected");

	// ---- both are findable in a .data-shaped region, not just acceptable in isolation ----------
	FakeDataSection data;
	data.Place(interp);
	VmFind::Found found;
	CHECK_TRUE(VmFind::FindVm(data.base(), sizeof(FakeDataSection::bytes), kCodeLow, kCodeHigh, found),
	           "the interpreted record is found by the scan");
	CHECK_TRUE(!found.native, "and is not reported as native");
}

static void TestFindGameState()
{
	Section("VmFind::FindGameState - the cgame's configstrings");

	// A real gamestate is sparse: only a handful of the 1024 indices are ever set and the rest
	// stay at offset 0. The scanner used to reject every zero gap, which meant the cgame's
	// copy was never found and the ESP waited for the first runtime "cs" command (~a minute).
	static q3::gameState_t gs;
	MakeGameState(gs);
	CHECK_TRUE(VmFind::GameStateLooksLive(&gs), "the sparse gameState we built looks live");

	static unsigned char segment[1u << 20];
	const size_t placedAt = 777 * 1024;
	for (size_t i = 0; i < sizeof(segment); ++i)
		segment[i] = (unsigned char)(i * 31 + 5);
	memcpy(segment + placedAt, &gs, sizeof(gs));

	const q3::gameState_t* found = NULL;
	CHECK_TRUE(VmFind::FindGameState(segment, sizeof(segment), &found),
	           "a realistic, sparse gameState is found (the ~1-minute regression)");
	CHECK_TRUE(found == (const q3::gameState_t*)(segment + placedAt), "found where it was placed");

	// the names the ESP needs are reachable through it
	char name[64] = { 0 };
	CHECK_TRUE(q3::InfoValueForKey(q3::ConfigString(found, q3::kCsPlayers + 1), "n", name, sizeof(name)),
	           "CS_PLAYERS+1 carries a name");
	CHECK_STR(name, "Bitterman", "and it is the one we wrote");

	// the densely packed (impossible-in-practice) pattern is still accepted
	static q3::gameState_t dense;
	MakeFullyPackedGameState(dense);
	found = NULL;
	CHECK_TRUE(VmFind::FindGameState(&dense, sizeof(dense), &found), "a fully packed table is accepted too");

	// ---- corruptions ---------------------------------------------------------------------------
	q3::gameState_t bad = gs;
	bad.dataCount = 0;
	CHECK_TRUE(!VmFind::GameStateLooksLive(&bad), "dataCount 0 is not live");
	bad = gs;
	bad.dataCount = q3::kMaxGamestateChars + 1;
	CHECK_TRUE(!VmFind::GameStateLooksLive(&bad), "a dataCount past the pool is not live");
	bad = gs;
	bad.stringOffsets[0] = 5;
	CHECK_TRUE(!VmFind::GameStateLooksLive(&bad), "stringOffsets[0] must be 1");
	CHECK_TRUE(!VmFind::GameStateLooksLive(NULL), "NULL is not live");

	// two SET slots sharing an offset: the packed order is broken
	bad = gs;
	bad.stringOffsets[q3::kCsSounds + 1] = bad.stringOffsets[q3::kCsSounds + 0];
	found = NULL;
	CHECK_TRUE(!VmFind::FindGameState(&bad, sizeof(bad), &found),
	           "duplicate offsets among set slots are rejected");

	// a set slot pointing outside the used pool
	bad = gs;
	bad.stringOffsets[q3::kCsModels + 2] = 999999;
	found = NULL;
	CHECK_TRUE(!VmFind::FindGameState(&bad, sizeof(bad), &found),
	           "a set offset past dataCount is rejected");

	// a set slot out of order (appended strings must be ascending by index)
	bad = gs;
	bad.stringOffsets[q3::kCsSounds + 1] = bad.stringOffsets[q3::kCsModels + 0];
	found = NULL;
	CHECK_TRUE(!VmFind::FindGameState(&bad, sizeof(bad), &found),
	           "a non-monotonic set offset is rejected");

	bad = gs;
	MakeGameState(bad, true, false);                   // serverinfo without mapname
	found = NULL;
	CHECK_TRUE(!VmFind::FindGameState(&bad, sizeof(bad), &found), "a serverinfo without mapname is rejected");

	bad = gs;
	MakeGameState(bad, false, true);                   // no clientinfo at all
	found = NULL;
	CHECK_TRUE(!VmFind::FindGameState(&bad, sizeof(bad), &found), "no players -> not the gameState we want");

	CHECK_TRUE(!VmFind::FindGameState(NULL, 0, &found), "an empty region is rejected");
	CHECK_TRUE(!VmFind::FindGameState(segment, sizeof(q3::gameState_t) - 1, &found),
	           "a region too small for a gameState is rejected");
}

// =============================================================================================== //

int main(void)
{
	printf("kutaQ3 hook tests - VM record / gameState scanners (vmFind.cpp)\n");
	printf("  sizeof(VmFind::Record) = %u, sizeof(q3::gameState_t) = %u\n",
	       (unsigned)sizeof(VmFind::Record), (unsigned)sizeof(q3::gameState_t));

	TestRecordLayout();
	TestFindVm();
	TestVmRejection();
	TestInterpretModes();
	TestFindGameState();

	printf("\n%d checks, %d failed - %s\n", g_checks, g_failed, g_failed ? "FAILED" : "all passed");
	return g_failed ? 1 : 0;
}
