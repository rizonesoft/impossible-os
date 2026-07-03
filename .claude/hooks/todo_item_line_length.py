#!/usr/bin/env python3
"""TODO item-line length cap.

Blocks edits that introduce TODO checklist items (`- [ ]` / `- [x]` / `- [/]`)
longer than ITEM_CHAR_CAP. The rule: items are scannable one-line summaries;
file:line citations, commit hashes, fix-loop adoption traces, Codex round
counts, and session-date debug logs belong in COMMIT MESSAGES, not in the
TODO bullet itself.

See feedback_terse_comments_and_notes + feedback_todo_notes_brevity memories.

Scope: only fires on .md files under todo/. Whitelists `Commit:` items
(short by convention) and OS Comparison rows (200-char cap is enforced
elsewhere by validate-todo-file).
"""
import json
import re
import sys

ITEM_CHAR_CAP = 250

d = json.load(sys.stdin)
ti = d.get('tool_input', {})
tn = d.get('tool_name', '')
path = ti.get('file_path', '')
if not path.endswith('.md'):
    sys.exit(0)
p = path.replace('\\', '/')
if 'todo/' not in p and not p.startswith('todo/'):
    sys.exit(0)

texts = []
if tn == 'Write':
    texts.append(ti.get('content', ''))
elif tn == 'Edit':
    texts.append(ti.get('new_string', ''))
elif tn == 'MultiEdit':
    for e in ti.get('edits', []) or []:
        texts.append(e.get('new_string', ''))
if not texts:
    sys.exit(0)

# Match top-level checklist items only; nested bullets under a parent
# `- [ ]` are detail and don't get a length cap.
ITEM_RE = re.compile(r'^- \[[ x/]\] ')

findings = []  # list of (line_no, length, preview)
for txt in texts:
    for i, line in enumerate(txt.splitlines(), 1):
        if not ITEM_RE.match(line):
            continue
        # Whitelist: `- [x] Commit:` / `- [ ] Commit:` (always short).
        if 'Commit:' in line[:30]:
            continue
        if len(line) <= ITEM_CHAR_CAP:
            continue
        preview = line[:90] + '...'
        findings.append((i, len(line), preview))

if not findings:
    sys.exit(0)

parts = []
for ln, length, preview in findings:
    parts.append('line ' + str(ln) + ' (' + str(length) + ' chars, over by '
                 + str(length - ITEM_CHAR_CAP) + '): ' + preview)

sys.stderr.write(
    '[todo-item-line BLOCK -- TODO item brevity discipline] '
    'Edit to ' + path + ' introduces ' + str(len(findings)) +
    ' checklist item(s) longer than ' + str(ITEM_CHAR_CAP) + ' chars: ' +
    ' || '.join(parts) + '. '
    'Items are scannable one-line summaries; file:line citations, commit '
    'hashes, fix-loop adoption traces, Codex round counts, and session-date '
    'investigation logs belong in COMMIT MESSAGES, not the bullet. '
    'Compress to one sentence naming what shipped + the canonical doc/file '
    'where details live. '
    'BEFORE RETRYING: count the rewritten line -- '
    "python3 -c 'print(len(\"<the exact line>\"))' -- and aim for <= "
    + str(ITEM_CHAR_CAP - 20) + ' so margin survives small tweaks; '
    'roughly a third of retries were STILL over the cap because the trim '
    'was done by feel. See feedback_terse_comments_and_notes memory.'
)
sys.exit(2)
