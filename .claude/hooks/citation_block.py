#!/usr/bin/env python3
"""PreToolUse hook -- block review/task citations in source comments.

Mirrors scripts/lint.sh Check 13. C / asm / header comments must not carry
review-archaeology like "(Codex H1 ...)", "Codex M3 fix:", "incident
YYYY-MM-DD", "see commit <hash>", or "per Codex review". Detection + path
applicability live in the shared `_content_lint` module (kept in sync with the
bare-section gate; they cross-report each other's findings -- C3).

Opt-out: SKIP_CITATION_BLOCK=1 on the same call AND state in your next message
what code-evidence quote justifies skipping.
"""
import json
import os
import sys

import _content_lint as cl

if os.environ.get('SKIP_CITATION_BLOCK') == '1':
    sys.exit(0)

d = json.load(sys.stdin)
ti = d.get('tool_input', {})
tn = d.get('tool_name', '')
path = ti.get('file_path', '')
if not path:
    sys.exit(0)

texts = []
if tn == 'Write':
    texts.append(ti.get('content', ''))
elif tn == 'Edit':
    texts.append(ti.get('new_string', ''))
elif tn == 'MultiEdit':
    for e in ti.get('edits', []) or []:
        texts.append(e.get('new_string', ''))
if not any(texts):
    sys.exit(0)

hits = cl.citation_hits(texts, path)
if not hits:
    sys.exit(0)

# C3 cross-report: if the same payload also trips the bare-section gate, name
# those lines HERE so both classes are fixed in one edit, not a gauntlet.
bare_also = cl.bare_section_hits(texts, path)
cross = ('ALSO (bare section-ref gate, fix in the SAME edit): '
         + ' | '.join(bare_also[:3]) + '. ') if bare_also else ''

sys.stderr.write(
    '[citation BLOCK -- scripts/lint.sh Check 13] '
    'Detected review/task citation in source comment of ' + path + ': '
    + ' | '.join(hits[:3]) + '. '
    'Code comments must not carry review-archaeology (Codex Mn/Hn, '
    '"adversarial review", "post-impl review", "incident YYYY-MM-DD", '
    '"see commit <hash>", "per Codex review", etc.). '
    'Per CLAUDE.md: "Don\'t reference the current task, fix, or callers '
    '... those belong in the PR description and rot as the codebase '
    'evolves." Keep the WHY (the invariant the code enforces) and '
    'delete the attribution. '
    + cross +
    'NOTE: this Edit did NOT apply -- the file is UNCHANGED; retry with the '
    'SAME old_string and only fix new_string (re-Reading and reconstructing '
    'old_string is what causes the "String to replace not found" cascade). '
    'Opt-out for legitimate uses (rare): SKIP_CITATION_BLOCK=1 on the '
    'same call AND state in chat what code evidence justifies skipping. '
    'Same pattern scripts/lint.sh Check 13 enforces at CI time; this '
    'hook catches it at edit time so CI never rejects.'
)
sys.exit(2)
