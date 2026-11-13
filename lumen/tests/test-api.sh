#!/bin/sh
set -eu
BIN="${1:-./build/Lumen}"
PORT="${LUMEN_TEST_PORT:-18080}"
KEY="${LUMEN_API_KEY:-}"
AUTH=""; [ -n "$KEY" ] && AUTH="Authorization: Bearer $KEY"
"$BIN" serve "$PORT" >/tmp/lumen-test.log 2>&1 &
PID=$!
trap 'kill "$PID" 2>/dev/null || true' EXIT INT TERM
sleep 1
health=$(curl -fsS "http://127.0.0.1:${PORT}/health")
case "$health" in *'"name":"Lumen"'*) ;; *) echo "health invalide: $health"; exit 1;; esac
models=$(curl -fsS -H "$AUTH" "http://127.0.0.1:${PORT}/v1/models")
case "$models" in *'lumen-local'*) ;; *) echo "models invalide: $models"; exit 1;; esac
metrics=$(curl -fsS -H "$AUTH" "http://127.0.0.1:${PORT}/metrics")
case "$metrics" in *'requests_total'*) ;; *) echo "metrics invalides: $metrics"; exit 1;; esac
echo "Lumen API smoke test: OK"
