"""The lock, sign-in and secure screens, captured from the host.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9.

    python3 scripts/inventory/host-capture.py --scenario session-secure

UI Automation never sees these screens (the secure desktop, the lock and
sign-in screens, the elevation prompt), so every record here is a hand record
measured from the host's framebuffer shots. Each measurement finds the pieces
of ink it expects in a region and refuses (core.Incomplete) when it finds a
different number, so a screen that changed between builds stops the capture
instead of publishing wrong geometry. Message text is recorded as a slot (its
place and length), never in Microsoft's words.

The run makes a second local account, other, whose password must be changed at
first sign-in, sets the lockout to three tries, and builds two fixture programs
in the guest: an unsigned console program (the unknown-publisher prompt) and a
window that holds shut down with a reason (the apps-preventing screen).
"""
import importlib.util
import pathlib
import sys


def _core():
    """host-capture.py's own capture_core when it loaded this scenario, so the
    Incomplete raised here is the class it catches; a fresh copy otherwise."""
    if "capture_core" in sys.modules:
        return sys.modules["capture_core"]
    spec = importlib.util.spec_from_file_location("capture_core", pathlib.Path(__file__).resolve().parents[1] / "capture_core.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


core = _core()

SURFACE = "session"
FILE = "secure"
PROFILE = "fhd-100"
CHECKPOINT = "clean"
TIMEOUT = 1800

# Virtual keys and the screen's fixed places (1920x1080, 100 percent).
ENTER, ESC, TAB, SPACE, WIN, ALT, CTRL, SHIFT, DOWN, F4 = 13, 27, 9, 32, 91, 18, 17, 16, 40, 115
NETWORK, EASE, POWER = (1782, 1038), (1830, 1038), (1878, 1038)
# The account list puts the account signed in last on top, so the account
# switched to is always the second row: other at first, ref after other signs out.
SECOND_ROW = (100, 1022)

BLOCKER = r"""
using System; using System.Runtime.InteropServices; using System.Windows.Forms;
public class Blocker : Form {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool ShutdownBlockReasonCreate(IntPtr h, string r);
    public Blocker() { Text = "Fixture"; Load += delegate { ShutdownBlockReasonCreate(Handle, "Fixture holds shutdown"); }; }
    protected override void WndProc(ref Message m) { if (m.Msg == 0x11) { m.Result = IntPtr.Zero; return; } base.WndProc(ref m); }
    [STAThread] public static void Main() { Application.Run(new Blocker()); }
}"""
FIXTURES = (
    "New-Item -ItemType Directory -Force C:\\ios | Out-Null; "
    "Add-Type -TypeDefinition 'public static class P{public static void Main(){System.Threading.Thread.Sleep(120000);}}' "
    "-OutputAssembly C:\\ios\\unsigned.exe -OutputType ConsoleApplication; "
    "Add-Type -TypeDefinition '" + BLOCKER.replace("'", "''") + "' -ReferencedAssemblies System.Windows.Forms "
    "-OutputAssembly C:\\ios\\blocker.exe -OutputType WindowsApplication"
)


def key(vk):
    return {"op": "key", "vk": vk}


def wait(sec):
    return {"op": "wait", "sec": sec}


def shot(name):
    return {"op": "shot", "name": name}


def chord(mod, vk):
    return {"op": "chord", "mod": mod, "vk": vk}


def click(at):
    return {"op": "click", "x": at[0], "y": at[1]}


FIRST_BURST = 12


def elevate(command, name):
    """Run `command` elevated from the Run box and shoot the prompt."""
    return [chord(WIN, ord("R")), wait(2), {"op": "text", "text": command}, chord([CTRL, SHIFT], ENTER), wait(6), shot(name)]


STEPS = [
    {"op": "user", "name": "other", "flags": "/logonpasswordchg:yes"},
    {"op": "admin", "script": "net accounts /lockoutthreshold:3 /lockoutduration:1 /lockoutwindow:1 | Out-Null; " + FIXTURES},
    # The elevation prompt, as ref (an administrator): consent, its details, an unknown publisher.
    *elevate("cmd", "elevation-consent"),
    # Declining the prompt brings the Run box back; the second Esc closes it.
    key(TAB), wait(1), key(ENTER), wait(2), shot("elevation-details"), key(ESC), wait(3), key(ESC), wait(1),
    *elevate("C:\\ios\\unsigned.exe", "elevation-unknown"), key(ESC), wait(3), key(ESC), wait(1),
    # The shut down dialog and its list.
    chord(WIN, ord("D")), wait(2), chord(ALT, F4), wait(3), shot("shut-down-dialog"),
    chord(ALT, DOWN), wait(2), shot("shut-down-list"), key(ESC), wait(1), key(ESC), wait(2),
    # Security options.
    {"op": "cad"}, wait(4), shot("security-options"), key(ESC), wait(3),
    # Lock, then the sign-in screen and its states.
    {"op": "guest", "cmd": "rundll32.exe user32.dll,LockWorkStation"}, wait(4), shot("lock"),
    key(SPACE), wait(3), shot("secure"),
    {"op": "text", "text": "wrong"}, wait(1), shot("typed"),
    key(ENTER), wait(5), shot("wrong-password"), key(ENTER), wait(2),
    click(POWER), wait(2), shot("power-menu"), key(ESC), wait(1),
    click(EASE), wait(2), shot("accessibility"), key(ESC), wait(1),
    click(NETWORK), wait(4), shot("network"), key(ESC), wait(1),
    # The second account: locked out, then expired, changed and signed in for the first time.
    click(SECOND_ROW), wait(3), shot("sign-in-other"),
    *[s for n in (1, 2, 3) for s in ({"op": "text", "text": f"bad{n}"}, key(ENTER), wait(4), key(ENTER), wait(2))],
    {"op": "text", "text": "bad4"}, key(ENTER), wait(4), shot("locked-account"), key(ENTER),
    wait(70), key(SPACE), wait(3), click(SECOND_ROW), wait(3),
    {"op": "typeof", "name": "other"}, wait(2), key(ENTER), wait(5), shot("password-expired"),
    key(ENTER), wait(3), shot("change-password"),
    # TypeText returns before the guest has taken every character; a Tab sent
    # straight after lands mid-password (2026-11-18: "did not match").
    {"op": "renew", "name": "other"}, wait(2), key(TAB), wait(1), {"op": "typeof", "name": "other"}, wait(2),
    shot("change-password-filled"),
    key(ENTER), wait(5), shot("password-changed"), key(ENTER),
    {"op": "burst", "name": "first-sign-in", "count": FIRST_BURST, "every": 4},
    {"op": "settle", "sec": 5, "timeout": 180}, shot("first-desktop"),
    # The credentials form of the elevation prompt, as other (a standard user).
    *elevate("cmd", "elevation-credentials"), key(ESC), wait(3), key(ESC), wait(1),
    # Sign out, then back in as ref.
    {"op": "cad"}, wait(4), click((925, 474)), {"op": "burst", "name": "signing-out", "count": 4, "every": 0.7},
    {"op": "settle", "sec": 3, "timeout": 90}, key(SPACE), wait(3), click(SECOND_ROW), wait(3),
    {"op": "secret"}, wait(2), key(ENTER), wait(12), {"op": "reconnect"},
    # A program that holds shut down, then Restart from the shut down dialog.
    {"op": "guest", "cmd": "cmd /c start \"\" C:\\ios\\blocker.exe"}, wait(4),
    chord(WIN, ord("D")), wait(2), chord(ALT, F4), wait(3), {"op": "text", "text": "r"}, wait(1), key(ENTER),
    wait(5), shot("apps-preventing"),
    click((749, 756)), wait(1), shot("restarting"),
]

SHOTS = ["lock", "secure", "typed", "wrong-password", "power-menu", "accessibility", "network", "sign-in-other",
         "locked-account", "password-expired", "change-password", "password-changed",
         *[f"first-sign-in-{i}" for i in range(1, FIRST_BURST + 1)], "first-desktop", "security-options", "elevation-consent", "elevation-details",
         "elevation-unknown", "elevation-credentials", "shut-down-dialog", "shut-down-list", "signing-out-1",
         "apps-preventing", "restarting"]


def white(p):
    return min(p) >= 225


def bright(p):
    return min(p) >= 120


def near_white(p):
    return min(p) >= 235


def accent(p):
    return p[2] >= 150 and p[0] <= 40 and p[1] <= 130


def pieces(img, region, n, what, test=white, gap=12):
    got = core.ink_boxes(img, region, test, gap)
    if len(got) != n:
        raise core.Incomplete(f"{what}: {len(got)} pieces where {n} were expected: {got}")
    return got


def one(img, region, what, test, gap=12):
    return pieces(img, region, 1, what, test, gap)[0]


def largest(img, region, what, test, gap=30):
    got = core.ink_boxes(img, region, test, gap)
    if not got:
        raise core.Incomplete(f"{what}: nothing found in {region}")
    return max(got, key=lambda b: b[2] * b[3])


def union(*boxes):
    x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
    x1, y1 = max(b[0] + b[2] for b in boxes), max(b[1] + b[3] for b in boxes)
    return (x0, y0, x1 - x0, y1 - y0)


def rows_in(img, region, test, gap=12):
    """Ink grouped into rows: pieces whose vertical centres lie within 10
    pixels are one row (an icon and its name drawn apart)."""
    rows = []
    for b in core.ink_boxes(img, region, test, gap):
        same = [i for i, r in enumerate(rows) if abs((r[1] + r[3] / 2) - (b[1] + b[3] / 2)) <= 10]
        if same:
            rows[same[0]] = union(rows[same[0]], b)
        else:
            rows.append(b)
    return sorted(rows, key=lambda b: b[1])


def disc(img, cx, cy, what):
    """The account picture: a disc a shade lighter than the wallpaper, found by
    walking out from beside the glyph at its centre until the shade changes."""
    w, _, rgb = img

    def px(x, y):
        i = (y * w + x) * 3
        return rgb[i], rgb[i + 1], rgb[i + 2]
    sample = px(cx - 47, cy)

    def same(p):
        return abs(p[0] - sample[0]) <= 6 and abs(p[2] - sample[2]) <= 10

    def walk(x, y, dx, dy):
        while same(px(x, y)):
            x, y = x + dx, y + dy
        return x, y
    left, right = walk(cx - 47, cy, -1, 0)[0] + 1, walk(cx + 47, cy, 1, 0)[0] - 1
    top, bottom = walk(cx, cy - 60, 0, -1)[1] + 1, walk(cx, cy + 60, 0, 1)[1] - 1
    size = (right - left + 1, bottom - top + 1)
    if abs(size[0] - size[1]) > 2 or size[0] < 100:
        raise core.Incomplete(f"{what}: the account picture is {size[0]}x{size[1]}, not a disc")
    return (left, top, size[0], size[1])


def records(shots, build, taken=None):
    src = f"hand:{build}"
    out = []

    def rec(rid, kind, label, parent=None, **extra):
        r = {"id": f"win11.session.{rid}", "surface": SURFACE, "kind": kind, "label": label,
             "order": len(out), "source": src, "capturedFrom": build}
        if parent:
            r["parent"] = f"win11.session.{parent}"
        for k, v in extra.items():
            if k == "bounds":
                v = {"x": v[0], "y": v[1], "width": v[2], "height": v[3]}
                r["captureProfile"], r["session"] = PROFILE, "console"   # the VM framebuffer, shot from the host
            r[k] = v
        r.setdefault("disposition", "build")
        out.append(r)
        return rid

    screen = (0, 0, 1920, 1080)
    root = rec("secure", "surface", "Lock, sign-in and secure screens", bounds=screen,
               behaviour="Screens shown on the secure desktop or before a session starts; full screen over the lock wallpaper or black.")

    def corner(img, state, what):
        """The bottom right buttons of the sign-in and security screens."""
        b = pieces(img, (1700, 1000, 220, 80), 3, f"{what}: bottom right buttons", bright, 10)
        for rid, label, box in zip(("network", "accessibility", "power"), ("Network", "Accessibility", "Power"), b):
            rec(f"{state}.{rid}", "control", f"{label} button", state, controlType="button", bounds=box)

    # Lock screen.
    img = shots["lock"]
    # By rows: the gap inside the time depends on its digits.
    lines = rows_in(img, (0, 100, 1920, 300), white)
    if len(lines) != 2:
        raise core.Incomplete(f"lock: {len(lines)} lines where the time and date were expected: {lines}")
    clock, date = lines
    widgets = one(img, (700, 960, 520, 80), "lock: widgets button", bright, 14)
    status = one(img, (1800, 1000, 120, 80), "lock: status", bright)
    lock = rec("lock", "state", "Lock screen", root, bounds=screen, openedBy="Win+L",
               behaviour="Wallpaper with the time and date centred above the middle, a widgets button near the bottom and network status at the bottom right.")
    rec("lock.clock", "control", "Time", lock, controlType="text", bounds=clock,
        behaviour="Hours and minutes without seconds or AM/PM, in large light text.")
    rec("lock.date", "control", "Date", lock, controlType="text", bounds=date,
        behaviour="Weekday, month and day under the time, one line.")
    rec("lock.widgets", "control", "Widgets button", lock, controlType="button", bounds=widgets,
        behaviour="An icon and a short label on a faint pill; the bounds are its icon and label.")
    rec("lock.status", "control", "Network status", lock, controlType="image", bounds=status)
    rec("lock.dismiss", "gesture", "Dismiss lock screen", lock,
        behaviour="Any key, a click or an upward swipe slides the lock screen up and shows sign-in for the last account.")

    # Sign-in screen.
    img = shots["secure"]
    glyph, name, users = pieces(img, (0, 300, 1600, 780), 3, "sign-in: picture, name and user list")
    sign = rec("sign-in", "state", "Sign-in screen", root, bounds=screen, openedBy="Dismissing the lock screen",
               behaviour="The account picture, name and credential box centred; the other accounts at the bottom left; network, accessibility and power at the bottom right.")
    rec("sign-in.picture", "control", "Account picture", sign, controlType="image",
        bounds=disc(img, glyph[0] + glyph[2] // 2, glyph[1] + glyph[3] // 2, "sign-in"),
        behaviour="A disc a shade lighter than the wallpaper with a person glyph when the account has no picture.")
    rec("sign-in.name", "control", "Account name", sign, controlType="text", bounds=name)
    box = one(img, (700, name[1] + name[3], 520, 120), "sign-in: credential box", lambda p: p[2] < 70, 6)
    rec("sign-in.password", "control", "Password box", sign, controlType="edit", bounds=box,
        behaviour="Dark box with a placeholder and an accent underline while focused; focused when the screen shows.")
    submit = one(img, (box[0] + box[2] - 40, box[1], 40, box[3]), "sign-in: submit", bright, 6)
    rec("sign-in.submit", "control", "Submit button", sign, controlType="button", bounds=submit,
        behaviour="An arrow at the right end of the box; Enter does the same.")
    reveal = min(pieces(shots["typed"], (box[0] + box[2] - 80, box[1], 80, box[3]), 2, "typed: reveal and submit", bright, 6))
    rec("sign-in.reveal", "control", "Reveal password button", sign, controlType="button", bounds=reveal,
        behaviour="An eye left of the submit arrow, shown only once the box holds text; shows the text while held.")
    tiles = rows_in(img, users, white, 6)
    if len(tiles) != 2:
        raise core.Incomplete(f"sign-in: {len(tiles)} account rows where 2 were expected: {tiles}")
    rec("sign-in.users", "group", "Other accounts", sign, bounds=users, itemCount=2,
        behaviour="One row per account, picture then name, at the bottom left; the selected account is highlighted and shown in the middle.")
    for rid, lab, b in zip(("ref", "other"), ("First account", "Second account"), tiles):
        rec(f"sign-in.users.{rid}", "control", lab, "sign-in.users", controlType="listItem", bounds=b)
    rec("sign-in.options", "group", "Sign-in options", sign,
        behaviour="A row of method icons under the box (password, PIN, face, fingerprint, security key) shown only when the account has more than one method; a password-only local account shows none.")
    corner(img, "sign-in", "sign-in")

    def message(state, label, shotname, length, extra_buttons=()):
        img = shots[shotname]
        _, nm, text, *buttons = pieces(img, (600, 300, 720, 450), 3 + 1 + len(extra_buttons),
                                       f"{shotname}: picture, name, message and buttons")
        # The message's width must fit its length, or this is another message
        # (a mistyped change shows a different one in the same place).
        if not 5.5 <= text[2] / length <= 7.2:
            raise core.Incomplete(f"{shotname}: the message is {text[2]} px wide, not a line of {length} characters")
        st = rec(state, "state", label, sign, bounds=screen,
                 behaviour="The account picture and name stay; the box is replaced by a message and its buttons.")
        rec(f"{state}.message", "control", "Message", st, controlType="text", bounds=text,
            behaviour=f"A text slot of {length} characters, one centred line under the name.")
        for (rid, lab), b in zip((("ok", "OK button"),) + tuple(extra_buttons), buttons):
            rec(f"{state}.{rid}", "control", lab, st, controlType="button", bounds=b)
        return st

    message("wrong-password", "Wrong password", "wrong-password", 37)
    message("locked-account", "Account locked out", "locked-account", 75)
    message("password-expired", "Password must change", "password-expired", 54, (("cancel", "Cancel button"),))
    message("password-changed", "Password changed", "password-changed", 31)

    img = shots["change-password"]
    form = rec("change-password", "state", "Change password form", sign, bounds=screen,
               openedBy="OK on the expired-password message",
               behaviour="Current, new and confirm boxes in one column under the name, the current one filled; then a reset-disk link and Cancel.")
    rec("change-password.new", "control", "New password box", form, controlType="edit",
        bounds=one(img, (700, 520, 520, 200), "change-password: focused box", lambda p: p[2] < 70, 6))
    link, cancel = pieces(img, (700, 700, 520, 120), 2, "change-password: link and cancel", bright, 10)
    rec("change-password.reset-disk", "control", "Reset disk link", form, controlType="hyperlink", bounds=link)
    rec("change-password.cancel", "control", "Cancel button", form, controlType="button", bounds=cancel)

    # First sign-in: classify the burst. The greeting is one small piece of
    # text on black; the progress page is one block of two dim lines.
    greeting = progress = None
    for i in range(1, FIRST_BURST + 1):
        got = core.ink_boxes(shots[f"first-sign-in-{i}"], screen, lambda p: min(p) >= 50, 20)
        if len(got) == 1 and got[0][2] < 200 and greeting is None:
            greeting = got[0]
        elif len(got) == 1 and got[0][2] >= 300 and progress is None:
            progress = got[0]
    if greeting is None or progress is None:
        raise core.Incomplete(f"first sign-in: the burst shows no {'greeting' if greeting is None else 'progress page'}")
    first = rec("first-sign-in", "state", "First sign-in", root, bounds=screen,
                behaviour="Black, then a short greeting slot centred, then a progress page with a slot of 34 and one of 23 characters fading in on black, then the desktop with Start open.")
    rec("first-sign-in.greeting", "control", "Greeting", first, controlType="text", bounds=greeting,
        behaviour="A text slot of 3 characters, centred on black.")
    rec("first-sign-in.progress", "control", "Progress message", first, controlType="text", bounds=progress,
        behaviour="Two centred lines, the second smaller and dimmer.")

    # Security options (Ctrl+Alt+Delete).
    img = shots["security-options"]
    focus, *items, cancel = pieces(img, (700, 300, 520, 400), 6, "security options: items", bright, 10)
    sec = rec("security-options", "state", "Security options", root, bounds=screen, openedBy="Ctrl+Alt+Delete",
              behaviour="Black screen with a left-aligned list in the middle and Cancel under it; the first item has keyboard focus.")
    names = (("lock", "Lock"), ("switch-user", "Switch user"), ("sign-out", "Sign out"),
             ("change-password", "Change a password"), ("task-manager", "Task Manager"))
    for (rid, lab), b in zip(names, [focus, *items]):
        rec(f"security-options.{rid}", "menuItem", lab, sec, bounds=b)
    rec("security-options.cancel", "control", "Cancel button", sec, controlType="button", bounds=cancel)
    corner(img, "security-options", "security options")

    # The flyouts of the sign-in screen's bottom right buttons.
    img = shots["power-menu"]
    icons = pieces(img, (1790, 935, 130, 70), 2, "power menu: rows")
    menu = rec("power-menu", "menu", "Power menu", sign, itemCount=2, openedBy="Power button",
               bounds=union(*icons), behaviour="Opens above the power button; one row per action, icon then name.")
    rec("power-menu.shut-down", "menuItem", "Shut down", menu, bounds=icons[0])
    rec("power-menu.restart", "menuItem", "Restart", menu, bounds=icons[1])

    img = shots["accessibility"]
    panel = largest(img, (1500, 600, 420, 420), "accessibility: panel", lambda p: min(p) >= 30 and max(p) < 90)
    ease = rec("accessibility", "menu", "Accessibility flyout", sign, itemCount=7, openedBy="Accessibility button",
               bounds=panel, behaviour="A panel above the button listing each assistive tool with an on/off switch at the right.")
    rows = rows_in(img, (1560, panel[1], 200, panel[3]), white)
    if len(rows) != 7:
        raise core.Incomplete(f"accessibility: {len(rows)} rows where 7 were expected: {rows}")
    for i, (rid, lab) in enumerate((("magnifier", "Magnifier"), ("contrast", "Contrast themes"), ("narrator", "Narrator"),
                                    ("voice-access", "Voice access"), ("keyboard", "On-screen keyboard"),
                                    ("sticky-keys", "Sticky keys"), ("filter-keys", "Filter keys"))):
        rec(f"accessibility.{rid}", "menuItem", lab, ease, bounds=rows[i])
    rec("network", "menu", "Network flyout", sign, itemCount=1, openedBy="Network button",
        behaviour="A panel above the button with the airplane mode tile only; no network list before sign-in.")
    rec("network.airplane-mode", "menuItem", "Airplane mode", "network",
        behaviour="A toggle tile with its icon over its name.")

    # The elevation prompt.
    img = shots["elevation-consent"]
    dlg = largest(img, (600, 250, 720, 600), "consent: dialog", lambda p: min(p) >= 200)
    no = one(img, (dlg[0], dlg[1] + dlg[3] - 80, dlg[2], 80), "consent: default button", accent, 6)
    yes = largest(img, (dlg[0], no[1] - 2, no[0] - dlg[0] - 4, no[3] + 4), "consent: other button", lambda p: min(p) >= 250, 4)
    elev = rec("elevation", "dialog", "Elevation prompt", root, bounds=dlg,
               openedBy="Run as administrator",
               behaviour="Centred over a dimmed copy of the desktop on the secure desktop; a header band, the program's icon and name, its publisher, a details link, then two buttons.")
    rec("elevation.header", "region", "Header band", elev,
        behaviour="Light neutral band for a verified publisher, a caution yellow band for an unknown one; holds the title and question.")
    rec("elevation.yes", "control", "Yes button", elev, controlType="button", bounds=yes)
    rec("elevation.no", "control", "No button", elev, controlType="button", bounds=no,
        behaviour="Right of Yes, filled with the accent and focused, so Enter declines.")
    rec("elevation.details", "control", "Details expander", elev, controlType="hyperlink",
        behaviour="A link under the publisher that expands the program path and certificate details in place.")
    rec("elevation.dim", "gesture", "Dimmed desktop", elev,
        behaviour="Everything behind the prompt is a still, darkened copy of the desktop; nothing behind it takes input.")
    rec("elevation-details", "state", "Details shown", elev,
        bounds=largest(shots["elevation-details"], (500, 100, 920, 880), "details: dialog", lambda p: min(p) >= 200))
    rec("elevation-unknown", "state", "Unknown publisher", elev,
        bounds=largest(shots["elevation-unknown"], (600, 250, 720, 600), "unknown: dialog", lambda p: min(p) >= 200),
        behaviour="The header band turns caution yellow and the publisher line reads unknown with the file's origin under it.")
    img = shots["elevation-credentials"]
    cdlg = largest(img, (600, 100, 720, 900), "credentials: dialog", lambda p: min(p) >= 200)
    cred = rec("elevation-credentials", "state", "Credentials form", elev, bounds=cdlg,
               behaviour="For a standard account: under the details link, an instruction slot, an administrator's name, a password box and the machine name; Yes takes the accent.")
    rec("elevation-credentials.yes", "control", "Yes button", cred, controlType="button",
        bounds=one(img, (cdlg[0], cdlg[1] + cdlg[3] - 80, cdlg[2], 80), "credentials: accent button", accent, 6))

    # The shut down dialog.
    img = shots["shut-down-dialog"]
    sdd = largest(img, (300, 100, 1320, 800), "shut down: dialog", near_white)
    sd = rec("shut-down", "dialog", "Shut down dialog", root, bounds=sdd, openedBy="Alt+F4 on the desktop",
             behaviour="A small window with a banner, a question, a drop-down of actions with a one-line description under it, then OK, Cancel and Help.")
    # Open, the list shows two wide accent shapes: the focused field and the
    # highlighted row under it.
    wide = [b for b in core.ink_boxes(shots["shut-down-list"], (sdd[0], sdd[1], sdd[2], sdd[3] + 200), accent, 3) if b[2] >= 300]
    if len(wide) != 2:
        raise core.Incomplete(f"shut down list: {len(wide)} wide accent shapes where 2 were expected: {wide}")
    rec("shut-down.list", "control", "Action list", sd, controlType="comboBox", itemCount=4, bounds=wide[0],
        behaviour="Switch user, Sign out, Shut down and Restart; opens on Shut down; the first letter selects.")
    for rid, lab in (("switch-user", "Switch user"), ("sign-out", "Sign out"), ("shut-down", "Shut down"), ("restart", "Restart")):
        rec(f"shut-down.list.{rid}", "menuItem", lab, "shut-down.list", **({"bounds": wide[1]} if rid == "shut-down" else {}))

    # Apps preventing restart, and the progress screens.
    img = shots["apps-preventing"]
    title = largest(img, (500, 200, 920, 200), "apps preventing: title", white, 12)
    restart_anyway, cancel = sorted(pieces(img, (600, 700, 600, 100), 2, "apps preventing: buttons", lambda p: min(p) >= 40, 4))
    ap = rec("apps-preventing", "state", "Apps preventing restart", root, bounds=screen,
             behaviour="Black screen: a title slot that counts the programs (28 characters for one) and a line of 72, a list of the programs with icon, name and reason, then Restart anyway and Cancel at the bottom left.")
    rec("apps-preventing.title", "control", "Title", ap, controlType="text", bounds=title)
    rec("apps-preventing.restart-anyway", "control", "Restart anyway button", ap, controlType="button", bounds=restart_anyway)
    rec("apps-preventing.cancel", "control", "Cancel button", ap, controlType="button", bounds=cancel)
    for rid, label, shotname, length in (("signing-out", "Signing out", "signing-out-1", 11), ("restarting", "Restarting", "restarting", 10)):
        img = shots[shotname]
        # The text is the lowest piece; the spinner is every dot above it,
        # however far apart its animation has spread them.
        got = core.ink_boxes(img, (700, 400, 520, 300), white, 12)
        if len(got) < 2:
            raise core.Incomplete(f"{shotname}: {len(got)} pieces where a spinner and its text were expected: {got}")
        *dots, text = sorted(got, key=lambda b: b[1] + b[3])
        spin = union(*dots)
        st = rec(rid, "state", label, root, bounds=screen,
                 behaviour=f"Black screen with a dotted spinner and a slot of {length} characters under it, centred.")
        rec(f"{rid}.spinner", "control", "Spinner", st, controlType="progressBar", bounds=spin)
        rec(f"{rid}.text", "control", "Status text", st, controlType="text", bounds=text)
    return out
