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
#   second display (two-display profiles, on the stand-in machine's two virtio
#             heads): secondSide is where the other display sits beside the main
#             one; "left" is Windows' "Make this my main display" on the right-hand
#             one, which moves the old main display to negative X (SetDisplayConfig
#             with the source positions changed). secondDpi is that display's
#             scale, set through the per-source scale packet the display driver
#             accepts there (DisplayConfigSetDeviceInfo, type -4), read back.
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
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Ios {
public static class Disp {
    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint Low; public int High; }
    [StructLayout(LayoutKind.Explicit, Size = 72)] public struct PATH {
        [FieldOffset(0)] public LUID SrcAdapter; [FieldOffset(8)] public uint SrcId; [FieldOffset(12)] public uint SrcMode;
        [FieldOffset(20)] public LUID TgtAdapter; [FieldOffset(28)] public uint TgtId; [FieldOffset(32)] public uint TgtMode;
        [FieldOffset(60)] public int TgtAvailable; [FieldOffset(68)] public uint Flags; }
    [StructLayout(LayoutKind.Explicit, Size = 64)] public struct MODE {
        [FieldOffset(0)] public uint InfoType; [FieldOffset(4)] public uint Id; [FieldOffset(8)] public LUID Adapter;
        [FieldOffset(16)] public uint Width; [FieldOffset(20)] public uint Height; [FieldOffset(28)] public int X; [FieldOffset(32)] public int Y; }
    [StructLayout(LayoutKind.Sequential)] public struct GETSCALE { public int Type; public uint Size; public LUID Adapter; public uint Id; public int Min; public int Cur; public int Max; }
    [StructLayout(LayoutKind.Sequential)] public struct SETSCALE { public int Type; public uint Size; public LUID Adapter; public uint Id; public int Rel; }
    [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
    [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint flags, ref uint np, [Out] PATH[] p, ref uint nm, [Out] MODE[] m, IntPtr topology);
    [DllImport("user32.dll")] static extern int SetDisplayConfig(uint np, [In] PATH[] p, uint nm, [In] MODE[] m, uint flags);
    [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref GETSCALE r);
    [DllImport("user32.dll")] static extern int DisplayConfigSetDeviceInfo(ref SETSCALE r);
    public static PATH[] Paths; public static MODE[] Modes;
    public static void Query() {
        uint np, nm;
        int rc = GetDisplayConfigBufferSizes(2, out np, out nm);   // QDC_ONLY_ACTIVE_PATHS
        if (rc != 0) throw new Exception("GetDisplayConfigBufferSizes " + rc);
        Paths = new PATH[np]; Modes = new MODE[nm];
        rc = QueryDisplayConfig(2, ref np, Paths, ref nm, Modes, IntPtr.Zero);
        if (rc != 0) throw new Exception("QueryDisplayConfig " + rc);
        Array.Resize(ref Paths, (int)np); Array.Resize(ref Modes, (int)nm);
    }
    public static int SourceX(int i) { return Modes[Paths[i].SrcMode].X; }
    public static int SourceY(int i) { return Modes[Paths[i].SrcMode].Y; }
    public static int SourceWidth(int i) { return (int)Modes[Paths[i].SrcMode].Width; }
    // Source i becomes the main display: it moves to (0,0) and every other source
    // keeps its place relative to it.
    public static void MakeMain(int i) {
        int dx = SourceX(i), dy = SourceY(i);
        for (int k = 0; k < Paths.Length; k++) { Modes[Paths[k].SrcMode].X -= dx; Modes[Paths[k].SrcMode].Y -= dy; }
        // SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES
        int rc = SetDisplayConfig((uint)Paths.Length, Paths, (uint)Modes.Length, Modes, 0x80 | 0x20 | 0x200 | 0x400);
        if (rc != 0) throw new Exception("SetDisplayConfig " + rc);
    }
    // Every connected display joins the desktop, as Win+P "Extend" does: one active
    // path per available target, each from its own source, with the modes and the
    // layout left to Windows, which keeps the result for this set of displays.
    // SDC_TOPOLOGY_EXTEND fails with ERROR_GEN_FAILURE (31) when the set is an
    // integrated panel plus another display with no saved layout (seen 2026-11-16),
    // so the paths are supplied.
    public static int Extend() {
        uint np, nm;
        int rc = GetDisplayConfigBufferSizes(1, out np, out nm);   // QDC_ALL_PATHS
        if (rc != 0) return rc;
        PATH[] all = new PATH[np]; MODE[] modes = new MODE[nm];
        rc = QueryDisplayConfig(1, ref np, all, ref nm, modes, IntPtr.Zero);
        if (rc != 0) return rc;
        var pick = new System.Collections.Generic.List<PATH>();
        var targets = new System.Collections.Generic.HashSet<string>();
        var sources = new System.Collections.Generic.HashSet<string>();
        for (int i = 0; i < np; i++) {
            PATH p = all[i];
            if (p.TgtAvailable == 0) continue;
            string t = p.TgtAdapter.High + ":" + p.TgtAdapter.Low + ":" + p.TgtId;
            string s = p.SrcAdapter.High + ":" + p.SrcAdapter.Low + ":" + p.SrcId;
            if (targets.Contains(t) || sources.Contains(s)) continue;
            targets.Add(t); sources.Add(s);
            p.Flags |= 1;                                    // DISPLAYCONFIG_PATH_ACTIVE
            p.SrcMode = 0xFFFFFFFF; p.TgtMode = 0xFFFFFFFF;  // DISPLAYCONFIG_PATH_MODE_IDX_INVALID
            pick.Add(p);
        }
        // SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES
        return SetDisplayConfig((uint)pick.Count, pick.ToArray(), 0, null, 0x80 | 0x20 | 0x200 | 0x400);
    }
    public static int[] Scale(int i) {
        GETSCALE g = new GETSCALE(); g.Type = -3; g.Size = (uint)Marshal.SizeOf(typeof(GETSCALE)); g.Adapter = Paths[i].SrcAdapter; g.Id = Paths[i].SrcId;
        int rc = DisplayConfigGetDeviceInfo(ref g);
        if (rc != 0) throw new Exception("DisplayConfigGetDeviceInfo scale " + rc);
        return new int[] { g.Min, g.Cur, g.Max };
    }
    public static int SetScale(int i, int rel) {
        SETSCALE r = new SETSCALE(); r.Type = -4; r.Size = (uint)Marshal.SizeOf(typeof(SETSCALE)); r.Adapter = Paths[i].SrcAdapter; r.Id = Paths[i].SrcId; r.Rel = rel;
        return DisplayConfigSetDeviceInfo(ref r);
    }
}
}
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

# The active sources: the main one is the source at (0,0).
function Get-Sources() {
    [Ios.Disp]::Query()
    $all = @(0..([Ios.Disp]::Paths.Length - 1))
    $main = @($all | Where-Object { [Ios.Disp]::SourceX($_) -eq 0 -and [Ios.Disp]::SourceY($_) -eq 0 })
    if ($main.Count -ne 1) { throw "$($main.Count) displays sit at (0,0); want one" }
    return [pscustomobject]@{ Main = $main[0]; Others = @($all | Where-Object { $_ -ne $main[0] }) }
}

# A two-display profile has both displays on the desktop. On a first boot with a
# set of displays it has no saved layout for, Windows can show the desktop on one
# of them only (seen 2026-11-16 on the stand-in, whose first head reports as an
# integrated panel); the profile then extends the desktop, as a user would.
function Set-Extended([int]$Count) {
    if (@((Get-Sources).Others).Count + 1 -ge $Count) { return $false }
    # A display that is still arriving after a cold sign-in is not yet available,
    # so the layout is supplied again until the desktop spans the displays.
    $deadline = (Get-Date).AddSeconds(60)
    $rc = -1
    do {
        $rc = [Ios.Disp]::Extend()
        Start-Sleep -Seconds 3
        $n = @((Get-Sources).Others).Count + 1
    } while ($n -lt $Count -and (Get-Date) -lt $deadline)
    if ($n -lt $Count) { Write-Output "set-profile: $Count displays wanted; the desktop has $n after extending (last SetDisplayConfig $rc)"; exit 2 }
    return $true
}

function Set-SecondSide([string]$Side) {
    $src = Get-Sources
    if ($src.Others.Count -ne 1) { Write-Output "set-profile: secondSide needs two displays; this machine has $($src.Others.Count + 1)"; exit 2 }
    $o = $src.Others[0]
    $now = if ([Ios.Disp]::SourceX($o) -lt 0) { 'left' } else { 'right' }
    if ($now -eq $Side) { return $false }
    # The other display becomes main; the old main then sits on the requested side.
    [Ios.Disp]::MakeMain($o)
    Start-Sleep -Seconds 5
    $src = Get-Sources
    $got = if ([Ios.Disp]::SourceX($src.Others[0]) -lt 0) { 'left' } else { 'right' }
    if ($got -ne $Side) { throw "the second display is on the $got after the main display moved, want $Side" }
    return $true
}

# One display's scale through the per-display call, as Settings sets each display
# of a two-display machine. $Which is 'main' or 'second'.
function Get-Source([string]$Which) {
    $src = Get-Sources
    if ($src.Others.Count -ne 1) { Write-Output "set-profile: a $Which-display scale needs two displays; this machine has $($src.Others.Count + 1)"; exit 2 }
    if ($Which -eq 'main') { return $src.Main } else { return $src.Others[0] }
}

function Set-DisplayScaling([string]$Which, [int]$Percent) {
    $o = Get-Source $Which
    $want = [array]::IndexOf($Steps, $Percent)
    if ($want -lt 0) { throw "scaling $Percent is not a Windows step" }
    $sc = [Ios.Disp]::Scale($o)   # min, current, max: steps from the recommended scale, which is step -min
    $rel = $want - [math]::Abs($sc[0])
    if ($rel -eq $sc[1]) { return $false }
    if ($rel -lt $sc[0] -or $rel -gt $sc[2]) {
        Write-Output "set-profile: $Which-display scaling $Percent is not offered (steps $($sc[0]) to $($sc[2]) from the recommended $($Steps[[math]::Abs($sc[0])]))"
        exit 2
    }
    $rc = [Ios.Disp]::SetScale($o, $rel)
    if ($rc -ne 0) { throw "DisplayConfigSetDeviceInfo scale $rel on the $Which display failed: $rc" }
    Start-Sleep -Seconds 3
    $now = [Ios.Disp]::Scale((Get-Source $Which))[1]
    if ($now -ne $rel) { throw "the $Which display's scale step is $now after setting $rel" }
    return $true
}

# The step a display is at now, against the step $Percent asks for.
function Test-DisplayScaling([string]$Which, [int]$Percent) {
    $o = Get-Source $Which
    $sc = [Ios.Disp]::Scale($o)
    $rel = [array]::IndexOf($Steps, $Percent) - [math]::Abs($sc[0])
    if ($sc[1] -ne $rel) { throw "the $Which display's scale step is $($sc[1]) after both were set, want $rel" }
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
if ($want.monitors -gt 1 -and (Set-Extended $want.monitors)) { $changed += "desktop extended to $($want.monitors) displays" }
# The main display first: scaling then applies to the display that ends up main.
if ($null -ne $want.secondSide -and (Set-SecondSide $want.secondSide)) { $changed += "second display $($want.secondSide)" }
if ($null -ne $want.secondDpi) {
    # Two displays: each display's own scale, the main one first. The logical DPI
    # override Set-Scaling uses moved both displays (seen 2026-11-16).
    $pct = [int][math]::Round($want.dpi * 100 / 96)
    $pct2 = [int][math]::Round($want.secondDpi * 100 / 96)
    if (Set-DisplayScaling 'main' $pct) { $changed += "main-display scaling $pct" }
    if (Set-DisplayScaling 'second' $pct2) { $changed += "second-display scaling $pct2" }
    Test-DisplayScaling 'main' $pct
} elseif ($null -ne $want.dpi) {
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
