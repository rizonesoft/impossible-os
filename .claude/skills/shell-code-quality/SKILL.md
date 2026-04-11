---
name: shell-code-quality
description: Pre-flight quality checklist for command shell code (src/shell/). Placeholder -- extend with shell-specific gates when cmd.exe development is active.
---

# Shell Code Quality

> This skill auto-loads when writing shell code. Extend with domain-specific gates when shell development is active.

## When This Applies

Every time you create or modify a `.c`, `.h`, or `.asm` file under `src/shell/`.

## Pre-Write Checklist

### Gate 1: Freestanding Rules
- [ ] Same as `kernel-code-quality` Gate 1 -- no stdlib, `kernel/types.h`, ASCII only.

### Gate 2: Win32 API Surface
- [ ] **Console API.** Shell uses Win32 console API (ReadConsole, WriteConsole, SetConsoleTitle). Implement via SSDT syscalls to kernel handlers.
- [ ] **Command parsing.** Use Windows cmd.exe conventions: `&`, `&&`, `||`, `|`, `>`, `>>`, `<`. No POSIX shell syntax in the native shell.
- [ ] **Path handling.** Windows-style paths (`C:\`, `\\`, backslash separator). `NtCreateFile` for file I/O.

### Gate 3: SMP + Error Handling
- [ ] Same as `kernel-code-quality` Gates 2 + 9 -- SMP safety and complete error handling.

> **Extend this skill** with gates for command history, tab completion, environment variables, batch file execution, and piping when those features are implemented.
