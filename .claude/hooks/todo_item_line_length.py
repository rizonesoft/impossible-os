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
# Match top-level checklist items only; nested bullets under a parent
# `- [ ]` are detail and don't get a length cap.
ITEM_RE = re.compile(r'^- \[[ x/]\] ')


def _check(line):
    """C1: deterministic fit query. `--check "<line>"` -> one JSON line
    {len, cap, overage, is_checklist_item, whitelisted, ok} so a rewrite is
    confirmed in ONE call instead of a blind manual `len()` recount loop."""
    is_item = bool(ITEM_RE.match(line))
    whitelisted = 'Commit:' in line[:30]
    n = len(line)
    ok = (not is_item) or whitelisted or n <= ITEM_CHAR_CAP
    return {'len': n, 'cap': ITEM_CHAR_CAP, 'overage': max(0, n - ITEM_CHAR_CAP),
            'is_checklist_item': is_item, 'whitelisted': whitelisted, 'ok': ok}


if len(sys.argv) >= 2 and sys.argv[1] == '--check':
    print(json.dumps(_check(sys.argv[2] if len(sys.argv) > 2 else '')))
    sys.exit(0)

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
    'NOTE: this Edit did NOT apply -- the file is UNCHANGED. Retry with the '
    'SAME old_string and only shorten new_string (do NOT re-Read and '
    'reconstruct old_string -- that is what causes the "String to replace not '
    'found" cascade). CONFIRM THE FIT IN ONE CALL: '
    'python3 .claude/hooks/todo_item_line_length.py --check "<the rewritten '
    'line>" returns {len, overage, cap, ok}; aim for len <= '
    + str(ITEM_CHAR_CAP - 20) + ' so margin survives small tweaks. No blind '
    'manual len() recount loop. See feedback_terse_comments_and_notes memory.'
)
sys.exit(2)
