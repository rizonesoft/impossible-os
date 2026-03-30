/* ============================================================================
 * ixfs-fuse-win.c -- Windows WinFsp read-only mount for IXFS partitions
 *
 * Usage:
 *   ixfs-mount.exe I: build\system-disk.img 2
 *   ixfs-mount.exe I: \\.\PhysicalDrive0 2
 *
 * Unmount:
 *   ixfs-mount.exe --unmount I:
 *   Or right-click drive in Explorer -> Eject
 * ============================================================================ */

#ifdef _WIN32

/* WinFsp headers use static_assert (MSVC keyword); C11 has _Static_assert */
#ifndef static_assert
#define static_assert _Static_assert
#endif

/* MinGW compat: include <windows.h> first, then fix _ReadWriteBarrier
 * before WinFsp's fsctl.h tries to redeclare it as a function.
 * MinGW's <intrin.h> defines _ReadWriteBarrier as a 0-arg macro,
 * but fsctl.h line 750 declares: void _ReadWriteBarrier(void);
 * which conflicts. We undef the macro AFTER windows.h loads intrin.h,
 * then provide it as a proper function-like macro. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#undef _ReadWriteBarrier
static __inline__ void _ReadWriteBarrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}

#include <winfsp/winfsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "ixfs-core.h"
#include "ixfs-disk.h"

/* -- File context: stored per open file/directory handle -- */
typedef struct {
    uint32_t ino;
    struct ixfs_inode inode;
    int is_dir;
} IXFS_FILE_CTX;

/* -- Global volume state -- */
static ixfs_vol_t *g_vol;
static SECURITY_DESCRIPTOR g_security_desc;

/* -- Helpers -- */

/* Convert Unix epoch seconds to Windows FILETIME (100-ns intervals since 1601) */
static UINT64 unix_to_wintime(uint32_t epoch)
{
    return ((UINT64)epoch + 11644473600ULL) * 10000000ULL;
}

/* Fill FSP_FSCTL_FILE_INFO from an IXFS inode */
static void fill_file_info(FSP_FSCTL_FILE_INFO *fi, const struct ixfs_inode *inode,
                           uint32_t ino)
{
    memset(fi, 0, sizeof(*fi));
    if (inode->i_mode & IXFS_S_DIR)
        fi->FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
    else
        fi->FileAttributes = FILE_ATTRIBUTE_READONLY;
    fi->FileSize = inode->i_size;
    fi->AllocationSize = (UINT64)inode->i_blocks * IXFS_BLOCK_SIZE;
    fi->CreationTime = unix_to_wintime(inode->i_ctime);
    fi->LastAccessTime = unix_to_wintime(inode->i_atime);
    fi->LastWriteTime = unix_to_wintime(inode->i_mtime);
    fi->ChangeTime = unix_to_wintime(inode->i_mtime);
    fi->IndexNumber = ino;
    fi->HardLinks = 0;
}

/* Resolve a backslash-separated Windows path to an IXFS inode number.
 * Path format: "\" = root, "\Impossible\Fonts" = nested. */
static uint32_t resolve_path(const WCHAR *path, struct ixfs_inode *out_inode)
{
    uint32_t ino = g_vol->sb.s_root_inode;
    struct ixfs_inode inode;
    char component[IXFS_MAX_NAME];
    const WCHAR *p;

    if (ixfs_read_inode(g_vol, ino, &inode) != 0)
        return 0;

    /* Root */
    if (path[0] == L'\\' && path[1] == L'\0') {
        if (out_inode) *out_inode = inode;
        return ino;
    }

    p = path;
    if (*p == L'\\') p++;

    while (*p) {
        /* Extract next path component */
        int i = 0;
        while (*p && *p != L'\\' && i < IXFS_MAX_NAME - 1) {
            /* Convert wide char to narrow (ASCII range) */
            component[i++] = (char)(*p & 0x7F);
            p++;
        }
        component[i] = '\0';
        if (*p == L'\\') p++;

        if (!(inode.i_mode & IXFS_S_DIR))
            return 0;

        ino = ixfs_lookup(g_vol, &inode, component);
        if (ino == 0)
            return 0;

        if (ixfs_read_inode(g_vol, ino, &inode) != 0)
            return 0;
    }

    if (out_inode) *out_inode = inode;
    return ino;
}

/* -- WinFsp callbacks -- */

static NTSTATUS ixfs_GetVolumeInfo(FSP_FILE_SYSTEM *FileSystem,
                                   FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    (void)FileSystem;
    VolumeInfo->TotalSize = g_vol->sb.s_total_blocks * IXFS_BLOCK_SIZE;
    VolumeInfo->FreeSize = g_vol->sb.s_free_blocks * IXFS_BLOCK_SIZE;

    /* Volume label from superblock */
    int len = 0;
    while (len < 31 && g_vol->sb.s_volume_name[len]) {
        VolumeInfo->VolumeLabel[len] = (WCHAR)g_vol->sb.s_volume_name[len];
        len++;
    }
    VolumeInfo->VolumeLabel[len] = L'\0';
    VolumeInfo->VolumeLabelLength = (UINT16)(len * sizeof(WCHAR));

    return STATUS_SUCCESS;
}

static NTSTATUS ixfs_GetSecurityByName(FSP_FILE_SYSTEM *FileSystem,
                                       PWSTR FileName,
                                       PUINT32 PFileAttributes,
                                       PSECURITY_DESCRIPTOR SecurityDescriptor,
                                       SIZE_T *PSecurityDescriptorSize)
{
    struct ixfs_inode inode;
    uint32_t ino;
    DWORD sd_len;

    (void)FileSystem;

    ino = resolve_path(FileName, &inode);
    if (ino == 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    if (PFileAttributes) {
        if (inode.i_mode & IXFS_S_DIR)
            *PFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY;
        else
            *PFileAttributes = FILE_ATTRIBUTE_READONLY;
    }

    if (PSecurityDescriptorSize) {
        sd_len = GetSecurityDescriptorLength(&g_security_desc);
        if (*PSecurityDescriptorSize < sd_len) {
            *PSecurityDescriptorSize = sd_len;
            return STATUS_BUFFER_OVERFLOW;
        }
        *PSecurityDescriptorSize = sd_len;
        if (SecurityDescriptor)
            memcpy(SecurityDescriptor, &g_security_desc, sd_len);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS ixfs_Open(FSP_FILE_SYSTEM *FileSystem,
                          PWSTR FileName,
                          UINT32 CreateOptions,
                          UINT32 GrantedAccess,
                          PVOID *PFileContext,
                          FSP_FSCTL_FILE_INFO *FileInfo)
{
    struct ixfs_inode inode;
    uint32_t ino;
    IXFS_FILE_CTX *ctx;

    (void)FileSystem;
    (void)CreateOptions;
    (void)GrantedAccess;

    ino = resolve_path(FileName, &inode);
    if (ino == 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    ctx = malloc(sizeof(IXFS_FILE_CTX));
    if (!ctx)
        return STATUS_INSUFFICIENT_RESOURCES;

    ctx->ino = ino;
    ctx->inode = inode;
    ctx->is_dir = (inode.i_mode & IXFS_S_DIR) ? 1 : 0;

    fill_file_info(FileInfo, &inode, ino);
    *PFileContext = ctx;

    return STATUS_SUCCESS;
}

static VOID ixfs_Close(FSP_FILE_SYSTEM *FileSystem,
                       PVOID FileContext)
{
    (void)FileSystem;
    free(FileContext);
}

static VOID ixfs_Cleanup(FSP_FILE_SYSTEM *FileSystem,
                         PVOID FileContext,
                         PWSTR FileName,
                         ULONG Flags)
{
    (void)FileSystem;
    (void)FileContext;
    (void)FileName;
    (void)Flags;
}

static NTSTATUS ixfs_Read(FSP_FILE_SYSTEM *FileSystem,
                          PVOID FileContext,
                          PVOID Buffer,
                          UINT64 Offset,
                          ULONG Length,
                          PULONG PBytesTransferred)
{
    IXFS_FILE_CTX *ctx = (IXFS_FILE_CTX *)FileContext;
    int64_t n;

    (void)FileSystem;

    n = ixfs_read_data(g_vol, &ctx->inode, Offset, Buffer, Length);
    if (n < 0)
        return STATUS_DEVICE_NOT_READY;

    *PBytesTransferred = (ULONG)n;
    return STATUS_SUCCESS;
}

/* ReadDirectory callback context */
struct readdir_win_ctx {
    PVOID Buffer;
    ULONG Length;
    PULONG PBytesTransferred;
    PWSTR Marker;
    int past_marker;
};

static int readdir_win_cb(const struct ixfs_dir_entry *de,
                          const struct ixfs_inode *inode, void *user)
{
    struct readdir_win_ctx *rc = (struct readdir_win_ctx *)user;
    WCHAR name_w[IXFS_MAX_NAME];
    int i;
    union {
        UINT8 B[FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) +
                 IXFS_MAX_NAME * sizeof(WCHAR)];
        FSP_FSCTL_DIR_INFO D;
    } entry_buf;
    FSP_FSCTL_DIR_INFO *di = &entry_buf.D;

    /* Convert name to wide chars */
    for (i = 0; i < IXFS_MAX_NAME - 1 && de->d_name[i]; i++)
        name_w[i] = (WCHAR)(unsigned char)de->d_name[i];
    name_w[i] = L'\0';

    /* Skip entries until we pass the Marker */
    if (!rc->past_marker) {
        if (rc->Marker && rc->Marker[0] != L'\0') {
            if (wcscmp(name_w, rc->Marker) <= 0)
                return 0;  /* keep scanning */
        }
        rc->past_marker = 1;
    }

    memset(di, 0, sizeof(entry_buf));
    di->Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) +
                         i * sizeof(WCHAR));
    fill_file_info(&di->FileInfo, inode, de->d_inode);
    memcpy(di->FileNameBuf, name_w, i * sizeof(WCHAR));

    if (!FspFileSystemAddDirInfo(di, rc->Buffer, rc->Length,
                                 rc->PBytesTransferred))
        return 1;  /* buffer full, stop */

    return 0;
}

static NTSTATUS ixfs_ReadDirectory(FSP_FILE_SYSTEM *FileSystem,
                                   PVOID FileContext,
                                   PWSTR Pattern,
                                   PWSTR Marker,
                                   PVOID Buffer,
                                   ULONG Length,
                                   PULONG PBytesTransferred)
{
    IXFS_FILE_CTX *ctx = (IXFS_FILE_CTX *)FileContext;
    struct readdir_win_ctx rc;

    (void)FileSystem;
    (void)Pattern;

    if (!ctx->is_dir)
        return STATUS_NOT_A_DIRECTORY;

    rc.Buffer = Buffer;
    rc.Length = Length;
    rc.PBytesTransferred = PBytesTransferred;
    rc.Marker = Marker;
    rc.past_marker = (Marker == NULL || Marker[0] == L'\0') ? 1 : 0;

    ixfs_readdir(g_vol, &ctx->inode, readdir_win_cb, &rc);

    /* Signal end of directory */
    FspFileSystemAddDirInfo(NULL, Buffer, Length, PBytesTransferred);

    return STATUS_SUCCESS;
}

static NTSTATUS ixfs_GetFileInfo(FSP_FILE_SYSTEM *FileSystem,
                                 PVOID FileContext,
                                 FSP_FSCTL_FILE_INFO *FileInfo)
{
    IXFS_FILE_CTX *ctx = (IXFS_FILE_CTX *)FileContext;

    (void)FileSystem;

    fill_file_info(FileInfo, &ctx->inode, ctx->ino);
    return STATUS_SUCCESS;
}

static FSP_FILE_SYSTEM_INTERFACE ixfs_winfsp_interface = {
    .GetVolumeInfo = ixfs_GetVolumeInfo,
    .GetSecurityByName = ixfs_GetSecurityByName,
    .Open = ixfs_Open,
    .Close = ixfs_Close,
    .Cleanup = ixfs_Cleanup,
    .Read = ixfs_Read,
    .ReadDirectory = ixfs_ReadDirectory,
    .GetFileInfo = ixfs_GetFileInfo,
};

/* -- Main -- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s <drive:> <image> <partition>\n"
        "  %s --unmount <drive:>\n"
        "\n"
        "Examples:\n"
        "  %s I: build\\system-disk.img 2\n"
        "  %s --unmount I:\n",
        prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    FSP_FILE_SYSTEM *fs = NULL;
    FSP_FSCTL_VOLUME_PARAMS vol_params;
    NTSTATUS result;
    WCHAR mount_point[8];
    ixfs_disk_ctx_t *disk;
    int part_idx;

    if (argc >= 3 && strcmp(argv[1], "--unmount") == 0) {
        /* Unmount mode: just remove the mount point */
        WCHAR mp[8];
        mbstowcs(mp, argv[2], 8);
        /* WinFsp doesn't have a standalone unmount -- use Windows API */
        if (!DefineDosDeviceW(DDD_REMOVE_DEFINITION, mp, NULL)) {
            fprintf(stderr, "Unmount failed (error %lu)\n", GetLastError());
            return 1;
        }
        printf("Unmounted %s\n", argv[2]);
        return 0;
    }

    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    /* Parse: <drive:> <image> <partition> */
    mbstowcs(mount_point, argv[1], 8);
    part_idx = atoi(argv[3]);

    if (part_idx < 1) {
        fprintf(stderr, "Error: partition index must be >= 1\n");
        return 1;
    }

    /* Open disk and IXFS volume */
    disk = disk_open(argv[2], part_idx);
    if (!disk)
        return 1;

    g_vol = ixfs_open(disk);
    if (!g_vol) {
        disk_close(disk);
        return 1;
    }

    fprintf(stderr, "IXFS: \"%s\" v%u, %llu blocks\n",
            g_vol->sb.s_volume_name,
            g_vol->sb.s_version,
            (unsigned long long)g_vol->sb.s_total_blocks);

    /* Create a basic security descriptor (Everyone: read) */
    InitializeSecurityDescriptor(&g_security_desc, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&g_security_desc, TRUE, NULL, FALSE);

    /* Set up volume parameters */
    memset(&vol_params, 0, sizeof(vol_params));
    vol_params.Version = sizeof(vol_params);
    vol_params.SectorSize = 512;
    vol_params.SectorsPerAllocationUnit = 8;  /* 4096 byte clusters */
    vol_params.MaxComponentLength = IXFS_MAX_NAME - 1;
    vol_params.VolumeCreationTime = unix_to_wintime(g_vol->sb.s_journal_seq);
    vol_params.VolumeSerialNumber = g_vol->sb.s_checksum;
    vol_params.FileInfoTimeout = 1000;
    vol_params.CaseSensitiveSearch = 0;
    vol_params.CasePreservedNames = 1;
    vol_params.UnicodeOnDisk = 0;
    vol_params.PersistentAcls = 0;
    vol_params.ReadOnlyVolume = 1;
    wcscpy(vol_params.FileSystemName, L"IXFS");

    result = FspFileSystemCreate(L"\\\\.\\WinFsp.Disk",
                                 &vol_params, &ixfs_winfsp_interface, &fs);
    if (!NT_SUCCESS(result)) {
        fprintf(stderr, "FspFileSystemCreate failed: 0x%08lX\n", result);
        ixfs_close(g_vol);
        disk_close(disk);
        return 1;
    }

    result = FspFileSystemSetMountPoint(fs, mount_point);
    if (!NT_SUCCESS(result)) {
        fprintf(stderr, "FspFileSystemSetMountPoint(%ls) failed: 0x%08lX\n",
                mount_point, result);
        FspFileSystemDelete(fs);
        ixfs_close(g_vol);
        disk_close(disk);
        return 1;
    }

    result = FspFileSystemStartDispatcher(fs, 0);
    if (!NT_SUCCESS(result)) {
        fprintf(stderr, "FspFileSystemStartDispatcher failed: 0x%08lX\n", result);
        FspFileSystemDelete(fs);
        ixfs_close(g_vol);
        disk_close(disk);
        return 1;
    }

    fprintf(stderr, "Mounted IXFS at %s -- press Ctrl+C to unmount\n", argv[1]);

    /* Block until Ctrl+C */
    Sleep(INFINITE);

    FspFileSystemStopDispatcher(fs);
    FspFileSystemDelete(fs);
    ixfs_close(g_vol);
    disk_close(disk);
    return 0;
}

#endif /* _WIN32 */
