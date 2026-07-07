# WebGPU - Risk Register

| Risk | Likelihood | Impact | Early signal | Mitigation | Review |
|------|------------|--------|--------------|------------|--------|
| Upstream refactors rendering_device_driver.h contract | M | H | PRs touching that header | Watch file via GitHub; pause stack, port once, don't chase weekly | monthly |
| Emscripten WebGPU binding churn (emdawnwebgpu vs legacy) | M | M | emsdk release notes | Pin emsdk; isolate binding behind one glue file | monthly |
| WASM size budget rejected by maintainers | M | H | Stage-0 size numbers | Measure early; flag-off must be zero-delta; discuss budget before shader patch | at Patch 1 |
| rerere poisoning | M | M | Regressions reappearing after syncs | Review staged auto-resolutions; `git rerere forget` | each rebase |
| Chrome headless-WebGPU flag churn breaks CI | H | L | Red CI on browser jobs only | Pin Playwright/Chromium; flags documented in webgpu-testing.md | as hit |
| Rebase time inflation (>1h two weeks running) | M | H | Timer | Identify churning patch; shrink shared surface | weekly |
| Solo bus factor / three skipped syncs | M | H | Calendar | Shrink stack rather than let it drift | monthly |
| Maintainer appetite shifts (other implementation adopted) | M | H | Proposal #6646 activity | Pivot to contributing there; see stop signals | monthly |

Stop/pivot signals: rendering team declines the patch shape; sustained RDD
contract refactor upstream; three consecutive missed weekly syncs; a
credible upstream-endorsed WebGPU effort appears elsewhere.
