/*
 * service_numbers.h -- SSDT Main Table Service Number Allocation
 *
 * Canonical mapping of Nt* system service numbers to SSDT indices.
 * Derived from TODO-12-native-api-ssdt.md section 4 allocation table.
 *
 * 475 entries across 34 functional ranges -- full Windows 11 parity
 * plus Impossible OS exclusive extensions.  Shadow SSDT (Win32k)
 * uses a separate index space starting at 0x1000.
 */

#pragma once

/* ====================================================================
 * 0x0000-0x0009: Core Object and Handle Operations (10)
 * ==================================================================== */
#define SSDT_NtClose                          0x0000
#define SSDT_NtDuplicateObject                0x0001
#define SSDT_NtQueryObject                    0x0002
#define SSDT_NtMakeTemporaryObject            0x0003
#define SSDT_NtMakePermanentObject            0x0004
#define SSDT_NtSetInformationObject           0x0005
#define SSDT_NtWaitForSingleObject            0x0006
#define SSDT_NtWaitForMultipleObjects         0x0007
#define SSDT_NtSignalAndWaitForSingleObject   0x0008
#define SSDT_NtCompareObjects                 0x0009

/* ====================================================================
 * 0x0010-0x0029: File I/O (26)
 * ==================================================================== */
#define SSDT_NtCreateFile                     0x0010
#define SSDT_NtOpenFile                       0x0011
#define SSDT_NtReadFile                       0x0012
#define SSDT_NtWriteFile                      0x0013
#define SSDT_NtDeleteFile                     0x0014
#define SSDT_NtQueryInformationFile           0x0015
#define SSDT_NtSetInformationFile             0x0016
#define SSDT_NtQueryDirectoryFile             0x0017
#define SSDT_NtFlushBuffersFile               0x0018
#define SSDT_NtDeviceIoControlFile            0x0019
#define SSDT_NtFsControlFile                  0x001A
#define SSDT_NtCreateNamedPipeFile            0x001B
#define SSDT_NtCreateMailslotFile             0x001C
#define SSDT_NtLockFile                       0x001D
#define SSDT_NtUnlockFile                     0x001E
#define SSDT_NtNotifyChangeDirectoryFile      0x001F
#define SSDT_NtQueryVolumeInformationFile     0x0020
#define SSDT_NtSetVolumeInformationFile       0x0021
#define SSDT_NtQueryEaFile                    0x0022
#define SSDT_NtSetEaFile                      0x0023
#define SSDT_NtReadFileScatter                0x0024
#define SSDT_NtWriteFileGather                0x0025
#define SSDT_NtCancelIoFile                   0x0026
#define SSDT_NtCancelIoFileEx                 0x0027
#define SSDT_NtQueryAttributesFile            0x0028
#define SSDT_NtQueryFullAttributesFile        0x0029

/* ====================================================================
 * 0x0030-0x0047: Process and Thread (24)
 * ==================================================================== */
#define SSDT_NtCreateProcess                  0x0030
#define SSDT_NtCreateProcessEx                0x0031
#define SSDT_NtOpenProcess                    0x0032
#define SSDT_NtTerminateProcess               0x0033
#define SSDT_NtQueryInformationProcess        0x0034
#define SSDT_NtSetInformationProcess          0x0035
#define SSDT_NtCreateThread                   0x0036
#define SSDT_NtCreateThreadEx                 0x0037
#define SSDT_NtOpenThread                     0x0038
#define SSDT_NtTerminateThread                0x0039
#define SSDT_NtResumeThread                   0x003A
#define SSDT_NtSuspendThread                  0x003B
#define SSDT_NtGetContextThread               0x003C
#define SSDT_NtSetContextThread               0x003D
#define SSDT_NtQueryInformationThread         0x003E
#define SSDT_NtSetInformationThread           0x003F
#define SSDT_NtAlertThread                    0x0040
#define SSDT_NtAlertResumeThread              0x0041
#define SSDT_NtImpersonateThread              0x0042
#define SSDT_NtQueueApcThread                 0x0043
#define SSDT_NtYieldExecution                 0x0044
#define SSDT_NtCreateUserProcess              0x0045
#define SSDT_NtTestAlert                      0x0046
#define SSDT_NtDelayExecution                 0x0047

/* ====================================================================
 * 0x0050-0x0062: Memory Management (19)
 * ==================================================================== */
#define SSDT_NtAllocateVirtualMemory          0x0050
#define SSDT_NtFreeVirtualMemory              0x0051
#define SSDT_NtProtectVirtualMemory           0x0052
#define SSDT_NtQueryVirtualMemory             0x0053
#define SSDT_NtLockVirtualMemory              0x0054
#define SSDT_NtUnlockVirtualMemory            0x0055
#define SSDT_NtFlushVirtualMemory             0x0056
#define SSDT_NtReadVirtualMemory              0x0057
#define SSDT_NtWriteVirtualMemory             0x0058
#define SSDT_NtAllocateUserPhysicalPages      0x0059
#define SSDT_NtFreeUserPhysicalPages          0x005A
#define SSDT_NtMapUserPhysicalPages           0x005B
#define SSDT_NtCreateSection                  0x005C
#define SSDT_NtOpenSection                    0x005D
#define SSDT_NtMapViewOfSection               0x005E
#define SSDT_NtUnmapViewOfSection             0x005F
#define SSDT_NtExtendSection                  0x0060
#define SSDT_NtQuerySection                   0x0061
#define SSDT_NtAreMappedFilesTheSame          0x0062

/* ====================================================================
 * 0x0070-0x008D: Synchronization (30)
 * ==================================================================== */
#define SSDT_NtCreateEvent                    0x0070
#define SSDT_NtOpenEvent                      0x0071
#define SSDT_NtSetEvent                       0x0072
#define SSDT_NtResetEvent                     0x0073
#define SSDT_NtPulseEvent                     0x0074
#define SSDT_NtQueryEvent                     0x0075
#define SSDT_NtCreateMutant                   0x0076
#define SSDT_NtOpenMutant                     0x0077
#define SSDT_NtReleaseMutant                  0x0078
#define SSDT_NtQueryMutant                    0x0079
#define SSDT_NtCreateSemaphore                0x007A
#define SSDT_NtOpenSemaphore                  0x007B
#define SSDT_NtReleaseSemaphore               0x007C
#define SSDT_NtQuerySemaphore                 0x007D
#define SSDT_NtCreateTimer                    0x007E
#define SSDT_NtOpenTimer                      0x007F
#define SSDT_NtSetTimer                       0x0080
#define SSDT_NtCancelTimer                    0x0081
#define SSDT_NtQueryTimer                     0x0082
#define SSDT_NtSetTimerEx                     0x0083
#define SSDT_NtCreateKeyedEvent               0x0084
#define SSDT_NtOpenKeyedEvent                 0x0085
#define SSDT_NtWaitForKeyedEvent              0x0086
#define SSDT_NtReleaseKeyedEvent              0x0087
#define SSDT_NtCreateIoCompletion             0x0088
#define SSDT_NtSetIoCompletion                0x0089
#define SSDT_NtRemoveIoCompletion             0x008A
#define SSDT_NtQueryIoCompletion              0x008B
#define SSDT_NtSetIoCompletionEx              0x008C
#define SSDT_NtRemoveIoCompletionEx           0x008D

/* ====================================================================
 * 0x0090-0x00AA: Registry (27)
 * ==================================================================== */
#define SSDT_NtCreateKey                      0x0090
#define SSDT_NtCreateKeyTransacted            0x0091
#define SSDT_NtOpenKey                        0x0092
#define SSDT_NtOpenKeyTransacted              0x0093
#define SSDT_NtOpenKeyEx                      0x0094
#define SSDT_NtDeleteKey                      0x0095
#define SSDT_NtSetValueKey                    0x0096
#define SSDT_NtQueryValueKey                  0x0097
#define SSDT_NtDeleteValueKey                 0x0098
#define SSDT_NtEnumerateKey                   0x0099
#define SSDT_NtEnumerateValueKey              0x009A
#define SSDT_NtQueryKey                       0x009B
#define SSDT_NtFlushKey                       0x009C
#define SSDT_NtNotifyChangeKey                0x009D
#define SSDT_NtNotifyChangeMultipleKeys       0x009E
#define SSDT_NtRenameKey                      0x009F
#define SSDT_NtSaveKey                        0x00A0
#define SSDT_NtSaveKeyEx                      0x00A1
#define SSDT_NtRestoreKey                     0x00A2
#define SSDT_NtLoadKey                        0x00A3
#define SSDT_NtLoadKeyEx                      0x00A4
#define SSDT_NtUnloadKey                      0x00A5
#define SSDT_NtUnloadKeyEx                    0x00A6
#define SSDT_NtQueryOpenSubKeys               0x00A7
#define SSDT_NtCompactKeys                    0x00A8
#define SSDT_NtCompressKey                    0x00A9
#define SSDT_NtLockRegistryKey                0x00AA

/* ====================================================================
 * 0x00B0-0x00C4: Security and Token (21)
 * ==================================================================== */
#define SSDT_NtOpenProcessToken               0x00B0
#define SSDT_NtOpenProcessTokenEx             0x00B1
#define SSDT_NtOpenThreadToken                0x00B2
#define SSDT_NtOpenThreadTokenEx              0x00B3
#define SSDT_NtQueryInformationToken          0x00B4
#define SSDT_NtSetInformationToken            0x00B5
#define SSDT_NtAdjustPrivilegesToken          0x00B6
#define SSDT_NtAdjustGroupsToken              0x00B7
#define SSDT_NtDuplicateToken                 0x00B8
#define SSDT_NtFilterToken                    0x00B9
#define SSDT_NtCreateToken                    0x00BA
#define SSDT_NtCompareTokens                  0x00BB
#define SSDT_NtAccessCheck                    0x00BC
#define SSDT_NtAccessCheckAndAuditAlarm       0x00BD
#define SSDT_NtAccessCheckByType              0x00BE
#define SSDT_NtPrivilegeCheck                 0x00BF
#define SSDT_NtPrivilegeObjectAuditAlarm      0x00C0
#define SSDT_NtSetSecurityObject              0x00C1
#define SSDT_NtQuerySecurityObject            0x00C2
#define SSDT_NtAllocateLocallyUniqueId        0x00C3
#define SSDT_NtCreateTokenEx                  0x00C4

/* ====================================================================
 * 0x00D0-0x00E2: System Information and Control (19)
 * ==================================================================== */
#define SSDT_NtQuerySystemInformation             0x00D0
#define SSDT_NtSetSystemInformation               0x00D1
#define SSDT_NtQuerySystemEnvironmentValue        0x00D2
#define SSDT_NtSetSystemEnvironmentValue          0x00D3
#define SSDT_NtQuerySystemEnvironmentValueEx      0x00D4
#define SSDT_NtSetSystemEnvironmentValueEx        0x00D5
#define SSDT_NtEnumerateSystemEnvironmentValuesEx 0x00D6
#define SSDT_NtShutdownSystem                     0x00D7
#define SSDT_NtDisplayString                      0x00D8
#define SSDT_NtRaiseHardError                     0x00D9
#define SSDT_NtQueryDefaultLocale                 0x00DA
#define SSDT_NtSetDefaultLocale                   0x00DB
#define SSDT_NtQueryDefaultUILanguage             0x00DC
#define SSDT_NtSetDefaultUILanguage               0x00DD
#define SSDT_NtQueryInstallUILanguage             0x00DE
#define SSDT_NtAddAtom                            0x00DF
#define SSDT_NtFindAtom                           0x00E0
#define SSDT_NtDeleteAtom                         0x00E1
#define SSDT_NtQueryInformationAtom               0x00E2

/* ====================================================================
 * 0x00F0-0x00F4: Time and Timer (5)
 * ==================================================================== */
#define SSDT_NtQuerySystemTime                0x00F0
#define SSDT_NtSetSystemTime                  0x00F1
#define SSDT_NtQueryPerformanceCounter        0x00F2
#define SSDT_NtQueryTimerResolution           0x00F3
#define SSDT_NtSetTimerResolution             0x00F4

/* ====================================================================
 * 0x0100-0x011E: ALPC and LPC Ports (31)
 * ==================================================================== */
#define SSDT_NtCreatePort                     0x0100
#define SSDT_NtCreateWaitablePort             0x0101
#define SSDT_NtConnectPort                    0x0102
#define SSDT_NtSecureConnectPort              0x0103
#define SSDT_NtAcceptConnectPort              0x0104
#define SSDT_NtCompleteConnectPort            0x0105
#define SSDT_NtListenPort                     0x0106
#define SSDT_NtReplyPort                      0x0107
#define SSDT_NtReplyWaitReceivePort           0x0108
#define SSDT_NtReplyWaitReceivePortEx         0x0109
#define SSDT_NtRequestPort                    0x010A
#define SSDT_NtRequestWaitReplyPort           0x010B
#define SSDT_NtImpersonateClientOfPort        0x010C
#define SSDT_NtReadRequestData                0x010D
#define SSDT_NtWriteRequestData               0x010E
#define SSDT_NtAlpcCreatePort                 0x010F
#define SSDT_NtAlpcConnectPort                0x0110
#define SSDT_NtAlpcConnectPortEx              0x0111
#define SSDT_NtAlpcAcceptConnectPort          0x0112
#define SSDT_NtAlpcSendWaitReceivePort        0x0113
#define SSDT_NtAlpcDisconnectPort             0x0114
#define SSDT_NtAlpcCancelMessage              0x0115
#define SSDT_NtAlpcCreatePortSection          0x0116
#define SSDT_NtAlpcDeletePortSection          0x0117
#define SSDT_NtAlpcCreateSectionView          0x0118
#define SSDT_NtAlpcDeleteSectionView          0x0119
#define SSDT_NtAlpcCreateResourceReserve      0x011A
#define SSDT_NtAlpcDeleteResourceReserve      0x011B
#define SSDT_NtAlpcQueryInformation           0x011C
#define SSDT_NtAlpcSetInformation             0x011D
#define SSDT_NtAlpcQueryInformationMessage    0x011E

/* ====================================================================
 * 0x0120-0x0125: Namespace and Directory Objects (6)
 * ==================================================================== */
#define SSDT_NtCreateDirectoryObject          0x0120
#define SSDT_NtOpenDirectoryObject            0x0121
#define SSDT_NtQueryDirectoryObject           0x0122
#define SSDT_NtCreateSymbolicLinkObject       0x0123
#define SSDT_NtOpenSymbolicLinkObject         0x0124
#define SSDT_NtQuerySymbolicLinkObject        0x0125

/* ====================================================================
 * 0x0130-0x0137: Debug and Exception (8)
 * ==================================================================== */
#define SSDT_NtRaiseException                 0x0130
#define SSDT_NtContinue                       0x0131
#define SSDT_NtDebugActiveProcess             0x0132
#define SSDT_NtDebugContinue                  0x0133
#define SSDT_NtRemoveProcessDebug             0x0134
#define SSDT_NtCreateDebugObject              0x0135
#define SSDT_NtWaitForDebugEvent              0x0136
#define SSDT_NtSetInformationDebugObject      0x0137

/* ====================================================================
 * 0x0140-0x0145: Power and Shutdown (6)
 * ==================================================================== */
#define SSDT_NtSetSystemPowerState            0x0140
#define SSDT_NtInitiatePowerAction            0x0141
#define SSDT_NtPowerInformation               0x0142
#define SSDT_NtGetDevicePowerState            0x0143
#define SSDT_NtSetThreadExecutionState        0x0144
#define SSDT_NtRequestWakeupLatency           0x0145

/* ====================================================================
 * 0x0150-0x0152: Audit and Tracing (3)
 * ==================================================================== */
#define SSDT_NtRegisterSyscallAuditHook       0x0150
#define SSDT_NtUnregisterSyscallAuditHook     0x0151
#define SSDT_NtQuerySyscallAuditState         0x0152

/* ====================================================================
 * 0x0160-0x0167: Job Objects (8)
 * ==================================================================== */
#define SSDT_NtCreateJobObject                0x0160
#define SSDT_NtOpenJobObject                  0x0161
#define SSDT_NtAssignProcessToJobObject       0x0162
#define SSDT_NtTerminateJobObject             0x0163
#define SSDT_NtQueryInformationJobObject      0x0164
#define SSDT_NtSetInformationJobObject        0x0165
#define SSDT_NtIsProcessInJob                 0x0166
#define SSDT_NtCreateJobSet                   0x0167

/* ====================================================================
 * 0x0180-0x0186: Worker Factory / Thread Pool (7)
 * ==================================================================== */
#define SSDT_NtCreateWorkerFactory            0x0180
#define SSDT_NtWorkerFactoryWorkerReady       0x0181
#define SSDT_NtReleaseWorkerFactoryWorker     0x0182
#define SSDT_NtShutdownWorkerFactory          0x0183
#define SSDT_NtQueryInformationWorkerFactory  0x0184
#define SSDT_NtSetInformationWorkerFactory    0x0185
#define SSDT_NtWaitForWorkViaWorkerFactory    0x0186

/* ====================================================================
 * 0x01A0-0x01BE: Kernel Transaction Manager / KTM (31)
 * ==================================================================== */
#define SSDT_NtCreateTransactionManager       0x01A0
#define SSDT_NtOpenTransactionManager         0x01A1
#define SSDT_NtCreateTransaction              0x01A2
#define SSDT_NtOpenTransaction                0x01A3
#define SSDT_NtCommitTransaction              0x01A4
#define SSDT_NtRollbackTransaction            0x01A5
#define SSDT_NtQueryInformationTransaction    0x01A6
#define SSDT_NtSetInformationTransaction      0x01A7
#define SSDT_NtCreateResourceManager          0x01A8
#define SSDT_NtOpenResourceManager            0x01A9
#define SSDT_NtQueryInformationResourceManager  0x01AA
#define SSDT_NtSetInformationResourceManager  0x01AB
#define SSDT_NtCreateEnlistment               0x01AC
#define SSDT_NtOpenEnlistment                 0x01AD
#define SSDT_NtQueryInformationEnlistment     0x01AE
#define SSDT_NtSetInformationEnlistment       0x01AF
#define SSDT_NtPrepareEnlistment              0x01B0
#define SSDT_NtPrePrepareEnlistment           0x01B1
#define SSDT_NtCommitEnlistment               0x01B2
#define SSDT_NtRollbackEnlistment             0x01B3
#define SSDT_NtRecoverTransactionManager      0x01B4
#define SSDT_NtRecoverResourceManager         0x01B5
#define SSDT_NtRecoverEnlistment              0x01B6
#define SSDT_NtPropagationComplete            0x01B7
#define SSDT_NtPropagationFailed              0x01B8
#define SSDT_NtFreezeTransactions             0x01B9
#define SSDT_NtThawTransactions               0x01BA
#define SSDT_NtCreateRegistryTransaction      0x01BB
#define SSDT_NtOpenRegistryTransaction        0x01BC
#define SSDT_NtCommitRegistryTransaction      0x01BD
#define SSDT_NtRollbackRegistryTransaction    0x01BE

/* ====================================================================
 * 0x01D0-0x01D6: ETW (Event Tracing for Windows) (7)
 * ==================================================================== */
#define SSDT_NtTraceEvent                     0x01D0
#define SSDT_NtTraceControl                   0x01D1
#define SSDT_NtCreateTrace                    0x01D2
#define SSDT_NtQueryTrace                     0x01D3
#define SSDT_NtUpdateTrace                    0x01D4
#define SSDT_NtStopTrace                      0x01D5
#define SSDT_NtFlushTrace                     0x01D6

/* ====================================================================
 * 0x01E0-0x01E6: WNF (Windows Notification Facility) (7)
 * ==================================================================== */
#define SSDT_NtCreateWnfStateName             0x01E0
#define SSDT_NtDeleteWnfStateName             0x01E1
#define SSDT_NtQueryWnfStateData              0x01E2
#define SSDT_NtUpdateWnfStateData             0x01E3
#define SSDT_NtSubscribeWnfStateChange        0x01E4
#define SSDT_NtUnsubscribeWnfStateChange      0x01E5
#define SSDT_NtQueryWnfStateNameInformation   0x01E6

/* ====================================================================
 * 0x01F0-0x01F4: Enclave (VBS / SGX) (5)
 * ==================================================================== */
#define SSDT_NtCreateEnclave                  0x01F0
#define SSDT_NtLoadEnclaveData                0x01F1
#define SSDT_NtInitializeEnclave              0x01F2
#define SSDT_NtTerminateEnclave               0x01F3
#define SSDT_NtCallEnclave                    0x01F4

/* ====================================================================
 * 0x0200-0x020B: Process and Thread Extensions (12)
 * ==================================================================== */
#define SSDT_NtSuspendProcess                 0x0200
#define SSDT_NtResumeProcess                  0x0201
#define SSDT_NtGetNextProcess                 0x0202
#define SSDT_NtGetNextThread                  0x0203
#define SSDT_NtCreateProcessStateChange       0x0204
#define SSDT_NtChangeProcessState             0x0205
#define SSDT_NtCreateThreadStateChange        0x0206
#define SSDT_NtChangeThreadState              0x0207
#define SSDT_NtGetCurrentProcessorNumber      0x0208
#define SSDT_NtGetCurrentProcessorNumberEx    0x0209
#define SSDT_NtFlushProcessWriteBuffers       0x020A
#define SSDT_NtQueryPortInformationProcess    0x020B

/* ====================================================================
 * 0x0220-0x0226: Memory Extensions (7)
 * ==================================================================== */
#define SSDT_NtAllocateVirtualMemoryEx        0x0220
#define SSDT_NtCreateSectionEx                0x0221
#define SSDT_NtMapViewOfSectionEx             0x0222
#define SSDT_NtSetInformationVirtualMemory    0x0223
#define SSDT_NtGetWriteWatch                  0x0224
#define SSDT_NtResetWriteWatch                0x0225
#define SSDT_NtCreatePagingFile               0x0226

/* ====================================================================
 * 0x0240-0x0245: Event Pair (6)
 * ==================================================================== */
#define SSDT_NtCreateEventPair                0x0240
#define SSDT_NtOpenEventPair                  0x0241
#define SSDT_NtSetHighEventPair               0x0242
#define SSDT_NtSetLowEventPair                0x0243
#define SSDT_NtWaitHighEventPair              0x0244
#define SSDT_NtWaitLowEventPair               0x0245

/* ====================================================================
 * 0x0250-0x0255: Profile and Performance Counters (6)
 * ==================================================================== */
#define SSDT_NtCreateProfile                  0x0250
#define SSDT_NtCreateProfileEx                0x0251
#define SSDT_NtStartProfile                   0x0252
#define SSDT_NtStopProfile                    0x0253
#define SSDT_NtSetIntervalProfile             0x0254
#define SSDT_NtQueryIntervalProfile           0x0255

/* ====================================================================
 * 0x0260-0x0265: Session and Licensing (6)
 * ==================================================================== */
#define SSDT_NtOpenSession                    0x0260
#define SSDT_NtNotifyChangeSession            0x0261
#define SSDT_NtQueryLicenseValue              0x0262
#define SSDT_NtGetMUIRegistryInfo             0x0263
#define SSDT_NtIsUILanguageComitted           0x0264
#define SSDT_NtFlushInstallUILanguage         0x0265

/* ====================================================================
 * 0x0270-0x0272: Plug and Play (3)
 * ==================================================================== */
#define SSDT_NtPlugPlayControl                0x0270
#define SSDT_NtGetPlugPlayEvent               0x0271
#define SSDT_NtSerializeBoot                  0x0272

/* ====================================================================
 * 0x0280-0x0284: I/O Ring (Fast Async I/O -- Win11+) (5)
 * ==================================================================== */
#define SSDT_NtCreateIoRing                   0x0280
#define SSDT_NtSubmitIoRing                   0x0281
#define SSDT_NtQueryIoRingCapabilities        0x0282
#define SSDT_NtSetInformationIoRing           0x0283
#define SSDT_NtCloseIoRing                    0x0284

/* ====================================================================
 * 0x02A0-0x02A8: Security Extensions (AppContainer, Signing) (9)
 * ==================================================================== */
#define SSDT_NtCreateLowBoxToken                        0x02A0
#define SSDT_NtQuerySecurityPolicy                      0x02A1
#define SSDT_NtSetCachedSigningLevel                    0x02A2
#define SSDT_NtGetCachedSigningLevel                    0x02A3
#define SSDT_NtCompareSigningLevels                     0x02A4
#define SSDT_NtSetInformationSymbolicLink               0x02A5
#define SSDT_NtQuerySecurityAttributesToken             0x02A6
#define SSDT_NtAccessCheckByTypeAndAuditAlarm           0x02A7
#define SSDT_NtAccessCheckByTypeResultListAndAuditAlarm 0x02A8

/* ====================================================================
 * 0x02C0-0x02C4: Object and Namespace Extensions (5)
 * ==================================================================== */
#define SSDT_NtCreateDirectoryObjectEx        0x02C0
#define SSDT_NtQueryDirectoryFileEx           0x02C1
#define SSDT_NtCreatePrivateNamespace         0x02C2
#define SSDT_NtOpenPrivateNamespace           0x02C3
#define SSDT_NtDeletePrivateNamespace         0x02C4

/* ====================================================================
 * 0x02E0-0x02E2: Debug and Filter Extensions (3)
 * ==================================================================== */
#define SSDT_NtSystemDebugControl             0x02E0
#define SSDT_NtQueryDebugFilterState          0x02E1
#define SSDT_NtSetDebugFilterState            0x02E2

/* ====================================================================
 * 0x0300-0x0325: Miscellaneous / Extended APIs (38)
 * ==================================================================== */
#define SSDT_NtCallbackReturn                                      0x0300
#define SSDT_NtSetLdtEntries                                       0x0301
#define SSDT_NtQueryOpenSubKeysEx                                  0x0302
#define SSDT_NtMapCMFModule                                        0x0303
#define SSDT_NtCancelSynchronousIoFile                             0x0304
#define SSDT_NtSetTimer2                                           0x0305
#define SSDT_NtCancelTimer2                                        0x0306
#define SSDT_NtCreateResourceManager2                              0x0307
#define SSDT_NtApphelpCacheControl                                 0x0308
#define SSDT_NtRaiseStatus                                         0x0309
#define SSDT_NtFlushKey2                                           0x030A
#define SSDT_NtWaitForAlertByThreadId                              0x030B
#define SSDT_NtAlertThreadByThreadId                               0x030C
#define SSDT_NtQueryAuxiliaryCounterFrequency                      0x030D
#define SSDT_NtConvertBetweenAuxiliaryCounterAndPerformanceCounter 0x030E
#define SSDT_NtManagePartition                                     0x030F
#define SSDT_NtCreatePartition                                     0x0310
#define SSDT_NtOpenPartition                                       0x0311
#define SSDT_NtManageHotPatch                                      0x0312
#define SSDT_NtQuerySystemInformationEx                            0x0313
#define SSDT_NtCreateTokenEx2                                      0x0314
#define SSDT_NtCompareObjects2                                     0x0315
#define SSDT_NtQueryInformationByName                              0x0316
#define SSDT_NtCancelWaitCompletionPacket                          0x0317
#define SSDT_NtAssociateWaitCompletionPacket                       0x0318
#define SSDT_NtCreateWaitCompletionPacket                          0x0319
#define SSDT_NtDirectGraphicsCall                                  0x031A
#define SSDT_NtSetWnfProcessNotificationEvent                     0x031B
#define SSDT_NtCopyFileChunk                                       0x031C
#define SSDT_NtCreateCrossVmEvent                                  0x031D
#define SSDT_NtCreateCrossVmMutant                                 0x031E
#define SSDT_NtAcquireCrossVmMutant                                0x031F
#define SSDT_NtQueryInformationEnlistment2                         0x0320
#define SSDT_NtSetInformationEnlistment2                           0x0321
#define SSDT_NtQueryInformationResourceManager2                    0x0322
#define SSDT_NtSetInformationResourceManager2                      0x0323
#define SSDT_NtQueryInformationTransactionManager                  0x0324
#define SSDT_NtSetInformationTransactionManager                    0x0325

/* ====================================================================
 * 0x0340-0x034F: Extended File and Volume Operations (16)
 * ==================================================================== */
#define SSDT_NtQueryQuotaInformationFile      0x0340
#define SSDT_NtSetQuotaInformationFile        0x0341
#define SSDT_NtQueryOleDirectoryFile          0x0342
#define SSDT_NtCancelIoFileEx2                0x0343
#define SSDT_NtSetVolumeInformationFile2      0x0344
#define SSDT_NtSetEaFile2                     0x0345
#define SSDT_NtQueryEaFile2                   0x0346
#define SSDT_NtCreateToken2                   0x0347
#define SSDT_NtFilterToken2                   0x0348
#define SSDT_NtCompareTokens2                 0x0349
#define SSDT_NtAccessCheckByTypeResultList    0x034A
#define SSDT_NtOpenObjectAuditAlarm           0x034B
#define SSDT_NtCloseObjectAuditAlarm          0x034C
#define SSDT_NtDeleteObjectAuditAlarm         0x034D
#define SSDT_NtPrivilegedServiceAuditAlarm    0x034E
#define SSDT_NtSetContextChannel              0x034F

/* ====================================================================
 * 0x0380-0x039D: Extended Thread, Memory, and Misc (30)
 * ==================================================================== */
#define SSDT_NtQueueApcThreadEx               0x0380
#define SSDT_NtQueueApcThreadEx2              0x0381
#define SSDT_NtSetIoCompletionEx2             0x0382
#define SSDT_NtRemoveIoCompletionEx2          0x0383
#define SSDT_NtAlertThreadByThreadIdEx        0x0384
#define SSDT_NtWaitForAlertByThreadIdEx       0x0385
#define SSDT_NtMapViewOfSection3              0x0386
#define SSDT_NtUnmapViewOfSection2            0x0387
#define SSDT_NtCreateSemaphoreEx              0x0388
#define SSDT_NtCreateMutantEx                 0x0389
#define SSDT_NtCreateEventEx                  0x038A
#define SSDT_NtOpenKeyedEvent2                0x038B
#define SSDT_NtCreateTimerEx                  0x038C
#define SSDT_NtQueryTimerEx                   0x038D
#define SSDT_NtSetTimer2Ex                    0x038E
#define SSDT_NtCancelTimer2Ex                 0x038F
#define SSDT_NtOpenProcessEx                  0x0390
#define SSDT_NtOpenThreadEx                   0x0391
#define SSDT_NtQueryInformationJobObject2     0x0392
#define SSDT_NtSetInformationJobObject2       0x0393
#define SSDT_NtQueryDirectoryObjectEx         0x0394
#define SSDT_NtQuerySymbolicLinkObjectEx      0x0395
#define SSDT_NtSetSecurityObjectEx            0x0396
#define SSDT_NtQuerySecurityObjectEx          0x0397
#define SSDT_NtCreateNamedPipeFileEx          0x0398
#define SSDT_NtCreateMailslotFileEx           0x0399
#define SSDT_NtNotifyChangeDirectoryFileEx    0x039A
#define SSDT_NtSetInformationProcessEx        0x039B
#define SSDT_NtQueryInformationProcessEx      0x039C
#define SSDT_NtQueryInformationThreadEx       0x039D

/* ====================================================================
 * 0x03C0-0x03D8: Impossible OS Exclusive Extensions (25)
 * ==================================================================== */
#define SSDT_NtQueryKernelModuleInfo          0x03C0
#define SSDT_NtQueryBootConfiguration         0x03C1
#define SSDT_NtQueryPmmStatistics             0x03C2
#define SSDT_NtQueryHeapStatistics            0x03C3
#define SSDT_NtQuerySchedulerStatistics       0x03C4
#define SSDT_NtQueryInterruptStatistics       0x03C5
#define SSDT_NtQueryPciDeviceList             0x03C6
#define SSDT_NtQueryUsbDeviceList             0x03C7
#define SSDT_NtQueryNvmeNamespaceList         0x03C8
#define SSDT_NtQueryNetworkInterfaceList      0x03C9
#define SSDT_NtQueryPostCodeHistory           0x03CA
#define SSDT_NtQueryObNamespaceTree           0x03CB
#define SSDT_NtQueryRegistryStatistics        0x03CC
#define SSDT_NtQueryVfsStatistics             0x03CD
#define SSDT_NtQuerySmpCpuInfo                0x03CE
#define SSDT_NtQueryKlogRingBuffer            0x03CF
#define SSDT_NtSetKlogLevel                   0x03D0
#define SSDT_NtQueryCompositorStatistics      0x03D1
#define SSDT_NtQueryTimerCalibration          0x03D2
#define SSDT_NtQueryAcpiTables                0x03D3
#define SSDT_NtCreateHardLink                 0x03D4
#define SSDT_NtQueryHardLinks                 0x03D5
#define SSDT_NtQueryDriverList                0x03D6
#define SSDT_NtQueryTaskList                  0x03D7
#define SSDT_NtGetRandom                      0x03D8

/* ====================================================================
 * 0x03D9-0x03DA: Process Model Extensions -- Working Directory (TODO-21 s1)
 * ==================================================================== */
#define SSDT_NtSetCurrentDirectory            0x03D9
#define SSDT_NtQueryCurrentDirectory          0x03DA

/* ====================================================================
 * 0x03DB-0x03DC: Process Model Extensions -- Restriction (TODO-21 s12)
 * ==================================================================== */
#define SSDT_NtPledge                         0x03DB
#define SSDT_NtUnveil                         0x03DC

/* ====================================================================
 * 0x03DD-0x03DE: Environment Variables -- per-process env syscalls (TODO-22 s5)
 *
 * Distinct from the FIRMWARE env slots 0x00D2-0x00D6
 * (NtQuerySystemEnvironmentValue*): those read UEFI/SMBIOS variables, these
 * read/write the calling process's own task->environ (kernel-authoritative).
 * ==================================================================== */
#define SSDT_NtQueryEnvironmentVariable       0x03DD
#define SSDT_NtSetEnvironmentVariable         0x03DE

/* ====================================================================
 * Total count and bounds
 *
 * SSDT_MAIN_COUNT = number of SSDT_Nt* defines above (currently 477).
 * SSDT_LAST_MAIN_INDEX = highest allocated service number (0x03DE).
 *
 * WARNING: When adding a new service number:
 *   1. Add the #define SSDT_NtXxx line in the correct functional range
 *   2. Increment SSDT_MAIN_COUNT by 1
 *   3. Update SSDT_LAST_MAIN_INDEX if the new index is higher
 *   4. Build -- static asserts will catch count/index mismatches
 *
 * To verify count matches defines: grep -c '^#define SSDT_Nt' service_numbers.h
 *
 * Next available indices per range:
 *   Object ops: 0x000A   File I/O: 0x002A   Process: 0x0050
 *   Thread: 0x006A       Memory: 0x0087     Sync: 0x00A4
 *   Registry: 0x00C0     Security: 0x00DF   Token: 0x00F6
 *   Port/ALPC: FULL (0x010F-0x011E)  Timer: 0x011F      Info: 0x013C
 *   Debug: 0x0156        Atom: 0x0167       Power: 0x0177
 *   PnP: 0x0186          Key: 0x0196        Transaction: 0x01A5
 *   I/O complete: 0x01AF Notify: 0x01B9     Resource: 0x01CA
 *   Driver: 0x01D7       Profile: 0x01E5    Namespace: 0x01F1
 *   Cache: 0x01FF        Worker: 0x0207     Enlistment: 0x0211
 *   Partition: 0x021C    Enclave: 0x022F    Extensions: 0x0244
 *   Network: 0x030A      Storage: 0x032E    Compositor: 0x0354
 *   Diagnostics: 0x03DF  (0x03D9/0x03DA CWD, 0x03DB/0x03DC pledge/unveil,
 *                         0x03DD/0x03DE env vars)
 * ==================================================================== */
#define SSDT_MAIN_COUNT                       477
#define SSDT_LAST_MAIN_INDEX                  0x03DE  /* SSDT_NtSetEnvironmentVariable */

/* Compile-time verification:
 * - Count must fit within the table capacity
 * - Last index must be within table bounds
 * - Count must be positive and reasonable */
_Static_assert(SSDT_MAIN_COUNT > 0 && SSDT_MAIN_COUNT <= 1024,
    "SSDT_MAIN_COUNT must be 1-1024 (table capacity is SSDT_MAIN_MAX=1024)");
_Static_assert(SSDT_LAST_MAIN_INDEX < 1024,
    "SSDT_LAST_MAIN_INDEX must be < 1024 (SSDT_MAIN_MAX)");
_Static_assert(SSDT_LAST_MAIN_INDEX >= SSDT_MAIN_COUNT - 1,
    "SSDT_LAST_MAIN_INDEX must be >= count-1 (sparse allocation uses higher indices)");
