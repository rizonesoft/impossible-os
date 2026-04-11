---
name: userland-code-quality
description: Pre-flight quality checklist for user-mode applications (user/, src/apps/). Placeholder -- extend with user-mode-specific gates when application development is active.
---

# Userland Code Quality

> This skill auto-loads when writing user-mode application code. Extend with domain-specific gates when application development is active.

## When This Applies

Every time you create or modify a `.c`, `.h`, or `.asm` file under `user/` or `src/apps/`.

## Pre-Write Checklist

### Gate 1: User-Mode Rules
- [ ] **Linked against user libc.** Use `user/user.ld` linker script. Code runs at user ELF range (0x800000-0x900000).
- [ ] **No kernel headers.** User code includes user-space headers only. Never `#include "kernel/..."`.
- [ ] **Syscall interface only.** Use `syscall()` or Win32 API wrappers to access kernel services. No direct port I/O, no MSR access, no physical memory access.

### Gate 2: Win32 API Conventions
- [ ] **Win32 naming.** Functions follow Win32 naming conventions (CreateFile, ReadFile, WriteFile).
- [ ] **HANDLE-based I/O.** File operations return/accept HANDLEs, not file descriptors.
- [ ] **Unicode strings.** Win32 API uses UTF-16LE (`WCHAR *`). Narrow API (`CreateFileA`) converts internally.

### Gate 3: Error Handling
- [ ] **Check every syscall return.** `NtCreateFile` returns NTSTATUS. `CreateFile` returns INVALID_HANDLE_VALUE on failure.
- [ ] **No silent crashes.** User processes that hit unrecoverable errors should call `ExitProcess()` with a meaningful exit code.

> **Extend this skill** with gates for PE loading, DLL imports, structured exception handling, heap management, and GUI APIs when those features are implemented.
