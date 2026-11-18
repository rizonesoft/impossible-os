"""Surface module: the input switcher and the IME candidate windows.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9 (the
shell overlays). Writes docs/design/inventory/flyouts/overlays-ime.json under
profile `ime` (profile-ime: en-US display with the Japanese and the Chinese
input method added, ref-vm.ps1 -Derive -Profile ime):

    python3 scripts/inventory/capture.py --surface flyouts --file overlays-ime \
        --module overlays-ime --profile ime

Measured on 26300.9974 (2026-11-18). Win+Space, held, lists the input methods
with their layouts; each further Space moves the highlight and letting Win go
switches to it (Esc does not cancel). The candidate windows are WinUI windows
of the Windows Input Experience, reached through UI Automation's root; no
EnumWindows walk sees them. Microsoft IME turns on with VK_IME_ON (a toggle
key such as VK_KANJI can turn it off) and a second Space opens its vertical
list; Microsoft Pinyin shows its horizontal list while composing, before any
Space. PageDown turns a page. The candidates are dictionary words: withheld.
"""
import ctypes
import time

import capture_core as core

SURFACE = "flyouts"
user32 = ctypes.WinDLL("user32", use_last_error=True)
WM_INPUTLANGCHANGEREQUEST = 0x0050
LAYOUTS = {"ja": 0x04110411, "zh": 0x08040804}
# Each fixture types a reading with more than one page of candidates.
FIXTURES = {"ja": "kouka", "zh": "shi"}
SETTLE = 1.5
WAIT = 5
POLL = 0.25
MAX_CANDIDATES = 10

CANDIDATE = {"controlType": "ListItem", "automationId": ""}
VALUES = [CANDIDATE]
IDENTITY = [({**CANDIDATE, "nth": n}, f"candidate-{n}") for n in range(1, MAX_CANDIDATES + 1)]
IDENTITY += [({"automationId": "IME_Candidate_Window"}, "candidate-window")]
LABELS = [(CANDIDATE, "Candidate"), ({"automationId": "PipsPagerScrollViewer"}, "Page pips"), ({"automationId": "TEMPLATE_PART_CandidateItemAnnotation"}, "Annotation")]

_first = {}


def notepad(ctx):
    return ctx.find_window(className="Notepad")


def open_switcher(ctx):
    # The field every IME scene types into; launched here, where it can take the
    # foreground (launched later, behind the switcher's focus change, it could not).
    ctx.launch("notepad.exe")
    notepad(ctx)
    time.sleep(SETTLE)
    ctx.hold("win+space")
    time.sleep(SETTLE)


def switcher(ctx):
    return ctx.find_window(className="Shell_InputSwitchTopLevelWindow", timeout=WAIT, foreground=False)


def close_switcher(ctx):
    # Esc does not cancel: the highlight goes round to the current layout first.
    ctx.keys("space")
    ctx.keys("space")
    ctx.release("win+space")


def candidates(ctx):
    live = ctx.live
    end = time.monotonic() + WAIT
    while time.monotonic() < end:
        for w in live.children(live.uia.GetRootElement(), live.uia.CreateTrueCondition()):
            if w.class_name == "ApplicationFrameWindow" and w.bounds[2] > 0 and not w.offscreen:
                return w
        time.sleep(POLL)
    raise core.Incomplete("no candidate window")


def first_candidate(ctx, w):
    items = ctx.live.uia.CreatePropertyCondition(30003, 50007)      # ControlType ListItem
    el = w.el.FindFirst(4, items)                                    # TreeScope_Descendants
    if el is None:
        raise core.Incomplete("the candidate window lists no candidate")
    return el.CurrentName


def compose(lang, page):
    def opener(ctx):
        w = notepad(ctx)
        h = w.el.CurrentNativeWindowHandle
        user32.PostMessageW(h, WM_INPUTLANGCHANGEREQUEST, 0, LAYOUTS[lang])
        time.sleep(SETTLE)
        tid = user32.GetWindowThreadProcessId(h, None)
        if user32.GetKeyboardLayout(tid) & 0xFFFFFFFF != LAYOUTS[lang]:
            raise core.Incomplete(f"Notepad did not take the {lang} input method")
        if lang == "ja":
            ctx.keys("ime_on")
        for ch in FIXTURES[lang]:
            ctx.keys(ch)
        if lang == "ja":
            ctx.keys("space")
            ctx.keys("space")
        time.sleep(SETTLE)
        first = first_candidate(ctx, candidates(ctx))
        if page == 1:
            _first[lang] = first
            return
        ctx.keys("pagedown")
        time.sleep(SETTLE)
        if first_candidate(ctx, candidates(ctx)) in (first, _first.get(lang)):
            raise core.Incomplete(f"PageDown did not reach a second {lang} candidate page")
    return opener


def cancel(ctx):
    for _ in range(3):
        ctx.keys("esc")


SCENES = [dict(slug="input-switcher", label="Input switcher", kind="surface", open=open_switcher, root=switcher,
               close=close_switcher, expand=False)]
for lang, name in (("ja", "Japanese"), ("zh", "Chinese")):
    SCENES += [dict(slug=f"ime-{lang}-candidates", label=f"{name} candidate window", kind="surface",
                    open=compose(lang, 1), root=candidates, close=cancel, expand=False),
               dict(slug=f"ime-{lang}-candidates-page-2", label=f"{name} candidate window, second page", kind="state",
                    open=compose(lang, 2), root=candidates, close=cancel, expand=False)]


def METRICS(records):
    """The switcher's width and row height and each candidate window's item
    pitch, from captured bounds."""
    p = f"win11.{SURFACE}."
    by = {r["id"]: r for r in records}

    def box(rid):
        r = by.get(p + rid)
        if r is None:
            raise ValueError(f"{rid} was not captured")
        return r["bounds"]

    def metric(parent, slug, label, order, value):
        return {"id": f"{parent}.{slug}", "parent": parent, "kind": "metric", "label": label, "order": order,
                "measure": {"value": value, "unit": "px", "scaling": "scales"}}

    rows = sorted((r for r in records if r.get("parent", "").startswith(p + "input-switcher.")
                   and r.get("controlType") == "listitem"), key=lambda r: r["bounds"]["y"])
    if len(rows) < 2:
        raise ValueError("the input switcher listed fewer than two input methods")
    panel = "windows-input-experience.candidate-window.template-part-candidatepanel"
    ja1, ja2 = box(f"ime-ja-candidates.{panel}.candidate-1"), box(f"ime-ja-candidates.{panel}.candidate-2")
    zh1, zh2 = box(f"ime-zh-candidates.{panel}.candidate-1"), box(f"ime-zh-candidates.{panel}.candidate-2")
    return [metric("input-switcher", "width", "Input switcher width", 100, box("input-switcher")["width"]),
            metric("input-switcher", "row-height", "Input switcher row height", 101, rows[1]["bounds"]["height"]),
            metric("ime-ja-candidates", "item-pitch", "Vertical candidate pitch", 100, ja2["y"] - ja1["y"]),
            metric("ime-ja-candidates", "item-height", "Vertical candidate height", 101, ja1["height"]),
            metric("ime-zh-candidates", "item-pitch", "Horizontal candidate pitch", 100, zh2["x"] - zh1["x"]),
            metric("ime-zh-candidates", "item-width", "Horizontal candidate width", 101, zh1["width"])]


HAND = [
    {"id": "input-switcher.win-space", "parent": "input-switcher", "kind": "shortcut", "label": "Switch input method",
     "order": 120, "shortcut": "Win+Space", "shortcutScope": "global",
     "behaviour": "Held, Win+Space lists the input methods with their layouts, the current one marked; each further "
     "Space moves the highlight, and letting Win go switches to it. Esc does not cancel."},
    {"id": "ime-ja-candidates.composition", "parent": "ime-ja-candidates", "kind": "gesture", "label": "Composition",
     "order": 140, "behaviour": "Typed kana stay underlined in the field as a composition; Space converts it in "
     "place and a second Space opens the vertical candidate list below the caret, the current choice marked with "
     "a bar; digits pick, PageDown and the arrows below turn pages, Tab opens the table view, Esc cancels."},
    {"id": "ime-zh-candidates.composition", "parent": "ime-zh-candidates", "kind": "gesture", "label": "Composition",
     "order": 140, "behaviour": "Pinyin stays underlined in the field while the horizontal candidate list shows "
     "below it; Space or a digit commits a candidate, PageDown and the arrows turn pages, Esc cancels."},
]
