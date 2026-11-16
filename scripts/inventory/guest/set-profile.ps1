# Applies the live settings of one capture profile in ref's interactive session:
# display scaling, the contrast theme, light or dark, and the text size, each
# through the path Windows itself uses (measured 2026-11-16 on build 26300.9711):
#   scaling   SPI_SETLOGICALDPIOVERRIDE, a step relative to the recommended
#             scale; Hyper-V's display refuses the per-source scale packet
#             (DisplayConfigSetDeviceInfo, error 31), and a step past the
#             display's maximum is silently clamped, so the result is read back.
#   contrast  the contrast theme file, as Settings applies it; turning it off
#             re-applies the theme saved when it was turned on.
#   light     the Personalize values and the ImmersiveColorSet broadcast.
#   text size the Settings text size slider and its Apply button: the registry
#             value alone changes no font (an Explorer restart neither).
# Settings that need a changed machine (resolution, display count, display
# language) are not touched: probe-profile.ps1 reports them and the caller
# refuses a mismatch. Changes only what differs, so applying the current
# profile is a no-op. Run through ref's interactive session (Invoke-RefInteractive
# or ref-run.ps1), never over PowerShell Direct.
# Exits 0 when every live value was applied, 2 when the profile asks for a value
# this display does not offer, 1 on any other failure.
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 19.
param(
    [Parameter(Mandatory = $true)][string]$Profiles,
    [Parameter(Mandatory = $true)][string]$Name
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -Namespace Ios -Name Live -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(System.IntPtr value);
[DllImport("user32.dll")] public static extern System.IntPtr MonitorFromPoint(POINT pt, uint flags);
[DllImport("shcore.dll")] public static extern int GetDpiForMonitor(System.IntPtr monitor, int type, out uint dpiX, out uint dpiY);
[DllImport("user32.dll", SetLastError = true)] public static extern bool SystemParametersInfo(uint action, int uiParam, System.IntPtr value, uint winIni);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern System.IntPtr SendMessageTimeout(System.IntPtr hwnd, uint msg, System.UIntPtr wParam, string lParam, uint flags, uint timeout, out System.UIntPtr result);
'@
[void][Ios.Live]::SetProcessDpiAwarenessContext([IntPtr]-4)

# The scale steps Windows offers, in percent, from 100 up.
$Steps = 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500
# Win11's four contrast themes by their Settings names: the theme file, and the
# pre-Win11 scheme name Windows still stores for it (measured on build 26300.9711).
# probe-profile.ps1 holds the same table; a test keeps the two equal.
$ContrastThemes = [ordered]@{
    'Aquatic' = @('hc1.theme', 'High Contrast #1')
    'Desert' = @('hc2.theme', 'High Contrast #2')
    'Dusk' = @('hcblack.theme', 'High Contrast Black')
    'Night sky' = @('hcwhite.theme', 'High Contrast White')
}
$Personalize = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'

function Get-Dpi() {
    $x = [uint32]0; $y = [uint32]0
    $rc = [Ios.Live]::GetDpiForMonitor([Ios.Live]::MonitorFromPoint((New-Object Ios.Live+POINT), 1), 0, [ref]$x, [ref]$y)
    if ($rc -ne 0) { throw "GetDpiForMonitor failed: $rc" }
    return [int]$x
}

function Set-DpiStep([int]$Rel) {
    $SPI_SETLOGICALDPIOVERRIDE = 0x9F; $SPIF_UPDATEINIFILE = 1
    if (-not [Ios.Live]::SystemParametersInfo($SPI_SETLOGICALDPIOVERRIDE, $Rel, [IntPtr]::Zero, $SPIF_UPDATEINIFILE)) {
        throw "SPI_SETLOGICALDPIOVERRIDE $Rel failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    Start-Sleep -Seconds 3
}

function Set-Scaling([int]$Percent) {
    $target = [int]($Percent * 96 / 100)
    if ((Get-Dpi) -eq $target) { return $false }
    $want = [array]::IndexOf($Steps, $Percent)
    if ($want -lt 0) { throw "scaling $Percent is not a Windows step" }
    # Steps count from the recommended scale, which nothing reports: step 0 shows it.
    Set-DpiStep 0
    $rec = [array]::IndexOf($Steps, [int][math]::Round((Get-Dpi) * 100 / 96))
    if ($rec -lt 0) { throw "the recommended scale is $(Get-Dpi) DPI, not a Windows step" }
    if ($want -ne $rec) { Set-DpiStep ($want - $rec) }
    $got = Get-Dpi
    if ($got -ne $target) {
        $reached = [math]::Round($got * 100 / 96)
        Set-DpiStep 0
        Write-Output "set-profile: scaling $Percent is not offered on this display (it reached $reached)"
        exit 2
    }
    return $true
}

function Send-SettingChange([string]$Area) {
    $r = [UIntPtr]::Zero
    [void][Ios.Live]::SendMessageTimeout([IntPtr]0xFFFF, 0x1A, [UIntPtr]::Zero, $Area, 2, 5000, [ref]$r)
}

function Get-Contrast() {
    $h = Get-ItemProperty 'HKCU:\Control Panel\Accessibility\HighContrast'
    if (([int]$h.Flags -band 1) -eq 0) { return 'none' }
    $stored = $h.'High Contrast Scheme'
    foreach ($k in $ContrastThemes.Keys) { if ($ContrastThemes[$k][1] -eq $stored) { return $k } }
    return $stored
}

# Applying a theme file opens Settings on its page; it is closed again once the
# theme holds, so the capture starts from the bare desktop.
function Use-ThemeFile([string]$Path, [scriptblock]$Done) {
    Start-Process -FilePath $Path
    $deadline = (Get-Date).AddSeconds(60)
    while (-not (& $Done)) {
        if ((Get-Date) -gt $deadline) { throw "the theme $Path did not take effect within 60 s" }
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 3
    Get-Process | Where-Object { $_.Name -eq 'SystemSettings' } | Stop-Process -Force
}

function Set-Contrast([string]$Scheme) {
    $cur = Get-Contrast
    if ($cur -eq $Scheme) { return $false }
    if ($Scheme -eq 'none') {
        # Back to the theme the account had before contrast was turned on.
        $saved = 'C:\ios\state\theme-before-contrast.theme'
        if (-not (Test-Path -LiteralPath $saved)) { throw 'contrast is on, but the theme before it was not saved' }
        Use-ThemeFile $saved { (Get-Contrast) -eq 'none' }
        Remove-Item -LiteralPath $saved
        return $true
    }
    if (-not $ContrastThemes.Contains($Scheme)) { throw "no contrast theme named '$Scheme' (Win11 has $($ContrastThemes.Keys -join ', '))" }
    if ($cur -eq 'none') {
        $current = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes').CurrentTheme
        New-Item -ItemType Directory -Path 'C:\ios\state' -Force | Out-Null
        Copy-Item -LiteralPath $current -Destination 'C:\ios\state\theme-before-contrast.theme' -Force
    }
    Use-ThemeFile (Join-Path $env:WINDIR "Resources\Ease of Access Themes\$($ContrastThemes[$Scheme][0])") { (Get-Contrast) -eq $Scheme }
    return $true
}

# The Settings text size; no value is Windows' default, 100.
function Get-TextScale() {
    $key = 'HKCU:\Software\Microsoft\Accessibility'
    if ((Test-Path -LiteralPath $key) -and $null -ne (Get-Item -LiteralPath $key).GetValue('TextScaleFactor')) {
        return [int](Get-ItemPropertyValue -LiteralPath $key -Name TextScaleFactor)
    }
    return 100
}

function Set-TextScale([int]$Percent) {
    if ((Get-TextScale) -eq $Percent) { return $false }
    Start-Process 'ms-settings:easeofaccess-display'
    $A = [Windows.Automation.AutomationElement]
    $cond = { param($p, $v) New-Object Windows.Automation.PropertyCondition($p, $v) }
    $deadline = (Get-Date).AddSeconds(60)
    $slider = $null
    while (-not $slider) {
        if ((Get-Date) -gt $deadline) { throw 'the Settings text size slider did not appear within 60 s' }
        Start-Sleep -Seconds 2
        $slider = $A::RootElement.FindFirst('Descendants', (& $cond $A::AutomationIdProperty 'SystemSettings_EaseOfAccess_Experience_TextScalingDesktop_Slider'))
    }
    $range = $slider.GetCurrentPattern([Windows.Automation.RangeValuePattern]::Pattern)
    if ($Percent -lt $range.Current.Minimum -or $Percent -gt $range.Current.Maximum) {
        Write-Output "set-profile: text size $Percent is not offered (Settings offers $($range.Current.Minimum) to $($range.Current.Maximum))"
        exit 2
    }
    $range.SetValue($Percent)
    $apply = $A::RootElement.FindFirst('Descendants', (& $cond $A::AutomationIdProperty 'SystemSettings_EaseOfAccess_Experience_TextScalingDesktop_ButtonRemove'))
    if (-not $apply) { throw 'the Settings text size Apply button is missing' }
    $apply.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
    # Windows shows "Please wait" while it rescales; the value lands when it is done.
    $deadline = (Get-Date).AddSeconds(90)
    do {
        if ((Get-Date) -gt $deadline) { throw "text size $Percent was not applied within 90 s" }
        Start-Sleep -Seconds 3
    } while ((Get-TextScale) -ne $Percent)
    Start-Sleep -Seconds 5
    Get-Process | Where-Object { $_.Name -eq 'SystemSettings' } | Stop-Process -Force
    return $true
}

$want = (Get-Content -Raw -LiteralPath $Profiles | ConvertFrom-Json).$Name
if (-not $want) { Write-Output "set-profile: no profile $Name in $Profiles"; exit 1 }

$changed = @()
if ($null -ne $want.dpi) {
    $pct = [int][math]::Round($want.dpi * 100 / 96)
    if (Set-Scaling $pct) { $changed += "scaling $pct" }
}
# Contrast before light and dark: a contrast theme sets the light values itself.
$scheme = 'none'
if ($null -ne $want.highContrast) { $scheme = $want.highContrast }
if (Set-Contrast $scheme) { $changed += "contrast $scheme" }
$theme = Get-ItemProperty $Personalize
$light = $false
foreach ($k in @(@('appsLight', 'AppsUseLightTheme'), @('systemLight', 'SystemUsesLightTheme'))) {
    if ($null -eq $want.($k[0])) { continue }
    if ([int]$theme.($k[1]) -ne [int]$want.($k[0])) {
        Set-ItemProperty $Personalize -Name $k[1] -Value ([int]$want.($k[0])) -Type DWord
        $light = $true
        $changed += "$($k[0]) $($want.($k[0]))"
    }
}
if ($light) { Send-SettingChange 'ImmersiveColorSet'; Start-Sleep -Seconds 3 }
$text = 100
if ($null -ne $want.textScale) { $text = [int]$want.textScale }
if (Set-TextScale $text) { $changed += "text size $text" }

if ($changed.Count) { Write-Output "set-profile: $Name applied ($($changed -join ', '))" } else { Write-Output "set-profile: $Name already applied" }
exit 0
