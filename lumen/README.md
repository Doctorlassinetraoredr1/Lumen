# Lumen 5.0 — IA généraliste native

Lumen est une **IA généraliste indépendante**. BAARO peut utiliser son API, mais Lumen n'est pas limité à BAARO.

## Principes

- cœur applicatif en C11
- llama.cpp natif pour l'inférence
- modèles GGUF locaux
- CPU/GPU directement sur la machine
- Linux et Windows
- serveur HTTP natif
- connexions HTTP concurrentes
- file d'attente naturelle autour du moteur LLM
- streaming SSE token par token
- sessions isolées et protégées par mutex
- API `/v1/chat` et `/v1/chat/completions`
- endpoint `/v1/models`
- métriques `/metrics`
- recherche web optionnelle
- RAG local
- authentification Bearer API key
- service systemd natif
- **aucun Docker, aucune VM, aucun conteneur, aucune virtualisation**
- logo Lumen conservé dans `assets_lumen_logo.svg`

## Architecture

```text
Client
  |
  v
HTTP natif Lumen
  |
  +---- sessions / mémoire
  |
  +---- outils / recherche / RAG
  |
  +---- file de requêtes concurrentes
  |
  v
Verrou d'inférence LLM
  |
  v
llama.cpp -> GGUF -> CPU/GPU
```

Les connexions réseau sont traitées par des threads natifs. L'inférence est protégée par un verrou car la version Lumen actuelle utilise un contexte LLM partagé et doit éviter toute corruption ou fuite de KV cache. Pour obtenir de vraies générations simultanées et du continuous batching, il faut ensuite passer à plusieurs slots/contextes LLM, comme le serveur llama.cpp actuel avec `--parallel` et `--cont-batching`. citeturn0search0

## Notes de sécurité

- `/health` est public ; toutes les autres routes exigent la clé API si `LUMEN_API_KEY` est définie.
- Derrière nginx, activez `LUMEN_TRUST_PROXY=1`, sinon toutes les requêtes partagent l'IP 127.0.0.1 et la même limite.
- Les balises `<|...|>` sont neutralisées dans les entrées utilisateur (anti-injection de rôles ChatML).
- `/v1/chat/completions` est sans état : envoyez tout l'historique dans `messages`. `/v1/chat` + `session_id` garde l'historique côté serveur (sessions inactives recyclées après 5 min quand la table est pleine).

## Streaming

`POST /v1/chat` ou `/v1/chat/completions` accepte :

```json
{"prompt":"Bonjour Lumen","stream":true}
```

La réponse utilise SSE :

```text
data: {"token":"Bonjour"}

data: {"token":" !"}

data: [DONE]
```

Le callback est branché directement sur la génération token par token. Le serveur llama.cpp actuel suit également cette approche de streaming et expose des paramètres SSE. citeturn0search0

## API

### Santé

```http
GET /health
```

### Modèles

```http
GET /v1/models
```

### Chat Lumen

```http
POST /v1/chat
Content-Type: application/json
Authorization: Bearer <clé>

{"session_id":"abc","prompt":"Explique-moi le Mali."}
```

### Chat compatible

```http
POST /v1/chat/completions
Content-Type: application/json
Authorization: Bearer <clé>

{"model":"lumen-local","messages":[{"role":"user","content":"Bonjour"}]}
```

### Streaming

Ajoutez `"stream":true`.

### Session

```http
POST /v1/session/reset
{"session_id":"abc"}
```

### Recherche web

```http
GET /v1/search?q=requête
```

Nécessite `BRAVE_API_KEY`.

### Monitoring

```http
GET /metrics
```

Retourne le nombre de requêtes, erreurs, tokens générés et connexions actives.

## RAG

Le RAG local est **hybride** :

- **BM25** lexical (stopwords multilingues Afrique : FR/EN/PT/AR + swahili, haoussa, yoruba, igbo, zoulou, xhosa, afrikaans, somali, wolof, bambara, lingala… ; hash O(1) ; normalisation d'accents ; IDF réel)
- **Similarité cosinus** lorsque des embeddings sont présents
- Score final : `α · cosinus + (1−α) · BM25` (`RAG_HYBRID_ALPHA = 0.65`)
- Indexation **récursive**, découpage intelligent (paragraphes → phrases → espaces)
- Déduplication basique des chunks quasi-identiques
- Capacité : 1024 chunks, top-k = 6, seuil `RAG_MIN_SCORE`
- Format d'index `LUMRAG04` (rétro-compatible lecture `LUMRAG02/03`)
- **Perf vectorielle** : pré-normalisation L2 → cosinus = dot product ; SIMD SSE2/AVX/FMA auto ; boucles déroulées

Détails de robustesse :

- les accents sont normalisés **des deux côtés** (requête et documents) : « cafe » retrouve « café », « coeur » retrouve « cœur » ;
- les chunks ne sont jamais coupés au milieu d'un caractère UTF-8 ;
- les liens symboliques sont ignorés à l'indexation (pas de boucle infinie), les extensions sont insensibles à la casse (`.TXT`) ;
- la source d'un chunk est le chemin complet du fichier ;
- sauvegarde atomique de l'index (`.tmp` puis renommage) ; un index illisible est rechargé comme index vide ;
- avertissement explicite quand `RAG_MAX_CHUNKS` est atteint (les documents suivants sont ignorés) ;
- le contexte injecté dans le prompt neutralise les balises `<|...|>` (un document ne peut pas détourner les rôles du modèle).

Hook d'embeddings : passer un `RagEmbedFn` à `rag_search` / `rag_index_compute_embeddings` pour brancher un modèle d'embeddings réel (llama.cpp embeddings, etc.). Sans callback, seule la recherche BM25 est utilisée — jamais de faux vecteurs.


## Configuration

```text
LLM_MODEL_PATH=/opt/lumen/models/model.gguf
LUMEN_CONTEXT_SIZE=8192
LUMEN_GPU_LAYERS=0
LUMEN_BIND=127.0.0.1
LUMEN_API_KEY=une-cle-longue-et-aleatoire
BRAVE_API_KEY=
LUMEN_RATE_LIMIT_PER_MIN=120
LUMEN_MAX_TOKENS=1024
LUMEN_TEMPERATURE=0.7      # 0 = glouton
LUMEN_TRUST_PROXY=0        # 1 derrière nginx : rate limit par IP réelle (X-Real-IP)
```

## Interface Windows

`Lumen gui` ouvre une fenêtre native (Win32, Unicode). Le modèle est chargé une seule fois, au premier message, et la génération / la recherche / le RAG tournent dans un thread séparé : la fenêtre reste réactive. `Ctrl+Entrée` envoie le message.

## Compilation native

```bash
cmake -S . -B build -DLUMEN_USE_LLAMA=ON
cmake --build build -j
```

`third_party/llama.cpp` doit contenir une version compatible de llama.cpp. L'option `LUMEN_USE_LLAMA=OFF` compile un binaire de diagnostic sans moteur LLM.

## Installation Linux native

```bash
./scripts/install-linux.sh
```

Puis le service `deploy/lumen.service` peut lancer directement Lumen avec systemd.

## État v5

- API concurrente : **implémentée**
- sessions thread-safe : **implémentées**
- streaming token : **implémenté** (réponse assistant désormais bien enregistrée en session)
- métriques : **implémentées**
- endpoint compatible `/v1/chat/completions` : **implémenté**
- modèle persistant : **implémenté**
- comparaison de clé API en temps constant : **implémentée**
- limite de connexions concurrentes + timeout socket (anti slowloris) : **implémentées**
- rate limiting par IP (`LUMEN_RATE_LIMIT_PER_MIN`) : **implémenté**
- slots LLM / continuous batching réel : **prochaine étape**
- RAG BM25 + hybride + index récursif : **implémenté**
- embeddings sémantiques réels (via `RagEmbedFn`) : **hook prêt**, brancher un modèle dédié
- reranking : **prochaine étape**
- multimodal : dépend du modèle/projecteur utilisé
