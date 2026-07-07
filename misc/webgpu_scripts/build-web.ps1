#!/usr/bin/env pwsh
# Build Godot web export templates. Patch 0: stock upstream build only.
# NOTE: no WebGPU build flag exists yet; when a later patch introduces one,
# add a parameter here in the same patch.
param(
    [ValidateSet("template_release", "template_debug")]
    [string]$Target = "template_debug",
    [switch]$Release
)
if ($Release) { $Target = "template_release" }
$ErrorActionPreference = "Stop"

$cache = Join-Path $env:USERPROFILE ".scons_cache"
$dev = if ($Target -eq "template_debug") { "dev_build=yes" } else { "dev_build=no" }

& scons platform=web target=$Target threads=yes $dev "cache_path=$cache"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Get-ChildItem bin\*.wasm | ForEach-Object {
    "{0}  {1:N0} bytes" -f $_.Name, $_.Length
}
