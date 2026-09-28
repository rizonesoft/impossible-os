<!-- docs: covers=todo/02-kernel-core/TODO-27-crash-dump-generation.md sources=src/kernel/panic.c,include/kernel/panic.h,include/kernel/bugcheck.h,include/kernel/crashdump.h,include/kernel/except.h,src/kernel/exec.c,include/kernel/exec.h,src/kernel/test/test_crashdump.c reviewed=2026-09-28 order=27 -->
# Crash Dump Generation

## What is it?

The kernel's Windows-compatible crash reporting stack: a STOP-code bugcheck entry point, capture of the crashing thread's CPU and FPU state into a Windows `CONTEXT` record, a loaded-module registry, and the WinDbg-compatible MDMP structure layout a binary dump file will use. What reaches disk today is still the text `crashdump.log`: the structures and capture code a binary `.dmp` writer will consume exist and are unit-tested, but the writer, the raw-partition sink and the post-boot recovery path do not exist yet.

## How does it work?

`KeBugCheckEx(code, p1, p2, p3, p4)` is the single entry point a kernel bugcheck flows through ([`panic.c`](../../src/kernel/panic.c)): it stores the STOP code and its four parameters in a global bugcheck record, resolves a readable name with `bugcheck_name()`, persists the code and parameters to `HKLM\SYSTEM\LastBugCheck` when the Registry is ready, and routes to the panic screen. Faults and NMIs enter through `KeBugCheckExFrame()` instead, which carries the trap frame and deliberately skips the Registry write, so `LastBugCheck` records only software-initiated bugchecks and can be older than the latest crash. The Windows-compatible STOP codes and the Impossible OS exclusive codes are defined in [`bugcheck.h`](../../include/kernel/bugcheck.h). A crash can also be triggered on purpose: an NMI on vector 2 bugchecks through `KeBugCheckExFrame()`, and `Ctrl+ScrollLock` pressed twice within two seconds (gated by `HKLM\SYSTEM\CrashControl\CrashOnCtrlScroll`) calls `KeBugCheckEx()`.

Once the panic path runs, `panic_capture_fpu_state()` clears `CR0.TS`, checks `CR4.OSXSAVE`, and captures the FPU, XMM and YMM state with `xsave64` (falling back to `fxsave64`) into a static aligned scratch buffer, because `kmalloc` cannot be trusted after a panic. `panic_build_context()` then fills a full Windows x64 `CONTEXT` record (general-purpose, segment and debug registers, the 512-byte `FltSave` area, AVX high halves) from the interrupt frame and that captured state. The `CONTEXT` type is defined once in [`except.h`](../../include/kernel/except.h) and shared with structured exception dispatch, so both consumers stay binary-identical.

Every loaded binary (the kernel image plus every PE and ELF module) registers into one `loaded_module_t` list through `exec_register_module()`, which a dump's module stream will enumerate ([`exec.h`](../../include/kernel/exec.h)). `exec_find_module_by_pc()` resolves a faulting address to the module that owns it, with one known gap: when a process re-execs at an address a registered module already covers, `task_exec` skips re-registration, so the lookup can name the previous binary. `exec_iterate_modules()` copies a locked snapshot of the list; `exec_iterate_modules_lockless()` copies it without the lock and is only correct once every other CPU has been frozen, because a concurrent `exec_register_module()` can leave torn entries. On top of this, [`crashdump.h`](../../include/kernel/crashdump.h) defines the MDMP binary format: the `MINIDUMP_HEADER`, its stream directory, the standard stream types (exception, module list, thread list, system info) at the Windows SDK layout, and a vendor-range `ImpossibleOSInfoStream` (`0x8001`) carrying the bugcheck code, kernel build and CPU identity. The structures are complete and size-asserted; nothing yet serializes a running system into them.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `KeBugCheckEx()`, `KeBugCheckExFrame()`, `bugcheck_name()` | Software and fault/NMI bugcheck entry points and the STOP-code taxonomy ([`bugcheck.h`](../../include/kernel/bugcheck.h)) |
| `panic_capture_fpu_state()`, `panic_build_context()` | Allocation-free FPU capture and `CONTEXT` record population ([`panic.h`](../../include/kernel/panic.h)) |
| `CONTEXT`, `EXCEPTION_RECORD` | The shared Windows x64 CPU-state layout, also used by exception dispatch ([`except.h`](../../include/kernel/except.h)) |
| `exec_register_module()`, `exec_find_module_by_pc()` | Loaded-module registration and address-to-module resolution ([`exec.h`](../../include/kernel/exec.h)) |
| `exec_iterate_modules()`, `exec_iterate_modules_lockless()` | Locked snapshot, and a lockless copy valid only with all other CPUs frozen |
| `MINIDUMP_HEADER`, `MINIDUMP_DIRECTORY`, `ImpossibleOSInfoStream` | The WinDbg-compatible MDMP format structures ([`crashdump.h`](../../include/kernel/crashdump.h)) |

## How do I use it?

There is no operator-facing command yet: a panic still produces the text `C:\Impossible\System\crashdump.log`, not a binary `.dmp` file. The capture and format code is exercised by its unit tests in [`test_crashdump.c`](../../src/kernel/test/test_crashdump.c).

```bash
bash scripts/test.sh SUITE=boot   # bugcheck names, FPU/CONTEXT, module registry, MDMP structs
make test-boot                    # shorthand for the same suite
```

## What is not implemented yet?

- **Sampling `CR0.TS` before the FPU capture.** `panic_capture_fpu_state()` clears `CR0.TS` without first recording it, so a faulting task whose FPU state was lazily deferred can have another task's register contents captured into its `CONTEXT` ([FPU/XMM/XSAVE State Capture](../../todo/02-kernel-core/TODO-27-crash-dump-generation.md#2-fpuxmmxsave-state-capture)).
- **Replacing a module entry on re-exec.** A re-exec at a covered address keeps the old name and range ([Module Registration for EIF](../../todo/02-kernel-core/TODO-20-eif-full-implementation.md#3-module-registration-for-eif)).
- **The minidump writer.** Nothing yet serializes the captured `CONTEXT`, thread list, module list and referenced pages into an MDMP image ([Minidump Writer](../../todo/02-kernel-core/TODO-27-crash-dump-generation.md#5-minidump-writer)).
- **A raw-partition dump sink.** There is no VFS-bypass block path to write a dump from panic context, so even a finished writer would have nowhere durable to put it ([Raw-Partition Dump Sink (VFS Bypass)](../../todo/02-kernel-core/TODO-27-crash-dump-generation.md#7-raw-partition-dump-sink-vfs-bypass)).
- **`dmpanalyze.exe`.** No tool parses a `.dmp` file into an `!analyze -v` style report or compares two dumps ([`dmpanalyze.exe` Crash Analyzer](../../todo/02-kernel-core/TODO-27-crash-dump-generation.md#9-dmpanalyzeexe-crash-analyzer)).

## How does it compare with Windows 11 and Linux?

The shipped pieces (STOP codes, a Windows `CONTEXT` with FPU state, a loaded-module list, SDK-exact MDMP layouts) follow the Windows model directly, while Linux's oops format and `kdump` ELF cores are a different design with no WinDbg-compatible output. The gap is entirely on the output side: Windows writes minidump, kernel or full dumps through a pagefile-backed sink and reports them through WER on the next boot, and Linux `kdump`/`makedumpfile` does the equivalent to an ELF core; Impossible OS captures the state but has no writer, dump sink or recovery UI yet. The one planned feature neither ships is on-device crash comparison through `dmpanalyze.exe`.

## See also

- [Crash Dump Generation roadmap](../../todo/02-kernel-core/TODO-27-crash-dump-generation.md)
- [Panic Screen and Crash Experience](panic-screen-crash-experience.md)
- [Exception Dispatch and SEH](exception-dispatch-seh.md)
- [System Logging (klog)](system-logging.md)
- [Kernel Resource Accounting and Quotas](kernel-resource-accounting-quotas.md)
