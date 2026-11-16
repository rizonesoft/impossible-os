# Prints the capture profile as the interactive desktop sees it, as one JSON line.
# Run by ref-vm.ps1 -Verify through ref's interactive session, never over
# PowerShell Direct, so DPI and resolution are the capture process's own.
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 2.
Add-Type -Namespace Ios -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(System.IntPtr value);
[DllImport("user32.dll")] public static extern uint GetDpiForSystem();
[DllImport("kernel32.dll")] public static extern ushort GetUserDefaultUILanguage();
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(System.IntPtr hwnd, System.Text.StringBuilder name, int max);
[DllImport("user32.dll")] public static extern System.IntPtr MonitorFromPoint(POINT pt, uint flags);
[DllImport("shcore.dll")] public static extern int GetDpiForMonitor(System.IntPtr monitor, int type, out uint dpiX, out uint dpiY);
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
'@
[void][Ios.Native]::SetProcessDpiAwarenessContext([IntPtr]-4)
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$theme = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$adv = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
$dwm = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\DWM'
$accent = 'none'
if ($null -ne $dwm.AccentColor) { $accent = '{0:X8}' -f [uint32]([int64]$dwm.AccentColor -band 0xFFFFFFFF) }
$prevalence = 0
if ($null -ne $theme.ColorPrevalence) { $prevalence = [int]$theme.ColorPrevalence }
$align = 1
if ($null -ne $adv.TaskbarAl) { $align = [int]$adv.TaskbarAl }
# The window with focus, by class: the desktop (Progman or WorkerW) or the
# taskbar when nothing is open; Start, Search or an app window otherwise.
# Scaling is the primary monitor's effective DPI: a change applied in the
# session (set-profile.ps1) moves it at once, while the system DPI keeps the
# value of sign-in until the next one.
$pt = New-Object Ios.Native+POINT
$dpiX = [uint32]0; $dpiY = [uint32]0
$MONITOR_DEFAULTTOPRIMARY = 1; $MDT_EFFECTIVE_DPI = 0
$rc = [Ios.Native]::GetDpiForMonitor([Ios.Native]::MonitorFromPoint($pt, $MONITOR_DEFAULTTOPRIMARY), $MDT_EFFECTIVE_DPI, [ref]$dpiX, [ref]$dpiY)
if ($rc -ne 0) { throw "GetDpiForMonitor failed: $rc" }
# Contrast theme by its Win11 Settings name. Windows stores the pre-Win11 scheme
# name; set-profile.ps1 holds the same table and a test keeps the two equal. An
# unknown stored name is reported as stored, so a comparison names it.
$ContrastNames = [ordered]@{ 'High Contrast #1' = 'Aquatic'; 'High Contrast #2' = 'Desert'; 'High Contrast Black' = 'Dusk'; 'High Contrast White' = 'Night sky' }
$hcKey = Get-ItemProperty 'HKCU:\Control Panel\Accessibility\HighContrast'
$contrast = 'none'
if ([int]$hcKey.Flags -band 1) {
    $contrast = $hcKey.'High Contrast Scheme'
    if ($ContrastNames.Contains($contrast)) { $contrast = $ContrastNames[$contrast] }
}
# Text size: no value is Windows' default, 100.
$acc = 'HKCU:\Software\Microsoft\Accessibility'
$text = 100
if ((Test-Path -LiteralPath $acc) -and $null -ne (Get-Item -LiteralPath $acc).GetValue('TextScaleFactor')) { $text = Get-ItemPropertyValue -LiteralPath $acc -Name TextScaleFactor }
# The second display, when there are two: its effective DPI and its side of the
# main display (Windows puts the main one at (0,0)). Read per monitor, so the
# process is per-monitor aware (above) and the bounds are physical pixels.
Add-Type -AssemblyName System.Windows.Forms
$secondDpi = $null; $secondSide = $null
$others = @([System.Windows.Forms.Screen]::AllScreens | Where-Object { -not $_.Primary })
if ($others.Count -eq 1) {
    $p2 = New-Object Ios.Native+POINT
    $p2.X = $others[0].Bounds.X + [int]($others[0].Bounds.Width / 2); $p2.Y = $others[0].Bounds.Y + [int]($others[0].Bounds.Height / 2)
    $x2 = [uint32]0; $y2 = [uint32]0
    $MONITOR_DEFAULTTONULL = 0
    $rc2 = [Ios.Native]::GetDpiForMonitor([Ios.Native]::MonitorFromPoint($p2, $MONITOR_DEFAULTTONULL), $MDT_EFFECTIVE_DPI, [ref]$x2, [ref]$y2)
    if ($rc2 -ne 0) { throw "GetDpiForMonitor on the second display failed: $rc2" }
    $secondDpi = [int]$x2
    $secondSide = if ($others[0].Bounds.X -lt 0) { 'left' } else { 'right' }
}
$SM_REMOTESESSION = 0x1000
$cls = New-Object System.Text.StringBuilder 256
[void][Ios.Native]::GetClassName([Ios.Native]::GetForegroundWindow(), $cls, 256)
[ordered]@{
    edition = $cv.EditionID
    displayVersion = $cv.DisplayVersion
    build = "$($cv.CurrentBuild).$($cv.UBR)"
    width = [Ios.Native]::GetSystemMetrics(0)
    height = [Ios.Native]::GetSystemMetrics(1)
    monitors = [Ios.Native]::GetSystemMetrics(80)
    dpi = $dpiX
    systemDpi = [Ios.Native]::GetDpiForSystem()
    # The display language Windows shows ref, from Windows itself. Get-UICulture
    # is the console host's own culture, which falls back to en-US for a language
    # the console cannot render, such as Arabic (seen 2026-11-16).
    uiLanguage = [Globalization.CultureInfo]::GetCultureInfo([int][Ios.Native]::GetUserDefaultUILanguage()).Name
    format = (Get-Culture).Name
    appsLight = [int]$theme.AppsUseLightTheme
    systemLight = [int]$theme.SystemUsesLightTheme
    accent = $accent
    accentOnSurfaces = $prevalence
    taskbarCentred = ($align -eq 1)
    highContrast = $contrast
    textScale = [int]$text
    secondDpi = $secondDpi
    secondSide = $secondSide
    remote = [bool][Ios.Native]::GetSystemMetrics($SM_REMOTESESSION)
    session = (Get-Process -Id $PID).SessionId
    foreground = $cls.ToString()
} | ConvertTo-Json -Compress
