<#
Runs inside the Windows 11 reference VM, from make-ms-hashes.ps1 through ref-run.ps1.
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 4.

Hashes every file of the types in C:\ios\in\asset-types.txt under the roots below
and writes C:\ios\out\ms-hashes.txt: sorted, unique "<sha256> <size>" lines and
no bytes. The machine is clean and offline, so everything under these roots is
Microsoft's: Windows itself, the inbox app packages and the shared data folder.
Directory reparse points are not followed, so a junction is never walked twice
or in a loop; a file that cannot be read and a folder that cannot be listed are
counted and named on stdout, so the run shows what the list does not cover.
#>
$ErrorActionPreference = 'Stop'
$types = @{}
foreach ($l in Get-Content -LiteralPath 'C:\ios\in\asset-types.txt') {
    $t = $l.Trim()
    if ($t -and -not $t.StartsWith('#')) { $types['.' + $t.ToLowerInvariant()] = $true }
}
$roots = New-Object System.Collections.Generic.List[string]
foreach ($r in 'C:\Windows', 'C:\Program Files', 'C:\Program Files (x86)', 'C:\ProgramData') { $roots.Add($r) }
# WindowsApps refuses a listing even to an administrator; each package folder is readable.
foreach ($p in Get-AppxPackage -AllUsers) { if ($p.InstallLocation) { $roots.Add($p.InstallLocation) } }

$seen = New-Object 'System.Collections.Generic.HashSet[string]'
$done = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$sha = [System.Security.Cryptography.SHA256]::Create()
$files = 0; $unread = 0; $unlisted = 0
$stack = New-Object System.Collections.Generic.Stack[string]
foreach ($r in $roots) { if (Test-Path -LiteralPath $r) { $stack.Push($r) } }
while ($stack.Count) {
    $dir = $stack.Pop()
    if (-not $done.Add($dir)) { continue }
    try { $entries = [System.IO.DirectoryInfo]::new($dir).GetFileSystemInfos() }
    catch { $unlisted++; Write-Output "unlisted: $dir"; continue }
    foreach ($e in $entries) {
        if ($e -is [System.IO.DirectoryInfo]) {
            if (-not ($e.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) { $stack.Push($e.FullName) }
            continue
        }
        if (-not $types.ContainsKey($e.Extension.ToLowerInvariant())) { continue }
        try {
            $fs = [System.IO.File]::Open($e.FullName, 'Open', 'Read', 'ReadWrite, Delete')
            try { $h = $sha.ComputeHash($fs); $len = $fs.Length } finally { $fs.Dispose() }
        } catch { $unread++; Write-Output "unread: $($e.FullName)"; continue }
        $files++
        [void]$seen.Add(([BitConverter]::ToString($h) -replace '-', '').ToLowerInvariant() + ' ' + $len)
    }
}
$v = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# Windows 11 reference VM asset hashes: SHA-256 and size only, no bytes.')
$lines.Add("# build $($v.CurrentBuild).$($v.UBR); made by scripts/inventory/make-ms-hashes.ps1; $files files, $($seen.Count) distinct")
$sorted = [string[]]@($seen)
[Array]::Sort($sorted, [StringComparer]::Ordinal)
foreach ($s in $sorted) { $lines.Add($s) }
[System.IO.File]::WriteAllText('C:\ios\out\ms-hashes.txt', ($lines -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Output "hash-assets: $files files, $($seen.Count) distinct, $unread unread, $unlisted folders unlisted"
