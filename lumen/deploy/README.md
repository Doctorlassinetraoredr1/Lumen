# Déploiement production Lumen

## 1. Préparer un serveur Linux

Ubuntu/Debian recommandé, avec accès root/sudo et une IP publique.

## 2. Installer Lumen

Depuis le dossier du projet :

```bash
sudo ./scripts/bootstrap-ubuntu.sh
```

Le script :
- installe les dépendances natives ;
- récupère `llama.cpp` depuis son dépôt officiel si absent ;
- compile Lumen en Release avec durcissement ;
- crée l'utilisateur système `lumen` ;
- installe le service systemd ;
- génère automatiquement une API key si `.env` n'existe pas ;
- démarre Lumen ;
- vérifie `/health`.

Aucune virtualisation n'est utilisée.

## 3. Ajouter le modèle GGUF

Place ton modèle dans :

```text
/opt/lumen/models/model.gguf
```

ou configure `LLM_MODEL_PATH` dans `/opt/lumen/.env`.

Tu peux aussi fournir `MODEL_URL` au bootstrap pour télécharger un modèle depuis une URL que tu contrôles :

```bash
sudo MODEL_URL='https://exemple/model.gguf' MODEL_FILE='model.gguf' ./scripts/bootstrap-ubuntu.sh
```

## 4. Vérifier

```bash
sudo systemctl status lumen
sudo journalctl -u lumen -f
curl http://127.0.0.1:8080/health
```

## 5. HTTPS et domaine

> **Ne transmettez jamais la clé API en HTTP simple** : activez TLS avant d'ouvrir le service à des clients.

Lumen écoute volontairement sur `127.0.0.1`. Nginx sert de reverse proxy public.

Copie `deploy/nginx/lumen.conf.example` vers `/etc/nginx/sites-available/lumen`, remplace le domaine, puis :

```bash
sudo ln -s /etc/nginx/sites-available/lumen /etc/nginx/sites-enabled/lumen
sudo nginx -t
sudo systemctl reload nginx
```

Ensuite :

```bash
sudo certbot --nginx -d lumen.example.com   # certificat + redirection HTTPS
sudo ufw allow OpenSSH && sudo ufw allow 80,443/tcp && sudo ufw enable
```

Le port 8080 reste fermé au public (Lumen écoute sur 127.0.0.1).

## 6. API

```http
POST /v1/chat/completions
Authorization: Bearer <LUMEN_API_KEY>
Content-Type: application/json

{"model":"lumen-local","messages":[{"role":"user","content":"Bonjour Lumen"}]}
```

Streaming : ajoute `"stream":true`.

## 7. Vérification production

```bash
LUMEN_API_KEY='ta-cle' ./scripts/check-production.sh
```

## Important

Le modèle GGUF n'est pas inclus dans le dépôt : sa taille peut être de plusieurs Go et sa licence doit être respectée séparément.
