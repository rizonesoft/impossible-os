# 14 SDK Tools

This domain covers SDK tools that run on the **host OS** (Windows, Linux) — both for Impossible OS development and for third-party developers working with Impossible OS formats.

## Belongs Here

- Filesystem tools for reading/writing IXFS, NTFS, FAT32 disk images
- Debug utilities that attach to QEMU or parse serial/crash logs
- SDK utilities that ship alongside the OS for developers
- Any standalone binary that interacts with Impossible OS formats from the host

## Does Not Belong Here

- Build-time tools (compilers, linkers, image generators) — those stay in `tools/` root
- Kernel-side utilities — those go in the appropriate kernel domain
- CI/CD scripts — those go in `00-infrastructure`

## Source Layout

```
sdk/
  ├── src/                # SDK tool source code
  │   └── ixfs-mount/     # WinFsp IXFS driver for Windows
  │       ├── ixfs-mount.c
  │       ├── Makefile
  │       └── README.md
  └── tools/              # Compiled SDK binaries (output)
      └── ixfs-mount.exe
```

SDK tools are built separately from the kernel. Each has its own Makefile/build script. They do NOT use the kernel toolchain (clang-19 cross-compiler) — they use the host's native compiler (MSVC, gcc, or clang). Binaries compile to `sdk/tools/`.

## Active TODOs

- [TODO-01 — IXFS Mount](TODO-01-ixfs-mount.md) — Mount IXFS partitions on Windows (WinFsp) and Linux (libfuse3) with USB auto-mount scripts
