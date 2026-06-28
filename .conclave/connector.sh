#!/usr/bin/env bash
# Conclave connector for impossible-os. Forwards to the ~/conclave engine, namespaced
# to this project. Fail-open: never blocks the caller if the engine is absent.
#
# Usage: bash .conclave/connector.sh <ask|dispatch|recall|note|poll|outcome|stats|doctor> [args...]
#   e.g. bash .conclave/connector.sh dispatch --mode stuck --target build --source /tmp/brief.txt
#        bash .conclave/connector.sh poll <job-id>
#        bash .conclave/connector.sh recall "smp race in scheduler"
set -euo pipefail

HOME_DIR="${CONCLAVE_HOME:-$HOME/conclave}"
if [ ! -d "$HOME_DIR/conclave" ]; then
  echo "conclave: engine not found at $HOME_DIR (clone github.com/rizonetech/Conclave)" >&2
  exit 0
fi

PY="$HOME_DIR/.venv/bin/python"
[ -x "$PY" ] || PY="python3"
export RUST_LOG="${RUST_LOG:-error}"   # quiet LanceDB's Rust info/warn chatter

cd "$HOME_DIR"
exec "$PY" -c 'import sys; from conclave import cli; sys.exit(cli.main(sys.argv[1:]))' "$@" --project impossible-os
