#!/usr/bin/env bash
set -euo pipefail
# Backward-compatible entry point for native Linux deployment.
exec "$(dirname "$0")/bootstrap-ubuntu.sh" "$@"
