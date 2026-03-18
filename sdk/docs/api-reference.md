# SDK API Reference

## Overview

The Impossible OS SDK provides a Win32-compatible API for building native applications.
Applications are PE32+ executables that use familiar Windows-style functions.

## Headers

| Header | Description |
|--------|-------------|
| `<impossible/windows.h>` | Main Win32 API (file I/O, processes, memory, threads) |

## Type Reference

| Type | Definition | Usage |
|------|-----------|-------|
| `HANDLE` | `void *` | Opaque handle to kernel objects |
| `DWORD` | `uint32_t` | 32-bit unsigned integer |
| `BOOL` | `int` | Boolean (TRUE=1, FALSE=0) |
| `LPCSTR` | `const char *` | Read-only string pointer |
| `LPVOID` | `void *` | Generic pointer |

## Functions

> Functions will be documented here as they are implemented in the kernel.
> See `TODO-045-Win32-VFS-Compat.md` for the implementation roadmap.
