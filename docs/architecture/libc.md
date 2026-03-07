# User-Mode C Library (libc)

Impossible OS provides a minimal C standard library for user-mode programs.
It wraps syscalls into familiar C functions and provides essential string,
math, and I/O utilities.

## Architecture

```
User program (main.c)
        │
        ▼
  ┌──────────┐
  │  libc.a  │  printf, malloc, strlen, atoi, ...
  └────┬─────┘
       │  wraps
       ▼
  ┌──────────┐
  │ syscalls │  INT 0x80 → kernel
  └──────────┘
```

Programs are linked: `crt0.o` + `program.o` + `libc.a` → `program.exe`

## Key Files

| File | Module | Functions |
|------|--------|-----------|
| `user/lib/crt0.asm` | C Runtime | `_start` → calls `main()` → `sys_exit()` |
| `user/lib/stdio.c` | Standard I/O | `printf`, `puts`, `putchar`, `getchar` |
| `user/lib/string.c` | Strings | `memcpy`, `memset`, `strlen`, `strcmp`, `strcpy`, `strncpy`, `strcat`, `strchr`, `memcmp` |
| `user/lib/stdlib.c` | Utilities | `atoi`, `itoa`, `malloc` (bump allocator) |
| `user/lib/ctype.c` | Character | `isalpha`, `isdigit`, `isspace`, `toupper`, `tolower` |
| `user/lib/math.c` | Integer Math | `iabs`, `imin`, `imax`, `ipow`, `isqrt` |
| `user/include/syscall.h` | Syscall ABI | `sys_write`, `sys_read`, `sys_exit`, `sys_fork`, ... |

## C Runtime Entry (`crt0.asm`)

```nasm
global _start
extern main

_start:
    call main       ; Call the program's main()
    mov  rdi, rax   ; Exit code = return value of main
    mov  rax, 2     ; SYS_EXIT
    int  0x80       ; Syscall
    hlt             ; Should never reach here
```

Every user program starts at `_start`, which calls `main()` and then
issues `SYS_EXIT` with the return value.

## Standard I/O (`stdio.c`)

| Function | Description |
|----------|-------------|
| `printf(fmt, ...)` | Formatted output to stdout (via `SYS_WRITE`) |
| `puts(str)` | Print string + newline |
| `putchar(c)` | Print single character |
| `getchar()` | Blocking read of one character (via `SYS_READ`) |

`printf` supports: `%d`, `%u`, `%x`, `%s`, `%c`, `%p`, `%%`.

## Memory Allocator (`stdlib.c`)

| Property | Value |
|----------|-------|
| Type | **Bump allocator** (no free) |
| Heap size | 64 KiB |
| Alignment | 8 bytes |

This is intentionally simple — user programs in Impossible OS are short-lived
and don't need a full `malloc`/`free` implementation. The entire 64 KiB heap
is reclaimed when the process exits.

## Build Process

```makefile
# 1. Assemble CRT0
nasm -f elf64 user/lib/crt0.asm -o build/user/crt0.o

# 2. Compile libc sources
x86_64-elf-gcc -ffreestanding -nostdlib -c user/lib/*.c

# 3. Archive into static library
x86_64-elf-ar rcs build/user/libc.a *.o

# 4. Link a user program
x86_64-elf-ld -o shell.exe crt0.o shell.o -L. -lc
```

## Syscall Interface (`syscall.h`)

User programs invoke syscalls via inline assembly:

```c
static inline int64_t syscall1(uint64_t nr, uint64_t arg1) {
    int64_t ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"(nr), "D"(arg1));
    return ret;
}
```

| Register | Purpose |
|----------|---------|
| `RAX` | Syscall number |
| `RDI` | Argument 1 |
| `RSI` | Argument 2 |
| `RDX` | Argument 3 |
| `RAX` (return) | Return value |
