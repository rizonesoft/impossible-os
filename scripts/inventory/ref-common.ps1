# Shared helpers for the Windows 11 reference VM (ref-vm.ps1, ref-run.ps1).
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 2.
# Windows PowerShell 5.1; needs Hyper-V Administrators membership, not elevation.

$script:Ns = 'root\virtualization\v2'

function Write-Ref([string]$Message) {
    $line = '{0} {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message
    Write-Host "ref: $line"
    if ($script:LogPath) { Add-Content -LiteralPath $script:LogPath -Value $line }
}

function Wait-Until([scriptblock]$Test, [int]$TimeoutSec, [string]$What, [int]$EverySec = 10) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $r = & $Test
        if ($r) { return $r }
        Start-Sleep -Seconds $EverySec
    }
    throw "timed out after $TimeoutSec s waiting for $What"
}

function Get-RefSystem([string]$Name) {
    $vm = Get-CimInstance -Namespace $script:Ns -ClassName Msvm_ComputerSystem -Filter "ElementName='$Name'"
    if (-not $vm) { throw "no VM named $Name" }
    return $vm
}

# Values the guest writes under HKLM\SOFTWARE\Microsoft\Virtual Machine\Guest (the
# Auto key is the guest's intrinsic pool and is reported separately).
function Get-GuestKvp([string]$Name) {
    $kvp = Get-CimAssociatedInstance -InputObject (Get-RefSystem $Name) -ResultClassName Msvm_KvpExchangeComponent
    $values = @{}
    foreach ($item in @($kvp.GuestExchangeItems)) {
        if (-not $item) { continue }
        $doc = [xml]$item
        $n = ($doc.INSTANCE.PROPERTY | Where-Object { $_.NAME -eq 'Name' }).VALUE
        $d = ($doc.INSTANCE.PROPERTY | Where-Object { $_.NAME -eq 'Data' }).VALUE
        if ($n) { $values[$n] = $d }
    }
    return $values
}

function Get-RefKeyboard([string]$Name) {
    return Get-CimAssociatedInstance -InputObject (Get-RefSystem $Name) -ResultClassName Msvm_Keyboard
}

function Send-RefKey([string]$Name, [uint32]$Key) {
    [void](Invoke-CimMethod -InputObject (Get-RefKeyboard $Name) -MethodName TypeKey -Arguments @{ keyCode = $Key })
}

# Host-to-guest values already set on the VM, by name.
function Get-HostKvp([string]$Name) {
    $vm = Get-WmiObject -Namespace $script:Ns -Class Msvm_ComputerSystem -Filter "ElementName='$Name'"
    $sd = $vm.GetRelated('Msvm_KvpExchangeComponent') | ForEach-Object { $_.GetRelated('Msvm_KvpExchangeComponentSettingData') }
    $values = @{}
    foreach ($x in @($sd.HostExchangeItems)) {
        if (-not $x) { continue }
        $doc = [xml]$x
        $values[($doc.INSTANCE.PROPERTY | Where-Object { $_.NAME -eq 'Name' }).VALUE] = ($doc.INSTANCE.PROPERTY | Where-Object { $_.NAME -eq 'Data' }).VALUE
    }
    return $values
}

# Set, or with -Remove clear, a host-to-guest value; the guest reads it under
# HKLM\SOFTWARE\Microsoft\Virtual Machine\External.
function Set-HostKvp([string]$Name, [string]$Key, [string]$Value = '', [switch]$Remove) {
    $svc = Get-WmiObject -Namespace $script:Ns -Class Msvm_VirtualSystemManagementService
    $vm = Get-WmiObject -Namespace $script:Ns -Class Msvm_ComputerSystem -Filter "ElementName='$Name'"
    $item = ([wmiclass]"\\.\${script:Ns}:Msvm_KvpExchangeDataItem").CreateInstance()
    $item.Name = $Key; $item.Data = $Value; $item.Source = 0
    $text = $item.PSBase.GetText([Management.TextFormat]::CimDtd20)
    $present = (Get-HostKvp $Name).ContainsKey($Key)
    if ($Remove) {
        if (-not $present) { return }
        $r = $svc.RemoveKvpItems($vm, @($text))
    } elseif ($present) {
        $r = $svc.ModifyKvpItems($vm, @($text))
    } else {
        $r = $svc.AddKvpItems($vm, @($text))
    }
    if ($r.ReturnValue -eq 0) { return }
    if ($r.ReturnValue -ne 4096) { throw "KVP $Key failed: $($r.ReturnValue)" }
    $job = [wmi]$r.Job
    $deadline = (Get-Date).AddSeconds(120)
    while ($job.JobState -eq 3 -or $job.JobState -eq 4) {
        if ((Get-Date) -gt $deadline) { throw "KVP $Key job still in state $($job.JobState) after 120 s" }
        Start-Sleep -Milliseconds 300; $job = [wmi]$r.Job
    }
    if ($job.JobState -ne 7) { throw "KVP $Key job failed: $($job.ErrorDescription)" }
}

# The VM's screen as 640x360 RGB565 bytes, read from the host without the guest's help.
function Get-RefThumbnail([string]$Name) {
    $vm = Get-RefSystem $Name
    $svc = Get-CimInstance -Namespace $script:Ns -ClassName Msvm_VirtualSystemManagementService
    $vssd = Get-CimAssociatedInstance -InputObject $vm -ResultClassName Msvm_VirtualSystemSettingData |
        Where-Object { $_.VirtualSystemType -eq 'Microsoft:Hyper-V:System:Realized' }
    $r = Invoke-CimMethod -InputObject $svc -MethodName GetVirtualSystemThumbnailImage -Arguments @{
        TargetSystem = $vssd; WidthPixels = [uint16]640; HeightPixels = [uint16]360 }
    if ($r.ReturnValue -ne 0 -or -not $r.ImageData) { throw "thumbnail failed: $($r.ReturnValue)" }
    return [byte[]]$r.ImageData
}

function Get-ThumbnailFacts([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $hash = ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    $colours = New-Object 'System.Collections.Generic.HashSet[uint16]'
    for ($i = 0; $i -lt $Bytes.Length; $i += 2) { [void]$colours.Add([BitConverter]::ToUInt16($Bytes, $i)) }
    return @{ Hash = $hash; Colours = $colours.Count }
}

function Save-Thumbnail([byte[]]$Bytes, [string]$Path) {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap 640, 360, ([System.Drawing.Imaging.PixelFormat]::Format16bppRgb565)
    $rect = New-Object System.Drawing.Rectangle 0, 0, 640, 360
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
    try {
        for ($y = 0; $y -lt 360; $y++) {
            [Runtime.InteropServices.Marshal]::Copy($Bytes, $y * 1280, [IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), 1280)
        }
    } finally { $bmp.UnlockBits($data) }
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# The operator-only folder rule: the folder's whole access list is replaced by
# full control for this user and SYSTEM, inherited by everything made in it, so
# no grant left on a reused folder reaches the password media (Hyper-V adds the
# VM's own identity to each file it attaches).
function Protect-RefPath([string]$Path) {
    $acl = New-Object Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    $inherit = [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
    foreach ($who in [Security.Principal.WindowsIdentity]::GetCurrent().User, (New-Object Security.Principal.SecurityIdentifier 'S-1-5-18')) {
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($who, 'FullControl', $inherit, 'None', 'Allow')))
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}

# One operation per VM at a time: two runs would restore each other's checkpoint
# and share C:\ios\run. Keyed to the VM name, not the folder, since two folders
# can name the same VM; lower-cased because Hyper-V matches names without case
# and mutex names are case-sensitive; Global, so another Windows session sees
# it. Windows releases the mutex when the process exits.
function Enter-RefLock([string]$Name) {
    $script:RefLock = New-Object Threading.Mutex($false, "Global\ios-ref-vm-$($Name.ToLowerInvariant())")
    try { $held = $script:RefLock.WaitOne(0) } catch [Threading.AbandonedMutexException] { $held = $true }
    if (-not $held) {
        Write-Host "ref: another ref-vm or ref-run is using $Name"
        exit 4
    }
}

# The reference build the inventory is pinned to, from the repository.
function Get-RefIdentity() {
    return Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\docs\design\inventory\reference.json') -Raw | ConvertFrom-Json
}

# The guest's edition, version and build, read over the session.
function Test-RefIdentity($Session) {
    $want = Get-RefIdentity
    $seen = Invoke-Command -Session $Session -ScriptBlock {
        $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
        @{ edition = $cv.EditionID; displayVersion = $cv.DisplayVersion; build = "$($cv.CurrentBuild).$($cv.UBR)" }
    }
    $problems = @()
    foreach ($k in 'edition', 'displayVersion', 'build') {
        if ("$($seen[$k])" -ne "$($want.$k)") { $problems += "$k is $($seen[$k]), reference.json says $($want.$k)" }
    }
    return $problems
}

function Get-RefCredential([string]$VmPath) {
    $file = Join-Path $VmPath 'ref.clixml'
    if (-not (Test-Path -LiteralPath $file)) { throw "no credential at $file; run ref-vm.ps1 first" }
    return Import-Clixml -LiteralPath $file
}

function New-RefSession([string]$Name, [string]$VmPath, [int]$TimeoutSec = 600) {
    $cred = Get-RefCredential $VmPath
    return Wait-Until -TimeoutSec $TimeoutSec -What 'PowerShell Direct as ref' -Test {
        try { New-PSSession -VMName $Name -Credential $cred -ErrorAction Stop } catch { $null }
    }
}

# Run one command line inside ref's interactive desktop session and wait for its
# exit code. PowerShell Direct is only the transport: its own process has no
# desktop, so the command is started by an interactive scheduled task.
function Invoke-RefInteractive($Session, [string]$CommandLine, [int]$TimeoutSec = 900) {
    Invoke-Command -Session $Session -ArgumentList $CommandLine -ScriptBlock {
        param($cmd)
        $run = 'C:\ios\run'
        if (Test-Path -LiteralPath $run) { Remove-Item -LiteralPath $run -Recurse -Force }
        New-Item -ItemType Directory -Path $run, 'C:\ios\out' -Force | Out-Null
        Set-Content -LiteralPath "$run\run.cmd" -Encoding ASCII -Value @(
            '@echo off', 'cd /d C:\ios', "$cmd > C:\ios\run\stdout.txt 2>&1", '> C:\ios\run\rc.tmp echo %ERRORLEVEL%',
            'move /y C:\ios\run\rc.tmp C:\ios\run\rc.txt > nul')
        # A headless console host: the command draws no window over the surface
        # it captures (a plain cmd.exe window covered the desktop, seen 2026-11-14).
        $action = New-ScheduledTaskAction -Execute 'conhost.exe' -Argument '--headless cmd.exe /c C:\ios\run\run.cmd'
        $principal = New-ScheduledTaskPrincipal -UserId 'ref' -LogonType Interactive -RunLevel Highest
        Register-ScheduledTask -TaskName 'ios-run' -Action $action -Principal $principal -Force | Out-Null
        Start-ScheduledTask -TaskName 'ios-run'
    }
    $rc = Wait-Until -TimeoutSec $TimeoutSec -EverySec 3 -What "the interactive command: $CommandLine" -Test {
        Invoke-Command -Session $Session -ScriptBlock {
            # The redirect goes first: "echo 0> file" would redirect handle 0 and leave the file empty.
            if (Test-Path 'C:\ios\run\rc.txt') { "$(Get-Content 'C:\ios\run\rc.txt' -Raw)".Trim() }
        }
    }
    $out = Invoke-Command -Session $Session -ScriptBlock {
        Unregister-ScheduledTask -TaskName 'ios-run' -Confirm:$false
        Get-Content 'C:\ios\run\stdout.txt' -Raw
    }
    if ($rc -notmatch '^-?[0-9]+$') { throw "the interactive command wrote no exit code: '$rc'" }
    return @{ ExitCode = [int]$rc; Output = $out }
}

# Copy files into or out of the guest as bytes over the session. Copy-Item
# -ToSession fails with "The request is not supported" when the source is on the
# WSL share this repository lives on (seen 2026-11-14). Trees move as one zip, so
# the cost is one remote call, not one per file.
function Send-RefFile($Session, [string]$Source, [string]$Destination) {
    $bytes = [IO.File]::ReadAllBytes($Source)
    Invoke-Command -Session $Session -ArgumentList $bytes, $Destination -ScriptBlock {
        param([byte[]]$b, $to)
        New-Item -ItemType Directory -Path (Split-Path -Parent $to) -Force | Out-Null
        [IO.File]::WriteAllBytes($to, $b)
    }
}

function Send-RefTree($Session, [string]$Source, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $root = (Resolve-Path -LiteralPath $Source).ProviderPath
    $zip = Join-Path $env:TEMP "ios-in-$PID.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    try {
        [IO.Compression.ZipFile]::CreateFromDirectory($root, $zip)
        Invoke-Command -Session $Session -ArgumentList ([IO.File]::ReadAllBytes($zip)), $Destination -ScriptBlock {
            param([byte[]]$b, $to)
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $z = Join-Path $env:TEMP 'ios-in.zip'
            [IO.File]::WriteAllBytes($z, $b)
            [IO.Compression.ZipFile]::ExtractToDirectory($z, $to)
            Remove-Item -LiteralPath $z -Force
        }
    } finally { if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force } }
}

# Returns the number of files received. $Destination must not hold earlier output.
function Receive-RefTree($Session, [string]$Source, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $bytes = Invoke-Command -Session $Session -ArgumentList $Source -ScriptBlock {
        param($from)
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $z = Join-Path $env:TEMP 'ios-out.zip'
        if (Test-Path -LiteralPath $z) { Remove-Item -LiteralPath $z -Force }
        [IO.Compression.ZipFile]::CreateFromDirectory($from, $z)
        $b = [IO.File]::ReadAllBytes($z)
        Remove-Item -LiteralPath $z -Force
        , $b
    }
    $zip = Join-Path $env:TEMP "ios-out-$PID.zip"
    try {
        [IO.File]::WriteAllBytes($zip, [byte[]]$bytes)
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try { $count = @($archive.Entries | Where-Object { $_.Name }).Count } finally { $archive.Dispose() }
        [IO.Compression.ZipFile]::ExtractToDirectory($zip, $Destination)
    } finally { if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force } }
    return $count
}

# Restore a checkpoint and start the VM from it. Hyper-V opens attached media as
# the VM's own account, and the VM folder does not inherit its access entries
# (Protect-RefPath), so the entry is granted again for every attached file in
# it: a restore of pre-oobe was refused "Access is denied" on answer-install.iso
# without it (seen 2026-11-14). Media outside the folder, such as the operator's
# ISO, is not ours to change and keeps the access Hyper-V gave it.
function Restore-RefCheckpoint([string]$Name, [string]$Checkpoint, [string]$VmPath) {
    # Inputs are checked before the VM is touched, so a bad argument leaves it as it was.
    $root = (Resolve-Path -LiteralPath $VmPath).ProviderPath.TrimEnd('\') + '\'
    $script:RefTouched = $true   # from here the VM is this run's to turn off
    if ((Get-VM -Name $Name).State -ne 'Off') { Stop-VM -Name $Name -TurnOff -Force }
    Restore-VMSnapshot -VMName $Name -Name $Checkpoint -Confirm:$false
    $id = (Get-VM -Name $Name).Id
    foreach ($d in @(Get-VMDvdDrive -VMName $Name | Where-Object { $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) })) {
        # Through cmd so a refusal is an exit code, not a PowerShell error record;
        # started from the Windows folder, since cmd refuses a WSL share as its
        # working directory.
        Push-Location -LiteralPath $env:SystemRoot
        try { $out = & cmd.exe /c "icacls.exe `"$($d.Path)`" /grant `"NT VIRTUAL MACHINE\${id}:(R)`" 2>&1" } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "icacls on $($d.Path) failed: $out" }
    }
    if ((Get-VM -Name $Name).State -ne 'Running') { Start-VM -Name $Name }
}

function Start-RefFrom([string]$Name, [string]$VmPath, [string]$Checkpoint) {
    Get-RefCredential $VmPath | Out-Null   # fails before the restore, not after
    Restore-RefCheckpoint $Name $Checkpoint $VmPath
    return New-RefSession $Name $VmPath
}
