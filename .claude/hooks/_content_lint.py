#!/usr/bin/env python3
"""Shared source-comment content-lint detectors (C3).

Single source of truth for the two source-comment gates -- `bare_section_refs.py`
(bare `section-sign` + digit) and `citation_block.py` (review-archaeology
citations). Both hooks import these so a single Write/Edit can surface BOTH
classes of violation at once (each gate cross-reports the other's findings),
collapsing the block-then-fix gauntlet on a new kernel/boot file into one pass.

Each detector is APPLICABILITY-AWARE: it returns [] when the check does not
apply to `path`, so a hook can cross-report the other check safely without
duplicating the path rules. Applicability + regexes mirror scripts/lint.sh
Check 5 / Check 13 (keep in sync).
"""
import os
import pathlib
import re

# ---- bare section-sign refs (mirror bare_section_refs.py / lint Check 5) ----
_BARE_RE = re.compile('§ ?[0-9]')
_SPEC_RE = re.compile(
    r'UEFI|Intel|SDM|AMD|APM|RFC [0-9]|ACPI [0-9]|NTFS|FAT[0-9]|NVMe|PCIe?|'
    r'PE/COFF|PE32|COFF|USB [0-9]|xHCI|EHCI|OHCI|UHCI|VirtIO|SMBIOS|IEEE|NIST|'
    r'TCG|WHEA|HPET|MP Spec|spec |specification')
_BARE_SUFFIXES = ('.c', '.h', '.asm', '.S', '.py', '.sh', '.bat', '.ps1',
                  '.yml', '.yaml', '.mk', '.ld', '.lds', '.inc')

# ---- review/task citations (mirror citation_block.py / lint Check 13) ----
_CITE_RE = re.compile(
    r'(\bCodex\b[^A-Za-z0-9_]{1,40}([HMFCS]\d|fix|review|finding|round|'
    r'adversarial|consistency)'
    r'|review caught|adversarial review|consistency review|design review'
    r'|post-impl review|incident 20\d{2}-\d{2}-\d{2}|see commit [0-9a-f]{7,}'
    r'|per Codex review)')
_ALLOW_RE = re.compile(r'CITATION-OK:\s*[A-Za-z]')
_CITE_SUFFIXES = ('.c', '.h', '.cc', '.cpp', '.hpp', '.asm', '.S', '.s')


def _repo_prefixes():
    """Absolute repo roots to strip, longest first, plus `./`.

    DERIVED, not hardcoded (2026-08-08). The literal here read
    `/home/derickpayne/impossible-os/` -- one path component short of the real
    checkout (`.../projects/impossible-os/`) -- so it stripped NOTHING, and
    every `norm`-based exemption below silently stopped applying to ABSOLUTE
    paths. That is the shape a hook actually receives: `tool_input.file_path` is
    absolute, so `scripts/todo-graph/`, `src/kernel/fs/ntfs/`,
    `scripts/test-ai-system.sh`, `include/stb_truetype.h` and `src/libs/` were
    all being judged as ordinary code at edit time while `scripts/lint.sh`
    Check 5 exempted them at commit time. Filed by the run as "the hook has no
    such case"; the case was there, the normalisation under it was not.

    This module sits at `<root>/.claude/hooks/`, so the root is two parents up;
    `CLAUDE_PROJECT_DIR` wins when the harness sets it. A stale literal cannot
    recur because nothing is written down.
    """
    roots = []
    env = os.environ.get('CLAUDE_PROJECT_DIR')
    if env:
        roots.append(env)
    try:
        roots.append(str(pathlib.Path(__file__).resolve().parents[2]))
    except (IndexError, OSError):
        pass
    out = []
    for r in roots:
        r = r.replace('\\', '/').rstrip('/')
        if r:
            out.append(r + '/')
    out.append('./')
    return sorted(set(out), key=len, reverse=True)


def _norm(path):
    p = path.replace('\\', '/')
    for prefix in _repo_prefixes():
        if p.startswith(prefix):
            return p, p[len(prefix):]
    return p, p


def bare_section_applies(path):
    p, norm = _norm(path)
    if p.endswith('.md'):
        return False
    if '/todo/' in p or p.startswith('todo/') or '.claude/' in p:
        return False
    if norm.startswith('src/kernel/fs/ntfs/') or norm == 'include/kernel/fs/ntfs.h':
        return False
    if norm == 'scripts/test-ai-system.sh' or norm.startswith('scripts/todo-graph/'):
        return False
    basename = norm.rsplit('/', 1)[-1]
    return basename == 'Makefile' or p.endswith(_BARE_SUFFIXES)


def bare_section_hits(texts, path):
    """Offending line snippets for the bare-section-ref check, or [] if the
    check does not apply to `path`. `texts` is a list of new-text payloads."""
    if not bare_section_applies(path):
        return []
    hits = []
    for line in '\n'.join(texts).splitlines():
        if _BARE_RE.search(line) and not _SPEC_RE.search(line):
            hits.append(line.strip()[:80])
    return hits


def citation_applies(path):
    p, norm = _norm(path)
    if not p.endswith(_CITE_SUFFIXES):
        return False
    if norm == 'include/stb_truetype.h' or norm.startswith('src/libs/'):
        return False
    return True


def _line_is_comment(line):
    s = line.lstrip()
    if s.startswith(('*', '//', '/*', ';')):
        return True
    if '//' in line:
        return _CITE_RE.search(line[line.find('//'):]) is not None
    return False


def citation_hits(texts, path):
    """Offending line snippets for the citation check, or [] if the check does
    not apply to `path`."""
    if not citation_applies(path):
        return []
    hits = []
    for txt in texts:
        for line in txt.splitlines():
            if not _CITE_RE.search(line):
                continue
            if not _line_is_comment(line) or _ALLOW_RE.search(line):
                continue
            hits.append(line.strip()[:100])
    return hits
