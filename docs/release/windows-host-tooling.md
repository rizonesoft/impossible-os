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

Expected last line on a clean run: `[summary] 24 pass, 0 fail`. Exit
code 0. The test produces and validates manifest fixtures in a temp
directory which is removed on completion. (The bash peer at
`scripts/release/test-build-manifest.sh` runs 23 assertions; the PS1
peer adds one extra fixture for the `size_mib > 2^32-1` upper-bound
parity check.)

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
