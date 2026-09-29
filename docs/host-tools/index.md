# Host Tools

Programs that run on the developer's Linux machine, not on Impossible OS, and work with its formats: disk images, IXFS partitions, serial logs, crash output and the BlackBox log partition. They build with the host's own compiler through `bash sdk/build.sh`, separately from the kernel. Only the build script and the IXFS mount tool exist as SDK tools; most of the planned tools already have a working stand-in elsewhere in the repository, which each page names.

## Built

| Document | Topics |
| --- | --- |
| [SDK Build System](sdk-build-system.md) | `sdk/build.sh`: compiler and dependency detection, tool discovery, adding a tool |
| [IXFS Mount (Linux)](ixfs-mount.md) | FUSE mount of an IXFS partition from an image or USB drive; currently behind the kernel's on-disk format |

## Planned

One page per roadmap file, each following the [page contract](../contributing/docs-page-contract.md) and naming what works today.

| Document | Topics |
| --- | --- |
| [Address Resolver (ixfs-addr2line)](addr2line.md) | Address to function, line and source; today `llvm-addr2line-19` and `build/kernel.map` |
| [Crash Decoder (crash-decode)](crash-decode.md) | One-command crash analysis from the serial `[PANIC]` block or the crash screen |
| [Boot Log Analyzer (serial-analyze)](serial-analyze.md) | Boot timing, warnings and two-log comparison; today `boot_timeline.py` and the smoke log |
| [Disk Image Inspector (disk-inspect)](disk-inspect.md) | GPT, IXFS metadata and raw sectors; today `bootimg.py inspect` |
| [IXFS Consistency Checker (ixfs-fsck)](ixfs-fsck.md) | Host-side check and repair; the kernel's `ixfs_fsck()` already exists |
| [BlackBox Log Extractor](blackbox-extractor.md) | Logs, events and boot records off the BlackBox partition; today `read-blackbox.sh` |
