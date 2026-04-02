# 02 Kernel Core

This domain holds kernel work that is foundational but is not primarily memory management, concurrency, drivers, or filesystems.

## Belongs Here

- Kernel entry sequencing, panic handling, logging, versioning, and core runtime policies.
- Binary loading, executable format integration, registry, and process model foundations.
- Cross-cutting kernel behavior that does not belong to a more specialized subsystem.

## Does Not Belong Here

- PMM, VMM, heap, scheduler, IPC, or SMP work. Put that in [03 Memory Concurrency](../03-memory-concurrency/INDEX.md).
- Driver or filesystem implementation. Put that in [04 Drivers Hardware](../04-drivers-hardware/INDEX.md) or [05 Storage Filesystems](../05-storage-filesystems/INDEX.md).

## Likely Source Areas

- [src/kernel](../src/kernel/)
- [src/kernel/main](../src/kernel/main/)
- [include/kernel](../include/kernel/)

## Epics

- None yet.

## Active TODOs

- [TODO-04 PEB / TEB & User-Mode ABI](./TODO-04-peb-teb-user-abi.md) — PEB, TEB, swapgs, KERNEL_GS_BASE, RTL_USER_PROCESS_PARAMETERS, Ldr module list, TLS slots, and initial stack frame. Greenfield — task_exec currently enters ring 3 with all-zero registers and no PEB/TEB.
- [TODO-05 Native API Layer (Nt/Zw)](./TODO-05-native-api-layer.md) — NTSTATUS, SYSCALL/SYSRET fast path, SSDT, Nt/Zw naming, file/process/sync/memory syscalls, IOSB, LastError, ZwXxx CPL alias layer. Greenfield — current syscall table is POSIX-style INT 0x80 with no NTSTATUS.
- [TODO-06 IRQL Model & DPCs](./TODO-06-irql-model-dpcs.md) — Windows-style IRQL contract, per-CPU IRQL tracking, interrupt-level transition rules, and `DISPATCH_LEVEL` DPC queueing so ISR top-halves can defer work safely.
- [TODO-07 Time & FILETIME Management](./TODO-07-time-filetime-management.md) — `FILETIME` type, invariant TSC/HPET monotonic clock, wall clock seeded from UEFI GetTime, `NtQuerySystemTime`, `QueryPerformanceCounter`, timezone/DST, and FAT32/NTFS timestamp encoding.
- [TODO-08 Binary Format System](./TODO-08-binary-system.md) — `exec_load()` multi-format dispatcher, enhanced ELF loader, EIF native format (spec + kernel loader + elf2eif tool), PE32+ loader with Win32 import resolution, ELF dynamic linker, ASLR, and EIF code signing.
- [TODO-09 Process Model Extensions](./TODO-09-process-model-extensions.md) — per-process `cwd`, standard handle pre-wiring, user-mode program break (brk/sbrk Linux compat), process priority classes, per-task scheduling policy (SCHED_FIFO/IDLE), and process capability bitmask with drop-only inheritance.
- [TODO-10 Exception Dispatch & SEH](./TODO-10-exception-dispatch-seh.md) — EXCEPTION_RECORD/CONTEXT types, #PF user/kernel triage, general fault-to-exception mapping, KiUserExceptionDispatcher ring-3 delivery, x64 table-based unwind (RtlVirtualUnwind), SEH chain walk, RtlUnwindEx, VEH, unhandled exception filter, kernel safe probing, kernel-mode `__try`/`__except` for drivers, and POSIX signal delivery for Linux compat.
- [TODO-11 Security Reference Monitor](./TODO-11-security-reference-monitor.md) — ACCESS_TOKEN objects, SID & LUID primitives, SECURITY_DESCRIPTOR/ACL/ACE types, SeAccessCheck engine, Mandatory Integrity Control (MIC), process/thread token assignment, SePrivilegeCheck, UAC filtered-token split & NtFilterToken, and full Win32 security API surface.
- [TODO-12 ALPC / Message Ports](./TODO-12-alpc-message-ports.md) — Connection-oriented ALPC_PORT kernel objects, three-way connect/accept handshake, synchronous send+wait+reply engine, async completion-list delivery, large-data port sections, client security token capture & impersonation, full NtAlpc* syscall surface, and CSRSS ApiPort bootstrap.
- [TODO-13 Registry System Completion](./TODO-13-registry-completion.md) — Access rights enforcement, advanced key ops (copy/rename/save/volatile), change notifications with coalescing and detail payloads, Nt/Zw syscalls with pointer validation, advapi32.dll A/W shims, HKCR merged view, registry virtualization, .reg import/export, regedit shell tool, dual-log WAJ, incremental delta flush, hive compaction, atomic transactions, search API, snapshot/diff, and per-PID quota.
- [TODO-14 Environment Variables & Process Arguments](./TODO-14-environment-variables.md) — Per-process environ storage (`env_get/set/unset/expand`), system defaults from Registry, `%VAR%` expansion, argv array preparation, `NtQueryEnvironmentVariable`/`NtSetEnvironmentVariable` syscalls, Win32 `GetEnvironmentVariable`/`ExpandEnvironmentStrings`/`GetCommandLine` wrappers, PATH-based command lookup, `SET`/`ECHO` shell commands, `.profile` startup script, and `WM_SETTINGCHANGE` env-change broadcast.
- [TODO-15 Power Management (S1–S4 & Device D-States)](./TODO-15-power-management.md) — ACPI sleep object parsing (S1/S3/S4), CPU halt idle integration, S3 suspend-to-RAM with CPU state save/restore and AP re-init, S4 hibernation image write/resume with LZ4 compression, ACPI Embedded Controller (EC) driver, battery `_BIF`/`_BST` and AC adapter, power button/lid events, PCI D-states (D0–D3cold), driver sleep/wake callbacks with priority ordering, connected standby (S0ix/Modern Standby), and power plan UI (`powercfg` + Power Options).
- [TODO-16 Crash Dump Generation](./TODO-16-crash-dump-generation.md) — Windows STOP code taxonomy (`KeBugCheckEx`), FPU/XMM/XSAVE state capture, kernel module registry (`LOADED_MODULE`), WinDbg-compatible MDMP binary format (header, directory, all stream types, custom `ImpossibleOSInfoStream`), minidump/kernel/full dump writers with LZ4, VFS-bypass raw-partition dump sink, post-boot dump recovery and CrashDumps archive, and built-in `dmpanalyze.exe` crash analyzer with `/compare` deduplication.
- [TODO-17 Kernel Security Hardening](./TODO-17-kernel-security-hardening.md) — NX/XD bit (EFER.NXE + PTE NX on all non-code mappings), SMEP/SMAP (CR4 activation + CLAC/STAC wrappers), KPTI dual page tables with CR3 swap at ring transitions, PCID TLB tagging (no-flush CR3 writes), Spectre mitigations (IBRS/IBPB + retpoline build flag), CET shadow stack (per-thread SSP), CET IBT (ENDBR64 enforcement), kernel heap cookies + redzones, stack canaries (`-fstack-protector-strong` + RDRAND), kernel stack guard pages, and KASLR.
- [TODO-18 Kernel Debugger (KD Protocol)](./TODO-18-kernel-debugger-kd-protocol.md) — Serial port upgrade to 115200 baud with IRQ-driven RX, KD packet framing (leaders/checksum/ACK/RESEND), WinDbg connection handshake, `#DB`/`#BP` routing with AP freeze/thaw, CONTEXT get/set, memory read/write (virtual + physical + search), software breakpoint table (64 slots), hardware breakpoints (DR0–DR3/DR7), single-step (RFLAGS.TF), `DbgKdGetVersionApi` + module list, I/O port + MSR access, and `kd_break()`/F12 keyboard breakin.
- [TODO-19 x86-64 Architecture Enhancements](./TODO-19-x86-64-architecture.md) — XSAVE/XRSTOR per-thread state management with lazy FPU, AVX/AVX2/AVX-512 kernel paths, centralised MSR infrastructure, UMIP + PKU protection keys (`SetThreadMemoryZone()`), 1 GiB huge pages + Write-Combining PAT for framebuffer, FRED event delivery + LKGS, Zen chiplet/Intel hybrid CPU topology, Intel PMU + AMD PMC counters, OSVW errata + RDTSCP per-CPU setup, AMD IBS profiling, virtualization detection, and a boot self-benchmark that auto-tunes SIMD dispatch and scheduler quantum.
- [TODO-20 Kernel Embedded Libraries](./TODO-20-kernel-libraries.md) — Freestanding string library (snprintf/vsnprintf — currently absent), complete floating-point math (sin/cos/atan2/exp/log/pow), LZ4 block compressor (needed by TODO-15 hibernation + TODO-16 crash dump), miniz deflate/inflate + ZIP, Monocypher crypto primitives (ChaCha20/Poly1305/Blake2b/Argon2id/X25519/Ed25519) with RDRAND-seeded kernel CSPRNG + SYS_GETRANDOM, cJSON DOM parser, and Mbed TLS 3.x freestanding port for the TLS record layer. Migrated from `todo-old/010-Kernel-Foundations/TODO-027-Kernel-Libraries.md`.

## Completed / Doc-converted

- ~~TODO-01 Kernel Init Sequencing~~ → [docs/kernel/init-sequencing.md](../../docs/kernel/init-sequencing.md) — 4-phase boot, readiness oracle, POST codes, recovery UI (completed 2026-04-02)
- ~~TODO-02 System Logging~~ → [docs/kernel/system-logging.md](../../docs/kernel/system-logging.md) — klog §1-§6, per-subsystem splitting, rotation, rate limiting, JSON events (completed 2026-04-02)
- ~~TODO-03 Object Manager~~ → [docs/kernel/object-manager.md](../../docs/kernel/object-manager.md) — ObXxx layer, 11 types, handle tables, namespace, security (completed 2026-04-02)

## Local Naming

- Use `TODO-01-kernel-init-sequencing.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
