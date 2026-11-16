<#
.SYNOPSIS
Makes a Windows 11 install ISO at a newer build from the pinned ISO and one cumulative update.

.DESCRIPTION
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 24.

The reference VM must run the build the operator's host runs, from its very
first setup page (pre-oobe) on, so the update goes into the install image
rather than into a running machine:

 1. Copies the source ISO's files to -Work (Mount-DiskImage, robocopy).
 2. Restores the reference VM's clean checkpoint, attaches -PackageDisk (a data
    disk holding the .msu, made and filled on first use), copies the source
    image in and, elevated in the guest, exports image -Index to a WIM, adds the
    package with DISM, commits, exports again and splits it into install.swm
    parts under 4 GB (Setup reads split images, and every part fits ISO 9660).
    The guest is shut down, never turned off, and nothing is checkpointed: the
    next run restores clean.
 3. Builds -Out with xorriso in WSL from the copied files, the install.esd
    replaced by the parts, booting the ISO's own efisys.bin (UEFI, with the
    same press-a-key prompt ref-vm.ps1 -Rebuild answers).

The new ISO holds one image, so build the VM from it with -ImageIndex 1:

    powershell.exe -File scripts\inventory\update-iso.ps1 -Package R:\11\updates\windows11.0-kb5132061-x64_<sha1>.msu -Out R:\11\win11-26300.9974.iso
    powershell.exe -File scripts\inventory\ref-vm.ps1 -Rebuild -IsoPath R:\11\win11-26300.9974.iso -ImageIndex 1

The package is checked against the SHA-1 in its catalog file name before use.
Only the cumulative update goes in: the catalog also offers the 24H2 checkpoint
update (KB5043080), which a 26300 image refuses with 0x80070228 (seen
2026-11-16). Exits 0 with -Out written; 1 on any failure, with -Out untouched.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Out,
    [string]$Iso = 'R:\11\win11.iso',
    [int]$Index = 6,
    [string]$Work = 'R:\11\update-iso',
    [string]$PackageDisk = 'R:\11\updates.vhdx',
    [string]$VmPath = 'R:\11\ios-win11-ref',
    [string]$Name = 'ios-win11-ref'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ref-common.ps1')

# The package disk is found in the guest by its size and label.
$DiskSize = 16GB
$DiskLabel = 'IOSUPD'
# Setup reads install.swm parts; each stays under ISO 9660's 4 GiB file limit.
$PartMB = 3800

if (Test-Path -LiteralPath $Out) { Write-Host "update-iso: $Out exists; give a new path"; exit 1 }
$pkgName = Split-Path -Leaf $Package
if ($pkgName -notmatch '_([0-9a-f]{40})\.msu$') { Write-Host "update-iso: $pkgName is not a catalog file name (<name>_<sha1>.msu)"; exit 1 }
if ((Get-FileHash -Algorithm SHA1 -LiteralPath $Package).Hash -ne $Matches[1].ToUpper()) { Write-Host "update-iso: $pkgName does not match the SHA-1 in its name"; exit 1 }

# 1. The source ISO's files.
$src = Join-Path $Work 'iso'
if (Test-Path -LiteralPath $src) { Remove-Item -LiteralPath $src -Recurse -Force }
$m = Mount-DiskImage -ImagePath $Iso -PassThru
try {
    $root = ($m | Get-Volume).DriveLetter + ':\'
    robocopy $root $src /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy of $Iso exited $LASTEXITCODE" }
} finally { Dismount-DiskImage -ImagePath $Iso | Out-Null }
$image = @(Get-ChildItem -LiteralPath (Join-Path $src 'sources') -Filter 'install.*' | Where-Object { $_.Extension -in '.esd', '.wim' })
if ($image.Count -ne 1) { throw "expected one install image in $src\sources, found $($image.Count)" }
Write-Ref "copied $Iso; install image $($image[0].Name)"

# 2. Service the image in the guest.
$parts = Join-Path $Work 'parts'
if (Test-Path -LiteralPath $parts) { Remove-Item -LiteralPath $parts -Recurse -Force }
New-Item -ItemType Directory -Path $parts | Out-Null
if (-not (Test-Path -LiteralPath $PackageDisk)) { New-VHD -Path $PackageDisk -SizeBytes $DiskSize -Dynamic | Out-Null }
Enter-RefLock $Name
$s = Start-RefFrom $Name $VmPath 'clean'
try {
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
    $guestPkg = "${letter}:\$pkgName"
    if (-not (Invoke-Command -Session $s -ArgumentList $guestPkg { param($p) Test-Path -LiteralPath $p })) {
        Copy-Item -ToSession $s -LiteralPath $Package -Destination $guestPkg
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
    # A turned-off guest mid-write left the unsaved state unbootable (2026-11-16):
    # shut it down and wait. The session drops as the guest goes down, so its
    # error is expected; Wait-Until is what proves the shutdown.
    Invoke-Command -Session $s { Stop-Computer -Force } -ErrorAction SilentlyContinue
    Wait-Until -TimeoutSec 600 -EverySec 5 -What 'the guest to shut down' -Test { (Get-VM -Name $Name).State -eq 'Off' } | Out-Null
} finally {
    Remove-PSSession $s -ErrorAction SilentlyContinue
    if ((Get-VM -Name $Name).State -ne 'Off') { Stop-VM -Name $Name -TurnOff -Force }
}

# 3. The ISO, built in WSL. Joliet long names: Setup's longest file name is 85 characters.
$label = (& wsl.exe -e xorriso -indev (wsl.exe -e wslpath -a $Iso) -pvd_info 2>$null | Select-String 'Volume Id\s*:\s*(\S+)').Matches.Groups[1].Value
if (-not $label) { throw "could not read the volume id of $Iso" }
Remove-Item -LiteralPath (Join-Path $src "sources\$($image[0].Name)")
foreach ($f in Get-ChildItem -LiteralPath $parts) { Move-Item -LiteralPath $f.FullName -Destination (Join-Path $src "sources\$($f.Name)") }
$wsrc = (& wsl.exe -e wslpath -a $src).Trim()
$wtmp = (& wsl.exe -e wslpath -a "$Out.partial").Trim()
& wsl.exe -e xorriso -as mkisofs -quiet -iso-level 3 -J -joliet-long -R -V $label `
    -e efi/microsoft/boot/efisys.bin -no-emul-boot -o $wtmp $wsrc
if ($LASTEXITCODE -ne 0) { throw "xorriso exited $LASTEXITCODE" }
Move-Item -LiteralPath "$Out.partial" -Destination $Out
Write-Ref "wrote $Out ($info)"
exit 0
