#!/usr/bin/env bash
# Build Godot web export templates. Patch 0: stock upstream build only.
# NOTE: no WebGPU build flag exists yet; add it here in the patch that
# introduces it, not before.
set -euo pipefail
TARGET="${1:-template_debug}"
case "$TARGET" in template_debug) DEV="dev_build=yes";; template_release) DEV="dev_build=no";;
  *) echo "usage: $0 [template_debug|template_release]" >&2; exit 2;; esac

scons platform=web target="$TARGET" threads=yes "$DEV" cache_path="$HOME/.scons_cache"
ls -la bin/*.wasm
