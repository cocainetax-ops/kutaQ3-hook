#include "main.h"        // Log()
#include "cgameHook.h"
#include "nameEsp.h"
#include "config.h"      // Config::g_Settings.nameEsp

#include <tlhelp32.h>
#include <stdio.h>

// =============================================================================================== //
// kutaQ3 hook - Detours hook on the cgame VM (see cgameHook.h for the overview)
// =============================================================================================== //

namespace
{
	// The cgame module's exports, spelled the way the engine prototypes them (cg_main.c vmMain,
	// cg_syscalls.c dllEntry). On x86 int and intptr_t are the same size, so this is the same
	// calling shape either way.
	typedef int  (Q3SDK_CDECL *cgameVmMain_t)(int command, int arg0, int arg1, int arg2, int arg3,
	                                          int arg4, int arg5, int arg6, int arg7, int arg8,
	                                          int arg9, int arg10, int arg11);
	typedef void (Q3SDK_CDECL *cgameDllEntry_t)(int (Q3SDK_CDECL *dllSyscall)(int, ...));

	HMODULE          s_module       = NULL;   // the cgame DLL our detours are patched into
	cgameVmMain_t    s_origVmMain   = NULL;   // trampolines, owned by Detours
	cgameDllEntry_t  s_origDllEntry = NULL;
	q3::syscall_t    s_syscall      = NULL;   // captured in newCgameDllEntry

	DWORD            s_lastPollMs   = 0;
	char             s_status[192]  = "no cgame VM loaded";

	// known retail / ioquake3 / mod names, cheapest first
	const char* const kKnownModules[] = { "cgame_mp_x86.dll", "cgamex86.dll", "cgame_x86.dll" };

	// modules that turned out not to be hookable (no vmMain export, Detours refused them). Poll()
	// looks every half second, so without this the same failure would be logged twice a second.
	HMODULE s_refused[4] = { NULL, NULL, NULL, NULL };

	bool WasRefused(HMODULE module)
	{
		for (size_t i = 0; i < sizeof(s_refused) / sizeof(s_refused[0]); ++i)
			if (s_refused[i] == module)
				return true;
		return false;
	}

	void RememberRefused(HMODULE module)
	{
		for (size_t i = 0; i < sizeof(s_refused) / sizeof(s_refused[0]); ++i)
		{
			if (s_refused[i] == NULL)
			{
				s_refused[i] = module;
				return;
			}
		}
		s_refused[0] = module;
	}

	void SetStatus(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		vsprintf_s(s_status, fmt, ap);
		va_end(ap);
	}

	// Is this module handle still mapped? GetModuleHandleEx on the module base tells us without
	// touching the (possibly reused) memory itself.
	bool ModuleResident(HMODULE module)
	{
		if (!module)
			return false;
		HMODULE found = NULL;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        (LPCSTR)module, &found))
			return false;
		return found == module;
	}

	void ModuleBaseName(const char* path, char* out, size_t outSize)
	{
		const char* name = path ? path : "";
		for (const char* p = name; *p; ++p)
		{
			if (*p == '\\' || *p == '/' || *p == ':')
				name = p + 1;
		}
		strncpy_s(out, outSize, name, _TRUNCATE);
	}

	// -------------------------------------------------------------------------------------------
	// the detours
	// -------------------------------------------------------------------------------------------

	void Q3SDK_CDECL newCgameDllEntry(int (Q3SDK_CDECL *dllSyscall)(int, ...))
	{
		// The engine hands the cgame its syscall trampoline here, once, right after the DLL is
		// mapped and before vmMain ever runs. This pointer is the door into the client state.
		s_syscall = (q3::syscall_t)dllSyscall;
		Log("[kutaQ3] cgame dllEntry: captured syscall trampoline %p", (void*)dllSyscall);

		if (s_origDllEntry)
			s_origDllEntry(dllSyscall);
	}

	int Q3SDK_CDECL newCgameVmMain(int command, int arg0, int arg1, int arg2, int arg3, int arg4,
	                               int arg5, int arg6, int arg7, int arg8, int arg9, int arg10,
	                               int arg11)
	{
		switch (command)
		{
		case q3::CG_INIT:
			// CG_Init( serverMessageNum, serverCommandSequence, clientNum )
			Log("[kutaQ3] cgame CG_INIT (local clientNum %d), syscall %s", arg2,
			    s_syscall ? "captured" : "MISSING - inject before the map loads");
			SetStatus(s_syscall ? "hooked (cgame initialising)"
			                    : "hooked, syscall trampoline missed");
			NameEsp::Reset();
			break;

		case q3::CG_SHUTDOWN:
			// level teardown: the tags describe a world that is going away
			NameEsp::Reset();
			break;

		case q3::CG_DRAW_ACTIVE_FRAME:
			// arg0 is cl.serverTime. This is the one point in the frame where the cgame VM is
			// current, so it is the only place the engine will resolve our pointers (see
			// cgameHook.h). Reading is skipped entirely while the feature is off - a snapshot copy
			// is ~52 KB per frame.
			if (Config::g_Settings.nameEsp)
				NameEsp::Gather(arg0, s_syscall);
			else if (NameEsp::Current().valid)
				NameEsp::Reset();
			break;

		default:
			break;
		}

		if (!s_origVmMain)
			return -1;
		return s_origVmMain(command, arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9,
		                    arg10, arg11);
	}

	// -------------------------------------------------------------------------------------------

	bool Attach(HMODULE module)
	{
		if (!module)
			return false;
		if (module == s_module)
			return true;

		if (WasRefused(module))
			return false;

		FARPROC vmMain = GetProcAddress(module, "vmMain");
		if (!vmMain)
		{
			Log("[kutaQ3] %p has no vmMain export - not hooking it", (void*)module);
			RememberRefused(module);
			return false;
		}
		FARPROC dllEntry = GetProcAddress(module, "dllEntry");

		cgameVmMain_t   origVmMain   = (cgameVmMain_t)vmMain;
		cgameDllEntry_t origDllEntry = (cgameDllEntry_t)dllEntry;

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&(PVOID&)origVmMain, newCgameVmMain);
		if (origDllEntry)
			DetourAttach(&(PVOID&)origDllEntry, newCgameDllEntry);
		const LONG result = DetourTransactionCommit();

		char modulePath[MAX_PATH] = { 0 };
		if (!GetModuleFileNameA(module, modulePath, sizeof(modulePath)) || !modulePath[0])
			strcpy_s(modulePath, "cgame module");

		char moduleName[MAX_PATH] = { 0 };
		ModuleBaseName(modulePath, moduleName, sizeof(moduleName));

		if (result != NO_ERROR)
		{
			Log("[kutaQ3] DetourTransactionCommit failed on %s: %ld", modulePath, result);
			SetStatus("hook failed (Detours %ld)", result);
			RememberRefused(module);
			return false;
		}

		s_module       = module;
		s_origVmMain   = origVmMain;
		s_origDllEntry = origDllEntry;

		Log("[kutaQ3] hooked vmMain %p (dllEntry %p) in %s", (void*)vmMain, (void*)dllEntry, modulePath);
		SetStatus("hooked: %s%s", moduleName, dllEntry ? "" : " (no dllEntry export)");
		return true;
	}

	// Drop an unloaded module WITHOUT detaching: the code Detours patched is gone, so a
	// DetourDetach would write the stolen bytes back into freed (possibly reused) memory. The
	// trampoline leaks - a few dozen bytes per map load.
	void Abandon()
	{
		Log("[kutaQ3] cgame module unloaded - dropping the hook (trampoline abandoned)");
		s_module       = NULL;
		s_origVmMain   = NULL;
		s_origDllEntry = NULL;
		s_syscall      = NULL;
		NameEsp::Reset();
		SetStatus("cgame unloaded - waiting for the next map load");
	}

	// Full enumeration of the loaded modules, for a cgame whose name is not one of the known ones.
	// NOT loader-lock safe (CreateToolhelp32Snapshot), so it only ever runs from Poll().
	HMODULE FindCgameModule()
	{
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		if (snapshot == INVALID_HANDLE_VALUE)
			return NULL;

		HMODULE found = NULL;
		MODULEENTRY32 entry;
		entry.dwSize = sizeof(entry);
		for (BOOL ok = Module32First(snapshot, &entry); ok; ok = Module32Next(snapshot, &entry))
		{
			if (q3::IsNativeCgameModule(entry.szModule))
			{
				found = (HMODULE)entry.modBaseAddr;
				break;
			}
		}
		CloseHandle(snapshot);
		return found;
	}
}

// =============================================================================================== //

bool CGame::Install()
{
	if (s_module)
		return ModuleResident(s_module);

	// cheap path: the names the engine and the common mods actually use. Safe under the loader lock.
	for (size_t i = 0; i < sizeof(kKnownModules) / sizeof(kKnownModules[0]); ++i)
	{
		HMODULE module = GetModuleHandleA(kKnownModules[i]);
		if (module && Attach(module))
			return true;
	}
	return false;
}

bool CGame::Poll()
{
	// Throttled to twice a second. ModuleResident() takes the loader lock and the module
	// enumeration is not free, and neither is time critical: the cgame always calls CG_SHUTDOWN
	// before it is unloaded (so stale tags are dropped in the vmMain detour), and the LoadLibrary
	// detour catches the next load the moment it happens.
	const DWORD now = timeGetTime();
	if (now - s_lastPollMs < 500)
		return s_module != NULL;
	s_lastPollMs = now;

	if (s_module)
	{
		if (ModuleResident(s_module))
			return true;
		Abandon();
	}

	if (Install())
		return true;

	HMODULE module = FindCgameModule();
	return module ? Attach(module) : false;
}

void CGame::OnModuleLoaded(const char* modulePath)
{
	if (!modulePath)
		return;
	if (!q3::IsNativeCgameModule(modulePath))
		return;

	char moduleName[MAX_PATH] = { 0 };
	ModuleBaseName(modulePath, moduleName, sizeof(moduleName));

	// GetModuleHandle takes the module's base name; the path the engine passed may be relative.
	HMODULE module = GetModuleHandleA(moduleName);
	if (!module)
	{
		Log("[kutaQ3] %s loaded but not resident by that name yet", modulePath);
		return;
	}
	Attach(module);
}

void CGame::Shutdown()
{
	if (!s_module)
		return;

	if (!ModuleResident(s_module))
	{
		// already unmapped - nothing that can safely be patched back
		s_module = NULL;
		s_origVmMain = NULL;
		s_origDllEntry = NULL;
		s_syscall = NULL;
		return;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	if (s_origVmMain)
		DetourDetach(&(PVOID&)s_origVmMain, newCgameVmMain);
	if (s_origDllEntry)
		DetourDetach(&(PVOID&)s_origDllEntry, newCgameDllEntry);
	DetourTransactionCommit();

	Log("[kutaQ3] cgame vmMain / dllEntry detached");
	s_module = NULL;
	s_origVmMain = NULL;
	s_origDllEntry = NULL;
	s_syscall = NULL;
}

q3::syscall_t CGame::Syscall()
{
	return s_syscall;
}

const char* CGame::Status()
{
	return s_status;
}
