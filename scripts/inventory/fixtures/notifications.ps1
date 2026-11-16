# Capture fixture: posts deterministic toasts from made-up senders, so the
# notification centre is captured with known groups, long text and overflow.
# Each sender is a per-user AppUserModelID registered under
# HKCU\Software\Classes\AppUserModelId with a "Sample" display name; each toast
# carries a fixed tag, group and displayTimestamp, so a second run replaces the
# same toasts and shows the same times. The fixture clears its own senders'
# history first and removes the senders it no longer uses.
#   groups    three senders, three toasts each, one with long body text
#   overflow  the same senders with twelve toasts each, more than the centre shows
# -State prints each sender's toast tags as one JSON line and changes nothing.
# Runs in ref's interactive session (capture.py --prepare, fixtures/run.py).
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 20.
# Variants: groups, overflow
param(
    [ValidateSet('groups', 'overflow')][string]$Variant = 'groups',
    [switch]$State
)
$ErrorActionPreference = 'Stop'
[void][Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime]
[void][Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom, ContentType = WindowsRuntime]

$Senders = [ordered]@{ 'IosSample.Mail' = 'Sample Mail'; 'IosSample.Calendar' = 'Sample Calendar'; 'IosSample.Chat' = 'Sample Chat' }
$PerSender = @{ groups = 3; overflow = 12 }
$Classes = 'HKCU:\Software\Classes\AppUserModelId'
# One fixed instant, so every capture shows the same relative or absolute time.
$Stamp = '2026-01-05T09:30:00Z'
$Long = 'This sample message is long on purpose: it runs past the two lines a collapsed notification shows, so the capture records where the text is cut, how the expanded card wraps it, and how far the card grows before it stops.'

function Get-Owned() {
    $h = [Windows.UI.Notifications.ToastNotificationManager]::History
    $o = [ordered]@{}
    foreach ($id in $Senders.Keys) {
        $tags = @()
        if (Test-Path -LiteralPath "$Classes\$id") { $tags = @($h.GetHistory($id) | ForEach-Object { "$($_.Group)/$($_.Tag)" } | Sort-Object) }
        $o[$id] = $tags
    }
    return $o
}

if ($State) { (Get-Owned) | ConvertTo-Json -Compress; exit 0 }

$history = [Windows.UI.Notifications.ToastNotificationManager]::History
foreach ($id in $Senders.Keys) {
    if (Test-Path -LiteralPath "$Classes\$id") { $history.Clear($id) }
    New-Item -Path "$Classes\$id" -Force | Out-Null
    New-ItemProperty -LiteralPath "$Classes\$id" -Name DisplayName -Value $Senders[$id] -PropertyType String -Force | Out-Null
}
$n = $PerSender[$Variant]
foreach ($id in $Senders.Keys) {
    $notifier = [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($id)
    for ($i = 1; $i -le $n; $i++) {
        $body = if ($i -eq 1) { $Long } else { "Sample message $i from $($Senders[$id])." }
        $xml = New-Object Windows.Data.Xml.Dom.XmlDocument
        $xml.LoadXml(("<toast displayTimestamp='{0}'><visual><binding template='ToastGeneric'><text>{1} {2}</text><text>{3}</text></binding></visual></toast>" -f
                $Stamp, $Senders[$id], $i, [Security.SecurityElement]::Escape($body)))
        $toast = New-Object Windows.UI.Notifications.ToastNotification $xml
        $toast.Tag = "t$i"; $toast.Group = 'sample'
        # Straight to the centre: no banner left on screen when the capture starts.
        $toast.SuppressPopup = $true
        $notifier.Show($toast)
    }
}
$owned = Get-Owned
$counts = ($owned.Keys | ForEach-Object { "$($Senders[$_]) $(@($owned[$_]).Count)" }) -join ', '
Write-Output "fixture notifications: $Variant ($counts)"
exit 0
