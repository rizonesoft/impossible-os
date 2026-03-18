/**
 * @file windows.h
 * @brief Win32-compatible API for Impossible OS
 *
 * This is the primary SDK header. Include this to access the full
 * Win32-compatible API surface (CreateFile, ReadFile, CreateProcess, etc.)
 *
 * @note This header is a stub — functions will be added as the
 *       Win32 compatibility layer (TODO-045) is implemented.
 */

#pragma once

/* ── Freestanding type definitions ────────────────────────────────────────── */
/* SDK headers are standalone — no dependency on system headers.              */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;
typedef unsigned long long  uintptr_t;
typedef signed long long    intptr_t;
typedef unsigned long long  size_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ── Windows type definitions ─────────────────────────────────────────────── */

typedef void *          HANDLE;
typedef uint32_t        DWORD;
typedef int32_t         LONG;
typedef int             BOOL;
typedef uint16_t        WORD;
typedef uint8_t         BYTE;
typedef char *          LPSTR;
typedef const char *    LPCSTR;
typedef void *          LPVOID;
typedef const void *    LPCVOID;
typedef DWORD *         LPDWORD;

/* ── Constants ────────────────────────────────────────────────────────────── */

#define INVALID_HANDLE_VALUE    ((HANDLE)(intptr_t)-1)
#define NULL_HANDLE             ((HANDLE)0)

#define TRUE                    1
#define FALSE                   0

/* Generic access rights */
#define GENERIC_READ            0x80000000
#define GENERIC_WRITE           0x40000000
#define GENERIC_EXECUTE         0x20000000
#define GENERIC_ALL             0x10000000

/* File creation dispositions */
#define CREATE_NEW              1
#define CREATE_ALWAYS           2
#define OPEN_EXISTING           3
#define OPEN_ALWAYS             4
#define TRUNCATE_EXISTING       5

/* File attributes */
#define FILE_ATTRIBUTE_NORMAL   0x00000080
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN   0x00000002
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010

/* ── API stubs (to be implemented in TODO-045) ────────────────────────────── */

// File I/O — TODO-045 §1.x
// HANDLE CreateFile(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
//                   void *lpSecurityAttributes, DWORD dwCreationDisposition,
//                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
// BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
//               LPDWORD lpNumberOfBytesRead, void *lpOverlapped);
// BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
//                LPDWORD lpNumberOfBytesWritten, void *lpOverlapped);
// BOOL CloseHandle(HANDLE hObject);
