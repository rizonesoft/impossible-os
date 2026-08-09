---
schema_version: 1
id: optical-media
domain: 05-storage-filesystems
status: active
title: "TODO-11 -- Optical Media: ISO 9660, Joliet & UDF"
---

# TODO-11 -- Optical Media: ISO 9660, Joliet & UDF

> **Goal:** Complete the ATAPI/SCSI command layer for optical drives (READ TOC, DISC INFO, TRACK INFO), then build read-only filesystem drivers for ISO 9660 (base + Rock Ridge), Joliet (UCS-2BE names), and UDF (DVD/Blu-ray). Add an auto-probe chain that selects the richest format (UDF > Joliet > ISO 9660). Expose disc metadata via `GetVolumeInformationW`. All three formats are physically read-only. Audio CD raw-sector read is included as a foundation for a future CD ripping feature.

> [!IMPORTANT]
> The low-level ATAPI transport is already working: `atapi_dma_command()`, `atapi_do_read()`/`atapi_do_read12()`, `atapi_read_capacity()`, `atapi_get_configuration()`, `atapi_request_sense()`, `atapi_test_unit_ready()`, and `atapi_inquiry()` all exist in `src/kernel/drivers/ahci/ahci_atapi.c`. This TODO starts at the **SCSI MMC layer** (READ TOC, DISC INFORMATION, READ TRACK INFO) -- §1 -- and builds up through the three filesystem layers. ISO 9660 §2 is the mandatory base for §3 (Rock Ridge) and §4 (Joliet); UDF §5 is independent and shares only the ATAPI read helper from §1. Wire `optical_probe()` into `vfs_probe()` at step 7 (→ XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §1` optical drive handling).

## Inputs

- `src/kernel/drivers/ahci/ahci_atapi.c` + `include/kernel/drivers/ahci_internal.h` -- existing ATAPI transport; `atapi_dma_command(port, cdb, cdb_len, buf, buf_len, dir)` is the CDB issuing primitive for §1; `atapi_do_read(port, lba, count, buf)` is the cooked-sector read primitive for §2–§5
- `include/kernel/drivers/ahci.h` -- `ahci_atapi_read(atapi_idx, lba, count, buf)` public API; `ahci_atapi_capacity(atapi_idx)` for disc capacity
- `src/kernel/fs/vfs.c` + `include/kernel/fs/vfs.h` -- `vfs_mount()`, `vfs_fs_driver`, read-only mount via `VFS_READONLY`
- → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §8` -- optical drive handling; `optical_probe()` is step 7 in `vfs_probe()`; tray-open command and autorun stub are owned there; this TODO provides the FS-level mount for `vfs_probe()` to call
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §3` -- `CreateFile` on an optical drive letter calls `NtCreateFile` → `vfs_open` → optical fs vtable; ensure all three vtables expose the full read-only 14-entry `vfs_fs_driver`
- Related (no stable XREF target): `04-drivers-hardware/TODO-xx-ahci-driver` -- AHCI ATAPI port detection (`AHCI_SIG_ATAPI`, `is_atapi` flag, `ahci_atapi_count()`) provides the `atapi_idx` used throughout §1

## Outcome

- READ TOC, DISC INFORMATION, and READ TRACK INFO SCSI MMC commands implemented; disc type (audio/data/mixed), track count, and disc status exposed via `atapi_disc_info_t`.
- ISO 9660 Primary Volume Descriptor parsed; directory records walked; files read via cooked 2048-byte sector reads; VFS mounted read-only.
- Rock Ridge SUSP extensions decoded: POSIX mode, symlinks, alternate names (up to 255 chars), timestamps, deep-directory relocation.
- Joliet Supplementary Volume Descriptor detected; UCS-2BE names decoded to UTF-8; El Torito boot record silently skipped.
- UDF Anchor Volume Descriptor Pointer found at sector 256; full VDS parsed; ICB-based directory and file read working; open/closed integrity state checked.
- Auto-probe selects richest available format (UDF > Joliet > ISO 9660).
- `GetVolumeInformationW` returns disc label, filesystem name, and serial number.
- Audio CD raw 2352-byte sector reads working via READ CD command; audio tracks detectable from TOC.

## Implementation Order

| ⭐  | Order | Deliverable                                                                      | Depends On                                                       | Status |
| --- | :---: | -------------------------------------------------------------------------------- | ---------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 SCSI MMC layer -- READ TOC, DISC INFO, TRACK INFO, `atapi_disc_info_t`        | Existing `atapi_dma_command()` transport                         |  [ ]   |
| 💎  |   2   | §2 ISO 9660 base -- PVD parse, directory records, file read, VFS mount           | §1 (`atapi_do_read` for cooked 2 KiB sectors)                    |  [ ]   |
| 💎  |   3   | §3 Rock Ridge extensions -- SUSP walker, PX/SL/NM/TF/CL/RE entries               | §2 (Rock Ridge SUSP data appended to ISO 9660 directory records) |  [ ]   |
| 💎  |   4   | §4 Joliet -- SVD `%/E` detection, UCS-2BE→UTF-8 decode, El Torito skip           | §2 (Joliet SVD shares the same VDS scan loop as PVD)             |  [ ]   |
| 💎  |   5   | §5 UDF -- AVDP, VDS, Partition+LV descriptors, ICB, directory + file read        | §1 (raw sector reads needed for AVDP at sector 256)              |  [ ]   |
| ⭐  |   6   | §6 Auto-probe priority -- UDF > Joliet > ISO 9660 selection + unified VFS mount  | §2, §4, §5 (all three format probes must exist before priority)  |  [ ]   |
| 💎  |   7   | §7 Disc label + metadata -- `GetVolumeInformationW`, `GetDriveTypeW`, serial CRC | §6 (format selected; label source depends on winner)             |  [ ]   |
| 💎  |   8   | §8 Audio CD raw reads -- TOC audio track detection, READ CD 2352-byte sectors    | §1 (TOC parse identifies audio tracks), §2 (disc already probed) |  [ ]   |

> §1–§5 and §7–§8 are `💎` parity -- every modern OS supports optical media. §6 is `⭐` exclusive: the unified in-kernel probe chain (UDF > Joliet > ISO) with automatic best-format selection is a level of optical drive intelligence that Linux leaves to userspace (`udisks2`) and Windows exposes only through the shell (not the kernel API). Impossible OS makes the right choice at mount time without any userspace daemon.

---

## 1. SCSI MMC Layer -- READ TOC, DISC INFO, TRACK INFO `[Sonnet]`

Extend `ahci_atapi.c` with READ TOC (CDB `0x43`), DISC INFORMATION (`0x51`), and READ TRACK INFO (`0x52`). Expose results in `atapi_disc_info_t`. Replace the bare `atapi_do_read()` call sites in the filesystem layers with the higher-level `atapi_read_cd()` cooked-sector helper.

**Files:** `src/kernel/drivers/ahci/ahci_atapi.c` (extend), `include/kernel/drivers/ahci.h` (extend)

> [!NOTE]
> GET CONFIGURATION (`atapi_get_configuration`) already exists and returns profile flags. READ TOC: CDB[0]=`0x43`, CDB[1]=bit1=`MSF` (0=LBA), CDB[2]=Format (`0x00` = TOC, `0x01` = Session info), CDB[6]=Track/Session number, CDB[7:8]=Allocation length (big-endian), CDB[9]=Control. Response: 4-byte header (data length, first track, last track) followed by track descriptor records (8 bytes each: session, ADR/Control, TrackNumber, reserved, AbsAddress). Audio track: `Control & 0x04 == 0`; data track: `Control & 0x04 != 0`. DISC INFORMATION: CDB[0]=`0x51`, CDB[7:8]=Allocation length. Response includes `disc_status` (0=blank, 1=appendable, 2=complete), `n_sessions`, `erasable`. READ TRACK INFO: CDB[0]=`0x52`, CDB[1]=Address/Number type (1=LBA, 2=track), CDB[2:5]=address, CDB[7:8]=Allocation length. Response: track start LBA, track size, data mode.

- [ ] `atapi_read_toc(atapi_idx, &toc_buf, buf_len)`: issue READ TOC Format=0x00; fill caller's buffer; return number of track descriptors
- [ ] `atapi_disc_information(atapi_idx, &disc_info_raw)`: issue DISC INFORMATION; parse `disc_status`, `erasable`, `n_sessions`
- [ ] `atapi_read_track_info(atapi_idx, track_or_lba, addr_type, &track_raw)`: issue READ TRACK INFO; return track start LBA + size + data mode
- [ ] `atapi_disc_info_t { uint8_t track_count; uint8_t first_track_lba_sec; uint8_t disc_type (audio/data/mixed/unknown); uint8_t disc_status; uint8_t n_sessions; uint8_t erasable; atapi_track_t tracks[99]; }` where `atapi_track_t { uint32_t start_lba; uint32_t size_lba; uint8_t audio; }`
- [ ] `atapi_get_disc_info(atapi_idx, &info)`: call TOC + DISC INFORMATION + TRACK INFO; populate `atapi_disc_info_t`; log `[ATAPI] Disc: %u tracks, type=%s, sessions=%u`
- [ ] `atapi_read_cd(atapi_idx, lba, count, buf)`: wrapper around existing `ahci_atapi_read()`; explicit cooked 2 KiB sector path; used by §2–§5
- [ ] Commit: `"drivers/atapi: READ TOC, DISC INFO, TRACK INFO, atapi_disc_info_t, atapi_read_cd wrapper"`

## 2. ISO 9660 Base `[Sonnet]`

Scan sectors 16–31 for the Primary Volume Descriptor. Parse the root directory record. Walk directory extents. Read files via 2 KiB sector reads. Register VFS read-only.

**Files:** `src/kernel/fs/optical/iso9660.c` (new), `include/kernel/fs/iso9660.h` (new)

> [!NOTE]
> Volume Descriptor Set: sectors 0–15 = System Area (skip). Scan sectors 16+ for Volume Descriptor records until `VolumeDescriptorSetTerminator (0xFF)`. PVD (type `0x01`): `Volume Identifier[32]`, `Volume Space Size[8]` (LE+BE pair), `Path Table Location LE[4]`, `Root Directory Record[34]` (embedded in PVD at offset 156). Directory Record layout (variable length): `LEN_DR(1)`, `EAR_LEN(1)`, `Extent Location[8]` (LE+BE), `Data Length[8]` (LE+BE), `Recording Date[7]`, `File Flags(1)` (bit1=dir, bit0=hidden), `File Unit Size(1)`, `Interleave Gap(1)`, `Volume Sequence Number[4]` (LE+BE), `File Identifier Length(1)`, `File Identifier[LEN_FI]`. File identifier `"\x00"` = `.` (current dir), `"\x01"` = `..`. Sector size is always 2 048 bytes for data CD/DVD.

- [ ] `iso9660_vol_t` struct: `root_extent_lba`, `root_extent_len`, `vol_size_sectors`, `label[33]`, `atapi_idx`
- [ ] `iso9660_parse_pvd(atapi_idx, &vol)`: read sector 16; verify `type == 0x01` and standard identifier `"CD001"`; extract root directory record (LBA + length) and volume label
- [ ] `iso9660_dir_record_t` struct: all parsed fields; `name[256]` (decoded from file identifier)
- [ ] `iso9660_readdir(vol, dir_lba, dir_len, callback, ctx)`: read all sectors in range `[dir_lba, dir_lba + ceil(dir_len/2048))`; for each sector: walk records by `LEN_DR`; skip padding entries (`LEN_DR == 0`); skip `.` and `..`; call `callback(ctx, &record)`
- [ ] `iso9660_file_read(vol, extent_lba, data_len, offset, buf, len)`: compute starting sector; `atapi_read_cd()` loop; handle partial first/last sectors
- [ ] VFS `finddir` callback: `iso9660_vfs_finddir(vfs_node, name)` → `iso9660_readdir()` + name compare (case-insensitive for base ISO 9660 names; uppercase compare)
- [ ] VFS `readdir` callback: `iso9660_vfs_readdir(vfs_node, index)` → `iso9660_readdir()` counting to `index`-th entry
- [ ] VFS `read` callback: `iso9660_vfs_read(vfs_node, buf, offset, len)` → `iso9660_file_read()`
- [ ] `iso9660_probe(atapi_idx)`: read sector 16; check standard identifier `"CD001"` at offset 1 and version `0x01`; return 1 or 0
- [ ] All write VFS callbacks: return `STATUS_MEDIA_WRITE_PROTECTED`
- [ ] Commit: `"fs/iso9660: PVD parse, directory record walk, file read, VFS probe + read-only mount"`

## 3. Rock Ridge Extensions `[Sonnet]`

Walk the System Use Sharing Protocol (SUSP) area appended to each directory record. Decode PX, SL, NM, TF, CL, RE entries. Use Rock Ridge names when present.

**Files:** `src/kernel/fs/optical/iso9660.c` (extend), `src/kernel/fs/optical/rockridge.c` (new)

> [!NOTE]
> SUSP area: starts at offset `33 + LEN_FI + (1 if LEN_FI is even else 0)` within the directory record (padding for even alignment). `RR` indicator entry at the start (signature `"RR"`) signals Rock Ridge extensions are present -- but many discs omit it; scan for known signatures regardless. Each SUSP entry: `signature[2]`, `length(1)`, `version(1)`, `data[length-4]`. Key entries: `PX` (POSIX attrs): `mode(4)`, `nlinks(4)`, `uid(4)`, `gid(4)`, `ino(4)`; `SL` (symlink): continuation flag + component records (`flags(1)`, `length(1)`, `data[length]`; flags: `0x02`=current, `0x04`=parent, `0x08`=root); `NM` (alternate name): flags + name bytes; may span multiple `NM` entries (continuation flag bit 0); `TF` (timestamps): short form (7 bytes, ISO date) or long form (17 bytes, ISO 8601); flags byte selects which timestamps are present; `CL` (child link): relocated directory's real location; `RE` (relocated entry): marks the directory as relocated (skip in parent listing, use `CL` location instead).

- [ ] `rrip_parse_susp(record_buf, record_len, &rrip_out)`: scan SUSP area; decode all known signatures; populate `rrip_entry_t { char nm[256]; mode_t mode; uid_t uid; gid_t gid; uint64_t ino; char sl_target[1024]; int has_px, has_nm, has_sl, has_tf; time_t mtime, atime, ctime; uint32_t cl_lba; int is_re; }`
- [ ] NM assembly: concatenate multiple `NM` continuation entries into `nm` buffer (up to 255 bytes)
- [ ] SL assembly: decode component records into a POSIX path string (handle `/` = root, `.` = current, `..` = parent components)
- [ ] TF: parse 7-byte date format (`YY MM DD HH MM SS OFFSET`) to FILETIME; handle long 17-byte format if present
- [ ] CL/RE: when `is_re` is set on a directory record → skip it in the listing; the parent's directory entry with `CL` points to the real location
- [ ] Integrate into `iso9660_readdir()`: after parsing a directory record, call `rrip_parse_susp()`; if `has_nm`: use `rrip.nm` as the filename; if `has_px`: set VFS node mode; if `has_sl`: expose as symlink VFS node
- [ ] VFS `readlink` callback: `iso9660_vfs_readlink(vfs_node, buf, len)` → stored `sl_target` from RRIP
- [ ] Commit: `"fs/iso9660: Rock Ridge SUSP -- PX/NM/SL/TF/CL/RE decode, long filenames, symlinks, POSIX attrs"`

## 4. Joliet `[Sonnet]`

Detect a Supplementary Volume Descriptor with `%/E` escape sequence (Joliet Level 3). Decode UCS-2BE filenames to UTF-8. Prefer Joliet SVD over PVD when present. Silently skip El Torito boot records.

**Files:** `src/kernel/fs/optical/joliet.c` (new)

> [!NOTE]
> VDS scan: scan sectors 16+ for all Volume Descriptor records until the set terminator. SVD: type `0x02`. Joliet SVD: `EscapeSequences[32]` at offset 88 contains `"%/E"` (Joliet Level 3 -- up to 64 UCS-2 characters = 128 bytes per filename), `"%/C"` (Level 1 -- 16 chars), or `"%/G"` (Level 2 -- 31 chars); check for `%/E` first. Joliet directory records are identical in structure to ISO 9660 records except the `File Identifier` field contains UCS-2BE (big-endian UTF-16) characters. File identifier length is in bytes (2 bytes per character). `joliet_to_utf8(ucs2be, byte_len, utf8_out, out_max)`: iterate pairs; for BMP codepoints: standard 1/2/3 byte UTF-8; for surrogate pairs (rare on disc): decode to 4-byte UTF-8. El Torito: VD type `0x00` with standard identifier `"CD001"` and system identifier `"EL TORITO SPECIFICATION"` -- read the record, log `[optical] El Torito boot record detected (skipped)`, continue VDS scan without mounting.

- [ ] `joliet_vol_t` struct: same as `iso9660_vol_t` but with UCS-2 decoding flag; shares most code with ISO 9660
- [ ] `joliet_scan_vds(atapi_idx, &pvd_vol, &joliet_vol, &el_torito_present)`: scan all VD records; if SVD with `%/E`/`%/C`/`%/G`: populate `joliet_vol`; if El Torito: set `el_torito_present`; if PVD: populate `pvd_vol`
- [ ] `joliet_to_utf8(ucs2be_buf, byte_len, utf8_out, out_max)` → bytes written; handle BE→LE swap; standard BMP encode; surrogate pair decode for supplementary chars
- [ ] `joliet_readdir(vol, dir_lba, dir_len, callback, ctx)`: same as `iso9660_readdir()` but call `joliet_to_utf8()` on each file identifier before callback; skip separator `0x3B` version number suffix (`;1`)
- [ ] VFS callbacks: reuse ISO 9660 `file_read` + `readdir`/`finddir` infrastructure; swap in `joliet_readdir()` and name comparison on `utf8` decoded names
- [ ] `joliet_probe(atapi_idx)`: call `joliet_scan_vds()`; return 1 if `joliet_vol.root_extent_lba != 0`, 0 otherwise
- [ ] Commit: `"fs/optical/joliet: SVD %/E detection, UCS-2BE→UTF-8 decode, readdir, El Torito skip"`

## 5. UDF `[Opus]`

Read the AVDP at sector 256. Parse the Volume Descriptor Sequence (Partition + Logical Volume + File Set). Walk ICB-based inodes for directory listing and file reads. Check volume integrity state.

**Files:** `src/kernel/fs/optical/udf.c` (new), `include/kernel/fs/udf_internal.h` (new)

> [!NOTE]
> AVDP (Anchor Volume Descriptor Pointer) at sector 256: 16-byte CRC tag (descriptor tag: `TagIdentifier=0x0002`, `TagLocation=256`, CRC), then `MainVDSExtent { ExtentLength(4), ExtentLocation(4) }` and `ReserveVDSExtent`. VDS scan: read `ceil(MainVDSExtent.ExtentLength / sectorsize)` sectors starting at `MainVDSExtent.ExtentLocation`; iterate descriptors by descriptor tag (`TagIdentifier`): `0x0001` = PVD (skip), `0x0004` = Implementation Use VD (skip), `0x0005` = Partition Descriptor (extract `pStartingLocation`, `pAccessType`; type 1=read-only, 3=rewritable), `0x0006` = Logical Volume Descriptor (extract `LogicalBlockSize`, `LogicalVolumeIdentifier` as CS0-encoded label, `IntegritySequenceExtent`, `MapTableLength` / `PartitionMaps`), `0x0009` = File Set Descriptor (from ICB in LVD's `LogicalVolumeContentsUse`; contains `rootICBLocation` → root directory File Entry ICB), `0x0008` = Terminating Descriptor (stop). Volume Integrity Descriptor: at `IntegritySequenceExtent`; `IntegrityType` 0=open (dirty), 1=close (clean); if open → mount read-only and log warning. ICB Tag: `FileType` 0=unspecified, 4=directory, 5=file, 12=symlink. Allocation Descriptor types (AD): Short AD (8 bytes: `ExtentLength(4)`, `ExtentPosition(4)` -- partition-relative), Long AD (16 bytes: `ExtentLength(4)`, `ExtentLocation { LogicalBlockNumber(4), PartitionReferenceNumber(2) }`, `ImplementationUse(6)`), Extended AD (20 bytes). `ExtentLength` high 2 bits = extent type: 0=recorded, 1=unallocated (sparse), 2=allocated but not recorded.

- [ ] `udf_vol_t` struct: `partition_start_sector`, `partition_len`, `lv_block_size`, `label_cs0[128]`, `root_icb_lba`, `root_icb_part`, `integrity_open`
- [ ] `udf_descriptor_tag_t` struct + `udf_tag_verify(buf)`: verify CRC-ITU-T of tag (first 16 bytes) and data; return 0 on success
- [ ] `udf_parse_avdp(atapi_idx, &avdp)`: read sector 256; verify tag `0x0002`; extract `MainVDSExtent`; if CRC fail: try sector 257 (AVDP backup)
- [ ] `udf_parse_vds(atapi_idx, &avdp, &vol)`: scan VDS sectors; decode Partition Descriptor and Logical Volume Descriptor; validate `IntegritySequenceExtent` (open = warn + read-only flag)
- [ ] `udf_partition_to_sector(vol, partition_ref, lbn)` → `vol->partition_start_sector + lbn`
- [ ] `udf_read_icb(atapi_idx, vol, icb_lba, part, &inode_out)`: read sector at `udf_partition_to_sector(part, icb_lba)`; parse File Entry or Extended File Entry; decode ICB tag (`file_type`), `informationLength`, `logicalBlocksRecorded`, allocation descriptor list type (from `ICBTag.flags & 0x0007`: 0=short, 1=long, 2=extended, 3=inline)
- [ ] `udf_readdir(atapi_idx, vol, dir_icb_lba, part, callback, ctx)`: read dir ICB's Short/Long ADs; for each sector: parse File Identifier Descriptors (`TagIdentifier=0x0101`): `L_FI(1)`, `L_ICB(1)`, `L_IU(2)`, `ICB(16 long_ad)`, `FileCharacteristics(1)` (bit1=dir, bit3=deleted), `FileIdentifier[L_FI]` (CS0-encoded name → UTF-8); skip deleted entries; call callback
- [ ] `udf_cs0_to_utf8(cs0_buf, cs0_len, utf8_out, out_max)`: CS0 byte 0 = compression ID: 8=UTF-8 bytes direct, 16=UTF-16BE (convert); decode accordingly
- [ ] `udf_file_read(atapi_idx, vol, icb_lba, part, offset, buf, len)`: read ICB; walk ADs; map to physical sectors; for Short ADs with inline data (`L_EA > 0`): memcpy from ICB extended attribute area; for allocated extents: `atapi_read_cd()` loop; for unallocated (sparse): memset 0
- [ ] `udf_probe(atapi_idx)`: try `udf_parse_avdp()`; return 1 if tag `0x0002` found and CRC valid, 0 otherwise
- [ ] VFS callbacks: `udf_vfs_finddir`, `udf_vfs_readdir`, `udf_vfs_read`, `udf_vfs_stat`, `udf_vfs_readlink` (symlink ICB: inline Short AD with text target)
- [ ] Commit: `"fs/udf: AVDP, VDS parse, Partition+LV+FileSet, ICB read, directory + file walk, CS0→UTF-8"`

## 6. Auto-Probe Priority `[Sonnet]`

In `vfs_probe()`, for optical drives call the three probes in order: UDF > Joliet > ISO 9660. Mount the richest available format. Fall back gracefully.

**Files:** `src/kernel/fs/optical/optical_probe.c` (new), `src/kernel/fs/partition.c` (extend)

> [!NOTE]
> Priority logic: call `udf_probe(atapi_idx)` first; if 1 → `udf_mount()`; else call `joliet_probe(atapi_idx)` (which internally detects the SVD); if Joliet SVD present → `joliet_mount()`; else call `iso9660_probe(atapi_idx)`; if 1 → `iso9660_mount()` (with Rock Ridge enabled automatically -- it degrades gracefully if no SUSP area is found). A single disc can contain all three simultaneously (this is common for commercially pressed DVDs); the priority ensures the richest name and metadata set is presented. If none of the three probes succeed: the disc is not a data disc (may be audio-only); set `vol->audio_only = 1` and skip filesystem mount.

- [ ] `optical_probe_and_mount(atapi_idx, letter)`: try UDF → Joliet → ISO 9660; log `[optical] %c: format=%s` with the winner; on all-fail: log `[optical] %c: no data filesystem (audio disc?)` and set audio-only flag
- [ ] `optical_umount(letter)`: teardown whichever driver was mounted; free cached volume struct
- [ ] Add `optical_probe_and_mount()` call in `vfs_probe()` at step 7 for ATAPI devices (detect via `ahci_atapi_count()` and `ahci_atapi_capacity()`)
- [ ] Commit: `"fs/optical: auto-probe chain UDF > Joliet > ISO9660, vfs_probe integration, audio-disc fallback"`

## 7. Disc Label + Metadata `[Sonnet]`

Implement `GetVolumeInformationW` for optical drives. Return the disc volume identifier, filesystem name string, and a serial number derived from the PVD/LVD volume set identifier CRC. Support `GetDriveTypeW` returning `DRIVE_CDROM`.

**Files:** `src/kernel/fs/optical/optical_probe.c` (extend)

> [!NOTE]
> `GetVolumeInformationW` for the optical volume driver: `lpVolumeNameBuffer` → disc label (from PVD `Volume Identifier` for ISO/Joliet, or LVD `Logical Volume Identifier` for UDF, decoded to UTF-16); `lpFileSystemNameBuffer` → `"ISO 9660"`, `"Joliet"`, or `"UDF"` depending on mounted format; `lpVolumeSerialNumber` → CRC32 of the 128-byte `Volume Set Identifier` from the PVD (or UDF UUID if available); `lpMaximumComponentLength` → 255 (Joliet/UDF) or 31 (base ISO 9660) or 64 (Joliet Level 1/2); `lpFileSystemFlags` → `FILE_READ_ONLY_VOLUME | FILE_UNICODE_ON_DISK`. `GetDriveTypeW` returns `DRIVE_CDROM (5)` for any optical drive letter, regardless of disc format. → XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §8` for Win32 volume API registration.

- [ ] `optical_get_label(atapi_idx, buf, buf_len)`: return label from whichever format is mounted (ISO PVD / Joliet SVD / UDF LVD); trim trailing spaces (ISO 9660 pads with spaces to 32 bytes)
- [ ] `optical_get_fsname(atapi_idx, buf)` → `"ISO 9660"` / `"Joliet"` / `"UDF 2.01"` (read UDF revision from LVD `DomainIdentifier`)
- [ ] `optical_get_serial(atapi_idx)` → CRC32 of 128-byte Volume Set Identifier from PVD; for UDF: truncate UUID to 32-bit
- [ ] `optical_get_max_component_len(atapi_idx)` → 255 (UDF/Joliet L3), 64 (Joliet L2), 31 (Joliet L1), 31 (ISO base), 255 (Rock Ridge)
- [ ] Wire into Win32 `GetVolumeInformationW` dispatch (→ XREF: `05-storage-filesystems/TODO-03-volume-management-automount.md §3`)
- [ ] `GetDriveTypeW` for any optical letter returns `DRIVE_CDROM` unconditionally (set in volume mount metadata at §6)
- [ ] Commit: `"fs/optical: GetVolumeInformationW -- disc label, filesystem name, serial CRC32, GetDriveTypeW=CDROM"`

## 8. Audio CD Raw Reads `[Sonnet]`

Detect audio tracks from the TOC. Issue READ CD (CDB `0xBE`) for raw 2 352-byte CD-DA sectors. Expose via `ahci_atapi_read_audio()`. Document the hook point for a future CD ripping feature.

**Files:** `src/kernel/drivers/ahci/ahci_atapi.c` (extend), `include/kernel/drivers/ahci.h` (extend)

> [!NOTE]
> READ CD: CDB[0]=`0xBE`, CDB[1]=`SectorType << 2` (`0x04` = CD-DA sectors, i.e. `SectorType=1`), CDB[2:5]=Starting LBA (big-endian), CDB[6:8]=Transfer length (big-endian, number of sectors), CDB[9]=`0xF0` (request raw 2352-byte sectors including sync, header, user data, EDC/ECC, subcode -- `SyncData|Header|UserData|EDCandECC`), CDB[10]=SubChannelDataSelection=0 (no subchannel), CDB[11]=Control=0. Each sector read is 2 352 bytes: 12-byte sync, 4-byte header, 2 048 bytes user data, 288 bytes EDC/ECC. Audio data (16-bit stereo PCM, 44 100 Hz, big-endian) occupies bytes 16–2 063 within the raw sector. TOC audio track: `atapi_disc_info_t.tracks[i].audio == 1`.

- [ ] `atapi_read_cd_audio(atapi_idx, lba, count, buf)`: issue READ CD with `SectorType=0x01` (CD-DA); buffer must be `count * 2352` bytes; return 0 or error
- [ ] `atapi_is_audio_disc(atapi_idx)`: call `atapi_get_disc_info()`; return 1 if any track has `audio == 1`
- [ ] Log: `[ATAPI] %c: audio disc with %u audio track(s)` when audio tracks detected
- [ ] Mark hook point for future CD ripping: `// TODO: route to audio ripping API when media player is implemented (→ 11-apps/TODO-xx-media-player)`; this TODO does not implement the ripping UI
- [ ] `ahci_atapi_read_audio(atapi_idx, lba, count, buf)` public API added to `ahci.h`
- [ ] Commit: `"drivers/atapi: READ CD audio -- 2352-byte raw sectors, audio track detection, ripping hook comment"`

---

## OS Comparison


| ⭐  | Feature                                             | 🪟 Win11                                                 | 🐧 Linux                                                   | 🚀 Impossible OS                                                 |
| --- | --------------------------------------------------- | -------------------------------------------------------- | ---------------------------------------------------------- | ---------------------------------------------------------------- |
| 💎  | READ TOC + DISC INFO + TRACK INFO SCSI MMC commands | ✅ `cdrom.sys`; full MMC support; `IOCTL_CDROM_READ_TOC` | ✅ `cdrom.ko`; `cdrom_read_toc()`, `cdrom_get_disc_info()` | ⚠️ §1 -- In progress -- ; `atapi_dma_command`                    |
| 💎  | ISO 9660 base                                       | ✅ `cdfs.sys`; full ISO 9660 R/O                         | ✅ `isofs.ko`; full ISO 9660 R/O                           | ⬜ §2 -- PVD scan, directory record walk,                        |
| 💎  | Rock Ridge SUSP                                     | ❌ `cdfs.sys` does not support Rock                      | ✅ `isofs.ko`; full Rock Ridge RRIP                        | ⬜ §3 -- all 6 SUSP entry types                                  |
| 💎  | Joliet UCS-2BE names                                | ✅ `cdfs.sys`; Joliet Level 1–3; preferred               | ✅ `isofs.ko`; Joliet with `-o iocharset`                  | ⬜ §4 -- `joliet_to_utf8()`, `%/E` detection, El Torito          |
| 💎  | UDF                                                 | ✅ `udfs.sys`; full UDF 1.5–2.6 R/W                      | ✅ `udf.ko`; UDF 1.5–2.6 R/O +                             | ⬜ §5 -- UDF 1.5/2.0/2.01 R/O; AVDP→VDS→FSD→ICB chain            |
| ⭐  | Auto-probe priority                                 | ✅ Windows mounts the "best" format                      | ✅ Linux uses `mount -t udf/iso9660`                       | ⬜ §6 -- single in-kernel `optical_probe_and_mount()`; no daemon |
| 💎  | `GetVolumeInformationW`                             | ✅ Full Win32 optical volume API                         | ✅ Via `libblkid` / `udisks2` (userspace);                 | ⬜ §7 -- in-kernel; label from PVD/LVD/SVD; CRC32                |
| 💎  | Audio CD raw READ CD                                | ✅ `cdaudio.sys` + `IOCTL_CDROM_READ_TOC`; Windows Media | ✅ `cdrom.ko`; `cdparanoia`/`cdda2wav` use raw READ        | ⬜ §8 -- `atapi_read_cd_audio()`, audio track detection from     |

> **After §1–§8:** Impossible OS handles every common optical disc format -- including Rock Ridge (which Windows 11 does not support), UDF for DVD/Blu-ray, and audio CDs -- all resolved in-kernel without a userspace daemon. The in-kernel auto-probe chain with graceful audio-disc detection is a level of optical intelligence that neither Windows nor Linux achieves in the kernel alone.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] READ TOC: insert data CD in QEMU with a `.iso` image; `atapi_get_disc_info()` → `track_count > 0`; serial log shows `[ATAPI] Disc: N tracks, type=data`
- [ ] ISO 9660: mount a standard ISO image in QEMU; `FindFirstFileW("D:\\*.*")` lists root files; `ReadFile` on a text file returns correct bytes
- [ ] Rock Ridge: mount a Linux-created Rock Ridge ISO (e.g., `genisoimage -R`); long filenames (> 11 chars) appear correctly; symlink `vfs_node` has `readlink` populated; POSIX mode bits decoded
- [ ] Joliet: mount an ISO with Joliet (`-J` flag); Unicode filename with Japanese characters round-trips correctly through `joliet_to_utf8()`; El Torito SVD (type 0x00) logged and skipped without error
- [ ] UDF auto-probe: mount a UDF 2.01 DVD image; `[optical] D: format=UDF` in log; directory listing matches Linux `ls`
- [ ] Auto-probe priority: disc image with both ISO 9660 PVD and UDF AVDP; UDF is selected; `GetVolumeInformationW` returns `"UDF 2.01"`
- [ ] Disc label: `GetVolumeInformationW("D:", ...)` returns correct label for ISO, Joliet, and UDF volumes; `GetDriveTypeW("D:")` returns `DRIVE_CDROM (5)`
- [ ] Write protection: `WriteFile` to any optical drive letter → `ERROR_WRITE_PROTECT`; no crash
- [ ] Audio CD: audio-only ISO image (tracks with `audio=1` in TOC); log `[optical] D: audio disc with N audio track(s)`; `ahci_atapi_read_audio(idx, lba, 1, buf)` returns 2352 bytes without error; sector 0 starts with 12-byte sync header `0x00 0xFF...0xFF 0x00`
- [ ] Open UDF integrity: UDF volume with `IntegrityType=0` (open/dirty); mount → `[UDF] volume integrity open -- mounting read-only`; `WriteFile` returns `ERROR_WRITE_PROTECT`
- [ ] Commit: `"fs/optical: complete ISO9660 + Rock Ridge + Joliet + UDF + auto-probe + audio CD read"`
