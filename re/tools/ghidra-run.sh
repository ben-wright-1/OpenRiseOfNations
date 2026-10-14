#!/bin/bash
# Run Ghidra post-scripts against the saved, already-analyzed RON project (read-only, no re-analysis).
# Usage: re/tools/ghidra-run.sh [--project RON|RON_d3dgl] Script.java re/jobs/args.txt [Script2.java re/jobs/args2.txt ...]
# Script arguments are passed as @argfiles (see ghidra_scripts/ArgFiles.java). Relative paths inside the
# argument files (e.g. an output dir) resolve against the repository root.
set -euo pipefail
re="$(cd "$(dirname "$0")/.." && pwd)"
root="$(dirname "$re")"

project=RON
if [[ "${1:-}" == "--project" ]]; then
	project="$2"
	shift 2
fi
case "$project" in
	RON) projDir="$re/ghidra" program=riseofnations.exe ;;
	RON_d3dgl) projDir="$re/ghidra_d3dgl" program=d3dgl.dll ;;
	*) echo "unknown project: $project (RON or RON_d3dgl)" >&2; exit 2 ;;
esac
if (( $# == 0 || $# % 2 )); then
	echo "usage: $0 [--project RON|RON_d3dgl] Script.java args.txt [Script2.java args2.txt ...]" >&2
	exit 2
fi

headless="${GHIDRA_HEADLESS:-}"
if [[ -z "$headless" ]]; then
	for c in "$(brew --prefix ghidra 2>/dev/null)/libexec/support/analyzeHeadless" "${GHIDRA_INSTALL_DIR:-/nonexistent}/support/analyzeHeadless"; do
		[[ -x "$c" ]] && headless="$c" && break
	done
fi
[[ -n "$headless" ]] || { echo "analyzeHeadless not found; brew install ghidra, or set GHIDRA_HEADLESS" >&2; exit 1; }
if [[ -z "${JAVA_HOME:-}" ]]; then
	jdk="$(brew --prefix openjdk@21 2>/dev/null)/libexec/openjdk.jdk/Contents/Home"
	[[ -d "$jdk" ]] && export JAVA_HOME="$jdk" && export PATH="$JAVA_HOME/bin:$PATH"
fi
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-12G}"

scriptArgs=()
while (( $# )); do
	argFile="${2#@}"
	[[ "$argFile" = /* ]] || argFile="$PWD/$argFile"
	scriptArgs+=(-postScript "$1" "@$argFile")
	shift 2
done

# One Ghidra run at a time: the project is locked while open. Parallel callers wait their turn.
lock="$re/jobs/.ghidra.lock"
until mkdir "$lock" 2>/dev/null; do
	# Clear a lock left by a run that was killed.
	owner="$(cat "$lock/pid" 2>/dev/null || true)"
	if [[ -n "$owner" ]] && ! kill -0 "$owner" 2>/dev/null; then rm -rf "$lock"; continue; fi
	sleep 2
done
echo $$ > "$lock/pid"
trap 'rm -rf "$lock"' EXIT

log="$re/jobs/last-run.log"
cd "$root"
: > "$log"
code=0
"$headless" "$projDir" "$project" -process "$program" -noanalysis -readOnly \
	-scriptPath "$re/ghidra_scripts" "${scriptArgs[@]}" -log "$log" > "$re/jobs/last-console.txt" 2>&1 || code=$?
grep -E 'ERROR|Exception|decompiled [0-9]+|> ' "$log" || true
exit $code
