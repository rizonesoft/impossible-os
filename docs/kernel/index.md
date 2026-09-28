# Kernel

Core kernel internals: init sequencing, configuration, libraries, logging, the Object Manager, the executive runtime, IRQL and DPCs, time, CPU features, security hardening, the user-mode ABI and the Native API.

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
