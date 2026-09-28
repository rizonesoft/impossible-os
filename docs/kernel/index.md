# Kernel

Core kernel internals: init sequencing, configuration, libraries, logging, the Object Manager, the executive runtime, IRQL and DPCs, time, CPU features, security hardening, the user-mode ABI, the Native API, atoms and locales, the Registry, security, notifications, binary loading, code integrity, process extensions, environment variables, exception dispatch and ALPC.

## Roadmap Overviews

One page per kernel-core roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document                                                        | Topics                                                         |
| --------------------------------------------------------------- | -------------------------------------------------------------- |
| [Kernel Init Sequencing](kernel-init-sequencing.md)             | Boot phases, readiness oracle, recovery screen, deferred init  |
| [Kernel Configuration and Policy Plane](kernel-configuration-policy.md) | Boot arguments, safe mode, tunables, feature flags, policy locks |
| [Kernel Embedded Libraries](kernel-libraries.md)                | snprintf, math, LZ4, Monocypher, CSPRNG, cJSON, CRC, SHA-3     |
| [System Logging (klog)](system-logging.md)                      | Ring buffer, log files, rotation, crash persistence, ETW       |
| [Object Manager](object-manager.md)                             | Object headers, handle tables, namespace, callbacks            |
| [Executive Support Runtime](executive-support-runtime.md)       | SLIST, rundown, callbacks, lookaside, fast refs, containers    |
| [IRQL, DPCs and APCs](irql-dpc.md)                              | IRQL levels, DPC queues, threaded DPCs, APCs, kworker          |
| [Time and FILETIME](time-filetime.md)                           | Monotonic clock, wall clock, shared time page, timer resolution |
| [x86-64 Architecture Features](x86-64-architecture.md)          | XSAVE, SIMD memops, MSRs, topology, PMCs, boot benchmark       |
| [Kernel Security Hardening](kernel-security-hardening.md)       | NX, SMEP/SMAP status, Spectre, heap and stack, W^X, KPTI       |
| [PEB, TEB and the User-Mode ABI](peb-teb-user-abi.md)           | PEB, TEB, swapgs, TLS, KUSER_SHARED_DATA, auxiliary vector     |
| [Native API and the SSDT](native-api-ssdt.md)                   | SSDT dispatch, NTSTATUS, ZwXxx, audit, system-call filters     |
| [Atom, NLS and Locale Subsystem](atom-nls-locale.md) | Case folding, atom tables, code pages, locales, sort keys |
| [Registry](registry.md) | Keys and values, hives, journaling, NT registry syscalls |
| [Security Reference Monitor](security-reference-monitor.md) | SIDs, tokens, ACLs, privileges, integrity levels |
| [Kernel Notification Facility](kernel-notification-facility.md) | Named state, sequence numbers, publish and subscribe |
| [Binary Format System](binary-format-system.md) | exec_load dispatch, ELF, PE32+ and EIF loaders, module list |
| [Kernel Image and Module Registry](kernel-image-module-registry.md) | KIMAGE_ENTRY data model, planned image registries |
| [Code Integrity and Trust Policy](code-integrity-trust-policy.md) | CI verdicts, enforcement modes, signature verification |
| [EIF Executable Format](eif-executable-format.md) | EIF header, SSDT imports, LZ4 segments, validation |
| [Process Model Extensions](process-model-extensions.md) | Working directory, pledge and unveil, process groups, rlimits, exec commit |
| [Environment Variables](environment-variables.md) | Per-task environment, argv, expansion, SearchPath |
| [Exception Dispatch and SEH](exception-dispatch-seh.md) | CONTEXT capture, fault triage, unwinding, kernel __try/__except |
| [ALPC and Message Ports](alpc-message-ports.md) | Ports, connect and accept, send-wait-receive, impersonation |
