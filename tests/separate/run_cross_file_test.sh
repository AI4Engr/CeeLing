#!/usr/bin/env bash
# Dangling / use-after-move checks across files.
#   tests/separate/run_cross_file_test.sh <path-to-cling>
# 1. every tests/safety fixture, split into two files, gets the same verdict (split_equivalence.py)
# 2. hand-written cases whose defect needs what ANOTHER file declares (tests/separate/xf_*.use.clg include xf_lib.clg); each states //@ expect: reject|accept
set -uo pipefail
CLING="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"     # (absolute: the scripts change directory)
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
bad=0
python3 -I "$HERE/split_equivalence.py" "$CLING" "$HERE/../safety" "$W/split" || bad=1
cp "$HERE"/xf_*.clg "$W/"
for f in "$W"/xf_*.use.clg; do
    name="$(basename "$f")"
    want="$(sed -n 's|^//@ expect: *||p' "$f" | head -1)"
    if "$CLING" --cpp "$W/${name%.clg}.cpp" "$f" > "$W/${name}.log" 2>&1; then got=accept; else got=reject; fi
    if [ "$got" != "$want" ]; then echo "FAIL: $name: expected $want, got $got"; grep -v Detected "$W/${name}.log" | head -3; bad=1; fi
done
[ "$bad" = 0 ] && echo "ok: dangling and move checks hold across files"
exit $bad
