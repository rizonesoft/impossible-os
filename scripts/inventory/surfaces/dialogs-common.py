"""Surface module: the common dialogs, message boxes and task dialogs.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 12 (the
Common Dialogs item). Writes docs/design/inventory/dialogs/common.json:

    python3 scripts/inventory/capture.py --surface dialogs --file common --module dialogs-common --prepare explorer:sample

Each scene starts shared/dialog_host.py, which shows one dialog the way an
application asks Windows for it (GetOpenFileName, ChooseColor, MessageBox,
TaskDialogIndirect and the rest) from a process with comctl32 v6, and walks
the window that appears. Notepad's Print is measured: it shows the classic
dialog on the reference. The sounds a message box plays are read from the sound scheme in the registry.
"""
import json
import pathlib
import sys
import time

import capture_core as core
from dialog_window import DIALOG_WAIT, appeared, baseline, close_new_windows, top_windows, user32
from explorer_window import SETTLE, _root, descendants, opened

SURFACE = "dialogs"
# Lists whose rows are this machine's data, kept as a frame with no rows: the
# address band's folders, the printers, the installed font faces, the folder trees (this account's folders),
# and the file lists (as the frame module keeps them).
ENVIRONMENT_LISTS = [{"className": "ToolbarWindow32", "namePrefix": "Address: "},
                     {"className": "SysListView32", "name": "Folder View"},
                     {"className": "SysTreeView32"},
                     {"className": "ComboLBox", "name": "Font:"},
                     {"className": "UIItemsView"}]
# The classic address band's toolbar names itself "Address: " and the folder's path.
ADDRESS_PATH = {"className": "ToolbarWindow32", "namePrefix": "Address: "}
# Refresh names the open folder; the About box names the reference build.
REFRESH = {"controlType": "Button", "namePrefix": "Refresh \""}
UP = {"controlType": "Button", "namePrefix": "Up to \""}       # names the parent folder
VERSION = {"controlType": "Text", "namePrefix": "Version "}
VALUES = [ADDRESS_PATH, REFRESH, UP, VERSION]
# Look-alike siblings named for what they are: the Save As labels (one automation
# id for both), and the colour and page-setup pictures by their dialog control ids.
IDENTITY = [({"automationId": "SaveDialogLabel", "name": "File name:"}, "file-name-label"),
            ({"automationId": "SaveDialogLabel", "name": "Save as type:"}, "save-as-type-label"),
            ({"className": "Static", "automationId": "720"}, "basic-color-swatches"),
            ({"className": "Static", "automationId": "721"}, "custom-color-swatches"),
            ({"className": "Static", "automationId": "710"}, "color-spectrum"),
            ({"className": "Static", "automationId": "702"}, "luminance-bar"),
            ({"className": "Static", "automationId": "709"}, "color-sample"),
            ({"className": "Static", "automationId": "1080"}, "page-preview"),
            ({"className": "Static", "automationId": "1081"}, "preview-right-shadow"),
            ({"className": "Static", "automationId": "1082"}, "preview-bottom-shadow")]
LABELS = [(ADDRESS_PATH, "Address"), (REFRESH, "Refresh"), (UP, "Up"), (VERSION, "Version")]

HOST = pathlib.Path(__file__).resolve().parent / "shared" / "dialog_host.py"
CAPTION = "Inventory sample"
WM_SYSCOMMAND, SC_CLOSE = 0x0112, 0xF060
GHOST_WAIT = 8                           # s: Windows calls a window hung after 5 s without messages
NOTEPAD_WAIT = 30
WER_WAIT = 30                            # s for Windows Error Reporting to show its dialog
CLASSIC_PRINT = "#32770"                 # the Win32 dialog class PrintDlgEx shows
# Controls of the classic Print dialog the Print scene walked (common.json).
CLASSIC_PRINT_CONTROLS = (("Button", "Preferences"), ("Button", "Find Printer..."), ("Button", "Print"),
                          ("List", "Folder View"))
# RaiseFailFastException with no record ends the process with STATUS_FAIL_FAST_EXCEPTION.
FAIL_FAST_EXIT = 0xC0000602
HOST_EXIT_WAIT = 3                       # s for a dialog host to end once its dialog closed
END_HOSTS = r"""
function Hosts { @(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -like '*dialog_host.py*' }) }
# A host can end between the listing and the stop (its dialog closed): the check
# after the loop is what proves none is left, not the stop's own result.
$found = Hosts
foreach ($p in $found) { try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop } catch [Microsoft.PowerShell.Commands.ProcessCommandException] { } }
if ($found.Count) { Start-Sleep 1 }
if ((Hosts).Count) { Write-Output ('dialog hosts still running: ' + (Hosts).Count); exit 1 }
exit 0
"""
COUNT_HOSTS = r"""
@(Get-CimInstance Win32_Process -Filter "Name = 'python.exe'" | Where-Object { $_.CommandLine -like '*dialog_host.py*' }).Count
"""
# The About frame prints the machine's registered owner and organisation. The
# reference VM's are the reference account's own; anything else would be
# published as text, so the scene refuses (and never prints the value).
SAFE_OWNERS = ("ref", "", "Windows User", "user")
CHECK_OWNER = r"""
$v = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$safe = ConvertFrom-Json '%s'
if (($safe -notcontains [string]$v.RegisteredOwner) -or ($safe -notcontains [string]$v.RegisteredOrganization)) {
    Write-Output 'the registered owner or organisation is not the reference account''s; the About frame would publish it'
    exit 1
}
exit 0
""" % json.dumps(SAFE_OWNERS).replace("'", "''")
# The sound each message box icon plays: the event its icon raises, from the
# current scheme (MessageBeep's mapping in WinUser.h).
ICON_EVENTS = {"error": "SystemHand", "question": "SystemQuestion", "warning": "SystemExclamation",
               "information": "SystemAsterisk"}
READ_SOUNDS = r"""
$out = [ordered]@{}
foreach ($e in ConvertFrom-Json '%s') {
    # An event the scheme does not list plays nothing: that, and only that, is a null.
    $key = "HKCU:\AppEvents\Schemes\Apps\.Default\$e\.Current"
    $v = if (Test-Path -LiteralPath $key) { (Get-ItemProperty -LiteralPath $key).'(default)' } else { $null }
    # A string from ConvertFrom-Json is a wrapped PSObject, which ConvertTo-Json refuses as a key (2026-11-20).
    $out[[string]$e] = if ($v) { Split-Path -Leaf ([Environment]::ExpandEnvironmentVariables($v)) } else { $null }
}
$out | ConvertTo-Json -Compress
"""
# Windows Error Reporting's DontShowUI, machine and user: whether a crash may show
# its dialog. An absent value is reported as null (the default: the dialog may show).
READ_WER = r"""
$out = [ordered]@{}
foreach ($hive in 'HKLM', 'HKCU') {
    $key = "${hive}:\SOFTWARE\Microsoft\Windows\Windows Error Reporting"
    $v = if (Test-Path -LiteralPath $key) { (Get-ItemProperty -LiteralPath $key).DontShowUI } else { $null }
    $out[$hive] = $v
}
$out | ConvertTo-Json -Compress
"""
_m = {}                                  # read by the scenes, used by METRICS

BUTTON_SETS = [("ok", "OK"), ("ok-cancel", "OK, Cancel"), ("yes-no", "Yes, No"),
               ("yes-no-cancel", "Yes, No, Cancel"), ("retry-cancel", "Retry, Cancel"),
               ("abort-retry-ignore", "Abort, Retry, Ignore"), ("cancel-try-continue", "Cancel, Try Again, Continue")]


_hosts = []                              # the dialog hosts this scene started (Popen handles)


def host(kind, check=None, ready=None):
    """Start dialog_host.py with `kind` and take the window it shows; `ready`
    waits for what the window must hold before it is walked."""
    def opener(ctx):
        baseline(ctx)
        if check:
            check(ctx)
        before = top_windows(ctx)
        _hosts.append(ctx.launch(sys.executable, str(HOST), kind))
        _root["el"] = appeared(ctx, before)
        if ready:
            ready(ctx, _root["el"])
    return opener


def close_scene(ctx):
    """Close what the scene opened, then end any dialog host still running. A
    host that exited with its dialog needs no process query; one still running,
    or a scene that started its host some other way, gets END_HOSTS. The hosts
    are ended even when a window refused to close, and that failure is still
    raised (review, 2026-11-21: the hung fixture sleeps 900 s)."""
    started = list(_hosts)
    _hosts.clear()
    try:
        close_new_windows(ctx)
    except BaseException:
        ctx.powershell(END_HOSTS)
        raise
    deadline = time.monotonic() + HOST_EXIT_WAIT
    while started and any(p.poll() is None for p in started) and time.monotonic() < deadline:
        time.sleep(0.2)
    if not started or any(p.poll() is None for p in started):
        ctx.powershell(END_HOSTS)


def close_hung(ctx):
    """close_scene for the not-responding scene, with the hung fixture ended first:
    while it hangs, every UI Automation desktop list waits 20 s on it, and
    close_scene's window scan is one (performance review, 2026-11-21). A host
    that will not die in time is left to close_scene's END_HOSTS."""
    for p in _hosts:
        if p.poll() is None:
            p.kill()
    deadline = time.monotonic() + HOST_EXIT_WAIT
    while any(p.poll() is None for p in _hosts) and time.monotonic() < deadline:
        time.sleep(0.2)
    close_scene(ctx)


# The credential prompt fills its fields in after it shows (2026-11-20: one
# capture held only the frame, the captions and Cancel).
CREDENTIAL_FIELDS = ("EditField_0", "PasswordField_1", "OkButton")


# The application's own control in the Save dialog (dialog_host's save-encoding):
# a combo box under the label it was added with.
ENCODING_LABEL = "Encoding:"


def encoding_ready(ctx, el):
    """Wait for the application's Encoding combo box. Each look stops at the
    combo, and none outlives the wait (review, 2026-11-21)."""
    def combo(k):                         # on screen: the walk keeps nothing offscreen
        return k.control_type == "ComboBox" and ENCODING_LABEL.rstrip(":") in (k.name or "") and not k.offscreen
    kids = []
    with ctx.within(DIALOG_WAIT) as deadline:
        while True:
            try:
                kids = descendants(ctx, el, 8, stop=combo)
            except core.Incomplete:
                kids = []                     # the look ran out of time; the deadline below decides
            if any(combo(k) for k in kids):
                return
            if time.monotonic() > deadline:
                raise core.Incomplete("the Save dialog showed no Encoding combo box: "
                                      f"{sorted({(k.control_type, k.name) for k in kids if k.name})[:40]}")
            time.sleep(0.5)


def credential_ready(ctx, el):
    """Wait for every credential field; each look stops once the last is seen."""
    have = set()

    def complete(k):
        if not k.offscreen:                   # on screen: the walk keeps nothing offscreen
            have.add(k.automation_id)
        return all(a in have for a in CREDENTIAL_FIELDS)
    with ctx.within(DIALOG_WAIT) as deadline:
        while True:
            have.clear()
            try:
                descendants(ctx, el, 4, stop=complete)
            except core.Incomplete:
                pass
            if all(a in have for a in CREDENTIAL_FIELDS):
                return
            if time.monotonic() > deadline:
                raise core.Incomplete(f"the credential prompt never showed {sorted(set(CREDENTIAL_FIELDS) - have)}")
            time.sleep(0.5)


def read_sounds(ctx):
    if "sounds" not in _m:
        _m["sounds"] = json.loads(ctx.powershell(READ_SOUNDS % json.dumps(list(ICON_EVENTS.values()))))


def check_owner(ctx):
    ctx.powershell(CHECK_OWNER)


def save_overwrite(ctx):
    """Save over a file that exists: the Confirm Save As prompt."""
    host("save")(ctx)
    before = top_windows(ctx)
    ctx.keys("enter")                    # the file name box holds Notes.txt, which exists in the sample folder
    _root["el"] = appeared(ctx, before)


def not_responding(ctx):
    """Close a window that has stopped answering: Windows asks whether to end it."""
    host("hung")(ctx)
    time.sleep(GHOST_WAIT)
    ctx.click(_root["el"])               # input it does not read: Windows ghosts it
    time.sleep(2)
    # Win32's window list only from here: with a window hung, each UI Automation
    # desktop list waits 20 s on it (2026-11-21), and the dialog that follows is
    # a Win32 one.
    ghosts = [w for w in top_windows(ctx, uia=False) if w[1] == "Ghost"]
    if len(ghosts) != 1:
        raise core.Incomplete(f"the hung window was ghosted {len(ghosts)} times")
    before = top_windows(ctx, uia=False)
    # The ghost's own close command, as its title bar's Close sends it: Alt+F4 from
    # this process did not reach it (2026-11-20, foreground is not ours to give).
    user32.PostMessageW(ghosts[0][0], WM_SYSCOMMAND, SC_CLOSE, 0)
    _root["el"] = appeared(ctx, before, skip=("CabinetWClass", "Ghost"), uia=False)


def measure_crash(ctx):
    """End a program on a fatal error and watch for what Windows shows. On the
    reference no window appeared (2026-11-20), so this is a measurement METRICS
    publishes, not a scene; a window that does appear is recorded and closed, and
    METRICS then refuses, because that dialog must be walked as a scene."""
    if "crash" in _m:
        return
    baseline(ctx)
    before = top_windows(ctx)
    proc = ctx.launch(sys.executable, str(HOST), "crash")
    deadline, shown = time.monotonic() + WER_WAIT, []
    known = {w[0] for w in before}            # by handle, as appeared(): a renamed window is not a new one
    while time.monotonic() < deadline and not shown:
        time.sleep(1)
        shown = sorted(w[1:] for w in top_windows(ctx) if w[0] not in known and w[1] != "CabinetWClass")
    if shown:
        close_new_windows(ctx)
    running = int(ctx.powershell(COUNT_HOSTS).strip())   # before the cleanup, which would end a held one
    exit_code = proc.poll()                               # the host's own status: did it reach the fail-fast?
    ctx.powershell(END_HOSTS)
    _m["crash"] = {"windows": shown, "running": running, "exit": exit_code, **json.loads(ctx.powershell(READ_WER))}


def measure_print(ctx):
    """Which Print dialog Notepad's Ctrl+P shows. On the reference it is the classic
    one (#32770, the Print scene's frame; review 2026-11-20 found the two records
    alike), so this is a measurement, not a scene; METRICS refuses any other class,
    because a modern dialog must then be walked as a scene."""
    if "print" in _m:
        return
    baseline(ctx)
    before = top_windows(ctx)
    ctx.launch("notepad.exe")
    notepad = appeared(ctx, before, wait=NOTEPAD_WAIT)
    ctx.click(notepad)
    before = top_windows(ctx)
    ctx.keys("ctrl+p")
    shown = appeared(ctx, before, wait=NOTEPAD_WAIT)
    time.sleep(SETTLE)
    controls = sorted({(k.control_type, k.name) for k in descendants(ctx, shown, 6)
                       if (k.control_type, k.name) in CLASSIC_PRINT_CONTROLS})
    _m["print"] = {"className": shown.class_name, "title": shown.name, "controls": controls}
    close_new_windows(ctx)


def scene(slug, label, opener):
    return dict(slug=slug, label=label, kind="dialog", open=opener, root=opened, close=close_scene, expand=False)


def measured_then(kind):
    """The first scenes also take the measurements METRICS publishes (each once)."""
    def opener(ctx):
        read_sounds(ctx)
        measure_crash(ctx)
        measure_print(ctx)
        host(kind)(ctx)
    return opener


SCENES = [scene(f"message-box-{slug}", f"Message box: {label}", measured_then(f"msgbox-{slug}"))
          for slug, label in BUTTON_SETS]
SCENES += [scene(f"message-box-icon-{icon}", f"Message box: the {icon} icon", host(f"msgbox-icon-{icon}"))
           for icon in ICON_EVENTS]
SCENES += [
    scene("message-box-default-second", "Message box: the second button as default", host("msgbox-default-second")),
    scene("message-box-long-text", "Message box: long text", host("msgbox-long")),
    scene("open", "Open", host("open")),
    scene("open-several", "Open: several files", host("open-multi")),
    scene("save-as", "Save As", host("save")),
    scene("save-as-encoding", "Save As with an application's Encoding control", host("save-encoding", ready=encoding_ready)),
    scene("confirm-save-as", "Confirm Save As", save_overwrite),
    scene("select-folder", "Select Folder", host("folder-pick")),
    scene("browse-for-folder", "Browse For Folder", host("browse-folder")),
    scene("color", "Color", host("color")),
    scene("color-custom", "Color: custom colours open", host("color-full")),
    scene("font", "Font", host("font")),
    scene("find", "Find", host("find")),
    scene("replace", "Replace", host("replace")),
    scene("page-setup", "Page Setup", host("page-setup")),
    scene("print", "Print", host("print")),
    scene("task-dialog", "Task dialog", host("task-dialog")),
    scene("credential", "Windows Security: credentials", host("credential", ready=credential_ready)),
    scene("about", "About", host("about", check=check_owner)),
    dict(scene("not-responding", "A program is not responding", not_responding), close=close_hung),
]


# The controls a scene exists to capture: readiness waited for them on screen,
# and the published records must hold them too (review, 2026-11-21).
REQUIRED = ("credential.editfield-0", "credential.passwordfield-1", "credential.okbutton",
            "save-as-encoding.encoding-combobox")


def METRICS(records):
    """The message box's width for long text, and the sound each icon plays."""
    by = {r["id"]: r for r in records}
    missing = [i for i in REQUIRED if f"win11.dialogs.{i}" not in by]
    if missing:
        raise ValueError(f"the scenes published without {missing}")
    out = []
    short, long_ = by.get("win11.dialogs.message-box-ok"), by.get("win11.dialogs.message-box-long-text")
    if not (short and long_ and "bounds" in short and "bounds" in long_):
        raise ValueError("the short and long message boxes were not both captured")
    out.append({"id": "message-box-long-text.width", "parent": "message-box-long-text", "kind": "metric",
                "label": "Width for long text", "order": 900,
                "measure": {"value": long_["bounds"]["width"], "unit": "px", "scaling": "scales"},
                "behaviour": f"Long text wraps at this width; the one-line box is {short['bounds']['width']} px wide."})
    sounds = _m.get("sounds")
    if sounds is None:
        raise ValueError("the sound scheme was not read")
    for i, (icon, event) in enumerate(ICON_EVENTS.items()):
        wav = sounds.get(event)
        out.append({"id": f"message-box-icon-{icon}.sound", "parent": f"message-box-icon-{icon}", "kind": "sound",
                    "label": f"Sound: {event}", "order": 900 + i,
                    "behaviour": f"Plays {wav}, the {event} event of the current scheme." if wav else
                                 f"Plays nothing: the {event} event has no file in the current scheme."})
    crash = _m.get("crash")
    if crash is None:
        raise ValueError("the crash was not measured")
    if crash["windows"]:
        raise ValueError(f"a crash showed {crash['windows']}: walk it as a scene")
    if crash["running"]:
        raise ValueError(f"{crash['running']} crashed host(s) were still running {WER_WAIT} s after the crash")
    # A host that failed before it raised (a Python error at start-up) leaves
    # the same empty screen; only the fail-fast status shows the crash happened.
    if crash.get("exit") != FAIL_FAST_EXIT:
        raise ValueError(f"the crash host ended with {crash.get('exit')!r}, not the fail-fast status {FAIL_FAST_EXIT:#x}")

    shown = _m.get("print")
    if shown is None:
        raise ValueError("Notepad's Print was not measured")
    # The classic dialog is told by its own controls, not its class alone: every
    # Win32 dialog is #32770 (an error box would pass on the class).
    if shown["className"] != CLASSIC_PRINT or shown["title"] != "Print" or \
            {tuple(c) for c in shown["controls"]} != set(CLASSIC_PRINT_CONTROLS):
        raise ValueError(f"Notepad's Print showed {shown}, not the classic Print dialog: walk it as a scene")
    out.append({"id": "print-modern", "kind": "state", "label": "Print from Notepad", "order": 901,
                "behaviour": f"Notepad's Print (Ctrl+P) opens the classic Print dialog ({CLASSIC_PRINT}, the "
                             "frame of the Print scene) on the reference build; no modern Print dialog is shown."})

    def setting(v):
        return "not set" if v is None else str(v)
    out.append({"id": "crash-report", "kind": "state", "label": "A program stopped working", "order": 900,
                "behaviour": f"A program that ends on a fatal error shows no dialog: the process ends and no window "
                             f"appears within {WER_WAIT} s. Windows Error Reporting's DontShowUI is "
                             f"{setting(crash['HKLM'])} for the machine and {setting(crash['HKCU'])} for the user."})
    return out


HAND = []
