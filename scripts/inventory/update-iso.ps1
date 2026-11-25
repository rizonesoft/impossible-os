<#
.SYNOPSIS
Makes a Windows 11 install ISO at a newer build from the pinned ISO and one cumulative update.

.DESCRIPTION
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 24.

The reference VM must run the build the operator's host runs, from its very
first setup page (pre-oobe) on, so the update goes into the install image
rather than into a running machine:

 1. Copies the source ISO's files to -Work\iso, all but the install image
    (Mount-DiskImage, robocopy).
 2. Restores the reference VM's clean checkpoint, attaches -PackageDisk (a data
    disk holding the .msu, made and filled on first use, its copy checked by
    hash), copies the install image in from the mounted ISO and, elevated in
    the guest, exports image -Index to a WIM, adds the package with DISM,
    commits, exports again and splits it into install.swm parts under 4 GB
    (Setup reads split images, and every part fits ISO 9660). The parts land in
    -Work\parts with parts.json, written last, naming the inputs and each
    part's size: a later run with the same inputs reuses them and skips this
    step, so a failed step 3 costs minutes, not the servicing. The guest is
    shut down, never turned off, and nothing is checkpointed: the next run
    restores clean.
 3. Builds -Out with xorriso in WSL from -Work\iso with the parts grafted in
    as sources\install*.swm, booting the ISO's own efisys.bin (UEFI, with the
    same press-a-key prompt ref-vm.ps1 -Rebuild answers).

One run at a time per -Work folder and per VM: both are locked before anything
is touched, and a second run exits 4.

The new ISO holds one image, so build the VM from it with -ImageIndex 1:

    powershell.exe -File scripts\inventory\update-iso.ps1 -Package D:\11\updates\windows11.0-kb5132061-x64_<sha1>.msu -Out D:\11\win11-26300.9974.iso
    powershell.exe -File scripts\inventory\ref-vm.ps1 -Rebuild -IsoPath D:\11\win11-26300.9974.iso -ImageIndex 1

The package is checked against the SHA-1 in its catalog file name before use.
Only the cumulative update goes in: the catalog also offers the 24H2 checkpoint
update (KB5043080), which a 26300 image refuses with 0x80070228 (seen
2026-11-16). Exits 0 with -Out written; 1 on any failure, with -Out untouched.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Out,
    [string]$Iso = 'D:\11\win11.iso',
    [int]$Index = 6,
    [string]$Work = 'D:\11\update-iso',
    [string]$PackageDisk = 'D:\11\updates.vhdx',
    [string]$VmPath = 'D:\11\ios-win11-ref',
    [string]$Name = 'ios-win11-ref'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ref-common.ps1')
Assert-RefTarget $Name @($VmPath, $Work, $PackageDisk, $Out)

# True when $Parts holds a finished servicing of the inputs $Key names:
# parts.json (written last) matches $Key and every part it lists is there at
# its recorded size. A run that died while copying the parts out never wrote
# parts.json, so its parts are not reused.
function Test-UpdateParts([string]$Parts, $Key) {
    $manifest = Join-Path $Parts 'parts.json'
    if (-not (Test-Path -LiteralPath $manifest)) { return $false }
    try { $m = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json } catch { return $false }
    foreach ($k in $Key.Keys) { if ("$($m.key.$k)" -ne "$($Key[$k])") { return $false } }
    if (-not $m.parts -or @($m.parts).Count -eq 0) { return $false }
    foreach ($p in $m.parts) {
        $f = Join-Path $Parts $p.name
        if (-not (Test-Path -LiteralPath $f) -or (Get-Item -LiteralPath $f).Length -ne $p.size) { return $false }
    }
    return $true
}

# The volume label from xorriso -pvd_info output. With stderr folded in, the
# output also holds the diagnostic "Volume id : 'LABEL'" (quoted, lower-case
# id) ahead of the PVD's "Volume Id : LABEL": only the PVD field counts, matched
# case-sensitively and exactly once. $null when it is missing or repeated.
function Get-PvdLabel([string[]]$Lines) {
    $hits = @($Lines | Select-String -CaseSensitive -Pattern '^Volume Id\s*:\s*(\S.*?)\s*$')
    if ($hits.Count -ne 1) { return $null }
    return $hits[0].Matches[0].Groups[1].Value
}

# Records the parts in $Parts as the servicing of $Key, through a temporary
# file renamed into place, so parts.json exists only once every part does.
function Write-UpdateParts([string]$Parts, $Key, [string]$Info) {
    $list = @(Get-ChildItem -LiteralPath $Parts -Filter '*.swm' | Sort-Object Name | ForEach-Object { [ordered]@{ name = $_.Name; size = $_.Length } })
    if ($list.Count -eq 0) { throw "no install.swm parts in $Parts" }
    $tmp = Join-Path $Parts 'parts.json.partial'
    [ordered]@{ key = $Key; parts = $list; info = $Info } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $tmp -Encoding UTF8
    Move-Item -LiteralPath $tmp -Destination (Join-Path $Parts 'parts.json') -Force
}

# The package disk is found in the guest by its size and label.
$DiskSize = 16GB
$DiskLabel = 'IOSUPD'
# Setup reads install.swm parts; each stays under ISO 9660's 4 GiB file limit.
$PartMB = 3800

if (Test-Path -LiteralPath $Out) { Write-Host "update-iso: $Out exists; give a new path"; exit 1 }
$pkgName = Split-Path -Leaf $Package
if ($pkgName -notmatch '_([0-9a-f]{40})\.msu$') { Write-Host "update-iso: $pkgName is not a catalog file name (<name>_<sha1>.msu)"; exit 1 }
$pkgSha = $Matches[1].ToUpper()
if ((Get-FileHash -Algorithm SHA1 -LiteralPath $Package).Hash -ne $pkgSha) { Write-Host "update-iso: $pkgName does not match the SHA-1 in its name"; exit 1 }

# Both locks before anything under -Work changes: a rejected second run must
# leave the first one's files alone, whichever VM it names.
$Work = [IO.Path]::GetFullPath($Work).TrimEnd('\')
$workKey = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($Work.ToLowerInvariant()))).Replace('-', '').Substring(0, 16)
$WorkLock = New-Object Threading.Mutex($false, "Global\ios-update-iso-$workKey")
try { $held = $WorkLock.WaitOne(0) } catch [Threading.AbandonedMutexException] { $held = $true }
if (-not $held) { Write-Host "update-iso: another update-iso run is using $Work"; exit 4 }
Enter-RefLock $Name

$src = Join-Path $Work 'iso'
$parts = Join-Path $Work 'parts'
$isoFile = Get-Item -LiteralPath $Iso
# What the parts were made from; parts.json must match it to be reused.
$key = [ordered]@{ iso = $isoFile.FullName; isoSize = $isoFile.Length; isoTime = $isoFile.LastWriteTimeUtc.Ticks; index = $Index; package = $pkgSha; partMB = $PartMB }
$reuse = Test-UpdateParts $parts $key

# 1. The source ISO's files, without the install image; the mount stays until
#    the guest has its copy of the image.
if (Test-Path -LiteralPath $src) { Remove-Item -LiteralPath $src -Recurse -Force }
$m = Mount-DiskImage -ImagePath $isoFile.FullName -PassThru
try {
    $root = ($m | Get-Volume).DriveLetter + ':\'
    $image = @(Get-ChildItem -LiteralPath (Join-Path $root 'sources') -Filter 'install.*' | Where-Object { $_.Extension -in '.esd', '.wim' })
    if ($image.Count -ne 1) { throw "expected one install image in ${root}sources, found $($image.Count)" }
    robocopy $root $src /E /XF $image[0].Name /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy of $Iso exited $LASTEXITCODE" }
    Write-Ref "copied $Iso without its install image $($image[0].Name)"

    # 2. Service the image in the guest, unless parts.json says it is done.
    if ($reuse) {
        Write-Ref "reusing the serviced parts in $parts"
    } else {
        if (Test-Path -LiteralPath $parts) { Remove-Item -LiteralPath $parts -Recurse -Force }
        New-Item -ItemType Directory -Path $parts | Out-Null
        if (-not (Test-Path -LiteralPath $PackageDisk)) { New-VHD -Path $PackageDisk -SizeBytes $DiskSize -Dynamic | Out-Null }
        $s = $null
        try {
            # Inside the try: a session that never comes up still turns the
            # restored VM off below.
            $s = Start-RefFrom $Name $VmPath 'clean'
            Add-VMHardDiskDrive -VMName $Name -Path $PackageDisk
            $letter = Invoke-Command -Session $s -ArgumentList $DiskSize, $DiskLabel {
                param($size, $label)
                $ErrorActionPreference = 'Stop'
                $deadline = (Get-Date).AddSeconds(60)
                do { Update-HostStorageCache; $d = Get-Disk | Where-Object { $_.Size -eq $size } | Select-Object -First 1; if (-not $d) { Start-Sleep 2 } } until ($d -or (Get-Date) -gt $deadline)
                if (-not $d) { throw 'the package disk never appeared in the guest' }
                if ($d.PartitionStyle -eq 'RAW') {
                    Initialize-Disk -Number $d.Number -PartitionStyle GPT
                    New-Partition -DiskNumber $d.Number -UseMaximumSize -AssignDriveLetter | Format-Volume -FileSystem NTFS -NewFileSystemLabel $label -Confirm:$false | Out-Null
                }
                if ($d.IsOffline) { Set-Disk -Number $d.Number -IsOffline $false }
                (Get-Volume -FileSystemLabel $label).DriveLetter
            }
            # The package disk outlives clean, so a copy a dead run cut short
            # stays on it: the copy is used only when its hash matches, and a
            # new copy lands under a temporary name and is renamed once checked.
            $guestPkg = "${letter}:\$pkgName"
            $hashOf = { param($p) if (Test-Path -LiteralPath $p) { (Get-FileHash -Algorithm SHA1 -LiteralPath $p).Hash } }
            if ((Invoke-Command -Session $s -ScriptBlock $hashOf -ArgumentList $guestPkg) -ne $pkgSha) {
                Copy-Item -ToSession $s -LiteralPath $Package -Destination "$guestPkg.partial"
                if ((Invoke-Command -Session $s -ScriptBlock $hashOf -ArgumentList "$guestPkg.partial") -ne $pkgSha) { throw "the copy of $pkgName on the package disk does not match its SHA-1" }
                Invoke-Command -Session $s -ArgumentList $guestPkg { param($p) Move-Item -LiteralPath "$p.partial" -Destination $p -Force }
            }
            Invoke-Command -Session $s { if (Test-Path C:\update-iso) { Remove-Item C:\update-iso -Recurse -Force }; New-Item -ItemType Directory C:\update-iso\mnt, C:\update-iso\out | Out-Null }
            Copy-Item -ToSession $s -LiteralPath $image[0].FullName -Destination "C:\update-iso\$($image[0].Name)"
            Write-Ref "servicing image $Index with $pkgName"
            $info = Invoke-Command -Session $s -ArgumentList "C:\update-iso\$($image[0].Name)", $Index, $guestPkg, $PartMB {
                param($img, $index, $pkg, $partMB)
                $ErrorActionPreference = 'Stop'
                function Invoke-Dism { $o = & dism.exe /English @args; if ($LASTEXITCODE -notin 0, 3010) { throw "dism $($args -join ' ') exited ${LASTEXITCODE}: $(($o | Select-Object -Last 3) -join ' ')" } }
                Invoke-Dism /Export-Image "/SourceImageFile:$img" "/SourceIndex:$index" /DestinationImageFile:C:\update-iso\work.wim /Compress:max /CheckIntegrity
                Remove-Item -LiteralPath $img
                Invoke-Dism /Mount-Image /ImageFile:C:\update-iso\work.wim /Index:1 /MountDir:C:\update-iso\mnt
                try {
                    Invoke-Dism /Image:C:\update-iso\mnt /Add-Package "/PackagePath:$pkg"
                    Invoke-Dism /Image:C:\update-iso\mnt /Cleanup-Image /StartComponentCleanup /ResetBase
                    Invoke-Dism /Unmount-Image /MountDir:C:\update-iso\mnt /Commit
                } catch { & dism.exe /Unmount-Image /MountDir:C:\update-iso\mnt /Discard | Out-Null; throw }
                Invoke-Dism /Export-Image /SourceImageFile:C:\update-iso\work.wim /SourceIndex:1 /DestinationImageFile:C:\update-iso\install.wim /Compress:max /CheckIntegrity
                Remove-Item C:\update-iso\work.wim
                $facts = & dism.exe /English /Get-ImageInfo /ImageFile:C:\update-iso\install.wim /Index:1
                Invoke-Dism /Split-Image /ImageFile:C:\update-iso\install.wim /SWMFile:C:\update-iso\out\install.swm "/FileSize:$partMB"
                ($facts | Select-String '^(Name|Version|ServicePack Build|Edition) :') -join '; '
            }
            Write-Ref "serviced: $info"
            foreach ($f in Invoke-Command -Session $s { Get-ChildItem C:\update-iso\out | ForEach-Object Name }) {
                Copy-Item -FromSession $s -LiteralPath "C:\update-iso\out\$f" -Destination (Join-Path $parts $f)
            }
            # Recorded before the shutdown: a teardown that fails must not cost
            # the next run a servicing whose parts are already here.
            Write-UpdateParts $parts $key $info
            # A turned-off guest mid-write left the unsaved state unbootable (2026-11-16):
            # shut it down and wait. The session drops as the guest goes down, so its
            # error is expected; Wait-Until is what proves the shutdown.
            Invoke-Command -Session $s { Stop-Computer -Force } -ErrorAction SilentlyContinue
            Wait-Until -TimeoutSec 600 -EverySec 5 -What 'the guest to shut down' -Test { (Get-VM -Name $Name).State -eq 'Off' } | Out-Null
        } finally {
            if ($s) { Remove-PSSession $s -ErrorAction SilentlyContinue }
            if ($script:RefTouched -and (Get-VM -Name $Name).State -ne 'Off') { Stop-VM -Name $Name -TurnOff -Force }
            Exit-RefTurn
        }
    }
} finally { Dismount-DiskImage -ImagePath $isoFile.FullName | Out-Null }
$info = (Get-Content -LiteralPath (Join-Path $parts 'parts.json') -Raw | ConvertFrom-Json).info

# 3. The ISO, built in WSL, the parts grafted in as sources\install*.swm.
#    Joliet long names: Setup's longest file name is 85 characters.
# xorriso writes its banner to stderr, which Windows PowerShell turns into a
# terminating error under ErrorActionPreference Stop: stderr is folded into
# stdout inside WSL, so PowerShell reads one stream and the exit code decides.
function Invoke-Xorriso { $o = & wsl.exe -e sh -c 'xorriso "$@" 2>&1' xorriso @args; if ($LASTEXITCODE -ne 0) { throw "xorriso exited ${LASTEXITCODE}: $(($o | Select-Object -Last 3) -join ' ')" }; $o }
$label = Get-PvdLabel (Invoke-Xorriso -indev (& wsl.exe -e wslpath -a $isoFile.FullName).Trim() -pvd_info)
if (-not $label) { throw "could not read the volume id of $Iso" }
$graft = @("/=$((& wsl.exe -e wslpath -a $src).Trim())")
foreach ($f in Get-ChildItem -LiteralPath $parts -Filter '*.swm') { $graft += "/sources/$($f.Name)=$((& wsl.exe -e wslpath -a $f.FullName).Trim())" }
$wtmp = (& wsl.exe -e wslpath -a "$Out.partial").Trim()
Invoke-Xorriso -as mkisofs -quiet -iso-level 3 -J -joliet-long -R -V $label `
    -e efi/microsoft/boot/efisys.bin -no-emul-boot -o $wtmp -graft-points @graft | Out-Null
Move-Item -LiteralPath "$Out.partial" -Destination $Out
Write-Ref "wrote $Out ($info)"
exit 0
