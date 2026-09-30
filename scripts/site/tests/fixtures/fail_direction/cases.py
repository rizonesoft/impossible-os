"""Control fixture for `check_nets.py --rule fail-direction` (never imported or run).

Every line marked `# expect: flag` must be reported and every line marked
`# expect: clean` must not; an unmarked line that is reported fails the control.
"""
import contextlib
import json
import os
import os.path
import subprocess
from contextlib import suppress
from pathlib import Path


def swallowed_read(path: Path) -> list:
    try:
        return json.loads(path.read_text())
    except OSError:
        return []  # expect: flag


def every_empty_spelling(path: Path):
    try:
        data = path.read_text()
    except OSError:
        return  # expect: flag
    try:
        data = json.loads(data)
    except ValueError:
        return None  # expect: flag
    try:
        data = data["x"]
    except KeyError:
        data = {}  # expect: flag
    try:
        data = data["y"]
    except KeyError:
        return ()  # expect: flag
    try:
        data = data["z"]
    except KeyError:
        return frozenset()  # expect: flag
    try:
        data = data.encode()
    except AttributeError:
        return b""  # expect: flag
    return data


def missing_manifest(store: Path) -> dict:
    if not (store / "versions.json").is_file():
        return {}  # expect: flag
    return json.loads((store / "versions.json").read_text())


def absent_branch(ref: str) -> set:
    r = subprocess.run(["git", "rev-parse", ref], capture_output=True, timeout=5)
    if r.returncode != 0:
        return set()  # expect: flag
    if subprocess.run(["git", "show", ref], capture_output=True, timeout=5).returncode:
        return set()  # expect: flag
    return {r.stdout}


def os_path_probe(p: str) -> str:
    if not os.path.isfile(p):
        return ""  # expect: flag
    if os.access(p, os.R_OK):
        pass
    else:
        return ""  # expect: flag
    return p


def ternaries(site: Path, r) -> object:
    files = set(site.iterdir()) if site.is_dir() else set()  # expect: flag
    out = r.stdout if r.returncode == 0 else None  # expect: flag
    return files, out


def ternary_returned(site: Path) -> list:
    return list(site.iterdir()) if site.exists() else []  # expect: flag


def attribute_target(self, path: Path) -> None:
    try:
        self.cache = json.loads(path.read_text())
    except ValueError:
        self.cache = {}  # expect: flag


def suppressed(path: Path) -> None:
    with contextlib.suppress(OSError):  # expect: flag
        path.unlink()
    with suppress(FileNotFoundError):  # expect: flag
        path.unlink()


def waived(path: Path) -> dict:
    try:
        return json.loads(path.read_text())
    except OSError:
        return {}  # fail-direction: a missing cache is a cold cache, rebuilt from source  # expect: clean


def waived_above(path: Path) -> dict:
    if not path.exists():
        # fail-direction: no overrides file means no overrides, which the defaults cover
        return {}  # expect: clean
    return json.loads(path.read_text())


def short_waiver(path: Path) -> dict:
    try:
        return json.loads(path.read_text())
    except OSError:
        return {}  # fail-direction: fine  # expect: flag


def stale_waiver(path: Path) -> str:
    return path.read_text()  # fail-direction: nothing here to waive at all  # expect: flag


def fail_closed(path: Path, errors: list) -> dict:
    try:
        return json.loads(path.read_text())
    except OSError as e:
        errors.append(f"{path}: unreadable ({e})")
        return {}  # expect: clean


def fail_closed_self(self, path: Path) -> str:
    if not path.is_file():
        self.errors.append(f"{path}: missing")
        return ""  # expect: clean
    return path.read_text()


def reraised(path: Path) -> dict:
    try:
        return json.loads(path.read_text())
    except OSError:
        raise  # expect: clean


def non_empty_default(path: Path) -> list:
    try:
        return json.loads(path.read_text())
    except OSError:
        return ["default"]  # expect: clean


def skipped_item(paths: list) -> list:
    out = []
    for p in paths:
        try:
            out.append(p.read_text())
        except OSError:
            continue  # expect: clean
    return out


def unrelated_if(items: list) -> list:
    if not items:
        return []  # expect: clean
    return items


def nested_scope_starts_clean(path: Path):
    try:
        path.read_text()
    except OSError:
        def fallback():
            return None  # expect: clean
        raise
    return fallback


def ternary_without_probe(x) -> list:
    return x if x else []  # expect: clean


def has_file(p: Path) -> bool:
    return p.is_file()


def through_helper(p: Path) -> str:
    if not has_file(p):
        return ""  # expect: flag
    return p.read_text()


def empty_extension_is_not_a_record(p: Path, errors: list):
    errors.extend([])
    try:
        return p.read_text()
    except OSError:
        return []  # expect: flag


def lambda_fallback(p: Path) -> str:
    return (lambda: p.read_text() if p.exists() else "")()  # expect: flag


def walrus_fallback(p: Path):
    try:
        return p.read_text()
    except OSError:
        return (cached := None)  # expect: flag


def each_ternary_owns_its_waiver(a: Path, b: Path) -> tuple:
    return (a.read_text() if a.exists() else "",  # fail-direction: an absent optional header reads as none  # expect: clean
            b.read_text() if b.exists() else "")  # expect: flag


def identical_ternaries(a: Path) -> tuple:
    return (a.read_text() if a.exists() else "", a.read_text() if a.exists() else "")  # fail-direction: one waiver, two fallbacks  # expect: flag


def local_probe_alias(p: str) -> str:
    from os.path import isfile as present
    if not present(p):
        return ""  # expect: flag
    return p


def available(p: str) -> bool:
    from os.path import isfile
    return isfile(p)


def through_locally_importing_helper(p: str) -> list:
    if not available(p):
        return []  # expect: flag
    return [p]


class Config:
    from contextlib import suppress as quietly
    with quietly(OSError):  # expect: flag
        Path("x").unlink()

    def method(self, p: Path):
        with quietly(OSError):  # expect: clean
            p.unlink()


from os.path import isfile as present


class LambdaProbe:
    from math import sqrt as present
    if not (lambda: present("/absent"))():
        result = []  # expect: flag
