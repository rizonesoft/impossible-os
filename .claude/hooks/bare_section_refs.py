#!/usr/bin/env python3
"""PreToolUse hook -- block bare `section-sign` + digit refs in source comments.

Mirrors scripts/lint.sh Check 5. Detection + path applicability live in the
shared `_content_lint` module so this gate and the citation gate stay in sync and
can cross-report each other's findings (C3: one block surfaces both classes).
"""
import json
import sys

import os
import pathlib

import _content_lint as cl

d = json.load(sys.stdin)
ti = d.get('tool_input', {})
tn = d.get('tool_name', '')
path = ti.get('file_path', '')
if not path:
    sys.exit(0)

# SCOPE: tracked code only. This mirrors scripts/lint.sh Check 5, and Check 5
# scans the REPO -- so a file outside it can never be the drift this rule
# exists to prevent. Blocking there is pure cost.
#
# Observed 2026-08-07: a throwaway /tmp helper written only to PATCH a TODO was
# refused because its string literals carried the section glyph plus a digit --
# text that is legal, and required, in the Markdown it was writing. The run
# worked around it by composing the glyph as chr(0xA7), which produces
# byte-identical output while satisfying the hook. A gate that a one-line
# expression defeats, on a file the corresponding lint never reads, is costing
# keystrokes without adding safety.
#
# Resolved against the repo root rather than trusting the string, so `..`
# traversal cannot smuggle a tracked file out of scope.
try:
    _root = pathlib.Path(
        os.environ.get('CLAUDE_PROJECT_DIR') or pathlib.Path.cwd()).resolve()
    if not pathlib.Path(path).resolve().is_relative_to(_root):
        sys.exit(0)
except (OSError, ValueError):
    pass  # unresolvable path: fall through and judge it, fail-closed

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

hits = cl.bare_section_hits(texts, path)
if not hits:
    sys.exit(0)

# C3 cross-report: if the same payload also trips the citation gate, name those
# lines HERE so both classes are fixed in one edit, not a sequential gauntlet.
cite_also = cl.citation_hits(texts, path)
cross = ('ALSO (citation gate, fix in the SAME edit): '
         + ' | '.join(cite_also[:3]) + '. ') if cite_also else ''

sys.stderr.write(
    '[bare section ref BLOCK -- scripts/lint.sh Check 5] ' +
    'Detected bare section-sign reference in code file ' + path + ': ' +
    ' | '.join(hits[:3]) + '. ' +
    'Code comments must name the FEATURE (e.g. "capability negotiation", "typed payload descriptors"), not a bare section number. ' +
    'Section numbers drift silently on TODO renumber and carry no TODO/domain context. ' +
    'External-spec citations stay legal when the line also carries a qualifier (UEFI, Intel, SDM, AMD, RFC <n>, ACPI <n>, NTFS, FAT<n>, NVMe, PCI/PCIe, PE/COFF, PE32, USB <n>, xHCI/EHCI/OHCI/UHCI, VirtIO, SMBIOS, IEEE, NIST, TCG, WHEA, HPET, MP Spec, "spec ", "specification"), and whole-file spec-code dirs (src/kernel/fs/ntfs/) are path-exempted. ' +
    'Fix: rewrite the comment to name the feature; OR prefix with an external-spec qualifier (e.g. "UEFI 2.10 section 4.6"); OR replace with a markdown-doc link. ' +
    cross +
    'NOTE: this Edit did NOT apply -- the file is UNCHANGED; retry with the SAME old_string and only fix new_string (re-Reading and reconstructing old_string is what causes the "String to replace not found" cascade). ' +
    'Same pattern scripts/lint.sh Check 5 enforces at CI time; this hook catches it at edit time so CI never rejects. ' +
    'See feedback_no_bare_section_refs_in_code memory and CLAUDE.md "Comments -- No Bare Section Refs in Code".'
)
sys.exit(2)
