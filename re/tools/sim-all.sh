#!/bin/bash
# Runs build/ron-sim on every stock scenario (*.scx under <game dir>/scenario and conquest) with the determinism self-check and
# reports the failures. Usage: re/tools/sim-all.sh <game dir> [frames] [extra ron-sim args...]
set -uo pipefail
game="${1:?usage: sim-all.sh <game dir> [frames]}"
frames="${2:-300}"
shift $(( $# >= 2 ? 2 : 1 ))
root="$(cd "$(dirname "$0")/../.." && pwd)"
ok=0
bad=0
combat=0
while IFS= read -r f; do
	out="$("$root/build/ron-sim" "$game" "$f" --frames "$frames" --digest-every 0 --combat-every "$frames" "$@" 2>&1)"
	if grep -q "self-check: OK" <<<"$out"; then
		ok=$((ok + 1))
	else
		bad=$((bad + 1))
		echo "FAILED: $f"
		tail -3 <<<"$out"
	fi
	# Scenarios where something died or a projectile flew during the run (combat activity).
	if grep -E "kills [1-9]|ammo [1-9]|stuck [1-9]|corpses [1-9]" <<<"$out" >/dev/null; then combat=$((combat + 1)); fi
done < <(find "$game/scenario" "$game/conquest" -iname "*.scx" | sort)
echo "scenarios: $((ok + bad)), self-check OK: $ok, failed: $bad, with combat activity: $combat"
[[ $bad -eq 0 ]]
