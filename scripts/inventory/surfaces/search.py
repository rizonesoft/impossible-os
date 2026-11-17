"""Surface module: the search flyout, its results, its preview pane and its menu.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 8 (Search
Flyout). Writes docs/design/inventory/start/search.json:

    python3 scripts/inventory/capture.py --surface start --file search --module search

Measured on 26300.9974 (2026-11-17): the taskbar's search button opens a
CoreWindow named "Search" (858 x 938, its search box standing in the taskbar)
whose page is a WebView2 document. UI Automation reads the page through
Chromium's accessibility tree: the filter row, the result list (best match,
then one group per kind), the preview pane on the right (a header, the file
facts, the Actions list) and the Options menu. The reference VM is offline, so
there is no Web filter, no web result, no highlights panel and no calculation
answer; those are hand records. The rows of every group are this machine's data
and are recorded as lists without rows; the queries are fixed, and a file and a
folder are made in Documents and waited for in the index, so each result kind is
the same in every capture.
"""
import re
import time

import capture_core as core

SURFACE = "start"
ROOT = "search"

# Rows of a result group, and the preview's header and file facts, are the
# machine's data: recorded once as a list, never entered.
ENVIRONMENT_LISTS = [{"className": "suggsList"}, {"className": "anchor clickable"},
                     {"className": "metadataContainer"}]
# Names that carry the query or the result: never a label, never an id.
# A file fact's value is named by its caption ("Location") and holds the file's
# own data, so it is labelled from LABELS, never from its name.
VALUES = [{"classPrefix": "suggestion cf"}, {"className": "details"}, {"className": "metadataContainer"}]
# Result group headings carry a runtime number as their automation id (gr53).
HEADINGS = [("Suggested", "suggested-heading"), ("Quick searches", "quick-searches-heading"),
            ("Top apps", "top-apps-heading"), ("Command", "command-heading"),
            ("Find results in settings, click enter to see more", "settings-heading"),
            ("Find results in documents, click enter to see more", "documents-heading"),
            ("Find results in folders, click enter to see more", "folders-heading"),
            ("Find results in apps, click enter to see more", "apps-heading")]
IDENTITY = ([({"classPrefix": "suggestion cf"}, "best-match"),
             ({"className": "metadataContainer", "name": "Location"}, "location-value"),
             ({"className": "metadataContainer", "name": "Last modified"}, "modified-value")]
            + [({"classPrefix": "groupHeader", "name": name}, seg) for name, seg in HEADINGS]
            + [({"className": "suggsList", "name": name}, seg.removesuffix("-heading")) for name, seg in HEADINGS])
# The browser frame around the page, the page itself, and each filter's tooltip
# wrapper are read through.
HOSTS = [{"className": "WebView2Standalone.Controls.WebView2"}, {"className": "Chrome_WidgetWin_1"},
         {"className": "BrowserRootView"}, {"automationId": "RootWebArea"}, {"className": "MouseHoverTooltip"}]
LABELS = [({"classPrefix": "suggestion cf"}, "Best match"),
          ({"className": "details"}, "No results message"),
          ({"automationId": "bpage"}, "Results page"),
          ({"automationId": "groups"}, "Result groups"),
          ({"classPrefix": "scopes-list"}, "Filter bar"),
          ({"className": "anchor clickable"}, "Preview header"),
          ({"className": "divider"}, "Divider"),
          ({"automationId": "search-description"}, "Status"),
          ({"className": "metadataContainer", "name": "Location"}, "Location value"),
          ({"className": "metadataContainer", "name": "Last modified"}, "Last modified value")]
# Empty holders the page keeps for messages and dialogs it is not showing.
OMIT = [{"automationId": "dialogBoxWrapper"}, {"automationId": "temporaryMessage"},
        {"automationId": "temporaryOption"}]
# The Options menu's entries and the Actions lists' entries act as menu items.
KINDS = [({"classPrefix": "menuItem"}, "menuItem"), ({"classPrefix": "sectionItem"}, "menuItem")]

WINDOW = {"className": "Windows.UI.Core.CoreWindow", "name": "Search"}
SETTLE = 2
OPEN_TRIES = 3
OPEN_WAIT = 20
CLOSED_FOR = 1
QUERY_WAIT = 15
# The fixtures a file result and a folder result are found by, made in the
# user's Documents (indexed by default) and waited for in the index.
FIXTURE_FILE = "Zephyrine report.txt"
FIXTURE_FOLDER = "Zephyrine folder"
MAKE = ("$d=[Environment]::GetFolderPath('MyDocuments'); "
        f"Set-Content -LiteralPath (Join-Path $d '{FIXTURE_FILE}') -Value 'inventory fixture'; "
        f"New-Item -ItemType Directory -Force -Path (Join-Path $d '{FIXTURE_FOLDER}') | Out-Null")
INDEXED = ("$c=New-Object -ComObject ADODB.Connection; "
           "$c.Open(\"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';\"); "
           "$r=$c.Execute(\"SELECT System.ItemPathDisplay FROM SYSTEMINDEX WHERE System.FileName LIKE 'Zephyrine%'\"); "
           "$n=0; while(-not $r.EOF){$n++; $r.MoveNext()}; $c.Close(); $n")
INDEX_WAIT = 420                   # 150 s seen on a fresh restore, 2026-11-17
INDEX_POLL = 15
REST = (8, 8)                      # a pointer rest on the desktop, away from every hover state

_indexed = []


def window(ctx):
    return ctx.find_window(timeout=10, **WINDOW)


def page(ctx):
    return ctx.find(within=window(ctx), automationId="RootWebArea", depth=20, timeout=15)


def ensure_fixtures(ctx):
    """Make the fixture file and folder once and wait until both are indexed."""
    if _indexed:
        return
    ctx.powershell(MAKE)
    deadline = time.monotonic() + INDEX_WAIT
    while True:
        n = ctx.powershell(INDEXED).strip()
        if n.isdigit() and int(n) >= 2:
            break
        if time.monotonic() >= deadline:
            raise core.Incomplete(f"the search fixtures were not indexed within {INDEX_WAIT} s (index holds {n})")
        time.sleep(INDEX_POLL)
    _indexed.append(True)


def open_search(ctx):
    """Click the taskbar's search button. A click soon after search closed can
    land while it is still closing and open nothing, and SearchHost can take
    longer than 10 s to show (2026-11-17), so the later tries wait longer and use
    Win+S, the keyboard's way to the same window."""
    bar = ctx.find_window(className="Shell_TrayWnd")
    for attempt in range(1, OPEN_TRIES + 1):
        if attempt == 1:
            ctx.click(ctx.find(within=bar, automationId="SearchButton", depth=4))
        else:
            ctx.keys("win+s")
        try:
            ctx.find_window(timeout=OPEN_WAIT, **WINDOW)
            break
        except core.Incomplete:
            if attempt == OPEN_TRIES:
                raise
    ctx.find(within=page(ctx), automationId="suggestionsList", depth=12, timeout=20)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def close_search(ctx):
    """Esc until the window is gone and stays gone. It can drop off screen for a
    moment while the preview pane relays and then stay open, and the next click
    on the search button would close it rather than open it (2026-11-17)."""
    for _ in range(OPEN_TRIES):
        ctx.keys("esc")
        ctx.gone(timeout=15, **WINDOW)
        time.sleep(CLOSED_FOR)
        try:
            ctx.find_window(timeout=0, **WINDOW)
        except core.Incomplete:
            return
    raise core.Incomplete(f"search stayed open after {OPEN_TRIES} presses of Esc")


# Characters a key chord types on the US layout, beside letters and digits.
TYPED = {" ": "space", ":": "shift+semicolon", "*": "shift+8"}


def type_query(ctx, text):
    ctx.click(ctx.find(within=window(ctx), automationId="SearchTextBox", depth=3))
    time.sleep(0.5)
    ctx.keys("ctrl+a")
    for ch in text:
        ctx.keys(TYPED.get(ch, ch))


def query(text, ready):
    """Open search and type `text`; `ready` names what the answer shows when it
    has arrived (the best match row, or the no-results status)."""
    def opener(ctx):
        if text.startswith("zephyrine"):
            ensure_fixtures(ctx)
        open_search(ctx)
        type_query(ctx, text)
        ctx.find(within=page(ctx), depth=14, timeout=QUERY_WAIT, **ready)
        time.sleep(SETTLE)
        ctx.hover_point(*REST)
    return opener


BEST = {"classPrefix": "suggestion cf"}
NONE = {"automationId": "search-description"}


def home(ctx):
    return ctx.find(within=page(ctx), automationId="suggestionsList", depth=4)


def results(ctx):
    return ctx.find(within=page(ctx), automationId="bpage", depth=4)


def actions(ctx):
    return ctx.find(within=page(ctx), controlType="List", name="Actions", depth=6)


def open_options(ctx):
    open_search(ctx)
    ctx.click(ctx.find(within=page(ctx), automationId="optionsButton", depth=8))
    options(ctx)
    time.sleep(1)
    ctx.hover_point(*REST)


def options(ctx):
    return ctx.find(within=page(ctx), className="contextMenu advancedScopesDropdown", depth=8, timeout=10)


def close_options(ctx):
    ctx.keys("esc")
    time.sleep(1)
    close_search(ctx)


def open_by_enter(ctx):
    query("notep", BEST)(ctx)
    ctx.keys("enter")
    notepad(ctx)
    time.sleep(SETTLE)


def notepad(ctx):
    return ctx.find_window(className="Notepad", timeout=20)


def close_notepad(ctx):
    """Notepad as Enter opened it: no file, nothing to save, so a stop loses nothing."""
    ctx.powershell("Stop-Process -Name notepad -Force")
    ctx.gone(timeout=15, className="Notepad")


# A results page other than the first leaves out the filter bar and the actions:
# the first results page records the bar, and each kind's actions are their own scene.
REST_OMIT = OMIT + [{"classPrefix": "scopes-list"}, {"controlType": "List", "name": "Actions"}]
FIRST_OMIT = OMIT + [{"controlType": "List", "name": "Actions"}]
QUERIES = [("app", "An app", "notep", BEST), ("file", "A file", "zephyrine report", BEST),
           ("folder", "A folder", "zephyrine folder", BEST), ("setting", "A setting", "mouse", BEST)]

SCENES = [
    # The window: its size and the search box. UI Automation's control view does not
    # reach the page through the browser frame, so the page's scenes root inside it.
    dict(slug=ROOT, label="Search", kind="surface", open=open_search, root=window, close=close_search, expand=False),
    dict(slug="home", label="Search home", kind="region", open=open_search, root=home, close=close_search,
         expand=False),
    dict(slug="options-menu", label="Search options menu", kind="menu", open=open_options, root=options,
         close=close_options, expand=False),
]
for i, (slug, label, text, ready) in enumerate(QUERIES):
    SCENES.append(dict(slug=f"results-{slug}", label=f"Results, {label.lower()}", kind="region",
                       open=query(text, ready), root=results, close=close_search,
                       omit=FIRST_OMIT if i == 0 else REST_OMIT, expand=False))
    SCENES.append(dict(slug=f"{slug}-actions", label=f"{label}'s actions", kind="menu", open=query(text, ready),
                       root=actions, close=close_search, expand=False))
SCENES += [
    dict(slug="no-results", label="No results", kind="state", open=query("zephyrine nothing", NONE),
         root=results, close=close_search, omit=REST_OMIT, expand=False),
    # Measured, not published: a filter typed as a word selects that filter, and
    # Enter opens the best match.
    dict(slug="typed-filter", label="A filter typed as a word", kind="state", open=query("apps: notep", BEST),
         root=results, close=close_search, omit=FIRST_OMIT, keep=False, expand=False),
    dict(slug="enter", label="Enter on the best match", kind="state", open=open_by_enter, root=notepad,
         close=close_notepad, keep=False, expand=False),
    # Measured, not published: offline, a calculation and a unit conversion each
    # end at No results (the hand record "Calculation and unit answers").
    dict(slug="calculation-answer", label="A calculation", kind="state", open=query("12*7", NONE), root=results,
         close=close_search, omit=REST_OMIT, keep=False, expand=False),
    dict(slug="unit-answer", label="A unit conversion", kind="state", open=query("10 km in miles", NONE),
         root=results, close=close_search, omit=REST_OMIT, keep=False, expand=False),
]


def METRICS(records):
    """The panel's size and place, the result rows, the preview pane, and the
    checks that keep the published ids stable."""
    p = f"win11.{SURFACE}."
    by = {r["id"]: r for r in records}
    loose = [r["id"] for r in records if re.search(r"\.gr\d+(\.|$)", r["id"])]
    if loose:
        raise ValueError(f"a result heading has no fixed id (add it to HEADINGS): {loose[:3]}")

    def box(rid, what):
        r = by.get(p + rid)
        if r is None or "bounds" not in r:
            raise ValueError(f"{what} was not captured ({rid})")
        return r["bounds"]

    for slug, _, _, _ in QUERIES:
        acts = [r for r in records if r.get("parent") == f"{p}{slug}-actions"]
        if not acts or any(r["kind"] != "menuItem" for r in acts):
            raise ValueError(f"the {slug} result's actions were not captured as menu entries")
    typed = by.get(p + "typed-filter.scopes-list.filter-your-results.l1scope1")
    if typed is None or "scope-tile--selected" not in typed.get("className", ""):
        raise ValueError("typing 'apps:' did not select the Apps filter")
    if p + "enter" not in by:
        raise ValueError("Enter on the best match did not open Notepad")

    out = []

    def metric(slug, label, value, order):
        out.append({"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "metric", "label": label, "order": order,
                    "measure": {"value": value, "unit": "px", "scaling": "scales"}})

    panel = box(ROOT, "the search window")
    sbox = box(f"{ROOT}.searchtextbox", "the search box")
    metric("panel-width", "Panel width", panel["width"], 0)
    metric("panel-height", "Panel height, the taskbar's search box included", panel["height"], 1)
    metric("box-width", "Search box width", sbox["width"], 2)
    metric("box-height", "Search box height", sbox["height"], 3)
    best = box("results-app.suggestionslist.best-match", "the best match row")
    metric("best-match-height", "Best match row height, an app", best["height"], 4)
    metric("best-match-height-file", "Best match row height, a file or folder",
           box("results-file.suggestionslist.best-match", "the file's best match row")["height"], 5)
    head = box("results-app.anchor-clickable", "the preview header")
    metric("preview-width", "Preview pane width", head["width"], 6)
    metric("preview-header-height", "Preview header height", head["height"], 7)
    rows = [r for r in records if r.get("parent") == p + "app-actions"]
    metric("action-row-height", "Action row height", rows[0]["bounds"]["height"], 8)
    metric("action-row-pitch", "Action row pitch", rows[1]["bounds"]["y"] - rows[0]["bounds"]["y"], 9)
    tile = box("results-app.scopes-list.filter-your-results.l1scope0", "the All filter")
    metric("filter-height", "Filter height", tile["height"], 10)
    return out


BEHAVIOURS = [
    ("prefix-match", "Prefix matching", "Results update with each key; a word's start finds it (\"notep\" finds "
                                        "Notepad as the best match)."),
    ("word-match", "Word matching", "A word inside a name matches as well as its first word (\"mouse\" finds "
                                    "\"Change the mouse pointer display or speed\")."),
    ("typed-filter", "Filter typed as a word", "Typing a filter's name and a colon before the query (\"apps: notep\") "
                                               "selects that filter, as a click on it does."),
    ("right-arrow", "Right arrow", "Right on a result moves into its preview pane's actions; every result's name "
                                   "ends with that hint."),
    ("enter", "Enter", "Enter runs the selected result's first action, Open; the best match is selected while "
                       "typing."),
    ("ctrl-shift-enter", "Ctrl+Shift+Enter", "Runs the selected app as administrator, the preview's \"Run as "
                                             "administrator\" action."),
    ("tab", "Tab and arrows", "Tab moves between the filter bar, the results and the preview pane; Up and Down move "
                              "through the results."),
    ("group-header", "A group heading", "Enter or a click on a group's heading (\"Find results in settings\") selects "
                                        "that group's filter with the query kept."),
    ("back", "Back to home", "Back, or emptying the box, returns to the home page with its suggestions."),
    ("close", "Closing", "Esc or a click outside closes search; it opens again on its home page."),
]
FRAMES = [
    ("home-suggestions", "Home suggestions", "The home page's Suggested list, Quick searches and Top apps hold this "
                                             "machine's apps and settings; recorded as frames, their rows are data."),
    ("recent-searches", "Recent searches", "A row of recent queries on the home page; this build shows none on a new "
                                           "account and the reference machine keeps none."),
    ("web-filter", "Web, Videos and other online filters", "With a network the filter row adds online filters behind "
                                                           "its scroll button; offline it ends at Photos."),
    ("web-results", "Web results", "Web suggestions and their preview come from the online service; offline the "
                                   "group is absent."),
    ("highlights", "Search highlights", "The home page's daily highlights panel comes from the online service; "
                                        "offline it is absent."),
    ("calculation", "Calculation and unit answers", "Offline, \"12*7\" and \"10 km in miles\" each return No "
                                                    "results on this build (both measured every run): the answer "
                                                    "card comes from the online service."),
    ("account-results", "Account results", "Results from a Microsoft account or a work or school account, offered "
                                           "by the Options menu's first two entries."),
]
HAND = ([{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "gesture", "label": label, "order": 40 + i,
          "behaviour": text} for i, (slug, label, text) in enumerate(BEHAVIOURS)]
        + [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "region", "label": label, "order": 60 + i,
            "behaviour": text} for i, (slug, label, text) in enumerate(FRAMES)])
