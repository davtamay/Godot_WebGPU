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

BACKUP="backup/stack-$(date +%Y%m%d-%H%M)"
git branch "$BACKUP" HEAD
git reset --hard "origin/$CANDIDATE"
bash "$(dirname "$0")/check-stack.sh"

cat <<EOF

Promoted $CANDIDATE onto webgpu-stack (backup: $BACKUP).
Before pushing, run the LOCAL gates CI cannot cover:

  1. Rebuild BOTH binaries (serially!) at this HEAD:
       scons platform=windows target=editor accesskit=no d3d12=no
       scons platform=web target=template_release dev_mode=yes debug_symbols=no use_closure_compiler=no threads=yes webgpu=yes
  2. Strict probe (real GPU):
       PROBE_REQUIRE_PIXELS=1 PROBE_CHANNEL=chrome node misc/webgpu_scripts/loader-smoke.mjs bin probe
  3. Loader smoke:  node misc/webgpu_scripts/loader-smoke.mjs bin
  4. Re-export + boot the demo projects (0 error lines), headset spot-check.
  5. Push (with approval):  git push --force-with-lease origin webgpu-stack

Recovery at any point:  git reset --hard $BACKUP
EOF
