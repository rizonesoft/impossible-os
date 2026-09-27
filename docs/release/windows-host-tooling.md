<!-- docs: covers=todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md -->
# Windows Host Tooling -- Release & Inspector Parity

> **Scope:** developer-facing reference for the Windows-host peers of the
> Linux release tooling. This document is the *single source of truth* for
> which Windows tools mirror which Linux tools, what the byte-parity
> contract is, and how to validate parity locally.
>
> **Owner:** TODO-06 boot-media-image-installer-handoff §12 (Manifest
> Tooling). Disk-converter peers (`write-usb.ps1`, `to-vhdx.ps1`,
> `to-iso.ps1`) and the test-subdir aggregate runner are owned by
> §13 / §14 and will be appended below as they ship.

## Manifest tooling

Three files define the Windows-host manifest workflow:

| Linux tool | Windows peer | Parity contract |
|---|---|---|
| `scripts/release/build-manifest.sh` | `scripts/release/build-manifest.ps1` | byte-identical `manifest.json` output for the same git SHA + same `--Format` |
| `scripts/release/test-build-manifest.sh` | `scripts/release/test-build-manifest.ps1` | same 23 assertions; same exit-code semantics |
| `tools/bootimg/bootimg.py` | `tools/bootimg/bootimg.bat` (shim) | the .bat is a host-resolver shim around `py -3` / `python.exe`; the Python tool itself is host-portable |

### Parity contract -- byte-identical output

Both peers MUST emit `manifest.json` files whose `sha256sum` matches when
run against the same git revision with the same `--Format` argument. This
keeps the artifact's identity (the deterministic `artifact_uuid` UUID v5)
host-agnostic: a Windows host releasing commit `abc123` with
`--Format raw` and a Linux host doing the same produce the *same* artifact
record. Without this guarantee, a release pipeline that ran some steps on
Windows and some on Linux would produce two manifests that *both claim to
describe the same image*, and a verifier could not tell which one was
canonical.

The parity is enforced mechanically by:

- **UUID v5 namespace pinned** -- both peers use `6f1b3c4a-1d4e-5a6b-8c9d-0e1f2a3b4c5d`
  with the same seed format `<format>|<bootloader_sha256>|<kernel_sha256>|<boot_info_version>`.
  PowerShell implements UUID v5 manually via `[System.Security.Cryptography.SHA1]`
  to match Python's `uuid.uuid5` byte-for-byte (network-order namespace bytes,
  UTF-8 name encoding, version 5 in byte 6, variant `10xx` in byte 8).
- **JSON whitespace pinned** -- both peers emit `indent=2`, no sorted keys
  (insertion order from `[ordered]` hashtable), `ensure_ascii=True`-style
  escaping. PowerShell ships a custom recursive serializer because
  `ConvertTo-Json` defaults to 4-space indent and reorders keys.
- **UTF-8 without BOM** -- both peers write UTF-8 with no byte-order mark
  and a single trailing `\n`. PowerShell uses
  `[System.IO.File]::WriteAllText($path, $content, (New-Object Text.UTF8Encoding $false))`
  to bypass `Out-File`'s default BOM behavior on PS 5.1.

### Validating parity locally

```sh
# On any host that has both bash and pwsh installed:
bash scripts/build.sh                         # produce build/kernel.exe + BOOTX64.EFI
bash tools/bootimg/tests/cross_host/test_manifest_parity.sh
```

Expected output on success:

```
[PASS] manifest parity: bash and pwsh produce byte-identical output (sha256=...)
```

The test SKIPs (exit 0) when `pwsh` is not on PATH (typical Linux dev
host) -- parity is then verified on PR CI matrix images that bundle
PowerShell. **Do not "fix" a parity failure by editing only one peer:**
diff the two outputs first to determine which side drifted, then update
the *correct* peer (or both, if the schema itself changed in this commit).

### Running the PowerShell test peer

On a Windows host with PowerShell 5.1 or later:

```powershell
pwsh -File scripts\release\test-build-manifest.ps1
```

Expected last line on a clean run: `[summary] 26 pass, 0 fail`. Exit
code 0. The test produces and validates manifest fixtures in a temp
directory which is removed on completion. (The bash peer at
`scripts/release/test-build-manifest.sh` runs 23 assertions; the PS1
peer adds three extra fixtures: one for the `size_mib > 2^32-1` upper-
bound parity check, and two for `total_sectors` at the
`Int64.MaxValue+1` and `UInt64.MaxValue` boundaries (validates that the
`Test-IsUInt64Range` helper bridges PowerShell's `ConvertFrom-Json`
upper-bound behavior to Python's unbounded-precision int parity).

### Running the offline inspector

The Python tool is cross-platform. On Windows, the `.bat` shim is a
convenience wrapper:

```cmd
tools\bootimg\bootimg.bat inspect build\release\disk.img
tools\bootimg\bootimg.bat verify build\release\disk.img --manifest build\artifacts\manifest.json
```

The shim resolves an interpreter via `py -3` (the Python launcher bundled
with python.org installers) and falls back to `python` on PATH. Exit code
127 if neither is found, with a `[ERROR] bootimg: no Python 3 interpreter`
message on stderr. The shim adds no logic of its own; all subcommand
behavior lives in `bootimg.py` and is identical on Windows and POSIX.

### Schema source of truth

The manifest schema itself is defined once at
[`docs/release/boot-artifact-manifest.md`](boot-artifact-manifest.md). Both
peers and the inspector validate against the same field set, the same
allowed-value enums, and the same cross-field consistency rules. When the
schema changes, all three implementations and the schema doc move
together in the same commit -- this is a hard invariant, not a guideline.

## Disk converters

Three Windows-host disk converter peers ship the same byte-content /
byte-parity contract as the manifest tooling above:

| Linux tool | Windows peer | Required Windows tool | Parity contract |
|---|---|---|---|
| `scripts/deploy/write-usb.sh` | `scripts/deploy/write-usb.ps1` | `mtype.exe` (mtools) | per-file sha256 of written ESP equals source ESP |
| `scripts/release/to-vhdx.sh` | `scripts/release/to-vhdx.ps1` | `qemu-img.exe` | byte-content equal (`qemu-img compare` exits 0) |
| `scripts/release/build-iso.sh` | `scripts/release/to-iso.ps1` | `xorriso.exe` + `pwsh` | sha256-equal Linux output when both run with `SOURCE_DATE_EPOCH=0` |

### Why qemu-img.exe and xorriso.exe (not Hyper-V / ADK)

The byte-parity contract pins each PS1 to the SAME tool the Linux peer
uses. The Windows-native alternatives (`Hyper-V` module + ADK
`oscdimg.exe`) are detected and reported as environment context, but
neither can satisfy the parity contract for these particular outputs:

- **`New-VHD` / `Convert-VHD`** -- `New-VHD -SourceDisk` accepts only
  physical disks (not raw .img files), and `Convert-VHD` requires VHD
  input not raw. There is no Windows-native cmdlet that takes a raw
  .img and produces a VHDX byte-equal to what `qemu-img convert -O
  vhdx` produces. Even if there were, the VHDX header carries
  per-conversion identifier fields (different between runs), so the
  byte-content gate uses `qemu-img compare` (which compares the
  *virtual* content) rather than `sha256sum` of the container.
- **`oscdimg.exe`** -- the Windows ADK ISO builder uses a different
  ISO9660 framing (different El Torito boot record placement,
  different Joliet directory record layout, different volume
  descriptor padding) than xorriso. Even with `SOURCE_DATE_EPOCH=0`
  pinning all timestamps, the bytes diverge. A Windows host using
  oscdimg would produce a "valid bootable ISO" but it would NOT be
  byte-equal to the Linux peer's output, breaking the cross-host
  reproducible-build claim.

The PS1 peers detect these tools' presence and write `[to-vhdx]` /
`[to-iso]` notes to stderr identifying what is installed; they do NOT
fall back to non-parity tools. If the byte-parity tool is missing, the
peer fails closed with `[ERROR]` and exit code 2.

### USB writer (`write-usb.ps1`)

Fail-closed shape:

- **Strict `BusType -eq 'USB'` filter.** No size or removable
  heuristics. If no USB disk is present, the script exits non-zero;
  it never falls back to "any small non-system disk". The pre-existing
  version (commit `c6a4dcca`, pre-§13) had a permissive fallback that
  could include internal SATA SSDs and SD card readers in the
  selection list.
- **TOCTOU re-check.** The selected disk's `BusType` and
  `FriendlyName` are re-queried via `Get-Disk -Number N` immediately
  before `Clear-Disk`. If a USB stick was unplugged between the menu
  and the write, with an internal disk taking its disk number, the
  second check catches it.
- **`#Requires -RunAsAdministrator`.** Raw `\\.\PhysicalDrive`N writes
  need elevation; PowerShell refuses to run the script unelevated.
- **Pre-write source ESP hash capture** via `mtype.exe` (mtools
  port). If `mtype` is missing, the script exits non-zero before
  touching the device -- there is no "skip verification, just write"
  path.
- **Fail-closed post-write sha256 verification** of `EFI\BOOT\BOOTX64.EFI`,
  `boot\kernel.exe`, and `EFI\ImpossibleOS\boot.conf`. Any missing
  file or hash mismatch -> exit 1. The `Done!` banner prints only
  after every check passes.

Invocation (from an elevated PowerShell window):

```powershell
pwsh -File scripts\deploy\write-usb.ps1
```

The script uses `build/release/disk.img` if present, falling back to
`build/system-disk.img`. Override with `-DiskImg <path>` if you need
to write a non-default image.

### VHDX converter (`to-vhdx.ps1`)

Mirrors the bash peer's argument surface and self-verifying contract:

```powershell
pwsh -File scripts\release\to-vhdx.ps1 --In build\release\disk.img --Out build\release\disk.vhdx --BlockSize 4194304
```

Post-convert verification runs `qemu-img info` (asserts `format: vhdx`)
and `qemu-img compare -f raw -F vhdx` (asserts byte-content
equivalence). `--NoVerify` disables the verifier for diagnostic runs.

### ISO converter (`to-iso.ps1`)

Mirrors the bash peer's pipeline: extract the ESP partition from the
raw image, stage `/IPOS/manifest.json` + `EFI/esp.img` + placeholder
installer/recovery directories, run `xorriso` with the same flags.

```powershell
pwsh -File scripts\release\to-iso.ps1 --In build\release\disk.img --Out build\release\disk.iso
```

The PS1 sets `SOURCE_DATE_EPOCH=0` + `TZ=UTC` + `LC_ALL=C` and touches
every staged file/directory mtime to the epoch BEFORE invoking xorriso,
so two consecutive runs from a clean tree produce byte-identical
output. If `bash.exe` is on PATH, the PS1 also runs the existing
`scripts/release/verify-esp.sh --manifest` cross-check; if not, the
cross-host ISO parity test (release-test runner) catches any drift by
sha256-comparing this peer's output against the Linux peer's.

### Prerequisites summary

| Tool | Where to get it | Required by |
|---|---|---|
| `mtype.exe` | mtools (Cygwin or MSYS2 package) | write-usb.ps1; to-iso.ps1 (when bash absent, for native manifest-to-ESP cross-check) |
| `qemu-img.exe` | qemu-tools or `choco install qemu-img` | to-vhdx.ps1 |
| `xorriso.exe` | Cygwin/MSYS2 xorriso package, or a standalone Windows xorriso build | to-iso.ps1 |
| `bash.exe` (optional) | Git for Windows / WSL | to-iso.ps1 (preferred manifest-to-ESP path; falls back to mtype.exe if absent) |
| `pwsh` (PowerShell 7+) | `winget install Microsoft.PowerShell` | all PS1 peers |
| Administrator elevation | `Run as administrator` | write-usb.ps1 only |

Optional informational tools (detected and reported, NOT used):

| Tool | Where to get it | Detection by |
|---|---|---|
| Hyper-V module | Windows Pro/Enterprise + Hyper-V feature enabled | to-vhdx.ps1 (env note only) |
| `oscdimg.exe` | Windows ADK | to-iso.ps1 (env note only) |

## Aggregate runner

`scripts\debug\release\` is the Windows-side test subdir for release
tooling, peer to the existing `kernel\` / `usermode\` / `desktop\`
subdirs. Per-artifact bat shims dispatch the matching PS1; the
aggregate `run-all-release-tests.bat` chains the non-destructive
shims; the cross-layer `scripts\debug\run-all-tests.bat` chains all
four per-layer aggregates with `if exist` so missing layers are
silent no-ops.

### Layout

| File | Role |
|---|---|
| `scripts\debug\release\run-build-manifest-tests.bat` | runs `test-build-manifest.ps1` (26 assertions) |
| `scripts\debug\release\run-write-usb.bat` | launches `write-usb.ps1` with UAC elevation (`Start-Process -Verb RunAs`); fire-and-forget -- the parent bat exits 0 once the elevation dialog is dispatched, regardless of whether the user accepts UAC or whether the elevated writer succeeds. Destructive + interactive, NOT chained by `run-all-release-tests.bat`; check the elevated PowerShell window's exit text for the actual write result. |
| `scripts\debug\release\run-to-vhdx.bat` | runs `to-vhdx.ps1` against `build\release\disk.img` |
| `scripts\debug\release\run-to-iso.bat` | runs `to-iso.ps1` against `build\release\disk.img` |
| `scripts\debug\release\run-all-release-tests.bat` | aggregate: chains manifest tests + to-vhdx + to-iso (skips write-usb); reports per-step failures |
| `scripts\debug\run-all-tests.bat` | cross-layer: chains kernel + usermode + desktop + release aggregates |

### Why write-usb is excluded from the aggregate

`run-all-release-tests.bat` deliberately does NOT chain
`run-write-usb.bat`. The USB writer is destructive (raw write to a
physical drive) and interactive (double confirmation), so a CI-style
aggregate run is the wrong shape. Click `run-write-usb.bat` from
File Explorer to launch it manually with UAC elevation.

### Adding a new layer

When a new test layer joins (e.g. `firmware/`):

1. Create `scripts\debug\<layer>\` and put the per-artifact bats there.
2. Add `scripts\debug\<layer>\run-all-<layer>-tests.bat` to chain them.
3. Add `<layer>` to `VALID_TEST_LAYERS` in `scripts/todo-graph/validate.py`
   (the TODO bat-alignment check resolves
   `scripts\debug\<layer>\run-<name>-tests.bat` paths in `Test runner:`
   lines against this tuple).
4. Add an `if exist` block to `scripts\debug\run-all-tests.bat`.

Only `run-all-tests.bat` is permitted at the `scripts\debug\` root;
all per-category bats live under the layer subdirs (enforced by the
bat-alignment check and `scripts/lint.sh`).

### UNC path support (`\\wsl.localhost\...`)

A common Windows-host workflow is to navigate to the repo via the
WSL UNC mount (`\\wsl.localhost\Ubuntu\home\<user>\impossible-os`) and
double-click a bat directly. CMD.EXE does NOT support UNC paths as a
working directory and silently falls back to `C:\Windows`, which
breaks every relative path the bat assumes. The release-layer bats
therefore prefix `pushd "%~dp0"` (CMD auto-maps the UNC dir to a
temporary drive letter) and the PowerShell scripts compute their
repo root via `[System.IO.Path]::GetFullPath()` (handles UNC and
normalizes `..\..\` segments correctly, unlike `Resolve-Path` which
can return provider-prefixed PSPath strings that downstream
`pwsh -File` rejects).

Effect: launching `run-all-release-tests.bat` from
`\\wsl.localhost\...\scripts\debug\release\` works exactly as it would
from a real local clone (e.g. `C:\impossible-os\scripts\debug\release\`).
Other layer aggregates that don't yet have the `pushd` prefix may still
require the user to first map the UNC path to a drive letter manually
or clone the repo to a local path; that's tracked under the layer's
owning TODO.
