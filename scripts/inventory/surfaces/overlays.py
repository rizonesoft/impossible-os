"""Surface module: the shell overlays a hotkey opens on the reference machine.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9 (Shell
Overlays). Writes docs/design/inventory/flyouts/overlays.json:

    python3 scripts/inventory/capture.py --surface flyouts --file overlays --module overlays

Measured on 26300.9974 (2026-11-18): Win+V and Win+period open one window, the
"Emoji and more" panel (an ApplicationFrameWindow over the Windows Input
Experience CoreWindow, its page a WebView), on its clipboard and its most
recently used tab; clipboard history is off on a new account, so the clipboard
tab first shows its turn-on card. Win+R opens the classic Run dialog (#32770);
the capture runs elevated, so a Run dialog it starts itself is the elevated
form. Win+Shift+S opens the capture bar over a frozen copy of the screen
(XamlWindow). Win+H opens voice typing, and on a new account its consent popup
for the speech service. The touch keyboard opens from its taskbar button
(shown once TabletTip's TipbandDesiredVisibility is set); its settings menu
offers the layouts Default, Compact, Traditional and Gamepad (no Small or
Split on this build) and Handwriting. Win+P and Win+K open Quick Settings on
its Project and Cast pages, which quick-settings.json holds; only their
hotkeys are recorded here. The volume and brightness indicators, the Project
modes and the input switcher need hardware or settings this machine lacks;
their own files hold them (overlays-laptop, overlays-dual, overlays-ime).
"""
import time
import unicodedata

import capture_core as core

SURFACE = "flyouts"
SETTLE = 2
TAB_WAIT = 10
PAGE_WAIT = 20
OPEN_TRIES = 3
OPEN_PAUSE = 3
TAB_SETTLE = 4
REST = (8, 8)

PANEL_WINDOW = {"className": "ApplicationFrameWindow"}
PANEL = {"automationId": "Windows.Shell.InputApp.FloatingSuggestionUI"}
TABS = [("Most recently used", "recent"), ("Emoji", "emoji"), ("GIF", "gif"), ("Kaomoji", "kaomoji"),
        ("Symbols", "symbols"), ("Clipboard history", "clipboard")]
# The panel's page nests its content five containers deep; they are read through.
HOSTS = [{"automationId": "Windows.Shell.InputApp.SuggestionUI.WebView"}, {"automationId": "input-suggestion-ui-body"},
         {"automationId": "content-root"}, {"controlType": "Group", "name": "", "automationId": ""}]
# A tab's grid holds the glyphs this machine offers or used last: the list is
# recorded, never its rows.
ENVIRONMENT_LISTS = [{"controlType": "List", "automationIdPrefix": "expanded-view-list-"}]
NAV = "navigation-menu-item-container-"
TONES = ["Default", "Light", "Medium Light", "Medium", "Medium Dark", "Dark"]
# A category tab replaces the tab row with Back and the category's own tabs; their
# automation ids carry the category and a URL-encoded name, so each is named here.
# The selected tab's name gains " Selected", so names match as prefixes and the
# label is the name alone.
SUBTABS = ["Back", "Recent", "Trending", "Smiley faces and animals", "People", "Celebrations and objects",
           "Food and plants", "Transportation and places", "Classic Ascii Emoticons", "Happy", "Greeting",
           "General punctuation", "Currency symbols", "Latin symbols", "Geometric symbols", "Math symbols",
           "Supplemental symbols", "Language symbols"]
TAB_NAMES = [name for name, _ in TABS] + SUBTABS
IDENTITY = ([({"controlType": "ListItem", "automationIdPrefix": NAV, "namePrefix": name}, f"{seg}-tab")
             for name, seg in TABS]
            + [({"controlType": "ListItem", "automationIdPrefix": NAV, "namePrefix": name}, f"{core.slug(name)}-tab")
               for name in SUBTABS]
            + [({"namePrefix": f"{tone} Skin Tone"}, f"{core.slug(tone)}-tone")
               for tone in sorted(TONES, key=len, reverse=True)]
            + [({"namePrefix": "Inventory sample"}, "item")]
            + [({"controlType": "Text", "name": "Open:"}, "open-label"), ({"controlType": "ComboBox", "name": "Open:"},
                                                                          "open-box")]
            + [({"controlType": "Button", "namePrefix": "See more "}, "see-more")])
# A tab's or a skin tone's name carries its selection ("People Selected"); the
# item's carries the text copied. Each is labelled from LABELS instead.
VALUES = [{"controlType": "ListItem", "namePrefix": "Clipboard "}, {"controlType": "ListItem", "automationIdPrefix": NAV},
          {"namePrefix": "Inventory sample"}]
VALUES += [{"namePrefix": f"{tone} Skin Tone"} for tone in TONES]
# Longest first, so "Recent" never labels "Recently ..." and a category's own
# "Symbols" tab and the top row's share one label.
# A touch keyboard key named only by a punctuation mark slugs to nothing; it is
# named by the character's Unicode name. Its automation id is "Button_" + the
# character (measured on every layout and layer, 2026-11-18).
KEY_MARKS = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\u00a3\u00a5\u00b0\u00d7\u00f7\u20ac"
IDENTITY += [({"className": "CRootKey", "automationId": f"Button_{ch}"}, f"{core.slug(unicodedata.name(ch))}-key")
             for ch in KEY_MARKS]
# The traditional layout has two Control and two alt keys, alike in id and name;
# left and right is what they are.
IDENTITY += [({"className": "CRootKey", "automationId": aid, "nth": n}, f"{side}-{word}-key")
             for aid, word in (("switchCtl", "control"), ("switchAlt", "alt")) for n, side in ((1, "left"), (2, "right"))]
# The language key names the input language in use: a value, labelled here.
LANGUAGE_KEY = {"className": "CRootKey", "namePrefix": "Change language"}
IDENTITY += [(LANGUAGE_KEY, "change-language")]
VALUES += [LANGUAGE_KEY]
LABELS = [({"controlType": "ListItem", "automationIdPrefix": NAV, "namePrefix": name}, name)
          for name in sorted(TAB_NAMES, key=len, reverse=True)]
LABELS += [({"namePrefix": f"{tone} Skin Tone"}, f"{tone} skin tone")
           for tone in sorted(TONES, key=len, reverse=True)]
LABELS += [({"namePrefix": "Inventory sample"}, "Clipboard item"), (LANGUAGE_KEY, "Change language")]

CLIP_SAMPLES = ("Inventory sample one", "Inventory sample two", "Inventory sample three")


def panel_window(ctx):
    return ctx.find_window(timeout=10, foreground=False, **PANEL_WINDOW)


def panel(ctx):
    return ctx.find(within=panel_window(ctx), depth=3, timeout=10, **PANEL)


def tab(ctx, name, timeout=10):
    return ctx.find(within=panel(ctx), controlType="ListItem", automationIdPrefix=NAV, namePrefix=name, depth=8,
                    timeout=timeout)


def page_summary(ctx):
    """The panel's named buttons and list items, for a failure message."""
    seen, level = [], [panel(ctx)]
    for _ in range(10):
        nxt = []
        for el in level:
            for k in el.children():
                if k.control_type in ("Button", "ListItem", "Text") and core.name_of(k):
                    seen.append(f"{k.control_type}:{core.name_of(k)[:24]}")
                nxt.append(k)
        level = nxt
    return ", ".join(seen[:30]) or "(nothing)"


def tab_shown(ctx, name):
    """The tab reports itself selected, or (a category tab) the page has
    replaced the tab row with Back and the category's own row."""
    try:
        if core.name_of(tab(ctx, name, timeout=0)).endswith(" Selected"):
            return True
    except core.Incomplete:
        pass
    try:
        ctx.find(within=panel(ctx), controlType="ListItem", name="Back", depth=10, timeout=0)
        return True
    except core.Incomplete:
        return False


def press_until(ctx, chord, shown):
    """Press a hotkey until what it opens shows. Right after sign-in the input
    host is still starting and swallows the first press (2026-11-18)."""
    for attempt in range(1, OPEN_TRIES + 1):
        ctx.keys(chord)
        try:
            return shown(ctx)
        except core.Incomplete:
            if attempt == OPEN_TRIES:
                raise
            time.sleep(OPEN_PAUSE)


def to_tab_row(ctx):
    """Wait for the page to show either its tab row or a category's Back
    button (it loads after the window shows), and go back to the tab row."""
    deadline = time.monotonic() + PAGE_WAIT
    while True:
        try:
            tab(ctx, "Most recently used", timeout=0)
            return
        except core.Incomplete:
            pass
        try:
            ctx.click(ctx.find(within=panel(ctx), controlType="ListItem", name="Back", depth=10, timeout=0))
            time.sleep(1)
            continue
        except core.Incomplete:
            pass
        if time.monotonic() > deadline:
            raise core.Incomplete(f"the panel's page showed neither its tab row nor Back within {PAGE_WAIT} s")
        time.sleep(0.5)


def open_panel(chord, tab_name=None):
    def opener(ctx):
        press_until(ctx, chord, panel)
        # The panel reopens on the tab it last showed, and a category tab
        # replaces the tab row with Back and its subcategories (2026-11-18).
        to_tab_row(ctx)
        if tab_name:
            ctx.click(tab(ctx, tab_name))
            # A tab's page loads after the click; a walk that starts while it
            # loads fails inside the web view's provider (Symbols, 2026-11-18).
            deadline = time.monotonic() + TAB_WAIT
            while not tab_shown(ctx, tab_name):
                if time.monotonic() > deadline:
                    raise core.Incomplete(f"the {tab_name} tab was not selected within {TAB_WAIT} s; "
                                          f"the panel shows: {page_summary(ctx)}")
                time.sleep(0.5)
        time.sleep(TAB_SETTLE if tab_name else SETTLE)
        ctx.hover_point(*REST)
    return opener


def close_panel(ctx):
    ctx.keys("esc")
    ctx.gone(timeout=10, **PANEL_WINDOW)


def history_on(ctx):
    ctx.click(ctx.find(within=panel(ctx), automationId="expanded-view-messageview-actiontext", depth=10, timeout=10))
    time.sleep(SETTLE)


def open_history_empty(ctx):
    open_panel("win+v")(ctx)
    history_on(ctx)
    ctx.hover_point(*REST)


def open_history_items(ctx):
    for text in CLIP_SAMPLES:
        ctx.powershell(f"Set-Clipboard -Value '{text}'")
        time.sleep(1.5)
    open_panel("win+v")(ctx)


def history_item(ctx):
    """The newest clipboard item: its card and the buttons it shows."""
    rows = ctx.find(within=panel(ctx), automationId="expanded-view-list-ClipboardHistory", depth=10, timeout=10)
    kids = rows.children()
    if not kids:
        raise core.Incomplete(f"the clipboard list is empty; the panel shows: {page_summary(ctx)}")
    return kids[0]


def open_item_more(ctx):
    open_history_items(ctx)
    ctx.click(ctx.find(within=history_item(ctx), name="See more", depth=4, timeout=10))
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def open_people(ctx):
    """The emoji tab's People category, the one that offers skin tones."""
    open_panel("win+period", "Emoji")(ctx)
    ctx.click(ctx.find(within=panel(ctx), controlType="ListItem", automationIdPrefix=NAV, namePrefix="People",
                       depth=10, timeout=10))
    time.sleep(TAB_SETTLE)
    ctx.hover_point(*REST)


def run_dialog(ctx):
    return ctx.find_window(timeout=10, className="#32770", name="Run")


def open_run(ctx):
    press_until(ctx, "win+r", run_dialog)
    time.sleep(SETTLE)


def open_run_elevated(ctx):
    # The capture's own process is elevated (ref-run's task runs at Highest), so
    # the dialog it starts is the elevated form.
    ctx.launch("rundll32.exe", "shell32.dll,#61")
    run_dialog(ctx)
    time.sleep(SETTLE)


def close_run(ctx):
    ctx.keys("esc")
    ctx.gone(timeout=10, className="#32770", name="Run")


CAPTURE_BAR = {"className": "XamlWindow", "name": "Select area to snip"}


def open_capture_bar(ctx):
    press_until(ctx, "win+shift+s", capture_bar)
    time.sleep(SETTLE)


def capture_bar(ctx):
    return ctx.find_window(timeout=10, foreground=False, **CAPTURE_BAR)


def mode_box(ctx):
    return ctx.find(within=capture_bar(ctx), automationId="SnippingModeComboBox", depth=6, timeout=10)


def open_capture_modes(ctx):
    """The mode dropdown open: a walk that expanded it in place recorded the
    shown value and the entry of the same name under one id (2026-11-18)."""
    open_capture_bar(ctx)
    ctx.click(mode_box(ctx))
    time.sleep(SETTLE)


def close_capture_bar(ctx):
    """Esc until the bar is gone: with its mode menu open the first Esc closes
    only the menu (2026-11-18)."""
    for _ in range(OPEN_TRIES):
        ctx.keys("esc")
        try:
            ctx.gone(timeout=3, **CAPTURE_BAR)
            return
        except core.Incomplete:
            pass
    ctx.gone(timeout=10, **CAPTURE_BAR)


def voice_bar(ctx):
    return ctx.find(within=panel_window(ctx), automationId="Windows.Shell.InputApp.VoiceTypingUI", depth=3, timeout=10)


def open_voice(ctx):
    press_until(ctx, "win+h", voice_bar)
    time.sleep(SETTLE)


def voice_consent(ctx):
    return ctx.find(within=panel_window(ctx), automationId="FloatyTip", depth=3, timeout=10)


def open_voice_settings(ctx):
    open_voice(ctx)
    ctx.click(ctx.find(within=voice_bar(ctx), automationId="SettingsFlyoutButton", depth=2, timeout=10))
    time.sleep(SETTLE)


def voice_settings(ctx):
    return ctx.find(within=panel_window(ctx), className="Popup", depth=6, timeout=10)


def close_voice(ctx):
    ctx.keys("esc")
    time.sleep(1)
    try:
        ctx.find_window(timeout=0, foreground=False, **PANEL_WINDOW)
    except core.Incomplete:
        return
    ctx.click(ctx.find(within=voice_bar(ctx), automationId="CloseButton", depth=2, timeout=5))
    ctx.gone(timeout=10, **PANEL_WINDOW)


def taskbar(ctx):
    return ctx.find_window(timeout=10, className="Shell_TrayWnd")


def power_menu(ctx):
    """Win+X's menu, the same one a right-click on Start opens. It shows no
    top-level window of its own (2026-11-18): it is looked for inside the
    taskbar, then among the popup windows."""
    try:
        return ctx.find(within=taskbar(ctx), controlType="Menu", depth=12, timeout=5)
    except core.Incomplete:
        pass
    return ctx.find(controlType="Menu", depth=6, timeout=5)


def open_power_menu(ctx):
    ctx.click(ctx.find(within=taskbar(ctx), automationId="StartButton", depth=4), button="right")
    time.sleep(SETTLE)
    power_menu(ctx)


def close_power_menu(ctx):
    ctx.keys("esc")
    time.sleep(1)



TABLET_TIP = r"HKCU:\Software\Microsoft\TabletTip\1.7"
KEYBOARD_BUTTON = f"New-Item -Path '{TABLET_TIP}' -Force | Out-Null; " \
                  f"Set-ItemProperty -LiteralPath '{TABLET_TIP}' -Name TipbandDesiredVisibility -Value 1 -Type DWord"
KEYBOARD_WAIT = 15
LAYOUT_SETTLE = 4
SETTINGS_BUTTON = "KeyboardSettingsMenuButton"
LAYOUT_SUBMENU = "Windows.Shell.InputApp.InputPanel.KeyboardModalityMenu"
LAYOUTS = [("ModalityOneHandedKeyboard", "compact", "Compact"), ("ModalityClassicKeyboard", "traditional", "Traditional"),
           ("ModalityStandardKeyboard", "default", "Default"), ("ModalityKeyboard", "gamepad", "Gamepad")]


def keyboard_frame(ctx):
    return ctx.find_window(timeout=KEYBOARD_WAIT, foreground=False, **PANEL_WINDOW)


def keyboard(ctx):
    """The input panel itself; its automation id names the layout shown."""
    return ctx.find(within=keyboard_frame(ctx), className="IPTip_Main_Window", depth=3, timeout=KEYBOARD_WAIT)


# The keyboard suggests the clipboard's newest text; the clipboard scenes leave
# their samples there, which no clean machine shows (2026-11-18).
CLEAR_CLIPBOARD = ("Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.Clipboard]::Clear(); "
                   "Add-Type -AssemblyName System.Runtime.WindowsRuntime; "
                   "[Windows.ApplicationModel.DataTransfer.Clipboard,Windows.ApplicationModel.DataTransfer,"
                   "ContentType=WindowsRuntime]::ClearHistory() | Out-Null")


def open_keyboard(ctx):
    ctx.powershell(KEYBOARD_BUTTON + "; " + CLEAR_CLIPBOARD)
    core.restart_explorer(ctx)
    ctx.click(ctx.find(within=taskbar(ctx), automationId="SystemTrayIcon", namePrefix="Touch Keyboard", depth=3,
                       timeout=30))
    board = keyboard(ctx)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)
    left = [t.name for t in ctx.live.children(board.el) if (t.name or "").startswith("Inventory sample")]
    if left:
        raise core.Incomplete(f"the keyboard still suggests the clipboard sample {left[0]!r}")


def keep_keyboard(ctx):
    """The keyboard stays up between its scenes; each scene changes what it shows."""


def keyboard_menus(ctx):
    """The keyboard's open menus: its settings menu and the layout submenu, both
    named "Keyboard Settings Flyout"; a closed one stays in the tree with no area."""
    found = []

    def visit(el, depth):
        for k in el.children():
            if k.control_type == "Menu" and not k.offscreen and k.bounds[2] > 0 and k.bounds[3] > 0:
                found.append(k)
            if depth < 8:
                visit(k, depth + 1)
    visit(keyboard_frame(ctx), 0)
    return found


def open_settings(ctx):
    ctx.click(ctx.find(within=keyboard_frame(ctx), automationId=SETTINGS_BUTTON, depth=8, timeout=10))
    time.sleep(SETTLE)
    if not keyboard_menus(ctx):
        raise core.Incomplete("the touch keyboard's settings menu did not open")


def settings_menu(ctx):
    menus = [m for m in keyboard_menus(ctx)
             if any(k.automation_id == LAYOUT_SUBMENU for k in m.children())]
    if not menus:
        raise core.Incomplete("the touch keyboard's settings menu is not open")
    return menus[0]


def layout_menu(ctx):
    """The submenu: the open menu that does not hold the layout entry."""
    menus = [m for m in keyboard_menus(ctx)
             if not any(k.automation_id == LAYOUT_SUBMENU for k in m.children())]
    if not menus:
        raise core.Incomplete("the touch keyboard's layout menu is not open")
    return menus[0]


def entry(menu, entry_id):
    """A menu's entry by automation id, from the menu's own children."""
    hit = [k for k in menu.children() if k.automation_id == entry_id]
    if not hit:
        raise core.Incomplete(f"{entry_id} is not in the menu; it holds {[k.automation_id for k in menu.children()]}")
    return hit[0]


def open_layouts(ctx):
    open_settings(ctx)
    # Neither a click nor a hover opens this submenu (2026-11-18).
    ctx.expand_only(entry(settings_menu(ctx), LAYOUT_SUBMENU))
    time.sleep(SETTLE)
    layout_menu(ctx)


def close_menus(ctx):
    """The keyboard never takes the focus, so Esc does not reach its menus; the
    settings button toggles them shut."""
    for _ in range(OPEN_TRIES):
        if not keyboard_menus(ctx):
            return
        ctx.click(ctx.find(within=keyboard_frame(ctx), automationId=SETTINGS_BUTTON, depth=8, timeout=10))
        time.sleep(1)
    raise core.Incomplete("the touch keyboard's menu stayed open")


def choose(entry_id, within):
    def opener(ctx):
        (open_layouts if within is layout_menu else open_settings)(ctx)
        # A synthesized click on the entry does not choose it; Invoke does.
        ctx.invoke(entry(within(ctx), entry_id))
        time.sleep(LAYOUT_SETTLE)
        keyboard(ctx)
        ctx.hover_point(*REST)
    return opener


def tap(*key_ids):
    def act(ctx):
        for key in key_ids:
            ctx.click(ctx.find(within=keyboard(ctx), automationId=key, depth=4, timeout=10))
            time.sleep(SETTLE)
        ctx.hover_point(*REST)
    return act


# The contextual layers and the emoji layer show only for a focused field: Edge,
# as an app window over a local page, gives each field type its input scope.
FIELDS_TITLE = "Inventory keyboard fields"
FIELDS_PAGE = r"C:\ios\keyboard-fields.html"
FIELDS_HTML = ("<!doctype html><title>" + FIELDS_TITLE + "</title><body style='font:16px sans-serif'>"
               "<p><input aria-label='Text field' type='text'></p><p><input aria-label='Password field' type='password'>"
               "</p><p><input aria-label='Number field' type='number'></p><p><input aria-label='URL field' type='url'></p>")
EDGE_POLICY = r"HKLM:\SOFTWARE\Policies\Microsoft\Edge"
OPEN_FIELDS = (f"Set-Content -LiteralPath '{FIELDS_PAGE}' -Value \"{FIELDS_HTML}\" -Encoding UTF8; "
               f"New-Item -Path '{EDGE_POLICY}' -Force | Out-Null; "
               f"Set-ItemProperty -LiteralPath '{EDGE_POLICY}' -Name HideFirstRunExperience -Value 1 -Type DWord; "
               "Start-Process msedge.exe -ArgumentList '--no-first-run','--no-default-browser-check',"
               f"'--app=file:///{FIELDS_PAGE.replace(chr(92), '/')}'")


def fields_window(ctx, timeout=10):
    return ctx.find_window(className="Chrome_WidgetWin_1", namePrefix=FIELDS_TITLE, timeout=timeout, foreground=False)


def focus_field(name):
    def opener(ctx):
        try:
            fields_window(ctx, timeout=0)
        except core.Incomplete:
            ctx.powershell(OPEN_FIELDS)
            fields_window(ctx, timeout=30)
            time.sleep(SETTLE)
        ctx.click(ctx.find(within=fields_window(ctx), name=name, depth=12, timeout=20))
        time.sleep(LAYOUT_SETTLE)
        keyboard(ctx)
        ctx.hover_point(*REST)
    return opener


def close_fields(ctx):
    ctx.powershell("Stop-Process -Name msedge -Force -ErrorAction SilentlyContinue")
    ctx.gone(className="Chrome_WidgetWin_1", namePrefix=FIELDS_TITLE, timeout=15)


EMOJI_TOGGLE = "ExpressiveInputToggleButton"
EMOJI_WAIT = 8


def emoji_shown(ctx):
    """Whether the keyboard shows its emoji page: it comes seconds after the
    toggle (2026-11-18), as the panel's tab row above the keys."""
    try:
        ctx.find(within=keyboard(ctx), automationId="navigation-menu-container", depth=10, timeout=0)
        return True
    except core.Incomplete:
        return False


def wait_emoji(ctx, shown):
    deadline = time.monotonic() + EMOJI_WAIT
    while emoji_shown(ctx) != shown:
        if time.monotonic() > deadline:
            raise core.Incomplete(f"the keyboard's emoji page did not {'open' if shown else 'close'} "
                                  f"within {EMOJI_WAIT} s")
        time.sleep(1)


def emoji_layer(ctx):
    """The first tap after the field takes the focus can go unanswered
    (2026-11-18), so the toggle is tapped again, a bounded number of times."""
    focus_field("Text field")(ctx)
    for attempt in range(1, OPEN_TRIES + 1):
        tap(EMOJI_TOGGLE)(ctx)
        try:
            wait_emoji(ctx, True)
            break
        except core.Incomplete:
            if attempt == OPEN_TRIES:
                raise
    time.sleep(SETTLE)


def close_emoji(ctx):
    tap(EMOJI_TOGGLE)(ctx)
    wait_emoji(ctx, False)


SCENES = [
    dict(slug="history-off", label="Clipboard history, turned off", kind="state", open=open_panel("win+v"),
         root=panel, close=close_panel, expand=False),
    dict(slug="history-empty", label="Clipboard history, empty", kind="state", open=open_history_empty,
         root=panel, close=close_panel, expand=False),
    dict(slug="history", label="Clipboard history", kind="surface", open=open_history_items, root=panel,
         close=close_panel, expand=False),
]
SCENES += [dict(slug="history-item", label="Clipboard history item", kind="region", open=open_history_items,
                root=history_item, close=close_panel, expand=False),
           dict(slug="history-item-more", label="Clipboard history item, more", kind="state",
                open=open_item_more, root=history_item, close=close_panel, expand=False),
           dict(slug="panel-people", label="Emoji and more, people", kind="state", open=open_people, root=panel,
                close=close_panel, expand=False)]
SCENES += [dict(slug=f"panel-{seg}", label=f"Emoji and more, {name.lower()} tab", kind="state",
                open=open_panel("win+period", None if seg == "recent" else name), root=panel, close=close_panel,
                expand=False) for name, seg in TABS if seg != "clipboard"]
SCENES += [
    dict(slug="run", label="Run", kind="surface", open=open_run, root=run_dialog, close=close_run, expand=False),
    dict(slug="run-elevated", label="Run, elevated", kind="state", open=open_run_elevated, root=run_dialog,
         close=close_run, expand=False),
    dict(slug="capture-bar", label="Capture bar", kind="surface", open=open_capture_bar, root=capture_bar,
         close=close_capture_bar, expand=False),
    dict(slug="capture-modes", label="Capture bar mode menu", kind="menu", open=open_capture_modes, root=mode_box,
         close=close_capture_bar, expand=False),
    dict(slug="voice-typing", label="Voice typing", kind="surface", open=open_voice, root=voice_bar,
         close=close_voice, expand=False),
    dict(slug="voice-consent", label="Voice typing, speech service consent", kind="state", open=open_voice,
         root=voice_consent, close=close_voice, expand=False),
    dict(slug="voice-settings", label="Voice typing settings", kind="menu", open=open_voice_settings,
         root=voice_settings, close=close_voice, expand=False),
    dict(slug="power-user-menu", label="Power user menu", kind="menu", open=open_power_menu, root=power_menu,
         close=close_power_menu, expand=False),
]
SCENES += [
    dict(slug="touch-keyboard", label="Touch keyboard", kind="surface", open=open_keyboard, root=keyboard,
         close=keep_keyboard, expand=False),
    dict(slug="touch-keyboard-settings", label="Touch keyboard settings", kind="menu", open=open_settings,
         root=settings_menu, close=close_menus, expand=False),
    dict(slug="touch-keyboard-layouts", label="Touch keyboard layouts", kind="menu", open=open_layouts,
         root=layout_menu, close=close_menus, expand=False),
    dict(slug="touch-keyboard-shift", label="Touch keyboard, shifted", kind="state", open=tap("switchShift"),
         root=keyboard, close=tap("switchShift"), expand=False),
    dict(slug="touch-keyboard-symbols", label="Touch keyboard, symbols", kind="state", open=tap("switchSymbol"),
         root=keyboard, close=tap("switchABC"), expand=False),
    dict(slug="touch-keyboard-symbols-2", label="Touch keyboard, symbols page two", kind="state",
         open=tap("switchSymbol", "switch_Next_Page"), root=keyboard, close=tap("switchABC"), expand=False),
    dict(slug="touch-keyboard-emoji", label="Touch keyboard, emoji", kind="state", open=emoji_layer, root=keyboard,
         close=close_emoji, expand=False),
    dict(slug="touch-keyboard-password", label="Touch keyboard, password field", kind="state",
         open=focus_field("Password field"), root=keyboard, close=keep_keyboard, expand=False),
    dict(slug="touch-keyboard-number", label="Touch keyboard, number field", kind="state",
         open=focus_field("Number field"), root=keyboard, close=keep_keyboard, expand=False),
    dict(slug="touch-keyboard-url", label="Touch keyboard, URL field", kind="state", open=focus_field("URL field"),
         root=keyboard, close=close_fields, expand=False),
]
# Default comes back after Compact and Traditional as a measured scene only, so the
# layers start from it; Handwriting next; Gamepad last, because in Gamepad the
# settings menu opens to neither a click nor a toggle (2026-11-18).
SCENES[-8:-8] = [dict(slug=f"touch-keyboard-{seg}", label=f"Touch keyboard, {label.lower()} layout", kind="state",
                      open=choose(entry, layout_menu), root=keyboard, close=keep_keyboard, expand=False,
                      keep=seg != "default") for entry, seg, label in LAYOUTS if seg != "gamepad"]
SCENES += [
    dict(slug="touch-keyboard-handwriting", label="Touch keyboard, handwriting", kind="state",
         open=choose("ModalityHandwriting", settings_menu), root=keyboard, close=keep_keyboard, expand=False),
    dict(slug="touch-keyboard-gamepad", label="Touch keyboard, gamepad layout", kind="state",
         open=choose("ModalityKeyboard", layout_menu), root=keyboard, close=keep_keyboard, expand=False),
]


def METRICS(records):
    """The emoji panel's size, the touch keyboard's key pitch and row, the Win-X
    menu's entry height and the Run dialog's size, from captured bounds."""
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

    panel_box, q, w = box("history"), box("touch-keyboard.keyboardgrid.button-q"), box("touch-keyboard.keyboardgrid.button-w")
    entries = [r for r in records if r.get("parent") == p + "power-user-menu" and r.get("kind") == "menuItem"]
    if not entries:
        raise ValueError("the power user menu held no entry")
    run_box = box("run")
    return [metric("history", "panel-width", "Emoji and more panel width", 100, panel_box["width"]),
            metric("history", "panel-height", "Emoji and more panel height", 101, panel_box["height"]),
            # UI Automation gives a key its whole cell, flush with the next key's
            # (q to w: 69 px apart, 69 px wide); the drawn key sits inside it.
            metric("touch-keyboard", "key-pitch", "Touch keyboard key pitch", 100, w["x"] - q["x"]),
            metric("touch-keyboard", "key-row-height", "Touch keyboard key row height", 101, q["height"]),
            metric("power-user-menu", "entry-height", "Power user menu entry height", 100, entries[0]["bounds"]["height"]),
            metric("run", "dialog-width", "Run dialog width", 100, run_box["width"]),
            metric("run", "dialog-height", "Run dialog height", 101, run_box["height"])]


SHORTCUTS = [
    ("history", "win-v", "Open clipboard history", "Win+V",
     "Opens the Emoji and more panel on its clipboard tab."),
    ("panel-recent", "win-period", "Open the emoji panel", "Win+.",
     "Opens the Emoji and more panel on its most recently used tab; Win+; does the same."),
    ("run", "win-r", "Open Run", "Win+R", "Opens the Run dialog."),
    ("capture-bar", "win-shift-s", "Open the capture bar", "Win+Shift+S",
     "Freezes the screen and shows the capture bar at the top centre."),
    ("voice-typing", "win-h", "Start voice typing", "Win+H", "Opens the voice typing bar and starts listening."),
    ("power-user-menu", "win-x", "Open the power user menu", "Win+X",
     "Opens the power user menu above the Start button, as a right-click on Start does."),
    # The pages themselves are Quick Settings records (quick-settings.json).
    ("project-page", "win-p", "Open Project", "Win+P", "Opens Quick Settings on its Project page."),
    ("cast-page", "win-k", "Open Cast", "Win+K", "Opens Quick Settings on its Cast page."),
]

BEHAVIOURS = [
    ("panel-recent", "reopens-on-last-tab", "Reopens where it was left",
     "The panel opens again on the tab and category last used; a category page shows Back in place of the tab row."),
    ("touch-keyboard", "no-focus", "Never takes the focus",
     "The keyboard never takes the keyboard focus: keys type into the focused field, Esc does not reach its menus, "
     "and its settings button closes them."),
    ("touch-keyboard", "follows-the-field", "Follows the focused field",
     "The layer follows the focused field's input scope: a URL field adds / and .com keys, a number field opens on "
     "the numbers and symbols layer, a password field keeps the letters; an empty text field starts in capitals."),
    ("touch-keyboard", "clipboard-suggestion", "Suggests the clipboard",
     "Above the keys the keyboard offers the clipboard's newest text as a suggestion."),
    ("touch-keyboard-emoji", "emoji-page", "Emoji above the keys",
     "With a field focused, the emoji button shows the Emoji and more panel's tab row and lists above the keys; it "
     "does nothing with no field focused."),
    ("touch-keyboard-gamepad", "gamepad-keys", "Gamepad key names",
     "The gamepad layout names the controller button for each action key, such as Backspace on X."),
    ("capture-modes", "esc-closes-menu-first", "Esc closes the menu first",
     "With the mode menu open, Esc closes only the menu; a second Esc leaves the capture bar."),
]


def _hand():
    out = [{"id": f"{parent}.{slug}", "parent": parent, "kind": "shortcut", "label": label, "order": 120 + i,
            "shortcut": keys, "shortcutScope": "global", "behaviour": text}
           for i, (parent, slug, label, keys, text) in enumerate(SHORTCUTS)]
    out += [{"id": f"{parent}.{slug}", "parent": parent, "kind": "gesture", "label": label, "order": 140 + i,
             "behaviour": text} for i, (parent, slug, label, text) in enumerate(BEHAVIOURS)]
    return out


HAND = _hand()
