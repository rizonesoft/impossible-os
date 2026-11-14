# The interactive-session proof of section 2: start Notepad, read its window
# through UI Automation, save a screenshot to C:\ios\out, print one JSON line.
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 2.
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing, System.Windows.Forms
$p = Start-Process notepad.exe -PassThru
try {
    $root = [Windows.Automation.AutomationElement]::RootElement
    $cond = New-Object Windows.Automation.PropertyCondition ([Windows.Automation.AutomationElement]::ClassNameProperty), 'Notepad'
    $win = $null
    for ($i = 0; $i -lt 60 -and -not $win; $i++) {
        $win = $root.FindFirst([Windows.Automation.TreeScope]::Children, $cond)
        if (-not $win) { Start-Sleep -Milliseconds 500 }
    }
    if (-not $win) { Write-Output '{"ok":false,"reason":"no Notepad window in UI Automation"}'; exit 1 }
    $count = $win.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition).Count
    $b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object Drawing.Bitmap $b.Width, $b.Height
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size)
    $bmp.Save('C:\ios\out\notepad.png', [Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    [ordered]@{ ok = $true; name = $win.Current.Name; descendants = $count; shot = 'notepad.png' } | ConvertTo-Json -Compress
} finally { Stop-Process -Id $p.Id -Force }
