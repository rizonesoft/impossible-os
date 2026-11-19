# Capture Settings pages on the operator's own Windows machine: the pages that
# need hardware the reference VM does not have (Bluetooth, a camera, a discrete
# GPU, a Wi-Fi adapter).
#
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 27.
# Driven by scripts/inventory/operator-capture.py, which builds the records,
# redacts what names the operator, and publishes.
#
#   powershell -File operator-host.ps1 -Out <folder> -Pages bluetooth,camera,...
#   powershell -File operator-host.ps1 -Out <folder> -Restore
#
# <folder> is a run folder directly under the capture root ($Captures). -Restore
# puts back the values an interrupted capture saved (the pending-restore file in
# that root): the caller runs it when a capture was killed before its own finally
# block ran, and every capture runs it first. One host runs at a time per user
# (a named mutex), however it was started.
#
# For the capture only, the session takes the capture profile's theme and
# accent (light, the default blue); the operator's own values are saved first
# and put back in a finally block, also on an error or a Ctrl-C. Each page
# opens maximized on the monitor that has the profile's size and scale, and
# leaves <out>\<page>.json (the window's UI Automation tree, raw view, at the
# top of the page and scrolled to its end) and <out>\<page>.png (that monitor's
# area, holding only the Settings window's own pixels).
# Raw output names the operator (the account header, device and network
# names): it stays in <out>, which is never published as it is.
param(
    [Parameter(Mandatory)] [string] $Out,
    [string[]] $Pages = @(),
    [switch] $Restore
)
$ErrorActionPreference = 'Stop'
# The capture profile (fhd-100 in docs/design/inventory/profiles.json): fixed, and
# recorded in every page's dump, so a run at another size or scale is refused.
$Width = 1920; $Height = 1080; $Dpi = 96; $TextScale = 100; $Contrast = 'none'

Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public MOUSEINPUT mi; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
  // The pointer moved to (x, y) as real mouse input, not only placed there: the
  // last input is then the mouse, so a window opened after it draws no keyboard
  // focus ring (the reference is captured with no keyboard in use).
  public static void PointerTo(int x, int y) {
    int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);   // SM_*VIRTUALSCREEN
    var i = new INPUT[1];
    i[0].type = 0;   // INPUT_MOUSE
    i[0].mi.dx = (int)(((long)(x - vx) * 65535) / Math.Max(1, vw - 1));
    i[0].mi.dy = (int)(((long)(y - vy) * 65535) / Math.Max(1, vh - 1));
    i[0].mi.flags = 0x0001 | 0x4000 | 0x8000;   // MOVE | VIRTUALDESK | ABSOLUTE
    if (SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) != 1) throw new System.ComponentModel.Win32Exception();
  }
  [DllImport("user32.dll")] public static extern IntPtr MonitorFromPoint(POINT p, uint f);
  [DllImport("shcore.dll")] public static extern int GetDpiForMonitor(IntPtr h, int t, out uint x, out uint y);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, UIntPtr w, string l, uint f, uint t, out UIntPtr r);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct POINT { public int X; public int Y; }
  public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
}
// The Wi-Fi names, read through the Native Wifi API: a profile name is UTF-16
// and an SSID is its own bytes, so no code page can garble either (netsh's text
// output can, and a garbled name masks nothing).
public static class Wlan {
  [DllImport("wlanapi.dll")] static extern uint WlanOpenHandle(uint v, IntPtr r, out uint n, out IntPtr h);
  [DllImport("wlanapi.dll")] static extern uint WlanCloseHandle(IntPtr h, IntPtr r);
  [DllImport("wlanapi.dll")] static extern uint WlanEnumInterfaces(IntPtr h, IntPtr r, out IntPtr l);
  [DllImport("wlanapi.dll")] static extern uint WlanGetProfileList(IntPtr h, ref Guid g, IntPtr r, out IntPtr l);
  [DllImport("wlanapi.dll")] static extern uint WlanQueryInterface(IntPtr h, ref Guid g, int op, IntPtr r, out uint s, out IntPtr d, IntPtr t);
  [DllImport("wlanapi.dll")] static extern void WlanFreeMemory(IntPtr p);
  const int InterfaceInfo = 532;   // GUID, WCHAR[256] description, state
  const int ProfileInfo = 516;     // WCHAR[256] name, flags
  const int Connected = 1;         // wlan_interface_state_connected
  const int CurrentConnection = 7; // wlan_intf_opcode_current_connection
  static void Ok(uint e, string what) { if (e != 0) throw new System.ComponentModel.Win32Exception((int)e, what); }
  // [0] each connected interface's SSID (and its profile name), [1] each saved profile's name.
  public static string[][] Names() {
    var ssids = new System.Collections.Generic.List<string>();
    var profiles = new System.Collections.Generic.List<string>();
    uint neg; IntPtr h;
    Ok(WlanOpenHandle(2, IntPtr.Zero, out neg, out h), "WlanOpenHandle");
    try {
      IntPtr il; Ok(WlanEnumInterfaces(h, IntPtr.Zero, out il), "WlanEnumInterfaces");
      try {
        int n = Marshal.ReadInt32(il);
        for (int i = 0; i < n; i++) {
          IntPtr it = il + 8 + i * InterfaceInfo;
          Guid g = (Guid)Marshal.PtrToStructure(it, typeof(Guid));
          IntPtr pl; Ok(WlanGetProfileList(h, ref g, IntPtr.Zero, out pl), "WlanGetProfileList");
          try {
            int m = Marshal.ReadInt32(pl);
            for (int j = 0; j < m; j++) profiles.Add(Marshal.PtrToStringUni(pl + 8 + j * ProfileInfo));
          } finally { WlanFreeMemory(pl); }
          if (Marshal.ReadInt32(it, 16 + 512) != Connected) continue;
          uint size; IntPtr ca;
          Ok(WlanQueryInterface(h, ref g, CurrentConnection, IntPtr.Zero, out size, out ca, IntPtr.Zero), "WlanQueryInterface");
          try {
            ssids.Add(Marshal.PtrToStringUni(ca + 8));                 // strProfileName
            byte[] b = new byte[Math.Min(Marshal.ReadInt32(ca, 520), 32)];
            Marshal.Copy(ca + 524, b, 0, b.Length);                    // DOT11_SSID
            ssids.Add(Encoding.UTF8.GetString(b));
            ssids.Add(Encoding.Default.GetString(b));                  // shown so when not UTF-8
          } finally { WlanFreeMemory(ca); }
        }
      } finally { WlanFreeMemory(il); }
    } finally { WlanCloseHandle(h, IntPtr.Zero); }
    return new[] { ssids.ToArray(), profiles.ToArray() };
  }
}
"@
# Per-monitor aware: UI Automation bounds and the screen copy are then in the
# same physical pixels as the monitor.
[W]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing, System.Windows.Forms

$Uri = @{ bluetooth = 'ms-settings:bluetooth'; camera = 'ms-settings:camera'
          graphics = 'ms-settings:display-advancedgraphics'; wifi = 'ms-settings:network-wifi' }
# The page's title, the last breadcrumb (en-US, the profile's display language):
# the window shows the requested page only once it is there.
$Title = @{ bluetooth = 'Devices'; camera = 'Cameras'; graphics = 'Graphics'; wifi = 'Wi-Fi' }
$SettleStep = 1000         # ms: a page is settled when two reads this far apart, twice running, agree
$WindowWait = 30            # s for the Settings window to appear
$MaxDepth = 30              # levels of the tree: a Settings page nests about 20 deep

function Monitor {
    foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
        $p = New-Object W+POINT; $p.X = $s.Bounds.X + 10; $p.Y = $s.Bounds.Y + 10
        $x = 0; $y = 0
        [W]::GetDpiForMonitor([W]::MonitorFromPoint($p, 2), 0, [ref]$x, [ref]$y) | Out-Null
        if ($s.Bounds.Width -eq $Width -and $s.Bounds.Height -eq $Height -and $x -eq $Dpi) { return $s.Bounds }
    }
    throw "no monitor is ${Width}x${Height} at $Dpi dpi"
}

$Personalize = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
$Accent = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent'
$Dwm = 'HKCU:\Software\Microsoft\Windows\DWM'
$Desktop = 'HKCU:\Control Panel\Desktop'
# The profile's values (fhd-100 in docs/design/inventory/profiles.json): light
# apps and system, the default blue accent (0078D4) and its palette.
$Profile = @(
    @($Personalize, 'AppsUseLightTheme', 'DWord', 1), @($Personalize, 'SystemUsesLightTheme', 'DWord', 1),
    @($Personalize, 'ColorPrevalence', 'DWord', 0), @($Desktop, 'AutoColorization', 'String', '0'),
    @($Accent, 'AccentColorMenu', 'DWord', 0xFFD47800), @($Accent, 'StartColorMenu', 'DWord', 0xFFC06700),
    @($Accent, 'AccentPalette', 'Binary', [byte[]](0x99,0xEB,0xFF,0,0x4C,0xC2,0xFF,0,0,0x91,0xF8,0,0,0x78,0xD4,0,0,0x67,0xC0,0,0,0x3E,0x92,0,0,0x1A,0x68,0,0xF7,0x63,0x0C,0)),
    @($Dwm, 'AccentColor', 'DWord', 0xFFD47800), @($Dwm, 'ColorizationColor', 'DWord', 0xC40078D4),
    @($Dwm, 'ColorizationAfterglow', 'DWord', 0xC40078D4)
)

# The shell told the colours changed. A notification not delivered throws, so a
# restore is not reported (and its pending file not removed) while the shell
# still shows the capture theme; the next run, or -Restore, writes and tells again.
function Broadcast {
    $r = [UIntPtr]::Zero
    if ([W]::SendMessageTimeout([IntPtr]0xFFFF, 0x1A, [UIntPtr]::Zero, 'ImmersiveColorSet', 2, 5000, [ref]$r) -eq [IntPtr]::Zero) {
        throw "the shell was not told the theme changed (WM_SETTINGCHANGE not delivered); the values are written, run again with -Restore"
    }
}

# A value as the registry holds it: $null only when its key or the value is
# confirmed missing. Any other failure to read (access, a damaged hive) throws,
# so nothing is saved, changed or restored on a guess.
function Read-Value($path, $name) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($path.Substring('HKCU:\'.Length))
    if ($null -eq $key) { return $null }
    try {
        if ($key.GetValueNames() -notcontains $name) { return $null }
        [pscustomobject]@{ Value = $key.GetValue($name, $null, 'DoNotExpandEnvironmentNames'); Kind = $key.GetValueKind($name).ToString() }
    } finally { $key.Dispose() }
}

function Save-Values {
    foreach ($v in $Profile) {
        # A value the operator never set is put back absent; one they set keeps its kind.
        $have = Read-Value $v[0] $v[1]
        if ($have) { [pscustomobject]@{ Path = $v[0]; Name = $v[1]; Type = $have.Kind; Value = $have.Value } }
        else { [pscustomobject]@{ Path = $v[0]; Name = $v[1]; Type = $v[2]; Value = $null } }
    }
}

function Put-Values($values) {
    foreach ($v in $values) {
        # Absent before the capture: removed again (already absent is not an error).
        # A failed removal throws, so the pending-restore file is kept for a retry.
        if ($null -eq $v.Value) {
            if (Read-Value $v.Path $v.Name) {   # already absent is done
                Remove-ItemProperty -Path $v.Path -Name $v.Name -ErrorAction Stop
            }
            continue
        }
        if (-not (Test-Path $v.Path)) { New-Item -Path $v.Path -Force | Out-Null }
        $value = $v.Value
        # A DWORD read back can arrive unsigned (4286644328); the registry takes it as Int32.
        # Read back from JSON, a Binary value is a list of numbers.
        if ($v.Type -eq 'Binary') { $value = [byte[]]@($value) }
        if ($v.Type -eq 'DWord') { $value = if ([int64]$value -gt 0x7FFFFFFF) { [int32]([int64]$value - 0x100000000) } else { [int32]$value } }
        New-ItemProperty -Path $v.Path -Name $v.Name -PropertyType $v.Type -Value $value -Force | Out-Null
    }
    Broadcast
}

function Settings-Window {
    $deadline = (Get-Date).AddSeconds($WindowWait)
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, 'ApplicationFrameWindow')
    while ((Get-Date) -lt $deadline) {
        foreach ($w in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)) {
            if ($w.Current.Name -eq 'Settings') { return $w }
        }
        Start-Sleep -Milliseconds 500
    }
    throw "no Settings window within $WindowWait s"
}

# The whole raw tree is read in one cross-process call: a CacheRequest for the
# properties the dump keeps, over the subtree, then walked from the cache.
$Cache = New-Object System.Windows.Automation.CacheRequest
$Cache.TreeFilter = [System.Windows.Automation.Automation]::RawViewCondition
$Cache.TreeScope = [System.Windows.Automation.TreeScope]::Subtree
$Cache.AutomationElementMode = [System.Windows.Automation.AutomationElementMode]::None
foreach ($p in @('NameProperty', 'ControlTypeProperty', 'AutomationIdProperty', 'ClassNameProperty',
                 'IsOffscreenProperty', 'IsEnabledProperty', 'BoundingRectangleProperty')) {
    $Cache.Add([System.Windows.Automation.AutomationElement]::$p)
}

function Walk($el, $depth) {
    $c = $el.Cached
    $r = $c.BoundingRectangle
    $node = [ordered]@{ name = $c.Name; controlType = $c.ControlType.ProgrammaticName -replace '^ControlType\.', ''
        automationId = $c.AutomationId; className = $c.ClassName; offscreen = $c.IsOffscreen; enabled = $c.IsEnabled
        bounds = if ($r.IsEmpty) { $null } else { @([int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height) }
        children = @() }
    if ($depth -lt $MaxDepth) {
        $node.children = @(foreach ($k in $el.CachedChildren) { Walk $k ($depth + 1) })
    }
    $node
}

# Two snapshots alike in every field and child, compared to the first difference.
function Same($a, $b) {
    foreach ($f in 'name', 'controlType', 'automationId', 'className', 'offscreen', 'enabled') {
        if ($a[$f] -cne $b[$f]) { return $false }
    }
    if ("$($a.bounds)" -ne "$($b.bounds)" -or $a.children.Count -ne $b.children.Count) { return $false }
    for ($i = 0; $i -lt $a.children.Count; $i++) {
        if (-not (Same $a.children[$i] $b.children[$i])) { return $false }
    }
    $true
}

function Snapshot($win) {
    Walk ($win.GetUpdatedCache($Cache)) 0
}

function Wait-Page($win, $page) {
    $deadline = (Get-Date).AddSeconds($WindowWait)
    $cond = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, 'Microsoft.UI.Xaml.Controls.BreadcrumbBarItem')),
        (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, $Title[$page])))
    # Searched in the breadcrumb bar (found again if navigation replaced it), not the whole window.
    $barCond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'PermanentNavigationViewBreadcrumbBar')
    $bar = $null
    while ((Get-Date) -lt $deadline) {
        try {
            if (-not $bar) { $bar = $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $barCond) }
            if ($bar -and $bar.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)) { return }
        } catch [System.Windows.Automation.ElementNotAvailableException] { $bar = $null }
        Start-Sleep -Milliseconds 500
    }
    throw "Settings did not show the $page page ($($Title[$page])) within $WindowWait s"
}

# Text size and contrast theme, read as guest\probe-profile.ps1 reads them: no
# TextScaleFactor is Windows' default 100, and a contrast theme is on when the
# HighContrast flags carry HCF_HIGHCONTRASTON (its stored scheme name is enough
# to refuse the run, so it is not translated here).
function Accessibility {
    $text = 100
    $acc = 'HKCU:\Software\Microsoft\Accessibility'
    if ((Test-Path -LiteralPath $acc) -and $null -ne (Get-Item -LiteralPath $acc).GetValue('TextScaleFactor')) {
        $text = [int](Get-ItemPropertyValue -LiteralPath $acc -Name TextScaleFactor)
    }
    $hc = Get-ItemProperty 'HKCU:\Control Panel\Accessibility\HighContrast' -ErrorAction SilentlyContinue
    $contrast = if ($hc -and ([int]$hc.Flags -band 1)) { [string]$hc.'High Contrast Scheme' } else { 'none' }
    @{ textScale = $text; highContrast = $contrast }
}

function Build {
    $k = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    "$($k.CurrentBuild).$($k.UBR)"
}

$MaxViews = 20             # half-viewport steps per page; a Settings page is a few viewports long

# The page's content pane and the scroll pattern that moves it (the pane's own,
# or the first under it), found once per page; $null pattern when it does not scroll.
function Content($win) {
    $content = $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty, 'ItemsControlScrollViewer')))
    if (-not $content) { throw "the page has no content pane (ItemsControlScrollViewer)" }
    $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::IsScrollPatternAvailableProperty, $true)
    $pane = $content.FindFirst([System.Windows.Automation.TreeScope]::Subtree, $cond)
    $pattern = if ($pane) { $pane.GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern) } else { $null }
    @{ element = $content; pane = $pane; pattern = $pattern }
}

# How far the content is scrolled, in pixels: a control's place on the page is
# its screen position plus this, the same in every view.
function Scrolled($c) {
    if (-not $c.pattern) { return 0 }
    $v = $c.pattern.Current
    if (-not $v.VerticallyScrollable -or $v.VerticalViewSize -ge 100 -or $v.VerticalViewSize -le 0) { return 0 }
    $h = $c.pane.Current.BoundingRectangle.Height
    [int][Math]::Round($v.VerticalScrollPercent / 100 * $h * (100 / $v.VerticalViewSize - 1))
}

# The page read again at each half-viewport step down to the end, so every
# control no taller than half the viewport is whole in at least one view (one
# cut off at an edge of one view is whole in the next). A page that does not
# scroll adds no view.
function Views($c) {
    $views = @()
    for ($i = 0; $i -le $MaxViews; $i++) {
        if (-not $c.pattern) { return , $views }
        $v = $c.pattern.Current
        if (-not $v.VerticallyScrollable -or $v.VerticalScrollPercent -ge 100 -or $v.VerticalViewSize -ge 100) { return , $views }
        # A page longer than the steps allow would be published part read.
        if ($i -eq $MaxViews) { throw "the page did not reach its end in $MaxViews half-viewport steps" }
        # Half a viewport, as a percentage of the scrollable range.
        $step = 50 * $v.VerticalViewSize / (100 - $v.VerticalViewSize)
        $c.pattern.SetScrollPercent(-1, [Math]::Min(100, $v.VerticalScrollPercent + $step))
        Start-Sleep -Seconds 1
        # After a scroll only the page's content is read again: the rest of the window does not move.
        $snap = Snapshot $c.element
        $snap.scrolled = Scrolled $c
        $views += , $snap
    }
}

$SavedPath = Join-Path $Out 'saved-values.json'
# Written before the first theme write and removed after the restore: a run cut
# off by a restart or a kill leaves it, and the next run restores from it before
# it saves anything, so the capture theme is never saved as the operator's own.
# The capture root, the same folder as capture.WORK in capture.py: the journal
# lives there whatever -Out a caller gives, so every run finds it.
$Captures = 'D:\11\captures'
if ((Split-Path -Parent $Out).TrimEnd('\') -ne $Captures) { throw "-Out must be a folder directly under $Captures" }
$Pending = Join-Path $Captures 'operator-pending-restore.json'
# Settings closed: a window opened under the capture theme keeps it until it closes.
# None open is fine; one that does not exit within 10 s throws.
# The shot is the Settings window's own pixels (Windows Graphics Capture), so no
# other window, notification or overlay can be in it. The helper is compiled once
# per source version into the capture root.
function Load-WindowCapture {
    $src = Join-Path $PSScriptRoot 'window-capture.cs'
    $dll = Join-Path $Captures ('window-capture-' + (Get-FileHash -Algorithm SHA256 -LiteralPath $src).Hash.Substring(0, 16) + '.dll')
    if (-not (Test-Path -LiteralPath $dll)) {
        $fw = [Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()
        $md = Join-Path $env:SystemRoot 'System32\WinMetadata'
        $log = & (Join-Path $fw 'csc.exe') /nologo /unsafe /target:library /optimize "/out:$dll.tmp" "/reference:$(Join-Path $fw 'System.Runtime.dll')" "/reference:$md\Windows.Foundation.winmd" "/reference:$md\Windows.Graphics.winmd" $src 2>&1
        if ($LASTEXITCODE -ne 0) { throw "the window-capture helper did not compile: $($log -join ' ')" }
        Move-Item -LiteralPath "$dll.tmp" -Destination $dll -Force
    }
    [Reflection.Assembly]::LoadFile($dll) | Out-Null
}

# The window's pixels placed on a monitor-sized image where the window shows
# them, so the shot and the tree share the monitor's coordinates; the rest (the
# taskbar strip, blanked in every shot) stays black.
function Shot($h) {
    $w = 0; $ht = 0; $x = 0; $y = 0
    $px = [WindowCapture]::Capture($h, 5000, [ref]$w, [ref]$ht, [ref]$x, [ref]$y)
    $x -= $mon.X; $y -= $mon.Y
    if ($x -lt 0 -or $y -lt 0 -or $x + $w -gt $mon.Width -or $y + $ht -gt $mon.Height) {
        throw "the Settings frame (${w}x$ht at $x,$y) is not inside the monitor"
    }
    $fmt = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
    $bmp = New-Object System.Drawing.Bitmap $mon.Width, $mon.Height, $fmt
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.Clear([System.Drawing.Color]::Black); $g.Dispose()
    $d = $bmp.LockBits([System.Drawing.Rectangle]::new($x, $y, $w, $ht), [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $fmt)
    try {
        for ($r = 0; $r -lt $ht; $r++) {
            [System.Runtime.InteropServices.Marshal]::Copy($px, $r * $w * 4, [IntPtr]($d.Scan0.ToInt64() + [long]$r * $d.Stride), $w * 4)
        }
    } finally { $bmp.UnlockBits($d) }
    $bmp
}

function Close-Settings {
    $open = @(Get-Process SystemSettings -ErrorAction SilentlyContinue)
    if (-not $open) { return }
    $open | Stop-Process -Force -ErrorAction SilentlyContinue
    # Polled by id: a killed process stays listed a moment after Wait-Process returns.
    $ids = @($open | ForEach-Object { $_.Id })
    $deadline = (Get-Date).AddSeconds(10)
    while (@(Get-Process -Id $ids -ErrorAction SilentlyContinue).Count) {
        if ((Get-Date) -gt $deadline) { throw "Settings did not close; close it to drop the capture theme" }
        Start-Sleep -Milliseconds 250
    }
}

# The operator's values written back and Settings closed; the journal goes only
# when both are done, so a window still open keeps it for the next run or -Restore.
function Restore-Values($values) {
    $err = $null
    try { Close-Settings } catch { $err = $_ }
    Put-Values $values
    if ($err) { throw $err }
    # Removed already is fine; one that cannot be removed throws, so a stale
    # journal is never left to undo a theme the operator picks later.
    if (Test-Path -LiteralPath $Pending) { Remove-Item -LiteralPath $Pending -ErrorAction Stop }
}

function Restore-Pending {
    if (Test-Path -LiteralPath $Pending) {
        # Windows PowerShell's ConvertFrom-Json emits a JSON array as one object: unrolled here.
        Restore-Values @(Get-Content -LiteralPath $Pending -Raw | ConvertFrom-Json | ForEach-Object { $_ })
        "restored the operator's theme and accent from an interrupted capture"
    }
}
# One host at a time on this machine, the Python lock or not, across Windows
# sessions too (the restore journal is one file for the account): released when
# the process ends, so a killed run frees it.
$Mutex = New-Object System.Threading.Mutex($false, 'Global\impossible-os-operator-host')
try { $held = $Mutex.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $held = $true }
if (-not $held) { $Mutex.Dispose(); throw 'another operator-host.ps1 is running; run one at a time' }
# Released at the end whichever way the script ends, also when it ran inside a
# PowerShell session that stays open.
try {
# An interrupted capture's theme first, before anything this run can refuse.
Restore-Pending
if ($Restore) { return }
# -File hands a comma list over as one string.
$Pages = @($Pages | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Pages) { throw 'no pages named' }
$Pages = @($Pages | Select-Object -Unique)
$build = Build
$mon = Monitor
$access = Accessibility
# Refused before the theme changes: such a run could never be published.
if ($access.textScale -ne $TextScale -or $access.highContrast -ne $Contrast) {
    throw "text size $($access.textScale)% and contrast '$($access.highContrast)' are not the capture profile's ($TextScale%, '$Contrast')"
}
Load-WindowCapture   # compiled before anything changes: a helper that cannot build stops the run here
# A fresh folder per run: a retry into an old one could leave its tree beside a new shot.
if (Test-Path -LiteralPath $Out) { throw "$Out exists already; every capture writes a new folder" }
New-Item -ItemType Directory -Path $Out | Out-Null
$ssids = @([Wlan]::Names()[0] | Where-Object { $_ })   # before the journal: a failed read changes nothing
$saved = @(Save-Values)
$saved | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $SavedPath -Encoding UTF8
# Written whole beside it, then renamed: recovery sees no journal or a complete one.
$PendingNew = "$Pending.new"
Copy-Item -LiteralPath $SavedPath -Destination $PendingNew
Move-Item -LiteralPath $PendingNew -Destination $Pending -Force
# Who and what the run read, for redacting it later: private, never published.
# Every Wi-Fi network name Windows can show for this machine is the operator's:
# the SSID of each connected interface (what Settings names a row for) and each
# saved profile. The query has its own deadline; without its answer the run stops.
# Read before the first page and again after the last (the two kept), so a
# network the adapter joins during the capture is masked too. identity.json is
# written once, after the last read: a run cut off before it cannot be replayed.
function Write-Identity($earlier) {
    $names = [Wlan]::Names()
    $ssids = @(@($earlier) + @($names[0]) | Where-Object { $_ } | Select-Object -Unique)
    $profiles = @($names[1] | Where-Object { $_ } | Select-Object -Unique)
    [ordered]@{ version = 10; computer = $env:COMPUTERNAME; user = $env:USERNAME; profile = $env:USERPROFILE
                ssids = $ssids; profiles = $profiles } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Out 'identity.json') -Encoding UTF8
}
try {
    Put-Values ($Profile | ForEach-Object { [pscustomobject]@{ Path = $_[0]; Name = $_[1]; Type = $_[2]; Value = $_[3] } })
    # A Settings window from before the change keeps the old theme; none open is fine.
    Close-Settings
    $first = $true
    foreach ($page in $Pages) {
        if (-not $Uri.ContainsKey($page)) { throw "unknown page $page" }
        # Mouse input before the window opens, so it opens without a keyboard focus ring.
        [W]::PointerTo($mon.X + $mon.Width - 2, $mon.Y + $mon.Height - 2)
        Start-Process $Uri[$page]
        $win = Settings-Window
        $h = [IntPtr]$win.Current.NativeWindowHandle
        if ($first) {
            # Posted, not sent, so a window that stops answering cannot hold the
            # run: each step is checked done before the next, under one deadline.
            $deadline = (Get-Date).AddSeconds($WindowWait)
            function Until($what, $done) {
                while (-not (& $done)) {
                    if ((Get-Date) -gt $deadline) { throw "Settings did not $what within $WindowWait s" }
                    Start-Sleep -Milliseconds 100
                }
            }
            [W]::ShowWindowAsync($h, 9) | Out-Null                          # SW_RESTORE
            Until 'restore' { -not [W]::IsZoomed($h) }
            [W]::SetWindowPos($h, [IntPtr]0, $mon.X + 100, $mon.Y + 100, 1200, 800, 0x4004) | Out-Null   # SWP_ASYNCWINDOWPOS | SWP_NOZORDER
            Until 'move to the capture monitor' { $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) -and $r.Left -eq $mon.X + 100 -and $r.Top -eq $mon.Y + 100 }
            [W]::ShowWindowAsync($h, 3) | Out-Null                          # SW_MAXIMIZE
            Until 'maximize' { [W]::IsZoomed($h) }
            # Moved across monitors, the window changed scale and its navigation
            # icons do not all reload. Closed (so Settings keeps the placement)
            # and opened again, it starts on the monitor at its scale.
            Start-Sleep -Seconds 1
            $win.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
            # This window gone (already gone is fine), so the window found next is the new one.
            Until 'close for the reopen' { -not [W]::IsWindow($h) }
            [W]::PointerTo($mon.X + $mon.Width - 2, $mon.Y + $mon.Height - 2)
            Start-Process $Uri[$page]
            $win = Settings-Window
            $h = [IntPtr]$win.Current.NativeWindowHandle
            $first = $false
        }
        Wait-Page $win $page
        [W]::SetForegroundWindow($h) | Out-Null
        # The pointer off the window: no hover state in the shot.
        [W]::PointerTo($mon.X + $mon.Width - 2, $mon.Y + $mon.Height - 2)
        # Its lists fill in after it opens: read until the content stops changing.
        $c = Content $win
        $deadline = (Get-Date).AddSeconds($WindowWait)
        $last = Snapshot $c.element; $alike = 0
        while ($alike -lt 2) {
            if ((Get-Date) -gt $deadline) { throw "the $page page kept changing for $WindowWait s" }
            Start-Sleep -Milliseconds $SettleStep
            $now = Snapshot $c.element
            $alike = if (Same $last $now) { $alike + 1 } else { 0 }
            $last = $now
        }
        # Settings in front (an inactive window draws its inactive backdrop) and
        # covering the monitor above the taskbar, as the reference shows it.
        $b = $win.Current.BoundingRectangle
        if ([W]::GetForegroundWindow() -ne $h) { throw "Settings is not the foreground window on $page" }
        if ($b.X -gt $mon.X -or $b.Y -gt $mon.Y -or $b.Right -lt $mon.Right -or $b.Bottom -lt $mon.Bottom - 48) {
            throw "Settings does not cover the monitor on $page ($b)"
        }
        # The tree read before and after the shot, the shot kept only when the two
        # agree and Settings stayed in front: the masks come from the tree, so a
        # list that changed between them would put them off the pixels they cover.
        # Both go in the dump, and publication checks them again.
        for ($try = 1; ; $try++) {
            $before = Snapshot $win
            $bmp = Shot $h
            $top = Snapshot $win
            $front = [W]::GetForegroundWindow() -eq $h
            if ($front -and (Same $before $top)) { break }
            $bmp.Dispose()
            if ($try -ge 3) {
                if (-not $front) { throw "Settings lost the foreground while $page was taken" }
                throw "the $page page kept changing while it was taken"
            }
            Start-Sleep -Seconds 2
        }
        $bmp.Save((Join-Path $Out "$page.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $top.scrolled = Scrolled $c
        $views = Views $c
        [ordered]@{ page = $page; uri = $Uri[$page]; build = $build; monitor = @($mon.X, $mon.Y, $mon.Width, $mon.Height); dpi = $Dpi
                    textScale = $access.textScale; highContrast = $access.highContrast
                    before = $before; top = $top; views = $views } |
            ConvertTo-Json -Depth 100 -Compress | Set-Content -LiteralPath (Join-Path $Out "$page.json") -Encoding UTF8
        $before = $top = $views = $null   # written: free for the next page
        "captured $page"
    }
    Write-Identity $ssids
}
finally {
    Restore-Values $saved           # on failure too
    "restored the operator's theme and accent"
}
}
finally {
    $Mutex.ReleaseMutex()
    $Mutex.Dispose()
}
