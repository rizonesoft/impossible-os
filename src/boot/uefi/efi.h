/* ============================================================================
 * efi.h — Minimal UEFI type definitions and protocol GUIDs
 *
 * Freestanding — no gnu-efi dependency. Only the types we actually use.
 * Based on UEFI Specification 2.9.
 *
 * All UEFI function pointers use the Microsoft x64 calling convention
 * (__attribute__((ms_abi))) because UEFI firmware is compiled with MSVC ABI.
 * ============================================================================ */

#ifndef UEFI_EFI_H
#define UEFI_EFI_H

/* UEFI calling convention — Microsoft x64 ABI (rcx, rdx, r8, r9) */
#define EFIAPI __attribute__((ms_abi))

/* --- Base types --- */
typedef unsigned char       BOOLEAN;
typedef signed char         INT8;
typedef unsigned char       UINT8;
typedef short               INT16;
typedef unsigned short      UINT16;
typedef int                 INT32;
typedef unsigned int        UINT32;
typedef long long           INT64;
typedef unsigned long long  UINT64;
typedef unsigned long long  UINTN;
typedef long long           INTN;
typedef void                VOID;
typedef UINT16              CHAR16;
typedef UINT8               CHAR8;

typedef UINTN               EFI_STATUS;
typedef VOID               *EFI_HANDLE;
typedef VOID               *EFI_EVENT;
typedef UINT64              EFI_PHYSICAL_ADDRESS;
typedef UINT64              EFI_VIRTUAL_ADDRESS;
typedef UINTN               EFI_TPL;

/* --- Status codes --- */
#define EFI_SUCCESS                 0
#define EFI_LOAD_ERROR              (1ULL | (1ULL << 63))
#define EFI_INVALID_PARAMETER       (2ULL | (1ULL << 63))
#define EFI_UNSUPPORTED             (3ULL | (1ULL << 63))
#define EFI_BUFFER_TOO_SMALL        (5ULL | (1ULL << 63))
#define EFI_NOT_FOUND               (14ULL | (1ULL << 63))

#define EFI_ERROR(status) ((INTN)(status) < 0)

/* --- GUID --- */
typedef struct {
    UINT32  Data1;
    UINT16  Data2;
    UINT16  Data3;
    UINT8   Data4[8];
} EFI_GUID;

/* --- Memory --- */
typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32                  Type;
    EFI_PHYSICAL_ADDRESS    PhysicalStart;
    EFI_VIRTUAL_ADDRESS     VirtualStart;
    UINT64                  NumberOfPages;
    UINT64                  Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_PAGE_SIZE 4096

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

/* --- Table Header --- */
typedef struct {
    UINT64  Signature;
    UINT32  Revision;
    UINT32  HeaderSize;
    UINT32  CRC32;
    UINT32  Reserved;
} EFI_TABLE_HEADER;

/* --- Forward declarations --- */
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_BOOT_SERVICES;
struct EFI_RUNTIME_SERVICES;

/* --- Simple Text Output --- */
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16 *String
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This
);

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    VOID                   *Reset;
    EFI_TEXT_STRING          OutputString;
    VOID                   *TestString;
    VOID                   *QueryMode;
    VOID                   *SetMode;
    VOID                   *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN    ClearScreen;
    VOID                   *SetCursorPosition;
    VOID                   *EnableCursor;
    VOID                   *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* --- Configuration Table --- */
typedef struct {
    EFI_GUID    VendorGuid;
    VOID       *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* --- Graphics Output Protocol (GOP) --- */
typedef enum {
    PixelRedGreenBlueReserved,
    PixelBlueGreenRedReserved,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32  RedMask;
    UINT32  GreenMask;
    UINT32  BlueMask;
    UINT32  ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                      Version;
    UINT32                      HorizontalResolution;
    UINT32                      VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT   PixelFormat;
    EFI_PIXEL_BITMASK           PixelInformation;
    UINT32                      PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                                  MaxMode;
    UINT32                                  Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION   *Info;
    UINTN                                   SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                    FrameBufferBase;
    UINTN                                   FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber,
    UINTN *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info
);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber
);

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE  QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE    SetMode;
    VOID                                    *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* --- Simple File System Protocol --- */
struct EFI_FILE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    struct EFI_FILE_PROTOCOL *This,
    struct EFI_FILE_PROTOCOL **NewHandle,
    CHAR16 *FileName,
    UINT64 OpenMode,
    UINT64 Attributes
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(
    struct EFI_FILE_PROTOCOL *This
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    struct EFI_FILE_PROTOCOL *This,
    UINTN *BufferSize,
    VOID *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(
    struct EFI_FILE_PROTOCOL *This,
    EFI_GUID *InformationType,
    UINTN *BufferSize,
    VOID *Buffer
);

typedef struct EFI_FILE_PROTOCOL {
    UINT64              Revision;
    EFI_FILE_OPEN       Open;
    EFI_FILE_CLOSE      Close;
    VOID               *Delete;
    EFI_FILE_READ       Read;
    VOID               *Write;
    VOID               *GetPosition;
    VOID               *SetPosition;
    EFI_FILE_GET_INFO   GetInfo;
    VOID               *SetInfo;
    VOID               *Flush;
} EFI_FILE_PROTOCOL;

typedef struct {
    UINT64  Size;
    UINT64  FileSize;
    UINT64  PhysicalSize;
    /* Time fields omitted — we only need Size/FileSize */
} EFI_FILE_INFO;

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
    struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    struct EFI_FILE_PROTOCOL **Root
);

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64                                          Revision;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME     OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_FILE_MODE_READ  0x0000000000000001ULL

/* --- Boot Services --- */
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    UINT32 *DescriptorVersion
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType,
    UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Memory
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS Memory,
    UINTN Pages
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE PoolType,
    UINTN Size,
    VOID **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    VOID *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    UINTN MapKey
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol,
    VOID *Registration,
    VOID **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    UINTN Timeout,
    UINT64 WatchdogCode,
    UINTN DataSize,
    CHAR16 *WatchdogData
);

typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);

typedef struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER        Hdr;

    /* Task Priority Services */
    VOID                   *RaiseTPL;           /* 0 */
    VOID                   *RestoreTPL;         /* 1 */

    /* Memory Services */
    EFI_ALLOCATE_PAGES      AllocatePages;      /* 2 */
    EFI_FREE_PAGES          FreePages;          /* 3 */
    EFI_GET_MEMORY_MAP      GetMemoryMap;       /* 4 */
    EFI_ALLOCATE_POOL       AllocatePool;       /* 5 */
    EFI_FREE_POOL           FreePool;           /* 6 */

    /* Event & Timer Services */
    VOID                   *CreateEvent;        /* 7 */
    VOID                   *SetTimer;           /* 8 */
    VOID                   *WaitForEvent;       /* 9 */
    VOID                   *SignalEvent;        /* 10 */
    VOID                   *CloseEvent;         /* 11 */
    VOID                   *CheckEvent;         /* 12 */

    /* Protocol Handler Services */
    VOID                   *InstallProtocolInterface;   /* 13 */
    VOID                   *ReinstallProtocolInterface; /* 14 */
    VOID                   *UninstallProtocolInterface; /* 15 */
    VOID                   *HandleProtocol;     /* 16 */
    VOID                   *Reserved;           /* 17 */
    VOID                   *RegisterProtocolNotify; /* 18 */
    VOID                   *LocateHandle;       /* 19 */
    VOID                   *LocateDevicePath;   /* 20 */
    VOID                   *InstallConfigurationTable; /* 21 */

    /* Image Services */
    VOID                   *LoadImage;          /* 22 */
    VOID                   *StartImage;         /* 23 */
    VOID                   *Exit;               /* 24 */
    VOID                   *UnloadImage;        /* 25 */
    EFI_EXIT_BOOT_SERVICES  ExitBootServices;   /* 26 */

    /* Miscellaneous Services */
    VOID                   *GetNextMonotonicCount; /* 27 */
    EFI_STALL               Stall;              /* 28 */
    EFI_SET_WATCHDOG_TIMER  SetWatchdogTimer;   /* 29 */

    /* DriverSupport Services */
    VOID                   *ConnectController;  /* 30 */
    VOID                   *DisconnectController; /* 31 */

    /* Open/Close Protocol Services */
    VOID                   *OpenProtocol;       /* 32 */
    VOID                   *CloseProtocol;      /* 33 */
    VOID                   *OpenProtocolInformation; /* 34 */

    /* Library Services */
    VOID                   *ProtocolsPerHandle; /* 35 */
    VOID                   *LocateHandleBuffer; /* 36 */
    EFI_LOCATE_PROTOCOL     LocateProtocol;     /* 37 */
    VOID                   *InstallMultipleProtocolInterfaces; /* 38 */
    VOID                   *UninstallMultipleProtocolInterfaces; /* 39 */

    /* 32-bit CRC Services */
    VOID                   *CalculateCrc32;     /* 40 */

    /* Misc */
    VOID                   *CopyMem;            /* 41 */
    VOID                   *SetMem;             /* 42 */
    VOID                   *CreateEventEx;      /* 43 */
} EFI_BOOT_SERVICES;

/* --- System Table --- */
typedef struct {
    EFI_TABLE_HEADER                Hdr;
    CHAR16                         *FirmwareVendor;
    UINT32                          FirmwareRevision;
    EFI_HANDLE                      ConsoleInHandle;
    VOID                           *ConIn;
    EFI_HANDLE                      ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                      StandardErrorHandle;
    VOID                           *StdErr;
    VOID                           *RuntimeServices;
    EFI_BOOT_SERVICES              *BootServices;
    UINTN                           NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE        *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* --- Well-known GUIDs --- */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, \
      { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x0964e5b22, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_INFO_ID \
    { 0x09576e92, 0x6d3f, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, \
      { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

#define EFI_ACPI_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, \
      { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

#endif /* UEFI_EFI_H */
