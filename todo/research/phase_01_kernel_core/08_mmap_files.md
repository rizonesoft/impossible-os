# 80 — Memory-Mapped Files

## Overview
Map a file's contents directly into a process's address space. Reading/writing memory = reading/writing the file. Used for fast file I/O, shared memory, and loading executables.

## API (POSIX-like)
```c
void *mmap(void *addr, uint32_t length, int prot, int flags, int fd, uint64_t offset);
int   munmap(void *addr, uint32_t length);
int   msync(void *addr, uint32_t length); /* flush to disk */
```

## Use cases
| Use case | How |
|----------|-----|
| Fast file reading | Map file, read as pointer |
| ELF/PE loading | Map sections directly into memory |
| Shared memory IPC | Both processes map same file |
| Database files | Direct memory access to data |

## Win32 mapping: `CreateFileMapping()` + `MapViewOfFile()` → `mmap()`
## Depends on: VMM (`src/kernel/mm/vmm.c`), VFS

## Files: `src/kernel/mm/mmap.c` (~200 lines) | 1 week
