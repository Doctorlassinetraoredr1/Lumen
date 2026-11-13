# API Lumen 5.0

Toutes les routes protégées acceptent `Authorization: Bearer <LUMEN_API_KEY>` lorsque `LUMEN_API_KEY` est configurée.

## GET /health

Retourne l'état du moteur. **Public** (pas de clé requise).

## GET /v1/models

Retourne le modèle logique `lumen-local`.

## POST /v1/chat

Entrée minimale :

```json
{"prompt":"Bonjour"}
```

Avec session :

```json
{"session_id":"user-123","prompt":"Bonjour"}
```

Streaming (`data: {"token":"..."}` puis `data: [DONE]`) :

```json
{"prompt":"Raconte une histoire","stream":true,"max_tokens":512}
```

## POST /v1/chat/completions

Compatible avec le format de chat courant :

```json
{
  "model":"lumen-local",
  "messages":[
    {"role":"user","content":"Bonjour Lumen"}
  ],
  "stream":false
}
```

Avec `stream:true`, la réponse suit le format OpenAI (`chat.completion.chunk`, `delta.content`). Tous les messages (`system`, `user`, `assistant`) sont pris en compte ; l'appel est sans état.

## POST /v1/session/reset

```json
{"session_id":"user-123"}
```

## GET /v1/search?q=...

Recherche web via Brave lorsque `BRAVE_API_KEY` est définie.

## GET /metrics

```json
{
  "requests_total": 0,
  "errors_total": 0,
  "generated_tokens": 0,
  "active_connections": 0
}
```
