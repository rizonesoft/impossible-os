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

#include <winfsp/winfsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <sddl.h>

#include "ixfs-core.h"
#include "ixfs-disk.h"

/* -- File context: stored per open file/directory handle -- */
typedef struct {
    uint32_t ino;
    uint32_t parent_ino;
    struct ixfs_inode inode;
    int is_dir;
    char name[IXFS_MAX_NAME];    /* filename for Cleanup delete */
} IXFS_FILE_CTX;

/* -- Global volume state -- */
static ixfs_vol_t *g_vol;
static PSECURITY_DESCRIPTOR g_security_desc;
static DWORD g_security_desc_len;

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
        fi->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    else
        fi->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (!g_vol->bitmap)
        fi->FileAttributes |= FILE_ATTRIBUTE_READONLY;
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

/* Split a path into parent path + filename.
 * parent_out must be at least 1024 bytes. Returns pointer to name within path. */
static const WCHAR *split_path(const WCHAR *path, WCHAR *parent_out)
{
    const WCHAR *last_sep = wcsrchr(path, L'\\');
    if (!last_sep || last_sep == path) {
        /* Root or single component */
        parent_out[0] = L'\\';
        parent_out[1] = L'\0';
        return last_sep ? last_sep + 1 : path;
    }
    size_t plen = (size_t)(last_sep - path);
    if (plen >= 1024) plen = 1023;
    memcpy(parent_out, path, plen * sizeof(WCHAR));
    parent_out[plen] = L'\0';
    return last_sep + 1;
}

/* Convert wide name to narrow */
static void wchar_to_narrow(const WCHAR *w, char *out, int max)
{
    int i = 0;
    while (i < max - 1 && w[i]) {
        out[i] = (char)(w[i] & 0x7F);
        i++;
    }
    out[i] = '\0';
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
        if (*PSecurityDescriptorSize < g_security_desc_len) {
            *PSecurityDescriptorSize = g_security_desc_len;
            return STATUS_BUFFER_OVERFLOW;
        }
        *PSecurityDescriptorSize = g_security_desc_len;
        if (SecurityDescriptor)
            memcpy(SecurityDescriptor, g_security_desc, g_security_desc_len);
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

    /* Store parent info for Cleanup delete */
    WCHAR parent_w[1024];
    const WCHAR *name_w = split_path(FileName, parent_w);
    struct ixfs_inode dummy;
    ctx->parent_ino = resolve_path(parent_w, &dummy);
    wchar_to_narrow(name_w, ctx->name, IXFS_MAX_NAME);

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

static NTSTATUS ixfs_Create(FSP_FILE_SYSTEM *FileSystem,
                            PWSTR FileName,
                            UINT32 CreateOptions,
                            UINT32 GrantedAccess,
                            UINT32 FileAttributes,
                            PSECURITY_DESCRIPTOR SecurityDescriptor,
                            UINT64 AllocationSize,
                            PVOID *PFileContext,
                            FSP_FSCTL_FILE_INFO *FileInfo)
{
    IXFS_FILE_CTX *ctx;
    WCHAR parent_w[1024];
    const WCHAR *name_w;
    char name_n[IXFS_MAX_NAME];
    struct ixfs_inode parent_inode;
    uint32_t parent_ino, new_ino;
    uint16_t type;

    (void)FileSystem;
    (void)GrantedAccess;
    (void)SecurityDescriptor;
    (void)AllocationSize;

    if (!g_vol->bitmap)
        return STATUS_MEDIA_WRITE_PROTECTED;

    name_w = split_path(FileName, parent_w);
    wchar_to_narrow(name_w, name_n, IXFS_MAX_NAME);

    parent_ino = resolve_path(parent_w, &parent_inode);
    if (parent_ino == 0)
        return STATUS_OBJECT_PATH_NOT_FOUND;

    type = (CreateOptions & FILE_DIRECTORY_FILE) ? IXFS_S_DIR : IXFS_S_FILE;
    new_ino = ixfs_create(g_vol, parent_ino, &parent_inode, name_n, type);
    if (new_ino == 0)
        return STATUS_DISK_FULL;

    ctx = malloc(sizeof(IXFS_FILE_CTX));
    if (!ctx) return STATUS_INSUFFICIENT_RESOURCES;

    ctx->ino = new_ino;
    ctx->parent_ino = parent_ino;
    ixfs_read_inode(g_vol, new_ino, &ctx->inode);
    ctx->is_dir = (type == IXFS_S_DIR) ? 1 : 0;
    strncpy(ctx->name, name_n, IXFS_MAX_NAME - 1);

    fill_file_info(FileInfo, &ctx->inode, new_ino);
    *PFileContext = ctx;
    return STATUS_SUCCESS;
}

static NTSTATUS ixfs_Write(FSP_FILE_SYSTEM *FileSystem,
                           PVOID FileContext,
                           PVOID Buffer,
                           UINT64 Offset,
                           ULONG Length,
                           BOOLEAN WriteToEndOfFile,
                           BOOLEAN ConstrainedIo,
                           PULONG PBytesTransferred,
                           FSP_FSCTL_FILE_INFO *FileInfo)
{
    IXFS_FILE_CTX *ctx = (IXFS_FILE_CTX *)FileContext;
    int64_t n;

    (void)FileSystem;
    (void)ConstrainedIo;

    if (!g_vol->bitmap)
        return STATUS_MEDIA_WRITE_PROTECTED;

    if (WriteToEndOfFile)
        Offset = ctx->inode.i_size;

    n = ixfs_write_data(g_vol, ctx->ino, &ctx->inode, Offset, Buffer, Length);
    if (n < 0)
        return STATUS_DEVICE_NOT_READY;

    *PBytesTransferred = (ULONG)n;
    fill_file_info(FileInfo, &ctx->inode, ctx->ino);
    return STATUS_SUCCESS;
}

static NTSTATUS ixfs_Overwrite(FSP_FILE_SYSTEM *FileSystem,
                               PVOID FileContext,
                               UINT32 FileAttributes,
                               BOOLEAN ReplaceFileAttributes,
                               UINT64 AllocationSize,
                               FSP_FSCTL_FILE_INFO *FileInfo)
{
    IXFS_FILE_CTX *ctx = (IXFS_FILE_CTX *)FileContext;

    (void)FileSystem;
    (void)FileAttributes;
    (void)ReplaceFileAttributes;
    (void)AllocationSize;

    /* Truncate to zero */
    ctx->inode.i_size = 0;
    ctx->inode.i_mtime = (uint32_t)time(NULL);
    ixfs_write_inode(g_vol, ctx->ino, &ctx->inode);

    fill_file_info(FileInfo, &ctx->inode, ctx->ino);
    return STATUS_SUCCESS;
}

static NTSTATUS ixfs_SetFileSize(FSP_FILE_SYSTEM *FileSystem,
                                 PVOID FileContext,
                                 UINT64 NewSize,
                                 BOOLEAN SetAllocationSize,
                                 FSP_FSCTL_FILE_INFO *FileInfo)
{
    IXFS_FILE_CTX *ctx = (IXFS_FILE_CTX *)FileContext;

    (void)FileSystem;
    (void)SetAllocationSize;

    ctx->inode.i_size = NewSize;
    ctx->inode.i_mtime = (uint32_t)time(NULL);
    ixfs_write_inode(g_vol, ctx->ino, &ctx->inode);

    fill_file_info(FileInfo, &ctx->inode, ctx->ino);
    return STATUS_SUCCESS;
}

static NTSTATUS ixfs_Rename(FSP_FILE_SYSTEM *FileSystem,
                            PVOID FileContext,
                            PWSTR FileName,
                            PWSTR NewFileName,
                            BOOLEAN ReplaceIfExists)
{
    IXFS_FILE_CTX *ctx = (IXFS_FILE_CTX *)FileContext;
    char old_name[IXFS_MAX_NAME], new_name[IXFS_MAX_NAME];
    WCHAR parent_w[1024];
    const WCHAR *new_name_w;

    (void)FileSystem;
    (void)ReplaceIfExists;

    if (!g_vol->bitmap)
        return STATUS_MEDIA_WRITE_PROTECTED;

    new_name_w = split_path(NewFileName, parent_w);
    strncpy(old_name, ctx->name, IXFS_MAX_NAME - 1);
    wchar_to_narrow(new_name_w, new_name, IXFS_MAX_NAME);

    struct ixfs_inode parent_inode;
    if (ixfs_read_inode(g_vol, ctx->parent_ino, &parent_inode) != 0)
        return STATUS_INTERNAL_ERROR;

    if (ixfs_rename(g_vol, ctx->parent_ino, &parent_inode, old_name, new_name) != 0)
        return STATUS_OBJECT_NAME_COLLISION;

    strncpy(ctx->name, new_name, IXFS_MAX_NAME - 1);
    return STATUS_SUCCESS;
}

static VOID ixfs_Cleanup_rw(FSP_FILE_SYSTEM *FileSystem,
                             PVOID FileContext,
                             PWSTR FileName,
                             ULONG Flags)
{
    IXFS_FILE_CTX *ctx = (IXFS_FILE_CTX *)FileContext;

    (void)FileSystem;
    (void)FileName;

    if (Flags & FspCleanupDelete) {
        if (g_vol->bitmap && ctx->parent_ino != 0) {
            struct ixfs_inode parent_inode;
            if (ixfs_read_inode(g_vol, ctx->parent_ino, &parent_inode) == 0)
                ixfs_delete(g_vol, ctx->parent_ino, &parent_inode, ctx->name);
        }
    }
}

static FSP_FILE_SYSTEM_INTERFACE ixfs_winfsp_interface = {
    .GetVolumeInfo = ixfs_GetVolumeInfo,
    .GetSecurityByName = 0,
    .Create = ixfs_Create,
    .Open = ixfs_Open,
    .Close = ixfs_Close,
    .Cleanup = ixfs_Cleanup_rw,
    .Read = ixfs_Read,
    .Write = ixfs_Write,
    .SetFileSize = ixfs_SetFileSize,
    .Overwrite = ixfs_Overwrite,
    .Rename = ixfs_Rename,
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

    if (part_idx < 0) {
        fprintf(stderr, "Error: partition index must be >= 0 (0 = raw image, no GPT)\n");
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

    /* Load bitmap for R/W support */
    int rw = (ixfs_load_bitmap(g_vol) == 0);

    fprintf(stderr, "IXFS: \"%s\" v%u, %llu blocks (%s)\n",
            g_vol->sb.s_volume_name,
            g_vol->sb.s_version,
            (unsigned long long)g_vol->sb.s_total_blocks,
            rw ? "R/W" : "read-only");

    /* No security descriptors — PersistentAcls=0 tells WinFsp to skip ACL checks */

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
    vol_params.ReadOnlyVolume = rw ? 0 : 1;
    wcscpy(vol_params.FileSystemName, L"IXFS");

    result = FspFileSystemCreate(L"WinFsp.Disk",
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
