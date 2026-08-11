#!/usr/bin/env bash
#
# Compare a shader-bake log against the recorded exclusion census.
#
#   bash misc/webgpu_scripts/check-bake-census.sh <bake.log> [expected.txt]
#
# The bake is where SPIR-V becomes WGSL, so it is the one place an upstream
# shader change shows up as something WebGPU cannot express. Translation
# failures are deliberately not fatal at bake time - the variant is excluded
# and the renderer takes another path - which means they are invisible unless
# something counts them. This does the counting.
#
# Fails when a class appears that is not expected, or when an expected class
# grows. A class that shrinks or disappears is reported but does not fail: that
# direction is an improvement, and the baseline should then be updated.

set -uo pipefail

LOG=${1:?usage: check-bake-census.sh <bake.log> [expected.txt]}
EXPECTED=${2:-$(dirname "$0")/bake_fixture/expected-exclusions.txt}

[ -f "$LOG" ] || { echo "no such log: $LOG" >&2; exit 2; }
[ -f "$EXPECTED" ] || { echo "no such baseline: $EXPECTED" >&2; exit 2; }

# The log is read with grep -a: a --verbose export mixes in binary-ish output
# and grep would otherwise decide the file is binary and print nothing.
ACTUAL=$(mktemp)
BASE=$(mktemp)
trap 'rm -f "$ACTUAL" "$BASE"' EXIT

grep -ao "excluding shader '[A-Za-z0-9_]*'" "$LOG" \
	| sed "s/excluding shader '//; s/'//" \
	| sort | uniq -c | awk '{ print $1, $2 }' | sort -k2 > "$ACTUAL"

grep -vE '^\s*(#|$)' "$EXPECTED" | awk '{ print $1, $2 }' | sort -k2 > "$BASE"

echo "bake census (expected -> actual):"
STATUS=0
IMPROVED=0

# Everything the baseline knows about.
while read -r want name; do
	got=$(awk -v n="$name" '$2 == n { print $1 }' "$ACTUAL")
	got=${got:-0}
	if [ "$got" -gt "$want" ]; then
		printf '  \033[31mGREW\033[0m      %-28s %s -> %s\n' "$name" "$want" "$got"
		STATUS=1
	elif [ "$got" -lt "$want" ]; then
		printf '  \033[33mimproved\033[0m  %-28s %s -> %s\n' "$name" "$want" "$got"
		IMPROVED=1
	else
		printf '  ok        %-28s %s\n' "$name" "$want"
	fi
done < "$BASE"

# Anything the baseline has never seen.
while read -r got name; do
	if ! awk -v n="$name" '$2 == n { found = 1 } END { exit !found }' "$BASE"; then
		printf '  \033[31mNEW\033[0m       %-28s %s variant(s) newly untranslatable\n' "$name" "$got"
		STATUS=1
	fi
done < "$ACTUAL"

echo
if [ "$STATUS" != 0 ]; then
	echo "FAILED: the bake lost coverage. An upstream shader change produced something"
	echo "WGSL cannot express, or one of our SPIR-V passes stopped handling a case."
	echo "Find the reason in the log before promoting this base:"
	# Stop at the first pipe: everything after it is the dumped WGSL, which
	# runs for hundreds of columns and buries the error it is attached to.
	grep -ao "excluding shader '[A-Za-z0-9_]*'[^|]*" "$LOG" \
		| sed 's/ \[[^]]*\]//' | cut -c1-150 | sort -u | head -8 | sed 's/^/  /'
	exit 1
fi
if [ "$IMPROVED" != 0 ]; then
	echo "Bake coverage improved. If that was deliberate, update $EXPECTED."
fi
echo "bake census OK"
exit 0
