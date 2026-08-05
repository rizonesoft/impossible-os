---
schema_version: 1
id: ssdt-master-table
domain: 02-kernel-core
status: active
title: "SSDT Master Table"
---

# SSDT Master Table

> Service numbers organized by functional range. Each range has headroom for future additions. Endpoints marked `→ TODO-XX` are implemented in that TODO and registered here.
> Win32k shadow SSDT (Table 1, indices `0x1000+`): [`../08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md`](../08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md).

**0x0000–0x000F: Core Object and Handle Operations**

| Index  | Function                       | §   | Owner                 | Done |
| ------ | ------------------------------ | --- | --------------------- | ---- |
| 0x0000 | NtClose                        | §5  | T12 (§5 SSDT wrapper) | [x]  |
| 0x0001 | NtDuplicateObject              | §6  | T12 (ob.c exists)     | [ ]  |
| 0x0002 | NtQueryObject                  | §6  | T12 (ob.c exists)     | [ ]  |
| 0x0003 | NtMakeTemporaryObject          | §17 | T12                   | [ ]  |
| 0x0004 | NtMakePermanentObject          | §17 | T12                   | [ ]  |
| 0x0005 | NtSetInformationObject         | §17 | T12                   | [ ]  |
| 0x0006 | NtWaitForSingleObject          | §8  | T12 (§8 upgraded)     | [x]  |
| 0x0007 | NtWaitForMultipleObjects       | §8  | T12 (§8 implemented)  | [x]  |
| 0x0008 | NtSignalAndWaitForSingleObject | §8  | T12 (§8 implemented)  | [x]  |
| 0x0009 | NtCompareObjects               | §17 | T12                   | [ ]  |

**0x0010–0x002F: File I/O**

| Index  | Function                     | §   | Owner                      | Done |
| ------ | ---------------------------- | --- | -------------------------- | ---- |
| 0x0010 | NtCreateFile                 | §6  | T12 (§6 implemented)       | [x]  |
| 0x0011 | NtOpenFile                   | §6  | T12 (§6 implemented)       | [x]  |
| 0x0012 | NtReadFile                   | §5  | T12 (§5 migration)         | [x]  |
| 0x0013 | NtWriteFile                  | §5  | T12 (§5 migration)         | [x]  |
| 0x0014 | NtDeleteFile                 | §13 | T12 §13 (nt_file.c)        | [x]  |
| 0x0015 | NtQueryInformationFile       | §13 | T12 §13 (nt_file.c)        | [x]  |
| 0x0016 | NtSetInformationFile         | §13 | T12 §13 (nt_file.c)        | [x]  |
| 0x0017 | NtQueryDirectoryFile         | §5  | T12 (§5 migration)         | [x]  |
| 0x0018 | NtFlushBuffersFile           | §13 | T12 §13 (nt_file.c)        | [x]  |
| 0x0019 | NtDeviceIoControlFile        | §13 | T12 §13 (IRP deferred)     | [/]  |
| 0x001A | NtFsControlFile              | §13 | T12 §13 (IRP deferred)     | [/]  |
| 0x001B | NtCreateNamedPipeFile        | §5  | T12 (§5 migration)         | [x]  |
| 0x001C | NtCreateMailslotFile         | §13 | T17-mem §3 (MSFS deferred) | [/]  |
| 0x001D | NtLockFile                   | §13 | T12 §13 (nt_file.c)        | [x]  |
| 0x001E | NtUnlockFile                 | §13 | T12 §13 (nt_file.c)        | [x]  |
| 0x001F | NtNotifyChangeDirectoryFile  | §13 | T12 §13 (async deferred)   | [/]  |
| 0x0020 | NtQueryVolumeInformationFile | §13 | T12 §13 (nt_file.c)        | [x]  |
| 0x0021 | NtSetVolumeInformationFile   | §13 | T12                        | [ ]  |
| 0x0022 | NtQueryEaFile                | §13 | T12                        | [ ]  |
| 0x0023 | NtSetEaFile                  | §13 | T12                        | [ ]  |
| 0x0024 | NtReadFileScatter            | §13 | T12 §13 (page-align def)   | [/]  |
| 0x0025 | NtWriteFileGather            | §13 | T12 §13 (page-align def)   | [/]  |
| 0x0026 | NtCancelIoFile               | §13 | T12 §13 (no-op sync I/O)   | [x]  |
| 0x0027 | NtCancelIoFileEx             | §13 | T12 §13 (no-op sync I/O)   | [x]  |
| 0x0028 | NtQueryAttributesFile        | §13 | T12 §13 (nt_file.c)        | [x]  |
| 0x0029 | NtQueryFullAttributesFile    | §13 | T12                        | [ ]  |

**0x0030–0x004F: Process and Thread**

| Index  | Function                  | §   | Owner                 | Done |
| ------ | ------------------------- | --- | --------------------- | ---- |
| 0x0030 | NtCreateProcess           | §7  | T12 (§7 implemented)  | [x]  |
| 0x0031 | NtCreateProcessEx         | §7  | T12 (§7 alias)        | [x]  |
| 0x0032 | NtOpenProcess             | §7  | T12 (§7 implemented)  | [x]  |
| 0x0033 | NtTerminateProcess        | §7  | T12 (§7 upgraded)     | [x]  |
| 0x0034 | NtQueryInformationProcess | §7  | T12 (§7 implemented)  | [x]  |
| 0x0035 | NtSetInformationProcess   | §7  | T12 (§7 implemented)  | [x]  |
| 0x0036 | NtCreateThread            | §7  | T12 (§7 implemented)  | [x]  |
| 0x0037 | NtCreateThreadEx          | §7  | T12 (§7 alias)        | [x]  |
| 0x0038 | NtOpenThread              | §7  | T12 (§7 implemented)  | [x]  |
| 0x0039 | NtTerminateThread         | §7  | T12 (§7 implemented)  | [x]  |
| 0x003A | NtResumeThread            | §7  | T12 (§7 implemented)  | [x]  |
| 0x003B | NtSuspendThread           | §7  | T12 (§7 implemented)  | [x]  |
| 0x003C | NtGetContextThread        | §7  | T23 §4 (CONTEXT stub) | [/]  |
| 0x003D | NtSetContextThread        | §7  | T23 §4 (CONTEXT stub) | [/]  |
| 0x003E | NtQueryInformationThread  | §7  | T12 (§7 implemented)  | [x]  |
| 0x003F | NtSetInformationThread    | §7  | T12 (§7 implemented)  | [x]  |
| 0x0040 | NtAlertThread             | §7  | T12 (§7 implemented)  | [x]  |
| 0x0041 | NtAlertResumeThread       | §7  | T12 (§7 implemented)  | [x]  |
| 0x0042 | NtImpersonateThread       | §7  | T15 (SRM stub)        | [/]  |
| 0x0043 | NtQueueApcThread          | §7  | T07 §11 (APC stub)    | [/]  |
| 0x0044 | NtYieldExecution          | §5  | T12 (§5 migration)    | [x]  |
| 0x0045 | NtCreateUserProcess       | §7  | T21 §4 (stub)         | [/]  |
| 0x0046 | NtTestAlert               | §7  | T12 (§7 implemented)  | [x]  |
| 0x0047 | NtDelayExecution          | §7  | T12 (§7 implemented)  | [x]  |

**0x0050–0x006F: Memory Management**

| Index  | Function                    | §   | Owner                  | Done |
| ------ | --------------------------- | --- | ---------------------- | ---- |
| 0x0050 | NtAllocateVirtualMemory     | §9  | T12 (§9 implemented)   | [x]  |
| 0x0051 | NtFreeVirtualMemory         | §9  | T12 (§9 implemented)   | [x]  |
| 0x0052 | NtProtectVirtualMemory      | §9  | T12 (§9 implemented)   | [x]  |
| 0x0053 | NtQueryVirtualMemory        | §9  | T12 (§9 implemented)   | [x]  |
| 0x0054 | NtLockVirtualMemory         | §9  | T12 (§9 no-op)         | [x]  |
| 0x0055 | NtUnlockVirtualMemory       | §9  | T12 (§9 no-op)         | [x]  |
| 0x0056 | NtFlushVirtualMemory        | §9  | T12 (§9 no-op)         | [x]  |
| 0x0057 | NtReadVirtualMemory         | §9  | T12 (§9 implemented)   | [x]  |
| 0x0058 | NtWriteVirtualMemory        | §9  | T12 (§9 implemented)   | [x]  |
| 0x0059 | NtAllocateUserPhysicalPages | §9  | T05-mem §11 (AWE stub) | [/]  |
| 0x005A | NtFreeUserPhysicalPages     | §9  | T05-mem §11 (AWE stub) | [/]  |
| 0x005B | NtMapUserPhysicalPages      | §9  | T05-mem §11 (AWE stub) | [/]  |
| 0x005C | NtCreateSection             | §18 | T12 (nt_section.c)     | [x]  |
| 0x005D | NtOpenSection               | §18 | T12 (nt_section.c)     | [x]  |
| 0x005E | NtMapViewOfSection          | §18 | T12 (nt_section.c)     | [x]  |
| 0x005F | NtUnmapViewOfSection        | §18 | T12 (nt_section.c)     | [x]  |
| 0x0060 | NtExtendSection             | §18 | T12 (nt_section.c)     | [x]  |
| 0x0061 | NtQuerySection              | §18 | T12 (nt_section.c)     | [x]  |
| 0x0062 | NtAreMappedFilesTheSame     | §18 | T12 (nt_section.c)     | [x]  |

**0x0070–0x008F: Synchronization**

| Index  | Function               | §   | Owner                    | Done |
| ------ | ---------------------- | --- | ------------------------ | ---- |
| 0x0070 | NtCreateEvent          | §8  | T12 (§8 implemented)     | [x]  |
| 0x0071 | NtOpenEvent            | §8  | T12 (§8 implemented)     | [x]  |
| 0x0072 | NtSetEvent             | §8  | T12 (§8 implemented)     | [x]  |
| 0x0073 | NtResetEvent           | §8  | T12 (§8 implemented)     | [x]  |
| 0x0074 | NtPulseEvent           | §8  | T12 (§8 implemented)     | [x]  |
| 0x0075 | NtQueryEvent           | §8  | T12 (§8 implemented)     | [x]  |
| 0x0076 | NtCreateMutant         | §8  | T12 (§8 implemented)     | [x]  |
| 0x0077 | NtOpenMutant           | §8  | T12 (§8 implemented)     | [x]  |
| 0x0078 | NtReleaseMutant        | §8  | T12 (§8 implemented)     | [x]  |
| 0x0079 | NtQueryMutant          | §8  | T12 (§8 implemented)     | [x]  |
| 0x007A | NtCreateSemaphore      | §8  | T12 (§8 implemented)     | [x]  |
| 0x007B | NtOpenSemaphore        | §8  | T12 (§8 implemented)     | [x]  |
| 0x007C | NtReleaseSemaphore     | §8  | T12 (§8 implemented)     | [x]  |
| 0x007D | NtQuerySemaphore       | §8  | T12 (§8 implemented)     | [x]  |
| 0x007E | NtCreateTimer          | §19 | T12 §19 (nt_timer.c)     | [x]  |
| 0x007F | NtOpenTimer            | §19 | T12 §19 (nt_timer.c)     | [x]  |
| 0x0080 | NtSetTimer             | §19 | T12 §19 (nt_timer.c)     | [x]  |
| 0x0081 | NtCancelTimer          | §19 | T12 §19 (nt_timer.c)     | [x]  |
| 0x0082 | NtQueryTimer           | §19 | T12 §19 (nt_timer.c)     | [x]  |
| 0x0083 | NtSetTimerEx           | §19 | T12 §19 (nt_timer.c)     | [x]  |
| 0x0084 | NtCreateKeyedEvent     | §8  | T08-mem §10 (stub)       | [/]  |
| 0x0085 | NtOpenKeyedEvent       | §8  | T08-mem §10 (stub)       | [/]  |
| 0x0086 | NtWaitForKeyedEvent    | §8  | T08-mem §10 (stub)       | [/]  |
| 0x0087 | NtReleaseKeyedEvent    | §8  | T08-mem §10 (stub)       | [/]  |
| 0x0088 | NtCreateIoCompletion   | §13 | T12 §13 (nt_file.c IOCP) | [x]  |
| 0x0089 | NtSetIoCompletion      | §13 | T12 §13 (nt_file.c IOCP) | [x]  |
| 0x008A | NtRemoveIoCompletion   | §13 | T12 §13 (nt_file.c IOCP) | [x]  |
| 0x008B | NtQueryIoCompletion    | §13 | T17-mem §4               | [ ]  |
| 0x008C | NtSetIoCompletionEx    | §13 | T17-mem §4               | [ ]  |
| 0x008D | NtRemoveIoCompletionEx | §13 | T17-mem §4               | [ ]  |

**0x0090–0x00AF: Registry**

| Index  | Function                   | §   | Owner                            | Done |
| ------ | -------------------------- | --- | -------------------------------- | ---- |
| 0x0090 | NtCreateKey                | §14 | T12 §14 (nt_registry.c)          | [x]  |
| 0x0091 | NtCreateKeyTransacted      | §14 | T14 §4                           | [ ]  |
| 0x0092 | NtOpenKey                  | §14 | T12 §14 (nt_registry.c)          | [x]  |
| 0x0093 | NtOpenKeyTransacted        | §14 | T14 §4                           | [ ]  |
| 0x0094 | NtOpenKeyEx                | §14 | T12 §14 (nt_registry.c)          | [x]  |
| 0x0095 | NtDeleteKey                | §14 | T12 §14 (nt_registry.c)          | [x]  |
| 0x0096 | NtSetValueKey              | §14 | T12 §14 (nt_registry.c)          | [x]  |
| 0x0097 | NtQueryValueKey            | §14 | T12 §14 (nt_registry.c)          | [x]  |
| 0x0098 | NtDeleteValueKey           | §14 | T12 §14 (nt_registry.c)          | [x]  |
| 0x0099 | NtEnumerateKey             | §14 | T12 §14 (nt_registry.c)          | [x]  |
| 0x009A | NtEnumerateValueKey        | §14 | T12 §14 (nt_registry.c)          | [x]  |
| 0x009B | NtQueryKey                 | §14 | T12 §14 (nt_registry.c)          | [x]  |
| 0x009C | NtFlushKey                 | §15 | T12 §15 (nt_registry.c)          | [x]  |
| 0x009D | NtNotifyChangeKey          | §15 | T14 §3 (stub: watchers not impl) | [/]  |
| 0x009E | NtNotifyChangeMultipleKeys | §14 | T14 §4                           | [ ]  |
| 0x009F | NtRenameKey                | §15 | T12 §15 (nt_registry.c)          | [x]  |
| 0x00A0 | NtSaveKey                  | §15 | T12 §15 (nt_registry.c)          | [x]  |
| 0x00A1 | NtSaveKeyEx                | §15 | T12 §15 (nt_registry.c)          | [x]  |
| 0x00A2 | NtRestoreKey               | §15 | T12 §15 (nt_registry.c)          | [x]  |
| 0x00A3 | NtLoadKey                  | §15 | T12 §15 (nt_registry.c)          | [x]  |
| 0x00A4 | NtLoadKeyEx                | §15 | T12 §15 (nt_registry.c)          | [x]  |
| 0x00A5 | NtUnloadKey                | §15 | T12 §15 (nt_registry.c)          | [x]  |
| 0x00A6 | NtUnloadKeyEx              | §15 | T12 §15 (nt_registry.c)          | [x]  |
| 0x00A7 | NtQueryOpenSubKeys         | §14 | T14 §4                           | [ ]  |
| 0x00A8 | NtCompactKeys              | §14 | T14 §4                           | [ ]  |
| 0x00A9 | NtCompressKey              | §14 | T14 §4                           | [ ]  |
| 0x00AA | NtLockRegistryKey          | §14 | T14 §4                           | [ ]  |

**0x00B0–0x00CF: Security and Token**

| Index  | Function                    | §   | Owner                  | Done |
| ------ | --------------------------- | --- | ---------------------- | ---- |
| 0x00B0 | NtOpenProcessToken          | §16 | T12 §16 (nt_token.c)   | [x]  |
| 0x00B1 | NtOpenProcessTokenEx        | §16 | T12 §16 (nt_token.c)   | [x]  |
| 0x00B2 | NtOpenThreadToken           | §16 | T12 §16 (nt_token.c)   | [x]  |
| 0x00B3 | NtOpenThreadTokenEx         | §16 | T12 §16 (nt_token.c)   | [x]  |
| 0x00B4 | NtQueryInformationToken     | §16 | T12 §16 (nt_token.c)   | [x]  |
| 0x00B5 | NtSetInformationToken       | §16 | T12 §16 (nt_token.c)   | [x]  |
| 0x00B6 | NtAdjustPrivilegesToken     | §16 | T12 §16 (nt_token.c)   | [x]  |
| 0x00B7 | NtAdjustGroupsToken         | §16 | T12 §16 (nt_token.c)   | [x]  |
| 0x00B8 | NtDuplicateToken            | §16 | T15 (token.c exists)   | [ ]  |
| 0x00B9 | NtFilterToken               | §16 | T15                    | [ ]  |
| 0x00BA | NtCreateToken               | §16 | T15                    | [ ]  |
| 0x00BB | NtCompareTokens             | §16 | T15                    | [ ]  |
| 0x00BC | NtAccessCheck               | §16 | T15 §4 (SeAccessCheck) | [ ]  |
| 0x00BD | NtAccessCheckAndAuditAlarm  | §16 | T15                    | [ ]  |
| 0x00BE | NtAccessCheckByType         | §16 | T15                    | [ ]  |
| 0x00BF | NtPrivilegeCheck            | §16 | T15 §12 (nt_token.c)   | [x]  |
| 0x00C0 | NtPrivilegeObjectAuditAlarm | §16 | T15                    | [ ]  |
| 0x00C1 | NtSetSecurityObject         | §16 | T15                    | [ ]  |
| 0x00C2 | NtQuerySecurityObject       | §16 | T15                    | [ ]  |
| 0x00C3 | NtAllocateLocallyUniqueId   | §16 | T12 §16 (nt_token.c)   | [x]  |
| 0x00C4 | NtCreateTokenEx             | §16 | T15                    | [ ]  |

**0x00D0–0x00EF: System Information and Control**

| Index  | Function                             | §   | Owner                     | Done |
| ------ | ------------------------------------ | --- | ------------------------- | ---- |
| 0x00D0 | NtQuerySystemInformation             | §5  | T12 (§5 migration)        | [x]  |
| 0x00D1 | NtSetSystemInformation               | §10 | T12 (stub -- needs privs) | [/]  |
| 0x00D2 | NtQuerySystemEnvironmentValue        | §23 | T12 §23 (uefi_runtime.c)  | [x]  |
| 0x00D3 | NtSetSystemEnvironmentValue          | §23 | T12 §23 (uefi_runtime.c)  | [x]  |
| 0x00D4 | NtQuerySystemEnvironmentValueEx      | §23 | T12 §23 (uefi_runtime.c)  | [x]  |
| 0x00D5 | NtSetSystemEnvironmentValueEx        | §23 | T12 §23 (uefi_runtime.c)  | [x]  |
| 0x00D6 | NtEnumerateSystemEnvironmentValuesEx | §23 | T12 §23                   | [ ]  |
| 0x00D7 | NtShutdownSystem                     | §5  | T12 (§5 migration)        | [x]  |
| 0x00D8 | NtDisplayString                      | §23 | T12                       | [x]  |
| 0x00D9 | NtRaiseHardError                     | §23 | T12                       | [x]  |
| 0x00DA | NtQueryDefaultLocale                 | §23 | T12                       | [x]  |
| 0x00DB | NtSetDefaultLocale                   | §23 | T12                       | [x]  |
| 0x00DC | NtQueryDefaultUILanguage             | §23 | T12                       | [x]  |
| 0x00DD | NtSetDefaultUILanguage               | §23 | T12                       | [x]  |
| 0x00DE | NtQueryInstallUILanguage             | §23 | T12                       | [x]  |
| 0x00DF | NtAddAtom                            | §23 | T12                       | [x]  |
| 0x00E0 | NtFindAtom                           | §23 | T12                       | [x]  |
| 0x00E1 | NtDeleteAtom                         | §23 | T12                       | [x]  |
| 0x00E2 | NtQueryInformationAtom               | §23 | T12                       | [x]  |

**0x00F0–0x00FF: Time and Timer (→ XREF TODO-17 §6,§9)** -- **5/5 DONE** (wall_clock.c, timer_resolution.c)

| Index  | Function                  | §   | Owner                       | Done |
| ------ | ------------------------- | --- | --------------------------- | ---- |
| 0x00F0 | NtQuerySystemTime         | §19 | T08 §9 (wall_clock.c)       | [x]  |
| 0x00F1 | NtSetSystemTime           | §19 | T08 §9 (wall_clock.c)       | [x]  |
| 0x00F2 | NtQueryPerformanceCounter | §19 | T08 §9 (wall_clock.c)       | [x]  |
| 0x00F3 | NtQueryTimerResolution    | §19 | T08 §8 (timer_resolution.c) | [x]  |
| 0x00F4 | NtSetTimerResolution      | §19 | T08 §8 (timer_resolution.c) | [x]  |

**0x0100–0x011F: ALPC and LPC Ports (→ XREF TODO-24 §8-§9)**

| Index  | Function                      | §   | Owner                                    | Done |
| ------ | ----------------------------- | --- | ---------------------------------------- | ---- |
| 0x0100 | NtCreatePort                  | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x0101 | NtCreateWaitablePort          | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x0102 | NtConnectPort                 | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x0103 | NtSecureConnectPort           | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x0104 | NtAcceptConnectPort           | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x0105 | NtCompleteConnectPort         | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x0106 | NtListenPort                  | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x0107 | NtReplyPort                   | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x0108 | NtReplyWaitReceivePort        | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x0109 | NtReplyWaitReceivePortEx      | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x010A | NtRequestPort                 | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x010B | NtRequestWaitReplyPort        | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x010C | NtImpersonateClientOfPort     | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x010D | NtReadRequestData             | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x010E | NtWriteRequestData            | §20 | T12 §20 (nt_lpc.c stub, T17 §7 retrofit) | [/]  |
| 0x010F | NtAlpcCreatePort              | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x0110 | NtAlpcConnectPort             | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x0111 | NtAlpcConnectPortEx           | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x0112 | NtAlpcAcceptConnectPort       | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x0113 | NtAlpcSendWaitReceivePort     | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x0114 | NtAlpcDisconnectPort          | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x0115 | NtAlpcCancelMessage           | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x0116 | NtAlpcCreatePortSection       | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x0117 | NtAlpcDeletePortSection       | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x0118 | NtAlpcCreateSectionView       | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x0119 | NtAlpcDeleteSectionView       | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x011A | NtAlpcCreateResourceReserve   | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x011B | NtAlpcDeleteResourceReserve   | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x011C | NtAlpcQueryInformation        | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x011D | NtAlpcSetInformation          | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |
| 0x011E | NtAlpcQueryInformationMessage | §31 | T12 §31 (nt_alpc.c stub, T24 §8 retrofit) | [/]  |

**0x0120–0x012F: Namespace and Directory Objects**

| Index  | Function                   | §   | Owner                    | Done |
| ------ | -------------------------- | --- | ------------------------ | ---- |
| 0x0120 | NtCreateDirectoryObject    | §17 | T12 §17 (nt_namespace.c) | [x]  |
| 0x0121 | NtOpenDirectoryObject      | §17 | T12 §17 (nt_namespace.c) | [x]  |
| 0x0122 | NtQueryDirectoryObject     | §17 | T12 §17 (nt_namespace.c) | [x]  |
| 0x0123 | NtCreateSymbolicLinkObject | §17 | T12 §17 (nt_namespace.c) | [x]  |
| 0x0124 | NtOpenSymbolicLinkObject   | §17 | T12 §17 (nt_namespace.c) | [x]  |
| 0x0125 | NtQuerySymbolicLinkObject  | §17 | T12 §17 (nt_namespace.c) | [x]  |

**0x0130–0x013F: Debug and Exception (→ XREF TODO-23 §5, TODO-29)**

| Index  | Function                    | §   | Owner   | Done |
| ------ | --------------------------- | --- | ------- | ---- |
| 0x0130 | NtRaiseException            | §21 | T23 §4  | [ ]  |
| 0x0131 | NtContinue                  | §21 | T23 §4  | [ ]  |
| 0x0132 | NtDebugActiveProcess        | §21 | T29 §13 | [ ]  |
| 0x0133 | NtDebugContinue             | §21 | T29 §13 | [ ]  |
| 0x0134 | NtRemoveProcessDebug        | §21 | T29 §13 | [ ]  |
| 0x0135 | NtCreateDebugObject         | §21 | T29 §13 | [ ]  |
| 0x0136 | NtWaitForDebugEvent         | §21 | T29 §13 | [ ]  |
| 0x0137 | NtSetInformationDebugObject | §21 | T29 §13 | [ ]  |

**0x0140–0x014F: Power and Shutdown (→ XREF TODO-26)**

| Index  | Function                  | §   | Owner   | Done |
| ------ | ------------------------- | --- | ------- | ---- |
| 0x0140 | NtSetSystemPowerState     | §22 | T26 §12 | [ ]  |
| 0x0141 | NtInitiatePowerAction     | §22 | T26 §12 | [ ]  |
| 0x0142 | NtPowerInformation        | §22 | T26 §12 | [ ]  |
| 0x0143 | NtGetDevicePowerState     | §22 | T26 §12 | [ ]  |
| 0x0144 | NtSetThreadExecutionState | §22 | T26 §12 | [ ]  |
| 0x0145 | NtRequestWakeupLatency    | §22 | T26 §12 | [ ]  |

**0x0150–0x015F: Audit and Tracing (Impossible OS exclusive)**

| Index  | Function                     | §   | Owner | Done |
| ------ | ---------------------------- | --- | ----- | ---- |
| 0x0150 | NtRegisterSyscallAuditHook   | §24 | T12   | [/]  |
| 0x0151 | NtUnregisterSyscallAuditHook | §24 | T12   | [/]  |
| 0x0152 | NtQuerySyscallAuditState     | §24 | T12   | [x]  |

**0x0160–0x017F: Job Objects**

| Index  | Function                    | §   | Owner   | Done |
| ------ | --------------------------- | --- | ------- | ---- |
| 0x0160 | NtCreateJobObject           | §7  | T21 §13 | [x]  |
| 0x0161 | NtOpenJobObject             | §7  | T21 §13 | [x]  |
| 0x0162 | NtAssignProcessToJobObject  | §7  | T21 §13 | [x]  |
| 0x0163 | NtTerminateJobObject        | §7  | T21 §13 | [x]  |
| 0x0164 | NtQueryInformationJobObject | §7  | T21 §13 | [x]  |
| 0x0165 | NtSetInformationJobObject   | §7  | T21 §13 | [x]  |
| 0x0166 | NtIsProcessInJob            | §7  | T21 §13 | [x]  |
| 0x0167 | NtCreateJobSet              | §7  | T21 §13 | [/]  |

**0x0180–0x019F: Worker Factory (Thread Pool)**

| Index  | Function                        | §   | Owner                  | Done |
| ------ | ------------------------------- | --- | ---------------------- | ---- |
| 0x0180 | NtCreateWorkerFactory           | §7  | T12-mem §5 (IOCP pool) | [ ]  |
| 0x0181 | NtWorkerFactoryWorkerReady      | §7  | T12-mem §5             | [ ]  |
| 0x0182 | NtReleaseWorkerFactoryWorker    | §7  | T12-mem §5             | [ ]  |
| 0x0183 | NtShutdownWorkerFactory         | §7  | T12-mem §5             | [ ]  |
| 0x0184 | NtQueryInformationWorkerFactory | §7  | T12-mem §5             | [ ]  |
| 0x0185 | NtSetInformationWorkerFactory   | §7  | T12-mem §5             | [ ]  |
| 0x0186 | NtWaitForWorkViaWorkerFactory   | §7  | T12-mem §5             | [ ]  |

**0x01A0–0x01CF: Kernel Transaction Manager (KTM)**

| Index  | Function                          | §   | Owner              | Done |
| ------ | --------------------------------- | --- | ------------------ | ---- |
| 0x01A0 | NtCreateTransactionManager        | §23 | T14 (registry txn) | [ ]  |
| 0x01A1 | NtOpenTransactionManager          | §23 | T14                | [ ]  |
| 0x01A2 | NtCreateTransaction               | §23 | T14                | [ ]  |
| 0x01A3 | NtOpenTransaction                 | §23 | T14                | [ ]  |
| 0x01A4 | NtCommitTransaction               | §23 | T14                | [ ]  |
| 0x01A5 | NtRollbackTransaction             | §23 | T14                | [ ]  |
| 0x01A6 | NtQueryInformationTransaction     | §23 | T14                | [ ]  |
| 0x01A7 | NtSetInformationTransaction       | §23 | T14                | [ ]  |
| 0x01A8 | NtCreateResourceManager           | §23 | T14                | [ ]  |
| 0x01A9 | NtOpenResourceManager             | §23 | T14                | [ ]  |
| 0x01AA | NtQueryInformationResourceManager | §23 | T14                | [ ]  |
| 0x01AB | NtSetInformationResourceManager   | §23 | T14                | [ ]  |
| 0x01AC | NtCreateEnlistment                | §23 | T14                | [ ]  |
| 0x01AD | NtOpenEnlistment                  | §23 | T14                | [ ]  |
| 0x01AE | NtQueryInformationEnlistment      | §23 | T14                | [ ]  |
| 0x01AF | NtSetInformationEnlistment        | §23 | T14                | [ ]  |
| 0x01B0 | NtPrepareEnlistment               | §23 | T14                | [ ]  |
| 0x01B1 | NtPrePrepareEnlistment            | §23 | T14                | [ ]  |
| 0x01B2 | NtCommitEnlistment                | §23 | T14                | [ ]  |
| 0x01B3 | NtRollbackEnlistment              | §23 | T14                | [ ]  |
| 0x01B4 | NtRecoverTransactionManager       | §23 | T14                | [ ]  |
| 0x01B5 | NtRecoverResourceManager          | §23 | T14                | [ ]  |
| 0x01B6 | NtRecoverEnlistment               | §23 | T14                | [ ]  |
| 0x01B7 | NtPropagationComplete             | §23 | T14                | [ ]  |
| 0x01B8 | NtPropagationFailed               | §23 | T14                | [ ]  |
| 0x01B9 | NtFreezeTransactions              | §23 | T14                | [ ]  |
| 0x01BA | NtThawTransactions                | §23 | T14                | [ ]  |
| 0x01BB | NtCreateRegistryTransaction       | §14 | T14 §4             | [ ]  |
| 0x01BC | NtOpenRegistryTransaction         | §14 | T14 §4             | [ ]  |
| 0x01BD | NtCommitRegistryTransaction       | §14 | T14 §4             | [ ]  |
| 0x01BE | NtRollbackRegistryTransaction     | §14 | T14 §4             | [ ]  |

**0x01D0–0x01DF: ETW (Event Tracing for Windows)** -- **7/7 DONE** (etw.c, T04 §7)

| Index  | Function       | §   | Owner          | Done |
| ------ | -------------- | --- | -------------- | ---- |
| 0x01D0 | NtTraceEvent   | §23 | T04 §7 (etw.c) | [x]  |
| 0x01D1 | NtTraceControl | §23 | T04 §7 (etw.c) | [x]  |
| 0x01D2 | NtCreateTrace  | §23 | T04 §7 (etw.c) | [x]  |
| 0x01D3 | NtQueryTrace   | §23 | T04 §7 (etw.c) | [x]  |
| 0x01D4 | NtUpdateTrace  | §23 | T04 §7 (etw.c) | [x]  |
| 0x01D5 | NtStopTrace    | §23 | T04 §7 (etw.c) | [x]  |
| 0x01D6 | NtFlushTrace   | §23 | T04 §7 (etw.c) | [x]  |

**0x01E0–0x01EF: WNF (Windows Notification Facility)**

| Index  | Function                       | §   | Owner | Done |
| ------ | ------------------------------ | --- | ----- | ---- |
| 0x01E0 | NtCreateWnfStateName           | §23 | T12   | [ ]  |
| 0x01E1 | NtDeleteWnfStateName           | §23 | T12   | [ ]  |
| 0x01E2 | NtQueryWnfStateData            | §23 | T12   | [ ]  |
| 0x01E3 | NtUpdateWnfStateData           | §23 | T12   | [ ]  |
| 0x01E4 | NtSubscribeWnfStateChange      | §23 | T12   | [ ]  |
| 0x01E5 | NtUnsubscribeWnfStateChange    | §23 | T12   | [ ]  |
| 0x01E6 | NtQueryWnfStateNameInformation | §23 | T12   | [ ]  |

**0x01F0–0x01FF: Enclave (VBS / SGX)**

| Index  | Function            | §   | Owner                    | Done |
| ------ | ------------------- | --- | ------------------------ | ---- |
| 0x01F0 | NtCreateEnclave     | §23 | T10 (security hardening) | [ ]  |
| 0x01F1 | NtLoadEnclaveData   | §23 | T10                      | [ ]  |
| 0x01F2 | NtInitializeEnclave | §23 | T10                      | [ ]  |
| 0x01F3 | NtTerminateEnclave  | §23 | T10                      | [ ]  |
| 0x01F4 | NtCallEnclave       | §23 | T10                      | [ ]  |

**0x0200–0x021F: Process and Thread Extensions**

| Index  | Function                      | §   | Owner  | Done |
| ------ | ----------------------------- | --- | ------ | ---- |
| 0x0200 | NtSuspendProcess              | §7  | T21 §4 | [ ]  |
| 0x0201 | NtResumeProcess               | §7  | T21 §4 | [ ]  |
| 0x0202 | NtGetNextProcess              | §7  | T21 §4 | [ ]  |
| 0x0203 | NtGetNextThread               | §7  | T21 §4 | [ ]  |
| 0x0204 | NtCreateProcessStateChange    | §7  | T21    | [ ]  |
| 0x0205 | NtChangeProcessState          | §7  | T21    | [ ]  |
| 0x0206 | NtCreateThreadStateChange     | §7  | T21    | [ ]  |
| 0x0207 | NtChangeThreadState           | §7  | T21    | [ ]  |
| 0x0208 | NtGetCurrentProcessorNumber   | §10 | T12    | [ ]  |
| 0x0209 | NtGetCurrentProcessorNumberEx | §10 | T12    | [ ]  |
| 0x020A | NtFlushProcessWriteBuffers    | §9  | T12    | [ ]  |
| 0x020B | NtQueryPortInformationProcess | §10 | T12    | [ ]  |

**0x0220–0x023F: Memory Extensions**

| Index  | Function                      | §   | Owner      | Done |
| ------ | ----------------------------- | --- | ---------- | ---- |
| 0x0220 | NtAllocateVirtualMemoryEx     | §9  | T11-mem §4 | [ ]  |
| 0x0221 | NtCreateSectionEx             | §18 | T12        | [ ]  |
| 0x0222 | NtMapViewOfSectionEx          | §18 | T12        | [ ]  |
| 0x0223 | NtSetInformationVirtualMemory | §9  | T11-mem §4 | [ ]  |
| 0x0224 | NtGetWriteWatch               | §9  | T11-mem §4 | [ ]  |
| 0x0225 | NtResetWriteWatch             | §9  | T11-mem §4 | [ ]  |
| 0x0226 | NtCreatePagingFile            | §9  | T12        | [ ]  |

**0x0240–0x024F: Event Pair**

| Index  | Function            | §   | Owner | Done |
| ------ | ------------------- | --- | ----- | ---- |
| 0x0240 | NtCreateEventPair   | §8  | T12   | [ ]  |
| 0x0241 | NtOpenEventPair     | §8  | T12   | [ ]  |
| 0x0242 | NtSetHighEventPair  | §8  | T12   | [ ]  |
| 0x0243 | NtSetLowEventPair   | §8  | T12   | [ ]  |
| 0x0244 | NtWaitHighEventPair | §8  | T12   | [ ]  |
| 0x0245 | NtWaitLowEventPair  | §8  | T12   | [ ]  |

**0x0250–0x025F: Profile and Performance Counters**

| Index  | Function               | §   | Owner | Done |
| ------ | ---------------------- | --- | ----- | ---- |
| 0x0250 | NtCreateProfile        | §10 | T12   | [ ]  |
| 0x0251 | NtCreateProfileEx      | §10 | T12   | [ ]  |
| 0x0252 | NtStartProfile         | §10 | T12   | [ ]  |
| 0x0253 | NtStopProfile          | §10 | T12   | [ ]  |
| 0x0254 | NtSetIntervalProfile   | §10 | T12   | [ ]  |
| 0x0255 | NtQueryIntervalProfile | §10 | T12   | [ ]  |

**0x0260–0x026F: Session and Licensing**

| Index  | Function                 | §   | Owner | Done |
| ------ | ------------------------ | --- | ----- | ---- |
| 0x0260 | NtOpenSession            | §23 | T12   | [ ]  |
| 0x0261 | NtNotifyChangeSession    | §23 | T12   | [ ]  |
| 0x0262 | NtQueryLicenseValue      | §23 | T12   | [ ]  |
| 0x0263 | NtGetMUIRegistryInfo     | §8  | T13   | [/]  |
| 0x0264 | NtIsUILanguageComitted   | §8  | T13   | [x]  |
| 0x0265 | NtFlushInstallUILanguage | §8  | T13   | [/]  |

**0x0270–0x027F: Plug and Play**

| Index  | Function           | §   | Owner                   | Done |
| ------ | ------------------ | --- | ----------------------- | ---- |
| 0x0270 | NtPlugPlayControl  | §23 | T01-drv §1 (device mgr) | [ ]  |
| 0x0271 | NtGetPlugPlayEvent | §23 | T01-drv §1              | [ ]  |
| 0x0272 | NtSerializeBoot    | §23 | T12                     | [ ]  |

**0x0280–0x029F: I/O Ring (Fast Async I/O -- Win11+)**

| Index  | Function                  | §   | Owner | Done |
| ------ | ------------------------- | --- | ----- | ---- |
| 0x0280 | NtCreateIoRing            | §13 | T12   | [ ]  |
| 0x0281 | NtSubmitIoRing            | §13 | T12   | [ ]  |
| 0x0282 | NtQueryIoRingCapabilities | §13 | T12   | [ ]  |
| 0x0283 | NtSetInformationIoRing    | §13 | T12   | [ ]  |
| 0x0284 | NtCloseIoRing             | §13 | T12   | [ ]  |

**0x02A0–0x02BF: Security Extensions (AppContainer, Signing)**

| Index  | Function                                 | §   | Owner                 | Done |
| ------ | ---------------------------------------- | --- | --------------------- | ---- |
| 0x02A0 | NtCreateLowBoxToken                      | §16 | T15                   | [ ]  |
| 0x02A1 | NtQuerySecurityPolicy                    | §16 | T15                   | [ ]  |
| 0x02A2 | NtSetCachedSigningLevel                  | §16 | T10 (security harden) | [ ]  |
| 0x02A3 | NtGetCachedSigningLevel                  | §16 | T10                   | [ ]  |
| 0x02A4 | NtCompareSigningLevels                   | §16 | T10                   | [ ]  |
| 0x02A5 | NtSetInformationSymbolicLink             | §17 | T12                   | [ ]  |
| 0x02A6 | NtQuerySecurityAttributesToken           | §16 | T15                   | [ ]  |
| 0x02A7 | NtAccessCheckByTypeAndAuditAlarm         | §16 | T15                   | [ ]  |
| 0x02A8 | NtAccessCheckByTypeResultListAndAuditAlarm | §16 | T15                   | [ ]  |

**0x02C0–0x02DF: Object and Namespace Extensions**

| Index  | Function                  | §   | Owner | Done |
| ------ | ------------------------- | --- | ----- | ---- |
| 0x02C0 | NtCreateDirectoryObjectEx | §17 | T12   | [ ]  |
| 0x02C1 | NtQueryDirectoryFileEx    | §13 | T12   | [ ]  |
| 0x02C2 | NtCreatePrivateNamespace  | §17 | T12   | [ ]  |
| 0x02C3 | NtOpenPrivateNamespace    | §17 | T12   | [ ]  |
| 0x02C4 | NtDeletePrivateNamespace  | §17 | T12   | [ ]  |

**0x02E0–0x02FF: Debug and Filter Extensions**

| Index  | Function                | §   | Owner   | Done |
| ------ | ----------------------- | --- | ------- | ---- |
| 0x02E0 | NtSystemDebugControl    | §21 | T29 §13 | [ ]  |
| 0x02E1 | NtQueryDebugFilterState | §21 | T29 §13 | [ ]  |
| 0x02E2 | NtSetDebugFilterState   | §21 | T29 §13 | [ ]  |

**0x0300–0x034F: Miscellaneous / Extended APIs**

| Index  | Function                                 | §   | Owner            | Done |
| ------ | ---------------------------------------- | --- | ---------------- | ---- |
| 0x0300 | NtCallbackReturn                         | §23 | T12              | [ ]  |
| 0x0301 | NtSetLdtEntries                          | §23 | T12 (x86 compat) | [ ]  |
| 0x0302 | NtQueryOpenSubKeysEx                     | §14 | T14 §4           | [ ]  |
| 0x0303 | NtMapCMFModule                           | §23 | T12              | [ ]  |
| 0x0304 | NtCancelSynchronousIoFile                | §13 | T12              | [ ]  |
| 0x0305 | NtSetTimer2                              | §19 | T12 §19          | [ ]  |
| 0x0306 | NtCancelTimer2                           | §19 | T12 §19          | [ ]  |
| 0x0307 | NtCreateResourceManager                  | §23 | T14              | [ ]  |
| 0x0308 | NtApphelpCacheControl                    | §23 | T12              | [ ]  |
| 0x0309 | NtRaiseStatus                            | §23 | T12              | [ ]  |
| 0x030A | NtFlushKey                               | §14 | T14 §4           | [ ]  |
| 0x030B | NtWaitForAlertByThreadId                 | §8  | T08-mem §4       | [ ]  |
| 0x030C | NtAlertThreadByThreadId                  | §8  | T08-mem §4       | [ ]  |
| 0x030D | NtQueryAuxiliaryCounterFrequency         | §10 | T12              | [ ]  |
| 0x030E | NtConvertBetweenAuxiliaryCounterAndPerformanceCounter | §10 | T12              | [ ]  |
| 0x030F | NtManagePartition                        | §23 | T12              | [ ]  |
| 0x0310 | NtCreatePartition                        | §23 | T12              | [ ]  |
| 0x0311 | NtOpenPartition                          | §23 | T12              | [ ]  |
| 0x0312 | NtManageHotPatch                         | §23 | T12              | [ ]  |
| 0x0313 | NtQuerySystemInformationEx               | §10 | T12              | [ ]  |
| 0x0314 | NtCreateTokenEx                          | §16 | T15              | [ ]  |
| 0x0315 | NtCompareObjects                         | §17 | T12              | [ ]  |
| 0x0316 | NtQueryInformationByName                 | §13 | T12              | [ ]  |
| 0x0317 | NtCancelWaitCompletionPacket             | §13 | T12              | [ ]  |
| 0x0318 | NtAssociateWaitCompletionPacket          | §13 | T12              | [ ]  |
| 0x0319 | NtCreateWaitCompletionPacket             | §13 | T12              | [ ]  |
| 0x031A | NtDirectGraphicsCall                     | §23 | T17-gfx (GPU)    | [ ]  |
| 0x031B | NtSetWnfProcessNotificationEvent         | §23 | T12              | [ ]  |
| 0x031C | NtCopyFileChunk                          | §13 | T12              | [ ]  |
| 0x031D | NtCreateCrossVmEvent                     | §8  | T12              | [ ]  |
| 0x031E | NtCreateCrossVmMutant                    | §8  | T12              | [ ]  |
| 0x031F | NtAcquireCrossVmMutant                   | §8  | T12              | [ ]  |
| 0x0320 | NtQueryInformationEnlistment             | §23 | T14              | [ ]  |
| 0x0321 | NtSetInformationEnlistment               | §23 | T14              | [ ]  |
| 0x0322 | NtQueryInformationResourceManager        | §23 | T14              | [ ]  |
| 0x0323 | NtSetInformationResourceManager          | §23 | T14              | [ ]  |
| 0x0324 | NtQueryInformationTransactionManager     | §23 | T14              | [ ]  |
| 0x0325 | NtSetInformationTransactionManager       | §23 | T14              | [ ]  |

**0x0340–0x037F: Extended File and Volume Operations**

| Index  | Function                      | §   | Owner | Done |
| ------ | ----------------------------- | --- | ----- | ---- |
| 0x0340 | NtQueryQuotaInformationFile   | §13 | T12   | [ ]  |
| 0x0341 | NtSetQuotaInformationFile     | §13 | T12   | [ ]  |
| 0x0342 | NtQueryOleDirectoryFile       | §13 | T12   | [ ]  |
| 0x0343 | NtCancelIoFileEx              | §13 | T12   | [ ]  |
| 0x0344 | NtSetVolumeInformationFile    | §13 | T12   | [ ]  |
| 0x0345 | NtSetEaFile                   | §13 | T12   | [ ]  |
| 0x0346 | NtQueryEaFile                 | §13 | T12   | [ ]  |
| 0x0347 | NtCreateToken                 | §16 | T15   | [ ]  |
| 0x0348 | NtFilterToken                 | §16 | T15   | [ ]  |
| 0x0349 | NtCompareTokens               | §16 | T15   | [ ]  |
| 0x034A | NtAccessCheckByTypeResultList | §16 | T15   | [ ]  |
| 0x034B | NtOpenObjectAuditAlarm        | §16 | T15   | [ ]  |
| 0x034C | NtCloseObjectAuditAlarm       | §16 | T15   | [ ]  |
| 0x034D | NtDeleteObjectAuditAlarm      | §16 | T15   | [ ]  |
| 0x034E | NtPrivilegedServiceAuditAlarm | §16 | T15   | [ ]  |
| 0x034F | NtSetContextChannel           | §23 | T12   | [ ]  |

**0x0380–0x03BF: Extended Thread, Memory, and Misc**

| Index  | Function                      | §   | Owner         | Done |
| ------ | ----------------------------- | --- | ------------- | ---- |
| 0x0380 | NtQueueApcThreadEx            | §7  | T07 §11 (APC) | [ ]  |
| 0x0381 | NtQueueApcThreadEx2           | §7  | T07 §11       | [ ]  |
| 0x0382 | NtSetIoCompletionEx           | §13 | T17-mem §4    | [ ]  |
| 0x0383 | NtRemoveIoCompletionEx        | §13 | T17-mem §4    | [ ]  |
| 0x0384 | NtAlertThreadByThreadIdEx     | §8  | T08-mem §4    | [ ]  |
| 0x0385 | NtWaitForAlertByThreadIdEx    | §8  | T08-mem §4    | [ ]  |
| 0x0386 | NtMapViewOfSection3           | §18 | T12           | [ ]  |
| 0x0387 | NtUnmapViewOfSection2         | §18 | T12           | [ ]  |
| 0x0388 | NtCreateSemaphoreEx           | §8  | T12           | [ ]  |
| 0x0389 | NtCreateMutantEx              | §8  | T12           | [ ]  |
| 0x038A | NtCreateEventEx               | §8  | T12           | [ ]  |
| 0x038B | NtOpenKeyedEvent2             | §8  | T08-mem §10   | [ ]  |
| 0x038C | NtCreateTimerEx               | §19 | T12 §19       | [ ]  |
| 0x038D | NtQueryTimerEx                | §19 | T12 §19       | [ ]  |
| 0x038E | NtSetTimer2                   | §19 | T12 §19       | [ ]  |
| 0x038F | NtCancelTimer2                | §19 | T12 §19       | [ ]  |
| 0x0390 | NtOpenProcessEx               | §7  | T21 §4        | [ ]  |
| 0x0391 | NtOpenThreadEx                | §7  | T21 §4        | [ ]  |
| 0x0392 | NtQueryInformationJobObject   | §7  | T21 §13       | [ ]  |
| 0x0393 | NtSetInformationJobObject     | §7  | T21 §13       | [ ]  |
| 0x0394 | NtQueryDirectoryObjectEx      | §17 | T12           | [ ]  |
| 0x0395 | NtQuerySymbolicLinkObjectEx   | §17 | T12           | [ ]  |
| 0x0396 | NtSetSecurityObjectEx         | §16 | T15           | [ ]  |
| 0x0397 | NtQuerySecurityObjectEx       | §16 | T15           | [ ]  |
| 0x0398 | NtCreateNamedPipeFileEx       | §6  | T17-mem §1    | [ ]  |
| 0x0399 | NtCreateMailslotFileEx        | §13 | T17-mem §3    | [ ]  |
| 0x039A | NtNotifyChangeDirectoryFileEx | §13 | T12           | [ ]  |
| 0x039B | NtSetInformationProcessEx     | §7  | T21 §4        | [ ]  |
| 0x039C | NtQueryInformationProcessEx   | §10 | T12           | [ ]  |
| 0x039D | NtQueryInformationThreadEx    | §7  | T12           | [ ]  |

**0x03C0–0x03DF: Impossible OS Exclusive Extensions**

| Index  | Function                    | §   | Owner                  | Done |
| ------ | --------------------------- | --- | ---------------------- | ---- |
| 0x03C0 | NtQueryKernelModuleInfo     | §10 | T12                    | [ ]  |
| 0x03C1 | NtQueryBootConfiguration    | §10 | T12                    | [ ]  |
| 0x03C2 | NtQueryPmmStatistics        | §10 | T12                    | [ ]  |
| 0x03C3 | NtQueryHeapStatistics       | §10 | T12                    | [ ]  |
| 0x03C4 | NtQuerySchedulerStatistics  | §10 | T12 (sched stats)      | [ ]  |
| 0x03C5 | NtQueryInterruptStatistics  | §10 | T12                    | [ ]  |
| 0x03C6 | NtQueryPciDeviceList        | §10 | T12                    | [ ]  |
| 0x03C7 | NtQueryUsbDeviceList        | §10 | T12                    | [ ]  |
| 0x03C8 | NtQueryNvmeNamespaceList    | §10 | T12                    | [ ]  |
| 0x03C9 | NtQueryNetworkInterfaceList | §10 | T12                    | [ ]  |
| 0x03CA | NtQueryPostCodeHistory      | §10 | T12 (boot diagnostics) | [ ]  |
| 0x03CB | NtQueryObNamespaceTree      | §17 | T12                    | [ ]  |
| 0x03CC | NtQueryRegistryStatistics   | §14 | T14                    | [ ]  |
| 0x03CD | NtQueryVfsStatistics        | §13 | T12                    | [ ]  |
| 0x03CE | NtQuerySmpCpuInfo           | §10 | T12 (per-CPU info)     | [ ]  |
| 0x03CF | NtQueryKlogRingBuffer       | §10 | T12 (serial log)       | [ ]  |
| 0x03D0 | NtSetKlogLevel              | §10 | T12                    | [ ]  |
| 0x03D1 | NtQueryCompositorStatistics | §10 | T12 (desktop stats)    | [ ]  |
| 0x03D2 | NtQueryTimerCalibration     | §19 | T08 §8                 | [ ]  |
| 0x03D3 | NtQueryAcpiTables           | §10 | T12                    | [ ]  |
| 0x03D4 | NtCreateHardLink            | §13 | T12                    | [ ]  |
| 0x03D5 | NtQueryHardLinks            | §13 | T12                    | [ ]  |
| 0x03D6 | NtQueryDriverList           | §10 | T12 (loaded drivers)   | [ ]  |
| 0x03D7 | NtQueryTaskList             | §10 | T12 (sched tasks)      | [ ]  |
| 0x03D8 | NtGetRandom                 | --  | T03 §5 kernel CSPRNG   | [x]  |
| 0x03D9 | NtSetCurrentDirectory       | §1  | T21 (per-process cwd)  | [x]  |
| 0x03DA | NtQueryCurrentDirectory     | §1  | T21 (per-process cwd)  | [x]  |
| 0x03DB | NtPledge                    | §12 | T21 (pledge)           | [x]  |
| 0x03DC | NtUnveil                    | §12 | T21 (unveil)           | [x]  |
| 0x03DD | NtQueryEnvironmentVariable  | §5  | T22 (per-process env)  | [x]  |
| 0x03DE | NtSetEnvironmentVariable    | §5  | T22 (per-process env)  | [x]  |

> **Total: 477 service entries** across 30 functional ranges -- full Windows 11 parity plus Impossible OS exclusive extensions. Shadow SSDT (Win32k) has a separate index space starting at 0x1000.
>
> **Implementation progress: 146/477 wired** (30.6%) -- **126 complete `[x]`** (26.4%) + **20 partial/stub `[/]`** (4.2%). Complete ranges: ETW (7/7), Time/Timer (5/5), Registry CRUD + advanced (19/27), File I/O (15/28 with 6 partial deferrals for IRP/async), Process+Thread (15/24 with 5 stubs), Memory (11/19 with 3 AWE stubs), Sync (17/30 with 4 keyed-event stubs), Token open/query/adjust (9/21 -- §16 complete), Namespace (6/6 -- §17 complete: directory + symlink). Run `/audit-ssdt` to refresh.

**Test checkpoint:** `ssdt_dispatch(0x0FFF)` (unregistered MAIN slot) returns `STATUS_NOT_IMPLEMENTED`; `ssdt_dispatch(0x2000)` (invalid table id) returns `STATUS_INVALID_PARAMETER`; `ssdt_dispatch(valid_index)` calls the correct handler. Serial at init: `"SSDT initialized: %u main slots (last=...)"`.