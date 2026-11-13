#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$ROOT/third_party"
if [[ -f "$ROOT/third_party/llama.cpp/CMakeLists.txt" ]]; then
  echo "llama.cpp already present"
  exit 0
fi
git clone --depth 1 https://github.com/ggml-org/llama.cpp.git "$ROOT/third_party/llama.cpp"
echo "llama.cpp installed in third_party/llama.cpp"
