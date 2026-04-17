#!/usr/bin/env bash
# ============================================================================
# compare.sh -- diff the kernel-view and mirror-view boot_info ABI manifests
#               emitted by tools/boot-info-manifest/dump-{kernel,mirror}.c.
#
# Emits a single-line PASS summary on success; on failure, prints the first
# mismatching field by name + the two offsets/sizes and exits 1. Also emits
# the SHA-256 fingerprints from both views so CI logs show what the current
# ABI "version hash" is.
#
# Usage:
#   bash tools/boot-info-manifest/compare.sh <kernel.json> <mirror.json>
#
# Dependencies: python3 only (no PyYAML, no network).
# ============================================================================

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <kernel.json> <mirror.json>" >&2
    exit 2
fi

KERNEL_JSON="$1"
MIRROR_JSON="$2"

for f in "$KERNEL_JSON" "$MIRROR_JSON"; do
    if [ ! -f "$f" ]; then
        echo "error: $f not found" >&2
        exit 2
    fi
done

python3 - "$KERNEL_JSON" "$MIRROR_JSON" <<'PYEOF'
import json
import sys

kernel_path, mirror_path = sys.argv[1], sys.argv[2]
with open(kernel_path) as fh:
    kernel = json.load(fh)
with open(mirror_path) as fh:
    mirror = json.load(fh)

GREEN = "\033[0;32m"
RED = "\033[0;31m"
DIM = "\033[0;90m"
NC = "\033[0m"
if not sys.stdout.isatty():
    GREEN = RED = DIM = NC = ""


def fail(msg, detail=None):
    print(f"{RED}FAIL{NC} boot_info ABI manifest: {msg}")
    if detail:
        print(f"      {DIM}{detail}{NC}")
    sys.exit(1)


# Compare summary invariants first -- these are cheapest and name the
# problem clearly when they trip.
if kernel["version"] != mirror["version"]:
    fail(
        "BOOT_INFO_VERSION mismatch",
        f"kernel view says {kernel['version']}, mirror view says {mirror['version']}; bump both or revert one",
    )
if kernel["struct_size"] != mirror["struct_size"]:
    fail(
        "sizeof(struct boot_info) mismatch",
        f"kernel view: {kernel['struct_size']} bytes, mirror view: {mirror['struct_size']} bytes",
    )

kernel_fields = kernel["fields"]
mirror_fields = mirror["fields"]

if len(kernel_fields) != len(mirror_fields):
    fail(
        f"field count mismatch: kernel={len(kernel_fields)}, mirror={len(mirror_fields)}",
        "one view added or removed a field without the other matching; check dump-fields.inc and the struct definitions",
    )

# Walk row-for-row. dump-fields.inc is shared, so ordering is guaranteed
# identical; the only thing that can drift is offset or size (or name, if
# someone edited one header without the other).
for i, (k, m) in enumerate(zip(kernel_fields, mirror_fields)):
    for key in ("name", "offset", "size"):
        if k[key] != m[key]:
            # Report the FIRST mismatch by name + line index. This is the
            # user-visible "drift detected" surface.
            fail(
                f"field #{i} '{k['name']}' diverges on '{key}'",
                f"kernel: {k['name']}@{k['offset']}+{k['size']}; mirror: {m['name']}@{m['offset']}+{m['size']}",
            )

# Happy path.
print(
    f"{GREEN}PASS{NC} boot_info ABI manifest: "
    f"{len(kernel_fields)} fields, "
    f"size={kernel['struct_size']}, "
    f"version={kernel['version']}, "
    f"kernel-sha256={kernel['sha256'][:12]}..., "
    f"mirror-sha256={mirror['sha256'][:12]}..."
)
PYEOF
