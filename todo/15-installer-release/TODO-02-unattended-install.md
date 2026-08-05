---
schema_version: 1
id: unattended-install
domain: 15-installer-release
status: active
title: "TODO-02 -- Unattended Installation & Deployment"
---

# TODO-02 -- Unattended Installation & Deployment

> **Goal:** Add unattended installation (answer files), sysprep/generalize, OEM
> preload support, WIM image capture, PXE network boot, and VM provisioning templates
> on top of the interactive installer -- covering every automated deployment scenario
> from CI/CD to hardware partner imaging.

> [!IMPORTANT]
> **Prerequisite**: `10-platform-services/TODO-11` -- interactive installer wizard,
> `gpt_create`/`ixfs_format`/`fat32_format`, `installer_copy_files`, `InstallerMode`
> Registry flag, `setup.log`. This TODO adds the unattended execution path through the
> same pipeline -- do not re-specify partitioning, format, or file-copy logic.
>
> **OOBE trigger** (`HKLM\SYSTEM\FirstBoot=1`) and first-boot wizard are owned by
> `10-platform-services/TODO-04 §1`; `auth_create_user()` is owned by `TODO-06`.
> Answer-file user creation (§2) calls these existing APIs -- do not re-specify them.
>
> **Kernel cmdline** is available via `boot_info->cmdline[BOOT_CONF_CMDLINE_MAX]`
> (from `include/kernel/boot_info.h`); installer reads `answer=<path>` token from it.
>
> **`ipkg_create.exe`** (for OEM package bundling) is specced in
> `11-user-platform-sdk/TODO-06 §1`; §4 here consumes it.

---

## Inputs

- `10-platform-services/TODO-11-installer-iso.md` (→ XREF) -- `installer_copy_files`, `gpt_create`, `ixfs_format`, `InstallerMode`, `setup.log`; unattended path (§7) runs through same pipeline
- `10-platform-services/TODO-04-restore-recovery.md §1` (→ XREF) -- OOBE (`HKLM\SYSTEM\FirstBoot=1`); answer-file sets values OOBE would collect
- `10-platform-services/TODO-06-auth-security.md` (→ XREF) -- `auth_create_user(username, password, privilege)` -- §2 §3
- `11-user-platform-sdk/TODO-06-sdk-distribution.md §1` (→ XREF) -- `ipkg_create.exe` for OEM package format -- §4
- `include/kernel/boot_info.h` -- `boot_info->cmdline` -- §2 `answer=<path>` kernel cmdline token
- `include/kernel/fs/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_readdir`, `vfs_unlink` -- §1 §2 §4 §5
- `include/kernel/ipc/` -- `SYS_SHMEM_CREATE/MAP` -- §6 TFTP/DHCP server IPC
- `include/libs/monocypher.h` -- `csprng_fill()` for machine SID generation -- §3
- `include/kernel/net/` -- UDP socket for TFTP server (§6)

---

## Outcome

A machine booted from the release ISO with an `answer.ini` on the same media installs
Impossible OS fully unattended -- partitions, formats, copies files, creates user, sets
hostname, reboots -- without a single keypress. `sysprep /generalize` creates a clean
deployment image. OEM partners place `$OEM$\` content on the media and it flows into
the install. `pxesrv start` serves netboot images over TFTP to diskless clients.

---

## Implementation Order

| Step | Section                                | 💎/⭐ | Dependency                                                        |
| ---- | -------------------------------------- | ----- | ----------------------------------------------------------------- |
| 1    | Answer file format + INI parser        | ⭐    | `vfs_read`; `boot_info->cmdline`                                  |
| 2    | Unattended install mode                | 💎    | §1; `D10T11` installer pipeline; `auth_create_user`               |
| 3    | `sysprep.exe` (generalize + SID clear) | 💎    | `auth_create_user` (`D10T06`); `csprng_fill` for SID              |
| 4    | OEM customization (`$OEM$` layout)     | ⭐    | §1 answer file; `ipkg_create.exe` (D12T06 §1)                     |
| 5    | WIM / image capture (`imagex.exe`)     | 💎    | `vfs_readdir` full tree; `monocypher` SHA-1 for single-instancing |
| 6    | Network boot (PXE + TFTP server)       | 💎    | §2 unattended; UDP sockets; DHCP proxy                            |
| 7    | VM provisioning templates              | ⭐    | §2 unattended; release ISO (TODO-01 §4)                           |
| 8    | Enterprise deployment guide            | ⭐    | §1–§7 all done                                                    |

---

## 1. Answer File Format + INI Parser `[Sonnet]`

**Source:** `src/installer/answer.c`; header `include/installer/answer.h`

- [ ] **`answer.ini` INI format** -- parsed by `answer_parse(path)`:
  ```ini
  [Setup]
  Language     = en-US
  Timezone     = UTC+0
  KeyboardLayout = US
  InstallMode  = Auto          ; Auto (unattended) | Interactive (force wizard)

  [Disk]
  DiskIndex    = 0             ; 0 = first detected disk
  PartitionScheme = GPT
  EFISize      = 512           ; MB
  SystemFS     = IXFS          ; IXFS | NTFS (NTFS not yet supported -- reserved)

  [User]
  Username     = admin
  PasswordHash = <argon2i hex> ; output of argon2i(password, machine_salt); or plain "Password=abc"
  Privilege    = Admin         ; Admin | User
  AutoLogin    = 0

  [Network]
  DHCP         = 1
  StaticIP     =               ; ignored if DHCP=1; e.g. 192.168.1.50/24
  Gateway      =
  DNS          =

  [OEM]
  CompanyName  =
  SupportURL   =
  WallpaperPath=               ; path on media (relative to media root)

  [Packages]
  InstallList  =               ; comma-separated .ipkg paths or URLs
  ```
- [ ] **`answer_t` struct** and **`answer_parse(const char *path, answer_t *out)`**:
  - Simple line-by-line parser: skip `#` + `;` comments; parse `[Section]` headers; split `Key=Value` on first `=`; strip leading/trailing whitespace
  - Return 0 on success; `-ENOENT` if file not found; `-EINVAL` if required key missing
- [ ] **Answer file discovery** (in priority order):
  1. `boot_info->cmdline` token `answer=<path>` -- e.g., `answer=\answer.ini`
  2. `\answer.ini` at root of installation media (scan by VFS path)
  3. `HKLM\SYSTEM\UnattendedSetup\AnswerFile` Registry string value
  - If `InstallMode=Interactive` in answer file: treat as if no answer file (force wizard)
- [ ] **Plain password fallback**: if `Password=<plaintext>` key present (no hash): compute Argon2i hash at install time with machine-generated salt; store hash in `HKCU\Security\Credentials`; never store plaintext after this point

---

## 2. Unattended Install Mode `[Sonnet]`

> Extends `10-platform-services/TODO-11` installer pipeline; auto-proceeds through
> every step that would otherwise show a UI dialog.

**Source:** extends `src/installer/installer.c`

- [ ] **Answer-file check at installer start**: `answer_parse()` (§1); if answer file found and `InstallMode != Interactive`: set `g_unattended = 1`; skip all `wm_create_window` / `CTRL_*` UI creation; log `[setup] Unattended mode: answer file {path}`
- [ ] **Unattended execution path** (mirrors wizard pages from `TODO-11 §5`, executed sequentially):
  1. Disk selection: use `answer.disk.disk_index`; validate `blkdev_count() > disk_index`; log selected disk
  2. Partition: call `gpt_create()` + `gpt_add_partition(EFI, efi_size_mb)` + `gpt_add_partition(SYSTEM, remaining)`; log partition layout
  3. Format: `fat32_format(esp, "ESP")` + `ixfs_format(sys, "Impossible")`; log
  4. Copy files: `installer_copy_files(manifest, unattended_progress_cb)` where `unattended_progress_cb` logs `"[setup] Copying: {file} ({pct}%)"` to `setup.log` every 5%
  5. User creation: `auth_create_user(answer.user.username, answer.user.password_hash, answer.user.privilege)` -- passes pre-hashed password directly (skip re-hash if `PasswordHash=` key present)
  6. Network: if `DHCP=0`: write static IP to `HKLM\SYSTEM\Network\StaticIP` etc.; if `DHCP=1`: write `HKLM\SYSTEM\Network\DHCP=1` (DHCP configured at next boot)
  7. AutoLogin: if `AutoLogin=1`: `reg_set_string(HKLM, "SOFTWARE\\Impossible\\AutoLogin", answer.user.username)`
  8. OEM setup: call `oem_apply(answer)` (§4)
  9. Packages: for each path in `InstallList`: `ipkg_install(path)` (calls `ipkg_create.exe` extract logic)
  10. Set `HKLM\SYSTEM\FirstBoot=0` (unattended already configured user -- skip OOBE); or `=1` if `[User]` section absent
  11. Write `HKLM\SYSTEM\InstallerMode=0`; auto-restart: `reg_set_string(HKLM, "SYSTEM\\PendingReboot", "1")` + `EFI_ResetSystem(RESET_WARM)`
- [ ] **`setup.log`** at `X:\Logs\setup.log`: append timestamped lines; each step logs start + completion + any error; readable after first boot
- [ ] **Error handling**: on any step failure: log error to `setup.log` + serial; halt with `"[setup] FATAL: {step} failed -- installation aborted"` + print to console; do NOT auto-restart on error (leaves system in diagnosable state)

---

## 3. `sysprep.exe` (Generalize + OOBE Seal) `[Opus]`

> Novel: machine SID generation and hardware-specific data removal. No prior Impossible
> OS sysprep. Security-sensitive: SID uniqueness must be guaranteed across cloned VMs.

**Source:** `src/tools/sysprep/sysprep.c`

- [ ] **`sysprep.exe /generalize /oobe /shutdown`** command-line interface:
  - Parse flags: `/generalize` (machine-specific data removal); `/oobe` (set FirstBoot); `/shutdown` or `/reboot` (post-sysprep action); `/audit` (skip /oobe -- boot to admin session)
  - Require Administrator privilege: check `HKCU\...\Privilege == Admin`; refuse with error if not
- [ ] **`/generalize` -- machine-specific data removal**:
  - **Machine SID**: generate new 96-bit random SID via `csprng_fill(sid_bytes, 12)`; encode as `S-1-5-21-{rand1}-{rand2}-{rand3}`; write to `HKLM\SYSTEM\MachineGUID` (GUID format) + `HKLM\SAM\SID`; old SID is discarded
  - **Hostname**: clear `HKLM\SYSTEM\ComputerName` → set to `"IMPOSSIBLEOS"` (default); will be set at OOBE or by admin after install
  - **Hardware fingerprint**: clear `HKLM\SYSTEM\HardwareID` (computed from SMBIOS UUID + disk serial at first boot); regenerated on next boot
  - **Network adapter GUIDs**: clear `HKLM\SYSTEM\Network\Adapters\*` -- regenerated at next boot when NICs are re-enumerated
  - **Cached credentials**: `auth_clear_all_sessions()`; clear `HKCU\Security\SessionTokens\*`
  - **Event logs**: truncate `X:\Logs\*.log` to zero bytes; clear `HKLM\SYSTEM\EventLog\*` ring entries
  - **Temp/cache**: `vfs_unlink_tree("C:\\Temp\\")` + recreate empty `C:\Temp\`; clear `C:\Users\*\AppData\Temp\`
  - Log all cleared items to `X:\Logs\sysprep.log`
- [ ] **`/oobe`**: `reg_set_dword(HKLM, "SYSTEM\\FirstBoot", 1)` -- OOBE triggers on next boot
- [ ] **`/audit`**: set `HKLM\SYSTEM\AuditMode=1` -- boot to admin desktop instead of OOBE; used by OEMs to install drivers before sealing
- [ ] **`/shutdown`** / **`/reboot`**: after all steps: print summary + `EFI_ResetSystem(RESET_SHUTDOWN)` or `RESET_WARM`
- [ ] **`sysprep.cpl`** Control Panel entry: simple dialog with "Generalize + OOBE + Shutdown" button + checkbox options; calls `CreateProcess("sysprep.exe /generalize /oobe /shutdown")`

---

## 4. OEM Customization `[Sonnet]`

**Source:** `src/installer/oem.c`; media layout convention `\$OEM$\`

- [ ] **`$OEM$` media layout** (conventional paths on installation media root):
  ```
  \$OEM$\
  ├── Drivers\          ← *.kmod files → auto-loaded by installer via kmod_load()
  ├── $1\               ← copies to C:\ on target (mirrors drive root)
  │   ├── registry.reg  ← Registry tweaks applied post-install
  │   └── Wallpaper\    ← extra wallpapers copied to C:\Impossible\Web\Wallpaper\
  └── Packages\         ← *.ipkg files → ipkg_install() called for each
  ```
- [ ] **`oem_apply(answer_t *answer, const char *media_root)`**:
  1. Check `[OEM] CompanyName` → `reg_set_string(HKLM, "SYSTEM\\OEM\\Name", company_name)`
  2. Check `[OEM] SupportURL` → `reg_set_string(HKLM, "SYSTEM\\OEM\\SupportURL", url)`
  3. Check `[OEM] WallpaperPath` → `vfs_copy(media_root + wallpaper_path, "C:\\Impossible\\Web\\Wallpaper\\oem-default.jpg")`; `reg_set_string(HKLM, "SOFTWARE\\Impossible\\Wallpaper", "oem-default.jpg")`
  4. Scan `\$OEM$\Drivers\*.kmod` → `kmod_load(path)` for each; log loaded driver names
  5. Copy `\$OEM$\$1\` tree to `C:\` (mirrors recursively via `vfs_copy_tree`)
  6. Apply `\$OEM$\$1\registry.reg` if present: parse `.reg` text format (lines: `[KEY]`, `"Name"="Value"`, `"Name"=dword:XXXXXXXX`); call `reg_set_string/dword` for each entry
  7. Install `\$OEM$\Packages\*.ipkg` via `ipkg_install(path)` for each
- [ ] **OEM info shown in System Properties**: `sysdm.cpl` reads `HKLM\SYSTEM\OEM\{Name, SupportURL, Logo}` and displays in "Manufacturer" / "Support" rows -- shown when OEM info is present, hidden when blank

---

## 5. WIM / Image Capture (`imagex.exe`) `[Opus]`

> Novel: WIM single-instancing (SHA-1 hash deduplication). No prior Impossible OS file
> archive with per-file identity-based deduplication. Custom `.iim` format if full WIM
> compatibility is not feasible.

**Source:** `src/tools/imagex/imagex.c`; header `include/tools/imagex.h`

- [ ] **WIM / `.iim` format overview**:
  - Single-instance storage: each unique file stored once; referenced by hash
  - Directory tree stored separately (metadata only); file data in deduplicated blob pool
  - Header: magic `"IIMF"` (4 bytes), version, image count, metadata offset, blob pool offset
  - Metadata: recursive directory tree (dir entries with file hash + size + attributes)
  - Blob pool: SHA-1 (via `cng_sha256` fallback using BLAKE2b truncated -- note: true SHA-1 needs adding to `TODO-07 §2` if WIM compatibility required; use BLAKE2b-160 as Impossible OS native)
- [ ] **`imagex.exe /capture <src> <dst.iim> <name> [/compress fast|max|none]`**:
  1. Walk `src` directory tree via `vfs_readdir` recursively; build in-memory dir tree
  2. For each file: read content; compute BLAKE2b-160 hash; check blob pool map for existing entry
  3. If new hash: write blob to pool at current offset; add `hash → offset` to pool index
  4. Build metadata tree with `{name, hash, size, attribs, mtime}` per file
  5. Compress blob pool with miniz `mz_stream_deflate` if `/compress fast` or `max`
  6. Write header + metadata + blob pool to `dst.iim`
  7. Report: `"Captured {file_count} files, {unique_count} unique blobs, {size_bytes} → {compressed_bytes} ({ratio}%)"`
- [ ] **`imagex.exe /apply <src.iim> <dst>`**:
  1. Parse header + metadata tree; for each file entry: look up blob by hash in pool
  2. Extract blob (decompress if needed); write to `dst/{path}` via `vfs_open` + `vfs_write`
  3. Restore attributes + mtime (if VFS supports `vfs_set_attribs`)
- [ ] **`imagex.exe /info <src.iim>`**: print image name, file count, total uncompressed size, unique blob count, compression ratio
- [ ] **Use cases**: capture installed `C:\` partition after `sysprep /generalize` → `.iim` file → deploy to new disk via `imagex /apply`

---

## 6. Network Boot (PXE + TFTP Server) `[Opus]`

> Novel: DHCP proxy + TFTP server. No prior Impossible OS network boot infrastructure.
> Requires UDP raw sockets and correct DHCP option parsing.

**Source:** `src/apps/pxeserver/pxeserver.c`

- [ ] **TFTP server** (`RFC 1350`):
  - Listen on UDP port 69; handle `RRQ` (read request) packets only (TFTP read)
  - Serve files from TFTP root directory (configurable: `HKLM\SOFTWARE\Impossible\PXE\TFTPRoot`, default `C:\Impossible\PXE\`)
  - Transfer in 512-byte blocks; send `DATA` packets; wait for `ACK` per block; retransmit on timeout (3s, 3 retries)
  - Support `octet` mode only (binary); reject `netascii`/`mail`
  - Serve: `EFI/BOOT/BOOTX64.EFI`, `boot/kernel.exe`, `answer.ini` (from TFTP root)
- [ ] **DHCP proxy** (PXE boot extension):
  - Listen on UDP port 67 (broadcast); detect DHCP `DISCOVER` packets with option 60 (`PXEClient` vendor string)
  - Respond with `DHCPOFFER` containing: option 43 (PXE-specific), option 54 (server IP), option 60 (`PXEClient`), `siaddr` = TFTP server IP, `file` field = `EFI/BOOT/BOOTX64.EFI`
  - Does NOT replace the real DHCP server -- only adds PXE options (proxy mode)
  - Requires a real DHCP server on the same segment for IP assignment
- [ ] **`pxesrv start/stop/status` shell command**:
  - `pxesrv start`: initialize TFTP + DHCP proxy; log `"PXE server started: TFTP root={path}, IP={local_ip}"`
  - `pxesrv stop`: send shutdown signal to pxeserver process; log
  - `pxesrv status`: print current state + TFTP root + number of active transfers + connected clients
- [ ] **Kernel cmdline injection**: BOOTX64.EFI receives `answer=\answer.ini` in kernel cmdline field via DHCP option or a separate TFTP-fetched `pxe-config.txt` file; unattended install proceeds automatically
- [ ] **TFTP root layout** for PXE boot:
  ```
  C:\Impossible\PXE\
  ├── EFI\BOOT\BOOTX64.EFI
  ├── boot\kernel.exe
  └── answer.ini              ← optional; triggers unattended install
  ```
- [ ] **Registry config**: `HKLM\SOFTWARE\Impossible\PXE\TFTPRoot`, `TFTPPort` (default 69), `DHCPProxyEnabled` (default 1), `ServerIP` (blank = auto-detect from NIC)

---

## 7. VM Provisioning Templates `[Sonnet]`

**Source:** `scripts/provision-qemu.sh`; `scripts/provision-hyperv.ps1`

> -> XREF: developer-local QEMU/VirtualBox/secure-boot launchers are owned by [`00-infrastructure/TODO-01 §4`](../00-infrastructure/TODO-01-developer-tooling-stack.md#4-machine-launcher-and-debug-profile-matrix) and documented in [`docs/infrastructure/machine-matrix.md`](../../docs/infrastructure/machine-matrix.md). Provisioning templates in this section target answer-file-driven unattended installs; they are not a substitute for the developer matrix and must not inline its launcher tables.

- [ ] **`scripts/provision-qemu.sh <answer.ini> [--disk-size 8G] [--name vm1]`**:
  1. Create QCOW2 disk: `qemu-img create -f qcow2 "${name}.qcow2" "${disk_size}"`
  2. Build QEMU command: `-drive file=${name}.qcow2,if=virtio -cdrom impossible-os-{ver}.iso -m 512 -cpu qemu64 -bios OVMF.fd -display none -serial stdio -append "answer=/answer.ini"`
  3. Copy `answer.ini` into ISO (via loop mount + `mcopy`) OR pass via `-smbios type=11,value=answer=<base64>` (stretch)
  4. Run QEMU; tail `-serial stdio` output; wait for `PendingReboot=1` registry write signal (detected via QEMU serial `"[setup] Unattended install complete"` string)
  5. `pkill qemu`; print `"Provisioned: ${name}.qcow2 ready"`
- [ ] **`scripts/provision-hyperv.ps1 -AnswerFile <path> [-DiskSize 8GB] [-VMName VM1]`**:
  - `New-VHD -Path "${VMName}.vhdx" -SizeBytes ${DiskSize} -Dynamic`
  - `New-VM -Name $VMName -Generation 2 -MemoryStartupBytes 512MB -VHDPath "${VMName}.vhdx"`
  - `Set-VMFirmware -VMName $VMName -EnableSecureBoot Off`
  - `Add-VMDvdDrive -VMName $VMName -Path "impossible-os-{ver}.iso"`
  - `Start-VM $VMName`; `Wait-VM -State Off -Timeout 600` (10 min timeout)
  - Print `"Provisioned: ${VMName}.vhdx ready"`
- [ ] **Provisioning validation**: after QEMU/Hyper-V boots provisioned disk (without ISO): `run_tier.sh 1 tier1_exit.exe` passes (confirms OS booted correctly)
- [ ] **CI integration**: `scripts/ci-provision.sh`: calls `provision-qemu.sh` with a standard `ci-answer.ini` (CI-specific user/disk settings); used by GitHub Actions to spin up fresh OS instances for regression tests

---

## 8. Enterprise Deployment Guide `[Sonnet]`

**Source:** `docs/guides/enterprise-deployment.md`

- [ ] **Guide structure and content**:
  - **§1 Answer file reference**: full table of every key in every section, with description, type, default, and example
  - **§2 Unattended install walkthrough**: step-by-step from ISO + answer.ini to first boot; common failure scenarios and troubleshooting
  - **§3 sysprep guide**: when to generalize, what is cleared, how to use `/audit` for driver installation before sealing; creating a golden image
  - **§4 WIM capture and deploy**: capture after sysprep → deploy to multiple disks; use cases for department-specific images
  - **§5 PXE boot setup**: prerequisites (DHCP server on LAN), configure TFTP root, network boot sequence, troubleshooting
  - **§6 OEM customization hooks**: `$OEM$` directory layout, `registry.reg` format, driver pre-loading, wallpaper + branding
  - **§7 QEMU farm example**: provision 10 VMs in parallel using `provision-qemu.sh` + `xargs`; CI/CD integration
  - **§8 Hyper-V cluster example**: PowerShell script for 20-node Hyper-V deployment; WDS alternative discussion
- [ ] **Answer file template**: `docs/guides/answer-template.ini` -- fully commented template with all keys and their defaults; linked from README
- [ ] **Troubleshooting table**: common errors (disk index out of range, hash mismatch, driver load failure, TFTP timeout) with resolution steps

---

## OS Comparison


| ⭐ | Feature                                  | 🪟 Win11                                               | 🐧 Linux                                             | 🚀 Impossible OS                                                                    |
|----|------------------------------------------|-----------------------------------------------------|---------------------------------------------------|----------------------------------------------------------------------------------|
| 💎 | Unattended install via answer file       | ✅ `unattend.xml`; Windows SIM; WDS                 | ✅ Kickstart (RHEL); `preseed` (Debian); AutoYaST | ⬜ §1 -- §2; `answer.ini` INI format; auto-proceed                                |
| 💎 | `sysprep /generalize`                    | ✅ `sysprep.exe /generalize`; full SID regeneration | ✅ `virt-sysprep`; cloud-init; `cloud-utils`      | ⬜ §3 -- CSPRNG 96-bit SID; hostname/GUID/credential/log clear                    |
| ⭐ | OEM `$OEM$` customization layer          | ✅ OEM `$OEM$` dirs; `setupcomplete.cmd`            | ✅ Kickstart `%post`; OEM preseed                 | ⬜ §4 -- `$OEM$\Drivers\*.kmod` auto-load; `registry.reg` injection; `ipkg`       |
| 💎 | WIM/image capture with single-instancing | ✅ WIM (wimlib, DISM); DISM `/add-package`          | ✅ `squashfs`; `dd`; Clonezilla                   | ⬜ §5 -- `.iim` BLAKE2b-160 deduplication; `imagex /capture`                      |
| 💎 | PXE + TFTP server for netboot            | ✅ WDS (Windows Deployment Services)                | ✅ `dnsmasq` + TFTP; PXElinux; iPXE               | ⬜ §6 -- DHCP proxy + TFTP server                                                 |
| ⭐ | VM provisioning scripts                  | ✅ Hyper-V PowerShell; Azure ARM templates          | ✅ `virt-install`; Vagrant; Packer                | ⬜ §7 -- `provision-qemu.sh` + `provision-hyperv.ps1`; CI-ready `ci-provision.sh` |
| 💎 | Enterprise deployment documentation      | ✅ MSDN WDS/MDT/SCCM docs                           | ✅ Anaconda/Kickstart/AutoYaST docs               | ⬜ §8 -- `docs/guides/enterprise-deployment.md`; full answer file reference       |

Impossible OS's `⭐` advantage: the entire deployment pipeline -- answer file, sysprep,
OEM customization, PXE, and VM provisioning -- is built into the OS itself with no
external tools (no MDT, no WDS, no Packer). The `$OEM$` layout directly mirrors
Windows OEM conventions so hardware partners need zero re-training, and BLAKE2b-160
single-instancing in `.iim` is faster than SHA-1 WIM while using the same monocypher
library already in the kernel.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **Answer file parse**: valid `answer.ini` → `answer_parse` returns 0; check all fields populated; invalid key → graceful default (not crash); missing required section → `-EINVAL` + log
- [ ] **Unattended install (QEMU)**: `scripts/provision-qemu.sh ci-answer.ini` → QEMU boots ISO, serial shows `[setup] Unattended mode: answer file`, each step logged; boot from provisioned disk → kernel boots, user from answer file exists, OOBE skipped
- [ ] **Unattended install error**: answer file with `DiskIndex=99` → serial shows `[setup] FATAL: no disk at index 99` + system halts (does not reboot)
- [ ] **sysprep**: `sysprep.exe /generalize /oobe /shutdown` → `setup.log` shows all cleared items; reboot → OOBE triggers (`FirstBoot=1`); `HKLM\SYSTEM\MachineGUID` has new value; `HKLM\SYSTEM\ComputerName == "IMPOSSIBLEOS"`; two sysprep runs produce different `MachineGUID` values (CSPRNG uniqueness)
- [ ] **OEM customization**: create `$OEM$\Drivers\test.kmod` on media; run installer → driver loads; `lsmod` shows `test`; `HKLM\SYSTEM\OEM\Name` populated from `[OEM] CompanyName`
- [ ] **imagex capture+apply**: `imagex /capture C:\ test.iim "Test"` → file created; duplicate files stored once (verify blob pool size < raw size); `imagex /apply test.iim D:\` → files restored with matching sizes + hashes
- [ ] **PXE boot**: start `pxesrv start`; boot another QEMU VM with `-netdev user,bootfile=EFI/BOOT/BOOTX64.EFI,tftp=C:\Impossible\PXE`; UEFI loads `BOOTX64.EFI` from TFTP; kernel starts; `pxesrv status` shows 1 active transfer
- [ ] **VM provisioning**: `scripts/provision-qemu.sh ci-answer.ini` completes; `.qcow2` file exists; boot provisioned VM without ISO → OS boots to desktop
- [ ] Commit: `"installer: answer file, unattended install, sysprep, OEM customization, imagex, PXE netboot, VM provisioning"`
