#!/usr/bin/env bash
# Enforce patch-boundary rules on every commit in upstream/master..HEAD.
# Run from repo root (Git Bash / WSL / Linux) before every push.
set -euo pipefail

# Files any patch may add/edit freely.
ALLOWED='^(drivers/webgpu/|platform/web/rendering_context_driver_webgpu|platform/web/js/libs/library_godot_webgpu\.js|modules/webxr/native/library_godot_webxr_ext\.js|misc/webgpu_scripts/|docs/webgpu-|\.github/workflows/(web-baseline|upstream-sync|tint-builder)\.yml|editor/shader/shader_baker/shader_baker_export_plugin_platform_webgpu)'
# Shared upstream files: edits allowed ONLY in commits tagged [shared].
SHARED_OK='^(platform/web/detect\.py|platform/web/SCsub|drivers/SCsub|main/main\.cpp|servers/rendering_server\.cpp|platform/web/js/engine/|platform/web/display_server_web\.(cpp|h)|editor/editor_node\.cpp|editor/export/shader_baker/shader_baker_export_plugin\.(cpp|h)|servers/rendering/renderer_rd/shader_rd\.cpp|editor/shader/shader_baker/SCsub|platform/web/export/export_plugin\.(cpp|h)|servers/rendering/renderer_rd/environment/gi\.(cpp|h)|servers/rendering/renderer_rd/shaders/environment/(sdfgi_preprocess|sdfgi_integrate|sdfgi_direct_light)\.glsl|servers/rendering/renderer_rd/effects/(tone_mapper|copy_effects)\.cpp|servers/rendering/renderer_rd/forward_mobile/render_forward_mobile\.(cpp|h)|servers/rendering/renderer_rd/cluster_builder_rd\.(cpp|h)|servers/rendering/renderer_rd/forward_clustered/(render|scene_shader)_forward_clustered\.(cpp|h)|servers/rendering/renderer_rd/shaders/forward_clustered/(best_fit_normal|integrate_dfg|scene_forward_clustered)\.glsl|servers/rendering/renderer_rd/shaders/environment/volumetric_fog_process\.glsl|servers/rendering/renderer_rd/renderer_scene_render_rd\.(cpp|h)|servers/rendering/rendering_device_commons\.h|drivers/vulkan/rendering_device_driver_vulkan\.cpp|drivers/d3d12/rendering_device_driver_d3d12\.cpp|drivers/metal/rendering_device_driver_metal\.cpp|servers/rendering/renderer_rd/storage_rd/(render_scene_buffers_rd\.(cpp|h)|particles_storage\.(cpp|h)|texture_storage\.cpp)|modules/webxr/(SCsub|webxr_interface(_js)?\.(cpp|h)|godot_webxr\.h|doc_classes/WebXRInterface\.xml|native/(library_godot_webxr\.js|webxr\.externs\.js))|servers/xr/xr_interface\.h|servers/rendering/renderer_viewport.cpp|scene/main/viewport.h|scene/main/window.(cpp|h)|editor/inspector/editor_property_name_processor\.cpp|scene/3d/trail_3d\.(cpp|h))'

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
