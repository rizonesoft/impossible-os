/* ============================================================================
 * kusd.h -- KUSER_SHARED_DATA at 0x7FFE0000
 *
 * Windows maps a single physical page at fixed virtual address 0x7FFE0000
 * (user read-only) and a kernel writable alias. User-mode code reads time,
 * tick count, OS version, processor features, and QPC frequency without a
 * syscall. Linux's vDSO serves the same purpose.
 *
 * The struct layout matches the Windows x64 KUSER_SHARED_DATA exactly.
 * _Static_assert offset checks enforce binary compatibility.
 *
 * Reference: Windows SDK ntddk.h, ReactOS ndk/ketypes.h
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- KSYSTEM_TIME -- lock-free 64-bit time for 32-bit readers ----------- */

typedef struct {
    uint32_t LowPart;
    int32_t  High1Time;
    int32_t  High2Time;
} KSYSTEM_TIME;

/* Write a 64-bit value using the triple-write protocol.
 * Writer order: High1Time, LowPart, High2Time.
 * Reader order: read High1Time, LowPart, High2Time; retry if High1!=High2. */
static inline void ksystem_time_write(volatile KSYSTEM_TIME *dst, uint64_t val)
{
    int32_t hi = (int32_t)(val >> 32);
    uint32_t lo = (uint32_t)val;
    dst->High1Time = hi;
    __asm__ volatile ("" ::: "memory");  /* compiler barrier */
    dst->LowPart = lo;
    __asm__ volatile ("" ::: "memory");
    dst->High2Time = hi;
}

/* ---- NT_PRODUCT_TYPE ---------------------------------------------------- */

#define NtProductWinNt      1
#define NtProductLanManNt   2
#define NtProductServer     3

/* ---- Processor Feature indices (PF_*) ----------------------------------- */

#define PF_FLOATING_POINT_PRECISION_ERRATA   0
#define PF_FLOATING_POINT_EMULATED           1
#define PF_COMPARE_EXCHANGE_DOUBLE           2
#define PF_MMX_INSTRUCTIONS_AVAILABLE        3
#define PF_XMMI_INSTRUCTIONS_AVAILABLE       6   /* SSE */
#define PF_3DNOW_INSTRUCTIONS_AVAILABLE      7
#define PF_RDTSC_INSTRUCTION_AVAILABLE       8
#define PF_XMMI64_INSTRUCTIONS_AVAILABLE    10   /* SSE2 */
#define PF_SSE3_INSTRUCTIONS_AVAILABLE      13
#define PF_COMPARE_EXCHANGE128              14
#define PF_SSSE3_INSTRUCTIONS_AVAILABLE     36
#define PF_SSE4_1_INSTRUCTIONS_AVAILABLE    37
#define PF_SSE4_2_INSTRUCTIONS_AVAILABLE    38
#define PF_AVX_INSTRUCTIONS_AVAILABLE       39
#define PF_AVX2_INSTRUCTIONS_AVAILABLE      40
#define PF_RDRAND_INSTRUCTION_AVAILABLE     28

#define PROCESSOR_FEATURE_MAX               64

/* ---- KUSER_SHARED_DATA -------------------------------------------------- */

/* WARNING: Field offsets are ABI-critical. Assembly and user-mode code reads
 * specific byte offsets. Do NOT reorder or resize fields.
 *
 * Offset map (x64):
 *   0x000  TickCountLowDeprecated (uint32_t)
 *   0x004  TickCountMultiplier    (uint32_t)
 *   0x008  InterruptTime          (KSYSTEM_TIME, 12 bytes)
 *   0x014  SystemTime             (KSYSTEM_TIME, 12 bytes)
 *   0x020  TimeZoneBias           (KSYSTEM_TIME, 12 bytes)
 *   0x02C  ImageNumberLow         (uint16_t)
 *   0x02E  ImageNumberHigh        (uint16_t)
 *   0x030  NtSystemRoot[260]      (uint16_t WCHAR, 520 bytes)
 *   0x238  MaxStackTraceDepth     (uint32_t)
 *   0x23C  CryptoExponent         (uint32_t)
 *   0x240  TimeZoneId             (uint32_t)
 *   0x244  LargePageMinimum       (uint32_t)
 *   0x248  AitSamplingValue       (uint32_t)
 *   0x24C  AppCompatFlag          (uint32_t)
 *   0x250  RNGSeedVersion         (uint64_t)
 *   0x258  GlobalValidationRunlevel (uint32_t)
 *   0x25C  TimeZoneBiasStamp      (int32_t)
 *   0x260  NtBuildNumber          (uint32_t)
 *   0x264  NtProductType          (uint32_t)
 *   0x268  ProductTypeIsValid     (uint8_t)
 *   0x269  Reserved0[1]           (uint8_t)
 *   0x26A  NativeProcessorArchitecture (uint16_t)
 *   0x26C  NtMajorVersion         (uint32_t)
 *   0x270  NtMinorVersion         (uint32_t)
 *   0x274  ProcessorFeatures[64]  (uint8_t)
 *   0x2B4  Reserved1              (uint32_t)
 *   0x2B8  Reserved3              (uint32_t)
 *   0x2BC  TimeSlip               (uint32_t)
 *   0x2C0  AlternativeArchitecture (uint32_t)
 *   0x2C4  BootId                 (uint32_t)
 *   0x2C8  SystemExpirationDate   (uint64_t)  -- LARGE_INTEGER
 *   0x2D0  SuiteMask              (uint32_t)
 *   0x2D4  KdDebuggerEnabled      (uint8_t)
 *   0x2D5  MitigationPolicies     (uint8_t)
 *   0x2D6  CyclesPerYield         (uint16_t)
 *   0x2D8  ActiveConsoleId        (uint32_t)
 *   0x2DC  DismountCount          (uint32_t)
 *   0x2E0  ComPlusPackage         (uint32_t)
 *   0x2E4  LastSystemRITEventTickCount (uint32_t)
 *   0x2E8  NumberOfPhysicalPages  (uint32_t)
 *   0x2EC  SafeBootMode           (uint8_t)
 *   0x2ED  VirtualizationFlags    (uint8_t)
 *   0x2EE  Reserved12[2]          (uint8_t)
 *   0x2F0  SharedDataFlags        (uint32_t)
 *   0x2F4  DataFlagsPad[1]        (uint32_t)
 *   0x2F8  TestRetInstruction     (uint64_t)
 *   0x300  QpcFrequency           (int64_t)
 *   0x308  SystemCall             (uint32_t)
 *   0x30C  Reserved2              (uint32_t)
 *   0x310  SystemCallPad[2]       (uint64_t)
 *   0x320  TickCount              (KSYSTEM_TIME) / TickCountQuad (uint64_t)
 *   0x330  Cookie                 (uint32_t)
 *   0x334  CookiePad[1]           (uint32_t)
 *   0x338  ConsoleSessionFgProcessId (int64_t)
 *   ...    (remainder not used yet)
 */

typedef struct __attribute__((packed)) {
    /* 0x000 */ uint32_t     TickCountLowDeprecated;
    /* 0x004 */ uint32_t     TickCountMultiplier;
    /* 0x008 */ KSYSTEM_TIME InterruptTime;
    /* 0x014 */ KSYSTEM_TIME SystemTime;
    /* 0x020 */ KSYSTEM_TIME TimeZoneBias;
    /* 0x02C */ uint16_t     ImageNumberLow;
    /* 0x02E */ uint16_t     ImageNumberHigh;
    /* 0x030 */ uint16_t     NtSystemRoot[260];
    /* 0x238 */ uint32_t     MaxStackTraceDepth;
    /* 0x23C */ uint32_t     CryptoExponent;
    /* 0x240 */ uint32_t     TimeZoneId;
    /* 0x244 */ uint32_t     LargePageMinimum;
    /* 0x248 */ uint32_t     AitSamplingValue;
    /* 0x24C */ uint32_t     AppCompatFlag;
    /* 0x250 */ uint64_t     RNGSeedVersion;
    /* 0x258 */ uint32_t     GlobalValidationRunlevel;
    /* 0x25C */ int32_t      TimeZoneBiasStamp;
    /* 0x260 */ uint32_t     NtBuildNumber;
    /* 0x264 */ uint32_t     NtProductType;
    /* 0x268 */ uint8_t      ProductTypeIsValid;
    /* 0x269 */ uint8_t      Reserved0[1];
    /* 0x26A */ uint16_t     NativeProcessorArchitecture;
    /* 0x26C */ uint32_t     NtMajorVersion;
    /* 0x270 */ uint32_t     NtMinorVersion;
    /* 0x274 */ uint8_t      ProcessorFeatures[PROCESSOR_FEATURE_MAX];
    /* 0x2B4 */ uint32_t     Reserved1;
    /* 0x2B8 */ uint32_t     Reserved3;
    /* 0x2BC */ uint32_t     TimeSlip;
    /* 0x2C0 */ uint32_t     AlternativeArchitecture;
    /* 0x2C4 */ uint32_t     BootId;
    /* 0x2C8 */ uint64_t     SystemExpirationDate;
    /* 0x2D0 */ uint32_t     SuiteMask;
    /* 0x2D4 */ uint8_t      KdDebuggerEnabled;
    /* 0x2D5 */ uint8_t      MitigationPolicies;
    /* 0x2D6 */ uint16_t     CyclesPerYield;
    /* 0x2D8 */ uint32_t     ActiveConsoleId;
    /* 0x2DC */ uint32_t     DismountCount;
    /* 0x2E0 */ uint32_t     ComPlusPackage;
    /* 0x2E4 */ uint32_t     LastSystemRITEventTickCount;
    /* 0x2E8 */ uint32_t     NumberOfPhysicalPages;
    /* 0x2EC */ uint8_t      SafeBootMode;
    /* 0x2ED */ uint8_t      VirtualizationFlags;
    /* 0x2EE */ uint8_t      Reserved12[2];
    /* 0x2F0 */ uint32_t     SharedDataFlags;
    /* 0x2F4 */ uint32_t     DataFlagsPad[1];
    /* 0x2F8 */ uint64_t     TestRetInstruction;
    /* 0x300 */ int64_t      QpcFrequency;
    /* 0x308 */ uint32_t     SystemCall;
    /* 0x30C */ uint32_t     Reserved2;
    /* 0x310 */ uint64_t     SystemCallPad[2];
    /* 0x320 */ union {
                    KSYSTEM_TIME TickCount;
                    uint64_t     TickCountQuad;
                };
    /* 0x330 */ uint32_t     Cookie;
    /* 0x334 */ uint32_t     CookiePad[1];
    /* 0x338 */ int64_t      ConsoleSessionFgProcessId;
    /* 0x340 */ uint8_t      _pad_to_page[0x1000 - 0x340];
} KUSER_SHARED_DATA;

/* ---- Offset checks (binary compatibility with Windows x64) -------------- */

_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, TickCountMultiplier) == 0x004,
    "KUSD: TickCountMultiplier must be at offset 0x004");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, InterruptTime) == 0x008,
    "KUSD: InterruptTime must be at offset 0x008");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, SystemTime) == 0x014,
    "KUSD: SystemTime must be at offset 0x014");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, TimeZoneBias) == 0x020,
    "KUSD: TimeZoneBias must be at offset 0x020");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, NtSystemRoot) == 0x030,
    "KUSD: NtSystemRoot must be at offset 0x030");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, NtBuildNumber) == 0x260,
    "KUSD: NtBuildNumber must be at offset 0x260");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, NtProductType) == 0x264,
    "KUSD: NtProductType must be at offset 0x264");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, NtMajorVersion) == 0x26C,
    "KUSD: NtMajorVersion must be at offset 0x26C");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, NtMinorVersion) == 0x270,
    "KUSD: NtMinorVersion must be at offset 0x270");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, ProcessorFeatures) == 0x274,
    "KUSD: ProcessorFeatures must be at offset 0x274");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, NumberOfPhysicalPages) == 0x2E8,
    "KUSD: NumberOfPhysicalPages must be at offset 0x2E8");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, QpcFrequency) == 0x300,
    "KUSD: QpcFrequency must be at offset 0x300");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, SystemCall) == 0x308,
    "KUSD: SystemCall must be at offset 0x308");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, TickCount) == 0x320,
    "KUSD: TickCount must be at offset 0x320");
_Static_assert(__builtin_offsetof(KUSER_SHARED_DATA, Cookie) == 0x330,
    "KUSD: Cookie must be at offset 0x330");
_Static_assert(sizeof(KUSER_SHARED_DATA) == 0x1000,
    "KUSD: struct must be exactly one page (4096 bytes)");

/* ---- Fixed user-mode virtual address (Windows standard) ----------------- */

#define KUSD_USER_VA   0x7FFE0000ULL

/* ---- API ---------------------------------------------------------------- */

/* Kernel-writable pointer to the KUSD page (cast to KUSER_SHARED_DATA *) */
extern volatile KUSER_SHARED_DATA *g_kusd;

/* Initialize the KUSD page: allocate physical frame, map at user VA
 * (read-only) and kernel alias (read-write). Populates all static fields.
 * Must be called after VMM + PMM. */
void kusd_init(void);

/* Update time fields from timer ISR. Volatile writes only, no locks.
 * Called at CLOCK_LEVEL IRQL from LAPIC/PIT timer handler. */
void kusd_update_time(void);

/* Returns 1 after kusd_init() completes successfully. */
int kusd_ready(void);
