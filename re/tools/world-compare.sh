#!/bin/bash
# Compare build/ron-world (engine/sim) with re/tools/worlddump.mjs on every stock .scx: the --json dump byte for byte
# and the printed summary line for line.
#   re/tools/world-compare.sh [game dir] [--seed n]
# Prints one line per difference and a total; exit code 1 when anything differs.
set -uo pipefail
cd "$(dirname "$0")/../.."
GAME="${1:-$HOME/Games/RiseOfNations}"
shift || true
EXTRA=("$@")
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

compare_one() {
	local rel="$1" key
	key="$(echo "$rel" | tr '/ ' '__')"
	node re/tools/worlddump.mjs "$rel" --install "$GAME" --json "$OUT/$key.js.json" "${EXTRA[@]}" >"$OUT/$key.js.txt" 2>&1
	./build/ron-world "$GAME" "$rel" --json "$OUT/$key.cpp.json" "${EXTRA[@]}" >"$OUT/$key.cpp.txt" 2>&1
	if ! cmp -s "$OUT/$key.js.json" "$OUT/$key.cpp.json"; then echo "JSON DIFF $rel"; fi
	if ! diff -q "$OUT/$key.js.txt" "$OUT/$key.cpp.txt" >/dev/null; then echo "TEXT DIFF $rel"; fi
}
export -f compare_one
export GAME OUT
export EXTRA_STR="${EXTRA[*]:-}"

files=$(cd "$GAME" && find scenario conquest -iname '*.scx' | sort)
count=$(echo "$files" | wc -l | tr -d ' ')
diffs=$(echo "$files" | tr '\n' '\0' | xargs -0 -P 8 -I{} bash -c 'EXTRA=($EXTRA_STR); compare_one "$1"' _ {})
[ -n "$diffs" ] && echo "$diffs"
n=$(printf '%s' "$diffs" | grep -c . || true)
echo "$count scenarios compared, $n differences"
[ "$n" -eq 0 ]
