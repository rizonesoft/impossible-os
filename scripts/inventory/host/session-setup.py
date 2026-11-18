"""First-run setup on the local-account path, captured from the host.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9.

    python3 scripts/inventory/host-capture.py --scenario session-setup

Runs from the pre-oobe checkpoint, where setup waits on its first page and no
account exists, so no guest session opens (SESSION = False) and every step is a
host keystroke, click or shot. The VM has no network adapter, so the network
page offers no way on; the local account form is opened the way Windows itself
documents for that case, Shift+F10 and `start ms-cxh:localonly`, and that
transition is recorded as the page's keyboard path. The account the form makes
gets a random password ref-host keeps in memory; the run restores the
checkpoint, so it ends with the run.

Every record is measured from the shots as in session-secure.py: a region holds
the pieces of ink it expects or the capture refuses. Text is recorded as a
slot, never in Microsoft's words. PAGES is the expected local-account path;
records() emits one page record per entry, in this order, and refuses a run
whose shots show another page sequence.
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
FILE = "setup"
PROFILE = "fhd-100"
CHECKPOINT = "pre-oobe"
SESSION = False
TIMEOUT = 1200

ENTER, ESC, TAB, SHIFT, DOWN, F10 = 13, 27, 9, 16, 40, 121
EASE = (1540, 949)
CONSOLE = (700, 500)
READY_BURST = 10
PREPARE_BURST = 8

# The local-account path, in order.
PAGES = ["region", "keyboard", "second-keyboard", "network", "local-account", "getting-ready", "privacy", "preparing"]


def key(vk):
    return {"op": "key", "vk": vk}


def wait(sec):
    return {"op": "wait", "sec": sec}


def shot(name):
    return {"op": "shot", "name": name}


def settle(timeout=120):
    return {"op": "settle", "sec": 3, "timeout": timeout}


def advance():
    return [key(ENTER), wait(3), settle()]


STEPS = [
    wait(5), settle(90), shot("setup"),
    # The accessibility menu opens about 5 s after the click and keeps the
    # keyboard; a second click closes it, as slowly, and gives the page its
    # focus back. An Enter sent within 3 s of that click went to the menu
    # (2026-11-18), and on the network page the click opened nothing.
    {"op": "click", "x": EASE[0], "y": EASE[1]}, wait(6), shot("setup-accessibility"),
    {"op": "click", "x": EASE[0], "y": EASE[1]}, wait(8),
    # The transition between pages, taken as soon as the page is submitted.
    key(ENTER), shot("between"), wait(3), settle(), shot("keyboard"),
    *advance(), shot("second-keyboard"),
    *advance(), shot("network"),
    {"op": "chord", "mod": SHIFT, "vk": F10}, wait(4), shot("console"),
    {"op": "click", "x": CONSOLE[0], "y": CONSOLE[1]}, wait(1),
    {"op": "text", "text": "start ms-cxh:localonly"}, key(ENTER), wait(6), settle(), shot("setup-local-account"),
    {"op": "text", "text": "owner"}, wait(1), key(TAB), {"op": "renew", "name": "owner"}, wait(2), key(TAB),
    {"op": "typeof", "name": "owner"}, wait(2), shot("local-account-filled"),
    # Three security questions, each a list and an answer.
    *[s for n in (1, 2, 3) for s in [key(TAB), wait(1), *[key(DOWN), wait(1)] * n, key(TAB), wait(1),
                                      {"op": "text", "text": f"answer{n}"}, wait(1)]],
    shot("security-questions"),
    key(ENTER), {"op": "burst", "name": "getting-ready", "count": READY_BURST, "every": 0},
    settle(180), shot("privacy"),
    *advance(), shot("privacy-more"),
    key(ENTER), {"op": "burst", "name": "preparing", "count": PREPARE_BURST, "every": 0},
    {"op": "settle", "sec": 5, "timeout": 240}, shot("first-desktop"),
]

SHOTS = ["setup", "setup-accessibility", "between", "keyboard", "second-keyboard", "network", "console",
         "setup-local-account", "local-account-filled", "security-questions",
         *[f"getting-ready-{i}" for i in range(1, READY_BURST + 1)], "privacy", "privacy-more",
         *[f"preparing-{i}" for i in range(1, PREPARE_BURST + 1)], "first-desktop"]


def white(p):
    return min(p) >= 225


def panel_white(p):
    """The panel's fill, which shades from white to a pale blue at its foot."""
    return min(p) >= 235


def dark(p):
    return max(p) <= 90


def accent(p):
    return p[2] >= 150 and p[0] <= 60 and p[1] <= 140


def form_blue(p):
    """The local account window's fill."""
    return p[2] >= 140 and p[0] <= 30 and 60 <= p[1] <= 120


def saturated(p):
    return max(p) - min(p) >= 60


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


def panel(img, what):
    """The setup frame's white panel: its edges along a row above the content
    and a column left of the illustration."""
    w, h, _ = img
    xs = [x for x in range(w) if panel_white(pixel(img, x, 200))]
    ys = [y for y in range(h) if panel_white(pixel(img, 400, y))]
    if not xs or not ys:
        raise core.Incomplete(f"{what}: no setup panel")
    box = (xs[0], ys[0], xs[-1] - xs[0] + 1, ys[-1] - ys[0] + 1)
    if len(xs) != box[2] or len(ys) != box[3]:
        raise core.Incomplete(f"{what}: the panel's edge row or column is broken: {box}")
    return box


def stamp(entry):
    return datetime.fromisoformat(entry["at"])


def records(shots, build, taken=None):
    src = f"hand:{build}"
    out = []

    def rec(rid, kind, label, parent=None, **extra):
        r = {"id": f"win11.session.setup{'.' + rid if rid else ''}", "surface": SURFACE, "kind": kind, "label": label,
             "order": len(out), "source": src, "capturedFrom": build}
        if parent is not None:
            r["parent"] = f"win11.session.setup{'.' + parent if parent else ''}"
        for k, v in extra.items():
            if k == "bounds":
                v = {"x": v[0], "y": v[1], "width": v[2], "height": v[3]}
                r["captureProfile"], r["session"] = PROFILE, "console"
            r[k] = v
        if kind not in ("shortcut", "metric", "state", "gesture"):
            r.setdefault("disposition", "build")
        out.append(r)
        return rid

    screen = (0, 0, 1920, 1080)
    frame = panel(shots["setup"], "setup")
    for name in ("between", "keyboard", "second-keyboard", "network", "privacy", "privacy-more", "preparing-1"):
        if panel(shots[name], name) != frame:
            raise core.Incomplete(f"{name}: the setup panel moved: {panel(shots[name], name)} against {frame}")
    fx, fy, fw, fh = frame
    content = (fx + fw // 2, fy, fw // 2, fh)
    art = (fx, fy, fw // 2, fh)

    root = rec("", "surface", "First-run setup", bounds=screen, openedBy="First start after installation",
               behaviour="Full screen: a soft wallpaper with one white panel centred on it, an illustration on the left half and the page on the right; the accessibility button sits below the panel's bottom right corner.")
    rec("frame", "region", "Setup panel", "", bounds=frame,
        behaviour="The same size and place on every page; pages change inside it and nothing outside it moves.")
    img = shots["setup"]
    globe = core.ink_box(img, (art[0] + 12, art[1] + 12, art[2] - 24, art[3] - 24), saturated)
    if globe is None:
        raise core.Incomplete("setup: no illustration")
    rec("frame.illustration", "region", "Illustration side", "frame", bounds=globe,
        behaviour="One picture per page, centred in the panel's left half; it changes with the page.")
    title = union(*core.ink_boxes(img, (content[0], fy + 40, content[2], 100), dark, 14))
    yes = one(img, (content[0], fy + fh - 120, content[2], 120), "setup: Yes button", accent, 30)
    rec("frame.content", "region", "Content side", "frame", bounds=union(title, yes),
        behaviour="The page's question as its title, its choices under it, and its buttons at the bottom; left aligned on the panel's right half.")
    rec("frame.buttons", "group", "Button row", "frame", bounds=yes, itemCount=1,
        behaviour="Bottom right of the content side; the primary button is the rightmost, with a link-style secondary action (Add layout, Learn more) left of it on the pages that have one.")
    rec("frame.buttons.primary", "control", "Primary button", "frame.buttons", controlType="button", bounds=yes,
        behaviour="Accent filled and focused when the page shows, so Enter takes it; disabled grey while the page cannot go on (the network page without a connection).")
    ease = one(img, (fx + fw - 60, fy + fh + 12, 120, 70), "setup: accessibility button", lambda p: max(p) <= 180, 6)
    rec("frame.accessibility", "control", "Accessibility button", "frame", controlType="button", bounds=ease,
        behaviour="Opens a menu of accessibility switches above it about five seconds after the click; it keeps the keyboard while open. No volume button is shown on this build in a VM without an audio device.")
    img = shots["setup-accessibility"]
    # The menu overlaps the white panel; its one-pixel grey border is its edge.
    edges = core.ink_boxes(img, (ease[0] - 300, ease[1] - 480, 480, 470), lambda p: 205 <= p[0] <= 222 and abs(p[1] - p[0]) <= 4 and abs(p[2] - p[0]) <= 4, 4)
    if not edges:
        raise core.Incomplete("setup-accessibility: no menu above the button")
    menu = max(edges, key=lambda b: b[2] * b[3])
    if menu[2] < 200 or menu[3] < 200:
        raise core.Incomplete(f"setup-accessibility: no menu border above the button: {menu}")
    items = rows_in(img, (menu[0] + 1, menu[1] + 1, menu[2] - 2, menu[3] - 2), dark, 6)
    names = ("Magnifier", "Contrast themes", "Narrator", "Voice access", "On-screen keyboard", "Sticky keys", "Filter keys")
    if len(items) != len(names):
        raise core.Incomplete(f"setup-accessibility: {len(items)} rows where {len(names)} were expected: {items}")
    rec("frame.accessibility.menu", "menu", "Accessibility menu", "frame.accessibility", bounds=menu, itemCount=len(names),
        behaviour="One row per switch: an icon, the name, and an Off/On label with a toggle at the right; a switch takes effect at once, before setup ends.")
    for n, (label, box) in enumerate(zip(names, items)):
        rec(f"frame.accessibility.menu.{core.slug(label)}", "menuItem", label, "frame.accessibility.menu",
            controlType="checkbox", bounds=box, toggleStates=["off", "on"])
    spin = one(shots["between"], content, "between: progress", accent, 20)
    rec("frame.progress", "control", "Progress between pages", "frame", controlType="progressBar", bounds=spin,
        behaviour="As soon as a page is submitted its content side clears to a small spinning ring centred in it, while the illustration stays; the next page replaces both when it is ready.")
    rec("keys.enter", "shortcut", "Primary button", "", shortcut="Enter",
        behaviour="Takes the focused primary button on every page; the walk through the local-account path is Enter on each page.")
    rec("keys.tab", "shortcut", "Next control", "", shortcut="Tab", behaviour="Moves the focus through the page's controls in reading order.")
    rec("keys.console", "shortcut", "Command prompt", "", shortcut="Shift+F10",
        behaviour="Opens an administrator command prompt over setup on any page; `start ms-cxh:localonly` there opens the local account form when no network is present.")

    # The pages, in the order the local-account path shows them.
    def page(rid, label, bounds, behaviour, **extra):
        return rec(f"page.{rid}", "page", label, "", bounds=bounds, behaviour=behaviour, **extra)

    def title_of(img, what):
        got = core.ink_boxes(img, (content[0], fy + 40, content[2], 110), dark, 14)
        if not got:
            raise core.Incomplete(f"{what}: no title")
        return union(*got)

    def list_rows(img, what, top, n):
        sel = one(img, (content[0], top, content[2], 90), f"{what}: selected row", accent, 30)
        rows = rows_in(img, (content[0], sel[1] + sel[3], content[2], fy + fh - 100 - sel[1] - sel[3]), dark, 10)
        if len(rows) != n - 1:
            raise core.Incomplete(f"{what}: {len(rows)} rows under the selection where {n - 1} were expected")
        return sel, union(sel, *rows)

    def back_of(img, what):
        return one(img, (fx, fy, 60, 60), f"{what}: back button", dark, 6)

    img = shots["setup"]
    sel, lst = list_rows(img, "region", fy + 140, 8)
    page("region", "Country or region", frame, "A list of countries and regions with the one the installation chose selected and scrolled to; typing jumps to a name. Yes accepts.")
    rec("page.region.title", "control", "Title", "page.region", controlType="text", bounds=title_of(img, "region"))
    rec("page.region.list", "control", "Region list", "page.region", controlType="list", bounds=lst,
        behaviour="Eight rows visible, the selected one accent filled; arrow keys move it and it scrolls.")
    rec("page.region.yes", "control", "Yes button", "page.region", controlType="button", bounds=yes)

    img = shots["keyboard"]
    sel, lst = list_rows(img, "keyboard", fy + 190, 7)
    page("keyboard", "Keyboard layout", frame, "A list of layouts for the chosen region, the region's default selected. Back returns to the region page.")
    rec("page.keyboard.back", "control", "Back button", "page.keyboard", controlType="button", bounds=back_of(img, "keyboard"),
        behaviour="An arrow at the panel's top left on every page after the first.")
    rec("page.keyboard.title", "control", "Title", "page.keyboard", controlType="text", bounds=title_of(img, "keyboard"),
        behaviour="Two lines with a smaller line under them saying a second layout can be added next.")
    rec("page.keyboard.list", "control", "Layout list", "page.keyboard", controlType="list", bounds=lst)
    rec("page.keyboard.yes", "control", "Yes button", "page.keyboard", controlType="button",
        bounds=one(img, (content[0], fy + fh - 120, content[2], 120), "keyboard: Yes", accent, 30))

    img = shots["second-keyboard"]
    skip, add = sorted(pieces(img, (content[0], fy + fh - 120, content[2], 120), 2, "second-keyboard: buttons", accent, 10), key=lambda b: -b[0])
    page("second-keyboard", "Second keyboard layout", frame, "Asks whether to add another layout; Skip is the primary button, so Enter skips the page. This is the one page of the path that can be skipped.")
    rec("page.second-keyboard.back", "control", "Back button", "page.second-keyboard", controlType="button", bounds=back_of(img, "second-keyboard"))
    rec("page.second-keyboard.title", "control", "Title", "page.second-keyboard", controlType="text", bounds=title_of(img, "second-keyboard"))
    rec("page.second-keyboard.add", "control", "Add layout", "page.second-keyboard", controlType="hyperlink", bounds=add,
        behaviour="A link-style button left of Skip; opens the layout list again for a second layout.")
    rec("page.second-keyboard.skip", "control", "Skip button", "page.second-keyboard", controlType="button", bounds=skip)

    img = shots["network"]
    nxt = one(img, (content[0], fy + fh - 120, content[2], 120), "network: Next", lambda p: 180 <= p[0] <= 200 and 180 <= p[2] <= 210 and p[2] - p[0] <= 15, 10)
    drivers = one(img, (content[0], fy + 400, content[2], 160), "network: Install drivers", accent, 10)
    page("network", "Network", frame, "Asks for a network connection. With no adapter it shows how to install a driver and Next stays disabled, so the page cannot be passed from the page itself: the local account path goes through the command prompt (Shift+F10).")
    rec("page.network.title", "control", "Title", "page.network", controlType="text", bounds=title_of(img, "network"))
    rec("page.network.drivers", "control", "Install drivers button", "page.network", controlType="button", bounds=drivers)
    rec("page.network.next", "control", "Next button", "page.network", controlType="button", bounds=nxt, enabled=False)

    img = shots["setup-local-account"]
    win = core.ink_boxes(img, (0, 0, 1920, 1080), form_blue, 4)
    if not win:
        raise core.Incomplete("setup-local-account: no form")
    win = max(win, key=lambda b: b[2] * b[3])

    def fields(img, what, n):
        """The form's text boxes and lists: a dark fill, or white while focused."""
        got = [b for b in core.ink_boxes(img, win, lambda p: (p[0] <= 10 and 40 <= p[1] <= 64 and 76 <= p[2] <= 104) or min(p) >= 200, 6)
               if 280 <= b[2] <= 300 and 26 <= b[3] <= 30]
        if len(got) != n:
            raise core.Incomplete(f"{what}: {len(got)} fields where {n} were expected: {got}")
        return got
    name_box, pw, confirm = fields(img, "setup-local-account", 3)
    nxt = one(img, (win[0] + win[2] // 2, win[1] + win[3] - 120, win[2] // 2, 120), "setup-local-account: Next",
              lambda p: p[2] >= 200 and p[0] <= 30 and p[1] >= 100, 10)
    page("local-account", "Create a local account", win,
         "A separate window over setup in the account service's blue, with a title bar of its own: who will use the PC and a password twice; once a password is typed, three security questions with their answers appear under them and the form scrolls. Next creates the account.")
    rec("page.local-account.name", "control", "User name box", "page.local-account", controlType="edit", bounds=name_box,
        behaviour="Focused when the form opens; a focused box turns white.")
    rec("page.local-account.password", "control", "Password box", "page.local-account", controlType="edit", bounds=pw)
    rec("page.local-account.confirm", "control", "Confirm password box", "page.local-account", controlType="edit", bounds=confirm)
    got = fields(shots["security-questions"], "security-questions", 9)[3:]
    rec("page.local-account.questions", "group", "Security questions", "page.local-account", bounds=union(*got), itemCount=3,
        behaviour="Shown under the passwords once a password is typed: three pairs of a question list and an answer box; a question chosen in one list is left out of the others.")
    for n in range(3):
        rec(f"page.local-account.questions.q{n + 1}", "control", f"Security question {n + 1}", "page.local-account.questions",
            controlType="comboBox", bounds=union(got[2 * n], got[2 * n + 1]), behaviour="A question list with its answer box under it.")
    rec("page.local-account.next", "control", "Next button", "page.local-account", controlType="button", bounds=nxt)

    # Getting ready: full screen messages between the account and privacy.
    ready = [n for n in sorted((k for k in shots if k.startswith("getting-ready-")), key=lambda k: int(k.rsplit("-", 1)[1]))]
    messages = []
    for n in ready:
        img = shots[n]
        if pixel(img, 10, 10) != (0, 0, 0):
            continue
        text = core.ink_boxes(img, screen, lambda p: min(p) >= 150, 20)
        if text:
            messages.append((n, union(*text)))
    if len(messages) < 3:
        raise core.Incomplete(f"getting-ready: {len(messages)} black frames with text where at least 3 were expected")
    page("getting-ready", "Getting ready", screen,
         "Full screen black between the account and the privacy page: one centred line at a time, each fading in and out (a moment, a greeting, getting things ready, a few minutes with a smaller line not to turn off the PC, almost there), and a deep blue glow behind the few-minutes line.")
    seen = []
    for n, box in messages:
        if seen and abs(seen[-1][1][2] - box[2]) <= 4 and abs(seen[-1][1][1] - box[1]) <= 4:
            continue
        seen.append((n, box))
    for i, (n, box) in enumerate(seen):
        rec(f"page.getting-ready.message-{i + 1}", "state", f"Message {i + 1}", "page.getting-ready", bounds=box,
            behaviour=f"A text slot {box[2]} px wide centred on the screen, frame {n}.")
    if taken:
        first = next(n for n, _ in messages)
        last = messages[-1][0]
        span = (stamp(taken[last]) - stamp(taken[first])).total_seconds() * 1000
        rec("page.getting-ready.duration", "metric", "Getting ready duration", "page.getting-ready",
            measure={"value": round(span), "unit": "ms", "readBy": f"host frame times, {first} to {last}"},
            behaviour="At least this long: the first and last black frames with text, taken about 2.5 s apart; the fade curves belong to the motion catalogue's recorder.")

    img = shots["privacy"]
    page("privacy", "Privacy settings", frame,
         "Five switches with a paragraph each; Next scrolls to the rest and turns into Accept, which saves them. Every switch here sends data to Microsoft or depends on its services.",
         disposition="equivalent")
    rec("page.privacy.title", "control", "Title", "page.privacy", controlType="text", bounds=title_of(img, "privacy"))

    def toggles(img, what, n):
        """A switch per state label (Yes, No, Required only): the label starts
        about 100 px right of the panel's middle and its switch left of it."""
        rows = []
        for b in core.ink_boxes(img, (content[0], fy + 150, content[2], fh - 250), dark, 4):
            if content[0] + 96 <= b[0] <= content[0] + 106:
                cy = b[1] + b[3] // 2
                got = core.ink_boxes(img, (content[0] + 40, cy - 13, 54, 26), lambda p: dark(p) or accent(p), 6)
                # A heading's word can start where a state label does; only a
                # switch-shaped piece left of it counts.
                rows += [g for g in got if 40 <= g[2] <= 48 and 17 <= g[3] <= 22]
        if len(rows) != n:
            raise core.Incomplete(f"{what}: {len(rows)} switches where {n} were expected: {rows}")
        return rows
    first = toggles(img, "privacy", 3)
    more = toggles(shots["privacy-more"], "privacy-more", 3)
    settings = (("location", "Location", "on", "build", None),
                ("find-my-device", "Find my device", "off", "excluded", "It needs a Microsoft account service that tracks the device; Impossible OS has none."),
                ("diagnostic-data", "Diagnostic data", "off", "equivalent", None),
                ("inking-typing", "Inking & typing", "on", "excluded", "It sends typing and inking samples to Microsoft; Impossible OS sends none."),
                ("personalized-offers", "Personalized offers", "on", "excluded", "Impossible OS shows no offers or advertising, so there is nothing to consent to."))
    for (rid, label, state, disp, why), box in zip(settings, first + more[1:]):
        extra = {"reason": why} if why else {}
        rec(f"page.privacy.{rid}", "setting", label, "page.privacy", controlType="toggle", bounds=box, toggleStates=["off", "on"],
            behaviour=f"Default {state}{' (Required only)' if rid == 'diagnostic-data' else ''}.", disposition=disp, **extra)
    img = shots["privacy-more"]
    accept = max(core.ink_boxes(img, (content[0], fy + fh - 120, content[2], 120), lambda p: accent(p) or dark(p), 4), key=lambda b: b[2] * b[3])
    if accept[2] < 100:
        raise core.Incomplete(f"privacy-more: no Accept button: {accept}")
    rec("page.privacy.accept", "control", "Accept button", "page.privacy", controlType="button", bounds=accept,
        behaviour="Next until the list has been scrolled to its end, then Accept with a focus outline.")

    img = shots["preparing-1"]
    page("preparing", "Preparing", frame, "After Accept: the privacy illustration stays and the content side clears to the spinning ring until the desktop shows.")
    rec("page.preparing.progress", "control", "Progress ring", "page.preparing", controlType="progressBar",
        bounds=one(img, content, "preparing-1: progress", accent, 20))
    built = [r["id"].rsplit(".", 1)[1] for r in out if r["kind"] == "page"]
    if built != PAGES:
        raise core.Incomplete(f"pages {built} are not the local-account path {PAGES}")

    rec("first-desktop", "state", "First desktop", "", bounds=screen,
        behaviour="The desktop with Start already open over it on the first sign-in; no other window and no notification shows.")
    return out
