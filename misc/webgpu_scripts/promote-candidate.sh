#!/usr/bin/env bash
# Promote an auto-rebase candidate produced by the upstream-sync workflow
# onto webgpu-stack, safely. Usage: promote-candidate.sh <YYYYMMDD>
#
# This script deliberately does NOT push: after it runs, execute the local
# verification gates it prints, then push with --force-with-lease yourself.
set -euo pipefail

STAMP="${1:-}"
if [ -z "$STAMP" ]; then
	echo "usage: promote-candidate.sh <YYYYMMDD>  (see the auto-rebase/* branches)" >&2
	exit 2
fi
CANDIDATE="auto-rebase/$STAMP"

if [ -n "$(git status --porcelain)" ]; then
	echo "refusing: working tree is not clean" >&2
	exit 1
fi
CURRENT=$(git rev-parse --abbrev-ref HEAD)
if [ "$CURRENT" != "webgpu-stack" ]; then
	echo "refusing: not on webgpu-stack (on $CURRENT)" >&2
	exit 1
fi

git fetch origin --prune
if ! git rev-parse --verify "origin/$CANDIDATE" >/dev/null 2>&1; then
	echo "refusing: origin/$CANDIDATE does not exist" >&2
	exit 1
fi

# A clean replay proves the patches apply; only CI proves they compile.
# Promoting first would spend a full gate run learning the same thing.
REPO=$(git remote get-url origin | sed -E 's#^(git@github\.com:|https://github\.com/)##; s#\.git$##')
CANDIDATE_SHA=$(git rev-parse "origin/$CANDIDATE")
VERDICT=$(gh run list -R "$REPO" --branch "$CANDIDATE" --workflow web-baseline.yml --limit 20 \
	--json headSha,status,conclusion \
	--jq "[.[] | select(.headSha == \"$CANDIDATE_SHA\")][0] | \"\\(.status) \\(.conclusion)\"" 2>/dev/null || true)
if [ "$VERDICT" != "completed success" ]; then
	echo "refusing: baseline CI on $CANDIDATE (${CANDIDATE_SHA:0:10}) is '${VERDICT:-unknown}', not green." >&2
	echo "The upstream-sync issues name the cause; a red candidate needs a new patch." >&2
	exit 1
fi

# The candidate must contain the same patch subjects as the current stack:
# a candidate built before recent pushes would silently drop patches.
git fetch upstream master >/dev/null 2>&1 || true
LOCAL_SUBJECTS=$(git log --format=%s upstream/master..HEAD | sort)
CAND_SUBJECTS=$(git log --format=%s upstream/master.."origin/$CANDIDATE" | sort)
if [ "$LOCAL_SUBJECTS" != "$CAND_SUBJECTS" ]; then
	echo "refusing: candidate patch list differs from the local stack -" >&2
	echo "it was likely built before your latest pushes. Re-run the sync" >&2
	echo "workflow (workflow_dispatch) to get a fresh candidate." >&2
	exit 1
fi
# The sync's verify step refuses a stack with any other subject shape, so a
# hotfix committed outside the convention blocks every replay after it.
STRAYS=$(printf '%s\n' "$CAND_SUBJECTS" | grep -v '^\[wgpu ' || true)
if [ -n "$STRAYS" ]; then
	echo "refusing: commit(s) without a [wgpu NN] subject:" >&2
	echo "$STRAYS" >&2
	exit 1
fi

BACKUP="backup/stack-$(date +%Y%m%d-%H%M)"
git branch "$BACKUP" HEAD
git reset --hard "origin/$CANDIDATE"
bash "$(dirname "$0")/check-stack.sh"

cat <<EOF

Promoted $CANDIDATE onto webgpu-stack (backup: $BACKUP).
Before pushing, run the LOCAL gates CI cannot cover:

  1. bash misc/webgpu_scripts/run-local-gates.sh --forward-plus
     (all three binaries at this HEAD, both probes, closure, loader smoke,
     both bake censuses, both boots, templates installed)
  2. Headset spot-check of anything the replay touched.
  3. Push (with approval):  git push --force-with-lease origin webgpu-stack

Recovery at any point:  git reset --hard $BACKUP
EOF
