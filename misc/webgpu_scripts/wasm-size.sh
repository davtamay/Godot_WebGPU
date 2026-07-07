#!/usr/bin/env bash
# Record raw + brotli sizes of .wasm files; optionally diff two records.
# Usage: wasm-size.sh <bin_dir> [out.txt]
#        wasm-size.sh --diff <old.txt> <new.txt>
set -euo pipefail

if [[ "${1:-}" == "--diff" ]]; then
  old="$2"; new="$3"
  join -j1 <(sort "$old") <(sort "$new") | while read -r name oraw obr nraw nbr; do
    oraw=${oraw#raw=}; obr=${obr#brotli=}; nraw=${nraw#raw=}; nbr=${nbr#brotli=}
    printf "%-45s raw %+d bytes   brotli %+d bytes\n" \
      "$name" "$((nraw - oraw))" "$((nbr - obr))"
  done
  exit 0
fi

dir="${1:-bin}"; out="${2:-/dev/stdout}"
command -v brotli >/dev/null || { echo "brotli not installed" >&2; exit 2; }
shopt -s nullglob
files=("$dir"/*.wasm)
[[ ${#files[@]} -gt 0 ]] || { echo "no .wasm files in $dir" >&2; exit 2; }
for f in "${files[@]}"; do
  raw=$(wc -c < "$f")
  br=$(brotli -q 5 -c "$f" | wc -c)
  echo "${f##*/} raw=$raw brotli=$br"
done | tee "$out" >/dev/null
[[ "$out" != "/dev/stdout" ]] && cat "$out"
