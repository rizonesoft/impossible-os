#!/usr/bin/env python3
"""Append one JSON object to a file, or print it.

    python3 scripts/overnight/manifest-line.py [--append FILE] key=value ...

The review dispatchers built their manifest lines with printf, so a path holding a quote, a backslash or a
newline produced a line no reader could parse, and every later dispatch added another (2026-11-11). Values are
strings unless the key is prefixed: `int:section=7`, `bool:pass_checked=true`. A key with an empty value after
`opt:` is left out (`opt:int:section=`). Exit 0 written, 2 a bad argument or the file cannot be written.
"""
import json
import sys


def main(argv):
    out = None
    if argv[:1] == ["--append"]:
        if len(argv) < 2:
            print("manifest-line: --append needs a file", file=sys.stderr)
            return 2
        out, argv = argv[1], argv[2:]
    obj = {}
    for arg in argv:
        key, sep, value = arg.partition("=")
        if not sep or not key:
            print(f"manifest-line: not key=value: {arg!r}", file=sys.stderr)
            return 2
        optional = key.startswith("opt:")
        if optional:
            key = key[4:]
            if value == "":
                continue
        try:
            if key.startswith("int:"):
                key, value = key[4:], int(value)
            elif key.startswith("bool:"):
                if value not in ("true", "false"):
                    raise ValueError(value)
                key, value = key[5:], value == "true"
        except ValueError:
            print(f"manifest-line: bad value for {key}: {value!r}", file=sys.stderr)
            return 2
        obj[key] = value
    line = json.dumps(obj) + "\n"
    if out is None:
        sys.stdout.write(line)
        return 0
    try:
        with open(out, "a", encoding="utf-8") as f:
            f.write(line)
    except OSError as exc:
        print(f"manifest-line: cannot write {out}: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
