# Capture fixture: the made-up folder tree File Explorer is captured over, so
# every view, menu and dialog has items of each kind to show while every
# payload stays fictional:
#   C:\Users\ref\Documents\Explorer sample   a document, an image, an archive, a
#       program, a shortcut, a file whose name wraps, the five folder kinds
#       (general, documents, pictures, music, videos, typed by desktop.ini) and
#       an empty folder
#   Sample library                              a library over the documents folder
#   Recycle Bin                                 one deleted sample file
#   the address bar's history                   three typed sample paths (TypedPaths)
# The fixture removes everything it owns before it adds it again, so a second
# run leaves the same state. -State prints what it owns as one JSON line and
# changes nothing.
# Runs elevated in ref's interactive session (capture.py --prepare).
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 12.
# Variants: sample
param(
    [ValidateSet('sample')][string]$Variant = 'sample',
    [switch]$State
)
$ErrorActionPreference = 'Stop'

$Root = Join-Path $env:USERPROFILE 'Documents\Explorer sample'
$Libraries = Join-Path $env:APPDATA 'Microsoft\Windows\Libraries'
$Library = Join-Path $Libraries 'Sample library.library-ms'
$Deleted = 'Sample deleted file.txt'
$Typed = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\TypedPaths'
# A name long enough to wrap onto two lines under an icon in every icon view.
$LongName = 'Sample file with a long name that wraps onto two lines under its icon.txt'
# The folder kinds a details view gives different default columns (desktop.ini FolderType).
$Kinds = [ordered]@{ 'General items' = 'Generic'; 'Documents items' = 'Documents'; 'Pictures items' = 'Pictures';
                     'Music items' = 'Music'; 'Videos items' = 'Videos' }

function Get-Bin() {
    # The Recycle Bin's items, by their original name (shell namespace 10).
    $bin = (New-Object -ComObject Shell.Application).Namespace(10)
    return @($bin.Items() | ForEach-Object { $_.Name })
}

function Get-Typed() {
    # The address bar's history: url1 is the newest typed path.
    if (-not (Test-Path -LiteralPath $Typed)) { return @() }
    $k = Get-Item -LiteralPath $Typed
    return @($k.GetValueNames() | Where-Object { $_ -like 'url*' } | Sort-Object { [int]$_.Substring(3) } | ForEach-Object { $k.GetValue($_) })
}

function Get-Owned() {
    $files = @()
    if (Test-Path -LiteralPath $Root) {
        $files = @(Get-ChildItem -LiteralPath $Root -Recurse -Force | Sort-Object FullName |
                   ForEach-Object { $_.FullName.Substring($Root.Length + 1) })
    }
    [ordered]@{
        root = (Test-Path -LiteralPath $Root -PathType Container)
        files = $files
        library = (Test-Path -LiteralPath $Library)
        bin = @(Get-Bin | Where-Object { $_ -like 'Sample*' })
        typed = @(Get-Typed)
    }
}

if ($State) { (Get-Owned) | ConvertTo-Json -Compress; exit 0 }

# Remove what an earlier run left: the tree, the library and the deleted file.
if (Test-Path -LiteralPath $Root) {
    # desktop.ini folders are read-only and system; clear that so they can go.
    Get-ChildItem -LiteralPath $Root -Recurse -Force -Directory | ForEach-Object { $_.Attributes = 'Directory' }
    Remove-Item -LiteralPath $Root -Recurse -Force
}
if (Test-Path -LiteralPath $Library) { Remove-Item -LiteralPath $Library -Force }
$bin = (New-Object -ComObject Shell.Application).Namespace(10)
foreach ($item in @($bin.Items() | Where-Object { $_.Name -like 'Sample*' })) { Remove-Item -LiteralPath $item.Path -Recurse -Force }
if (Test-Path -LiteralPath $Typed) {
    foreach ($v in @((Get-Item -LiteralPath $Typed).GetValueNames() | Where-Object { $_ -like 'url*' })) { Remove-ItemProperty -LiteralPath $Typed -Name $v }
}
$left = Get-Owned
if ($left.root -or $left.library -or @($left.bin).Count -or @($left.typed).Count) { throw "entries left after removal: $($left | ConvertTo-Json -Compress)" }

New-Item -ItemType Directory -Path $Root -Force | Out-Null
Set-Content -LiteralPath (Join-Path $Root 'Sample document.txt') -Value 'Sample content' -Encoding ASCII
Set-Content -LiteralPath (Join-Path $Root $LongName) -Value 'Sample content' -Encoding ASCII

# An image of our own: a 64 px square in two colours.
Add-Type -AssemblyName System.Drawing
function New-Image([string]$path, [int]$hue) {
    $bmp = New-Object System.Drawing.Bitmap 64, 64
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::FromArgb(255, 40, 90 + $hue, 160))
    $g.FillEllipse([System.Drawing.Brushes]::White, 16, 16, 32, 32)
    $g.Dispose(); $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}
New-Image (Join-Path $Root 'Sample image.png') 0

$zipSource = Join-Path $env:TEMP 'Sample archive content.txt'
Set-Content -LiteralPath $zipSource -Value 'Sample content' -Encoding ASCII
Compress-Archive -LiteralPath $zipSource -DestinationPath (Join-Path $Root 'Sample archive.zip') -Force
Remove-Item -LiteralPath $zipSource -Force

# A program: Windows' whoami.exe under a sample name, so the program
# verbs (run as administrator, pin) show.
Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\whoami.exe') -Destination (Join-Path $Root 'Sample program.exe')

$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut((Join-Path $Root 'Sample shortcut.lnk'))
$lnk.TargetPath = Join-Path $Root 'Sample document.txt'
$lnk.Save()

foreach ($name in $Kinds.Keys) {
    $dir = Join-Path $Root $name
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    switch ($Kinds[$name]) {
        'Pictures' { 1..3 | ForEach-Object { New-Image (Join-Path $dir "Sample picture $_.png") (30 * $_) } }
        'Music' { 1..3 | ForEach-Object { Set-Content -LiteralPath (Join-Path $dir "Sample track $_.wav") -Value '' } }
        'Videos' { 1..3 | ForEach-Object { Set-Content -LiteralPath (Join-Path $dir "Sample clip $_.mp4") -Value '' } }
        default { 1..3 | ForEach-Object { Set-Content -LiteralPath (Join-Path $dir "Sample item $_.txt") -Value 'Sample content' -Encoding ASCII } }
    }
    $ini = Join-Path $dir 'desktop.ini'
    Set-Content -LiteralPath $ini -Value "[ViewState]`r`nMode=`r`nVid=`r`nFolderType=$($Kinds[$name])" -Encoding Unicode
    (Get-Item -LiteralPath $ini -Force).Attributes = 'Hidden, System'
    # Explorer reads desktop.ini only in a read-only or system folder.
    (Get-Item -LiteralPath $dir).Attributes = 'Directory, ReadOnly'
}
New-Item -ItemType Directory -Path (Join-Path $Root 'Empty folder') -Force | Out-Null

# A library over the documents folder, written the way Explorer writes one.
$docs = Join-Path $Root 'Documents items'
@"
<?xml version="1.0" encoding="UTF-8"?>
<libraryDescription xmlns="http://schemas.microsoft.com/windows/2009/library">
  <isLibraryPinned>true</isLibraryPinned>
  <iconReference>imageres.dll,-1002</iconReference>
  <searchConnectorDescriptionList>
    <searchConnectorDescription>
      <isDefaultSaveLocation>true</isDefaultSaveLocation>
      <isSupported>false</isSupported>
      <simpleLocation><url>$docs</url></simpleLocation>
    </searchConnectorDescription>
  </searchConnectorDescriptionList>
</libraryDescription>
"@ | Set-Content -LiteralPath $Library -Encoding UTF8

# One file in the Recycle Bin, sent there the way Delete sends it.
$victim = Join-Path $Root $Deleted
Set-Content -LiteralPath $victim -Value 'Sample content' -Encoding ASCII
Add-Type -AssemblyName Microsoft.VisualBasic
[Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($victim, 'OnlyErrorDialogs', 'SendToRecycleBin')

# The address bar's history, newest first: paths a person typed into it.
New-Item -Path $Typed -Force | Out-Null
$n = 1
foreach ($t in @((Join-Path $Root 'Pictures items'), (Join-Path $Root 'Documents items'), $Root)) {
    New-ItemProperty -LiteralPath $Typed -Name "url$n" -Value $t -PropertyType String -Force | Out-Null
    $n++
}

$owned = Get-Owned
if (-not ($owned.root -and $owned.library -and @($owned.bin).Count -eq 1 -and @($owned.typed).Count -eq 3)) { throw "fixture incomplete: $($owned | ConvertTo-Json -Compress)" }
Write-Output "fixture explorer: $(@($owned.files).Count) entries, library, $(@($owned.bin).Count) in the Recycle Bin, $(@($owned.typed).Count) typed paths"
exit 0
