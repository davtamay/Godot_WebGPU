# Device check

Runs a fixed set of rendering cases on whatever device opens the page and
compares sampled pixels with reference colours rendered natively (desktop
Vulkan, same Mobile renderer). Pixels are captured at the page level through
WebGPU (`devcheck.js` adds COPY_SRC to the canvas and maps a copy
asynchronously), so no engine change is involved and it runs on any phone.

The local gates (`run-local-gates.sh`) export this project with the
single-threaded template and run it in Chrome through `device-check.mjs`.
The step fails on any case outside tolerance, any failed numeric check, and
any console error or warning the page logs.

- `main.gd` - the cases (flat colour regions, sampled at normalized points),
  plus numeric checks with exact answers (GPU mesh readback).
- `golden.json` - reference colours. Regenerate after changing a case: run
  the project natively (`godot --path misc/webgpu_scripts/devcheck_fixture
  --rendering-method mobile --rendering-driver vulkan`); it writes the file
  and quits.
- `devcheck.js` - the page harness, copied next to `index.html` on export.
- `../device-check.mjs <export_dir> [shot.png]` - runs it in Chrome here.
- `../devcheck-serve.mjs <export_dir> <port> <reports_dir>` - HTTPS server
  for other devices; each report is saved as JSON and summarized on one line
  in its log. Needs `DEVCHECK_CERT_DIR`.

Cases: unshaded, MSAA 2x and 4x, alpha blending, a sky shader with and
without 3D scaling, the procedural sky (plain and with MSAA 4x + 0.8 scale),
geometry near the far plane, a shadowed floor, an orthographic shadow past
its range, a 16-bit index buffer with an odd index count, canvas shapes
redrawn every frame, a 2,000-box timing case (reported, not graded), and the
mesh readback check. Tolerance is 24 per channel.
