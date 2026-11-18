"""Boot, boot manager, recovery, Safe mode and stop error screens, captured from the host.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9.

    python3 scripts/inventory/host-capture.py --scenario session-boot

None of these screens has a desktop session to read with UI Automation. The run
adds a second boot entry (a copy of the current one with safeboot minimal), so
the boot manager shows its menu, and turns on a stop error for a host NMI; then
it restarts and walks: the boot logo, the boot manager menu and its Options
page, the recovery environment (Choose an option, Troubleshoot, Reset this PC,
Advanced options), Startup Settings and its numbered restart menu, Safe mode
by its number, a stop error raised by an NMI from the host, and, from the menu
that restart brings back, a Startup Repair that ends on the repair-failed
screen and More recovery options. Recovery takes keys only: arrows move
the focus, Enter takes it, Backspace goes back a page (Esc does nothing).

The boot logo is drawn in the firmware's own mode (1024x768 on this VM), so its
records carry positions as fractions of the screen and no fhd-100 bounds. Every
screen is a hand record measured from the shots; text is recorded as slots.
"""
import importlib.util
import pathlib
import sys
from datetime import datetime


def _core():
    if "capture_core" in sys.modules:
        return sys.modules["capture_core"]
    spec = importlib.util.spec_from_file_location("capture_core", pathlib.Path(__file__).resolve().parents[1] / "capture_core.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


core = _core()

SURFACE = "session"
FILE = "boot"
PROFILE = "fhd-100"
CHECKPOINT = "clean"
TIMEOUT = 1800

ENTER, TAB, BACK, UP, DOWN, LEFT, RIGHT, FOUR = 13, 9, 8, 38, 40, 37, 39, 52
BOOT_BURST, REPAIR_BURST, SAFE_BURST, STOP_BURST = 14, 12, 8, 10

# A second entry so the boot manager shows its menu, and a stop error on NMI.
FIXTURE = (
    r'$o = bcdedit /copy "{current}" /d "Safe Mode"; '
    r'$g = [regex]::Match(($o -join " "), "\{[0-9a-fA-F-]+\}").Value; '
    r'if (-not $g) { throw "bcdedit /copy gave no identifier: $o" }; '
    r'bcdedit /set $g safeboot minimal | Out-Null; bcdedit /displayorder "{current}" $g | Out-Null; '
    r'New-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl -Name NMICrashDump -Value 1 -PropertyType DWord -Force | Out-Null; '
    r'Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl -Name CrashDumpEnabled -Value 3; '
    r'shutdown.exe /r /t 3'
)


def key(vk):
    return {"op": "key", "vk": vk}


def wait(sec):
    return {"op": "wait", "sec": sec}


def shot(name):
    return {"op": "shot", "name": name}


def keys(*vks):
    return [s for vk in vks for s in (key(vk), wait(1))]


# The boot manager menu, reached again after the stop error restarts.
TO_RECOVERY = [*keys(DOWN, DOWN, TAB), key(ENTER), wait(5), *keys(DOWN, DOWN), key(ENTER), wait(18)]

# The guest's own screenshot of the desktop, taken just before a host shot of
# it: host-capture.py checks the two match pixel for pixel (PROOF).
GUEST_SHOT = (r'powershell.exe -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms,System.Drawing; '
              r'$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds; $m = New-Object System.Drawing.Bitmap $b.Width, $b.Height; '
              r'$g = [System.Drawing.Graphics]::FromImage($m); $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size); '
              r"$m.Save('C:\ios\out\guest-desktop.png', [System.Drawing.Imaging.ImageFormat]::Png)" + '"')
PROOF = ("desktop", "guest-desktop.png")

STEPS = [
    {"op": "guest", "cmd": GUEST_SHOT}, shot("desktop"),
    {"op": "fetch", "path": r"C:\ios\out\guest-desktop.png", "name": "guest-desktop.png"},
    {"op": "admin", "script": FIXTURE},
    # Restarting, the firmware logo with the spinner, then the menu counting down.
    {"op": "burst", "name": "boot", "count": BOOT_BURST, "every": 0},
    shot("boot-manager"),
    *keys(DOWN), shot("boot-manager-second"),
    *keys(DOWN, TAB), shot("boot-manager-link"),
    key(ENTER), wait(5), shot("boot-options"),
    # Choose other options restarts into the recovery environment.
    *keys(DOWN, DOWN), key(ENTER), wait(18), shot("recovery"),
    *keys(RIGHT), key(ENTER), wait(4), shot("recovery-troubleshoot"),
    *keys(DOWN), key(ENTER), wait(6), shot("recovery-reset"), key(BACK), wait(4),
    *keys(UP, RIGHT), key(ENTER), wait(4), shot("recovery-advanced"),
    # Startup Settings shows its Restart page, then the numbered menu after a restart.
    *keys(DOWN), key(ENTER), wait(4), shot("recovery-startup-settings"),
    key(ENTER), wait(30), shot("startup-settings"),
    key(FOUR), {"op": "burst", "name": "safe-boot", "count": SAFE_BURST, "every": 2},
    {"op": "settle", "sec": 5, "timeout": 240}, shot("safe-mode"),
    # An NMI from the host stops Windows; it writes its dump and restarts to the menu.
    {"op": "nmi"}, {"op": "burst", "name": "stop", "count": STOP_BURST, "every": 0}, wait(3),
    # Back into recovery for Startup Repair, which looks for the network and gives up.
    *TO_RECOVERY, *keys(RIGHT), key(ENTER), wait(4), *keys(RIGHT), key(ENTER), wait(4),
    key(ENTER), {"op": "burst", "name": "repair", "count": REPAIR_BURST, "every": 3}, wait(30), shot("automatic-repair"),
    # After a failed repair the first page is More recovery options: Continue,
    # Quick machine recovery and Troubleshoot down the first column, Turn off
    # your PC beside Continue.
    key(ENTER), wait(8), shot("recovery-more"),
]

# Bursts across a restart: a shot there may straddle a mode change.
MOVING = ("boot-", "repair-", "safe-boot-", "stop-")

SHOTS = [*[f"boot-{i}" for i in range(1, BOOT_BURST + 1)], "boot-manager", "boot-manager-second", "boot-manager-link",
         "boot-options", "recovery", "recovery-troubleshoot", "recovery-reset", "recovery-advanced",
         "recovery-startup-settings", "startup-settings", *[f"safe-boot-{i}" for i in range(1, SAFE_BURST + 1)], "safe-mode",
         *[f"stop-{i}" for i in range(1, STOP_BURST + 1)],
         *[f"repair-{i}" for i in range(1, REPAIR_BURST + 1)], "automatic-repair", "recovery-more"]


SCREEN = (0, 0, 1920, 1080)
BLUE = (33, 101, 181)         # the boot manager's and recovery's background
TILE = (74, 166, 222)         # the square behind each choice's icon


def lit(p):
    return max(p) >= 100


def white(p):
    return min(p) >= 200


def tile_fill(p):
    return p == TILE


def pieces(img, region, n, what, test, gap=12):
    got = core.ink_boxes(img, region, test, gap)
    if len(got) != n:
        raise core.Incomplete(f"{what}: {len(got)} pieces where {n} were expected: {got}")
    return got


def one(img, region, what, test, gap=12):
    return pieces(img, region, 1, what, test, gap)[0]


def union(*boxes):
    x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
    x1, y1 = max(b[0] + b[2] for b in boxes), max(b[1] + b[3] for b in boxes)
    return (x0, y0, x1 - x0, y1 - y0)


def rows_in(img, region, test, gap=12):
    """Ink grouped into rows by vertical centre (within 8 pixels)."""
    rows = []
    for b in core.ink_boxes(img, region, test, gap):
        same = [i for i, r in enumerate(rows) if abs((r[1] + r[3] / 2) - (b[1] + b[3] / 2)) <= 8]
        if same:
            rows[same[0]] = union(rows[same[0]], b)
        else:
            rows.append(b)
    return sorted(rows, key=lambda b: (b[1], b[0]))


def pixel(img, x, y):
    w, _, data = img
    i = (y * w + x) * 3
    return tuple(data[i:i + 3])


def body(img):
    """The whole screen but its first row, where Hyper-V's thumbnail can carry
    a stray pixel at the left end."""
    return (0, 1, img[0], img[1] - 1)


def place(img, b):
    """Where a box sits on a screen whose mode is not the profile's, in
    fractions of that screen: its centre across and down."""
    return f"centred at {(b[0] + b[2] / 2) / img[0]:.0%} across and {(b[1] + b[3] / 2) / img[1]:.0%} down"


def stamp(entry):
    return datetime.fromisoformat(entry["at"])


def records(shots, build, taken=None):
    src = f"hand:{build}"
    out = []
    taken = taken or {}

    def rec(rid, kind, label, parent=None, **extra):
        r = {"id": f"win11.session.boot{'.' + rid if rid else ''}", "surface": SURFACE, "kind": kind, "label": label,
             "order": len(out), "source": src, "capturedFrom": build}
        if parent is not None:
            r["parent"] = f"win11.session.boot{'.' + parent if parent else ''}"
        for k, v in extra.items():
            if k == "bounds":
                v = {"x": v[0], "y": v[1], "width": v[2], "height": v[3]}
                r["captureProfile"], r["session"] = PROFILE, "console"
            r[k] = v
        if kind not in ("shortcut", "metric", "state", "gesture"):
            r.setdefault("disposition", "build")
        out.append(r)
        return rid

    def full(name):
        img = shots[name]
        if img[:2] != (1920, 1080):
            raise core.Incomplete(f"{name}: {img[0]}x{img[1]}, not the profile's 1920x1080; no bounds are taken from it")
        return img

    def firmware(name):
        img = shots[name]
        if img[:2] == (1920, 1080):
            raise core.Incomplete(f"{name}: drawn at 1920x1080, where the firmware's own mode was expected")
        return img

    def frames(prefix, test, exact=True):
        """The burst frames whose screen passes test, in order. Geometry is
        measured only from exact frames (the mode held across the shot); a
        frame taken while the mode changed is skipped unless `exact` is False,
        for a caller that reads only its time."""
        names = sorted((n for n in shots if n.startswith(prefix + "-") and n[len(prefix) + 1:].isdigit()),
                       key=lambda n: int(n[len(prefix) + 1:]))
        return [n for n in names if (not exact or taken.get(n, {}).get("exact")) and test(shots[n])]

    root = rec("", "surface", "Boot, recovery and stop screens", bounds=SCREEN,
               behaviour="Screens drawn before Windows starts, while it restarts or stops, and by the recovery environment; full screen, no desktop session.")

    # Restarting: a spinner over a one-word label on black, at the display's mode.
    def restarting(img):
        return img[:2] == (1920, 1080) and pixel(img, 10, 10) == (0, 0, 0) and len(rows_in(img, body(img), lit)) == 2
    got = frames("boot", restarting)
    if not got:
        raise core.Incomplete("boot burst: no Restarting frame (a spinner over a label on black)")
    img = shots[got[0]]
    spin, word = rows_in(img, body(img), lit)
    st = rec("restarting", "state", "Restarting screen", root, bounds=SCREEN, openedBy="Restart",
             behaviour="Black after the session closes: a dotted spinner with a one-word label under it, both centred, until the firmware takes over.")
    rec("restarting.spinner", "control", "Spinner", st, controlType="progressBar", bounds=spin,
        behaviour="Dots running round a circle, centred above the middle of the screen.")
    rec("restarting.label", "control", "Label", st, controlType="text", bounds=word,
        behaviour="A text slot of 10 characters in small bold white, centred under the spinner.")

    # The firmware's logo with Windows' spinner, in the firmware's own mode.
    def logo(img):
        return img[:2] != (1920, 1080) and len(core.ink_boxes(img, body(img), lit, 10)) == 2
    got = frames("boot", logo)
    if not got:
        raise core.Incomplete("boot burst: no frame with the firmware logo and the spinner")
    img = firmware(got[0])
    mark, spin = sorted(core.ink_boxes(img, body(img), lit, 10), key=lambda b: b[1])
    st = rec("logo", "state", "Boot logo", root, openedBy="Power on or restart",
             behaviour=f"Black in the firmware's own mode ({img[0]}x{img[1]} on the reference machine), the firmware's logo and Windows' spinner under it; positions are fractions of that screen, which no profile governs.")
    rec("logo.firmware", "control", "Firmware logo", st, controlType="image",
        behaviour=f"The logo the firmware publishes in its boot graphics table, drawn as it is, {place(img, mark)}; {mark[2] / img[0]:.0%} of the screen wide.")
    every = frames("boot", lambda i: True)
    alone = [n for n in every[every.index(got[0]) + 1:]
             if shots[n][:2] == (1920, 1080) and len(core.ink_boxes(shots[n], body(shots[n]), lit, 10)) == 1]
    if not alone:
        raise core.Incomplete("boot burst: no frame with the spinner alone after the logo")
    lone = core.ink_boxes(shots[alone[0]], body(shots[alone[0]]), lit, 10)[0]
    rec("logo.spinner", "control", "Spinner", st, controlType="progressBar",
        behaviour=f"A ring of dots {place(img, spin)}, below the logo; it keeps turning alone, at the same place, once Windows sets the display's own mode ({place(shots[alone[0]], lone)} at 1920x1080).")

    # Tile pages: the boot manager's and the recovery environment's.
    def tiles(img, what, n):
        got = [b for b in core.ink_boxes(img, (0, 120, img[0], img[1] - 120), tile_fill, 4) if 100 <= b[2] <= 120 and 100 <= b[3] <= 120]
        if len(got) != n:
            raise core.Incomplete(f"{what}: {len(got)} choice tiles where {n} were expected: {got}")
        return sorted(got, key=lambda b: (b[0] // 200, b[1]))

    def ring(img, t):
        """The focus rectangle around tile t: (x, y, w, h), or None when unfocused."""
        x, y = t[0] - 12, t[1] - 12
        if not white(pixel(img, x, t[1] + t[3] // 2)):
            return None
        w = next(i for i in range(img[0] - x) if not white(pixel(img, x + i, y)))
        h = next(i for i in range(img[1] - y) if not white(pixel(img, x, y + i)))
        return (x, y, w, h)

    size = {}

    def tile_page(rid, label, name, choices, parent, *, back, opened, behaviour, title_band=(40, 90)):
        img = full(name)
        ts = tiles(img, name, len(choices))
        title = union(*core.ink_boxes(img, (170, title_band[0], 1700, title_band[1]), white, 14))
        arrow = core.ink_boxes(img, (90, 50, 70, 70), white, 6)
        if bool(arrow) != back:
            raise core.Incomplete(f"{name}: back button {'missing' if back else 'shown'}")
        rings = [r for r in (ring(img, t) for t in ts) if r]
        if len(rings) > 1:
            raise core.Incomplete(f"{name}: {len(rings)} focus rectangles")
        if rings:
            size.setdefault("ring", rings[0][2:])
            if rings[0][2:] != size["ring"]:
                raise core.Incomplete(f"{name}: focus rectangle {rings[0][2:]}, not {size['ring']} as on the other pages")
        page = rec(rid, "page", label, parent, bounds=SCREEN, openedBy=opened, behaviour=behaviour)
        if back:
            rec(f"{rid}.back", "control", "Back button", page, controlType="button", bounds=arrow[0],
                behaviour="A circled arrow left of the title; Backspace does the same.")
        rec(f"{rid}.title", "control", "Title", page, controlType="text", bounds=title)
        w, h = size.get("ring", (404, 134))
        # The group encloses its buttons (tile, name and focus rectangle), not
        # only the icon squares (review, 2026-11-18).
        rec(f"{rid}.choices", "group", "Choices", page, bounds=union(*((t[0] - 12, t[1] - 12, w, h) for t in ts)), itemCount=len(choices),
            behaviour="Tiles in columns of three, read down the first column then the next; arrow keys move the focus, Enter takes it.")
        for (cid, clabel, disp, reason), t in zip(choices, ts):
            extra = {"disposition": disp} if disp else {}
            if reason:
                extra["reason"] = reason
            rows = rows_in(img, (t[0] + 121, t[1] - 4, w - 133, t[3] + 4), white, 10)
            if not rows:
                raise core.Incomplete(f"{name}: no name beside the {clabel} tile")
            rec(f"{rid}.{cid}", "control", clabel, f"{rid}.choices", controlType="button", bounds=(t[0] - 12, t[1] - 12, w, h),
                behaviour=f"A {t[2]} px square of the lighter blue with a white line icon, the name beside it at its top{' and a description under it' if len(rows) > 1 else ''}; focus draws a white rectangle round tile and text.",
                **extra)
        return img, page, ts, rings

    # Boot manager menu.
    img, bm, ts, rings = tile_page("boot-manager", "Boot manager menu", "boot-manager",
                                   (("first", "First entry", None, None), ("second", "Second entry", None, None)), root, back=False,
                                   opened="Power on, when the boot manager lists more than one entry",
                                   behaviour="Blue, the title at the top left, a countdown line, one tile per boot entry named by its description, and a link to the options at the bottom.")
    if not rings or rings[0][:2] != (ts[0][0] - 12, ts[0][1] - 12):
        raise core.Incomplete("boot-manager: the default entry is not the focused one")
    title = union(*core.ink_boxes(img, (170, 40, 1700, 90), white, 14))
    count = one(img, (170, title[1] + title[3] + 10, 1000, ts[0][1] - 12 - title[1] - title[3] - 20), "boot-manager: countdown", white, 16)
    rec("boot-manager.countdown", "control", "Countdown", bm, controlType="text", bounds=count,
        behaviour="A text slot naming the default entry and the seconds left, counting down from the boot manager's timeout (30 in this walk).")
    link = one(img, (170, ts[-1][1] + ts[-1][3] + 20, 1000, 400), "boot-manager: options link", white, 16)
    rec("boot-manager.options", "control", "Change defaults or choose other options", bm, controlType="hyperlink", bounds=link,
        behaviour="A text link near the bottom left; Tab focuses it with a dotted rectangle, Enter opens the options page.")
    second = full("boot-manager-second")
    if core.ink_boxes(second, count, white, 10):
        raise core.Incomplete("boot-manager-second: the countdown is still shown after a key")
    if ring(second, ts[1]) is None or ring(second, ts[0]) is not None:
        raise core.Incomplete("boot-manager-second: Down did not move the focus to the second entry")
    rec("boot-manager.stop-countdown", "gesture", "Stop the countdown", bm,
        behaviour="A key press (Down in this walk) removes the countdown line; the menu then waits for a choice.")
    linked = full("boot-manager-link")
    if ring(linked, ts[0]) or ring(linked, ts[1]):
        raise core.Incomplete("boot-manager-link: a tile still has the focus after Tab")
    for vk, lab, beh in (("Up, Down", "Move between entries", "Arrow keys move the focus up and down the tiles."),
                         ("Tab", "Focus the options link", "Tab moves the focus from the tiles to the link and back."),
                         ("Enter", "Start the focused entry", "Enter starts the focused entry or opens the focused link.")):
        rec(f"boot-manager.{lab.lower().replace(' ', '-')}", "shortcut", lab, bm, shortcut=vk, behaviour=beh)

    tile_page("boot-options", "Boot manager options", "boot-options",
              (("timer", "Change the timer", None, None), ("default", "Choose a default operating system", None, None),
               ("other", "Choose other options", None, None)),
              root, back=True, opened="Change defaults or choose other options",
              behaviour="The same blue page with a back button: each tile names a setting and its current value under it.")

    rx = "Choose other options on the boot manager's options page, which restarts into the recovery environment"
    tile_page("recovery", "Choose an option", "recovery",
              (("continue", "Continue", None, None), ("device", "Use a device", None, None), ("other-os", "Use another operating system", None, None),
               ("troubleshoot", "Troubleshoot", None, None), ("off", "Turn off your PC", None, None)),
              root, back=True, opened=rx,
              behaviour="The recovery environment's first page, in the same tile layout as the boot manager; this walk reaches it through the boot manager's options page.")
    tile_page("recovery-troubleshoot", "Troubleshoot", "recovery-troubleshoot",
              (("restore", "Point-in-time restore", None, None), ("reset", "Reset this PC", None, None),
               ("cloud", "Cloud rebuild", "excluded", "It reinstalls Windows from Microsoft's cloud; Impossible OS reinstalls from its own recovery image (Reset this PC)."),
               ("advanced", "Advanced options", None, None)),
              "recovery", back=True, opened="Troubleshoot",
              behaviour="Tiles for restoring, resetting and the advanced tools.")
    img, rp, ts, _ = tile_page("recovery-reset", "Reset this PC", "recovery-reset",
                               (("keep", "Keep my files", None, None), ("remove", "Remove everything", None, None)),
                               "recovery-troubleshoot", back=True, opened="Reset this PC",
                               behaviour="A subtitle asks for a choice; two tiles with an arrow icon, each with a line on what it keeps.")
    sub = one(img, (170, 130, 1000, ts[0][1] - 12 - 130), "recovery-reset: subtitle", white, 16)
    rec("recovery-reset.subtitle", "control", "Subtitle", rp, controlType="text", bounds=sub)
    img, ap, ts, _ = tile_page("recovery-advanced", "Advanced options", "recovery-advanced",
                               (("repair", "Startup Repair", None, None), ("settings", "Startup Settings", None, None), ("prompt", "Command Prompt", None, None),
                                ("uninstall", "Uninstall Updates", None, None), ("firmware", "UEFI Firmware Settings", None, None), ("restore", "System Restore", None, None)),
                               "recovery-troubleshoot", back=True, opened="Advanced options",
                               behaviour="Six tiles for the repair tools, the startup settings, a command prompt, removing updates, the firmware's setup and a restore point.")
    more = one(img, (170, ts[2][1] + ts[2][3] + 20, 1000, 200), "recovery-advanced: more link", white, 16)
    rec("recovery-advanced.more", "control", "See more recovery options", ap, controlType="hyperlink", bounds=more)

    # Startup Settings: what the restart changes, then the numbered menu.
    img = full("recovery-startup-settings")
    arrow = one(img, (90, 50, 70, 70), "recovery-startup-settings: back", white, 6)
    title = union(*core.ink_boxes(img, (170, 40, 1700, 90), white, 14))
    bullets = [b for b in core.ink_boxes(img, (170, 200, 30, 300), white, 2) if b[2] <= 6 and b[3] <= 6]
    button = one(img, (700, 380, 400, 120), "recovery-startup-settings: Restart button", white, 6)
    page = rec("recovery-startup-settings", "page", "Startup Settings", "recovery-advanced", bounds=SCREEN, openedBy="Startup Settings",
               behaviour="An intro line and a bulleted list of what the restart lets you change, then a Restart button; no tiles.")
    rec("recovery-startup-settings.back", "control", "Back button", page, controlType="button", bounds=arrow)
    rec("recovery-startup-settings.title", "control", "Title", page, controlType="text", bounds=title)
    effects = ("Low-resolution video", "Debugging", "Boot logging", "Safe Mode", "Driver signature enforcement off",
               "Early-launch anti-malware off", "Automatic restart on failure off")
    if len(bullets) != len(effects):
        raise core.Incomplete(f"recovery-startup-settings: {len(bullets)} bullets where {len(effects)} were expected")
    rows = [union(b, *core.ink_boxes(img, (b[0] + 10, b[1] - 8, 700, 20), white, 10)) for b in bullets]
    # The group encloses its rows, not only the bullet dots (review, 2026-11-18).
    rec("recovery-startup-settings.list", "group", "What it changes", page, bounds=union(*rows), itemCount=len(bullets),
        behaviour="Small bullets with one line each, from low-resolution video to automatic restart after a failure.")
    for i, (lab, row) in enumerate(zip(effects, rows), 1):
        rec(f"recovery-startup-settings.list.{i}", "control", lab, "recovery-startup-settings.list", controlType="listItem", bounds=row)
    rec("recovery-startup-settings.restart", "control", "Restart", page, controlType="button", bounds=button,
        behaviour="A small outlined button; Enter restarts into the numbered menu.")

    img = firmware("startup-settings")
    lines = rows_in(img, body(img), white, 4)
    numbered = [r for r in lines if r[0] < 200 and img[1] * 0.27 <= r[1] <= img[1] * 0.66]
    if len(numbered) != 9:
        raise core.Incomplete(f"startup-settings: {len(numbered)} numbered lines where 9 were expected: {numbered}")
    page = rec("startup-settings", "page", "Startup Settings menu", "recovery-startup-settings", openedBy="Restart on the Startup Settings page",
               behaviour=f"A text menu on the recovery blue in the firmware's mode ({img[0]}x{img[1]}): title, an instruction, nine numbered lines {place(img, union(*numbered))}, and two footer lines; positions are fractions of that screen.")
    rec("startup-settings.options", "group", "Numbered options", page, itemCount=9,
        behaviour="One line each, the number first; the number or function key of that line takes it.")
    for i, lab in enumerate(("Enable debugging", "Enable boot logging", "Enable low-resolution video", "Enable Safe Mode",
                             "Enable Safe Mode with Networking", "Enable Safe Mode with Command Prompt",
                             "Disable driver signature enforcement", "Disable early launch anti-malware protection",
                             "Disable automatic restart after failure"), 1):
        rec(f"startup-settings.options.{i}", "control", lab, "startup-settings.options", controlType="listItem",
            behaviour=f"Line {i}, {place(img, numbered[i - 1])}.")
    rec("startup-settings.choose", "shortcut", "Choose an option", page, shortcut="1-9, F1-F9",
        behaviour="The page's hint line names the number keys and F1 to F9; 4 started Safe mode in this walk.")
    rec("startup-settings.more", "shortcut", "More options", page, shortcut="F10, 0",
        behaviour="Named by the first footer line, for more options; not taken in this walk.")
    rec("startup-settings.return", "shortcut", "Return to Windows", page, shortcut="Enter",
        behaviour="Named by the second footer line, to return to the operating system; not taken in this walk.")

    # Safe mode desktop: the marks Windows draws over a black desktop.
    img = firmware("safe-mode")
    bar = next(y for y in range(img[1] - 1, 0, -1) if pixel(img, img[0] // 2, y) == (0, 0, 0))
    marks = [one(img, (0, 1, 120, 16), "safe-mode: top left mark", white, 6), one(img, (img[0] - 120, 1, 120, 16), "safe-mode: top right mark", white, 6),
             one(img, (0, bar - 16, 120, 16), "safe-mode: bottom left mark", white, 6), one(img, (img[0] - 120, bar - 16, 120, 16), "safe-mode: bottom right mark", white, 6)]
    build_line = union(*core.ink_boxes(img, (200, 1, img[0] - 400, 16), white, 6))
    st = rec("safe-mode", "state", "Safe mode desktop", root, openedBy="4 on the Startup Settings menu",
             behaviour=f"The desktop on black with the basic display driver ({img[0]}x{img[1]} on the reference machine), after sign-in; positions are fractions of that screen.")
    rec("safe-mode.marks", "group", "Safe Mode marks", st, itemCount=4,
        behaviour=f"The two words in small white text in each corner, the bottom pair resting on the taskbar ({marks[0][3]} px tall at this mode), over the wallpaper.")
    for (cid, lab), m in zip((("top-left", "Top left mark"), ("top-right", "Top right mark"),
                              ("bottom-left", "Bottom left mark"), ("bottom-right", "Bottom right mark")), marks):
        rec(f"safe-mode.marks.{cid}", "control", lab, "safe-mode.marks", controlType="text",
            behaviour=f"Flush with the screen's {cid.replace('-', ' ')} corner{', on the taskbar' if cid.startswith('bottom') else ''}; {m[2]} by {m[3]} px at {img[0]}x{img[1]}.")
    rec("safe-mode.build", "control", "Build line", st, controlType="text",
        behaviour=f"The product name and full build string in one small white line {place(img, build_line)}, at the top edge.")

    # Stop error: raised by an NMI from the host.
    def stopped(img):
        return img[:2] != (1920, 1080) and len(rows_in(img, body(img), white, 10)) == 3
    got = frames("stop", stopped)
    if not got:
        raise core.Incomplete("stop burst: no stop error frame (three text lines on black)")
    img = shots[got[0]]
    message, progress, code = rows_in(img, body(img), white, 10)
    st = rec("stop", "state", "Stop error screen", root, openedBy="A stop error (here a non-maskable interrupt from the host)",
             behaviour=f"Black in the firmware's mode ({img[0]}x{img[1]} here), no icon or code image: a message, a progress line and a stop code line; the machine restarted by itself within seconds.")
    rec("stop.message", "control", "Message", st, controlType="text",
        behaviour=f"A text slot of 52 characters in one line, {place(img, message)}.")
    rec("stop.progress", "control", "Progress", st, controlType="text",
        behaviour=f"A text slot of a percentage and one word, {place(img, progress)}; it read 0% in every frame before the restart.")
    rec("stop.code", "control", "Stop code", st, controlType="text",
        behaviour=f"A small text slot with a label, the stop code's name and its number in hex, {place(img, code)}, near the bottom edge.")
    every = frames("stop", lambda i: True, exact=False)
    after = every[every.index(got[-1]) + 1:]
    if not after or got[0] not in taken or after[0] not in taken:
        raise core.Incomplete("stop burst: no frame after the stop error to time it by")
    shown = round((stamp(taken[after[0]]) - stamp(taken[got[0]])).total_seconds() * 1000)
    rec("stop.shown", "metric", "Stop error shown for", st, measure={"value": shown, "unit": "ms"},
        behaviour="From the first frame with the stop error to the first frame after it, from frames about 2.5 s apart, with the small memory dump this walk sets.")

    # Startup Repair: progress, then the failure screen and More recovery options.
    def waiting(img):
        return img[:2] != (1920, 1080) and len(core.ink_boxes(img, body(img), lit, 10)) == 2
    got = frames("repair", waiting)
    if not got:
        raise core.Incomplete("repair burst: no Please wait frame (the logo and a label)")
    img = shots[got[0]]
    mark, label = sorted(core.ink_boxes(img, body(img), lit, 10), key=lambda b: b[1])
    rec("repair-progress", "state", "Startup Repair progress", "recovery-advanced", openedBy="Startup Repair",
        behaviour=f"A restart to the firmware's logo with a short label {place(img, label)}, then a spinner with a label for diagnosing, on black.")
    img = full("automatic-repair")
    rows = rows_in(img, body(img), lit, 14)
    if len(rows) != 3:
        raise core.Incomplete(f"automatic-repair: {len(rows)} text rows where 3 were expected: {rows}")
    page = rec("automatic-repair", "page", "Repair failed", "repair-progress", bounds=SCREEN, openedBy="Startup Repair, when it finds nothing to fix",
               behaviour="Black, centred text: what happened, how to go on, and two small grey lines with the reason and the log file's path.")
    for (cid, clab, beh), b in zip((("message", "Message", "A text slot of 56 characters in one line."),
                                     ("instruction", "Instruction", "A text slot of 51 characters naming Enter and a click."),
                                     ("details", "Details", "Two small grey lines: the reason and the log file's path.")), rows):
        rec(f"automatic-repair.{cid}", "control", clab, page, controlType="text", bounds=b, behaviour=beh)
    rec("automatic-repair.go-on", "shortcut", "Show other recovery options", page, shortcut="Enter",
        behaviour="Enter or a click opens More recovery options.")
    tile_page("recovery-more", "More recovery options", "recovery-more",
              (("continue", "Continue", None, None),
               ("quick", "Quick machine recovery", "excluded", "It looks for a fix on Microsoft's servers; Impossible OS has no such service and recovers from its own image."),
               ("troubleshoot", "Troubleshoot", None, None), ("off", "Turn off your PC", None, None)),
              "automatic-repair", back=True, opened="Enter on the repair failed screen",
              behaviour="The recovery environment's first page after a failed repair: a cloud recovery tile replaces Use a device and Use another operating system, and Turn off sits beside Continue.")
    # A group's bounds enclose its members' (review, 2026-11-18: the choices
    # once covered only the icon squares).
    box = {r["id"]: r["bounds"] for r in out if "bounds" in r}
    for r in out:
        g = box.get(r.get("parent"))
        if g and "bounds" in r and next(p for p in out if p["id"] == r["parent"])["kind"] == "group":
            b = r["bounds"]
            if b["x"] < g["x"] or b["y"] < g["y"] or b["x"] + b["width"] > g["x"] + g["width"] or b["y"] + b["height"] > g["y"] + g["height"]:
                raise core.Incomplete(f"{r['id']}: outside its group {r['parent']}")
    return out
