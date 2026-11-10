"""Exercise headless editor import teardown with fresh project caches.

Usage: python import-shutdown-smoke.py <editor> <evidence-directory> [runs]
Keeps every log and fixture; never removes an existing project or cache.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

editor = Path(sys.argv[1]).resolve()
evidence = Path(sys.argv[2]).resolve()
runs = int(sys.argv[3]) if len(sys.argv) > 3 else 8
assert runs > 0
fixture = Path(__file__).resolve().parent / 'webxr_fixture'
evidence.mkdir(parents=True, exist_ok=True)
results = []
for index in range(runs):
    project = evidence / f'import-{index + 1}-{uuid.uuid4().hex[:8]}'
    project.mkdir()
    for name in ['project.godot', 'main.gd', 'main.gd.uid', 'main.tscn']:
        shutil.copyfile(fixture / name, project / name)
    log_path = evidence / f'{project.name}.log'
    with log_path.open('w', encoding='utf-8') as log:
        completed = subprocess.run(
            [str(editor), '--headless', '--path', str(project), '--import'],
            stdout=log, stderr=subprocess.STDOUT, timeout=120,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    log_text = log_path.read_text(encoding='utf-8', errors='replace')
    passed = completed.returncode == 0 and 'ERROR:' not in log_text
    results.append({'run': index + 1, 'exit_code': completed.returncode, 'passed': passed, 'log': str(log_path)})
    (evidence / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    print(f"{'PASS' if passed else 'FAIL'} fresh import {index + 1}: exit={completed.returncode}", flush=True)
    if not passed:
        sys.exit(1)
print(f'PASS: all {runs} fresh imports exited cleanly.')
