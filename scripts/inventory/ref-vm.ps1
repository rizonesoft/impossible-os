<#
.SYNOPSIS
Builds and verifies the Windows 11 reference VM the parity inventory is captured from.

.DESCRIPTION
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 2.

Build (no switch): creates the Generation 2 VM from the ISO, installs Windows
unattended to the first setup page, takes the checkpoint pre-oobe there,
completes setup with the local account ref, sets the fhd-100 profile and takes
the checkpoint clean. Refuses when the VM exists unless -Rebuild is given.

-Verify: restores clean, reads the profile inside ref's interactive session and
exits 0 only when every value holds and the bare desktop has focus. Prints
"ref-vm: ios-win11-ref ok, profile fhd-100".

-Reclean: restores pre-oobe, completes setup again and replaces clean, without
reinstalling Windows; then verifies. Use it when the profile or the probes change.

Windows PowerShell 5.1 on the Hyper-V host; Hyper-V Administrators membership,
no elevation. From WSL: powershell.exe -File scripts\inventory\ref-vm.ps1 -Verify

The ref password is random, made here, and stored only as DPAPI Export-Clixml in
the VM folder. It also exists, by necessity, in the OOBE answer media while
setup runs (made fresh, operator-only, detached and deleted afterwards) and in
the guest's own LSA autologon store; the guest's cached answer files are
deleted before clean is taken.
#>
[CmdletBinding()]
param(
    [string]$IsoPath = 'R:\11\win11.iso',
    [string]$VmPath = 'R:\11\ios-win11-ref',
    [string]$Name = 'ios-win11-ref',
    [int]$ImageIndex = 6,
    [switch]$Verify,
    [switch]$Rebuild,
    [switch]$Reclean
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ref-common.ps1')

# fhd-100: the profile every canonical record is captured under.
$FhdProfile = [ordered]@{ width = 1920; height = 1080; dpi = 96; monitors = 1; uiLanguage = 'en-US'; format = 'en-US'; appsLight = 1; systemLight = 1; accent = 'FFD47800'; accentOnSurfaces = 0; taskbarCentred = $true }
$GenericProKey = 'VK7JG-NPHTM-C97JM-9MPGT-3V66T'  # Microsoft's published generic install key: installs Pro, activates nothing

function New-AnswerIso([string]$Dir, [string]$IsoOut, [string]$Label) {
    if (-not ('Ios.IsoWriter' -as [type])) {
        Add-Type -TypeDefinition @'
using System; using System.IO; using System.Runtime.InteropServices; using System.Runtime.InteropServices.ComTypes;
namespace Ios { public static class IsoWriter {
  public static void Save(object stream, string path) {
    IStream s = (IStream)stream; System.Runtime.InteropServices.ComTypes.STATSTG st; s.Stat(out st, 1);
    byte[] buf = new byte[1 << 20]; IntPtr got = Marshal.AllocHGlobal(4);
    try { using (FileStream f = File.Create(path)) { long left = st.cbSize;
      while (left > 0) { s.Read(buf, buf.Length, got); int n = Marshal.ReadInt32(got); if (n <= 0) break; f.Write(buf, 0, n); left -= n; }
      if (left != 0) throw new IOException("short ISO stream"); } }
    finally { Marshal.FreeHGlobal(got); } } } }
'@
    }
    $fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
    $fsi.FileSystemsToCreate = 3   # ISO 9660 and Joliet
    $fsi.VolumeName = $Label
    $fsi.Root.AddTree($Dir, $false)
    [Ios.IsoWriter]::Save($fsi.CreateResultImage().ImageStream, $IsoOut)
}

function Get-InstallAnswer() {
    # A RunSynchronous Path holds at most 259 characters; a longer one fails the
    # specialize pass with "restarted unexpectedly" (seen 2026-11-14), so the work
    # lives in ios-setup.cmd on the answer media.
    $cmd = 'cmd /c for %d in (D E F G H I) do @if exist %d:\ios-setup.cmd %d:\ios-setup.cmd'
    if ($cmd.Length -gt 259) { throw "the specialize command is $($cmd.Length) characters; the limit is 259" }
    return @"
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="windowsPE">
    <component name="Microsoft-Windows-International-Core-WinPE" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <SetupUILanguage><UILanguage>en-US</UILanguage></SetupUILanguage>
      <InputLocale>en-US</InputLocale><SystemLocale>en-US</SystemLocale><UILanguage>en-US</UILanguage><UserLocale>en-US</UserLocale>
    </component>
    <component name="Microsoft-Windows-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <DiskConfiguration>
        <Disk wcm:action="add"><DiskID>0</DiskID><WillWipeDisk>true</WillWipeDisk>
          <CreatePartitions>
            <CreatePartition wcm:action="add"><Order>1</Order><Type>EFI</Type><Size>260</Size></CreatePartition>
            <CreatePartition wcm:action="add"><Order>2</Order><Type>MSR</Type><Size>16</Size></CreatePartition>
            <CreatePartition wcm:action="add"><Order>3</Order><Type>Primary</Type><Extend>true</Extend></CreatePartition>
          </CreatePartitions>
          <ModifyPartitions>
            <ModifyPartition wcm:action="add"><Order>1</Order><PartitionID>1</PartitionID><Format>FAT32</Format><Label>System</Label></ModifyPartition>
            <ModifyPartition wcm:action="add"><Order>2</Order><PartitionID>3</PartitionID><Format>NTFS</Format><Label>Windows</Label><Letter>C</Letter></ModifyPartition>
          </ModifyPartitions>
        </Disk>
      </DiskConfiguration>
      <ImageInstall><OSImage>
        <InstallFrom><MetaData wcm:action="add"><Key>/IMAGE/INDEX</Key><Value>$ImageIndex</Value></MetaData></InstallFrom>
        <InstallTo><DiskID>0</DiskID><PartitionID>3</PartitionID></InstallTo>
      </OSImage></ImageInstall>
      <UserData><AcceptEula>true</AcceptEula><ProductKey><Key>$GenericProKey</Key><WillShowUI>OnError</WillShowUI></ProductKey></UserData>
    </component>
  </settings>
  <settings pass="specialize">
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <ComputerName>IOS-REF</ComputerName>
    </component>
    <component name="Microsoft-Windows-Deployment" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <RunSynchronous><RunSynchronousCommand wcm:action="add"><Order>1</Order><Path>$([Security.SecurityElement]::Escape($cmd))</Path></RunSynchronousCommand></RunSynchronous>
    </component>
  </settings>
</unattend>
"@
}

function Get-OobeAnswer([string]$Password) {
    $pw = [Security.SecurityElement]::Escape($Password)
    $desktop = 'cmd /c reg add "HKLM\SOFTWARE\Microsoft\Virtual Machine\Guest" /v ios-desktop /d ready /f'
    return @"
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-International-Core" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <InputLocale>en-US</InputLocale><SystemLocale>en-US</SystemLocale><UILanguage>en-US</UILanguage><UserLocale>en-US</UserLocale>
    </component>
    <component name="Microsoft-Windows-Shell-Setup" processorArchitecture="amd64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
      <OOBE>
        <HideEULAPage>true</HideEULAPage><HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens><HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <HideLocalAccountScreen>true</HideLocalAccountScreen><ProtectYourPC>3</ProtectYourPC>
      </OOBE>
      <UserAccounts><LocalAccounts><LocalAccount wcm:action="add">
        <Name>ref</Name><DisplayName>ref</DisplayName><Group>Administrators</Group>
        <Password><Value>$pw</Value><PlainText>true</PlainText></Password>
      </LocalAccount></LocalAccounts></UserAccounts>
      <AutoLogon><Enabled>true</Enabled><LogonCount>1</LogonCount><Username>ref</Username>
        <Password><Value>$pw</Value><PlainText>true</PlainText></Password></AutoLogon>
      <FirstLogonCommands><SynchronousCommand wcm:action="add"><Order>1</Order>
        <CommandLine>$([Security.SecurityElement]::Escape($desktop))</CommandLine></SynchronousCommand></FirstLogonCommands>
      <TimeZone>UTC</TimeZone>
    </component>
  </settings>
</unattend>
"@
}

function New-RefPassword() {
    $alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789'.ToCharArray()
    $bytes = New-Object byte[] 24
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    return -join ($bytes | ForEach-Object { $alphabet[$_ % $alphabet.Length] })
}

function Wait-SetupPage([int]$TimeoutSec) {
    # The guest watcher says the setup window has been up 90 s; two host
    # thumbnails 30 s apart must then match and not be blank.
    return Wait-Until -TimeoutSec $TimeoutSec -EverySec 15 -What 'the first setup page' -Test {
        $kvp = Get-GuestKvp $Name
        if ($kvp['ios-oobe'] -ne 'ready') { return $null }
        $a = Get-ThumbnailFacts (Get-RefThumbnail $Name)
        Start-Sleep -Seconds 30
        $bytes = Get-RefThumbnail $Name
        $b = Get-ThumbnailFacts $bytes
        if ($a.Hash -eq $b.Hash -and $b.Colours -gt 16) { return @{ Bytes = $bytes; Hash = $b.Hash } }
        return $null
    }
}

function Invoke-Build() {
    $existing = @(Get-VM | Where-Object { $_.Name -eq $Name })
    if ($existing.Count -and -not $Rebuild) {
        $names = @(Get-VMSnapshot -VMName $Name | ForEach-Object { $_.Name })
        if ($names -contains 'pre-oobe' -and $names -notcontains 'clean' -and $existing[0].State -eq 'Running' -and (Get-GuestKvp $Name)['ios-desktop'] -eq 'ready') {
            $script:LogPath = Join-Path $VmPath 'ref-vm.log'
            Write-Ref 'resuming at the signed-in desktop'
            Complete-Clean
            return
        }
    }
    if ($existing.Count) {
        if (-not $Rebuild) { Write-Host "ref-vm: $Name exists; -Rebuild removes it and its checkpoints"; $script:Refused = 3; return }
        # Checked before anything is deleted: a mistyped ISO path must not cost the reference.
        if (-not (Test-Path -LiteralPath $IsoPath -PathType Leaf)) { Write-Host "ref-vm: no ISO at $IsoPath; $Name is left as it was"; $script:Refused = 2; return }
        Write-Ref "removing the existing $Name"
        if ((Get-VM -Name $Name).State -ne 'Off') { Stop-VM -Name $Name -TurnOff -Force }
        Remove-VM -Name $Name -Force
        foreach ($p in 'disk.vhdx', 'Virtual Machines', 'Snapshots') {
            $full = Join-Path $VmPath $p
            if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
        }
    }
    if (-not (Test-Path -LiteralPath $IsoPath)) { throw "no ISO at $IsoPath" }
    New-Item -ItemType Directory -Path $VmPath -Force | Out-Null
    Protect-RefPath $VmPath
    $script:LogPath = Join-Path $VmPath 'ref-vm.log'
    Write-Ref "build $Name from $IsoPath into $VmPath"

    $pw = New-RefPassword
    $cred = New-Object Management.Automation.PSCredential ('ref', (ConvertTo-SecureString $pw -AsPlainText -Force))
    $cred | Export-Clixml -LiteralPath (Join-Path $VmPath 'ref.clixml')

    # Install media: no secret.
    $stage = Join-Path $VmPath 'stage-install'
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Path $stage | Out-Null
    Set-Content -LiteralPath (Join-Path $stage 'autounattend.xml') -Value (Get-InstallAnswer) -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $stage 'ios-setup.cmd') -Encoding ASCII -Value @(
        '@echo off',
        'mkdir C:\Windows\Setup\Scripts',
        'copy /y "%~dp0oobe-watch.ps1" C:\Windows\Setup\Scripts\',
        'schtasks /create /tn ios-oobe-watch /sc onstart /ru SYSTEM /rl HIGHEST /f /tr "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\Windows\Setup\Scripts\oobe-watch.ps1"')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'guest\oobe-watch.ps1') -Destination $stage
    $installIso = Join-Path $VmPath 'answer-install.iso'
    New-AnswerIso $stage $installIso 'IOSANSWER'
    Remove-Item -LiteralPath $stage -Recurse -Force

    $vhd = Join-Path $VmPath 'disk.vhdx'
    New-VM -Name $Name -Generation 2 -MemoryStartupBytes 4GB -NewVHDPath $vhd -NewVHDSizeBytes 80GB -Path $VmPath | Out-Null
    Set-VM -Name $Name -ProcessorCount 4 -StaticMemory -CheckpointType Standard -AutomaticCheckpointsEnabled $false -SnapshotFileLocation $VmPath
    Get-VMNetworkAdapter -VMName $Name | Remove-VMNetworkAdapter
    Set-VMFirmware -VMName $Name -EnableSecureBoot On -SecureBootTemplate MicrosoftWindows
    Set-VMKeyProtector -VMName $Name -NewLocalKeyProtector
    Enable-VMTPM -VMName $Name
    Set-VMVideo -VMName $Name -HorizontalResolution 1920 -VerticalResolution 1080 -ResolutionType Single
    $dvd = Add-VMDvdDrive -VMName $Name -Path $IsoPath -Passthru
    Add-VMDvdDrive -VMName $Name -Path $installIso
    Set-VMFirmware -VMName $Name -FirstBootDevice $dvd
    Write-Ref 'VM created; starting the install'

    Start-VM -Name $Name
    # The firmware asks for a key to boot from the DVD. The key is x: Setup's own
    # screens come up within these seconds, and Enter or Space there presses
    # Cancel (seen 2026-11-14), while x is no accelerator on any Setup page.
    for ($i = 0; $i -lt 10; $i++) { Start-Sleep -Seconds 2; Send-RefKey $Name 0x58 }

    Wait-Until -TimeoutSec 5400 -EverySec 30 -What 'the guest watcher' -Test { (Get-GuestKvp $Name)['ios-boot'] } | Out-Null
    Write-Ref 'Windows is installed; waiting for the first setup page'
    $page = Wait-SetupPage 3600
    Save-Thumbnail $page.Bytes (Join-Path $VmPath 'pre-oobe.png')
    Checkpoint-VM -Name $Name -SnapshotName 'pre-oobe'
    Write-Ref "checkpoint pre-oobe taken (screen $($page.Hash.Substring(0, 12)))"

    # A restore of pre-oobe must show the same page.
    Restore-RefCheckpoint $Name 'pre-oobe' $VmPath
    Wait-Until -TimeoutSec 300 -EverySec 10 -What 'the restored setup page' -Test {
        $f = Get-ThumbnailFacts (Get-RefThumbnail $Name); if ($f.Hash -eq $page.Hash) { $f } else { $null }
    } | Out-Null
    Write-Ref 'restored pre-oobe shows the same page'

    Complete-Oobe $pw
    Complete-Clean
}

# From the pre-oobe setup page: finish setup with the OOBE answer media and wait
# for ref's desktop. The media holds the password and exists only for this step.
function Complete-Oobe([string]$pw) {
    $stage = Join-Path $VmPath 'stage-oobe'
    $oobeIso = Join-Path $VmPath 'answer-oobe.iso'
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Protect-RefPath $stage
    try {
        Set-Content -LiteralPath (Join-Path $stage 'oobe.xml') -Value (Get-OobeAnswer $pw) -Encoding UTF8
        Set-Content -LiteralPath (Join-Path $stage 'oobe.cmd') -Encoding ASCII -Value @(
            '@echo off',
            'schtasks /delete /tn ios-oobe-watch /f',
            'del /q C:\Windows\Setup\Scripts\oobe-watch.ps1',
            '%WINDIR%\System32\Sysprep\sysprep.exe /oobe /reboot /unattend:%~dp0oobe.xml')
        New-AnswerIso $stage $oobeIso 'IOSOOBE'
        Remove-Item -LiteralPath $stage -Recurse -Force
        Set-VMDvdDrive -VMName $Name -ControllerNumber 0 -ControllerLocation 2 -Path $oobeIso

        # The guest watcher runs oobe.cmd from this media when asked; no keys are typed.
        Set-HostKvp $Name 'ios-oobe-go' '1'
        $state = Wait-Until -TimeoutSec 180 -EverySec 5 -What 'the watcher to start oobe.cmd' -Test {
            $v = (Get-GuestKvp $Name)['ios-oobe']; if ($v -eq 'started' -or $v -eq 'no-media') { $v } }
        if ($state -ne 'started') { throw 'the guest watcher found no oobe.cmd on any drive' }
        Write-Ref 'sysprep is completing setup'
        Wait-Until -TimeoutSec 1800 -EverySec 20 -What 'the ref desktop' -Test { (Get-GuestKvp $Name)['ios-desktop'] -eq 'ready' } | Out-Null
    } finally {
        # Each step is tried on its own, so a failed KVP removal cannot leave the
        # password media behind. ios-oobe-go is cleared so a later restore of
        # pre-oobe stays on the setup page.
        $failed = @()
        try { Set-HostKvp $Name 'ios-oobe-go' -Remove } catch { $failed += "clearing ios-oobe-go: $($_.Exception.Message)" }
        try { if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force } } catch { $failed += "removing $($stage): $($_.Exception.Message)" }
        try {
            $drive = Get-VMDvdDrive -VMName $Name | Where-Object { $_.Path -eq $oobeIso }
            if ($drive) { Set-VMDvdDrive -VMName $Name -ControllerNumber $drive.ControllerNumber -ControllerLocation $drive.ControllerLocation -Path $null }
        } catch { $failed += "detaching $($oobeIso): $($_.Exception.Message)" }
        try { if (Test-Path -LiteralPath $oobeIso) { Remove-Item -LiteralPath $oobeIso -Force } } catch { $failed += "deleting $($oobeIso): $($_.Exception.Message)" }
        if (Test-Path -LiteralPath $oobeIso) { $failed += "$oobeIso, which holds the ref password, is still on disk; delete it by hand" }
        foreach ($f in $failed) { Write-Ref "cleanup: $f" }
        if ($failed.Count) { throw "OOBE cleanup failed: $($failed -join '; ')" }
    }
    Write-Ref 'ref is signed in'
}

# From ref's signed-in desktop: set the profile, clear the cached answer files,
# copy the probes in and take clean. Also the resume point after a failure here.
function Complete-Clean() {
    $s = New-RefSession $Name $VmPath
    try {
        Invoke-Command -Session $s -ScriptBlock {
            $p = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
            Set-ItemProperty $p -Name AppsUseLightTheme -Value 1 -Type DWord
            Set-ItemProperty $p -Name SystemUsesLightTheme -Value 1 -Type DWord
            Set-ItemProperty 'HKCU:\Control Panel\Desktop' -Name ScreenSaveActive -Value '0'
            foreach ($x in 'monitor-timeout-ac', 'standby-timeout-ac', 'hibernate-timeout-ac') { & powercfg.exe /change $x 0 }
            # Cached answer files hold the password; setup is finished with them.
            foreach ($f in 'C:\Windows\Panther\unattend.xml', 'C:\Windows\Panther\Unattend', 'C:\Windows\System32\Sysprep\unattend.xml') {
                if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Recurse -Force }
            }
        }
        Invoke-Command -Session $s -ScriptBlock { New-Item -ItemType Directory -Path 'C:\ios\probe' -Force | Out-Null }
        foreach ($f in 'probe-profile.ps1', 'probe-notepad.ps1') {
            Send-RefFile $s (Join-Path $PSScriptRoot "guest\$f") "C:\ios\probe\$f"
        }
        # First sign-in opens Start on this build (seen 2026-11-14). Every capture
        # starts from clean, so clean must show the bare desktop with focus, and
        # the whole profile is checked here, before the old clean is touched.
        $problems = @('not checked')
        for ($try = 1; $try -le 5 -and $problems.Count; $try++) {
            Start-Sleep -Seconds 20
            Send-RefKey $Name 0x1B
            Start-Sleep -Seconds 3
            $problems = @(Get-ProfileProblems $s)
            Write-Ref "profile before checkpoint (try $try): $($script:SeenJson)"
        }
        if ($problems.Count) { throw "clean not taken: $($problems -join '; ')" }
    } finally { Remove-PSSession $s }
    # The old clean is replaced only once its successor exists.
    Repair-CleanPromotion
    if (Get-VMSnapshot -VMName $Name | Where-Object { $_.Name -eq 'clean-next' }) { Remove-VMSnapshot -VMName $Name -Name 'clean-next' }
    Checkpoint-VM -Name $Name -SnapshotName 'clean-next'
    if (Get-VMSnapshot -VMName $Name | Where-Object { $_.Name -eq 'clean' }) { Remove-VMSnapshot -VMName $Name -Name 'clean' }
    Rename-VMSnapshot -VMName $Name -Name 'clean-next' -NewName 'clean'
    Write-Ref 'checkpoint clean taken'
}

function Invoke-Verify() {
    $problems = @()
    $vm = Get-VM | Where-Object { $_.Name -eq $Name } | Select-Object -First 1
    if (-not $vm) { Write-Host "ref-vm: no VM named $Name"; return 1 }
    Repair-CleanPromotion
    $names = @(Get-VMSnapshot -VMName $Name | ForEach-Object { $_.Name })
    foreach ($c in 'pre-oobe', 'clean') { if ($names -notcontains $c) { $problems += "missing checkpoint $c" } }
    if ($vm.ProcessorCount -ne 4) { $problems += "vCPU $($vm.ProcessorCount), want 4" }
    if ($vm.MemoryStartup -ne 4GB -or $vm.DynamicMemoryEnabled) { $problems += 'memory is not a static 4 GiB' }
    if (@(Get-VMNetworkAdapter -VMName $Name).Count -ne 0) { $problems += 'the VM has a network adapter' }
    if (-not (Get-VMSecurity -VMName $Name).TpmEnabled) { $problems += 'TPM is off' }
    if ($problems.Count -eq 0) {
        $s = $null
        try {
            $s = Start-RefFrom $Name $VmPath 'clean'
            $problems += @(Get-ProfileProblems $s)
        } finally { if ($s) { Remove-PSSession $s } }
    }
    if ($problems.Count) {
        foreach ($p in $problems) { Write-Host "ref-vm: $p" }
        return 1
    }
    Write-Host "ref-vm: $Name ok, profile fhd-100 (Windows 11 $($script:Seen.edition) $($script:Seen.displayVersion) build $($script:Seen.build))"
    return 0
}

# A run stopped between removing clean and renaming clean-next leaves clean-next
# as the only desktop checkpoint: finish that promotion before anything else.
# With clean present, a clean-next is a stale candidate and may go.
function Repair-CleanPromotion() {
    $names = @(Get-VMSnapshot -VMName $Name | ForEach-Object { $_.Name })
    if ($names -contains 'clean-next' -and $names -notcontains 'clean') {
        Rename-VMSnapshot -VMName $Name -Name 'clean-next' -NewName 'clean'
        Write-Ref 'finished an interrupted promotion of clean-next to clean'
    }
}

# Everything that must hold in clean: the reference identity, the fhd-100 values
# read inside ref's session, and the bare desktop with focus.
function Get-ProfileProblems($Session) {
    $r = Invoke-RefInteractive $Session $script:ProfileProbe 300
    $script:SeenJson = $r.Output.Trim()
    $seen = $script:SeenJson | ConvertFrom-Json
    $script:Seen = $seen
    $problems = @()
    $want = Get-RefIdentity
    foreach ($k in 'edition', 'displayVersion', 'build') {
        if ("$($seen.$k)" -ne "$($want.$k)") { $problems += "$k is $($seen.$k), docs/design/inventory/reference.json says $($want.$k)" }
    }
    foreach ($k in $FhdProfile.Keys) {
        if ("$($seen.$k)" -ne "$($FhdProfile[$k])") { $problems += "$k is $($seen.$k), want $($FhdProfile[$k])" }
    }
    if ($script:QuietForeground -notcontains $seen.foreground) { $problems += "the foreground is $($seen.foreground), not the desktop" }
    if ($seen.session -eq 0) { $problems += 'the probe ran in session 0, not the desktop' }
    return $problems
}

# Take clean again from pre-oobe: the same restore a replay of first-run setup
# uses, without reinstalling Windows.
function Invoke-Reclean() {
    $names = @(Get-VMSnapshot -VMName $Name | ForEach-Object { $_.Name })
    if ($names -notcontains 'pre-oobe') { Write-Host "ref-vm: $Name has no pre-oobe checkpoint; build it with -Rebuild"; $script:Refused = 1; return }
    $script:LogPath = Join-Path $VmPath 'ref-vm.log'
    Write-Ref 'taking clean again from pre-oobe'
    Restore-RefCheckpoint $Name 'pre-oobe' $VmPath
    $pw = (Get-RefCredential $VmPath).GetNetworkCredential().Password
    Complete-Oobe $pw
    Complete-Clean
}

Enter-RefLock $Name
$script:ProfileProbe = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ios\probe\probe-profile.ps1'
$script:QuietForeground = @('Progman', 'WorkerW', 'Shell_TrayWnd')
# Every path ends with the VM off, a failed one included: a running reference VM
# holds 4 GiB of the host. The exit comes after the finally, because cmdlets in a
# finally entered by exit are skipped (seen 2026-11-14).
# A refusal sets $script:Refused and returns, for the same reason.
$rc = 1
$script:Refused = 0
try {
    if ($Reclean) { Invoke-Reclean } elseif (-not $Verify) { Invoke-Build }
    if ($script:Refused) { $rc = $script:Refused } else { $rc = Invoke-Verify | Select-Object -Last 1 }
} finally {
    $vm = Get-VM | Where-Object { $_.Name -eq $Name }
    if ($vm -and $vm.State -ne 'Off') { Stop-VM -Name $Name -TurnOff -Force }
}
exit $rc
