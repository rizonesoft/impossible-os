# 040.99-Reference — Filesystem, Partition Tables & VM Boot Notes

> **Purpose:** Essential reference for how partition tables, filesystems, and
> virtual machine boot requirements interact. Covers QEMU and Hyper-V testing,
> disk image creation, and common boot failure diagnostics.

---

## Filesystem ↔ Partition Table Independence

### NTFS is partition-table-agnostic

NTFS runs on **both MBR and GPT**. The filesystem format is completely independent
of which partition table points to its volume.

| Partition Table | NTFS Identifier |
|-----------------|-----------------|
| MBR | Type code `0x07` |
| GPT | Type GUID `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` (Microsoft Basic Data) |

Windows has used NTFS on MBR disks since Windows NT 3.1 (1993). The filesystem
driver reads its superblock — it never inspects which partition table was used
to locate the volume.

### IXFS supports both MBR and GPT

IXFS (Impossible OS native filesystem) is registered in both partition table systems:

| Partition Table | IXFS Identifier | Source File |
|-----------------|-----------------|-------------|
| MBR | Type code `0xDA` | `src/kernel/fs/mbr.c` |
| GPT | GUID `DA000000-0000-4978-4653-000000000001` | `src/kernel/fs/gpt.c` |

The partition scanner (`src/kernel/fs/partition.c`) probes both MBR and GPT,
then detects IXFS via its **superblock magic** regardless of which partition
table pointed to it.

> [!IMPORTANT]
> **All filesystems are partition-table-agnostic.** FAT32, NTFS, ext4, exFAT,
> IXFS — none of them care which partition table (MBR or GPT) is used. The
> partition table only defines *where* the volume starts and ends on disk.
> The filesystem driver reads its own superblock/BPB to identify the format.

---

## Virtual Machine Boot Requirements

### Hyper-V

| VM Generation | Firmware | Partition Table | Boot Method |
|---------------|----------|-----------------|-------------|
| **Generation 1** | Legacy BIOS | **MBR** | BIOS bootstrap code at LBA 0 |
| **Generation 2** | **UEFI only** | **GPT required** | EFI System Partition (FAT32) → `BOOTX64.EFI` |

> [!CAUTION]
> **Hyper-V Gen 2 has Secure Boot enabled by default.** Your `BOOTX64.EFI` is
> not signed by Microsoft's key, so Secure Boot will reject it. You **must
> disable Secure Boot** in the VM settings before Impossible OS will boot.
>
> **Settings → Security → uncheck "Enable Secure Boot"**

### QEMU

| QEMU Mode | Firmware | Partition Table | Boot Method |
|-----------|----------|-----------------|-------------|
| Default (no OVMF) | SeaBIOS (legacy BIOS) | MBR | BIOS bootstrap |
| `-bios OVMF.fd` | UEFI (OVMF) | **GPT required** | ESP → `BOOTX64.EFI` |

Our build system (`scripts/build.sh run`) uses QEMU with OVMF for UEFI booting.

### VirtualBox

| VirtualBox Mode | Firmware | Partition Table | Boot Method |
|-----------------|----------|-----------------|-------------|
| Default | Legacy BIOS | MBR | BIOS bootstrap |
| EFI enabled | UEFI | **GPT required** | ESP → `BOOTX64.EFI` |

---

## Impossible OS Boot Chain

The UEFI boot sequence for Impossible OS:

```
UEFI Firmware
  → Reads GPT at LBA 1
  → Finds EFI System Partition (type GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B)
  → Mounts ESP as FAT32
  → Loads EFI/BOOT/BOOTX64.EFI (Limine bootloader)
  → Limine reads limine.conf from ESP
  → Limine loads kernel.exe from the IXFS data partition
  → Kernel takes control
```

> [!NOTE]
> **IXFS is never involved in the boot chain.** The UEFI firmware only reads
> the FAT32-formatted ESP. The Limine bootloader then reads the IXFS partition
> to load the kernel binary. If the kernel fails to boot, the issue is in the
> UEFI/GPT/ESP/Limine layer — not in IXFS.

---

## Disk Image Layout

The `system-disk.img` built by `scripts/build.sh` uses this layout:

```
┌──────────────────────────────────────────────────────────────┐
│ LBA 0          │ Protective MBR (type 0xEE)                  │
│ LBA 1          │ Primary GPT Header                          │
│ LBA 2–33       │ Primary Partition Entry Array (128 entries) │
│ LBA 2048+      │ Partition 1: ESP (FAT32, ~64 MB)            │
│                │   └── EFI/BOOT/BOOTX64.EFI (Limine)         │
│                │   └── limine.conf                           │
│ LBA xxxxx+     │ Partition 2: IXFS Data (~remainder)         │
│                │   └── kernel.exe                            │
│                │   └── fonts/, icons/, wallpapers/           │
│ End - 33       │ Backup Partition Entry Array                │
│ Last LBA       │ Backup GPT Header                           │
└──────────────────────────────────────────────────────────────┘
```

---

## Common Boot Failure Diagnostics

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Hyper-V Gen 2: "No boot device" | Disk image is MBR, not GPT | Ensure build creates GPT + ESP |
| Hyper-V Gen 2: "Secure Boot failed" | Unsigned EFI binary | Disable Secure Boot in VM settings |
| QEMU OVMF: "No bootable device" | Missing ESP or wrong FAT32 format | Check ESP partition has `BOOTX64.EFI` |
| Boots in QEMU but not Hyper-V | Secure Boot or VHD format issue | Disable Secure Boot; convert raw → VHDX if needed |
| Boots in QEMU but not VirtualBox | EFI not enabled in VBox settings | Enable EFI in System → Motherboard |
| Kernel panic after bootloader | Kernel issue, not partition/FS | Check serial output for panic message |
| "IXFS superblock not found" | IXFS partition not formatted or wrong LBA | Verify `mkfs.ixfs` ran during image build |

---

## Converting Raw Images for Hyper-V

Hyper-V requires **VHDX** format (not raw `.img`). Convert with:

```bash
# On the WSL host (not inside QEMU):
qemu-img convert -f raw -O vhdx build/system-disk.img build/system-disk.vhdx
```

Then attach `system-disk.vhdx` to the Hyper-V Gen 2 VM as a SCSI disk.

> [!TIP]
> Add a `vhdx` target to `scripts/build.sh` to automate this conversion step.

---

## VFS Capability Routing Strategy

When a Win32 API requests a feature the underlying filesystem doesn't support,
the VFS layer has **four possible responses**, depending on the feature:

| Strategy | When to Use | Example |
|----------|-------------|---------|
| **1. Return error code** | Feature is fundamental, can't fake | `SetFileSecurity()` on FAT32 → `ERROR_NOT_SUPPORTED` |
| **2. Silently succeed (no-op)** | Caller doesn't check, cosmetic | `SetFileAttributes(ARCHIVE)` on ext4 → return `TRUE`, discard |
| **3. Emulate in VFS** | Can be faked at a higher layer | `GetFileInformationByHandle.nFileIndexHigh` → VFS generates a synthetic ID |
| **4. Store in sidecar** | Data can be stored alongside | ADS on FAT32 → store in `._streams/` hidden directory |

### Concrete Examples per Feature

```
CreateFile("D:\photo.jpg:Zone.Identifier")  ← ADS on FAT32
├─ FAT32 doesn't support ADS
├─ Option A: Return ERROR_NOT_SUPPORTED (what Windows does)
├─ Option B: Store in D:\.streams\photo.jpg\Zone.Identifier (sidecar)
└─ We choose: Option A (match Windows behavior exactly)

SetFileSecurity(hFile, dacl)  ← ACLs on FAT32
├─ FAT32 has no security at all
├─ Windows returns ERROR_NOT_SUPPORTED
└─ We choose: Option A (return ERROR_NOT_SUPPORTED)

GetFileAttributes(hFile)  ← Win32 attrs on ext4
├─ ext4 has no Win32 attributes (hidden, system, archive)
├─ But ext4 HAS xattrs — can store them in user.win32_attrs
└─ We choose: Option 3/4 (emulate via xattr if writable, else return 0)

LockFile(hFile, ...)  ← Byte-range locking on any FS
├─ This is a VFS-level feature, not filesystem-dependent
└─ We choose: Option 3 (VFS handles it entirely — lock table in memory)

FindFirstChangeNotification()  ← File change notifications
├─ This is VFS-level (inotify-style event tracking)
└─ We choose: Option 3 (VFS tracks all changes regardless of FS)
```

### The Key Principle

The VFS `vfs_ops` struct should include a **capability flags** field:

```c
#define VFS_CAP_ADS          (1 << 0)  /* Alternate Data Streams */
#define VFS_CAP_ACLS         (1 << 1)  /* Security descriptors */
#define VFS_CAP_COMPRESSION  (1 << 2)  /* Transparent compression */
#define VFS_CAP_ENCRYPTION   (1 << 3)  /* Per-file encryption */
#define VFS_CAP_HARDLINKS    (1 << 4)  /* Hard links */
#define VFS_CAP_SYMLINKS     (1 << 5)  /* Symbolic links */
#define VFS_CAP_SPARSE       (1 << 6)  /* Sparse files */
#define VFS_CAP_CASE_SENS    (1 << 7)  /* Case-sensitive names */

struct vfs_fs_driver {
    uint32_t     capabilities;  /* VFS_CAP_* flags */
    struct vfs_ops ops;
};
```

Then each filesystem sets what it supports:

| Filesystem | Capabilities |
|------------|-------------|
| **IXFS** | ALL flags (ADS + ACLs + compression + encryption + hardlinks + symlinks + sparse) |
| **NTFS** | ADS + ACLs + hardlinks + symlinks + compression + sparse |
| **ext4** | hardlinks + symlinks + sparse + case-sensitive |
| **FAT32** | *(none)* |
| **exFAT** | *(none)* |

When a Win32 API comes in, the VFS checks `capabilities` first:

```c
// In VFS CreateFile handler, when "file:stream" is requested:
if (!(driver->capabilities & VFS_CAP_ADS)) {
    return ERROR_NOT_SUPPORTED;  // FAT32, ext4
}
// Else: pass through to filesystem's native ADS handler (IXFS, NTFS)
```

> [!IMPORTANT]
> This capability routing is partially covered in the VFS TODO (§2 "Feature
> Spoofing/Routing"). The `VFS_CAP_*` flags formalize it into a compile-time-checkable,
> per-driver capability matrix that makes the routing deterministic and auditable.
