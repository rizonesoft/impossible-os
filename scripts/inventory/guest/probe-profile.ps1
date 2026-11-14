# Prints the capture profile as the interactive desktop sees it, as one JSON line.
# Run by ref-vm.ps1 -Verify through ref's interactive session, never over
# PowerShell Direct, so DPI and resolution are the capture process's own.
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 2.
Add-Type -Namespace Ios -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(System.IntPtr value);
[DllImport("user32.dll")] public static extern uint GetDpiForSystem();
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(System.IntPtr hwnd, System.Text.StringBuilder name, int max);
'@
[void][Ios.Native]::SetProcessDpiAwarenessContext([IntPtr]-4)
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$theme = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$adv = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
$align = 1
if ($null -ne $adv.TaskbarAl) { $align = [int]$adv.TaskbarAl }
# The window with focus, by class: the desktop (Progman or WorkerW) or the
# taskbar when nothing is open; Start, Search or an app window otherwise.
$cls = New-Object System.Text.StringBuilder 256
[void][Ios.Native]::GetClassName([Ios.Native]::GetForegroundWindow(), $cls, 256)
[ordered]@{
    edition = $cv.EditionID
    displayVersion = $cv.DisplayVersion
    build = "$($cv.CurrentBuild).$($cv.UBR)"
    width = [Ios.Native]::GetSystemMetrics(0)
    height = [Ios.Native]::GetSystemMetrics(1)
    dpi = [Ios.Native]::GetDpiForSystem()
    uiLanguage = (Get-UICulture).Name
    format = (Get-Culture).Name
    appsLight = [int]$theme.AppsUseLightTheme
    systemLight = [int]$theme.SystemUsesLightTheme
    taskbarCentred = ($align -eq 1)
    session = (Get-Process -Id $PID).SessionId
    foreground = $cls.ToString()
} | ConvertTo-Json -Compress
