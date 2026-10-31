#!/usr/bin/env bash
# Report a finished baseline run in the issues the sync protocol says to read.
# Usage: report-build-verdict.sh <run id>   (DRY_RUN=1 prints instead of writing)
#
# The sync robot proves that the patches still apply, never that they still
# compile: on 2026-10-31 all 172 replayed cleanly and upstream had changed a
# virtual they override, so "candidate ready" stood next to a red build. This
# script turns the build result into the same kind of issue a failed replay
# gets. Only the head of webgpu-stack and of the newest candidate count: an
# older run has nothing to say about what would be promoted today.
set -euo pipefail

RUN_ID="${1:?usage: report-build-verdict.sh <run id>}"
: "${GH_REPO:?GH_REPO must name this fork}"

STACK_TITLE="Baseline CI: webgpu-stack is red"
BUILD_TITLE="Upstream sync: FAILED - candidate does not build"
READY_TITLE="Upstream sync: candidate ready"

gh_write() {
	if [ -n "${DRY_RUN:-}" ]; then
		printf 'would run: gh'
		printf ' %q' "$@"
		printf '\n'
	else
		gh "$@"
	fi
}

open_issue() {
	gh issue list --label upstream-sync --state open --json number,title \
		--jq '.[] | "\(.number) \(.title)"' | grep -F " $1" | head -1 | cut -d' ' -f1 || true
}

FIELDS=$(gh run view "$RUN_ID" --json headBranch,event,status,conclusion,headSha,url \
	--jq '[.headBranch, .event, .status, .conclusion, .headSha, .url] | @tsv')
IFS=$'\t' read -r BRANCH EVENT STATUS CONCLUSION SHA URL <<<"$FIELDS"

if [ "$EVENT" != push ] || [ "$STATUS" != completed ]; then
	echo "Run $RUN_ID: event=$EVENT status=$STATUS, nothing to report."
	exit 0
fi
case "$CONCLUSION" in
success | failure | timed_out) ;;
*)
	echo "Run $RUN_ID concluded '$CONCLUSION', nothing to report."
	exit 0
	;;
esac

case "$BRANCH" in
webgpu-stack) TITLE=$STACK_TITLE ;;
auto-rebase/*)
	TITLE=$BUILD_TITLE
	NEWEST=$(gh api "repos/$GH_REPO/git/matching-refs/heads/auto-rebase/" --jq '.[].ref' |
		sed 's|refs/heads/||' | sort | tail -1)
	if [ "$BRANCH" != "$NEWEST" ]; then
		echo "$BRANCH is not the newest candidate ($NEWEST), nothing to report."
		exit 0
	fi
	;;
*)
	echo "Branch $BRANCH is not reported."
	exit 0
	;;
esac

HEAD_SHA=$(gh api "repos/$GH_REPO/git/ref/heads/$BRANCH" --jq .object.sha)
if [ "$SHA" != "$HEAD_SHA" ]; then
	echo "Run $RUN_ID built ${SHA:0:10}, $BRANCH is at ${HEAD_SHA:0:10}, nothing to report."
	exit 0
fi

SHORT=${SHA:0:10}
NUM=$(open_issue "$TITLE")

if [ "$CONCLUSION" = success ]; then
	if [ -n "$NUM" ]; then
		gh_write issue close "$NUM" --comment "Resolved: baseline CI is green on \`$BRANCH\` ($SHORT). $URL"
	fi
	READY=$(open_issue "$READY_TITLE")
	if [ "$BRANCH" != webgpu-stack ] && [ -n "$READY" ]; then
		gh_write issue comment "$READY" --body "Baseline CI is green on \`$BRANCH\` ($SHORT): it builds on GCC and emcc and both bake censuses hold. Only the local gates can prove it renders. $URL"
	fi
	echo "$BRANCH ($SHORT) is green."
	exit 0
fi

FAILED=$(gh api "repos/$GH_REPO/actions/runs/$RUN_ID/jobs" --jq '.jobs[] | select(.conclusion == "failure" or .conclusion == "timed_out") |
	"- \(.name): \([.steps[] | select(.conclusion == "failure") | .name] | join(", "))"')
# gh prints the compiler's colour codes in caret notation, so strip both forms.
LOG=$(gh run view "$RUN_ID" --log-failed 2>/dev/null |
	sed -E 's/(\x1b|\^\[)\[[0-9;]*[A-Za-z]//g' | cut -f3- | sed -E 's/^[0-9T:.-]+Z //' || true)
first_lines() {
	printf '%s\n' "$LOG" | grep -E "$1" | awk '!seen[$0]++' | head -8 | cut -c1-220 || true
}
ERRORS=$(first_lines ': (fatal )?error[: ]|JSC_[A-Z_]+')
[ -n "$ERRORS" ] || ERRORS=$(first_lines '^ *(ERROR|FAIL|NEW|GREW)|\*\*\* \[|^::error')
[ -n "$ERRORS" ] || ERRORS="No error line matched; read the run log."

FENCE='```'
if [ "$BRANCH" = webgpu-stack ]; then
	L1="\`webgpu-stack\` ($SHORT) does not pass baseline CI. This is the stack itself, not a candidate."
else
	L1="\`$BRANCH\` ($SHORT) replayed cleanly but does not pass baseline CI. Do not promote it. A clean replay proves the patches apply, not that they compile against what upstream changed around them. Fix it as a new patch on \`webgpu-stack\`, then dispatch \`upstream-sync.yml\` for a fresh candidate."
fi
BODY=$(printf '%s\n\n**Failed:**\n%s\n\n**First errors:**\n%s\n%s\n%s\n\nRun: %s' "$L1" "$FAILED" "$FENCE" "$ERRORS" "$FENCE" "$URL")

if [ -n "$NUM" ]; then
	gh_write issue comment "$NUM" --body "$BODY"
else
	CREATED=$(gh_write issue create --title "$TITLE" --label upstream-sync --body "$BODY")
	echo "$CREATED"
	NUM=${CREATED##*/}
fi

# A candidate that does not build is not ready: leave one issue saying so,
# not two that disagree. The next clean replay opens a fresh "ready" issue.
READY=$(open_issue "$READY_TITLE")
if [ "$BRANCH" != webgpu-stack ] && [ -n "$READY" ]; then
	gh_write issue close "$READY" --comment "\`$BRANCH\` does not build, see #$NUM."
fi
echo "$BRANCH ($SHORT) is red."
