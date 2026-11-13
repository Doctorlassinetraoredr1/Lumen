#!/usr/bin/env bash
set -euo pipefail

# Native Linux deployment. No Docker, VM, or container is used.
PREFIX=${PREFIX:-/opt/lumen}
MODEL_URL=${MODEL_URL:-}
MODEL_FILE=${MODEL_FILE:-model.gguf}
LUMEN_REPO_DIR=${LUMEN_REPO_DIR:-$(cd "$(dirname "$0")/.." && pwd)}

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root: sudo $0"
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y build-essential cmake git pkg-config libcjson-dev libcurl4-openssl-dev nginx openssl ca-certificates

if ! id lumen >/dev/null 2>&1; then
  useradd --system --home "$PREFIX" --shell /usr/sbin/nologin lumen
fi

mkdir -p "$PREFIX"/{bin,models,data,logs}
chown -R lumen:lumen "$PREFIX"

# Fetch the official llama.cpp source only when it is not already bundled.
if [[ ! -f "$LUMEN_REPO_DIR/third_party/llama.cpp/CMakeLists.txt" ]]; then
  mkdir -p "$LUMEN_REPO_DIR/third_party"
  git clone --depth 1 https://github.com/ggml-org/llama.cpp.git "$LUMEN_REPO_DIR/third_party/llama.cpp"
fi

cmake -S "$LUMEN_REPO_DIR" -B "$LUMEN_REPO_DIR/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLUMEN_USE_LLAMA=ON \
  -DLUMEN_HARDENING=ON \
  -DLUMEN_WERROR=OFF
cmake --build "$LUMEN_REPO_DIR/build" --parallel "$(nproc)"
install -m 0755 "$LUMEN_REPO_DIR/build/Lumen" "$PREFIX/bin/Lumen"

if [[ ! -f "$PREFIX/.env" ]]; then
  install -m 0600 "$LUMEN_REPO_DIR/.env.example" "$PREFIX/.env"
  KEY=$(openssl rand -hex 32)
  sed -i "s#^LUMEN_API_KEY=.*#LUMEN_API_KEY=$KEY#" "$PREFIX/.env"
  sed -i "s#^LUMEN_TRUST_PROXY=.*#LUMEN_TRUST_PROXY=1#" "$PREFIX/.env"   # derrière nginx : IP réelle via X-Real-IP
fi

if [[ -n "$MODEL_URL" && ! -f "$PREFIX/models/$MODEL_FILE" ]]; then
  curl -fL --retry 3 --output "$PREFIX/models/$MODEL_FILE" "$MODEL_URL"
fi

MODEL_PATH=$(grep -E '^LLM_MODEL_PATH=' "$PREFIX/.env" | tail -1 | cut -d= -f2- || true)
if [[ -z "$MODEL_PATH" ]]; then
  MODEL_PATH="$PREFIX/models/model.gguf"
fi
if [[ ! -f "$MODEL_PATH" ]]; then
  echo "Model GGUF introuvable: $MODEL_PATH"
  echo "Place un modèle GGUF à cet emplacement ou relance avec MODEL_URL=..."
  exit 2
fi

chown -R lumen:lumen "$PREFIX"
chown -R root:root "$PREFIX/bin"   # le service ne peut pas réécrire son propre exécutable
install -m 0644 "$LUMEN_REPO_DIR/deploy/lumen.service" /etc/systemd/system/lumen.service
systemctl daemon-reload
systemctl enable lumen
systemctl restart lumen

# Le serveur charge le modèle AVANT d'écouter : on attend jusqu'à 3 min (gros GGUF = plusieurs dizaines de secondes).
echo "Attente du démarrage de Lumen (chargement du modèle)…"
ok=0
for i in $(seq 1 90); do
  if curl -fsS http://127.0.0.1:8080/health >/dev/null 2>&1; then ok=1; break; fi
  if ! systemctl is-active --quiet lumen; then break; fi
  sleep 2
done
if [[ "$ok" -ne 1 ]]; then
  echo "Lumen n'a pas répondu au health check. Inspecter : journalctl -u lumen -n 100 --no-pager"
  exit 1
fi
curl -fsS http://127.0.0.1:8080/health; echo

echo "Lumen is running locally on http://127.0.0.1:8080"
echo "Next: configure deploy/nginx/lumen.conf.example with your domain and TLS."
