#!/usr/bin/env python3
"""Check that every CI job that builds the OS restores the compile cache.

Usage: check-compile-cache.py [workflows-dir]
       check-compile-cache.py --selftest [workflows-dir]

A job in build.yml or visual-regression.yml that runs `bash scripts/build.sh`
must, before that step, restore ~/.cache/ccache and point CCACHE_DIR at it.
The compile-cache step is the cache step whose path is that directory. It is
held to an exact shape, not to a pattern: its keys and its inputs are a fixed
set (so no `if`, no `lookup-only`), its key ends in the commit and its restore
key is that key's prefix. The step that sets CCACHE_DIR must be exactly
CONFIG_RUN. Exact matches are deliberate: a rule that pattern matched shell
text passed a commented-out write and a single-quoted $HOME.

Other caches are not held to these rules, but no cache step anywhere may name
an absolute path: unprivileged tar cannot restore a root-owned one (that
failed the job on 2026-06-11), so only home and workspace-relative paths
pass. release.yml must not restore any cache: a release is compiled from
source.

Exit: 0 clean, 1 a rule is broken (each named on stderr), 2 unreadable input.

--selftest copies the real workflows, breaks one rule at a time and requires
the check to refuse each copy with that rule's own message.
"""
import shutil
import sys
import tempfile
from pathlib import Path

import yaml

CACHE_PATH = "~/.cache/ccache"
CACHED = ("build.yml", "visual-regression.yml")
UNCACHED = ("release.yml",)


def steps_of(job):
    return [s for s in (job.get("steps") or []) if isinstance(s, dict)]


def is_cache(step):
    """Any step that restores a cache: actions/cache and actions/cache/restore."""
    uses = str(step.get("uses", ""))
    return uses.startswith(("actions/cache@", "actions/cache/restore@"))


def cache_paths(step):
    return [p.strip() for p in str((step.get("with") or {}).get("path", "")).splitlines() if p.strip()]


# The whole `run` text of the step that carries CCACHE_DIR to the build.
CONFIG_RUN = ('echo "CCACHE_DIR=$HOME/.cache/ccache" >> "$GITHUB_ENV"\n'
              'CCACHE_DIR="$HOME/.cache/ccache" ccache --zero-stats')
CACHE_STEP_KEYS = {"name", "uses", "continue-on-error", "with"}
CACHE_WITH_KEYS = {"path", "key", "restore-keys"}


def check_job(name, job, problems):
    steps = steps_of(job)
    builds = [i for i, s in enumerate(steps) if "bash scripts/build.sh" in str(s.get("run", ""))]
    if not builds:
        return
    before = steps[: builds[0]]
    # Installing ccache is not checked here: CONFIG_RUN runs `ccache`, so a job
    # that does not install it fails at that step, loudly, on its first run.
    # The compile cache is the cache step that names the ccache directory. Any
    # other cache a job restores is its own business (the root-owned-path rule
    # covers it).
    caches = [s for s in before if is_cache(s) and CACHE_PATH in cache_paths(s)]
    if not caches:
        problems.append(f"{name}: no actions/cache step for {CACHE_PATH} before the build")
    for s in caches:
        w = s.get("with") or {}
        if cache_paths(s) != [CACHE_PATH]:
            problems.append(f"{name}: cache path is {cache_paths(s)}, expected [{CACHE_PATH}]")
        if not str(s.get("uses", "")).startswith("actions/cache@"):
            problems.append(f"{name}: the compile cache is restored but never saved (use actions/cache)")
        extra = sorted((set(s) - CACHE_STEP_KEYS) | (set(w) - CACHE_WITH_KEYS))
        if extra:
            problems.append(f"{name}: the compile-cache step carries {extra}; only the fixed shape is accepted")
        key = str(w.get("key", ""))
        suffix = "${{ github.sha }}"
        if not key.endswith(suffix):
            problems.append(f"{name}: cache key does not end in the commit ({key})")
        restore = [k.strip() for k in str(w.get("restore-keys", "")).splitlines() if k.strip()]
        if key[: -len(suffix)] not in restore:
            problems.append(f"{name}: no restore key equal to the key without its commit")
        if s.get("continue-on-error") is not True:
            problems.append(f"{name}: the cache step can fail the job (continue-on-error is not true)")
    if not any(set(s) == {"name", "run"} and str(s["run"]).strip() == CONFIG_RUN for s in before):
        problems.append(f"{name}: CCACHE_DIR is not set to the restored directory before the build")


def workflows(root):
    """Every workflow file: GitHub reads both .yml and .yaml."""
    return sorted(f for f in root.iterdir() if f.suffix in (".yml", ".yaml"))


def check(root):
    """Return (exit status, messages) for the workflows under `root`."""
    problems = []
    try:
        for wf in CACHED:
            doc = yaml.safe_load((root / wf).read_text(encoding="utf-8"))
            built = 0
            for jname, job in (doc.get("jobs") or {}).items():
                check_job(f"{wf}:{jname}", job, problems)
                if any("bash scripts/build.sh" in str(s.get("run", "")) for s in steps_of(job)):
                    built += 1
            if built == 0:
                problems.append(f"{wf}: no job runs bash scripts/build.sh (the check has nothing to hold)")
        for wf in UNCACHED:
            doc = yaml.safe_load((root / wf).read_text(encoding="utf-8"))
            for jname, job in (doc.get("jobs") or {}).items():
                if any(is_cache(s) for s in steps_of(job)):
                    problems.append(f"{wf}:{jname}: a release job must not restore a cache")
        for wf in workflows(root):
            doc = yaml.safe_load(wf.read_text(encoding="utf-8"))
            for jname, job in ((doc or {}).get("jobs") or {}).items():
                for s in steps_of(job):
                    if not str(s.get("uses", "")).startswith("actions/cache"):
                        continue
                    for p in cache_paths(s):
                        # Home (~/) and workspace-relative paths only. Any absolute
                        # path is refused: a list of root-owned prefixes missed /root.
                        if p.startswith("/") or p.startswith("!/"):
                            problems.append(f"{wf.name}:{jname}: cache path {p} is absolute "
                                            "(a root-owned path cannot be restored)")
    except (OSError, yaml.YAMLError, AttributeError, TypeError, ValueError) as exc:   # ValueError: not UTF-8
        return 2, [f"cannot read the workflows: {exc}"]
    return (1 if problems else 0), problems


WRITE_DIR = 'echo "CCACHE_DIR=$HOME/.cache/ccache" >> "$GITHUB_ENV"'
CACHE_STEP_HEAD = "      - name: Restore compile cache\n        uses: actions/cache@v4\n"

# (name, file, text to find, replacement, expected exit, expected message part).
# Each breaks exactly one rule in an otherwise valid copy.
MUTATIONS = [
    ("no cache step", "build.yml", "        uses: actions/cache@v4\n", "        uses: actions/checkout@v4\n", 1,
     "no actions/cache step for"),
    ("wrong path", "build.yml", "          path: ~/.cache/ccache\n", "          path: ~/.ccache\n", 1,
     "no actions/cache step for"),
    ("a second path in the compile cache", "build.yml", "          path: ~/.cache/ccache\n",
     "          path: |\n            ~/.cache/ccache\n            build\n", 1, "cache path is"),
    ("compile cache skipped", "build.yml", CACHE_STEP_HEAD, CACHE_STEP_HEAD + "        if: false\n", 1,
     "only the fixed shape"),
    ("lookup-only compile cache", "build.yml", "          path: ~/.cache/ccache\n",
     "          path: ~/.cache/ccache\n          lookup-only: true\n", 1, "only the fixed shape"),
    ("CCACHE_DIR write commented out", "build.yml", WRITE_DIR,
     '# echo "CCACHE_DIR=$HOME/.cache/ccache" >> "$GITHUB_ENV"', 1, "CCACHE_DIR is not set"),
    ("CCACHE_DIR write behind a shell conditional", "build.yml",
     WRITE_DIR,
     'if false; then echo "CCACHE_DIR=$HOME/.cache/ccache" >> "$GITHUB_ENV"; fi', 1, "CCACHE_DIR is not set"),
    ("CCACHE_DIR with a literal $HOME", "build.yml", WRITE_DIR,
     "echo 'CCACHE_DIR=$HOME/.cache/ccache' >> \"$GITHUB_ENV\"", 1, "CCACHE_DIR is not set"),
    ("config step skipped", "build.yml", "      - name: Configure compile cache\n",
     "      - name: Configure compile cache\n        if: false\n", 1, "CCACHE_DIR is not set"),
    ("restore-only compile cache", "build.yml", "        uses: actions/cache@v4\n",
     "        uses: actions/cache/restore@v4\n", 1, "never saved"),
    ("CCACHE_DIR exported in its own step only", "build.yml", WRITE_DIR,
     "export CCACHE_DIR=$HOME/.cache/ccache", 1, "CCACHE_DIR is not set"),
    ("release restores with cache/restore", "release.yml", "uses: actions/checkout@v4",
     "uses: actions/cache/restore@v4", 1, "a release job must not restore a cache"),
    ("root-owned path through cache/restore", "pages.yml", "    steps:\n",
     "    steps:\n      - uses: actions/cache/restore@v4\n        with:\n          path: /opt/x\n          key: k\n",
     1, "is absolute"),
    ("not UTF-8", "build.yml", None, b"\xff\xfe jobs", 2, "cannot read the workflows"),
    # An unrelated cache beside a correct compile cache is allowed.
    ("unrelated cache", "build.yml", CACHE_STEP_HEAD,
     "      - uses: actions/cache@v4\n        with:\n          path: ~/.npm\n          key: npm\n"
     + CACHE_STEP_HEAD, 0, ""),
    ("key without the commit", "build.yml", "-tests-on-${{ github.sha }}\n", "-tests-on-${{ github.ref }}\n", 1,
     "does not end in the commit"),
    ("no restore prefix", "build.yml", "-tests-on-\n", "-other-\n", 1, "no restore key equal"),
    ("cache step can fail the job", "build.yml",
     "        continue-on-error: true\n        with:\n          path: ~/.cache/ccache",
     "        with:\n          path: ~/.cache/ccache", 1, "can fail the job"),
    ("CCACHE_DIR unset", "build.yml", WRITE_DIR, "true", 1,
     "CCACHE_DIR is not set"),
    ("release uses a cache", "release.yml", "uses: actions/checkout@v4", "uses: actions/cache@v4", 1,
     "a release job must not restore a cache"),
    ("root-owned path elsewhere", "pages.yml", "    steps:\n",
     "    steps:\n      - uses: actions/cache@v4\n        with:\n          path: /usr/lib/llvm-19\n          key: k\n",
     1, "is absolute"),
    ("root's own cache directory", "pages.yml", "    steps:\n",
     "    steps:\n      - uses: actions/cache@v4\n        with:\n"
     "          path: /root/.cache/ccache\n          key: k\n",
     1, "is absolute"),
    ("absolute path in a .yaml workflow", "probe.yaml", None,
     "jobs:\n  j:\n    steps:\n      - uses: actions/cache@v4\n        with:\n"
     "          path: /usr/lib/llvm-19\n          key: k\n", 1, "is absolute"),
    ("no building job", "visual-regression.yml", "bash scripts/build.sh", "bash scripts/other.sh", 1,
     "no job runs bash scripts/build.sh"),
    ("malformed YAML", "build.yml", "jobs:\n", "jobs: [\n", 2, "cannot read the workflows"),
    ("not a mapping", "build.yml", None, "- a\n- b\n", 2, "cannot read the workflows"),
]


def selftest(root):
    status, msgs = check(root)
    if status != 0:
        print(f"selftest: the real workflows do not pass ({msgs})", file=sys.stderr)
        return 1
    failed = 0
    for name, wf, find, repl, want, part in MUTATIONS:
        with tempfile.TemporaryDirectory() as tmp:
            for f in workflows(root):
                shutil.copy(f, tmp)
            target = Path(tmp) / wf
            text = target.read_text(encoding="utf-8") if target.exists() else ""
            if find is None:
                text = repl
            elif find not in text:
                print(f"selftest: {name}: the text to break is not in {wf}", file=sys.stderr)
                failed += 1
                continue
            else:
                text = text.replace(find, repl, 1)
            if isinstance(text, bytes):
                target.write_bytes(text)
            else:
                target.write_text(text, encoding="utf-8")
            status, msgs = check(Path(tmp))
        if status != want or (want != 0 and not any(part in m for m in msgs)):
            print(f"selftest: {name}: exit {status} {msgs}, wanted exit {want} naming '{part}'", file=sys.stderr)
            failed += 1
    with tempfile.TemporaryDirectory() as tmp:   # a required workflow is missing
        status, msgs = check(Path(tmp))
        if status != 2:
            print(f"selftest: missing workflows: exit {status}, wanted 2", file=sys.stderr)
            failed += 1
    print(f"selftest: {len(MUTATIONS) + 1 - failed} of {len(MUTATIONS) + 1} refusals correct")
    return 1 if failed else 0


def main(argv):
    if argv and argv[0] == "--selftest":
        return selftest(Path(argv[1] if len(argv) > 1 else ".github/workflows"))
    status, msgs = check(Path(argv[0] if argv else ".github/workflows"))
    for m in msgs:
        print(f"check-compile-cache: {m}", file=sys.stderr)
    if status == 0:
        print("check-compile-cache: every building job restores the compile cache")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
