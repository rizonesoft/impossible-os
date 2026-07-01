#!/usr/bin/env bash
# Conclave connector for impossible-os. Runs the IN-REPO engine under conclave/,
# using the shared (project-agnostic) corpus -- no per-project namespace. Fail-open:
# never blocks the caller if the engine is unavailable.
#
# Usage: bash .conclave/connector.sh <ask|dispatch|recall|note|poll|outcome|stats|doctor> [args...]
#   e.g. bash .conclave/connector.sh dispatch --mode stuck --target build --source /tmp/brief.txt
#        bash .conclave/connector.sh poll <job-id>
#        bash .conclave/connector.sh recall "smp race in scheduler"
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE="${CONCLAVE_HOME:-$REPO_ROOT/conclave}"
if [ ! -d "$ENGINE/conclave" ]; then
  echo "conclave: engine not found at $ENGINE" >&2
  exit 0
fi

# Prefer an in-repo venv; fall back to a local ~/conclave venv (transitional while
# ~/conclave still holds the installed deps) then python3.
PY="$ENGINE/.venv/bin/python"
[ -x "$PY" ] || PY="$HOME/conclave/.venv/bin/python"
[ -x "$PY" ] || PY="python3"
export RUST_LOG="${RUST_LOG:-error}"   # quiet LanceDB's Rust info/warn chatter

cd "$ENGINE"
exec "$PY" -c 'import sys; from conclave import cli; sys.exit(cli.main(sys.argv[1:]))' "$@" --corpus
