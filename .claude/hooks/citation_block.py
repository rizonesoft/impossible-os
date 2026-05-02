#!/usr/bin/env python3
"""PreToolUse hook -- block edits that introduce review/task citations.

Mirrors scripts/lint.sh Check 13.  C / asm / header comments must not
carry review-archaeology like "(Codex H1 ...)", "Codex M3 fix:",
"incident YYYY-MM-DD", "see commit <hash>", or "per Codex review".
Per CLAUDE.md "Don't reference the current task, fix, or callers ...
those belong in the PR description and rot as the codebase evolves."

Opt-out: SKIP_CITATION_BLOCK=1 on the same call AND state in your
next message what code-evidence quote justifies skipping.
"""
import json, os, re, sys

if os.environ.get('SKIP_CITATION_BLOCK') == '1':
    sys.exit(0)

d = json.load(sys.stdin)
ti = d.get('tool_input', {})
tn = d.get('tool_name', '')
path = ti.get('file_path', '')
if not path:
    sys.exit(0)

p = path.replace('\\', '/')
# Only police source code.  Markdown / TODO / .claude / hook code may
# legitimately reference review history (the hooks themselves talk
# about Codex review reception, etc.).
if not p.endswith(('.c', '.h', '.cc', '.cpp', '.hpp', '.asm', '.S', '.s')):
    sys.exit(0)

# Vendored / third-party paths we don't own.
norm = p
for prefix in ('/home/derickpayne/impossible-os/', './'):
    if norm.startswith(prefix):
        norm = norm[len(prefix):]
        break
if norm == 'include/stb_truetype.h' or norm.startswith('src/libs/'):
    sys.exit(0)

# Collect the new-text payload from the edit.
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

# "Codex" alone is too loose -- it matches the OpenAI product name in
# legitimate API/SDK references.  Require a citation cue word within
# ~40 chars of the bare token to count as a real citation.
cite_re = re.compile(
    r'(\bCodex\b[^A-Za-z0-9_]{1,40}([HMFCS]\d|fix|review|finding|round|adversarial|consistency)'
    r'|review caught'
    r'|adversarial review'
    r'|consistency review'
    r'|design review'
    r'|post-impl review'
    r'|incident 20\d{2}-\d{2}-\d{2}'
    r'|see commit [0-9a-f]{7,}'
    r'|per Codex review)'
)

# Only flag lines that look like comments. Don't trigger on identifiers.
def line_is_comment(line: str) -> bool:
    s = line.lstrip()
    if s.startswith('*') or s.startswith('//') or s.startswith('/*') or s.startswith(';'):
        return True
    # Inline trailing // comment: anything after // is a comment.
    if '//' in line:
        # Citation must appear after the // marker.
        idx = line.find('//')
        return cite_re.search(line[idx:]) is not None
    return False

allow_re = re.compile(r'CITATION-OK:\s*[A-Za-z]')

hits = []
for txt in texts:
    for line in txt.splitlines():
        if not cite_re.search(line):
            continue
        if not line_is_comment(line):
            continue
        if allow_re.search(line):
            continue
        hits.append(line.strip()[:100])

if not hits:
    sys.exit(0)

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
    'Opt-out for legitimate uses (rare): SKIP_CITATION_BLOCK=1 on the '
    'same call AND state in chat what code evidence justifies skipping. '
    'Same pattern scripts/lint.sh Check 13 enforces at CI time; this '
    'hook catches it at edit time so CI never rejects.'
)
sys.exit(2)
