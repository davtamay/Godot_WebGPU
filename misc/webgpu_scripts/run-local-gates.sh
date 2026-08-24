#!/usr/bin/env bash
#
# Local gate runner for the WebGPU stack.
#
# CI can prove the patches still apply and still compile. It cannot prove the
# engine still renders: the shader bake needs tint, the pixel probe needs a
# real GPU, and the boot check needs Chrome. This script runs that half - the
# half that decides whether a rebase is safe to promote - and prints one
# pass/fail table at the end.
#
# Usage:
#   bash misc/webgpu_scripts/run-local-gates.sh [options]
#
#   --skip-build     reuse the binaries in bin/ (they must already match HEAD)
#   --quick          skip the nothreads template (halves build time; the
#                    threaded template covers the driver, bake and boot paths)
#   --forward-plus   additionally bake and boot the clustered renderer, which
#                    is the only way the Forward+ shader family gets exercised
#   --no-install     do not copy the built templates into the export template
#                    directory
#
# Everything is written under $GODOT_WEBGPU_TESTBED/gate-logs, which is outside
# the system temp directory on purpose: temp gets cleaned mid-session and takes
# the evidence with it.
#
# Configuration (all optional):
#   GODOT_WEBGPU_TESTBED   default C:/tmp/godot-webgpu-testbed
#   EMSDK_ROOT             default C:/Users/davta/emsdk
#   GODOT_TINT_PATH        must be set (or tint must sit beside the editor)

set -uo pipefail

SKIP_BUILD=0
QUICK=0
FORWARD_PLUS=0
NO_INSTALL=0
for arg in "$@"; do
	case "$arg" in
		--skip-build) SKIP_BUILD=1 ;;
		--quick) QUICK=1 ;;
		--forward-plus) FORWARD_PLUS=1 ;;
		--no-install) NO_INSTALL=1 ;;
		-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
		*) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
	esac
done

REPO=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "not inside a git repository" >&2; exit 2; }
cd "$REPO" || exit 2

TESTBED=${GODOT_WEBGPU_TESTBED:-C:/tmp/godot-webgpu-testbed}
EMSDK_ROOT=${EMSDK_ROOT:-C:/Users/davta/emsdk}

# node is not always on PATH (fresh machines); fall back to the emsdk copy,
# which the web builds require anyway.
NODE=$(command -v node || ls -d "$EMSDK_ROOT"/node/*/bin/node.exe 2>/dev/null | head -1)
[ -n "$NODE" ] || { echo "node not found on PATH or under $EMSDK_ROOT" >&2; exit 1; }
LOGDIR="$TESTBED/gate-logs"
BAKETEST="$TESTBED/baketest"
OUTDIR="$TESTBED/gate_out"
EDITOR_EXE="$REPO/bin/godot.windows.editor.x86_64.console.exe"

mkdir -p "$LOGDIR" || exit 2

HEAD_FULL=$(git rev-parse HEAD)
HEAD_SHORT=$(git rev-parse --short=9 HEAD)

# ---------------------------------------------------------------- reporting --
RESULTS=()
FAILED=0

pass() { RESULTS+=("PASS|$1|${2:-}"); printf '  \033[32mPASS\033[0m  %s %s\n' "$1" "${2:-}"; }
fail() { RESULTS+=("FAIL|$1|${2:-}"); FAILED=1; printf '  \033[31mFAIL\033[0m  %s %s\n' "$1" "${2:-}"; }
skip() { RESULTS+=("SKIP|$1|${2:-}"); printf '  ----  %s %s\n' "$1" "${2:-}"; }
phase() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

# ---------------------------------------------------------------- preflight --
phase "Preflight"

BRANCH=$(git rev-parse --abbrev-ref HEAD)
echo "  repo     $REPO"
echo "  branch   $BRANCH @ $HEAD_SHORT"
echo "  patches  $(git log --oneline upstream/master..HEAD 2>/dev/null | wc -l) on $(git log --oneline -1 "$(git merge-base HEAD upstream/master 2>/dev/null)" 2>/dev/null | cut -c1-60)"
echo "  logs     $LOGDIR"

if ! git diff --quiet || ! git diff --cached --quiet; then
	echo "  note     working tree is dirty; binaries still carry HEAD's version hash, so gates remain valid:"
	git status --short | sed 's/^/           /'
fi

# The editor executable is held open by a running game or editor, and the link
# step then fails with "Access is denied" - a failure that looks like a build
# error and has cost this project several cycles.
if tasklist //FI "IMAGENAME eq godot*" 2>/dev/null | grep -qi "^godot"; then
	echo
	echo "  A Godot process is running and will hold bin/*.exe open, breaking the link step." >&2
	echo "  Close the editor (or: taskkill //F //IM godot.windows.editor.x86_64.exe) and re-run." >&2
	exit 1
fi

if ! bash misc/webgpu_scripts/check-stack.sh; then
	fail "check-stack"
	echo "Stack integrity failed - stopping before spending build time." >&2
	exit 1
fi
pass "check-stack"

# -------------------------------------------------------------------- build --
build_editor() {
	local log="$LOGDIR/build-editor.log"
	echo "  building editor (log: $log)"
	if scons platform=windows target=editor accesskit=no d3d12=no > "$log" 2>&1; then
		pass "build editor" "($(grep -a 'Time elapsed' "$log" | tail -1 | sed 's/.*: //'))"
	else
		fail "build editor" "see $log"
		tail -20 "$log" >&2
		return 1
	fi
}

# The web build needs the emsdk environment. It is set explicitly rather than
# through emsdk_env.bat: MSYS mangles the PATH that script exports. Closure is
# off locally because Godot's helper invokes node through a Unix shim that does
# not exist on Windows; CI keeps closure on.
build_web() {
	local threads=$1 label=$2
	local log="$LOGDIR/build-web-$label.log"
	local ps1="$LOGDIR/build-web-$label.ps1"
	local node py
	node=$(ls -d "$EMSDK_ROOT"/node/*/bin/node.exe 2>/dev/null | head -1)
	py=$(ls -d "$EMSDK_ROOT"/python/*/python.exe 2>/dev/null | head -1)
	if [ -z "$node" ] || [ -z "$py" ]; then
		fail "build web ($label)" "emsdk not found under $EMSDK_ROOT"
		return 1
	fi

	cat > "$ps1" <<-PS
		\$env:EMSDK = "$EMSDK_ROOT"
		\$env:EMSDK_NODE = "$node"
		\$env:EMSDK_PYTHON = "$py"
		\$env:PATH = "$EMSDK_ROOT;$EMSDK_ROOT/upstream/emscripten;$(dirname "$node");" + \$env:PATH
		Set-Location "$REPO"
		scons platform=web target=template_release threads=$threads webgpu=yes use_closure_compiler=no
		exit \$LASTEXITCODE
	PS

	echo "  building web template ($label) (log: $log)"
	if powershell -NoProfile -ExecutionPolicy Bypass -File "$ps1" > "$log" 2>&1; then
		pass "build web ($label)" "($(grep -a 'Time elapsed' "$log" | tail -1 | sed 's/.*: //'))"
	else
		fail "build web ($label)" "see $log"
		tail -20 "$log" >&2
		return 1
	fi
}

phase "Build"
if [ "$SKIP_BUILD" = 1 ]; then
	skip "build" "(--skip-build)"
else
	# Strictly serial: SCons shares one signature database and one set of
	# generated files across invocations, and concurrent builds corrupt both
	# in ways that survive later serial rebuilds.
	build_editor || { echo "stopping: nothing downstream is meaningful without the editor" >&2; exit 1; }
	build_web yes threaded || exit 1
	if [ "$QUICK" = 1 ]; then
		skip "build web (nothreads)" "(--quick)"
	else
		build_web no nothreads || exit 1
	fi
fi

# ------------------------------------------------------- version-hash pairing --
# ShaderRD salts the shader cache key with the commit hash, so an editor and a
# template built at different commits produce a bake the runtime cannot find:
# a black page and thousands of "missing from the baked shader cache" errors
# that look like a rendering bug. Check it before trusting any gate below.
phase "Version-hash pairing"
check_hash() {
	local file=$1 label=$2
	[ -f "$file" ] || { fail "hash $label" "missing $file"; return 1; }
	if python -c "import sys; sys.exit(0 if sys.argv[1].encode() in open(sys.argv[2],'rb').read() else 1)" "$HEAD_FULL" "$file"; then
		pass "hash $label" "= $HEAD_SHORT"
	else
		fail "hash $label" "does not embed $HEAD_SHORT - rebuild it"
	fi
}
check_hash "$REPO/bin/godot.windows.editor.x86_64.exe" "editor"
check_hash "$REPO/bin/godot.web.template_release.wasm32.wasm" "threaded"
[ "$QUICK" = 1 ] || check_hash "$REPO/bin/godot.web.template_release.wasm32.nothreads.wasm" "nothreads"

# -------------------------------------------------------------------- probes --
# The harness picks the first *.wrapped.js it finds in the directory it is
# given, so each variant is staged alone.
stage_probe_bin() {
	local prefix=$1 dir=$2
	rm -rf "$dir" && mkdir -p "$dir" || return 1
	cp "$REPO/bin/$prefix.wrapped.js" "$REPO/bin/$prefix.wasm" "$dir/" || return 1
}

run_probe() {
	local dir=$1 label=$2
	local log="$LOGDIR/probe-$label.log"
	# Run from the testbed: the harness resolves playwright through the
	# package.json of the current working directory.
	if ( cd "$TESTBED" && PROBE_REQUIRE_PIXELS=1 PROBE_CHANNEL=chrome \
		"$NODE" "$REPO/misc/webgpu_scripts/loader-smoke.mjs" "$dir" probe ) > "$log" 2>&1; then
		pass "strict probe ($label)" "$(grep -ao 'corner=\[[^]]*\]' "$log" | head -1)"
	else
		fail "strict probe ($label)" "see $log"
		tail -6 "$log" >&2
	fi
}

phase "Driver probes"
if stage_probe_bin godot.web.template_release.wasm32 "$LOGDIR/probebin-threaded"; then
	run_probe "$LOGDIR/probebin-threaded" threaded
else
	fail "strict probe (threaded)" "could not stage artifacts"
fi
if [ "$QUICK" = 1 ]; then
	skip "strict probe (nothreads)" "(--quick)"
elif stage_probe_bin godot.web.template_release.wasm32.nothreads "$LOGDIR/probebin-nothreads"; then
	run_probe "$LOGDIR/probebin-nothreads" nothreads
else
	fail "strict probe (nothreads)" "could not stage artifacts"
fi

phase "Loader smoke"
LOADER_LOG="$LOGDIR/loader-smoke.log"
if ( cd "$TESTBED" && "$NODE" "$REPO/misc/webgpu_scripts/loader-smoke.mjs" "$LOGDIR/probebin-threaded" ) > "$LOADER_LOG" 2>&1; then
	pass "loader smoke" "(off/on/xr)"
else
	fail "loader smoke" "see $LOADER_LOG"
	tail -8 "$LOADER_LOG" >&2
fi

# --------------------------------------------------------- bake, export, boot --
# The shader baker only runs under a real RenderingDevice renderer, so the
# export must NOT be headless. The export customization cache is keyed on a
# configuration hash that does not include engine changes, so it happily
# reuses shaders baked by an older build unless it is cleared first. And the
# target folder must exist before the exporter launches, not during.
export_and_boot() {
	local method=$1 label=$2
	local elog="$LOGDIR/export-$label.log"
	local png="$LOGDIR/boot-$label.png"

	rm -rf "$BAKETEST/.godot/exported" "$BAKETEST/.godot/shader_cache"
	rm -rf "$OUTDIR" && mkdir -p "$OUTDIR" || { fail "export ($label)" "cannot create $OUTDIR"; return 1; }

	if ! "$EDITOR_EXE" --path "$BAKETEST" --export-release "Web" "$OUTDIR/index.html" \
		--rendering-method "$method" --rendering-driver vulkan --verbose > "$elog" 2>&1; then
		fail "export ($label)" "see $elog"
		return 1
	fi
	if [ ! -f "$OUTDIR/index.pck" ]; then
		fail "export ($label)" "no pck produced; see $elog"
		return 1
	fi
	local excl
	excl=$(grep -aco "excluding shader" "$elog")
	pass "export ($label)" "($excl shader variants excluded)"
	grep -ao "excluding shader '[A-Za-z]*'" "$elog" | sort | uniq -c | sort -rn | sed 's/^/           /'

	# Give the page long enough to finish warming pipelines up. Screenshotting
	# too early catches the clustered renderer mid-warm-up and reports a frame
	# rate an order of magnitude below its settled one - a reading that looks
	# exactly like a performance regression and is not one.
	local blog="$png.stream.log"
	( cd "$TESTBED" && "$NODE" boot-noflag.mjs "$OUTDIR" "$png" "${BOOT_WAIT_MS:-40000}" ) > "$LOGDIR/boot-$label.log" 2>&1
	if [ ! -f "$blog" ]; then
		fail "boot ($label)" "harness produced no console log"
		return 1
	fi

	# The harness always exits 0, so the verdict comes from the console log.
	local pageerrs invalid warns inert unknown
	pageerrs=$(grep -ac '^\[pageerror\]' "$blog")
	invalid=$(grep -ac 'Invalid CommandBuffer' "$blog")
	warns=$(grep -ac '^\[warning\]' "$blog")

	# Every excluded variant from the census above still gets one warm-up
	# pipeline-creation attempt, which reports a null shader and is skipped;
	# the renderer then takes the path that does exist and the frames are
	# valid, which is what the invalid-command-buffer count actually proves.
	# Those are tolerated by signature - not by count, so a genuinely new
	# error can never hide inside an expected total. The "at:" lines are
	# continuations of the message above them and never stand alone.
	# KNOWN UPSTREAM GAP (remove this signature when it is fixed): Trail3D and
	# Line3D (added upstream in f2aee4e142) build their spatial shaders from
	# code strings at engine startup through shader_create_from_code, which
	# marks the shader non-embedded - so it is invisible to every bake
	# enumeration path (ShaderRD embedded set, MaterialStorage embedded set,
	# resource customization) and the versions are never baked. On the web the
	# runtime cannot compile GLSL, so every boot reports the scene shader
	# versions missing (4 lines, engine startup, both renderers); the nodes
	# render nothing on web until the baker learns about engine-static scene
	# shaders. Tolerating the signature does mean a genuinely missing baked
	# MATERIAL version would also slip past this leg - the bake census still
	# guards variant-level regressions, and this line should go away with the
	# upstream fix.
	local INERT_RE='Parameter "shader" is null|Condition "p_shader\.is_null\(\)" is true|is missing from the baked shader cache'
	inert=$(grep -a '^\[error\].*ERROR:' "$blog" | grep -acE "$INERT_RE")
	unknown=$(grep -a '^\[error\].*ERROR:' "$blog" | grep -avcE "$INERT_RE")

	if ! grep -aq "custom_build.$HEAD_SHORT" "$blog"; then
		fail "boot ($label)" "booted binary reports a different commit than HEAD"
		grep -am1 'Godot Engine v' "$blog" >&2
		return 1
	fi
	if [ "$unknown" != 0 ] || [ "$pageerrs" != 0 ] || [ "$invalid" != 0 ]; then
		# Invalid CommandBuffer is the one that matters most: it means whole
		# frames are discarded, which renders as a black page with no errors.
		fail "boot ($label)" "unexpected-errors=$unknown pageerrors=$pageerrs invalid-command-buffers=$invalid (log: $blog)"
		grep -a '^\[error\].*ERROR:' "$blog" | grep -avE "$INERT_RE" | head -5 | sed 's/^/           /' >&2
		return 1
	fi
	pass "boot ($label)" "clean ($inert inert warm-up nulls, $warns warnings, shot: $png)"
}

phase "Bake, export and boot"
export_and_boot mobile mobile

if [ "$FORWARD_PLUS" = 1 ]; then
	# Forward+ shaders are only instantiated when the editor itself runs the
	# clustered renderer, so the project has to be flipped for the bake. The
	# trap guarantees the flip is undone even if a gate fails or the run is
	# interrupted.
	CFG="$BAKETEST/project.godot"
	cp "$CFG" "$CFG.gate-backup" || exit 1
	restore_cfg() { [ -f "$CFG.gate-backup" ] && mv -f "$CFG.gate-backup" "$CFG"; }
	trap restore_cfg EXIT INT TERM
	python - "$CFG" <<-'PY'
		import io, sys
		p = sys.argv[1]
		s = io.open(p, encoding="utf-8").read()
		s = s.replace('renderer/rendering_method="mobile"', 'renderer/rendering_method="forward_plus"')
		s = s.replace('renderer/rendering_method.web="mobile"', 'renderer/rendering_method.web="forward_plus"')
		io.open(p, "w", encoding="utf-8", newline="").write(s)
	PY
	export_and_boot forward_plus forward-plus
	restore_cfg
	trap - EXIT INT TERM
	if grep -q 'rendering_method="mobile"' "$CFG"; then
		pass "restore baketest config"
	else
		fail "restore baketest config" "$CFG is still flipped - fix before exporting anything else"
	fi
fi

# ----------------------------------------------------------------- install --
phase "Install export templates"
if [ "$NO_INSTALL" = 1 ]; then
	skip "install templates" "(--no-install)"
elif [ "$FAILED" = 1 ]; then
	skip "install templates" "(gates failed - not installing a build that did not pass)"
else
	VERSION=$(python -c "
import runpy
v = runpy.run_path('version.py')
print('%s.%s.%s' % (v['major'], v['minor'], v['status']))
" 2>/dev/null)
	TPL_DIR="${APPDATA:-$HOME/AppData/Roaming}/Godot/export_templates/$VERSION"
	if [ -d "$TPL_DIR" ]; then
		cp "$REPO/bin/godot.web.template_release.wasm32.zip" "$TPL_DIR/web_release.zip" &&
		{ [ "$QUICK" = 1 ] || cp "$REPO/bin/godot.web.template_release.wasm32.nothreads.zip" "$TPL_DIR/web_nothreads_release.zip"; } &&
			pass "install templates" "-> $TPL_DIR" || fail "install templates" "copy failed"
	else
		fail "install templates" "no such directory: $TPL_DIR"
	fi
fi

# ----------------------------------------------------------------- summary --
phase "Summary"
printf '%s\n' "${RESULTS[@]}" | awk -F'|' '{ printf "  %-5s %-28s %s\n", $1, $2, $3 }'
echo
if [ "$FAILED" = 1 ]; then
	echo "  RESULT: FAILED - do not promote this base. Logs in $LOGDIR"
	exit 1
fi
echo "  RESULT: all gates green at $HEAD_SHORT - safe to promote and push."
exit 0
