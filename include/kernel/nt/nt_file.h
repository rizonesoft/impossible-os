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
