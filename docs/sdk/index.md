# SDK and User Platform

How programs are built for and run on Impossible OS: the libraries compiled into the system, the environment and command-line contract, loadable modules, the user-mode runtime (`ntdll`) and the Win32 subsystem server, the SDK package, and the ladder that will measure Win32 compatibility. Most of this roadmap domain has not started. Where the kernel already ships the substrate (the TEB and PEB, the environment API, the PE import tables, the random generator), each page says so plainly and links the roadmap section that owns each gap.

## Roadmap Overviews

One page per SDK roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [Embedded Libraries for Developers](embedded-libraries.md) | Shipped strings, maths, CSPRNG and cJSON; planned ZIP writer, TLS client, host library tests |
| [Environment Variables and Process ABI for Developers](process-environment-abi.md) | Shipped kernel environment API, `Nt` calls and `cmd_tokenize()`; planned path templates, `.profile`, `set`/`env`/`where` |
| [ELF Relocations and Kernel Modules](elf-relocations-modules.md) | The static ELF loader and reserved module type; planned relocation engine, `.kmod` loader, `dlopen()` |
| [NTDLL and the User-Mode Runtime](ntdll-user-runtime.md) | Shipped TEB/PEB layouts, TLS slots and PEB hand-off; planned heap, DLL loader, VEH, fibers |
| [Win32 Subsystem Server](win32-subsystem.md) | Native windows and controls it will wrap; planned message queues, classes, `DefWindowProc()`, painting |
| [SDK Distribution and Developer Experience](sdk-distribution.md) | The `sdk/` folder and host tools build; planned `make sdk`, `gendoc`, samples, profiler, release pipeline |
| [Win32 Compatibility Matrix and Bring-Up Ladder](win32-compat-matrix.md) | The PE export tables; planned coverage tracker, nine test tiers, stub counters, CI gate |
