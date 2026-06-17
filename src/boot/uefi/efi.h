/* ============================================================================
 * efi.h -- Minimal UEFI type definitions and protocol GUIDs
 *
 * Freestanding -- no gnu-efi dependency. Only the types we actually use.
 * Based on UEFI Specification 2.9.
 *
 * All UEFI function pointers use the Microsoft x64 calling convention
 * (__attribute__((ms_abi))) because UEFI firmware is compiled with MSVC ABI.
 * ============================================================================ */

#ifndef UEFI_EFI_H
#define UEFI_EFI_H

/* UEFI calling convention -- Microsoft x64 ABI (rcx, rdx, r8, r9) */
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
#define EFI_OUT_OF_RESOURCES        (9ULL | (1ULL << 63))
#define EFI_ACCESS_DENIED           (15ULL | (1ULL << 63))
#define EFI_NOT_FOUND               (14ULL | (1ULL << 63))
#define EFI_SECURITY_VIOLATION      (26ULL | (1ULL << 63))

/* --- ResetSystem types (UEFI 2.10 Table 8-3 / EFI_RESET_TYPE) --- */
#define EFI_RESET_COLD              0
#define EFI_RESET_WARM              1
#define EFI_RESET_SHUTDOWN          2
#define EFI_RESET_PLATFORM_SPECIFIC 3

#define EFI_ERROR(status) ((INTN)(status) < 0)

/* --- Variable attributes (UEFI Spec 2.10 Table 8.2) --- */
#define EFI_VARIABLE_NON_VOLATILE                  0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS            0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS                0x00000004

/* --- Boot error codes (S13: NVRAM persistence) --- */
#define BOOT_ERR_OK                 0x0000
#define BOOT_ERR_ELF_CORRUPT        0x0001  /* S1: ELF bounds check failure */
#define BOOT_ERR_EXIT_BS_FAIL       0x0002  /* S2: ExitBootServices exhausted */
#define BOOT_ERR_KERNEL_NOT_FOUND   0x0003  /* S3: kernel.exe not on any volume */
#define BOOT_ERR_NO_SERIAL          0x0004  /* S4: no serial port detected */
#define BOOT_ERR_NO_GOP             0x0005  /* S5: no usable GOP mode */
#define BOOT_ERR_MMAP_OVERFLOW      0x0006  /* S6: memory map entry overflow */
#define BOOT_ERR_CONF_INVALID       0x0007  /* S7: boot.conf parse error */
#define BOOT_ERR_ALLOC_FAIL         0x0008  /* S8: kernel allocation failed */
#define BOOT_ERR_MMAP_GEOMETRY      0x0009  /* S12: descriptor geometry invalid */
#define BOOT_ERR_MMAP_GETMAP_FAIL   0x000A  /* GetMemoryMap call failed */
#define BOOT_ERR_WATCHDOG_TIMEOUT   0x000B  /* S11: watchdog timeout */
#define BOOT_ERR_EBS_MMAP_FAIL      0x000C  /* GetMemoryMap failed in EBS retry */
#define BOOT_ERR_BOOT_INFO_RESERVED 0x000D  /* S16: boot_info range already owned by firmware */
#define BOOT_ERR_ESP_TYPE_GUID      0x000E  /* ESP integrity: GPT partition type GUID mismatch */
#define BOOT_ERR_ESP_BPB            0x000F  /* ESP integrity: FAT BPB sanity check failed */
#define BOOT_ERR_ESP_MISSING_FILES  0x0010  /* ESP integrity: required boot files absent */
#define BOOT_ERR_ROLLBACK_REFUSE    0x0011  /* anti-rollback refusal: shipped < required */
#define BOOT_ERR_UKI_PAYLOAD        0x0012  /* UKI signed-payload copy/allocate failed */
/* UKI mode rejects disk-side initrd= / module= / recovery_image= cmdline override */
#define BOOT_ERR_UKI_DISK_OVERRIDE  0x0013
/* F10 firmware-setup ResetSystem returned (UEFI 2.10 spec violation; OsIndications was already written) */
#define BOOT_ERR_FW_SETUP_RESET_RET 0x0014

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
    EfiUnacceptedMemoryType,    /* UEFI 2.9+: confidential-computing accept-required pages */
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32                  Type;
    EFI_PHYSICAL_ADDRESS    PhysicalStart;
    EFI_VIRTUAL_ADDRESS     VirtualStart;
    UINT64                  NumberOfPages;
    UINT64                  Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_PAGE_SIZE           4096
#define EFI_MEMORY_RUNTIME      0x8000000000000000ULL  /* Survives ExitBootServices */

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

/* --- EFI_RUNTIME_SERVICES --- */
typedef EFI_STATUS (EFIAPI *EFI_GET_TIME)(
    VOID *Time, VOID *Capabilities);
typedef EFI_STATUS (EFIAPI *EFI_SET_TIME)(
    VOID *Time);
typedef EFI_STATUS (EFIAPI *EFI_GET_WAKEUP_TIME)(
    BOOLEAN *Enabled, BOOLEAN *Pending, VOID *Time);
typedef EFI_STATUS (EFIAPI *EFI_SET_WAKEUP_TIME)(
    BOOLEAN Enable, VOID *Time);
typedef EFI_STATUS (EFIAPI *EFI_SET_VIRTUAL_ADDRESS_MAP)(
    UINTN MemoryMapSize, UINTN DescriptorSize,
    UINT32 DescriptorVersion, EFI_MEMORY_DESCRIPTOR *VirtualMap);
typedef EFI_STATUS (EFIAPI *EFI_CONVERT_POINTER)(
    UINTN DebugDisposition, VOID **Address);
typedef EFI_STATUS (EFIAPI *EFI_GET_VARIABLE)(
    CHAR16 *VariableName, EFI_GUID *VendorGuid,
    UINT32 *Attributes, UINTN *DataSize, VOID *Data);
typedef EFI_STATUS (EFIAPI *EFI_GET_NEXT_VARIABLE_NAME)(
    UINTN *VariableNameSize, CHAR16 *VariableName, EFI_GUID *VendorGuid);
typedef EFI_STATUS (EFIAPI *EFI_SET_VARIABLE)(
    CHAR16 *VariableName, EFI_GUID *VendorGuid,
    UINT32 Attributes, UINTN DataSize, VOID *Data);
typedef VOID (EFIAPI *EFI_RESET_SYSTEM)(
    UINT32 ResetType, EFI_STATUS ResetStatus,
    UINTN DataSize, VOID *ResetData);
typedef EFI_STATUS (EFIAPI *EFI_UPDATE_CAPSULE)(
    VOID **CapsuleHeaderArray, UINTN CapsuleCount, EFI_PHYSICAL_ADDRESS ScatterGatherList);
typedef EFI_STATUS (EFIAPI *EFI_QUERY_CAPSULE_CAPABILITIES)(
    VOID **CapsuleHeaderArray, UINTN CapsuleCount,
    UINT64 *MaximumCapsuleSize, UINT32 *ResetType);
typedef EFI_STATUS (EFIAPI *EFI_QUERY_VARIABLE_INFO)(
    UINT32 Attributes, UINT64 *MaximumVariableStorageSize,
    UINT64 *RemainingVariableStorageSize, UINT64 *MaximumVariableSize);
typedef EFI_STATUS (EFIAPI *EFI_GET_NEXT_HIGH_MONO_COUNT)(
    UINT32 *HighCount);

typedef struct {
    EFI_TABLE_HEADER                Hdr;

    /* Time Services */
    EFI_GET_TIME                    GetTime;
    EFI_SET_TIME                    SetTime;
    EFI_GET_WAKEUP_TIME             GetWakeupTime;
    EFI_SET_WAKEUP_TIME             SetWakeupTime;

    /* Virtual Memory Services */
    EFI_SET_VIRTUAL_ADDRESS_MAP     SetVirtualAddressMap;
    EFI_CONVERT_POINTER             ConvertPointer;

    /* Variable Services */
    EFI_GET_VARIABLE                GetVariable;
    EFI_GET_NEXT_VARIABLE_NAME      GetNextVariableName;
    EFI_SET_VARIABLE                SetVariable;

    /* Miscellaneous Services */
    EFI_GET_NEXT_HIGH_MONO_COUNT    GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM                ResetSystem;

    /* Capsule Services (UEFI 2.0+) */
    EFI_UPDATE_CAPSULE              UpdateCapsule;
    EFI_QUERY_CAPSULE_CAPABILITIES  QueryCapsuleCapabilities;

    /* Variable Info (UEFI 2.0+) */
    EFI_QUERY_VARIABLE_INFO         QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

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
    EFI_STATUS (EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
    EFI_TEXT_CLEAR_SCREEN    ClearScreen;
    VOID                   *SetCursorPosition;
    VOID                   *EnableCursor;
    VOID                   *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* Console text colors */
#define EFI_WHITE       0x0F
#define EFI_LIGHTGRAY   0x07
#define EFI_LIGHTRED    0x0C
#define EFI_YELLOW      0x0E
#define EFI_BLUE        0x01
#define EFI_BACKGROUND_BLUE  0x10
#define EFI_BACKGROUND_RED   0x40

/* --- Simple Text Input Protocol (ConIn) --- */
typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

/* EFI_INPUT_KEY ScanCode constants per UEFI 2.10 spec Table 12.4. Used
 * by the boot menu's navigation. UnicodeChar=='\r' (0x000D) is Enter;
 * ScanCode==0x17 is Esc. Plain ASCII keys come through as ScanCode=0
 * + UnicodeChar=<char>. */
#define EFI_SCAN_NULL          0x0000
#define EFI_SCAN_UP            0x0001
#define EFI_SCAN_DOWN          0x0002
#define EFI_SCAN_RIGHT         0x0003
#define EFI_SCAN_LEFT          0x0004
#define EFI_SCAN_HOME          0x0005
#define EFI_SCAN_END           0x0006
#define EFI_SCAN_INSERT        0x0007
#define EFI_SCAN_DELETE        0x0008
#define EFI_SCAN_PAGE_UP       0x0009
#define EFI_SCAN_PAGE_DOWN     0x000A
#define EFI_SCAN_F1            0x000B
#define EFI_SCAN_F8            0x0012
#define EFI_SCAN_F10           0x0014
#define EFI_SCAN_F11           0x0015
#define EFI_SCAN_ESC           0x0017
#define EFI_CHAR_CR            0x000D
#define EFI_CHAR_LF            0x000A

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
    VOID *WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* --- Simple Text Input Ex Protocol (UEFI 2.10 spec 12.2) ---
 *
 * Required to portably read scan codes >= 0x15 (F11/F12 and beyond).
 * The plain Simple Text Input protocol covers Appendix B Table B-1
 * scan codes 0x00..0x14 only; F11 + F12 are defined in the Ex table
 * exclusively. We use this for the boot-menu F11 force-show probe;
 * F8/F10 + Esc/Enter stay on the plain ConIn path because they fall
 * inside the Simple Text Input scan-code range.
 */
typedef struct {
    UINT32  KeyShiftState;
    UINT8   KeyToggleState;
} EFI_KEY_STATE;
typedef struct {
    EFI_INPUT_KEY  Key;
    EFI_KEY_STATE  KeyState;
} EFI_KEY_DATA;

typedef struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadKeyStrokeEx)(struct EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *KeyData);
    VOID *WaitForKeyEx;
    VOID *SetState;
    VOID *RegisterKeyNotify;
    VOID *UnregisterKeyNotify;
} EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

#define EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID \
    { 0xdd9e7534, 0x7762, 0x4698, { 0x8c, 0x14, 0xf5, 0x85, 0x17, 0xa6, 0x25, 0xaa } }

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

typedef EFI_STATUS (EFIAPI *EFI_FILE_WRITE)(
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

typedef EFI_STATUS (EFIAPI *EFI_FILE_DELETE)(
    struct EFI_FILE_PROTOCOL *This);

typedef struct EFI_FILE_PROTOCOL {
    UINT64              Revision;
    EFI_FILE_OPEN       Open;
    EFI_FILE_CLOSE      Close;
    EFI_FILE_DELETE     Delete;
    EFI_FILE_READ       Read;
    EFI_FILE_WRITE      Write;
    VOID               *GetPosition;
    VOID               *SetPosition;
    EFI_FILE_GET_INFO   GetInfo;
    VOID               *SetInfo;
    EFI_STATUS (EFIAPI *Flush)(struct EFI_FILE_PROTOCOL *This);
} EFI_FILE_PROTOCOL;

/* EFI_TIME per UEFI 2.10 spec section 8.3.1, 16 bytes total. */
typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

/* EFI_FILE_INFO per UEFI 2.10 spec section 13.5. FileName is a
 * variable-length CHAR16 NUL-terminated string immediately after
 * Attribute; treat the whole struct as a header + open-ended FileName
 * (fi->FileName[N]). The caller-provided buffer holds both. */
typedef struct {
    UINT64   Size;
    UINT64   FileSize;
    UINT64   PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64   Attribute;
    CHAR16   FileName[1];  /* variable-length tail */
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

#define EFI_FILE_MODE_READ    0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE   0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE  0x8000000000000000ULL

/* EFI_FILE_INFO.Attribute bits (UEFI 2.10 spec section 13.5). The
 * EFI_FILE_DIRECTORY flag is required as the 4th argument to
 * EFI_FILE_PROTOCOL.Open() when creating a directory. */
#define EFI_FILE_READ_ONLY    0x0000000000000001ULL
#define EFI_FILE_HIDDEN       0x0000000000000002ULL
#define EFI_FILE_SYSTEM       0x0000000000000004ULL
#define EFI_FILE_RESERVED     0x0000000000000008ULL
#define EFI_FILE_DIRECTORY    0x0000000000000010ULL
#define EFI_FILE_ARCHIVE      0x0000000000000020ULL
#define EFI_FILE_VALID_ATTR   0x0000000000000037ULL

/* --- Loaded Image Protocol (identifies the device we booted from) --- */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

typedef struct {
    UINT32            Revision;
    EFI_HANDLE        ParentHandle;
    VOID             *SystemTable;
    EFI_HANDLE        DeviceHandle;     /* Device we booted from */
    VOID             *FilePath;
    VOID             *Reserved;
    UINT32            LoadOptionsSize;
    VOID             *LoadOptions;
    VOID             *ImageBase;
    UINT64            ImageSize;
    UINT32            ImageCodeType;
    UINT32            ImageDataType;
    VOID             *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

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

typedef enum {
    AllHandles,
    ByRegisterNotify,
    ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    EFI_LOCATE_SEARCH_TYPE SearchType,
    EFI_GUID *Protocol,
    VOID *SearchKey,
    UINTN *NoHandles,
    EFI_HANDLE **Buffer);

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
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface); /* 16 */
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
    EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer; /* 36 */
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
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                      ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                      StandardErrorHandle;
    VOID                           *StdErr;
    EFI_RUNTIME_SERVICES           *RuntimeServices;
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

/* SMBIOS 3.x (64-bit) entry point GUID + SMBIOS 2.x (32-bit) entry
 * point GUID per UEFI 2.10 spec section 4.6. Each ConfigurationTable
 * entry with these GUIDs points at the corresponding _SM3_ / _SM_
 * anchor structure. Mirrors UEFI_GUID_SMBIOS3 / UEFI_GUID_SMBIOS in
 * include/kernel/boot_info.h. */
#define EFI_SMBIOS3_TABLE_GUID \
    { 0xf2fd1544, 0x9794, 0x4a2c, \
      { 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94 } }

#define EFI_SMBIOS_TABLE_GUID \
    { 0xeb9d2d31, 0x2d88, 0x11d3, \
      { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

/* --- EFI RNG Protocol (UEFI 2.10 section 37.5) --- */
#define EFI_RNG_PROTOCOL_GUID \
    { 0x3152bca5, 0xeade, 0x433d, \
      { 0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44 } }

typedef EFI_GUID EFI_RNG_ALGORITHM;

typedef struct _EFI_RNG_PROTOCOL EFI_RNG_PROTOCOL;
struct _EFI_RNG_PROTOCOL {
    /* GetInfo(This, &AlgoListSize, AlgoList) -- enumerate algorithms */
    EFI_STATUS (EFIAPI *GetInfo)(EFI_RNG_PROTOCOL *This,
                                 UINTN *RNGAlgorithmListSize,
                                 EFI_RNG_ALGORITHM *RNGAlgorithmList);
    /* GetRNG(This, Algo-or-NULL, ValueLength, Value) -- NULL algorithm
     * selects the firmware default per UEFI 2.10 37.5.2 */
    EFI_STATUS (EFIAPI *GetRNG)(EFI_RNG_PROTOCOL *This,
                                EFI_RNG_ALGORITHM *RNGAlgorithm,
                                UINTN RNGValueLength,
                                UINT8 *RNGValue);
};

/* --- EDID Active Protocol -- provides raw EDID block from firmware --- */
/* Bytes 54–71 of EDID: preferred timing descriptor.
 *   H-active: byte[56] | (byte[58] >> 4) << 8
 *   V-active: byte[59] | (byte[61] >> 4) << 8  */
typedef struct {
    UINT32  SizeOfEdid;   /* in bytes, typically 128 or 256 */
    UINT8  *Edid;         /* pointer to raw EDID data */
} EFI_EDID_ACTIVE_PROTOCOL;

#define EFI_EDID_ACTIVE_PROTOCOL_GUID \
    { 0xbd8c1056, 0x9f36, 0x44ec, \
      { 0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86 } }

/* --- TCG2 Protocol (TPM Measured Boot) --- */

#define EFI_TCG2_PROTOCOL_GUID \
    { 0x607f766c, 0x7455, 0x42be, \
      { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f } }

/* Event log format identifiers */
#define EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2   0x00000001
#define EFI_TCG2_EVENT_LOG_FORMAT_TCG_2     0x00000002  /* Crypto-agile */

/* EFI_TCG2_BOOT_SERVICE_CAPABILITY */
typedef struct {
    UINT8   Size;                  /* sizeof this struct */
    /* UEFI 2.4 Errata B fields */
    UINT8   StructureVersion_Major;
    UINT8   StructureVersion_Minor;
    UINT8   ProtocolVersion_Major;
    UINT8   ProtocolVersion_Minor;
    UINT32  HashAlgorithmBitmap;   /* supported hash algorithms */
    UINT32  SupportedEventLogs;    /* bitmask of supported event log formats */
    BOOLEAN TPMPresentFlag;        /* TRUE if TPM is present */
    UINT16  MaxCommandSize;
    UINT16  MaxResponseSize;
    UINT32  ManufacturerID;
    UINT32  NumberOfPCRBanks;
    UINT32  ActivePCRBanks;        /* bitmask of active PCR banks */
} EFI_TCG2_BOOT_SERVICE_CAPABILITY;

/* Forward-declare the protocol struct for function pointer typedefs */
struct EFI_TCG2_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TCG2_GET_CAPABILITY)(
    struct EFI_TCG2_PROTOCOL *This,
    EFI_TCG2_BOOT_SERVICE_CAPABILITY *ProtocolCapability);

typedef EFI_STATUS (EFIAPI *EFI_TCG2_GET_EVENT_LOG)(
    struct EFI_TCG2_PROTOCOL *This,
    UINT32 EventLogFormat,
    EFI_PHYSICAL_ADDRESS *EventLogLocation,
    EFI_PHYSICAL_ADDRESS *EventLogLastEntry,
    BOOLEAN *EventLogTruncated);

typedef struct EFI_TCG2_PROTOCOL {
    EFI_TCG2_GET_CAPABILITY     GetCapability;
    VOID                        *HashLogExtendEvent; /* not used */
    VOID                        *SubmitCommand;      /* not used */
    EFI_TCG2_GET_EVENT_LOG      GetEventLog;
    VOID                        *HashLogExtendEventEx; /* not used */
} EFI_TCG2_PROTOCOL;

/* --- USB I/O Protocol (per USB device -- available before ExitBootServices) --- */

#define EFI_USB_IO_PROTOCOL_GUID \
    { 0x2B2F68D6, 0x0CD2, 0x44cf, \
      { 0x8E, 0x8B, 0xBB, 0xA2, 0x0B, 0x1B, 0x5B, 0x75 } }

/* USB descriptor structures (UEFI spec representation) */
typedef struct {
    UINT8   Length;
    UINT8   DescriptorType;     /* 0x01 = Device */
    UINT16  BcdUSB;
    UINT8   DeviceClass;
    UINT8   DeviceSubClass;
    UINT8   DeviceProtocol;
    UINT8   MaxPacketSize0;
    UINT16  IdVendor;
    UINT16  IdProduct;
    UINT16  BcdDevice;
    UINT8   StrManufacturer;
    UINT8   StrProduct;
    UINT8   StrSerialNumber;
    UINT8   NumConfigurations;
} EFI_USB_DEVICE_DESCRIPTOR;

typedef struct {
    UINT8   Length;
    UINT8   DescriptorType;     /* 0x04 = Interface */
    UINT8   InterfaceNumber;
    UINT8   AlternateSetting;
    UINT8   NumEndpoints;
    UINT8   InterfaceClass;
    UINT8   InterfaceSubClass;
    UINT8   InterfaceProtocol;
    UINT8   Interface;
} EFI_USB_INTERFACE_DESCRIPTOR;

typedef struct {
    UINT8   Length;
    UINT8   DescriptorType;     /* 0x05 = Endpoint */
    UINT8   EndpointAddress;    /* bit 7 = direction (1=IN) */
    UINT8   Attributes;         /* bits 1:0 = transfer type */
    UINT16  MaxPacketSize;
    UINT8   Interval;
} EFI_USB_ENDPOINT_DESCRIPTOR;

/* Forward-declare for function pointer typedefs */
struct EFI_USB_IO_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_CONTROL_TRANSFER)(
    struct EFI_USB_IO_PROTOCOL *This,
    VOID *Request, UINT32 Direction,
    UINT32 Timeout, VOID *Data, UINTN DataLength, UINT32 *Status);

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_BULK_TRANSFER)(
    struct EFI_USB_IO_PROTOCOL *This,
    UINT8 Endpoint, VOID *Data, UINTN *DataLength,
    UINTN Timeout, UINT32 *Status);

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_GET_DEVICE_DESCRIPTOR)(
    struct EFI_USB_IO_PROTOCOL *This,
    EFI_USB_DEVICE_DESCRIPTOR *DeviceDescriptor);

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_GET_CONFIG_DESCRIPTOR)(
    struct EFI_USB_IO_PROTOCOL *This,
    VOID *ConfigurationDescriptor);

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_GET_INTERFACE_DESCRIPTOR)(
    struct EFI_USB_IO_PROTOCOL *This,
    EFI_USB_INTERFACE_DESCRIPTOR *InterfaceDescriptor);

typedef EFI_STATUS (EFIAPI *EFI_USB_IO_GET_ENDPOINT_DESCRIPTOR)(
    struct EFI_USB_IO_PROTOCOL *This,
    UINT8 EndpointIndex,
    EFI_USB_ENDPOINT_DESCRIPTOR *EndpointDescriptor);

typedef struct EFI_USB_IO_PROTOCOL {
    EFI_USB_IO_CONTROL_TRANSFER        UsbControlTransfer;
    EFI_USB_IO_BULK_TRANSFER           UsbBulkTransfer;
    VOID                              *UsbAsyncInterruptTransfer;
    VOID                              *UsbSyncInterruptTransfer;
    VOID                              *UsbIsochronousTransfer;
    VOID                              *UsbAsyncIsochronousTransfer;
    EFI_USB_IO_GET_DEVICE_DESCRIPTOR   UsbGetDeviceDescriptor;
    EFI_USB_IO_GET_CONFIG_DESCRIPTOR   UsbGetConfigDescriptor;
    EFI_USB_IO_GET_INTERFACE_DESCRIPTOR UsbGetInterfaceDescriptor;
    EFI_USB_IO_GET_ENDPOINT_DESCRIPTOR UsbGetEndpointDescriptor;
    VOID                              *UsbGetStringDescriptor;
    VOID                              *UsbGetSupportedLanguages;
    VOID                              *UsbPortReset;
} EFI_USB_IO_PROTOCOL;

/* --- Block I/O Protocol (for reading MSC disk geometry) --- */

#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT32   MediaId;
    BOOLEAN  RemovableMedia;
    BOOLEAN  MediaPresent;
    BOOLEAN  LogicalPartition;
    BOOLEAN  ReadOnly;
    BOOLEAN  WriteCaching;
    UINT32   BlockSize;
    UINT32   IoAlign;
    UINT64   LastBlock;          /* LBA of last block (block_count = LastBlock + 1) */
} EFI_BLOCK_IO_MEDIA;

struct EFI_BLOCK_IO_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_RESET)(
    struct EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_BLOCK_READ)(
    struct EFI_BLOCK_IO_PROTOCOL *This,
    UINT32 MediaId, UINT64 Lba, UINTN BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_BLOCK_WRITE)(
    struct EFI_BLOCK_IO_PROTOCOL *This,
    UINT32 MediaId, UINT64 Lba, UINTN BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_BLOCK_FLUSH)(
    struct EFI_BLOCK_IO_PROTOCOL *This);

typedef struct EFI_BLOCK_IO_PROTOCOL {
    UINT64                  Revision;
    EFI_BLOCK_IO_MEDIA     *Media;
    EFI_BLOCK_RESET         Reset;
    EFI_BLOCK_READ          ReadBlocks;
    EFI_BLOCK_WRITE         WriteBlocks;
    EFI_BLOCK_FLUSH         FlushBlocks;
} EFI_BLOCK_IO_PROTOCOL;

/* --- Device Path Protocol (boot device path) --- */

typedef struct {
    UINT8  Type;
    UINT8  SubType;
    UINT8  Length[2];   /* little-endian 16-bit length */
} EFI_DEVICE_PATH_PROTOCOL;

/* UEFI Global Variable GUID (§6: BootOrder, BootCurrent, BootNext) */
#define EFI_GLOBAL_VARIABLE_GUID \
    { 0x8BE4DF61, 0x93CA, 0x11D2, \
      { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } }

/* Device path node type/subtype constants (boot device type detection) */
#define EFI_DP_TYPE_MESSAGING    0x03
#define EFI_DP_TYPE_MEDIA        0x04
#define EFI_DP_TYPE_END          0x7F

#define EFI_DP_MSG_SATA          0x12
#define EFI_DP_MSG_NVME          0x17
#define EFI_DP_MSG_USB           0x05
#define EFI_DP_MSG_IPV4          0x0C
#define EFI_DP_MSG_IPV6          0x0D
#define EFI_DP_MSG_SD            0x1A  /* UEFI 2.10 spec 10.3.4.24 -- Secure Digital */
#define EFI_DP_MSG_EMMC          0x1D  /* UEFI 2.10 spec 10.3.4.27 -- eMMC */
/* Messaging subtypes that signal a firmware NETWORK launch (UEFI 2.10 spec
 * 10.3.4). MAC + IPv4/IPv6 (above) appear on PXE paths; URI marks HTTP Boot. */
#define EFI_DP_MSG_MAC           0x0B  /* spec 10.3.4.11 -- MAC Address */
#define EFI_DP_MSG_URI           0x18  /* spec 10.3.4.23 -- URI (HTTP Boot) */
#define EFI_DP_MSG_DNS           0x1F  /* spec 10.3.4.31 -- DNS */

/* Hardware path subtypes (UEFI 2.10 spec 10.3.2) */
#define EFI_DP_TYPE_HW           0x01
#define EFI_DP_HW_PCI            0x01

#define EFI_DP_MEDIA_HARDDRIVE   0x01

#define EFI_DP_SUBTYPE_END_ENTIRE 0xFF

/* --- Device Path To Text Protocol (human-readable device path) --- */

#define EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID \
    { 0x8b843e20, 0x8132, 0x4852, \
      { 0x90, 0xcc, 0x55, 0x1a, 0x4e, 0x4a, 0x7f, 0x1c } }

typedef CHAR16 * (EFIAPI *EFI_DEVICE_PATH_TO_TEXT_NODE)(
    const EFI_DEVICE_PATH_PROTOCOL *DeviceNode,
    BOOLEAN DisplayOnly, BOOLEAN AllowShortcuts);

typedef CHAR16 * (EFIAPI *EFI_DEVICE_PATH_TO_TEXT_PATH)(
    const EFI_DEVICE_PATH_PROTOCOL *DevicePath,
    BOOLEAN DisplayOnly, BOOLEAN AllowShortcuts);

typedef struct {
    EFI_DEVICE_PATH_TO_TEXT_NODE  ConvertDeviceNodeToText;
    EFI_DEVICE_PATH_TO_TEXT_PATH ConvertDevicePathToText;
} EFI_DEVICE_PATH_TO_TEXT_PROTOCOL;

/* --- Device Path Utilities Protocol (optional -- for device path from handle) --- */

#define EFI_DEVICE_PATH_UTILITIES_PROTOCOL_GUID \
    { 0x0379BE4E, 0xD706, 0x437D, \
      { 0xB0, 0x37, 0xED, 0xB8, 0x2F, 0xB7, 0x72, 0xA4 } }

/* GetDevicePathSize validates + measures a device path (END node included),
 * UEFI 2.10 spec 10.2 -- used to bound the network-boot scanner to the real
 * object length instead of a heuristic cap. Only GetDevicePathSize (the first
 * vtable slot) is consumed; the rest are opaque. */
typedef UINTN (EFIAPI *EFI_DEVICE_PATH_UTILS_GET_DEVICE_PATH_SIZE)(
    const EFI_DEVICE_PATH_PROTOCOL *DevicePath);

typedef struct {
    EFI_DEVICE_PATH_UTILS_GET_DEVICE_PATH_SIZE GetDevicePathSize;
    VOID *DuplicateDevicePath;
    VOID *AppendDevicePath;
    VOID *AppendDeviceNode;
    VOID *AppendDevicePathInstance;
    VOID *GetNextDevicePathInstance;
    VOID *IsDevicePathMultiInstance;
    VOID *CreateDeviceNode;
} EFI_DEVICE_PATH_UTILITIES_PROTOCOL;

/* --- Device Path From Handle (HandleProtocol with this GUID) --- */

#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576E91, 0x6D3F, 0x11D2, \
      { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

/* --- Network boot protocols (UEFI 2.10 spec 24) -----------------------------
 * Used by TODO-25 network/PXE/HTTP boot discovery. The Simple Network Protocol
 * layout MUST match the firmware ABI exactly up to the Mode pointer: the
 * interface returned by HandleProtocol is real firmware memory, so a short or
 * misaligned struct would read a function-pointer slot as Mode. All fields
 * through Mode are declared in spec order (callbacks as opaque VOID *) and the
 * _Static_asserts below pin the load-bearing offsets. */

#define EFI_SIMPLE_NETWORK_PROTOCOL_GUID \
    { 0xA19832B9, 0xAC25, 0x11D3, \
      { 0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }

#define EFI_PXE_BASE_CODE_PROTOCOL_GUID \
    { 0x03C4E603, 0xAC28, 0x11D3, \
      { 0x9A, 0x2D, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }

/* 32-byte hardware address container (UEFI 2.10 spec 24.1). Only the first
 * HwAddressSize bytes are significant (6 for Ethernet). */
typedef struct { UINT8 Addr[32]; } EFI_MAC_ADDRESS;

typedef struct {
    UINT32          State;
    UINT32          HwAddressSize;          /* significant MAC bytes */
    UINT32          MediaHeaderSize;
    UINT32          MaxPacketSize;
    UINT32          NvRamSize;
    UINT32          NvRamAccessSize;
    UINT32          ReceiveFilterMask;
    UINT32          ReceiveFilterSetting;
    UINT32          MaxMCastFilterCount;
    UINT32          MCastFilterCount;
    EFI_MAC_ADDRESS MCastFilter[16];
    EFI_MAC_ADDRESS CurrentAddress;
    EFI_MAC_ADDRESS BroadcastAddress;
    EFI_MAC_ADDRESS PermanentAddress;
    UINT8           IfType;
    BOOLEAN         MacAddressChangeable;
    BOOLEAN         MultipleTxSupported;
    BOOLEAN         MediaPresentSupported;  /* link detection is reliable */
    BOOLEAN         MediaPresent;           /* cable/link is up */
} EFI_SIMPLE_NETWORK_MODE;

_Static_assert(__builtin_offsetof(EFI_SIMPLE_NETWORK_MODE, State) == 0,
    "SNP mode State offset");
_Static_assert(__builtin_offsetof(EFI_SIMPLE_NETWORK_MODE, HwAddressSize) == 4,
    "SNP mode HwAddressSize offset");
_Static_assert(__builtin_offsetof(EFI_SIMPLE_NETWORK_MODE, CurrentAddress) == 552,
    "SNP mode CurrentAddress offset (10 u32 + 16 MAC)");
_Static_assert(__builtin_offsetof(EFI_SIMPLE_NETWORK_MODE, MediaPresent) == 652,
    "SNP mode MediaPresent offset");

typedef struct _EFI_SIMPLE_NETWORK_PROTOCOL {
    UINT64                   Revision;
    VOID                    *Start;
    VOID                    *Stop;
    VOID                    *Initialize;
    VOID                    *Reset;
    VOID                    *Shutdown;
    VOID                    *ReceiveFilters;
    VOID                    *StationAddress;
    VOID                    *Statistics;
    VOID                    *MCastIpToMac;
    VOID                    *NvData;
    VOID                    *GetStatus;
    VOID                    *Transmit;
    VOID                    *Receive;
    EFI_EVENT                WaitForPacket;
    EFI_SIMPLE_NETWORK_MODE *Mode;
} EFI_SIMPLE_NETWORK_PROTOCOL;

_Static_assert(__builtin_offsetof(EFI_SIMPLE_NETWORK_PROTOCOL, Mode) == 120,
    "SNP Mode pointer offset (Revision + 13 callbacks + WaitForPacket)");

#endif /* UEFI_EFI_H */
