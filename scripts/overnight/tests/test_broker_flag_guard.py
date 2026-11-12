#!/usr/bin/env python3
"""A flag word in a review prompt reaches the reviewer as prose.

codex-companion re-splits a one-argument prompt on whitespace and reads any word
starting with `-` as its own option. The broker used to refuse a prompt holding a
bare `--flag` (19 refusals in the 2026-09-28/29 canary, 11 on `--cached`) and still
let single-dash words through: on 2026-11-12 `git -C after` in a plan body became
--cwd after and every pass died "git is not installed". The prompt now goes after
`--`, so it is one positional however it is worded, and nothing is refused for it.
"""
from __future__ import annotations

import json
import os
import pathlib
import subprocess
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
BROKER = REPO / "scripts/overnight/review-broker-codex-dispatch.sh"
HEAD = "[review-kind: adversarial] todo/00-infrastructure/TODO-10-documentation-site.md section 99\n"
COMPANION = """
const fs = require('fs');
const path = require('path');
fs.writeFileSync(path.join(__dirname, 'argv.json'), JSON.stringify(process.argv.slice(3)));
process.stdout.write('# Codex Adversarial Review\\n\\nfine\\n');
"""


def _broker(tmp_path, body):
    repo = tmp_path / "repo"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", str(repo)], check=True, timeout=30)
    fake = tmp_path / "companion.js"
    fake.write_text(COMPANION)
    env = {**os.environ, "CODEX_COMPANION_PATH": str(fake), "CODEX_OUTAGE_OVERRIDE": "1"}
    done = subprocess.run(["bash", str(BROKER), HEAD + body], capture_output=True, text=True,
                          timeout=60, cwd=str(repo), env=env)
    assert done.returncode == 0, (done.returncode, done.stderr)
    log = pathlib.Path(json.loads(done.stdout.strip().splitlines()[-1])["logFile"])
    deadline = time.time() + 60
    while time.time() < deadline and not (log.exists() and "Turn completed (rc=" in log.read_text()):
        time.sleep(0.2)
    assert "Turn completed (rc=0)" in log.read_text(), log.read_text()[-400:]
    return json.loads((tmp_path / "argv.json").read_text())


def test_flag_words_in_the_prompt_arrive_as_one_positional(tmp_path):
    body = "Check that git diff --cached lists staged paths, --scope all of it, and git -C after a separator."
    argv = _broker(tmp_path, body)
    assert argv == ["--", HEAD + body], argv


if __name__ == "__main__":
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        test_flag_words_in_the_prompt_arrive_as_one_positional(pathlib.Path(d))
    print("PASS: broker passes the prompt after --")
