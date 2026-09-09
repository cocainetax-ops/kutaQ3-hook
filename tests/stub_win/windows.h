#pragma once

// =============================================================================================== //
// kutaQ3 hook tests - a stand-in for <Windows.h>, for the vmHook.cpp syntax check only
//
// vmHook.cpp is Win32-only: it walks the executable's PE headers, asks VirtualQuery for the
// writable regions and patches the cgame VM's syscall dispatcher with Detours. None of that can run
// on a Linux host, but it CAN be compiled, and compiling it is what catches a typo, a wrong member
// name or a type mismatch before the DLL is built in Visual Studio.
//
// So this file declares exactly the surface vmHook.cpp touches - no more. It is a type-checking
// shim, NOT a reimplementation: the functions have no bodies, the structs are the shape the real
// ones are, and nothing here validates that the real Win32 API behaves the way this hook assumes.
// tests/stub/windows.h is the other, unrelated shim (the one the GL tests run against).
// =============================================================================================== //

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- basic types ------------------------------------------------------------------------------
typedef int            BOOL;
typedef unsigned char  BYTE;
typedef unsigned char* PBYTE;
typedef unsigned short WORD;
typedef unsigned short USHORT;
typedef unsigned long  DWORD;
typedef unsigned long  ULONG;
typedef long           LONG;
typedef char           CHAR;
typedef const char*    PCSTR;
typedef void           VOID;
typedef unsigned long  DWORD_PTR;
typedef void*          HANDLE;
typedef void*          HMODULE;
typedef void*          PVOID;
typedef void*          LPVOID;
typedef const void*    LPCVOID;
typedef char*          PCHAR;
typedef void*          HINSTANCE;
typedef void*          HWND;
typedef void*          HDC;
typedef void*          LPSECURITY_ATTRIBUTES;
typedef void*          LPSTARTUPINFOA;
typedef void*          LPPROCESS_INFORMATION;
typedef void*          LPSTARTUPINFOW;
typedef void*          HMENU;
typedef void*          HFONT;
typedef void*          HGDIOBJ;
typedef int            INT;
typedef wchar_t*       LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef size_t         SIZE_T;
typedef char*          LPSTR;
typedef const char*    LPCSTR;

#define WINAPI
#define CALLBACK
#define WINAPIV
#define APIENTRY
#define _In_
#define _In_opt_
#define _Reserved_
#define __in_z
#define MAX_PATH 260
#define NO_ERROR 0L
#define TRUE  1
#define FALSE 0

// ---- PE headers: ImageRange() reads these ------------------------------------------------------
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE  0x00004550

typedef struct _IMAGE_DOS_HEADER
{
	WORD e_magic;
	BYTE pad[58];
	LONG e_lfanew;
} IMAGE_DOS_HEADER;

typedef struct _IMAGE_FILE_HEADER
{
	WORD Machine;
	WORD NumberOfSections;
	DWORD TimeDateStamp;
	DWORD PointerToSymbolTable;
	DWORD NumberOfSymbols;
	WORD  SizeOfOptionalHeader;
	WORD  Characteristics;
} IMAGE_FILE_HEADER;

typedef struct _IMAGE_DATA_DIRECTORY
{
	DWORD VirtualAddress;
	DWORD Size;
} IMAGE_DATA_DIRECTORY;

#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16

typedef struct _IMAGE_OPTIONAL_HEADER
{
	WORD  Magic;
	DWORD AddressOfEntryPoint;
	DWORD BaseOfCode;
	DWORD BaseOfData;
	DWORD ImageBase;
	DWORD SizeOfImage;
} IMAGE_OPTIONAL_HEADER;

typedef struct _IMAGE_NT_HEADERS
{
	DWORD                 Signature;
	IMAGE_FILE_HEADER     FileHeader;
	IMAGE_OPTIONAL_HEADER OptionalHeader;
} IMAGE_NT_HEADERS, IMAGE_NT_HEADERS32;

typedef struct _IMAGE_OPTIONAL_HEADER64
{
	WORD  Magic;
	DWORD AddressOfEntryPoint;
	DWORD BaseOfCode;
	DWORD ImageBase;
	DWORD SizeOfImage;
} IMAGE_OPTIONAL_HEADER64;

typedef struct _IMAGE_NT_HEADERS64
{
	DWORD                   Signature;
	IMAGE_FILE_HEADER       FileHeader;
	IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64;

#define IMAGE_SIZEOF_SHORT_NAME 8

typedef struct _IMAGE_SECTION_HEADER
{
	BYTE  Name[IMAGE_SIZEOF_SHORT_NAME];
	DWORD VirtualSize;
	DWORD VirtualAddress;
	DWORD SizeOfRawData;
	DWORD PointerToRawData;
	DWORD Characteristics;
} IMAGE_SECTION_HEADER;

// ---- memory walking: ForEachWritableRegion() ---------------------------------------------------
#define MEM_COMMIT   0x1000
#define MEM_RESERVE  0x2000

#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_WRITECOPY         0x08
#define PAGE_EXECUTE           0x10
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD             0x100

typedef struct _MEMORY_BASIC_INFORMATION
{
	void*  BaseAddress;
	void*  AllocationBase;
	DWORD  AllocationProtect;
	SIZE_T RegionSize;
	DWORD  State;
	DWORD  Protect;
	DWORD  Type;
} MEMORY_BASIC_INFORMATION;

SIZE_T VirtualQuery(LPCVOID address, MEMORY_BASIC_INFORMATION* info, SIZE_T length);

HMODULE GetModuleHandleA(LPCSTR moduleName);
#define GetModuleHandle GetModuleHandleA

HANDLE GetCurrentThread(void);
DWORD  timeGetTime(void);

// ---- the *_s CRT helpers the hook uses, array sized the way MSVC sizes them --------------------
#define _TRUNCATE ((size_t)-1)

template <size_t N>
inline int strncpy_s(char (&dest)[N], const char* src, size_t count)
{
	const size_t n = (count == _TRUNCATE || count >= N) ? N - 1 : count;
	size_t i = 0;
	for (; i < n && src[i]; ++i)
		dest[i] = src[i];
	dest[i] = 0;
	return 0;
}

template <size_t N>
inline int vsprintf_s(char (&dest)[N], const char* fmt, va_list ap)
{
	return vsnprintf(dest, N, fmt, ap);
}
