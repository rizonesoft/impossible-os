<#
.SYNOPSIS
Builds and verifies the Windows 11 reference VM the parity inventory is captured from.

.DESCRIPTION
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 2.

Build (no switch): creates the Generation 2 VM from the ISO, installs Windows
unattended to the first setup page, takes the checkpoint pre-oobe there,
completes setup with the local account ref, sets the fhd-100 profile, installs
the capture runtime of runtime.json (Python and comtypes, fetched once into
-RuntimeCache and checked by SHA-256, since the VM has no network) and takes the
checkpoint clean. Refuses when the VM exists unless -Rebuild is given.

-Verify: restores clean, reads the profile inside ref's interactive session and
exits 0 only when every value holds, the bare desktop has focus and the capture
runtime imports at the pinned versions. Prints
"ref-vm: ios-win11-ref ok, profile fhd-100".

-Verify -Profile <id>|live|all: the same, then applies each named live profile of
profiles.json (scaling, light or dark, contrast theme, text size) in ref's
session with guest\set-profile.ps1 and checks it with the same probe, printing
"ref-vm: profile <id> ok" for each. "live" is every profile that differs from
fhd-100 in live settings only, and "all" every profile; a profile that needs a changed machine
(resolution, displays, display language) is refused by name.

-Reclean: restores pre-oobe, completes setup again and replaces clean, without
reinstalling Windows; then verifies. Use it when the profile or the probes change.

-Derive -Profile <id>|machine: takes the checkpoint profile-<id> from clean for a
profile that needs a changed machine on one display: the VM's video mode for
another resolution (Set-VMVideo, with the guest shut down), for another
display language or added input methods (inputMethods) the language packs, installed through a network adapter on the
Default Switch that exists only while it installs (automatic updates and Store
updates are held off by policy for that time, and the build is checked after),
then the user's display language. The live values are applied with
set-profile.ps1 and the whole profile is probed before the checkpoint is taken;
an existing profile-<id> is replaced only once its successor exists. "machine"
derives every such profile. A two-display profile is refused: Hyper-V gives a VM
one display, so those are the stand-in machine's ("dual" in standin/variants.json).
-Verify -Profile all then restores each profile-<id> and probes it, and checks
each two-display profile on the stand-in machine (standin/probe.py).

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
    [string]$IsoPath = 'D:\11\win11.iso',
    [string]$VmPath = 'D:\11\ios-win11-ref',
    [string]$RuntimeCache = 'D:\11\runtime',
    [string]$Name = 'ios-win11-ref',
    [int]$ImageIndex = 6,
    [switch]$Verify,
    [switch]$Rebuild,
    [switch]$Reclean,
    [switch]$Derive,
    [Alias('Profile')][string]$ProfileId = 'fhd-100'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ref-common.ps1')
Resolve-RefTarget $Name VmPath, RuntimeCache, IsoPath

# fhd-100: the profile every canonical record is captured under. Its values live
# once, in docs/design/inventory/profiles.json, which the capture tool reads too.
$ProfilesPath = Join-Path $PSScriptRoot '..\..\docs\design\inventory\profiles.json'
$AllProfiles = Get-Content -Raw -LiteralPath $ProfilesPath | ConvertFrom-Json
$FhdProfile = $AllProfiles.'fhd-100'
if (-not $FhdProfile) { throw 'docs/design/inventory/profiles.json has no fhd-100 profile' }
# The settings set-profile.ps1 applies in ref's session; any other difference
# from fhd-100 needs a changed machine.
$LiveKeys = 'dpi', 'appsLight', 'systemLight', 'highContrast', 'textScale', 'secondDpi', 'secondSide'
$GuestScripts = 'probe-profile.ps1', 'probe-notepad.ps1', 'set-profile.ps1'
# The machine keys a derived Hyper-V checkpoint provides; a profile needing any
# other (a second display) is the stand-in machine's.
$DerivableKeys = 'width', 'height', 'uiLanguage', 'inputMethods'

function Get-MachineKeys($Want) {
    return @($Want.PSObject.Properties | Where-Object { $LiveKeys -notcontains $_.Name -and "$($_.Value)" -ne "$($FhdProfile.($_.Name))" } | ForEach-Object { $_.Name })
}

function Send-GuestScripts($Session) {
    Invoke-Command -Session $Session -ScriptBlock { New-Item -ItemType Directory -Path 'C:\ios\probe' -Force | Out-Null }
    foreach ($f in $GuestScripts) { Send-RefFile $Session (Join-Path $PSScriptRoot "guest\$f") "C:\ios\probe\$f" }
    Send-RefFile $Session $ProfilesPath 'C:\ios\probe\profiles.json'
}
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
            # The VM is on, and the SDK's machine does not start while it is: a wait
            # would only keep the memory from the job that has the place.
            Enter-RefTurn 1
            $script:RefTouched = $true
            Complete-Clean
            return
        }
    }
    if ($existing.Count) {
        if (-not $Rebuild) { Write-Host "ref-vm: $Name exists; -Rebuild removes it and its checkpoints"; $script:Refused = 3; return }
        # Checked before anything is deleted: a mistyped ISO path must not cost the reference.
        if (-not (Test-Path -LiteralPath $IsoPath -PathType Leaf)) { Write-Host "ref-vm: no ISO at $IsoPath; $Name is left as it was"; $script:Refused = 2; return }
        $script:RefTouched = $true
        Write-Ref "removing the existing $Name"
        Remove-RefVm $Name $VmPath
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
    $script:RefTouched = $true
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

    Start-RefVm $Name
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
# One file of runtime.json, from the cache or fetched into it, refused unless its
# SHA-256 matches the pin.
function Get-RefRuntimeFile($Entry) {
    New-Item -ItemType Directory -Path $RuntimeCache -Force | Out-Null
    $path = Join-Path $RuntimeCache ($Entry.url -split '/')[-1]
    if (-not (Test-Path -LiteralPath $path)) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Write-Ref "fetching $($Entry.url)"
        Invoke-WebRequest -UseBasicParsing -Uri $Entry.url -OutFile "$path.part"
        Move-Item -LiteralPath "$path.part" -Destination $path
    }
    $got = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($got -ne $Entry.sha256) {
        Remove-Item -LiteralPath $path
        throw "$path has SHA-256 $got, runtime.json pins $($Entry.sha256); the copy was removed"
    }
    return $path
}

# The capture runtime: embeddable Python with comtypes in C:\ios\python, on the
# machine PATH, with the UI Automation wrapper generated, so a capture needs no
# network and no first-run step.
function Install-RefRuntime($Session) {
    $rt = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'runtime.json') | ConvertFrom-Json
    $py = Get-RefRuntimeFile $rt.python
    $ct = Get-RefRuntimeFile $rt.comtypes
    Send-RefFile $Session $py 'C:\ios\stage\python.zip'
    Send-RefFile $Session $ct 'C:\ios\stage\comtypes.zip'
    $out = Invoke-Command -Session $Session -ScriptBlock {
        $ErrorActionPreference = 'Stop'
        $root = 'C:\ios\python'
        if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
        Expand-Archive -LiteralPath 'C:\ios\stage\python.zip' -DestinationPath $root
        Expand-Archive -LiteralPath 'C:\ios\stage\comtypes.zip' -DestinationPath "$root\Lib\site-packages"
        Remove-Item -LiteralPath 'C:\ios\stage' -Recurse -Force
        $pth = @(Get-ChildItem -LiteralPath $root -Filter '*._pth')
        if ($pth.Count -ne 1) { throw "expected one ._pth file in $root, found $($pth.Count)" }
        $stdlib = (Get-ChildItem -LiteralPath $root -Filter 'python3*.zip' | Select-Object -First 1).Name
        [IO.File]::WriteAllText($pth[0].FullName, "$stdlib`r`n.`r`nLib\site-packages`r`nimport site`r`n")
        $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
        if (($machine -split ';') -notcontains $root) { [Environment]::SetEnvironmentVariable('Path', "$root;$machine", 'Machine') }
        $o = & "$root\python.exe" -c "import comtypes.client as c; c.GetModule('UIAutomationCore.dll'); import comtypes.gen.UIAutomationClient, sys, comtypes; print(sys.version.split()[0], comtypes.__version__)" 2>&1
        if ($LASTEXITCODE -ne 0) { throw "the capture runtime does not import: $o" }
        "$o"
    }
    if ("$out".Trim() -ne "$($rt.python.version) $($rt.comtypes.version)") { throw "the capture runtime reports '$out', runtime.json pins $($rt.python.version) $($rt.comtypes.version)" }
    Write-Ref "capture runtime installed: Python and comtypes $out"
}

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
        Send-GuestScripts $s
        Install-RefRuntime $s
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
    Repair-RefPromotion $Name 'clean'
    if (Get-VMSnapshot -VMName $Name | Where-Object { $_.Name -eq 'clean-next' }) { Remove-VMSnapshot -VMName $Name -Name 'clean-next' }
    # Renamed through the object Checkpoint-VM returns, as Invoke-Derive does: a
    # lookup by name straight after the checkpoint did not find it (2026-11-19).
    $next = Checkpoint-VM -Name $Name -SnapshotName 'clean-next' -Passthru
    if (Get-VMSnapshot -VMName $Name | Where-Object { $_.Name -eq 'clean' }) { Remove-VMSnapshot -VMName $Name -Name 'clean' }
    Rename-VMSnapshot -VMSnapshot $next -NewName 'clean'
    Write-Ref 'checkpoint clean taken'
}

function Invoke-Verify() {
    $problems = @()
    $vm = Get-VM | Where-Object { $_.Name -eq $Name } | Select-Object -First 1
    if (-not $vm) { Write-Host "ref-vm: no VM named $Name"; return 1 }
    Repair-RefPromotion $Name 'clean'
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
            # This checkout's probes, not the copies baked into clean.
            Send-GuestScripts $s
            $problems += @(Get-ProfileProblems $s)
            $problems += @(Get-RuntimeProblems $s)
            $base = $script:Seen
            $script:MachineIds = @()
            if ($problems.Count -eq 0) { $problems += @(Test-LiveProfiles $s) }
            $script:Seen = $base
        } finally { if ($s) { Remove-PSSession $s } }
        foreach ($id in $script:MachineIds) { $problems += @(Test-DerivedProfile $id) }
    }
    if ($problems.Count) {
        foreach ($p in $problems) { Write-Host "ref-vm: $p" }
        return 1
    }
    Write-Host "ref-vm: $Name ok, profile fhd-100 (Windows 11 $($script:Seen.edition) $($script:Seen.displayVersion) build $($script:Seen.build))"
    return 0
}

# Everything that must hold in clean: the reference identity, the fhd-100 values
# read inside ref's session, and the bare desktop with focus.
function Get-RuntimeProblems($Session) {
    $rt = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'runtime.json') | ConvertFrom-Json
    $want = "$($rt.python.version) $($rt.comtypes.version)"
    $r = Invoke-RefInteractive $Session 'python -c "import comtypes.gen.UIAutomationClient, sys, comtypes; print(sys.version.split()[0], comtypes.__version__)"' 120
    if ($r.ExitCode -ne 0 -or $r.Output.Trim() -ne $want) { return @("capture runtime reports '$($r.Output.Trim())' (exit $($r.ExitCode)), want $want") }
    return @()
}

# Apply each profile -ProfileId names in turn and check it. set-profile.ps1 sets
# every live value absolutely, so one restore of clean serves them all.
function Test-LiveProfiles($Session) {
    if ($ProfileId -eq 'fhd-100') { return @() }
    $others = @($AllProfiles.PSObject.Properties | Where-Object { $_.Name -ne 'fhd-100' })
    if ($ProfileId -eq 'live') {
        $ids = @($others | Where-Object { (Get-MachineKeys $_.Value).Count -eq 0 } | ForEach-Object { $_.Name })
    } elseif ($ProfileId -eq 'all') {
        $ids = @($others | ForEach-Object { $_.Name })
    } elseif ($ProfileId -eq 'machine') {
        $ids = @($others | Where-Object { (Get-MachineKeys $_.Value).Count } | ForEach-Object { $_.Name })
    } else { $ids = @($ProfileId) }
    $problems = @()
    foreach ($id in $ids) {
        $want = $AllProfiles.$id
        if (-not $want) { $problems += "no profile $id in docs/design/inventory/profiles.json"; continue }
        $machine = @(Get-MachineKeys $want)
        if ($machine.Count) {
            if ('all', 'machine', $id -contains $ProfileId) { $script:MachineIds += $id }
            else { $problems += "profile $id needs a changed machine ($($machine -join ', ')), not a live setting" }
            continue
        }
        $r = Invoke-RefInteractive $Session "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ios\probe\set-profile.ps1 -Profiles C:\ios\probe\profiles.json -Name $id" 300
        if ($r.ExitCode -ne 0) { $problems += "profile $id not applied: $("$($r.Output)".Trim())"; continue }
        $wrong = @(Get-ProfileProblems $Session $want)
        if ($wrong.Count) { $problems += @($wrong | ForEach-Object { "profile ${id}: $_" }) } else { Write-Host "ref-vm: profile $id ok" }
    }
    # Back to fhd-100, so a later step in the same session sees the canonical profile.
    if ($ids.Count) { [void](Invoke-RefInteractive $Session "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ios\probe\set-profile.ps1 -Profiles C:\ios\probe\profiles.json -Name fhd-100" 300) }
    return $problems
}

# The differences between what the probe saw and the build and profile it should
# be. Pure, so tests/test_capture.py runs it; PowerShell names are case-blind, so
# no local here may share a parameter's name.
function Compare-RefProfile($Seen, $Identity, $Expected, $QuietForeground) {
    $problems = @()
    foreach ($k in 'edition', 'displayVersion', 'build') {
        if ("$($Seen.$k)" -ne "$($Identity.$k)") { $problems += "$k is $($Seen.$k), docs/design/inventory/reference.json says $($Identity.$k)" }
    }
    foreach ($p in $Expected.PSObject.Properties) {
        if ("$($Seen.($p.Name))" -ne "$($p.Value)") { $problems += "$($p.Name) is $($Seen.($p.Name)), want $($p.Value)" }
    }
    if ($QuietForeground -notcontains $Seen.foreground) { $problems += "the foreground is $($Seen.foreground), not the desktop" }
    if ($Seen.session -eq 0) { $problems += 'the probe ran in session 0, not the desktop' }
    return $problems
}

function Get-ProfileProblems($Session, $Expected = $FhdProfile) {
    $r = Invoke-RefInteractive $Session $script:ProfileProbe 300
    $script:SeenJson = $r.Output.Trim()
    $script:Seen = $script:SeenJson | ConvertFrom-Json
    return @(Compare-RefProfile $script:Seen (Get-RefIdentity) $Expected $script:QuietForeground)
}

# A machine profile is checked on its own checkpoint, restored like any capture's.
function Test-DerivedProfile([string]$Id) {
    $want = $AllProfiles.$Id
    $other = @(Get-MachineKeys $want | Where-Object { $DerivableKeys -notcontains $_ })
    if ($other.Count) { return @(Test-StandinProfile $Id $want) }
    $cp = "profile-$Id"
    Repair-RefPromotion $Name $cp
    if (-not (Get-VMSnapshot -VMName $Name | Where-Object { $_.Name -eq $cp })) { return @("missing checkpoint $cp (ref-vm.ps1 -Derive -Profile $Id)") }
    $s = $null
    try {
        $s = Start-RefFrom $Name $VmPath $cp
        Send-GuestScripts $s
        $wrong = @(Get-ProfileProblems $s $want)
        if ($wrong.Count) { return @($wrong | ForEach-Object { "profile ${Id}: $_" }) }
        Write-Host "ref-vm: profile $Id ok (checkpoint $cp)"
        return @()
    } finally { if ($s) { Remove-PSSession $s } }
}

# A profile with two displays is the stand-in machine's (Hyper-V gives a VM one
# display): standin/probe.py boots its variant under QEMU in WSL, applies the
# profile there and prints the probe's line, which is compared here like any other.
function Test-StandinProfile([string]$Id, $Want) {
    $linux = (& wsl.exe -e wslpath -u $PSScriptRoot | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $linux) { return @("profile ${Id}: this checkout's path has no WSL form ($PSScriptRoot)") }
    # The exit code is the probe's verdict. Under Stop, 5.1 made a wsl.exe
    # warning on stderr fatal ("Failed to translate" a dead PATH entry,
    # 2026-11-20), and a stderr line could also end up as the "last line" read
    # as the probe's JSON; so stderr is kept apart and only named on a failure.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $all = @(& wsl.exe -e python3 "$linux/standin/probe.py" $Id 2>&1) } finally { $ErrorActionPreference = $prev }
    $code = $LASTEXITCODE
    $out = @($all | Where-Object { $_ -isnot [Management.Automation.ErrorRecord] } | ForEach-Object { "$_" })
    $err = @($all | Where-Object { $_ -is [Management.Automation.ErrorRecord] } | ForEach-Object { "$_".TrimEnd() })
    if ($code -ne 0 -or -not $out.Count) { return @("profile ${Id} on the stand-in machine: $((@($err) + @($out) | Select-Object -Last 3) -join ' / ')") }
    $script:SeenJson = ($out | Select-Object -Last 1).Trim()
    $seen = $script:SeenJson | ConvertFrom-Json
    $wrong = @(Compare-RefProfile $seen (Get-RefIdentity) $Want $script:QuietForeground)
    if ($seen.remote) { $wrong += 'the stand-in probe ran in a remote session, not the console' }
    if ($wrong.Count) { return @($wrong | ForEach-Object { "profile ${Id} (stand-in): $_" }) }
    Write-Host "ref-vm: profile $Id ok (stand-in machine, console session)"
    return @()
}

# The machine profiles this VM can provide: they differ from fhd-100 in the
# resolution or the display language, and in nothing else a VM sets.
function Get-DerivableIds([string]$Which) {
    $ids = @()
    foreach ($p in $AllProfiles.PSObject.Properties) {
        $machine = @(Get-MachineKeys $p.Value)
        if (-not $machine.Count) { continue }
        if ($Which -ne 'machine' -and $Which -ne $p.Name) { continue }
        $other = @($machine | Where-Object { $DerivableKeys -notcontains $_ })
        if ($other.Count) {
            if ($Which -eq $p.Name) { throw "profile $($p.Name) needs $($other -join ', '): Hyper-V gives a VM one display, so it is the stand-in machine's (standin/probe.py)" }
            continue
        }
        $ids += $p.Name
    }
    if (-not $ids.Count) { throw "no machine profile named $Which in docs/design/inventory/profiles.json" }
    return $ids
}

# Language pack install with a network adapter that exists only for it. Policies
# hold off automatic and Store updates while the guest is online; both are
# removed again, with the adapter, before anything is checkpointed.
# -Typing installs each language with its features: an input method needs its
# typing feature (the Pinyin IME has no dictionary without it, seen 2026-11-18);
# a display language needs none of them.
function Install-RefLanguage($Session, [string[]]$Language, [switch]$Typing) {
    $adapter = 'ios-derive-net'
    # Only what is added here is removed again: the values, and a key only when
    # this function created it.
    $created = Invoke-Command -Session $Session -ScriptBlock {
        $made = @()
        foreach ($k in 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate', 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU', 'HKLM:\SOFTWARE\Policies\Microsoft\WindowsStore') {
            if (-not (Test-Path -LiteralPath $k)) { New-Item -Path $k | Out-Null; $made += $k }
        }
        Set-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU' -Name NoAutoUpdate -Value 1 -Type DWord
        Set-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Policies\Microsoft\WindowsStore' -Name AutoDownload -Value 2 -Type DWord
        return $made
    }
    try {
        Add-VMNetworkAdapter -VMName $Name -SwitchName 'Default Switch' -Name $adapter
        Write-Ref "installing the $($Language -join ', ') language pack(s) (temporary network)"
        Invoke-Command -Session $Session -ArgumentList $Language, [bool]$Typing -ScriptBlock {
            param($lang, $typing)
            $ErrorActionPreference = 'Stop'
            $deadline = (Get-Date).AddMinutes(3)
            while (-not (Test-NetConnection -ComputerName 'www.msftconnecttest.com' -Port 80 -InformationLevel Quiet -WarningAction SilentlyContinue)) {
                if ((Get-Date) -gt $deadline) { throw 'the guest found no network on the Default Switch' }
                Start-Sleep -Seconds 5
            }
            foreach ($one in $lang) {
                if ($typing) { Install-Language -Language $one | Out-Null } else { Install-Language -Language $one -ExcludeFeatures | Out-Null }
                if (-not (Get-InstalledLanguage -Language $one)) { throw "Install-Language $one reported success but the language is not installed" }
            }
        }
    } finally {
        Get-VMNetworkAdapter -VMName $Name | Where-Object { $_.Name -eq $adapter } | Remove-VMNetworkAdapter
        Invoke-Command -Session $Session -ArgumentList (, @($created)) -ScriptBlock {
            param($made)
            Remove-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU' -Name NoAutoUpdate -ErrorAction SilentlyContinue
            Remove-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Policies\Microsoft\WindowsStore' -Name AutoDownload -ErrorAction SilentlyContinue
            # Deepest first, so a parent is empty when its turn comes.
            foreach ($k in @($made | Sort-Object Length -Descending)) { Remove-Item -LiteralPath $k -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }
}

function Invoke-DeriveProfile([string]$Id) {
    $want = $AllProfiles.$Id
    Write-Ref "deriving profile-$Id from clean"
    # A validated profile-<id>-next left by an interrupted promotion becomes
    # profile-<id> before the stale -next below is removed.
    Repair-RefPromotion $Name "profile-$Id"
    $s = Start-RefFrom $Name $VmPath 'clean'
    try {
        Clear-RefPendingSetup $s
        if ("$($want.uiLanguage)" -ne "$($FhdProfile.uiLanguage)") {
            Install-RefLanguage $s $want.uiLanguage
            # ref's display language, as Settings sets it: the language first in ref's
            # list and as the override, the US keyboard kept as the input method so a
            # typed fixture types the same text, and the format the profile's own.
            $l = $want.uiLanguage
            $r = Invoke-RefInteractive $s ("powershell.exe -NoProfile -Command `$l = New-WinUserLanguageList $l; `$l.Add('en-US'); " +
                "Set-WinUserLanguageList `$l -Force; Set-WinUILanguageOverride -Language $l; " +
                "Set-WinDefaultInputMethodOverride -InputTip '0409:00000409'; Set-Culture $($want.format)") 120
            if ($r.ExitCode -ne 0) { throw "display language not set: $($r.Output.Trim())" }
        }
        if ("$($want.inputMethods)" -ne "$($FhdProfile.inputMethods)") {
            # Input methods only: each language joins ref's list after the display
            # language, bringing its default IME (Microsoft IME for ja-JP, Microsoft
            # Pinyin for zh-CN); the display language and the format stay as they are.
            $tags = @("$($want.inputMethods)" -split ',')
            if ($tags[0] -ne $want.uiLanguage) { throw "profile ${Id}: inputMethods must start with its display language $($want.uiLanguage)" }
            Install-RefLanguage $s @($tags | Select-Object -Skip 1) -Typing
            $r = Invoke-RefInteractive $s ("powershell.exe -NoProfile -Command `$l = New-WinUserLanguageList $($tags[0]); " +
                (@($tags | Select-Object -Skip 1 | ForEach-Object { "`$l.Add('$_'); " }) -join '') +
                "Set-WinUserLanguageList `$l -Force") 120
            if ($r.ExitCode -ne 0) { throw "input methods not set: $($r.Output.Trim())" }
        }
    } finally { Remove-PSSession $s }
    # The video mode is set with the guest off; the language takes effect at the
    # next sign-in, which autologon makes.
    Stop-VM -Name $Name -Force
    Set-VMVideo -VMName $Name -ResolutionType Single -HorizontalResolution $want.width -VerticalResolution $want.height
    Start-RefVm $Name
    $s = New-RefSession $Name $VmPath
    try {
        Send-GuestScripts $s
        $wrong = @(Test-RefIdentity $s $Name)
        if ($wrong.Count) { throw "the build changed while online: $($wrong -join '; ')" }
        $r = Invoke-RefInteractive $s "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ios\probe\set-profile.ps1 -Profiles C:\ios\probe\profiles.json -Name $Id" 300
        if ($r.ExitCode -ne 0) { throw "profile $Id not applied: $("$($r.Output)".Trim())" }
        # As for clean: the first sign-in may open Start; the desktop must have focus.
        $problems = @('not checked')
        for ($try = 1; $try -le 5 -and $problems.Count; $try++) {
            Start-Sleep -Seconds 20
            Send-RefKey $Name 0x1B
            # After a cold sign-in no window holds focus; clean's desktop got it when
            # Start closed. Win+D gives it to the desktop the same way a user would.
            Send-RefChord $Name 0x5B 0x44
            Start-Sleep -Seconds 3
            $problems = @(Get-ProfileProblems $s $want)
            Write-Ref "profile-$Id before checkpoint (try $try): $($script:SeenJson)"
        }
        if ($problems.Count) {
            # Say what ref's session holds, so a language that did not take names why.
            $d = Invoke-RefInteractive $s ("powershell.exe -NoProfile -Command `"'override=' + (Get-WinUILanguageOverride).Name; " +
                "'list=' + ((Get-WinUserLanguageList).LanguageTag -join ','); 'installed=' + ((Get-InstalledLanguage).LanguageId -join ',')`"") 120
            throw "profile-$Id not taken: $($problems -join '; ') ($("$($d.Output)".Trim() -replace '\s+', ' '))"
        }
    } finally { Remove-PSSession $s }
    if (@(Get-VMNetworkAdapter -VMName $Name).Count) { throw "profile-$Id not taken: the VM still has a network adapter" }
    $cp = "profile-$Id"
    foreach ($old in @(Get-VMSnapshot -VMName $Name | Where-Object { $_.Name -eq "$cp-next" })) { Remove-VMSnapshot -VMSnapshot $old }
    # Renamed through the object Checkpoint-VM returns: a lookup by name straight
    # after the checkpoint did not find it (seen 2026-11-16).
    $next = Checkpoint-VM -Name $Name -SnapshotName "$cp-next" -Passthru
    foreach ($old in @(Get-VMSnapshot -VMName $Name | Where-Object { $_.Name -eq $cp })) { Remove-VMSnapshot -VMSnapshot $old }
    Rename-VMSnapshot -VMSnapshot $next -NewName $cp
    Write-Ref "checkpoint $cp taken"
}

function Invoke-Derive() {
    $script:LogPath = Join-Path $VmPath 'ref-vm.log'
    $ids = Get-DerivableIds $ProfileId
    foreach ($id in $ids) { Invoke-DeriveProfile $id }
}

# Take clean again from pre-oobe: the same restore a replay of first-run setup
# uses, without reinstalling Windows.
function Invoke-Reclean() {
    $names = @(Get-VMSnapshot -VMName $Name | ForEach-Object { $_.Name })
    if ($names -notcontains 'pre-oobe') { Write-Host "ref-vm: $Name has no pre-oobe checkpoint; build it with -Rebuild"; $script:Refused = 1; return }
    $script:LogPath = Join-Path $VmPath 'ref-vm.log'
    Write-Ref 'taking clean again from pre-oobe'
    # The credential is loaded first: a failure here leaves the VM as it was.
    $pw = (Get-RefCredential $VmPath).GetNetworkCredential().Password
    Restore-RefCheckpoint $Name 'pre-oobe' $VmPath
    Complete-Oobe $pw
    Complete-Clean
}

Enter-RefLock $Name
Enter-RefFolder $Name $VmPath
$script:ProfileProbe = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ios\probe\probe-profile.ps1'
$script:QuietForeground = @('Progman', 'WorkerW', 'Shell_TrayWnd')
# Every path that changed the VM ends with it off, a failed one included: a
# running reference VM holds 4 GiB of the host. A refusal changes nothing and
# leaves the VM as it was. The exit comes after the finally, because cmdlets in
# a finally entered by exit are skipped (seen 2026-11-14); a refusal sets
# $script:Refused and returns for the same reason.
$rc = 1
$script:Refused = 0
try {
    if ($Derive) { Invoke-Derive } elseif ($Reclean) { Invoke-Reclean } elseif (-not $Verify) { Invoke-Build }
    if ($script:Refused) { $rc = $script:Refused } else { $rc = Invoke-Verify | Select-Object -Last 1 }
} finally {
    $vm = Get-VM | Where-Object { $_.Name -eq $Name }
    if ($script:RefTouched -and $vm -and $vm.State -ne 'Off') { Stop-VM -Name $Name -TurnOff -Force }
    Exit-RefTurn
}
exit $rc
