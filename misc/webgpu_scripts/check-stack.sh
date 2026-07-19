#!/usr/bin/env bash
# Enforce patch-boundary rules on every commit in upstream/master..HEAD.
# Run from repo root (Git Bash / WSL / Linux) before every push.
set -euo pipefail

# Files any patch may add/edit freely.
ALLOWED='^(drivers/webgpu/|platform/web/rendering_context_driver_webgpu|platform/web/js/libs/library_godot_webgpu\.js|misc/webgpu_scripts/|docs/webgpu-|\.github/workflows/(web-baseline|upstream-sync|tint-builder)\.yml|editor/shader/shader_baker/shader_baker_export_plugin_platform_webgpu)'
# Shared upstream files: edits allowed ONLY in commits tagged [shared].
SHARED_OK='^(platform/web/detect\.py|platform/web/SCsub|drivers/SCsub|main/main\.cpp|servers/rendering_server\.cpp|platform/web/js/engine/|platform/web/display_server_web\.(cpp|h)|editor/editor_node\.cpp|editor/export/shader_baker_export_plugin\.(cpp|h)|servers/rendering/renderer_rd/shader_rd\.cpp|editor/shader/shader_baker/SCsub|platform/web/export/export_plugin\.(cpp|h)|servers/rendering/renderer_rd/effects/(tone_mapper|copy_effects)\.cpp|servers/rendering/renderer_rd/forward_mobile/render_forward_mobile\.cpp|servers/rendering/renderer_rd/cluster_builder_rd\.(cpp|h)|servers/rendering/renderer_rd/storage_rd/(particles_storage\.(cpp|h)|texture_storage\.cpp)|modules/webxr/(webxr_interface_js\.(cpp|h)|godot_webxr\.h|native/(library_godot_webxr\.js|webxr\.externs\.js))|servers/xr/xr_interface\.h|servers/rendering/renderer_viewport.cpp|scene/main/viewport.h|scene/main/window.(cpp|h)|editor/inspector/editor_property_name_processor\.cpp)'

fail=0
for c in $(git rev-list --reverse upstream/master..HEAD); do
  subj=$(git log -1 --format=%s "$c")
  while IFS= read -r f; do
    [[ "$f" =~ $ALLOWED ]] && continue
    if [[ "$f" =~ $SHARED_OK ]]; then
      if [[ "$subj" != *"[shared]"* ]]; then
        echo "SHARED file '$f' in commit lacking [shared] tag: $subj"
        fail=1
      fi
    else
      echo "FORBIDDEN file '$f' touched by: $subj"
      fail=1
    fi
  done < <(git diff-tree --no-commit-id --name-only -r "$c")
done

if [[ $fail -eq 0 ]]; then
  echo "check-stack: OK ($(git rev-list --count upstream/master..HEAD) patch(es) clean)"
fi
exit $fail
