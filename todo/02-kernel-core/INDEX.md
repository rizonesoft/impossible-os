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

- [src/kernel](../../src/kernel/)
- [src/kernel/main](../../src/kernel/main/)
- [include/kernel](../../include/kernel/)

## Epics

- [Gap Analysis](./GAP-ANALYSIS.md) -- 2026-04-16 coverage audit identifying the remaining kernel-core feature lanes needed to complete the domain.

## Active TODOs

- [TODO-01 Kernel Init Sequencing](./TODO-01-kernel-init-sequencing.md) -- Formal phase model, readiness oracle, dependency gates, and failure policy replacing the current ad-hoc boot sequence.
- [TODO-02 Kernel Configuration & Policy Plane](./TODO-02-kernel-configuration-policy.md) -- Typed boot/runtime configuration, BCD-style boot entries, safe-mode flags, control sets, LastKnownGood rollback, feature flags, runtime tunables, policy lock phases, and native query/set syscalls.
- [TODO-03 Kernel Embedded Libraries](./TODO-03-kernel-libraries.md) -- Freestanding string library, math library, LZ4, miniz/ZIP, Monocypher/CSPRNG, cJSON, and Mbed TLS freestanding ports used by crash dumps, hibernation, CI, logging, and networking.
- [TODO-04 System Logging](./TODO-04-system-logging.md) -- Per-subsystem log splitting, rotation, structured JSON events, rate limiting, ETW, crash-persistent capture, and log integrity. Core klog infrastructure is complete.
- [TODO-05 Object Manager](./TODO-05-object-manager.md) -- OBJECT_HEADER/OBJECT_TYPE infrastructure, reference counting, per-process handle table, named object namespace, security descriptors, and Win32 handle APIs.
- [TODO-06 Executive Support Runtime](./TODO-06-executive-support-runtime.md) -- NT Executive support primitives: callback objects, rundown protection, NPaged lookaside lists, fast references, generic tables, ERESOURCE-style wrappers, work items, bugcheck callbacks, and verifier hooks.
- [TODO-07 IRQL Model & DPCs](./TODO-07-irql-model-dpcs.md) -- Windows-style IRQL contract, per-CPU IRQL tracking, interrupt-level transition rules, `DISPATCH_LEVEL` DPC queueing, APC delivery, and IRQL diagnostics.
- [TODO-08 Time & FILETIME Management](./TODO-08-time-filetime-management.md) -- FILETIME, monotonic and wall clocks, HPET/TSC calibration, `NtQuerySystemTime`, `QueryPerformanceCounter`, timezone/DST, KUSER_SHARED_DATA time, and filesystem timestamp encoding.
- [TODO-09 x86-64 Architecture Enhancements](./TODO-09-x86-64-architecture.md) -- XSAVE/XRSTOR, SIMD kernel paths, MSR infrastructure, UMIP/PKU, huge pages/PAT, FRED/LKGS, CPU topology, PMCs, IBS, virtualization detection, and auto-tuning.
- [TODO-10 Kernel Security Hardening](./TODO-10-kernel-security-hardening.md) -- NX, SMEP/SMAP, KPTI, PCID, Spectre mitigations, CET, kernel heap cookies/redzones, stack canaries, guard pages, KASLR, and hardening-related syscalls.
- [TODO-11 PEB / TEB & User-Mode ABI](./TODO-11-peb-teb-user-abi.md) -- PEB, TEB, swapgs, KERNEL_GS_BASE, RTL_USER_PROCESS_PARAMETERS, Ldr module list, TLS slots, KUSER_SHARED_DATA, auxv, and initial user stack frame.
- [TODO-12 Native API Layer (Nt/Zw)](./TODO-12-native-api-ssdt.md) -- NTSTATUS, SYSCALL/SYSRET fast path, SSDT, Nt/Zw naming, migrated native syscalls, IOSB, LastError, Zw aliases, syscall filtering, callbacks, and SSDT integrity.
- [TODO-13 Atom, NLS & Locale Subsystem](./TODO-13-atom-nls-locale-subsystem.md) -- Global/local atom tables, Unicode string primitives, case folding, NLS table loading, code-page conversion, locale metadata, sort keys, native atom/NLS syscalls, and kernel consumer retrofits.
- [TODO-14 Registry System Completion](./TODO-14-registry-completion.md) -- Access rights, timestamps, advanced key ops, notifications, Nt/Zw registry syscalls, advapi32 shims, virtualization, .reg tooling, hive features, transactions, search, schema, SMP safety, and quotas.
- [TODO-15 Security Reference Monitor](./TODO-15-security-reference-monitor.md) -- ACCESS_TOKEN objects, SID/LUID primitives, security descriptors, ACL/ACE types, SeAccessCheck, MIC, token assignment, privileges, UAC/NtFilterToken, AppContainer, and access denial diagnostics.
- [TODO-16 Kernel Notification Facility](./TODO-16-kernel-notification-facility.md) -- WNF-style notification states, publish/subscribe API, waitable subscriptions, security policy, built-in event catalog, ETW/klog bridge, native notification syscalls, and diagnostics counters.
- [TODO-17 Binary Format System](./TODO-17-binary-system.md) -- Multi-format `exec_load()`, hardened ELF, EIF spec/loader/tooling, PE32+ loader/imports/TLS/CFG/unwind, dynamic ELF, ASLR, shebang dispatch, and EIF signing hooks.
- [TODO-18 Kernel Image & Module Registry](./TODO-18-kernel-image-module-registry.md) -- Canonical loaded-image registry for kernel, drivers, modules, PE/ELF/EIF images, symbols, unwind metadata, provenance, CI decisions, KD, crash dumps, and image notifications.
- [TODO-19 Code Integrity & Trust Policy](./TODO-19-code-integrity-trust-policy.md) -- Kernel Code Integrity policy, image validation callbacks, embedded/catalog signatures, revocation, measured-boot binding, driver/user image enforcement, and CI audit/syscalls.
- [TODO-20 EIF Full Implementation](./TODO-20-eif-full-implementation.md) -- Complete EIF from basic loader to production: segment permissions, ASLR, API version gating, metadata parsing, LZ4 decompression, range overlap validation, module registration, optional import stubs, and dispatch-table isolation.
- [TODO-21 Process Model Extensions](./TODO-21-process-model-extensions.md) -- `cwd`, standard handles, brk/sbrk, priority classes, scheduling policy, capabilities, accounting, rlimits, affinity, mitigation policy, pledge/unveil-style restrictions, Job Objects, and exit cleanup.
- [TODO-22 Environment Variables & Process Arguments](./TODO-22-environment-variables.md) -- Per-process environment storage, defaults, `%VAR%` expansion, argv/argc preparation, environment syscalls, Win32 wrappers, PATH/PATHEXT lookup, App Paths, sanitization, and shell integration.
- [TODO-23 Exception Dispatch & SEH](./TODO-23-exception-dispatch-seh.md) -- EXCEPTION_RECORD/CONTEXT, page-fault triage, CPU fault mapping, debugger first/second chance, user-mode delivery, x64 unwind, SEH/VEH/VCH, safe probing, kernel `__try`, POSIX signals, and telemetry.
- [TODO-24 ALPC / Message Ports](./TODO-24-alpc-message-ports.md) -- ALPC_PORT objects, connect/accept handshake, send/wait/reply engine, async completions, port sections, token capture/impersonation, message zones, NtAlpc* surface, CSRSS bootstrap, and profiling.
- [TODO-25 Kernel Resource Accounting & Quotas](./TODO-25-kernel-resource-accounting-quotas.md) -- Unified resource type registry, quota blocks, charge/return API, process/token/job ownership, object/handle/pool/registry/ALPC/notification quotas, pressure events, and quota syscalls.
- [TODO-26 Power Management](./TODO-26-power-management.md) -- ACPI S-states, S3/S4, hibernation, fast startup, EC/battery/lid/power events, PCI D-states, driver power callbacks, runtime idle, wake sources, thermal zones, CPU governors, connected standby, and power UI/syscalls.
- [TODO-27 Crash Dump Generation](./TODO-27-crash-dump-generation.md) -- STOP code taxonomy, FPU/XMM/XSAVE capture, module registry, WinDbg-compatible MDMP streams, minidump/kernel/full dump writers, raw dump sink, post-boot recovery, and `dmpanalyze.exe`.
- [TODO-28 BSOD / Panic Screen & Crash Experience](./TODO-28-bsod-ux-enhancements.md) -- TTF panic rendering, layout, recent logs, QR crash data, restart countdown, crash hints, statistics, safe-mode suggestion, faulting module identification, modes, beeps, recovery actions, and dump progress.
- [TODO-29 Kernel Debugger (KD Protocol)](./TODO-29-kernel-debugger-kd-protocol.md) -- Serial KD transport, packet framing, WinDbg handshake, breakin, #DB/#BP routing, AP freeze/thaw, context and memory access, breakpoints, single-step, module list, I/O/MSR access, and debug syscalls.
- [TODO-30 System Health & Recovery Orchestrator](./TODO-30-system-health-recovery-orchestrator.md) -- Runtime health states, subsystem heartbeats, failure buckets, live kernel dumps, degraded-mode transitions, recovery actions, repeated-failure escalation, and health query APIs.
- [TODO-31 Kernel Bulletproofing](./TODO-31-kernel-bulletproofing.md) -- 5-layer invariant defense for gs offsets, boot_config, user ELF range, vectors, guard pages, IXFS, security structs, framebuffer rules, exec_pending, and related fragile contracts.
- [TODO-32 Kernel Logging v2: Lockless](./TODO-32-kernel-logging-v2-lockless.md) -- Replace klog v1 with per-CPU SPSC rings, priority lanes, fail-proof FATAL path, structured fields, backpressure-aware drain worker, NMI-safe enqueue, boot-survival snapshot, and v1 retirement.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-kernel-init-sequencing.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
