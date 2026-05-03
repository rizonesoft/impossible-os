#!/bin/bash
# scripts/test-firmware-decode.sh -- host-side firmware-tables-decode round-trip
#
# Builds the host decoder if needed, runs --round-trip against the captured
# fixture in src/kernel/test/fixtures/firmware/, asserts the decoder
# pretty-prints the schema_version + tables[] sections, and confirms the
# canonical re-emitter is byte-identical across two parse-emit cycles.
#
# Used by the firmware-tables host-decoder regression test.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DECODER="$ROOT/build/tools/firmware-tables-decode"
FIXTURE="$ROOT/src/kernel/test/fixtures/firmware/firmware-tables-sample.json"

if [ ! -x "$DECODER" ]; then
    echo "[firmware-decode] building decoder..." >&2
    make -C "$ROOT" build/tools/firmware-tables-decode >/dev/null
fi

if [ ! -f "$FIXTURE" ]; then
    echo "[firmware-decode] FAIL: fixture missing at $FIXTURE" >&2
    exit 2
fi

# Pretty-print: must contain schema_version, tables[], at least one block
out="$("$DECODER" "$FIXTURE")"
for line in "schema_version" "tables\[\]" "acpi --"; do
    if ! echo "$out" | grep -qE -- "$line"; then
        echo "[firmware-decode] FAIL: pretty-print missing '$line'" >&2
        echo "$out" >&2
        exit 3
    fi
done

# Round-trip: byte-identical canonical re-emitter
if ! "$DECODER" --round-trip "$FIXTURE"; then
    echo "[firmware-decode] FAIL: --round-trip failed" >&2
    exit 4
fi

# Schema-version validation: bad input rejected (pretty-print mode)
echo '{"schema_version":99}' > /tmp/firmware-decode-badver.json
if "$DECODER" /tmp/firmware-decode-badver.json 2>/dev/null; then
    echo "[firmware-decode] FAIL: bad schema_version=99 was accepted (pretty-print)" >&2
    rm -f /tmp/firmware-decode-badver.json
    exit 5
fi

# Round-trip must ALSO reject bad schema_version (not just pretty-print).
# A v99 file that round-trips byte-stably could have leaked through the
# round-trip gate; validate_schema_version() must run before emit.
if "$DECODER" --round-trip /tmp/firmware-decode-badver.json 2>/dev/null; then
    echo "[firmware-decode] FAIL: --round-trip accepted schema_version=99" >&2
    rm -f /tmp/firmware-decode-badver.json
    exit 6
fi
rm -f /tmp/firmware-decode-badver.json

# Trailing-garbage rejection: a valid v1 object followed by stale tail
# bytes must NOT silently parse as the prefix on round-trip.
cat "$FIXTURE" > /tmp/firmware-decode-trailgarbage.json
echo 'EXTRA_GARBAGE_AFTER_JSON' >> /tmp/firmware-decode-trailgarbage.json
if "$DECODER" --round-trip /tmp/firmware-decode-trailgarbage.json 2>/dev/null; then
    echo "[firmware-decode] FAIL: --round-trip accepted trailing garbage" >&2
    rm -f /tmp/firmware-decode-trailgarbage.json
    exit 7
fi
rm -f /tmp/firmware-decode-trailgarbage.json

# Malformed-number rejection: strict RFC 8259 number grammar must reject
# inputs the kernel writer would never emit (bare "-", "1e", ".", "01").
for bad in '{"schema_version":1,"x":1e}' '{"schema_version":1,"x":-}' \
           '{"schema_version":1,"x":.5}' '{"schema_version":1,"x":01}'; do
    echo "$bad" > /tmp/firmware-decode-badnum.json
    if "$DECODER" --round-trip /tmp/firmware-decode-badnum.json 2>/dev/null; then
        echo "[firmware-decode] FAIL: --round-trip accepted malformed number: $bad" >&2
        rm -f /tmp/firmware-decode-badnum.json
        exit 8
    fi
done
rm -f /tmp/firmware-decode-badnum.json

# v1 required-field validation: schema_version=1 with missing required keys
# must be rejected. Schema-version gate alone is insufficient -- a file
# missing tables[] / firmware_platform / etc. would otherwise pretty-print
# with empty sections and pass --round-trip on byte-stable canonicalization.
echo '{"schema_version":1}' > /tmp/firmware-decode-missingreq.json
if "$DECODER" /tmp/firmware-decode-missingreq.json 2>/dev/null; then
    echo "[firmware-decode] FAIL: pretty-print accepted v1 with missing required keys" >&2
    rm -f /tmp/firmware-decode-missingreq.json
    exit 9
fi
if "$DECODER" --round-trip /tmp/firmware-decode-missingreq.json 2>/dev/null; then
    echo "[firmware-decode] FAIL: --round-trip accepted v1 with missing required keys" >&2
    rm -f /tmp/firmware-decode-missingreq.json
    exit 10
fi
rm -f /tmp/firmware-decode-missingreq.json

# v1 wrong-type rejection: required key with wrong type must fail
echo '{"schema_version":1,"firmware_platform":42,"generated_at_utc":"x","conformance_profile":{},"tables":[],"degraded":[],"quirks_active":[],"acpi":{},"smbios":{},"mat":{},"rt_properties":{},"esrt":{},"apei":{},"dbg2":{},"wsmt":{}}' > /tmp/firmware-decode-wrongtype.json
if "$DECODER" /tmp/firmware-decode-wrongtype.json 2>/dev/null; then
    echo "[firmware-decode] FAIL: accepted firmware_platform as integer" >&2
    rm -f /tmp/firmware-decode-wrongtype.json
    exit 11
fi
rm -f /tmp/firmware-decode-wrongtype.json

# Embedded NUL rejection: a string containing the literal JSON escape
# for U+0000 must be refused so the NUL-terminated decoder cannot
# silently truncate at encode time. Built via python so the literal
# 6-char escape sequence reaches the decoder unmodified.
python3 -c "import sys; bs=chr(92); q=chr(34); sys.stdout.write('{'+q+'schema_version'+q+':1,'+q+'firmware_platform'+q+':'+q+'a'+bs+'u0000b'+q+','+q+'generated_at_utc'+q+':'+q+'x'+q+','+q+'conformance_profile'+q+':{},'+q+'tables'+q+':[],'+q+'degraded'+q+':[],'+q+'quirks_active'+q+':[],'+q+'acpi'+q+':{},'+q+'smbios'+q+':{},'+q+'mat'+q+':{},'+q+'rt_properties'+q+':{},'+q+'esrt'+q+':{},'+q+'apei'+q+':{},'+q+'dbg2'+q+':{},'+q+'wsmt'+q+':{}}')" > /tmp/firmware-decode-embnul.json
if "$DECODER" /tmp/firmware-decode-embnul.json 2>/dev/null; then
    echo "[firmware-decode] FAIL: accepted embedded \\u0000 in string" >&2
    rm -f /tmp/firmware-decode-embnul.json
    exit 12
fi
rm -f /tmp/firmware-decode-embnul.json

echo "[firmware-decode] PASS (pretty-print + round-trip + schema reject + trailing-garbage reject + malformed-number reject + missing-required reject + wrong-type reject + embedded-NUL reject)"
