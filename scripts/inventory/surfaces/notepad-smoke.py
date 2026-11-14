"""Surface module: Notepad, the end-to-end check of the capture tool.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 3. Notepad's
own inventory is section 13's; this module proves the tool against a real app
and is never published (capture.py --dry-run).
"""
SURFACE = "notepad-smoke"
ROOT_SLUG = "window"
ROOT_LABEL = "Notepad"       # the title holds the document name, which is data
ROOT_KIND = "surface"
# The tab strip lists open documents by file name: the machine's own data.
ENVIRONMENT_LISTS = [{"controlType": "Tab"}]
# The status bar shows the caret position, the character count, the zoom, the
# line endings and the encoding of the open document: live values, not labels.
VALUES = [{"automationId": "ContentTextBlock"}]
IDENTITY = []


def open(ctx):
    ctx.launch("notepad.exe")


def root(ctx):
    return ctx.find_window(className="Notepad")


def close(ctx):
    root(ctx)              # brings it to the foreground
    ctx.keys("alt+f4")     # a new, empty document closes without a prompt
