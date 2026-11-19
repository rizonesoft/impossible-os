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


def band_bytes(img, box):
    """The pixels of box in a read_png image, row by row."""
    w, _, data = img
    x, y, bw, bh = box
    return b"".join(bytes(data[((y + r) * w + x) * 3:((y + r) * w + x + bw) * 3]) for r in range(bh))


def band_change(imgs, box):
    """What the frames show of one band: the first frame's and every frame of
    its size are compared, never a frame of another mode (review, 2026-11-18)."""
    alike = [i for i in imgs if i[:2] == imgs[0][:2]]
    if len(alike) < 2:
        return "one frame caught it, so this walk shows no change"
    if len({band_bytes(i, box) for i in alike}) > 1:
        return f"its text changed across the {len(alike)} frames compared"
    return f"its text stayed the same in the {len(alike)} frames compared"


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
BLUE = (33, 101, 181)         # the boot manager's and recovery's background
MENU = {"x": 10, "y": 10, "rgb": list(BLUE)}
# A point on Safe mode's taskbar (1920 by 1080 coordinates, as awaitcolour
# takes them): x 100, y 760 at its 1024x768, measured on the reference.
SAFE_TASKBAR = {"x": 188, "y": 1069, "rgb": [239, 239, 239]}   # the boot menu is up: its blue in the corner
# The boot and stop bursts end at the boot menu (until), so their counts are
# ceilings: 14 boot frames ran out before Windows' spinner showed alone on a
# slower restart (2026-11-18), and the boot burst keeps sampling until the menu
# rather than handing the rest of a slow boot to the menu wait, which keeps no
# frames (review, 2026-11-18). The boot and stop bursts watch for that many
# seconds at one frame a second at most, so neither a fast nor a slow host
# changes how long they watch.
BOOT_BURST, REPAIR_BURST, STOP_BURST = 150, 80, 120

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
# Choose other options opens recovery without a restart: the page was up and
# still within 5 s in a probe (2026-11-18), blue like the options page before
# it. The walk waits for the screen to stop changing rather than a fixed 18 s;
# a page that never came fails the recovery tile checks, a rerun, never a
# wrong record.
INTO_RECOVERY = [wait(3), {"op": "settle", "sec": 3, "timeout": 60}]
TO_RECOVERY = [*keys(DOWN, DOWN, TAB), key(ENTER), wait(5), *keys(DOWN, DOWN), key(ENTER), *INTO_RECOVERY]

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
    # The burst ends at the first frame with the menu's blue, and the menu is
    # then confirmed before any key: its 30 s countdown is the walk's only
    # window, which a fixed burst could outrun (review, 2026-11-18), and the
    # mode alone comes back while Windows' spinner still shows.
    # The exact frame that met the menu's blue is the boot-manager shot, so the
    # first key goes in at once (review, 2026-11-19). A burst that ran out
    # before the menu waits 10 s for it and fails there, before minutes of
    # navigation a missing frame would waste (review, 2026-11-18).
    {"op": "burst", "name": "boot", "count": BOOT_BURST, "every": 0, "cadence": 1, "seconds": BOOT_BURST, "until": MENU,
     "as": "boot-manager", "astimeout": 10},
    *keys(DOWN), shot("boot-manager-second"),
    *keys(DOWN, TAB), shot("boot-manager-link"),
    key(ENTER), wait(5), shot("boot-options"),
    # Choose other options opens the recovery environment.
    *keys(DOWN, DOWN), key(ENTER), *INTO_RECOVERY, shot("recovery"),
    *keys(RIGHT), key(ENTER), wait(4), shot("recovery-troubleshoot"),
    *keys(DOWN), key(ENTER), wait(6), shot("recovery-reset"), key(BACK), wait(4),
    *keys(UP, RIGHT), key(ENTER), wait(4), shot("recovery-advanced"),
    # Startup Settings shows its Restart page, then the numbered menu after a restart.
    *keys(DOWN), key(ENTER), wait(4), shot("recovery-startup-settings"),
    # Restart shows the numbered menu after a reboot: blue in the firmware's
    # mode, where the page before it is blue at the display's. A fixed 30 s
    # could send 4 to a screen still restarting (review, 2026-11-18).
    key(ENTER), {"op": "awaitcolour", **MENU, "width": 1024, "height": 768, "timeout": 120}, wait(2), shot("startup-settings"),
    # Safe mode boots to its desktop; no record reads the frames on the way
    # (review, 2026-11-18), so the walk waits for the desktop's light taskbar
    # at the basic driver's mode: the restart before it holds a black screen
    # still for 8 s and more, which a settle alone took for the desktop.
    key(FOUR), {"op": "awaitcolour", **SAFE_TASKBAR, "width": 1024, "height": 768, "timeout": 240},
    {"op": "settle", "sec": 3, "timeout": 60}, shot("safe-mode"),
    # An NMI from the host stops Windows; it writes its dump and restarts to the menu.
    # The stop burst watches the whole way to the menu, like the boot burst:
    # 20 unpaced frames could all land on the stop screen (review, 2026-11-18).
    {"op": "nmi"}, {"op": "burst", "name": "stop", "count": STOP_BURST, "every": 0, "cadence": 1, "seconds": STOP_BURST, "until": MENU},
    {"op": "awaitcolour", **MENU, "timeout": 10},
    # Back into recovery for Startup Repair, which looks for the network and gives up.
    *TO_RECOVERY, *keys(RIGHT), key(ENTER), wait(4), *keys(RIGHT), key(ENTER), wait(4),
    # The progress screens turn a spinner and the failure page is still, so the
    # page is the first screen that stays unchanged; a fixed 30 s wait came
    # after a failure page that was up already (review, 2026-11-18). One
    # burst keeps frames until that page holds still, so a repair that starts
    # late is still on them; a settle after a fixed burst kept none (review,
    # 2026-11-18).
    # The exact frame that held still is the failure page's shot (review,
    # 2026-11-19: it was shot again, the same pixels).
    key(ENTER), {"op": "burst", "name": "repair", "count": REPAIR_BURST, "every": 0, "cadence": 3, "seconds": 240, "still": 5,
                 "as": "automatic-repair"},
    # After a failed repair the first page is More recovery options: Continue,
    # Quick machine recovery and Troubleshoot down the first column, Turn off
    # your PC beside Continue.
    key(ENTER), wait(8), shot("recovery-more"),
]

KEY_NAMES = {UP: "Up", DOWN: "Down", LEFT: "Left", RIGHT: "Right", TAB: "Tab", ENTER: "Enter"}
ARROWS = (UP, DOWN, LEFT, RIGHT)


def step_at(name):
    """The index of the step that takes the shot `name` (a burst's `as` too)."""
    return next(i for i, st in enumerate(STEPS) if st["op"] in ("shot", "burst") and (st.get("as") or st.get("name")) == name)


def sent_after(name):
    """The keys the walk sends on the page shot as `name`: those before the
    next shot, up to and with the first Enter (it leaves the page) and before
    any Backspace (it goes back, so later keys are another page's)."""
    out = []
    for st in STEPS[step_at(name) + 1:]:
        if st["op"] in ("shot", "burst"):
            break
        if st["op"] == "key":
            if st["vk"] == BACK:
                break
            out.append(st["vk"])
            if st["vk"] == ENTER:
                break
    return out


def tile_keys(name):
    """What a tile page's keys were seen to do, from the keys the walk sends
    on it, or None when it sends none: each page published the same arrow and
    Enter claim, walked or not (review, 2026-11-19)."""
    sent = sent_after(name)
    arrows = [KEY_NAMES[k] for k in dict.fromkeys(k for k in sent if k in ARROWS)]
    parts = [f"{' and '.join(arrows)} move{'s' if len(arrows) == 1 else ''} the focus"] if arrows else []
    if ENTER in sent:
        parts.append("Enter takes the focused tile")
    return ", ".join(parts) or None


# The boot menu's shortcuts: each is exactly the keys sent between the two
# shots that show its effect (review, 2026-11-19: Up, Tab alone and Enter on
# a tile were claimed, and the walk sends none of them).
BOOT_MENU_KEYS = (
    ("Move to the next entry", "boot-manager", "boot-manager-second", "moves the focus from the first tile to the second"),
    ("Focus the options link", "boot-manager-second", "boot-manager-link", "from the second tile put the focus on the options link"),
    ("Open the focused link", "boot-manager-link", "boot-options", "on the focused options link opens the options page"),
)


def keys_between(a, b):
    """The keys sent after the shot `a` and before the shot `b`."""
    return [st["vk"] for st in STEPS[step_at(a) + 1:step_at(b)] if st["op"] == "key"]


def key_names(vks):
    return ", ".join(KEY_NAMES[k] for k in vks)


# Bursts across a restart: a shot there may straddle a mode change.
MOVING = ("boot-", "repair-", "stop-")
# Bursts that end at the boot menu, so they may hold fewer frames than their count.
EARLY = ("boot-", "repair-", "stop-")

SHOTS = [*[f"boot-{i}" for i in range(1, BOOT_BURST + 1)], "boot-manager", "boot-manager-second", "boot-manager-link",
         "boot-options", "recovery", "recovery-troubleshoot", "recovery-reset", "recovery-advanced",
         "recovery-startup-settings", "startup-settings", "safe-mode",
         *[f"stop-{i}" for i in range(1, STOP_BURST + 1)],
         *[f"repair-{i}" for i in range(1, REPAIR_BURST + 1)], "automatic-repair", "recovery-more"]


SCREEN = (0, 0, 1920, 1080)
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


def band(img, region, test):
    """region cut down to its first band of text: from the first row with ink
    to the first two blank rows after it. Safe Mode's corner marks sit just
    above desktop icons, which a fixed crop runs into (review, 2026-11-18)."""
    x, y, w, h = region
    lit_rows = [any(test(pixel(img, xx, yy)) for xx in range(x, x + w)) for yy in range(y, y + h)]
    if True not in lit_rows:
        return region
    top = lit_rows.index(True)
    end = top
    while end < h and (lit_rows[end] or True in lit_rows[end + 1:end + 2]):
        end += 1
    return (x, y + top, w, end - top)


def burst_frames(names, prefix, after=None):
    """The frames of the burst named prefix among names, in burst order;
    with after, only those after that frame."""
    def number(n):
        return int(n[len(prefix) + 1:])
    got = sorted((n for n in names if n.startswith(prefix + "-") and n[len(prefix) + 1:].isdigit()), key=number)
    return [n for n in got if after is None or number(n) > number(after)]


def frames_word(n):
    return f"{n} frame" if n == 1 else f"{n} frames"


def logo_spinner(imgs, logo):
    """(extent, frames): the union of the spinner's dots seen under the
    firmware logo (a box within 4 px of logo) in every frame that shows the
    logo and only compact boxes below it, and how many frames that was."""
    dots, n = [], 0
    for img in imgs:
        boxes = core.ink_boxes(img, body(img), lit, 10)
        marks = [b for b in boxes if all(abs(b[i] - logo[i]) <= 4 for i in range(4))]
        rest = [b for b in boxes if b not in marks]
        if len(marks) == 1 and rest and all(b[2] <= 80 and b[3] <= 80 and b[1] >= marks[0][1] + marks[0][3] for b in rest):
            dots += rest
            n += 1
    return union(*dots), n


def restarting_parts(img):
    """(dots, label) on a Restarting frame, else None: at the display's
    mode, on black, exactly one line of text (at least three times as wide
    as it is tall, 12 to 40 px tall) with at least one compact box (80 px at
    most) above it, within 150 px and roughly centred on it."""
    if img[:2] != (1920, 1080) or pixel(img, 10, 10) != (0, 0, 0):
        return None
    boxes = core.ink_boxes(img, body(img), lit, 10)

    def layout(label):
        dots = [b for b in boxes if b is not label]
        cx = label[0] + label[2] / 2
        if not dots or any(b[2] > 80 or b[3] > 80 or b[1] + b[3] > label[1] or label[1] - b[1] > 150
                           or abs(b[0] + b[2] / 2 - cx) > 100 for b in dots):
            return None
        return dots, label
    # A wide arc of the spinner is shaped like a line of text too, so the
    # label is the one candidate with only dots above it (review, 2026-11-19).
    fits = [f for f in (layout(b) for b in boxes if b[2] >= 3 * b[3] and 12 <= b[3] <= 40) if f]
    return fits[0] if len(fits) == 1 else None


def restarting_spinner(imgs):
    """(spinner, label, frames) from Restarting frames: the union of the
    dots seen above the first frame's label in every frame whose label is the
    same (within 2 px), and how many frames that was."""
    parts = [restarting_parts(i) for i in imgs]
    parts = [p for p in parts if p]
    word = parts[0][1]
    same = [p for p in parts if all(abs(p[1][i] - word[i]) <= 2 for i in range(4))]
    return union(*(d for dots, _ in same for d in dots)), word, len(same)


def repair_label(img, logo):
    """(logo, label) on Startup Repair's first frame, else None: in the
    firmware's mode, on black, the firmware logo where the boot burst measured
    it (logo, within 4 px) and exactly one line of text below it, 60 px wide
    or more and 12 to 30 px tall. A compact spinner may sit between them.
    Counting two boxes refused that frame and took a lone spinner for the
    label; the topmost box and any 3:1 box took a spinner for the logo and a
    fragment for the label (review, 2026-11-18)."""
    if img[:2] == (1920, 1080) or pixel(img, 10, 10) != (0, 0, 0):
        return None
    boxes = sorted(core.ink_boxes(img, body(img), lit, 10), key=lambda b: b[1])
    marks = [b for b in boxes if all(abs(b[i] - logo[i]) <= 4 for i in range(4))]
    if len(marks) != 1:
        return None
    mark = marks[0]
    below = [b for b in boxes if b[1] >= mark[1] + mark[3]]
    labels = [b for b in below if b[2] >= 60 and 12 <= b[3] <= 30]
    if len(labels) != 1 or any(b[2] > 80 or b[3] > 80 for b in below if b is not labels[0]):
        return None
    return mark, labels[0]


def stop_span(names, taken, shots, test):
    """(first, end) of a screen across burst frames: the first exact frame
    (its mode held across the shot) that passes test, and the first exact
    frame after it that does not. None for end when no such frame exists.
    A screen that passes again after end is no single span to time, and is
    refused (review, 2026-11-18: the end was searched after the last match)."""
    exact = [n for n in names if taken.get(n, {}).get("exact")]
    first = next((n for n in exact if test(shots[n])), None)
    if first is None:
        return None, None
    rest = exact[exact.index(first) + 1:]
    end = next((n for n in rest if not test(shots[n])), None)
    if end is not None and any(test(shots[n]) for n in rest[rest.index(end) + 1:]):
        raise core.Incomplete(f"stop burst: the stop error showed again after {end}, so its span is not one")
    return first, end


def tile_text(tile, ring_width):
    """The text region beside a tile's icon square: from just right of the
    square to just short of the focus rectangle's right edge, whose border
    read as a second text line (review, 2026-11-18). The focus rectangle
    starts 12 px left of the square and is ring_width wide."""
    x, y, _, h = tile
    return (x + 121, y - 4, ring_width - 137, h + 4)


def has_description(img, region):
    """True when the text beside a tile has a line in the smaller description
    face (at most 16 px tall) under its name. Rows are read 4 px apart, so a
    name that wraps stays two name lines and is not taken for a description."""
    rows = sorted(rows_in(img, region, white, 4), key=lambda r: r[1])
    return any(r[3] <= 16 for r in rows[1:])


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


def spinner_alone(img):
    """The box of Windows' spinner on a frame that shows it alone, else None:
    black at the display's mode and one compact box. The blue boot menu also
    reads as one box, the whole screen (review, 2026-11-18)."""
    if img[:2] != (1920, 1080) or pixel(img, 10, 10) != (0, 0, 0):
        return None
    boxes = core.ink_boxes(img, body(img), lit, 10)
    if len(boxes) != 1:
        return None
    x, y, w, h = boxes[0]
    # The spinner's arc, small and below the middle: the firmware logo (183 by 45,
    # high on the screen) is one box too (review, 2026-11-18).
    return boxes[0] if w <= 80 and h <= 80 and y + h / 2 > img[1] / 2 else None


def diagnosing(img, mark):
    """True on Startup Repair's second phase: at the display's mode, on black,
    the logo (a box of mark's size), a compact spinner well below it, and a
    label under the spinner. Words of one label on one line are not a spinner
    (review, 2026-11-18)."""
    if img[:2] != (1920, 1080) or pixel(img, 10, 10) != (0, 0, 0):
        return False
    boxes = core.ink_boxes(img, body(img), lit, 10)
    top = [b for b in boxes if abs(b[2] - mark[2]) <= 4 and abs(b[3] - mark[3]) <= 4]
    if len(top) != 1:
        return False
    below = [b for b in boxes if b[1] > top[0][1] + top[0][3] + 200]
    spins = [b for b in below if b[2] <= 80 and b[3] <= 80]
    return any(any(l[1] >= sp[1] + sp[3] for l in below if l is not sp) for sp in spins)


def link_focus(img, link):
    """The dotted focus rectangle around the text box link, or None: one box
    of white ink (gaps of 2 px) that holds link with room on every side, at
    the display's mode, whose top and bottom edges are dotted (a third to
    two thirds of their pixels lit)."""
    if img[:2] != (1920, 1080):
        return None
    region = (max(0, link[0] - 40), max(0, link[1] - 40), min(img[0], link[0] + 1200) - max(0, link[0] - 40), link[3] + 80)
    for x, y, w, h in core.ink_boxes(img, region, white, 2):
        if not (x < link[0] and y < link[1] and x + w > link[0] + link[2] and y + h > link[1] + link[3]):
            continue
        edges = [sum(white(pixel(img, xx, yy)) for xx in range(x, x + w)) / w for yy in (y, y + h - 1)]
        if all(1 / 3 <= e <= 2 / 3 for e in edges):
            return (x, y, w, h)
    return None


def repair_phases(frames, logo):
    """(first, mark, label, later) from the repair burst's exact frames in
    order: the first frame with the firmware logo and a label, its logo and
    label, and whether a later frame shows the second phase (diagnosing).
    None when no frame shows the first phase."""
    for i, img in enumerate(frames):
        got = repair_label(img, logo)
        if got:
            mark, label = got
            return img, mark, label, any(diagnosing(f, mark) for f in frames[i + 1:])
    return None


def failure_rows(img, mark):
    """The repair failure page's three text rows (message, instruction,
    details) or None: at the display's mode, on black, not the diagnosing
    screen (the logo, a spinner and a label are three rows too; review,
    2026-11-19), two lines at least 400 px wide and 12 to 40 px tall, then a
    narrower block below them of two lines, each 8 to 20 px tall."""
    if img[:2] != (1920, 1080) or pixel(img, 10, 10) != (0, 0, 0) or diagnosing(img, mark):
        return None
    rows = rows_in(img, body(img), lit, 14)
    if len(rows) != 3:
        return None
    one, two, three = rows
    if not all(r[2] >= 400 and 12 <= r[3] <= 40 for r in (one, two)) or three[2] >= one[2] or three[1] <= two[1] + two[3]:
        return None
    # The details are two lines, the reason and the log file's path; a block
    # with one of them is not what the record says (review, 2026-11-19).
    lines = rows_in(img, three, lit, 3)
    if len(lines) != 2 or any(not 8 <= r[3] <= 20 for r in lines):
        return None
    return rows


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

    def frames(prefix, test, exact=True, first=False, after=None):
        """The burst frames whose screen passes test, in order. Geometry is
        measured only from exact frames (the mode held across the shot); a
        frame taken while the mode changed is skipped unless `exact` is False,
        for a caller that reads only its time. With `first`, only the first
        match is looked for; with `after`, only frames after that one."""
        names = burst_frames(shots, prefix, after)
        got = []
        for n in names:
            if (not exact or taken.get(n, {}).get("exact")) and test(shots[n]):
                got.append(n)
                if first:
                    break
        return got

    root = rec("", "surface", "Boot, recovery and stop screens", bounds=SCREEN,
               behaviour="Screens drawn before Windows starts, while it restarts or stops, and by the recovery environment; full screen, no desktop session.")

    # Restarting: a spinner over a one-word label on black, at the display's mode.
    # The spinner's dots move, so one frame shows one phase of them: the
    # record publishes the union of the dots seen above the same label in
    # every exact Restarting frame (review, 2026-11-18: one frame's cluster
    # left out dots another frame showed).
    got = frames("boot", lambda i: restarting_parts(i) is not None)
    if not got:
        raise core.Incomplete("boot burst: no Restarting frame (a spinner over a label on black)")
    spin, word, seen = restarting_spinner([shots[n] for n in got])
    st = rec("restarting", "state", "Restarting screen", root, bounds=SCREEN, openedBy="Restart",
             behaviour="Black after the session closes: a dotted spinner with a one-word label under it, both centred, until the firmware takes over.")
    rec("restarting.spinner", "control", "Spinner", st, controlType="progressBar", bounds=spin,
        behaviour=f"Dots running round a circle, centred above the middle of the screen; the bounds are the dots seen in {frames_word(seen)}, so the full circle can reach a little further.")
    rec("restarting.label", "control", "Label", st, controlType="text", bounds=word,
        behaviour="A text slot of 10 characters in small bold white, centred under the spinner.")

    # The firmware's logo with Windows' spinner, in the firmware's own mode.
    def logo(img):
        return img[:2] != (1920, 1080) and len(core.ink_boxes(img, body(img), lit, 10)) == 2
    got = frames("boot", logo, first=True)
    if not got:
        raise core.Incomplete("boot burst: no frame with the firmware logo and the spinner")
    img = firmware(got[0])
    mark, spin = sorted(core.ink_boxes(img, body(img), lit, 10), key=lambda b: b[1])
    boot_logo = mark
    st = rec("logo", "state", "Boot logo", root, openedBy="Power on or restart",
             behaviour=f"Black in the firmware's own mode ({img[0]}x{img[1]} on the reference machine), the firmware's logo and Windows' spinner under it; positions are fractions of that screen, which no profile governs.")
    rec("logo.firmware", "control", "Firmware logo", st, controlType="image",
        behaviour=f"The logo the firmware publishes in its boot graphics table, drawn as it is, {place(img, mark)}; {mark[2] / img[0]:.0%} of the screen wide.")
    # The dots move, so each frame shows one phase: both extents are unions
    # over every exact frame that shows them, and no centre or unchanged
    # place is claimed (review, 2026-11-18: one frame's midpoint moved
    # between 49% and 51% with the phase caught).
    every = frames("boot", lambda i: True)
    later = every[every.index(got[0]) + 1:]
    fw, fw_n = logo_spinner([shots[n] for n in every if shots[n][:2] == img[:2]], mark)
    lone = [b for b in (spinner_alone(shots[n]) for n in later) if b]
    if not lone:
        raise core.Incomplete("boot burst: no frame with the spinner alone after the logo")
    disp = next(shots[n] for n in later if spinner_alone(shots[n]))
    rec("logo.spinner", "control", "Spinner", st, controlType="progressBar",
        behaviour=f"A solid arc turning round a circle below the logo, not the Restarting screen's dots; the arc seen in {frames_word(fw_n)} in this mode sits {place(img, fw)}. It keeps turning alone once Windows sets the display's own mode, where the arc seen in {frames_word(len(lone))} sits {place(disp, union(*lone))} at 1920x1080.")

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
            behaviour="Tiles in columns of three, read down the first column then the next"
                      + (f"; in this walk {keys}." if (keys := tile_keys(name)) else "."))
        for (cid, clabel, disp, reason), t in zip(choices, ts):
            extra = {"disposition": disp} if disp else {}
            if reason:
                extra["reason"] = reason
            # The text region stops short of the focus rectangle's right edge
            # (review, 2026-11-18: the border read as a second text line).
            text = tile_text(t, w)
            if not rows_in(img, text, white, 10):
                raise core.Incomplete(f"{name}: no name beside the {clabel} tile")
            rec(f"{rid}.{cid}", "control", clabel, f"{rid}.choices", controlType="button", bounds=(t[0] - 12, t[1] - 12, w, h),
                behaviour=f"A {t[2]} px square of the lighter blue with a white line icon, the name beside it at its top{' and a description under it' if has_description(img, text) else ''}; focus draws a white rectangle round tile and text.",
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
    # The link's extent is its focus rectangle, measured on the frame after
    # Tab: a frame with no tile focused, a blank one too, passed for focus
    # (review, 2026-11-19).
    linked = full("boot-manager-link")
    if ring(linked, ts[0]) or ring(linked, ts[1]):
        raise core.Incomplete("boot-manager-link: a tile still has the focus after Tab")
    focus = link_focus(linked, link)
    if not focus:
        raise core.Incomplete("boot-manager-link: no dotted focus rectangle around the options link after Tab")
    opts = rec("boot-manager.options", "control", "Change defaults or choose other options", bm, controlType="hyperlink", bounds=focus,
               behaviour="A text link near the bottom left, wider than its text; "
               + f"{' and then '.join(KEY_NAMES[k] for k in keys_between(*BOOT_MENU_KEYS[1][1:3]))} from the second tile focus it "
               + "with a dotted white rectangle, these bounds, and Enter opens the options page.")
    rec("boot-manager.options.text", "control", "Text", opts, controlType="text", bounds=link,
        behaviour="The link's words in white, at the rectangle's left.")
    second = full("boot-manager-second")
    if core.ink_boxes(second, count, white, 10):
        raise core.Incomplete("boot-manager-second: the countdown is still shown after a key")
    if ring(second, ts[1]) is None or ring(second, ts[0]) is not None:
        raise core.Incomplete("boot-manager-second: Down did not move the focus to the second entry")
    rec("boot-manager.stop-countdown", "gesture", "Stop the countdown", bm,
        behaviour="A key press (Down in this walk) removes the countdown line; the menu then waits for a choice.")
    for lab, before, after, does in BOOT_MENU_KEYS:
        sent = keys_between(before, after)
        rec(f"boot-manager.{lab.lower().replace(' ', '-')}", "shortcut", lab, bm, shortcut=key_names(sent),
            behaviour=f"{' and then '.join(KEY_NAMES[k] for k in sent)} {does}.")

    tile_page("boot-options", "Boot manager options", "boot-options",
              (("timer", "Change the timer", None, None), ("default", "Choose a default operating system", None, None),
               ("other", "Choose other options", None, None)),
              root, back=True, opened="Change defaults or choose other options",
              behaviour="The same blue page with a back button: the first two tiles name a setting with its current value under it, and the third opens the recovery environment, with a line naming what it holds.")

    rx = "Choose other options on the boot manager's options page, which opens the recovery environment without a restart"
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
    marks = [one(img, band(img, r, white), f"safe-mode: {what} mark", white, 6)
             for r, what in (((0, 1, 120, 16), "top left"), ((img[0] - 120, 1, 120, 16), "top right"),
                             ((0, bar - 16, 120, 16), "bottom left"), ((img[0] - 120, bar - 16, 120, 16), "bottom right"))]
    build_line = union(*core.ink_boxes(img, (200, 1, img[0] - 400, 16), white, 6))
    st = rec("safe-mode", "state", "Safe mode desktop", root, openedBy="4 on the Startup Settings menu",
             behaviour=f"The desktop on black with the basic display driver ({img[0]}x{img[1]} on the reference machine), after sign-in; positions are fractions of that screen.")
    rec("safe-mode.marks", "group", "Safe Mode marks", st, itemCount=4,
        behaviour=f"The two words in small white text in each corner, over the wallpaper; the bottom pair sits just above the taskbar, which is {img[1] - 1 - bar} px tall here. Each mark's size is on its own record.")
    for (cid, lab), m in zip((("top-left", "Top left mark"), ("top-right", "Top right mark"),
                              ("bottom-left", "Bottom left mark"), ("bottom-right", "Bottom right mark")), marks):
        rec(f"safe-mode.marks.{cid}", "control", lab, "safe-mode.marks", controlType="text",
            behaviour=f"In the screen's {cid.replace('-', ' ')} corner{', just above the taskbar' if cid.startswith('bottom') else ''}; {m[2]} by {m[3]} px at {img[0]}x{img[1]}.")
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
    # What the frames show of the percentage: whether the line's band changed
    # between them. It read 0% in some walks and reached 100% in others
    # (stop-6, review 2026-11-18), so no single value is published.
    moved = band_change([shots[n] for n in got], (0, progress[1], img[0], progress[3]))
    rec("stop.progress", "control", "Progress", st, controlType="text",
        behaviour=f"A text slot of a percentage and one word, {place(img, progress)}; {moved}.")
    rec("stop.code", "control", "Stop code", st, controlType="text",
        behaviour=f"A small text slot with a label, the stop code's name and its number in hex, {place(img, code)}, near the bottom edge.")
    every = frames("stop", lambda i: True, exact=False)
    # The end is the first exact frame after the first stop frame that no
    # longer shows it: a frame taken while the mode changed can still show it
    # (review, 2026-11-18).
    first, end = stop_span(every, taken, shots, stopped)
    if not end or first != got[0]:
        raise core.Incomplete("stop burst: no exact frame after the stop error to time it by")
    shown = round((stamp(taken[end]) - stamp(taken[got[0]])).total_seconds() * 1000)
    rec("stop.shown", "metric", "Stop error shown for", st,
        measure={"value": shown, "unit": "ms", "readBy": f"host frame times, {got[0]} to {end}"},
        behaviour="The span between the host's times for the first exact frame with the stop error and the first exact frame without it; each time is taken as its frame is saved, so this is the span observed, not a bound on the screen's own; with the small memory dump this walk sets.")

    # Startup Repair: progress, then the failure screen and More recovery options.
    # The second phase is published only when a frame after the first
    # phase shows it (review, 2026-11-18: it was claimed from the first phase
    # alone; 2026-11-19: from a frame before it).
    phases = repair_phases([shots[n] for n in frames("repair", lambda i: True)], boot_logo)
    if not phases:
        raise core.Incomplete("repair burst: no Please wait frame (the logo and a label)")
    img, mark, label, later = phases
    then = ", then, at the display's mode, the logo again with a spinner and a label below it" if later else ""
    rec("repair-progress", "state", "Startup Repair progress", "recovery-advanced", openedBy="Startup Repair",
        behaviour=f"A restart to the firmware's logo with a short label {place(img, label)}{then}, on black.")
    img = full("automatic-repair")
    rows = failure_rows(img, mark)
    if not rows:
        raise core.Incomplete(f"automatic-repair: not the failure page (two lines of text and the details under them): {rows_in(img, body(img), lit, 14)}")
    page = rec("automatic-repair", "page", "Repair failed", "repair-progress", bounds=SCREEN, openedBy="Startup Repair, when it cannot repair the PC; the reason shown in this walk, which runs with no network, is that it could not connect to the network",
               behaviour="Black, centred text: what happened, how to go on, and two smaller white lines with the reason and the log file's path.")
    for (cid, clab, beh), b in zip((("message", "Message", "A text slot of 56 characters in one line."),
                                     ("instruction", "Instruction", "A text slot of 51 characters naming Enter and a click."),
                                     ("details", "Details", "Two smaller white lines: the reason and the log file's path.")), rows):
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
