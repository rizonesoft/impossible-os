#!/usr/bin/env bash
# brand_contract_gate.sh -- the brand contract part of lint Check 30 (docs/design/icons.md#how-are-icons-rendered).
#
#   bash scripts/brand_contract_gate.sh <repo-root>            judge the working tree
#   bash scripts/brand_contract_gate.sh <repo-root> --staged   judge the git index (the commit gate)
#
# docs/design/brand-contract.json must equal a fresh generation from its sources. Under
# --staged the checker itself is the INDEX revision, run from a copy, so an unstaged edit or a
# deleted working copy can neither change nor skip the verdict. A snapshot that holds the
# contract or the SVG profile but not the checker fails: the gate never passes by absence.
# Exit 0 = current; 1 = stale, a missing checker, contract or profile, or an error.
set -u
ROOT="${1:?usage: brand_contract_gate.sh <repo-root> [--staged]}"
SCOPE="${2:-}"
CHECKER=""
COPY=""
cleanup() { [ -n "$COPY" ] && rm -f "$COPY"; }
trap cleanup EXIT

present() {   # present <path>: the path is in the judged snapshot
    if [ "$SCOPE" = "--staged" ]; then
        git -C "$ROOT" cat-file -e ":$1" 2>/dev/null
    else
        [ -e "$ROOT/$1" ]
    fi
}

# The gate exists, so the contract does: the checker and both data files must all be there.
for need in scripts/brand_contract.py docs/design/brand-contract.json docs/design/svg-profile.json; do
    if ! present "$need"; then
        echo "brand-contract: the snapshot lacks $need, which the brand contract needs"
        exit 1
    fi
done
if present scripts/brand_contract.py; then
    if [ "$SCOPE" = "--staged" ]; then
        COPY=$(mktemp --suffix=.py)
        git -C "$ROOT" show ":scripts/brand_contract.py" > "$COPY" || exit 1
        CHECKER="$COPY"
    else
        CHECKER="$ROOT/scripts/brand_contract.py"
    fi
elif present docs/design/brand-contract.json || present docs/design/svg-profile.json; then
    echo "brand-contract: the snapshot holds the contract but not scripts/brand_contract.py, which checks it"
    exit 1
else
    exit 0
fi

python3 "$CHECKER" --root "$ROOT" --check $SCOPE
