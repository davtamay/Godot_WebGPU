# Upstream sync protocol

How this fork stays current with godotengine/godot without two people (or two
agent sessions) solving the same Monday twice. This file is tracked so every
checkout, including cloud sessions that never see local notes, reads the same
rules.

## Who does what

| Actor | May do | Never does |
|---|---|---|
| `upstream-sync.yml` (Mondays 06:17 UTC, or dispatched by hand) | Replays the stack from `origin/webgpu-stack` onto upstream master; on a clean replay pushes `auto-rebase/<date>` and tag `backup/auto-<date>`; files or updates the `upstream-sync` issues | Touches `webgpu-stack` |
| The landscape routine (Mondays 13:00 UTC) | Reads and reports: files one `landscape-watch` issue | Rebases, cherry-picks, pushes anything, dispatches workflows, comments on issues |
| A cloud session started by a person | Investigates, drafts, answers; may prepare a patch in its own checkout for the person to review | Pushes to `webgpu-stack` or to `auto-rebase/*`; creates candidate branches; numbers patches on its own |
| The local session on the gated machine | Hand rebases, runs `misc/webgpu_scripts/run-local-gates.sh`, promotes | Pushes without green local gates and an explicit go from the owner |

The candidate branch exists so the robot's work is visible; it is disposable
and the next sync `--force`s over the same dated name. The stack is
`origin/webgpu-stack` and nothing else.

## Before any hand rebase

1. `git fetch origin upstream --prune` and look at both:
   the newest `origin/auto-rebase/<date>` and the open issues labeled
   `upstream-sync`. If a candidate for this week exists, start from it
   (`git range-diff` it against the local stack) instead of re-resolving
   the same conflicts; a second resolution of the same week is the hazard
   this file exists to prevent (it happened on 2026-09-14, 2026-09-28 and
   2026-10-19).
2. Snapshot: `git branch backup/stack-$(date +%Y%m%d) HEAD` before the rebase,
   push it with the promotion.
3. Replay, resolve, then check: `git range-diff backup/stack-<date>...HEAD`
   shows only the expected drift; `misc/webgpu_scripts/check-stack.sh` is OK;
   every subject starts with `[wgpu NN]`.
4. Gate: `bash misc/webgpu_scripts/run-local-gates.sh --forward-plus` (all
   three binaries at the new head, both probes, closure, loader smoke, both
   bake censuses, both boots). CI proves the replay compiles; only the local
   gates prove it renders.
5. Promote: `git push --force-with-lease=webgpu-stack:<old origin head> origin
   webgpu-stack`, push the backup branch, dispatch `upstream-sync.yml` by hand
   so the robot confirms a clean replay the same day, close the sync issue
   with what changed.

## Numbering

One number per patch, assigned when the patch lands on `webgpu-stack`, never
reused. The next number is one more than the highest `[wgpu NN]` on
`origin/webgpu-stack`; a session that cannot see the owner's unpushed work
must say so in the issue rather than guess. Gaps are allowed (51 was dropped
as obsolete); renumbering existing patches is not done.

## When a sync fails

The robot opens or updates an issue titled `Upstream sync: FAILED` naming the
cause: the conflicting patch and files, or the subjects that break the
naming rule. It never appends a failure to a `candidate ready` issue. Resolve
it locally per the steps above; a clean replay closes the FAILED issue.

## When a candidate goes unpromoted

A candidate is useful for one week. If the `candidate ready` issue is still
open when the next sync runs, the robot says so in its title and report with
the age in days and how far upstream has moved. Treat that as the first item
of the week: every week of drift makes the next hand rebase larger (475
upstream commits in October 2026).

## Record keeping

Decisions that affect the stack (which of two resolutions won, why a patch was
dropped or folded) go in the sync issue of that week, where the next person
or session will look. Do not rely on notes outside the repository.
