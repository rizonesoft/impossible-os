# 14 Host Tools

This domain covers tools that run on the **host OS** (Windows, Linux) -- both for Impossible OS development and for third-party developers working with Impossible OS formats.

## Belongs Here

- Filesystem tools for reading/writing IXFS, NTFS, FAT32 disk images
- Debug utilities that attach to QEMU or parse serial/crash logs
- Host utilities that ship alongside the OS for developers
- Any standalone binary that interacts with Impossible OS formats from the host

## Does Not Belong Here

- Build-time tools (compilers, linkers, image generators) -- they also live in `tools/`, but the kernel build owns them
- Kernel-side utilities -- those go in the appropriate kernel domain
- CI/CD scripts -- those go in `00-infrastructure`

## Source Layout

```
tools/
  ├── build-host.sh       # builds every tools/*/ that has a Makefile
  └── ixfs-mount/         # Linux libfuse3 IXFS mount (TODO-02)
      ├── ixfs-core.c, ixfs-disk.c, ixfs-fuse-linux.c
      ├── ixfs-structs.h, test_ixfs_core.c
      ├── mount-ixfs-usb.sh, extract-logs.sh
      └── Makefile
build/host-tools/         # compiled binaries (output, gitignored)
  └── ixfs-mount
```

Host tools are built separately from the kernel. Each has its own Makefile. They do NOT use the kernel toolchain (clang-19 cross-compiler) -- they use the host's native compiler (gcc, falling back to clang). Binaries compile to `build/host-tools/`. Only Linux builds exist today; no Windows (WinFsp or MinGW) build has been written. User-facing pages: `docs/host-tools/`.

## Active TODOs

- [TODO-01 -- Host Tools Build](TODO-01-host-tools-build.md) -- Build scripts for host tools with progress bars, dependency detection, auto-discovery
- [TODO-02 -- IXFS Mount](TODO-02-ixfs-mount.md) -- Mount IXFS partitions on Linux (libfuse3) with a USB auto-mount script; §6 resyncs it with the IXFS v2 on-disk format
- [TODO-03 -- ixfs-addr2line](TODO-03-addr2line.md) -- Enhanced address resolver with source context, error decode, and memory region mapping
- [TODO-04 -- crash-decode](TODO-04-crash-decode.md) -- Post-mortem crash analyzer: paste BSOD dump, get full analysis with root cause hypothesis
- [TODO-05 -- serial-analyze](TODO-05-serial-analyze.md) -- Boot log analyzer: timing breakdown, warning highlight, boot comparison, HTML reports
- [TODO-06 -- disk-inspect](TODO-06-disk-inspect.md) -- Interactive disk image browser: GPT, IXFS superblock, inodes, hex dump, directory tree
- [TODO-07 -- ixfs-fsck](TODO-07-ixfs-fsck.md) -- IXFS filesystem consistency checker with optional repair mode
- [TODO-08 -- BlackBox Log Extractor](TODO-08-blackbox-log-extractor.md) -- Host-side CLI (Linux + Windows) to extract, view, and filter logs from BlackBox partition in disk images
