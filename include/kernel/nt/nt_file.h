/* ============================================================================
 * nt_file.h -- NT file I/O constants for NtCreateFile / NtOpenFile
 *
 * CreateDisposition, CreateOptions, FileAttributes, and IOSB Information
 * values matching the Windows NT native API convention.
 * ============================================================================ */

#pragma once

/* ---- CreateDisposition (NtCreateFile a5) -------------------------------- */

#define FILE_SUPERSEDE      0   /* if exists: replace; if not: create */
#define FILE_OPEN           1   /* file must exist */
#define FILE_CREATE         2   /* file must NOT exist */
#define FILE_OPEN_IF        3   /* open if exists, create if not */
#define FILE_OVERWRITE      4   /* file must exist, truncate */
#define FILE_OVERWRITE_IF   5   /* truncate if exists, create if not */

/* ---- CreateOptions (NtCreateFile a6 low 16 bits) ------------------------ */

#define FILE_DIRECTORY_FILE             0x00000001
#define FILE_WRITE_THROUGH              0x00000002
#define FILE_SEQUENTIAL_ONLY            0x00000004
#define FILE_NO_INTERMEDIATE_BUFFERING  0x00000008
#define FILE_SYNCHRONOUS_IO_ALERT       0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT    0x00000020
#define FILE_NON_DIRECTORY_FILE         0x00000040
#define FILE_RANDOM_ACCESS              0x00000800
#define FILE_DELETE_ON_CLOSE            0x00001000

/* ---- FileAttributes ----------------------------------------------------- */

#define FILE_ATTRIBUTE_READONLY     0x00000001
#define FILE_ATTRIBUTE_HIDDEN       0x00000002
#define FILE_ATTRIBUTE_SYSTEM       0x00000004
#define FILE_ATTRIBUTE_DIRECTORY    0x00000010
#define FILE_ATTRIBUTE_ARCHIVE      0x00000020
#define FILE_ATTRIBUTE_NORMAL       0x00000080
#define FILE_ATTRIBUTE_TEMPORARY    0x00000100

/* ---- IoStatusBlock.Information values ----------------------------------- */

#define FILE_SUPERSEDED     0
#define FILE_OPENED         1
#define FILE_CREATED        2
#define FILE_OVERWRITTEN    3
#define FILE_EXISTS         4
#define FILE_DOES_NOT_EXIST 5

/* ---- FILE_INFORMATION_CLASS (NtQueryInformationFile / NtSetInformationFile) */

#define FileBasicInformation              4
#define FileStandardInformation           5
#define FileNameInformation               9
#define FileRenameInformation            10
#define FileDispositionInformation       13
#define FilePositionInformation          14
#define FileAllInformation               18
#define FileAllocationInformation        19
#define FileEndOfFileInformation         20
#define FileNetworkOpenInformation       34

/* ---- File information structures ---------------------------------------- */

#include "kernel/nt/filetime.h"

typedef struct {
    FILETIME CreationTime;
    FILETIME LastAccessTime;
    FILETIME LastWriteTime;
    FILETIME ChangeTime;
    uint32_t FileAttributes;
    uint32_t _pad;
} FILE_BASIC_INFORMATION;

typedef struct {
    uint64_t AllocationSize;
    uint64_t EndOfFile;
    uint32_t NumberOfLinks;
    uint8_t  DeletePending;
    uint8_t  Directory;
    uint16_t _pad;
} FILE_STANDARD_INFORMATION;

typedef struct {
    uint32_t FileNameLength;     /* in bytes */
    uint16_t FileName[260];      /* UTF-16LE, not null-terminated */
} FILE_NAME_INFORMATION;

typedef struct {
    uint64_t CurrentByteOffset;
} FILE_POSITION_INFORMATION;

typedef struct {
    uint64_t EndOfFile;
} FILE_END_OF_FILE_INFORMATION;

typedef struct {
    uint64_t AllocationSize;
} FILE_ALLOCATION_INFORMATION;

typedef struct {
    uint8_t DeleteFile;
} FILE_DISPOSITION_INFORMATION;

typedef struct {
    uint8_t  ReplaceIfExists;
    uint8_t  _pad[7];
    uint64_t RootDirectory;      /* 0 = absolute path */
    uint32_t FileNameLength;     /* in bytes */
    uint16_t FileName[260];      /* UTF-16LE */
} FILE_RENAME_INFORMATION;

typedef struct {
    FILETIME CreationTime;
    FILETIME LastAccessTime;
    FILETIME LastWriteTime;
    FILETIME ChangeTime;
    uint64_t AllocationSize;
    uint64_t EndOfFile;
    uint32_t FileAttributes;
    uint32_t _pad;
} FILE_NETWORK_OPEN_INFORMATION;

/* ---- FS_INFORMATION_CLASS (NtQueryVolumeInformationFile) ---------------- */

#define FileFsVolumeInformation        1
#define FileFsSizeInformation          3
#define FileFsAttributeInformation     5

typedef struct {
    uint64_t TotalAllocationUnits;
    uint64_t AvailableAllocationUnits;
    uint32_t SectorsPerAllocationUnit;
    uint32_t BytesPerSector;
} FILE_FS_SIZE_INFORMATION;

typedef struct {
    FILETIME VolumeCreationTime;
    uint32_t VolumeSerialNumber;
    uint32_t VolumeLabelLength;
    uint8_t  SupportsObjects;
    uint8_t  _pad[3];
    uint16_t VolumeLabel[32];    /* UTF-16LE */
} FILE_FS_VOLUME_INFORMATION;

typedef struct {
    uint32_t FileSystemAttributes;
    uint32_t MaximumComponentNameLength;
    uint32_t FileSystemNameLength;
    uint16_t FileSystemName[16]; /* UTF-16LE, e.g., "IXFS" */
} FILE_FS_ATTRIBUTE_INFORMATION;

/* ---- I/O Completion Port ------------------------------------------------ */

typedef struct {
    uint64_t KeyContext;
    uint64_t ApcContext;
    NTSTATUS IoStatus;
    uint32_t _pad;
    uint64_t IoStatusInformation;
} IO_COMPLETION_ENTRY;

#define IOCP_MAX_ENTRIES 64

typedef struct {
    IO_COMPLETION_ENTRY entries[IOCP_MAX_ENTRIES];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} IO_COMPLETION_PORT;

/* ---- DeviceIoControlFile method codes ----------------------------------- */

#define METHOD_BUFFERED    0
#define METHOD_IN_DIRECT   1
#define METHOD_OUT_DIRECT  2
#define METHOD_NEITHER     3

/* Registration for SSDT handlers (called from nt_syscall_register_ssdt) */
void nt_file_register_ssdt(void);
