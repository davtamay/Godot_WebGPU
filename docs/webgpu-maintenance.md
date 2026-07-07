# WebGPU Patch Stack - Maintenance Guide

This repository maintains a **replayable patch stack** for an experimental
WebGPU backend, not a conventional fork. The branch is disposable; the
ordered patches are the product. Invariants:

1. `git log upstream/master..webgpu-stack` shows exactly the numbered
   patches, in PR order. No merge commits, ever.
2. Every patch builds and passes CI on its own.
3. Behavior with WebGPU disabled is a provable no-op vs. upstream.

## Branch table

| Branch | Base | Purpose | Rebased | Force-push |
|--------|------|---------|---------|------------|
| webgpu-stack | upstream/master | Clean ordered patch series (product) | weekly | yes (`--force-with-lease`) |
| webgpu-dev | webgpu-stack | Experiments; harvested via fixups | ad hoc | yes |
| demo/<tag> | stable tag | Stack replayed onto a release; frozen | never | no |
| pr/<nn>-<name> | upstream/master | Stack truncated at patch nn, for PRs | per review | yes |
| backup/stack-<d> | pre-rebase HEAD | Snapshots; range-diff anchors | never | no |

## Weekly sync procedure

    git fetch upstream --tags --prune
    git switch webgpu-stack
    git branch backup/stack-$(date +%Y%m%d) HEAD
    git rebase -i upstream/master
    # resolve conflicts; ALWAYS `git diff --staged` before --continue,
    # especially when rerere auto-resolved (rerere.autoUpdate is on)
    git log --oneline upstream/master..            # exactly N patches?
    git range-diff backup/stack-<date>...HEAD      # only expected drift?
    bash misc/webgpu_scripts/check-stack.sh        # boundaries clean?
    # build + smoke test (see webgpu-testing.md)
    git push --force-with-lease origin webgpu-stack

## rerere caveats

rerere replays recorded conflict resolutions silently. If a regression
reappears after a sync, suspect a poisoned recording:
`git rerere diff` during the rebase, `git rerere forget <path>`, re-resolve.

## Recovery

    git rebase --abort                       # mid-rebase
    git reset --hard backup/stack-<date>     # completed-but-bad rebase
    git reflog && git reset --hard HEAD@{n}  # no snapshot taken

## Demo branches

    git switch -c demo/<tag> webgpu-stack
    git rebase --onto <tag> upstream/master demo/<tag>

Frozen after creation. Any fix made on a demo branch gets a same-day
`git commit --fixup` into the stack, or a tracked issue.

## Worktrees

Main checkout = webgpu-stack (all rebases happen here).
`../godot-upstream` = pristine upstream/master (reference, bisects).
`../godot-dev` = webgpu-dev. Each worktree keeps its own SCons artifacts.
