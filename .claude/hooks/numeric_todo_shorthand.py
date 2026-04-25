#!/usr/bin/env python3
import json, sys, re
d = json.load(sys.stdin)
ti = d.get('tool_input', {})
tn = d.get('tool_name', '')
path = ti.get('file_path', '')
if not path:
    sys.exit(0)
p = path.replace('\\', '/')
# Only .md files outside todo/ and .claude/ (and the PR template that teaches the shorthand)
if not p.endswith('.md'):
    sys.exit(0)
if '/todo/' in p or p.startswith('todo/'):
    sys.exit(0)
if '.claude/' in p:
    sys.exit(0)
if p.endswith('.github/PULL_REQUEST_TEMPLATE.md'):
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
all_text = '\n'.join(texts)
# Mirror scripts/lint.sh Check 4 regex for drift-prone shorthand
pat = re.compile(r'(TODO-\d+\s*§\d+|TODO-\d+\s+section\s+\d+|D\d+\s*T\d+\s*§?\d+|\bT\d+\s*§\d+)')
hits = pat.findall(all_text)
if not hits:
    sys.exit(0)
sys.stderr.write(
    '[numeric TODO shorthand BLOCK -- scripts/lint.sh Check 4] ' +
    'Detected drift-prone TODO shorthand in ' + path + ': ' +
    ', '.join(sorted(set(hits))[:5]) + '. ' +
    'Outside todo/** and .claude/** (where the shorthand is deliberate) cross-TODO refs must use anchor links or capability names -- e.g. [AI Workflow Regression Suite](../../todo/00-infrastructure/TODO-02-ai-development-system.md#9-ai-workflow-regression-suite) NOT "TODO-02 section 9". Numeric refs go stale silently on renumber. ' +
    'Fix: replace the shorthand with a named-anchor markdown link, then retry the edit. Same pattern that scripts/lint.sh Check 4 enforces at CI time; this hook catches it at edit time so the CI never rejects.'
)
sys.exit(2)
