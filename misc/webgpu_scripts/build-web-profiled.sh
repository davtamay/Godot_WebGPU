#!/usr/bin/env bash
#
# Build a web template stripped to one project's build profile.
#
# Godot's web template carries the whole engine: every class registered, both
# renderers, every server, whether or not the project touches them. A build
# profile is the engine's own answer to that - it is what the editor's
# "Customize Engine Build Configuration" dialog writes - but it is a
# COMPILE-TIME CONTRACT WITH ONE PROJECT: the classes it leaves out are gone
# from the binary, so a project that grows past the profile it was built
# against fails at load. That is why this script always re-detects instead of
# reusing a profile from disk, and why it verifies by running the export.
#
# Usage:
#   bash misc/webgpu_scripts/build-web-profiled.sh <project-dir> [options]
#
#   --preset NAME    export preset to verify with (default "Web")
#   --method NAME    renderer to export with (default "mobile")
#   --threads no     build the single-threaded template (default yes)
#   --detect-only    write the profile and stop, without building
#   --skip-verify    build the template but do not export and boot it
#
# Configuration (all optional):
#   GODOT_WEBGPU_TESTBED   default C:/tmp/godot-webgpu-testbed
#   EMSDK_ROOT             default C:/Users/davta/emsdk
#   GODOT_TINT_PATH        must be set (or tint must sit beside the editor)

set -uo pipefail

PRESET="Web"
METHOD="mobile"
THREADS="yes"
DETECT_ONLY=0
SKIP_VERIFY=0
PROJECT=""
while [ $# -gt 0 ]; do
	case "$1" in
		--preset) PRESET=$2; shift 2 ;;
		--method) METHOD=$2; shift 2 ;;
		--threads) THREADS=$2; shift 2 ;;
		--detect-only) DETECT_ONLY=1; shift ;;
		--skip-verify) SKIP_VERIFY=1; shift ;;
		-h|--help) sed -n '2,27p' "$0"; exit 0 ;;
		-*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
		*) [ -z "$PROJECT" ] || { echo "only one project may be given" >&2; exit 2; }; PROJECT=$1; shift ;;
	esac
done
[ -n "$PROJECT" ] || { sed -n '2,27p' "$0"; exit 2; }

REPO=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "not inside a git repository" >&2; exit 2; }
cd "$REPO" || exit 2

TESTBED=${GODOT_WEBGPU_TESTBED:-C:/tmp/godot-webgpu-testbed}
EMSDK_ROOT=${EMSDK_ROOT:-C:/Users/davta/emsdk}
LOGDIR="$TESTBED/gate-logs"
EDITOR_EXE="$REPO/bin/godot.windows.editor.x86_64.console.exe"
PLUGIN_SRC="$REPO/misc/webgpu_scripts/build_profile_tool"
NODE=$(command -v node || ls -d "$EMSDK_ROOT"/node/*/bin/node.exe 2>/dev/null | head -1)

PROJECT=$(cd "$PROJECT" 2>/dev/null && pwd) || { echo "no such directory: $PROJECT" >&2; exit 2; }
PROJECT_NAME=$(basename "$PROJECT")
PROFILE="$LOGDIR/$PROJECT_NAME.build"
OUTDIR="$TESTBED/profiled_${PROJECT_NAME}_out"
mkdir -p "$LOGDIR" || exit 2

# Classes a web build needs whether or not any scene names them. Empty by
# default: the strip surface is only Node and Resource subclasses, and those
# are exactly what detection reads out of the project's own files.
KEEP_CLASSES=${GODOT_BUILD_PROFILE_KEEP:-}

# Build options the TARGET decides, not the project. Detection answers "does
# this project use it"; for these three the answer does not matter, because a
# web export needs its renderer, its rendering device, and the WebGL fallback
# the loader drops to when a browser has no WebGPU. Everything else follows
# the project, which is the point of a profile.
KEEP_OPTIONS="rendering_device forward_mobile_renderer opengl3"

RESULTS=()
FAILED=0
pass() { RESULTS+=("PASS|$1|${2:-}"); printf '  \033[32mPASS\033[0m  %s %s\n' "$1" "${2:-}"; }
fail() { RESULTS+=("FAIL|$1|${2:-}"); FAILED=1; printf '  \033[31mFAIL\033[0m  %s %s\n' "$1" "${2:-}"; }
phase() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

# ---------------------------------------------------------------- preflight --
phase "Preflight"
echo "  project  $PROJECT"
echo "  profile  $PROFILE"
echo "  logs     $LOGDIR"
[ -f "$PROJECT/project.godot" ] || { fail "preflight" "$PROJECT has no project.godot"; exit 1; }
[ -x "$EDITOR_EXE" ] || { fail "preflight" "no editor at $EDITOR_EXE"; exit 1; }
[ -d "$PLUGIN_SRC" ] || { fail "preflight" "no plugin at $PLUGIN_SRC"; exit 1; }
if tasklist //FI "IMAGENAME eq godot*" 2>/dev/null | grep -qi "^godot"; then
	fail "preflight" "a Godot process is running and will hold bin/*.exe open"
	exit 1
fi

# The baked shader cache is keyed by the engine's version hash, so an editor
# built at a different commit than the template bakes shaders the template
# will never ask for - hundreds of missing-shader errors that read like a
# renderer regression and are not one. The template is about to be built from
# HEAD, so the editor has to be at HEAD too.
HEAD_SHORT=$(git rev-parse --short=9 HEAD)
EDITOR_HASH=$("$EDITOR_EXE" --version 2>/dev/null | tail -1 | sed 's/.*custom_build\.//')
if [ "$EDITOR_HASH" != "$HEAD_SHORT" ]; then
	fail "preflight" "editor is at $EDITOR_HASH but HEAD is $HEAD_SHORT"
	echo "  Rebuild it first: scons platform=windows target=editor accesskit=no d3d12=no" >&2
	exit 1
fi
pass "preflight" "(editor and HEAD both $HEAD_SHORT)"

# ------------------------------------------------------------------ detect --
# The plugin is installed into the project, run once, and removed again: the
# editor rewrites project.godot as it exits, so the file is restored from a
# copy rather than edited back.
phase "Detect"
PLUGIN_DIR="$PROJECT/addons/build_profile_tool"
BACKUP="$LOGDIR/$PROJECT_NAME.project.godot.orig"
cleanup_project() {
	[ -f "$BACKUP" ] && cp "$BACKUP" "$PROJECT/project.godot"
	rm -rf "$PLUGIN_DIR"
}
# A scan still in flight would describe a subset of the project, so the import
# runs to completion first and the plugin waits for the editor's own file
# system besides. It runs before the plugin is installed because the plugin
# plays no part in importing. The editor has been seen to crash on its way out
# of a headless import after the work itself finished, so this reports rather
# than stops: the plugin's own wait is what the profile's completeness rests
# on.
"$EDITOR_EXE" --headless --import --path "$PROJECT" > "$LOGDIR/profile-import.log" 2>&1
import_rc=$?
[ "$import_rc" = 0 ] || echo "  note: import exited $import_rc; see $LOGDIR/profile-import.log"

cp "$PROJECT/project.godot" "$BACKUP" || exit 1
trap cleanup_project EXIT
mkdir -p "$PLUGIN_DIR" && cp "$PLUGIN_SRC"/* "$PLUGIN_DIR/" || { fail "detect" "cannot install the plugin"; exit 1; }
if ! python "$REPO/misc/webgpu_scripts/profile_helper.py" enable "$PROJECT/project.godot"; then
	fail "detect" "cannot enable the plugin"
	exit 1
fi
rm -f "$PROFILE"
GODOT_BUILD_PROFILE_OUT="$PROFILE" GODOT_BUILD_PROFILE_KEEP="$KEEP_CLASSES" \
	"$EDITOR_EXE" --editor --path "$PROJECT" > "$LOGDIR/profile-detect.log" 2>&1
cleanup_project
trap - EXIT

if [ ! -f "$PROFILE" ]; then
	fail "detect" "no profile written; see $LOGDIR/profile-detect.log"
	grep -a "BUILD_PROFILE\|SCRIPT ERROR" "$LOGDIR/profile-detect.log" | head -5 >&2
	exit 1
fi
pass "detect" "($(grep -a 'BUILD_PROFILE classes_disabled' "$LOGDIR/profile-detect.log" | tail -1 | sed 's/BUILD_PROFILE //'))"

# ------------------------------------------------------------------ curate --
phase "Curate"
if ! python "$REPO/misc/webgpu_scripts/profile_helper.py" curate "$PROFILE" $KEEP_OPTIONS; then
	fail "curate" "cannot rewrite the profile"
	exit 1
fi
pass "curate"

if [ "$DETECT_ONLY" = 1 ]; then
	printf '\n  profile written to %s\n' "$PROFILE"
	exit 0
fi

# ------------------------------------------------------------------- build --
phase "Build"
ZIP="$REPO/bin/godot.web.template_release.wasm32"
[ "$THREADS" = "yes" ] || ZIP="$ZIP.nothreads"
ZIP="$ZIP.zip"
# Keep whatever template is there to report a size against. It is named for
# what it is - the one from before this run - because on a second run that is
# a profiled template too, and calling it "unprofiled" would quietly turn a
# no-op into a claim.
[ -f "$ZIP" ] && cp "$ZIP" "$ZIP.previous"

NODE_DIR=$(dirname "$NODE")
PY=$(ls -d "$EMSDK_ROOT"/python/*/python.exe 2>/dev/null | head -1)
PS1="$LOGDIR/build-web-profiled.ps1"
LOG="$LOGDIR/build-web-profiled.log"
{
	echo "\$env:EMSDK = \"$EMSDK_ROOT\""
	echo "\$env:EMSDK_NODE = \"$NODE\""
	echo "\$env:EMSDK_PYTHON = \"$PY\""
	echo "\$env:PATH = \"$EMSDK_ROOT;$EMSDK_ROOT/upstream/emscripten;$NODE_DIR;\" + \$env:PATH"
	echo "Set-Location \"$REPO\""
	echo "scons platform=web target=template_release threads=$THREADS webgpu=yes use_closure_compiler=no build_profile=$PROFILE"
	echo "exit \$LASTEXITCODE"
} > "$PS1"
echo "  building (log: $LOG)"
if powershell -NoProfile -ExecutionPolicy Bypass -File "$PS1" > "$LOG" 2>&1; then
	pass "build" "($(grep -a 'Time elapsed' "$LOG" | tail -1 | sed 's/.*: //'))"
else
	fail "build" "see $LOG"
	grep -a "error:" "$LOG" | sed 's/.*error:/error:/' | sort -u | head -10 >&2
	exit 1
fi
if [ -f "$ZIP.previous" ]; then
	python "$REPO/misc/webgpu_scripts/profile_helper.py" size "$ZIP.previous" "$ZIP"
fi
# Leave the profile beside the template it was built from. A template with a
# profile next to it is a template that only fits one project, and that is
# something an exporter can check for itself rather than being told.
cp "$PROFILE" "$ZIP.build"

if [ "$SKIP_VERIFY" = 1 ]; then
	exit $FAILED
fi

# ------------------------------------------------------------------ verify --
# A stripped class the project still needs fails when the scene loads, so
# running the export is what actually proves the profile fits it. Anything a
# static scan of the profile could catch, this catches too.
phase "Verify"
rm -rf "$PROJECT/.godot/exported" "$PROJECT/.godot/shader_cache"
rm -rf "$OUTDIR" && mkdir -p "$OUTDIR" || { fail "verify" "cannot create $OUTDIR"; exit 1; }
ELOG="$LOGDIR/export-profiled.log"
if ! "$EDITOR_EXE" --path "$PROJECT" --export-release "$PRESET" "$OUTDIR/index.html" \
		--rendering-method "$METHOD" --rendering-driver vulkan --verbose > "$ELOG" 2>&1; then
	fail "export" "see $ELOG"
	exit 1
fi
[ -f "$OUTDIR/index.pck" ] || { fail "export" "no pck produced; see $ELOG"; exit 1; }
pass "export" "($(grep -aco 'excluding shader' "$ELOG") shader variants excluded)"

if [ -z "$NODE" ] || [ ! -f "$TESTBED/boot-noflag.mjs" ]; then
	fail "boot" "no boot harness at $TESTBED/boot-noflag.mjs"
	exit 1
fi
PNG="$LOGDIR/boot-profiled.png"
( cd "$TESTBED" && "$NODE" boot-noflag.mjs "$OUTDIR" "$PNG" "${BOOT_WAIT_MS:-40000}" ) > "$LOGDIR/boot-profiled.log" 2>&1
BLOG="$PNG.stream.log"
[ -f "$BLOG" ] || { fail "boot" "harness produced no console log"; exit 1; }

# The census is the gate runner's, for the same reason: every excluded shader
# variant still gets one warm-up attempt that reports a null shader, so errors
# are tolerated by signature and never by count.
INERT_RE='Parameter "shader" is null|Condition "p_shader\.is_null\(\)" is true'
PAGEERRS=$(grep -ac '^\[pageerror\]' "$BLOG")
INVALID=$(grep -ac 'Invalid CommandBuffer' "$BLOG")
UNKNOWN=$(grep -a '^\[error\].*ERROR:' "$BLOG" | grep -avcE "$INERT_RE")
if [ "$UNKNOWN" != 0 ] || [ "$PAGEERRS" != 0 ] || [ "$INVALID" != 0 ]; then
	fail "boot" "$UNKNOWN unexpected error(s), $PAGEERRS page error(s), $INVALID invalid command buffer(s)"
	grep -a '^\[error\].*ERROR:' "$BLOG" | grep -avE "$INERT_RE" | head -10 >&2
	echo "  A class the profile stripped but the project still needs reports itself here." >&2
else
	pass "boot" "(clean; screenshot $PNG)"
fi

# ------------------------------------------------------------------ report --
phase "Summary"
for r in "${RESULTS[@]}"; do
	IFS='|' read -r status name detail <<< "$r"
	printf '  %-5s %-12s %s\n' "$status" "$name" "$detail"
done
if [ "$FAILED" = 0 ]; then
	printf '\n  The template in bin/ now fits %s only. Rebuild it whenever the project grows.\n' "$PROJECT_NAME"
fi
exit $FAILED
