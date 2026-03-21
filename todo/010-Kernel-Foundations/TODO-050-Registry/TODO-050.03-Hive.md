# 050.03-Hive — Registry Hive Persistence

> **Goal:** Provide crash-safe persistent storage for the registry tree using binary hive
> files. Defines the `hive_header_t` format (4 KiB page-aligned, CRC32-validated), the
> per-hive disk layout (`SYSTEM.hive`, `SOFTWARE.hive`, `HARDWARE.hive`, `DEFAULT.hive`
> under `C:\Impossible\System\Config\Registry\`), dirty-flag tracking for lazy-write
> flushing, and a Write-Ahead Journal (WAJ) crash-recovery protocol using `.hive.log`
> and `.hive.bak` files. All 3 sections are complete.

> [!IMPORTANT]
> **Prerequisite:** Depends on [TODO-050.01-Registry-Engine.md](TODO-050.01-Registry-Engine.md)
> (§1 Core Engine — `reg_key_t`, `reg_value_t`, pools) and
> [TODO-050.02-Win32-Reg-API.md](TODO-050.02-Win32-Reg-API.md)
> (§2 Win32 API — `RegSetValueEx`, `RegDeleteValue` trigger dirty marking).

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for the hive serialization buffer.
> Hive files can exceed 4 KB — never use `kmalloc` for the I/O buffer.
> The 2 MiB heap will silently corrupt on large allocations.

---

### Dependency Graph

```mermaid
graph TD
    ENGINE["TODO-050.01 Registry Engine ✅<br/>reg_key_t, reg_value_t, pools"]
    API["TODO-050.02 Win32 Reg API ✅<br/>RegSetValueEx, RegDeleteValue"]
    VFS["VFS Layer<br/>vfs_open, vfs_write, vfs_read"]

    A["§4.1 Hive File Format ✅<br/>hive_header_t, hive_save, hive_load, CRC32"]
    B["§4.2 Disk Layout ✅<br/>hive_table, registry_flush, dirty tracking"]
    C["§4.3 Crash-Safe Journaling ✅<br/>WAJ: .hive.log → .hive → .hive.bak"]

    REGEDIT["TODO-050-Registry §8<br/>Regedit Command"]
    REGFLUSH["TODO-050.02 §2.7<br/>RegFlushKey"]
    REGSAVE["TODO-050.02 §2.9<br/>RegSaveKey / RegRestoreKey"]
    PERF_MMAP["TODO-050-Registry §9.2<br/>Memory-Mapped Hives"]
    PERF_BTREE["TODO-050-Registry §9.3<br/>B-Tree Format"]

    ENGINE --> A
    API --> B
    VFS --> A
    A --> B
    B --> C

    C --> REGEDIT
    B --> REGFLUSH
    A --> REGSAVE
    C --> PERF_MMAP
    C --> PERF_BTREE
```

### Phase-by-Phase Implementation Order

| ⭐  | Phase  | Section                       | Description                                                            | Depends On      | Status |
| --- | :----: | ----------------------------- | ---------------------------------------------------------------------- | --------------- | :----: |
| 💎  | **0**  | TODO-050.01 Registry Engine   | `reg_key_t`, `reg_value_t`, static pools, root keys                    | —               |   ✅   |
| 💎  | **0**  | TODO-050.02 Win32 Reg API     | `RegSetValueEx`, `RegDeleteValue` (trigger dirty marking)              | —               |   ✅   |
| 💎  | **1**  | §4.1 Hive File Format         | `hive_header_t` (4 KiB), `hive_save`, `hive_load`, CRC32 validation   | Phase 0         |   ✅   |
| 💎  | **2**  | §4.2 Disk Layout              | `hive_table[4]`, `registry_flush`, `registry_mark_dirty`, auto-flush   | Phase 1 (§4.1)  |   ✅   |
| 💎  | **3**  | §4.3 Crash-Safe Journaling    | WAJ protocol: `.hive.log` → `.hive` → `.hive.bak`, `hive_best_source` | Phase 2 (§4.2)  |   ✅   |

> [!NOTE]
> **All phases complete.** The hive persistence system is fully implemented and verified.
>
> **Phase 1** established the binary hive format: `hive_header_t` (magic `"REGH"`,
> CRC32 checksum, 4096-byte page-aligned header), `hive_serialize_key` (depth-first
> tree serialization), and `hive_load` (validation + deserialization).
>
> **Phase 2** added the on-disk layout: 4 hive files under
> `C:\Impossible\System\Config\Registry\`, dirty-flag tracking per hive via
> `registry_mark_dirty()` (walks parent chain to find root), and `registry_flush()`
> called from the compositor loop to lazily write only dirty hives.
>
> **Phase 3** added Write-Ahead Journaling (WAJ): the 4-step save protocol
> (write `.hive.log` → backup `.hive.bak` → overwrite `.hive` → invalidate log),
> and the boot-time recovery via `hive_best_source()` which checks
> `.hive.log` → `.hive` → `.hive.bak` in priority order.

> [!TIP]
> **PMM buffer for serialization:** `hive_save` allocates the I/O buffer via
> `pmm_alloc_contiguous()`, not `kmalloc`, to avoid heap overflow on large hives.
>
> **CRC32 algorithm:** Uses table-less CRC32 with polynomial `0xEDB88320` —
> no lookup table needed, saves 1 KiB of static data.
>
> **Dirty tracking hook points:** `registry_mark_dirty()` is called from
> `RegSetValueEx` and `RegDeleteValue` — these are the only write paths.

---

## 1. Hive File Format

### 4.1 Hive File Format *(done)* ✅

**Prompt:** Verify the hive file format implementation. In `registry.h`, confirm `hive_header_t` is defined as a packed struct with `magic` (HIVE_MAGIC = 0x48474552), `version` (1), `checksum` (CRC32), `timestamp`, `root_name[64]`, `total_keys`, `total_values`, `data_offset`, `data_size`, and `padding` to 4096 bytes. Confirm `hive_save` and `hive_load` are declared. In `registry.c`, confirm `hive_crc32` implements table-less CRC32 with polynomial 0xEDB88320. Confirm `hive_serialize_key` writes depth-first key records as `[name_len:u16][name:N][value_count:u16][child_count:u16]` and value records as `[name_len:u16][name:N][type:u32][data_size:u32][data:N]`. Confirm `hive_save` uses PMM for the buffer, fills the header, computes CRC32, and writes via VFS. Confirm `hive_load` validates magic, version, and CRC32, and returns -1 with a klog warning on corrupt files. Run `bash scripts/build.sh clean` and confirm zero warnings.

> [!NOTE]
> **Implementation Notes:**
> - `hive_header_t`: packed struct, 4096 bytes, magic `"REGH"` (0x48474552)
> - CRC32: table-less with polynomial `0xEDB88320` — `hive_crc32()` in `registry.c`
> - Serialization: depth-first, format: `[name_len:u16][name:N][value_count:u16]
>   [child_count:u16]` per key, `[name_len:u16][name:N][type:u32][data_size:u32][data:N]`
>   per value
> - `hive_save` allocates buffer via `pmm_alloc_contiguous()` (never `kmalloc`)
> - `hive_load` validates magic, version, CRC32 — returns `-1` on corrupt file

- [x] Define hive file header struct (4096 bytes):
  - [x] Magic: `"REGH"` (4 bytes)
  - [x] Version: `1` (uint32)
  - [x] Checksum: CRC32 of header (uint32)
  - [x] Timestamp: PIT ticks at save time (uint64)
  - [x] Root key name (64 bytes)
  - [x] Total key count (uint32)
  - [x] Total value count (uint32)
  - [x] Reserved padding to 4096 bytes
- [x] Implement `hive_save(root_key, filepath)` — serialize tree to file
- [x] Implement `hive_load(filepath, &root_key)` — deserialize file into tree
- [x] Add CRC32 checksum validation on load
- [x] Handle corrupt hive: log warning, skip file, use defaults
- [x] Commit: `"registry: hive file format"`

---

## 2. Disk Layout

### 4.2 Hive File Layout on Disk *(done)* ✅

**Prompt:** Verify the hive file disk layout. In `registry.h`, confirm `REG_HIVE_DIR` is `"C:\Impossible\System\Config\Registry"` and `REG_HIVE_COUNT` is 4. Confirm `registry_flush()`, `registry_save_all()`, and `registry_load_hives()` are declared. In `registry.c`, confirm `hive_table` maps 4 descriptors: SYSTEM.hive→HKLM\SYSTEM, SOFTWARE.hive→HKLM\SOFTWARE, HARDWARE.hive→HKLM\HARDWARE, DEFAULT.hive→HKU\Default. Confirm `registry_mark_dirty()` walks up the parent chain to mark the correct hive dirty. Confirm it's called from `RegSetValueEx` and `RegDeleteValue`. Confirm `registry_flush()` only writes dirty hives. Confirm `hive_ensure_dir()` creates the directory chain. In `main.c`, confirm `registry_flush()` is enabled (not commented out). Run `bash scripts/build.sh clean` and confirm zero warnings.

> [!NOTE]
> **Implementation Notes:**
> - `REG_HIVE_DIR` = `"C:\\Impossible\\System\\Config\\Registry"`
> - `REG_HIVE_COUNT` = 4
> - `hive_table[4]` maps: `SYSTEM.hive→HKLM\SYSTEM`, `SOFTWARE.hive→HKLM\SOFTWARE`,
>   `HARDWARE.hive→HKLM\HARDWARE`, `DEFAULT.hive→HKU\Default`
> - `registry_mark_dirty()` walks parent chain to root → marks correct hive dirty
> - `registry_flush()` only writes dirty hives — called from compositor loop
> - `registry_save_all()` writes all hives unconditionally — for clean shutdown
> - `hive_ensure_dir()` creates `C:\Impossible\System\Config\Registry\` on first boot

- [x] Define hive file paths:
  - [x] `C:\Impossible\System\Config\Registry\SYSTEM.hive` → HKLM\SYSTEM
  - [x] `C:\Impossible\System\Config\Registry\SOFTWARE.hive` → HKLM\SOFTWARE
  - [x] `C:\Impossible\System\Config\Registry\HARDWARE.hive` → HKLM\HARDWARE
  - [x] `C:\Impossible\System\Config\Registry\DEFAULT.hive` → HKU\Default
- [x] Create Registry directory at first boot if missing
- [x] Implement dirty-flag tracking per hive
- [x] Implement `registry_flush()` — write only dirty hives
- [x] Implement `registry_save_all()` — for clean shutdown
- [x] Hook dirty tracking into `RegSetValueEx` and `RegDeleteValue`
- [x] Enable `registry_flush()` in `main.c` compositor loop
- [x] Commit: `"registry: hive file disk layout"`

---

## 3. Crash-Safe Journaling

### 4.3 Crash-Safe Journaling *(done)* ✅

**Prompt:** Verify crash-safe journaling. In `registry.c`, confirm `hive_save` follows the 4-step sequence: (1) write `.hive.log`, (2) copy `.hive` → `.hive.bak`, (3) overwrite `.hive`, (4) invalidate `.hive.log` by zeroing magic. Confirm `hive_validate_file` checks magic, version, and CRC32. Confirm `hive_best_source` checks `.hive.log` → `.hive` → `.hive.bak` in priority order and copies the best source to `.hive`. Confirm `registry_load_hives` calls `hive_best_source` for each hive before loading. Confirm `hive_copy_file` does a byte-by-byte copy via VFS. Run `bash scripts/build.sh clean` and confirm zero warnings.

> [!NOTE]
> **Implementation Notes:**
> - **WAJ 4-step save protocol:**
>   1. Write new data to `.hive.log` (journal-first)
>   2. Copy old `.hive` → `.hive.bak` (backup)
>   3. Overwrite `.hive` with new data
>   4. Invalidate `.hive.log` by zeroing magic byte
> - `hive_validate_file()` checks: magic, version, CRC32
> - `hive_best_source()` priority: `.hive.log` → `.hive` → `.hive.bak`
> - `hive_copy_file()` does byte-by-byte copy via VFS
> - Recovery is automatic at boot — no user interaction required

- [x] Before saving: write new data to `.hive.log` first (journal)
- [x] After log + main hive written: invalidate `.hive.log` by zeroing magic
- [x] On boot: if `.hive.log` has valid header, replay it (crash recovery via `hive_best_source`)
- [x] On boot: if `.hive` is corrupt (bad CRC), fall back to `.hive.bak`
- [x] Keep one backup: copy old `.hive` → `.hive.bak` before overwriting
- [x] Commit: `"registry: crash-safe journaling"`

---

## Priority Order

| Priority  | Section                                | Description                                                      |
| --------- | -------------------------------------- | ---------------------------------------------------------------- |
| ✅ Done   | §4.1 Hive File Format                  | Binary format: header, serialization, CRC32 validation           |
| ✅ Done   | §4.2 Disk Layout                       | 4-hive table, dirty tracking, lazy flush, auto-create directory  |
| ✅ Done   | §4.3 Crash-Safe Journaling             | WAJ protocol: `.hive.log` → `.hive` → `.hive.bak`               |

> All items are ✅ complete.

---

## OS Comparison

| Feature                                   | 🪟 Windows 11                               | 🐧 Linux                                    | 🚀 Impossible OS                                        |
| ----------------------------------------- | ------------------------------------------- | -------------------------------------------- | ------------------------------------------------------- |
| Persistent hive file format               | ✅ REGF binary format (4 KiB base block)     | ❌ No hive concept (dconf binary db)          | ✅ §4.1 — `hive_header_t` (4 KiB, CRC32, page-aligned)  |
| Multiple hive files per root key          | ✅ SYSTEM, SOFTWARE, SAM, SECURITY, DEFAULT  | ❌ No concept                                 | ✅ §4.2 — 4 hive files under `Config\Registry\`          |
| Dirty-flag lazy write (only changed)      | ✅ Configuration Manager lazy writer          | ⚠️ dconf background flush (user-space)       | ✅ §4.2 — `registry_mark_dirty` + compositor flush       |
| CRC32 checksum validation                 | ✅ XOR-32 on Base Block                       | ❌ No checksum on dconf                       | ✅ §4.1 — CRC32 on header (0xEDB88320 polynomial)        |
| Crash-safe transaction log                | ✅ `.log1` / `.log2` dual-log strategy        | ❌ No crash-safe guarantee on dconf           | ✅ §4.3 — WAJ: `.hive.log` + `.hive.bak`                 |
| Automatic crash recovery at boot          | ✅ Sequence number mismatch → replay logs     | ❌ No boot-time recovery                      | ✅ §4.3 — `hive_best_source` auto-recovery               |
| Backup hive (`*.bak`)                     | ⚠️ `RegBack` folder (deprecated Win10+)      | ❌ No backup                                  | ✅ §4.3 — `.hive.bak` on every save                      |
| **Kernel-space WAJ (not user-space)**     | ✅ Kernel Configuration Manager               | ❌ dconf runs in user-space                   | ✅ §4.3 — **in-kernel WAJ, zero daemon overhead** 🚀     |
| **PMM-based I/O buffer (no heap)**        | ❌ Dynamic paged pool allocation              | ❌ malloc-based                               | ✅ §4.1 — **pmm_alloc_contiguous, zero heap pressure** 🚀|
| **Table-less CRC32 (no lookup table)**    | ❌ Uses 1 KiB lookup table                    | ❌ Uses lookup table                          | ✅ §4.1 — **saves 1 KiB static data** 🚀                 |
| **Auto-create config directory**          | ✅ Created by setup                            | ❌ Created by package manager                 | ✅ §4.2 — **`hive_ensure_dir` on first boot** 🚀         |

> **Current state:** Impossible OS provides full hive persistence with crash-safe journaling.
> The in-kernel WAJ protocol, PMM-based I/O buffers, and auto-directory creation are
> exclusive advantages over both Windows (which uses paged pool allocation and deprecated
> RegBack) and Linux (which has no in-kernel registry persistence).
