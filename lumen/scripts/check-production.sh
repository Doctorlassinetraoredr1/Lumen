#!/usr/bin/env bash
set -euo pipefail
BASE_URL=${BASE_URL:-http://127.0.0.1:8080}
KEY=${LUMEN_API_KEY:-}

printf 'Health: '
curl -fsS "$BASE_URL/health"; echo
printf 'Models: '
if [[ -n "$KEY" ]]; then curl -fsS -H "Authorization: Bearer $KEY" "$BASE_URL/v1/models"; else curl -fsS "$BASE_URL/v1/models"; fi
echo
printf 'Metrics: '
if [[ -n "$KEY" ]]; then curl -fsS -H "Authorization: Bearer $KEY" "$BASE_URL/metrics"; else curl -fsS "$BASE_URL/metrics"; fi; echo

if [[ -n "$KEY" ]]; then
  printf 'Sans clé (doit être 401): '
  code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE_URL/v1/models"); echo "$code"
  [[ "$code" == "401" ]] || { echo "ERREUR : l'API est accessible sans clé"; exit 1; }
fi
printf 'Chat: '
AUTH=(); [[ -n "$KEY" ]] && AUTH=(-H "Authorization: Bearer $KEY")
curl -fsS --max-time 300 "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"model":"lumen-local","max_tokens":24,"messages":[{"role":"user","content":"Dis bonjour en une phrase."}]}' \
  "$BASE_URL/v1/chat/completions"; echo
echo "Production check: OK"
