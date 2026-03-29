# 14 Host Tools

This domain covers development tools that run on the **host OS** (Windows, Linux) to support Impossible OS development. These are NOT part of the kernel or OS — they run alongside it.

## Belongs Here

- Filesystem drivers for reading/writing Impossible OS disk images from the host
- Debug utilities that attach to QEMU or parse serial logs
- Asset pipeline tools that run during development (not build)
- Any tool that makes the dev workflow faster without modifying the OS itself

## Does Not Belong Here

- Build-time tools (compilers, linkers, image generators) — those stay in `tools/` root
- Kernel-side utilities — those go in the appropriate kernel domain
- CI/CD scripts — those go in `00-infrastructure`

## Source Layout

```
tools/
  ├── *.c, *.py           # Build-time tools (existing, unchanged)
  └── host/               # Host-side development tools
      └── ixfs-mount/     # WinFsp IXFS driver for Windows
          ├── ixfs-mount.c
          ├── Makefile
          └── README.md
```

Host tools are built separately from the kernel. Each has its own Makefile/build script. They do NOT use the kernel toolchain (clang-19 cross-compiler) — they use the host's native compiler (MSVC, gcc, or clang).

## Active TODOs

- [TODO-01 — IXFS Mount for Windows](TODO-01-ixfs-mount-windows.md) — WinFsp user-mode filesystem driver to mount IXFS partitions as drive letters in Windows Explorer
