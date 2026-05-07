<#
.SYNOPSIS
  Windows PowerShell peer of scripts/release/build-manifest.sh.

.DESCRIPTION
  Produces or verifies the boot artifact manifest. Output is byte-identical
  to the bash peer for any given input: same UUID v5 namespace, same JSON
  structure, same field-validation gate, same `[ERROR] ...` stderr grammar,
  same exit codes.

  Subcommands:
    build   Emit build/artifacts/manifest.json (or --Out PATH) from current
            build outputs.
    check   Verify a manifest at the given path. Returns 0 on pass; non-zero
            with [ERROR] lines on stderr on fail.

  Schema spec: docs/release/boot-artifact-manifest.md
  Schema owner: TODO-06 boot-media-image-installer-handoff
  Bash peer: scripts/release/build-manifest.sh (kept in lockstep)

.NOTES
  PowerShell 5.1+ compatible. UUID v5 implemented manually via SHA-1 to
  match Python's uuid.uuid5 byte-for-byte. JSON serializer is custom
  (mirroring Python json.dump indent=2, sort_keys=False, ensure_ascii=
  default=True) so two consecutive builds and the Linux/bash peer all
  emit byte-identical files.

  Env contract (matches bash peer):
    BOOT_INFO_ABI_FILE     Path to build/boot-info-abi.kernel.json.
    SIGN_FINGERPRINT_FILE  Path to build/.signed-artifacts.fingerprint.
    SIGN_STAMP_FILE        Path to build/.signed-artifacts.stamp.
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)] [string] $Subcommand,
    [Parameter(Position = 1, ValueFromRemainingArguments = $true)] [string[]] $Rest
)

$ErrorActionPreference = 'Stop'

# ---- repo root + helpers ----------------------------------------------------

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..\..')
Set-Location $RepoRoot

function Write-Err {
    param([string] $Message)
    [Console]::Error.WriteLine("[ERROR] $Message")
}

function Show-Usage {
    $usage = @"
usage:
  build-manifest.ps1 build [--Out PATH] [--Format FORMAT]
  build-manifest.ps1 check PATH

  build mode:
    --Out PATH      Output path (default: build/artifacts/manifest.json).
    --Format FORMAT One of raw|usb|vhd|vhdx|vdi|iso|qcow2|ova|recovery|installer
                    (default: raw).

  check mode:
    PATH            Path to a manifest.json to verify. Validates required
                    top-level fields, partition_map non-empty, entries[]
                    has bootloader+kernel, sha256 fields are 64-hex, and
                    denormalized sha256 matches entries[].
"@
    [Console]::Error.WriteLine($usage)
}

function Get-Sha256OfFile {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Write-Err "missing input file: $Path"
        throw "missing input file"
    }
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-FileSizeBytes {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return 0
    }
    [int64](Get-Item -LiteralPath $Path).Length
}

function Get-ToolchainFingerprint {
    # One-line concatenation of clang / lld / nasm versions to match the
    # bash peer. Tools may not exist on a Windows host that only consumes
    # release artifacts; missing tools collapse to "missing-<tool>".
    $clang = $null
    try {
        $out = & clang-19 --version 2>$null
        if ($LASTEXITCODE -eq 0 -and $out) { $clang = ($out | Select-Object -First 1).Trim() }
    } catch { }
    if (-not $clang) { $clang = 'missing-clang' }

    $lld = $null
    try {
        $out = & ld.lld-19 --version 2>$null
        if ($LASTEXITCODE -eq 0 -and $out) { $lld = ($out | Select-Object -First 1).Trim() }
    } catch { }
    if (-not $lld) { $lld = 'missing-lld' }

    $nasm = $null
    try {
        $out = & nasm -v 2>$null
        if ($LASTEXITCODE -eq 0 -and $out) { $nasm = ($out | Select-Object -First 1).Trim() }
    } catch { }
    if (-not $nasm) { $nasm = 'missing-nasm' }

    "$clang | $lld | $nasm"
}

function Read-BootInfoVersionFromBinary {
    # Stale-binding guard: refuse to publish a manifest when the ABI JSON
    # is older than either artifact we are about to hash. Mirrors the bash
    # peer's stat-based mtime check.
    $abi = $env:BOOT_INFO_ABI_FILE
    if (-not $abi) { $abi = 'build/boot-info-abi.kernel.json' }
    if (-not (Test-Path -LiteralPath $abi -PathType Leaf)) {
        Write-Err "missing $abi -- run scripts/build.sh first to generate the binary-derived ABI manifest"
        throw "missing ABI manifest"
    }
    $bl = 'build/tools/BOOTX64.EFI'
    $kr = 'build/kernel.exe'
    $abi_mt = (Get-Item -LiteralPath $abi).LastWriteTimeUtc
    $bl_mt  = if (Test-Path -LiteralPath $bl) { (Get-Item -LiteralPath $bl).LastWriteTimeUtc } else { [datetime]::MinValue }
    $kr_mt  = if (Test-Path -LiteralPath $kr) { (Get-Item -LiteralPath $kr).LastWriteTimeUtc } else { [datetime]::MinValue }
    if ($bl_mt -gt $abi_mt -or $kr_mt -gt $abi_mt) {
        Write-Err "$abi is older than build artifacts; rebuild via scripts/build.sh to regenerate the binary-derived ABI manifest"
        throw "stale ABI manifest"
    }
    $abiJson = Get-Content -LiteralPath $abi -Raw | ConvertFrom-Json
    [int] $abiJson.version
}

function Get-SecureBootStatus {
    # Trust the sign-stamp pair produced by scripts/sign-efi.sh; mirrors
    # the bash peer's mtime comparison. Returns 'signed' or 'unsigned'.
    $stamp_fp = $env:SIGN_FINGERPRINT_FILE
    if (-not $stamp_fp) { $stamp_fp = 'build/.signed-artifacts.fingerprint' }
    $stamp_mk = $env:SIGN_STAMP_FILE
    if (-not $stamp_mk) { $stamp_mk = 'build/.signed-artifacts.stamp' }
    $bl = 'build/tools/BOOTX64.EFI'
    $kr = 'build/kernel.exe'

    if (-not (Test-Path -LiteralPath $stamp_fp -PathType Leaf) -or
        ((Get-Item -LiteralPath $stamp_fp).Length -le 0) -or
        -not (Test-Path -LiteralPath $stamp_mk)) {
        return 'unsigned'
    }
    if (-not (Test-Path -LiteralPath $bl) -or -not (Test-Path -LiteralPath $kr)) {
        return 'unsigned'
    }
    $stamp_mt = (Get-Item -LiteralPath $stamp_mk).LastWriteTimeUtc
    $bl_mt    = (Get-Item -LiteralPath $bl).LastWriteTimeUtc
    $kr_mt    = (Get-Item -LiteralPath $kr).LastWriteTimeUtc
    if ($stamp_mt -lt $bl_mt -or $stamp_mt -lt $kr_mt) {
        return 'unsigned'  # stale: artifacts rebuilt after last signing run
    }
    'signed'
}

# ---- UUID v5 (mirrors Python uuid.uuid5 byte-for-byte) ----------------------

function ConvertTo-UuidBytes {
    # Convert an 8-4-4-4-12 hex UUID string to its 16-byte network-order
    # representation. Matches Python uuid.UUID(s).bytes.
    param([string] $UuidString)
    $hex = $UuidString.Replace('-', '')
    if ($hex.Length -ne 32) {
        throw "ConvertTo-UuidBytes: expected 32 hex chars, got $($hex.Length): $UuidString"
    }
    $bytes = New-Object byte[] 16
    for ($i = 0; $i -lt 16; $i++) {
        $bytes[$i] = [byte]([Convert]::ToInt32($hex.Substring($i * 2, 2), 16))
    }
    , $bytes
}

function New-UuidV5 {
    # SHA-1(namespace_bytes || name.utf8) -> first 16 bytes -> set version=5
    # in byte 6, variant=10xx in byte 8 -> emit 8-4-4-4-12 lowercase hex.
    # Matches Python uuid.uuid5(uuid.UUID(ns), name) byte-for-byte.
    param(
        [string] $NamespaceUuid,
        [string] $Name
    )
    $ns_bytes  = ConvertTo-UuidBytes -UuidString $NamespaceUuid
    $nm_bytes  = [System.Text.Encoding]::UTF8.GetBytes($Name)
    $combined  = New-Object byte[] ($ns_bytes.Length + $nm_bytes.Length)
    [Array]::Copy($ns_bytes, 0, $combined, 0, $ns_bytes.Length)
    [Array]::Copy($nm_bytes, 0, $combined, $ns_bytes.Length, $nm_bytes.Length)

    $sha1 = [System.Security.Cryptography.SHA1]::Create()
    try {
        $hash = $sha1.ComputeHash($combined)
    } finally {
        $sha1.Dispose()
    }

    $u = New-Object byte[] 16
    [Array]::Copy($hash, 0, $u, 0, 16)
    # Set version 5 in the high nibble of byte 6.
    $u[6] = [byte]((($u[6] -band 0x0F) -bor 0x50))
    # Set variant 10xx in the high bits of byte 8.
    $u[8] = [byte]((($u[8] -band 0x3F) -bor 0x80))

    $hex = ($u | ForEach-Object { $_.ToString('x2') }) -join ''
    "$($hex.Substring(0,8))-$($hex.Substring(8,4))-$($hex.Substring(12,4))-$($hex.Substring(16,4))-$($hex.Substring(20,12))"
}

# ---- JSON emitter (mirrors Python json.dump(indent=2, sort_keys=False)) -----

function ConvertTo-PyJsonString {
    # Mirror Python's default ensure_ascii=True string escaping: ASCII
    # printables verbatim except backslash + double-quote + control chars
    # (\b \f \n \r \t for those, \uXXXX for everything else < 0x20 or >=
    # 0x7F). Output is a JSON string token with surrounding quotes.
    param([string] $S)
    if ($null -eq $S) { return '""' }
    $sb = New-Object System.Text.StringBuilder
    [void] $sb.Append('"')
    for ($i = 0; $i -lt $S.Length; $i++) {
        $c = $S[$i]
        $code = [int] $c
        if     ($c -eq '"')  { [void] $sb.Append('\"') }
        elseif ($c -eq '\')  { [void] $sb.Append('\\') }
        elseif ($code -eq 8) { [void] $sb.Append('\b') }
        elseif ($code -eq 9) { [void] $sb.Append('\t') }
        elseif ($code -eq 10){ [void] $sb.Append('\n') }
        elseif ($code -eq 12){ [void] $sb.Append('\f') }
        elseif ($code -eq 13){ [void] $sb.Append('\r') }
        elseif ($code -lt 32 -or $code -ge 127) {
            [void] $sb.Append(('\u{0:x4}' -f $code))
        } else {
            [void] $sb.Append($c)
        }
    }
    [void] $sb.Append('"')
    $sb.ToString()
}

function ConvertTo-PyJson {
    # Recursive JSON emitter mirroring Python json.dump(indent=2,
    # sort_keys=False). Inputs:
    #   - [ordered] hashtables -> JSON objects (key order preserved)
    #   - System.Object[] / generic List -> JSON arrays
    #   - string -> escaped JSON string
    #   - [bool] -> "true" / "false"
    #   - integer types -> bare integer literal
    #   - $null -> "null"
    #
    # Indent style: 2 spaces per level, opening brace/bracket on the
    # same line as the key, members separated by ",\n", closing brace/
    # bracket aligned with the key's column.
    param(
        [object] $Value,
        [int] $Indent = 0
    )
    $pad = ' ' * ($Indent * 2)
    $padInner = ' ' * (($Indent + 1) * 2)

    if ($null -eq $Value) {
        return 'null'
    }
    if ($Value -is [bool]) {
        return $(if ($Value) { 'true' } else { 'false' })
    }
    if ($Value -is [string]) {
        return (ConvertTo-PyJsonString $Value)
    }
    if ($Value -is [int] -or $Value -is [long] -or $Value -is [int64] -or
        $Value -is [int32] -or $Value -is [int16] -or $Value -is [byte] -or
        $Value -is [uint16] -or $Value -is [uint32] -or $Value -is [uint64]) {
        return [string] $Value
    }
    if ($Value -is [double] -or $Value -is [single] -or $Value -is [decimal]) {
        # Not used by the manifest, but harmless to support.
        return [string] $Value
    }
    if ($Value -is [System.Collections.IDictionary]) {
        $keys = @($Value.Keys)
        if ($keys.Count -eq 0) { return '{}' }
        $sb = New-Object System.Text.StringBuilder
        [void] $sb.Append('{')
        for ($i = 0; $i -lt $keys.Count; $i++) {
            [void] $sb.Append("`n")
            [void] $sb.Append($padInner)
            [void] $sb.Append((ConvertTo-PyJsonString ([string] $keys[$i])))
            [void] $sb.Append(': ')
            [void] $sb.Append((ConvertTo-PyJson -Value $Value[$keys[$i]] -Indent ($Indent + 1)))
            if ($i -lt $keys.Count - 1) { [void] $sb.Append(',') }
        }
        [void] $sb.Append("`n")
        [void] $sb.Append($pad)
        [void] $sb.Append('}')
        return $sb.ToString()
    }
    if ($Value -is [System.Collections.IEnumerable] -and -not ($Value -is [string])) {
        $items = @($Value)
        if ($items.Count -eq 0) { return '[]' }
        $sb = New-Object System.Text.StringBuilder
        [void] $sb.Append('[')
        for ($i = 0; $i -lt $items.Count; $i++) {
            [void] $sb.Append("`n")
            [void] $sb.Append($padInner)
            [void] $sb.Append((ConvertTo-PyJson -Value $items[$i] -Indent ($Indent + 1)))
            if ($i -lt $items.Count - 1) { [void] $sb.Append(',') }
        }
        [void] $sb.Append("`n")
        [void] $sb.Append($pad)
        [void] $sb.Append(']')
        return $sb.ToString()
    }
    throw "ConvertTo-PyJson: unsupported type $($Value.GetType().FullName)"
}

function Write-Utf8NoBom {
    # Write a string to a file as UTF-8 without BOM, with LF line endings.
    # Python's json.dump writes UTF-8 without BOM and the bash peer adds a
    # trailing newline; we mirror both.
    param([string] $Path, [string] $Content)
    $enc = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($Path, $Content, $enc)
}

# ---- build mode -------------------------------------------------------------

function Invoke-CmdBuild {
    param([string[]] $Args)

    $out = 'build/artifacts/manifest.json'
    $format = 'raw'
    $vmImagePath = ''

    $i = 0
    while ($i -lt $Args.Count) {
        $a = $Args[$i]
        switch ($a) {
            '--Out'      { $out = $Args[$i + 1]; $i += 2 }
            '--out'      { $out = $Args[$i + 1]; $i += 2 }
            '--Format'   { $format = $Args[$i + 1]; $i += 2 }
            '--format'   { $format = $Args[$i + 1]; $i += 2 }
            '--VmImage'  { $vmImagePath = $Args[$i + 1]; $i += 2 }
            '--vm-image' { $vmImagePath = $Args[$i + 1]; $i += 2 }
            '-h'         { Show-Usage; exit 0 }
            '--help'     { Show-Usage; exit 0 }
            default      { Write-Err "unknown build flag: $a"; Show-Usage; exit 2 }
        }
    }

    $allowedFormats = @('raw', 'usb', 'vhd', 'vhdx', 'vdi', 'iso', 'qcow2', 'ova', 'recovery', 'installer')
    if ($allowedFormats -notcontains $format) {
        Write-Err "invalid --Format: $format"
        exit 2
    }

    $bl = 'build/tools/BOOTX64.EFI'
    $kr = 'build/kernel.exe'
    foreach ($f in @($bl, $kr)) {
        if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
            Write-Err "missing required build artifact: $f (run scripts/build.sh first)"
            exit 1
        }
    }

    $bl_sha   = Get-Sha256OfFile $bl
    $kr_sha   = Get-Sha256OfFile $kr
    $bl_size  = Get-FileSizeBytes $bl
    $kr_size  = Get-FileSizeBytes $kr

    try {
        $biv = Read-BootInfoVersionFromBinary
    } catch {
        exit 1
    }

    $secureBootStatus = Get-SecureBootStatus

    $outDir = Split-Path -Parent $out
    if ($outDir -and -not (Test-Path -LiteralPath $outDir -PathType Container)) {
        New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    }

    # Cross-field consistency: role-bearing formats imply matching media_role.
    $mediaRole = switch ($format) {
        'installer' { 'installer' }
        'recovery'  { 'recovery' }
        default     { 'normal' }
    }

    $toolchain = Get-ToolchainFingerprint
    $sourceSha = $null
    try {
        $sourceSha = (& git rev-parse HEAD 2>$null).Trim()
        if ($LASTEXITCODE -ne 0 -or -not $sourceSha) { $sourceSha = 'unknown' }
    } catch {
        $sourceSha = 'unknown'
    }
    $manifestSeed = "$sourceSha|$format"

    # boot.conf is required ESP content.
    $bootConf = 'resources/boot/boot.conf'
    if (-not (Test-Path -LiteralPath $bootConf -PathType Leaf)) {
        Write-Err "missing required ESP source file: $bootConf -- cannot emit boot_config entry"
        exit 1
    }
    $bootConfSha  = Get-Sha256OfFile $bootConf
    $bootConfSize = Get-FileSizeBytes $bootConf

    # vm_image_metadata derivation (--vm-image PATH or convention).
    $vmFormat = ''; $vmSubformat = ''; $vmBlockSize = 0; $vmVirtualSize = 0
    if (-not $vmImagePath) {
        $vmImagePath = switch ($format) {
            'vhd'   { 'build/release/disk.vhd' }
            'vhdx'  { 'build/release/disk.vhdx' }
            'vdi'   { 'build/release/disk.vdi' }
            'qcow2' { 'build/release/disk.qcow2' }
            default { '' }
        }
    }
    if ($vmImagePath -and (Test-Path -LiteralPath $vmImagePath -PathType Leaf)) {
        $qemuImg = Get-Command qemu-img -ErrorAction SilentlyContinue
        if (-not $qemuImg) {
            Write-Err "vm_image_metadata derivation needs qemu-img (install qemu-tools)"
            exit 1
        }
        $infoJson = & qemu-img info --output=json $vmImagePath
        if ($LASTEXITCODE -ne 0) {
            Write-Err "qemu-img info failed for $vmImagePath"
            exit 1
        }
        $info = $infoJson | ConvertFrom-Json
        $vmFormat = if ($info.format -eq 'vpc') { 'vhd' } else { [string] $info.format }
        $vmVirtualSize = [int64] $info.'virtual-size'
        $sub = $null
        if ($info.PSObject.Properties['format-specific']) {
            $fsi = $info.'format-specific'.data
            if ($fsi -and $fsi.PSObject.Properties['subformat']) {
                $sub = [string] $fsi.subformat
            }
        }
        if (-not $sub) {
            $sub = if (@('vhdx', 'vdi', 'qcow2') -contains $vmFormat) { 'dynamic' } else { '' }
        }
        $vmSubformat = $sub
        # Block size: cluster-size at top level, or format-specific.block-size.
        $bs = 0
        if ($info.PSObject.Properties['cluster-size']) { $bs = [int64] $info.'cluster-size' }
        if (-not $bs -and $info.PSObject.Properties['format-specific']) {
            $fsi = $info.'format-specific'.data
            if ($fsi) {
                if ($fsi.PSObject.Properties['block-size']) { $bs = [int64] $fsi.'block-size' }
                if (-not $bs -and $fsi.PSObject.Properties['cluster_size']) { $bs = [int64] $fsi.cluster_size }
            }
        }
        if (-not $bs -and $vmFormat -eq 'vdi') { $bs = 1048576 }
        $vmBlockSize = $bs
    } elseif ($vmImagePath) {
        Write-Err "missing --vm-image / conventional VM image: $vmImagePath"
        exit 1
    }

    if ($vmFormat) {
        if ($vmVirtualSize -le 0) {
            Write-Err "qemu-img info did not report a positive virtual-size for $vmImagePath"
            exit 1
        }
        if ($vmBlockSize -le 0) {
            Write-Err "qemu-img info did not report a positive container block size for $vmImagePath (format=$vmFormat)"
            exit 1
        }
    }

    # ---- assemble the manifest ----
    # Same UUID v5 namespace as the bash peer.
    $UUID_NS = '6f1b3c4a-1d4e-5a6b-8c9d-0e1f2a3b4c5d'
    $seed = $format, $bl_sha, $kr_sha, [string] $biv -join '|'
    $artifactUuid = New-UuidV5 -NamespaceUuid $UUID_NS -Name $seed

    $ESP_TYPE_GUID     = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B'
    $MSBASIC_TYPE_GUID = 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7'

    # [ordered] hashtables preserve insertion order; mirrors Python's
    # dict ordering as the bash peer relies on.
    $partitionMap = if ($format -eq 'iso') {
        @(
            [ordered] @{
                'index'       = 1
                'name'        = 'ESP'
                'type_guid'   = $ESP_TYPE_GUID
                'size_mib'    = 64
                'filesystem'  = 'fat32'
            }
        )
    } else {
        @(
            [ordered] @{
                'index'       = 1
                'name'        = 'ESP'
                'type_guid'   = $ESP_TYPE_GUID
                'size_mib'    = 64
                'filesystem'  = 'fat32'
            },
            [ordered] @{
                'index'       = 2
                'name'        = 'BlackBox'
                'type_guid'   = $MSBASIC_TYPE_GUID
                'size_mib'    = 128
                'filesystem'  = 'fat32'
            },
            [ordered] @{
                'index'       = 3
                'name'        = 'IXFS-System'
                'type_guid'   = $MSBASIC_TYPE_GUID
                'size_mib'    = 318
                'filesystem'  = 'ixfs'
            }
        )
    }

    $entries = New-Object System.Collections.ArrayList
    [void] $entries.Add([ordered] @{
        'name'       = 'bootloader'
        'path'       = '\EFI\BOOT\BOOTX64.EFI'
        'sha256'     = $bl_sha
        'size_bytes' = [int64] $bl_size
        'optional'   = $false
    })
    [void] $entries.Add([ordered] @{
        'name'       = 'kernel'
        'path'       = '\boot\kernel.exe'
        'sha256'     = $kr_sha
        'size_bytes' = [int64] $kr_size
        'optional'   = $false
    })
    [void] $entries.Add([ordered] @{
        'name'       = 'boot_config'
        'path'       = '\EFI\ImpossibleOS\boot.conf'
        'sha256'     = $bootConfSha
        'size_bytes' = [int64] $bootConfSize
        'optional'   = $false
    })

    $m = [ordered] @{
        'manifest_version'   = 1
        'artifact_format'    = $format
        'artifact_uuid'      = $artifactUuid
        'boot_target'        = 'uefi-x86_64'
        'boot_info_version'  = [int] $biv
        'secure_boot_status' = $secureBootStatus
        'media_role'         = $mediaRole
        'partition_map'      = $partitionMap
        'entries'            = $entries
        'bootloader_sha256'  = $bl_sha
        'kernel_sha256'      = $kr_sha
        'toolchain_version'  = $toolchain
        'source_sha'         = $sourceSha
        'manifest_seed'      = $manifestSeed
    }

    if ($vmFormat) {
        $m['vm_image_metadata'] = [ordered] @{
            'format'             = $vmFormat
            'subformat'          = $vmSubformat
            'block_size_bytes'   = [int64] $vmBlockSize
            'virtual_size_bytes' = [int64] $vmVirtualSize
        }
    }

    $jsonText = (ConvertTo-PyJson -Value $m -Indent 0) + "`n"
    Write-Utf8NoBom -Path $out -Content $jsonText

    Write-Output $out
}

# ---- check mode -------------------------------------------------------------

$Script:CheckErrors = New-Object System.Collections.ArrayList
function Add-CheckError { param([string] $Msg) [void] $Script:CheckErrors.Add($Msg) }

function Test-Hex {
    param([string] $S, [int] $Len)
    if ($null -eq $S) { return $false }
    if ($S.Length -ne $Len) { return $false }
    foreach ($c in $S.ToCharArray()) {
        if (-not (($c -ge '0' -and $c -le '9') -or ($c -ge 'a' -and $c -le 'f') -or ($c -ge 'A' -and $c -le 'F'))) {
            return $false
        }
    }
    $true
}

function Test-UuidLowercase {
    param([string] $S)
    if ($null -eq $S) { return $false }
    if ($S.Length -ne 36) { return $false }
    if ($S[8] -ne '-' -or $S[13] -ne '-' -or $S[18] -ne '-' -or $S[23] -ne '-') { return $false }
    foreach ($c in $S.ToCharArray()) {
        if ($c -eq '-') { continue }
        if (-not (($c -ge '0' -and $c -le '9') -or ($c -ge 'a' -and $c -le 'f'))) { return $false }
    }
    $true
}

function Test-UuidAnyCase {
    param([string] $S)
    if ($null -eq $S) { return $false }
    if ($S.Length -ne 36) { return $false }
    if ($S[8] -ne '-' -or $S[13] -ne '-' -or $S[18] -ne '-' -or $S[23] -ne '-') { return $false }
    foreach ($c in $S.ToCharArray()) {
        if ($c -eq '-') { continue }
        if (-not (($c -ge '0' -and $c -le '9') -or ($c -ge 'a' -and $c -le 'f') -or ($c -ge 'A' -and $c -le 'F'))) { return $false }
    }
    $true
}

function Get-MapValue {
    # Read a property by exact key from a hashtable / PSCustomObject.
    param([object] $Obj, [string] $Key)
    if ($null -eq $Obj) { return $null }
    if ($Obj -is [System.Collections.IDictionary]) {
        if ($Obj.Contains($Key)) { return $Obj[$Key] }
        return $null
    }
    if ($Obj -is [System.Management.Automation.PSCustomObject]) {
        $p = $Obj.PSObject.Properties[$Key]
        if ($p) { return $p.Value }
        return $null
    }
    $null
}

function Test-MapHas {
    param([object] $Obj, [string] $Key)
    if ($null -eq $Obj) { return $false }
    if ($Obj -is [System.Collections.IDictionary]) { return $Obj.Contains($Key) }
    if ($Obj -is [System.Management.Automation.PSCustomObject]) { return ($null -ne $Obj.PSObject.Properties[$Key]) }
    $false
}

function Test-IsInt {
    # Match Python's int (excluding bool).
    param([object] $V)
    if ($null -eq $V) { return $false }
    if ($V -is [bool]) { return $false }
    if ($V -is [int] -or $V -is [int16] -or $V -is [int32] -or $V -is [int64] -or
        $V -is [byte] -or $V -is [uint16] -or $V -is [uint32] -or $V -is [uint64]) { return $true }
    $false
}

function Test-IsUInt64Range {
    # Accept any value whose textual representation is a non-negative
    # integer that fits in [0, 2^64-1]. Bridges the parity gap between
    # PowerShell's ConvertFrom-Json (which surfaces JSON integers above
    # Int64.MaxValue as Decimal/Double/BigInteger depending on host PS
    # version) and Python's unbounded-precision int. Without this, a
    # legal manifest with total_sectors=18446744073709551615 would be
    # rejected by the PS validator while the bash peer accepts it.
    param([object] $V)
    if ($null -eq $V -or $V -is [bool]) { return $false }
    if ((Test-IsInt $V) -and $V -ge 0) { return $true }
    $s = [string] $V
    if ([string]::IsNullOrEmpty($s)) { return $false }
    if ($s -notmatch '^\d+$') { return $false }
    $u = [uint64] 0
    return [uint64]::TryParse($s, [ref] $u)
}

function Test-IsBool {
    param([object] $V)
    $V -is [bool]
}

function Validate-EntryRow {
    param([object] $E, [bool] $Required)
    $rowErrs = New-Object System.Collections.ArrayList
    if (-not (($E -is [System.Collections.IDictionary]) -or ($E -is [System.Management.Automation.PSCustomObject]))) {
        [void] $rowErrs.Add("entries[] item is not an object")
        return $rowErrs
    }
    $name = Get-MapValue $E 'name'
    if (-not ($name -is [string]) -or -not $name) {
        [void] $rowErrs.Add("entries[] item missing string name (got: $name)")
        return $rowErrs
    }
    $allowedNames = @('bootloader', 'kernel', 'boot_config', 'boot_entries', 'blackbox_skeleton', 'recovery_payloads')
    if ($allowedNames -notcontains $name) {
        $opt = Get-MapValue $E 'optional'
        if ($Required -or ($opt -ne $true)) {
            [void] $rowErrs.Add("entries[].name must be one of $($allowedNames -join ', ') when not optional (got: $name)")
            return $rowErrs
        }
    }
    $label = "entries[name=$name]"
    $path = Get-MapValue $E 'path'
    if (-not ($path -is [string]) -or -not $path) {
        [void] $rowErrs.Add("$label.path missing or not a non-empty string")
    }
    $sha = Get-MapValue $E 'sha256'
    if (-not ($sha -is [string]) -or -not (Test-Hex -S $sha -Len 64)) {
        [void] $rowErrs.Add("$label.sha256 missing or not a 64-character hex string")
    }
    $sz = Get-MapValue $E 'size_bytes'
    if (-not (Test-IsUInt64Range $sz)) {
        [void] $rowErrs.Add("$label.size_bytes must be a non-negative integer <= 2^64-1 (got: $sz)")
    }
    $opt = Get-MapValue $E 'optional'
    if (-not (Test-MapHas $E 'optional') -or -not (Test-IsBool $opt)) {
        [void] $rowErrs.Add("$label.optional missing or not a boolean")
    }
    if ($Required -and ($opt -eq $true)) {
        [void] $rowErrs.Add("$label is a required entry but has optional=true")
    }
    $rowErrs
}

function Invoke-CmdCheck {
    param([string[]] $Args)
    if ($Args.Count -ne 1) {
        Write-Err "check requires exactly one path argument"
        Show-Usage
        exit 2
    }
    $path = $Args[0]
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Write-Err "manifest not found: $path"
        exit 1
    }
    try {
        $raw = Get-Content -LiteralPath $path -Raw
        $m = $raw | ConvertFrom-Json
    } catch {
        Write-Err "manifest is not valid JSON: $($_.Exception.Message)"
        exit 1
    }

    $required = @(
        'manifest_version', 'artifact_format', 'artifact_uuid',
        'boot_target', 'boot_info_version', 'secure_boot_status',
        'media_role', 'partition_map', 'entries',
        'bootloader_sha256', 'kernel_sha256'
    )
    $missing = $required | Where-Object { -not (Test-MapHas $m $_) }
    if ($missing.Count -gt 0) {
        foreach ($k in $missing) { Write-Err "required field missing: $k" }
        exit 1
    }

    $Script:CheckErrors = New-Object System.Collections.ArrayList

    $manifestVersion = Get-MapValue $m 'manifest_version'
    if ($manifestVersion -ne 1) {
        Add-CheckError "manifest_version must be 1 (this is the v1 packaging gate; got: $manifestVersion)"
    }

    $allowedFormats = @('raw', 'usb', 'vhd', 'vhdx', 'vdi', 'iso', 'qcow2', 'ova', 'recovery', 'installer')
    $artifactFormat = Get-MapValue $m 'artifact_format'
    if ($allowedFormats -notcontains $artifactFormat) {
        Add-CheckError "artifact_format must be one of $($allowedFormats -join ', ') (got: $artifactFormat)"
    }

    $artifactUuid = Get-MapValue $m 'artifact_uuid'
    if (-not (Test-UuidLowercase $artifactUuid)) {
        Add-CheckError "artifact_uuid must match 8-4-4-4-12 lowercase hex (got: $artifactUuid)"
    } else {
        $blSha = Get-MapValue $m 'bootloader_sha256'
        $krSha = Get-MapValue $m 'kernel_sha256'
        $biv   = Get-MapValue $m 'boot_info_version'
        if (($artifactFormat -is [string]) -and ($blSha -is [string]) -and ($krSha -is [string]) -and (Test-IsInt $biv)) {
            $UUID_NS = '6f1b3c4a-1d4e-5a6b-8c9d-0e1f2a3b4c5d'
            $seed = $artifactFormat, $blSha, $krSha, [string] $biv -join '|'
            $expected = New-UuidV5 -NamespaceUuid $UUID_NS -Name $seed
            if ($artifactUuid -ne $expected) {
                Add-CheckError "artifact_uuid does not match deterministic UUID v5 of artifact_format|bootloader_sha256|kernel_sha256|boot_info_version (expected: $expected, got: $artifactUuid)"
            }
        }
    }

    if (Test-MapHas $m 'disk_guid') {
        $dg = Get-MapValue $m 'disk_guid'
        if ($null -ne $dg -and (-not ($dg -is [string]) -or -not (Test-UuidAnyCase $dg))) {
            Add-CheckError "disk_guid must match GUID format when present (got: $dg)"
        }
    }
    if (Test-MapHas $m 'sector_size') {
        $ss = Get-MapValue $m 'sector_size'
        if ($null -ne $ss -and (-not (Test-IsInt $ss) -or @(512, 4096) -notcontains $ss)) {
            Add-CheckError "sector_size must be 512 or 4096 when present (got: $ss)"
        }
    }
    if (Test-MapHas $m 'total_sectors') {
        $ts = Get-MapValue $m 'total_sectors'
        if ($null -ne $ts -and -not (Test-IsUInt64Range $ts)) {
            Add-CheckError "total_sectors must be a non-negative integer <= 2^64-1 when present (got: $ts)"
        }
    }

    $bt = Get-MapValue $m 'boot_target'
    if (-not ($bt -is [string]) -or -not $bt) {
        Add-CheckError "boot_target must be a non-empty string (got: $bt)"
    }

    $biv = Get-MapValue $m 'boot_info_version'
    if (-not (Test-IsInt $biv) -or $biv -lt 0 -or $biv -gt 65535) {
        Add-CheckError "boot_info_version must be a non-negative integer <= 65535 (got: $biv)"
    }

    $pm = Get-MapValue $m 'partition_map'
    if (-not ($pm -is [System.Collections.IEnumerable]) -or @($pm).Count -eq 0) {
        Add-CheckError "partition_map must be a non-empty array"
    } else {
        $allowedFs = @('fat32', 'ixfs', 'ntfs')
        $idx = 0
        foreach ($p in $pm) {
            $label = "partition_map[$idx]"
            if (-not (($p -is [System.Collections.IDictionary]) -or ($p -is [System.Management.Automation.PSCustomObject]))) {
                Add-CheckError "$label must be an object"
                $idx++
                continue
            }
            $pidx = Get-MapValue $p 'index'
            if (-not (Test-IsInt $pidx) -or $pidx -lt 1) {
                Add-CheckError "$label.index must be a positive integer (got: $pidx)"
            }
            $pname = Get-MapValue $p 'name'
            if (-not ($pname -is [string]) -or -not $pname) {
                Add-CheckError "$label.name must be a non-empty string"
            }
            $tg = Get-MapValue $p 'type_guid'
            if (-not ($tg -is [string]) -or -not (Test-UuidAnyCase $tg)) {
                Add-CheckError "$label.type_guid must match 8-4-4-4-12 hex GUID (got: $tg)"
            }
            $sm = Get-MapValue $p 'size_mib'
            if (-not (Test-IsInt $sm) -or $sm -lt 0 -or $sm -gt [uint32]::MaxValue) {
                Add-CheckError "$label.size_mib must be a non-negative integer <= 2^32-1 (got: $sm)"
            }
            if (Test-MapHas $p 'filesystem') {
                $fs = Get-MapValue $p 'filesystem'
                if ($null -ne $fs -and ($allowedFs -notcontains $fs)) {
                    Add-CheckError "$label.filesystem must be one of $($allowedFs -join ', ') or absent (got: $fs)"
                }
            }
            if (Test-MapHas $p 'unique_partition_guid') {
                $pg = Get-MapValue $p 'unique_partition_guid'
                if ($null -ne $pg -and (-not ($pg -is [string]) -or -not (Test-UuidAnyCase $pg))) {
                    Add-CheckError "$label.unique_partition_guid must match GUID format when present (got: $pg)"
                }
            }
            foreach ($optInt in @('first_lba', 'last_lba', 'attributes')) {
                if (Test-MapHas $p $optInt) {
                    $v = Get-MapValue $p $optInt
                    if ($null -ne $v -and -not (Test-IsUInt64Range $v)) {
                        Add-CheckError "$label.$optInt must be a non-negative integer <= 2^64-1 when present (got: $v)"
                    }
                }
            }
            $idx++
        }
    }

    $entries = Get-MapValue $m 'entries'
    if (-not ($entries -is [System.Collections.IEnumerable])) {
        Add-CheckError "entries must be an array"
    } else {
        $entriesList = @($entries)
        $espFormats = @('raw', 'usb', 'vhd', 'vhdx', 'vdi', 'qcow2', 'ova', 'installer', 'recovery')
        $requiredNames = @('bootloader', 'kernel')
        if ($espFormats -contains $artifactFormat) { $requiredNames += 'boot_config' }
        foreach ($req in $requiredNames) {
            $matches = @($entriesList | Where-Object {
                (($_ -is [System.Collections.IDictionary]) -or ($_ -is [System.Management.Automation.PSCustomObject])) -and
                (Get-MapValue $_ 'name') -eq $req
            })
            if ($matches.Count -eq 0) {
                Add-CheckError "entries[] missing required name=$req"
            } elseif ($matches.Count -gt 1) {
                Add-CheckError "entries[] has $($matches.Count) rows with name=$req; expected exactly 1"
            } else {
                foreach ($e in (Validate-EntryRow -E $matches[0] -Required $true)) {
                    Add-CheckError $e
                }
            }
        }
        foreach ($e in $entriesList) {
            if (-not (($e -is [System.Collections.IDictionary]) -or ($e -is [System.Management.Automation.PSCustomObject]))) { continue }
            $en = Get-MapValue $e 'name'
            if (@('bootloader', 'kernel') -contains $en) { continue }
            foreach ($err in (Validate-EntryRow -E $e -Required $false)) {
                Add-CheckError $err
            }
        }
    }

    foreach ($k in @('bootloader_sha256', 'kernel_sha256')) {
        $v = Get-MapValue $m $k
        if (-not ($v -is [string]) -or -not (Test-Hex -S $v -Len 64)) {
            Add-CheckError "$k must be a 64-character hex sha256 (got: $v)"
        }
    }

    $entriesList = @(Get-MapValue $m 'entries')
    function Find-EntrySha {
        param([string] $N)
        foreach ($e in $entriesList) {
            if ((($e -is [System.Collections.IDictionary]) -or ($e -is [System.Management.Automation.PSCustomObject])) -and (Get-MapValue $e 'name') -eq $N) {
                return Get-MapValue $e 'sha256'
            }
        }
        $null
    }
    $blEntry = Find-EntrySha 'bootloader'
    $blTop = Get-MapValue $m 'bootloader_sha256'
    if ($null -ne $blEntry -and $blEntry -ne $blTop) {
        Add-CheckError "bootloader_sha256 does not match entries[name=bootloader].sha256"
    }
    $krEntry = Find-EntrySha 'kernel'
    $krTop = Get-MapValue $m 'kernel_sha256'
    if ($null -ne $krEntry -and $krEntry -ne $krTop) {
        Add-CheckError "kernel_sha256 does not match entries[name=kernel].sha256"
    }

    $sbStatus = Get-MapValue $m 'secure_boot_status'
    if (@('signed', 'unsigned', 'unknown') -notcontains $sbStatus) {
        Add-CheckError "secure_boot_status must be signed|unsigned|unknown (got: $sbStatus)"
    }

    $mr = Get-MapValue $m 'media_role'
    if (@('normal', 'installer', 'live', 'recovery', 'manufacturing', 'diagnostics') -notcontains $mr) {
        Add-CheckError "media_role must be one of normal|installer|live|recovery|manufacturing|diagnostics (got: $mr)"
    }

    if ($artifactFormat -eq 'installer' -and $mr -ne 'installer') {
        Add-CheckError "artifact_format=installer requires media_role=installer (got: $mr)"
    }
    if ($artifactFormat -eq 'recovery' -and $mr -ne 'recovery') {
        Add-CheckError "artifact_format=recovery requires media_role=recovery (got: $mr)"
    }

    if (Test-MapHas $m 'source_sha') {
        $v = Get-MapValue $m 'source_sha'
        if (-not ($v -is [string]) -or ($v -ne 'unknown' -and -not (Test-Hex -S $v -Len 40))) {
            Add-CheckError "source_sha must be 40-hex git SHA or 'unknown' (got: $v)"
        }
    }
    if (Test-MapHas $m 'toolchain_version') {
        $v = Get-MapValue $m 'toolchain_version'
        if (-not ($v -is [string]) -or -not $v) {
            Add-CheckError "toolchain_version must be a non-empty string (got: $v)"
        }
    }
    if (Test-MapHas $m 'manifest_seed') {
        $v = Get-MapValue $m 'manifest_seed'
        if (-not ($v -is [string]) -or $v -notlike '*|*') {
            Add-CheckError "manifest_seed must be '<source_sha>|<artifact_format>' (got: $v)"
        } elseif ((Test-MapHas $m 'source_sha') -and (Test-MapHas $m 'artifact_format')) {
            $src = Get-MapValue $m 'source_sha'
            $expected = "$src|$artifactFormat"
            if ($v -ne $expected) {
                Add-CheckError "manifest_seed must equal '$expected' (got: $v)"
            }
        }
    }

    if (Test-MapHas $m 'vm_image_metadata') {
        $vm = Get-MapValue $m 'vm_image_metadata'
        if (-not (($vm -is [System.Collections.IDictionary]) -or ($vm -is [System.Management.Automation.PSCustomObject]))) {
            Add-CheckError "vm_image_metadata must be an object when present (got: $vm)"
        } else {
            $vmFmts = @('vhd', 'vhdx', 'vdi', 'qcow2')
            $vmSubs = @('dynamic', 'fixed', '')
            $f = Get-MapValue $vm 'format'
            if ($vmFmts -notcontains $f) {
                Add-CheckError "vm_image_metadata.format must be one of $($vmFmts -join ', ') (got: $f)"
            }
            $sub = Get-MapValue $vm 'subformat'
            if (-not ($sub -is [string]) -or ($vmSubs -notcontains $sub)) {
                Add-CheckError "vm_image_metadata.subformat must be one of dynamic, fixed or '' (got: $sub)"
            }
            $bs = Get-MapValue $vm 'block_size_bytes'
            if (-not (Test-IsUInt64Range $bs) -or [string]$bs -eq '0') {
                Add-CheckError "vm_image_metadata.block_size_bytes must be a positive integer <= 2^64-1 (got: $bs)"
            }
            $vs = Get-MapValue $vm 'virtual_size_bytes'
            if (-not (Test-IsUInt64Range $vs) -or [string]$vs -eq '0') {
                Add-CheckError "vm_image_metadata.virtual_size_bytes must be a positive integer <= 2^64-1 (got: $vs)"
            }
            if (($vmFmts -contains $f) -and ($vmFmts -contains $artifactFormat) -and ($f -ne $artifactFormat)) {
                Add-CheckError "vm_image_metadata.format=$f disagrees with artifact_format=$artifactFormat"
            }
        }
    }

    if ($Script:CheckErrors.Count -gt 0) {
        foreach ($e in $Script:CheckErrors) { Write-Err $e }
        exit 1
    }
    Write-Output "manifest OK: $path"
}

# ---- dispatch ---------------------------------------------------------------

if (-not $Subcommand) {
    Show-Usage
    exit 2
}
switch ($Subcommand) {
    'build'  { Invoke-CmdBuild -Args $Rest }
    'check'  { Invoke-CmdCheck -Args $Rest }
    '-h'     { Show-Usage; exit 0 }
    '--help' { Show-Usage; exit 0 }
    default  { Write-Err "unknown subcommand: $Subcommand"; Show-Usage; exit 2 }
}
