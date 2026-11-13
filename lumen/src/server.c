/*
 * Lumen — serveur HTTP natif (Linux / Windows).
 *
 * Un thread par connexion ; l'inférence est sérialisée par g_llm_mutex
 * (un seul contexte llama.cpp partagé).
 */
#include "server.h"
#include "search.h"
#include "llm.h"
#include "session.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include <errno.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define strncasecmp _strnicmp
#endif

#ifndef LUMEN_VERSION
#define LUMEN_VERSION "5.0.0"
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef _WIN32
typedef SOCKET socket_t;
#define INVALID_SOCKET_VALUE INVALID_SOCKET
#define CLOSESOCKET closesocket
#else
typedef int socket_t;
#define INVALID_SOCKET_VALUE (-1)
#define CLOSESOCKET close
#endif

#ifndef _WIN32
static pthread_mutex_t g_llm_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_metrics_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_rate_mutex    = PTHREAD_MUTEX_INITIALIZER;
#define MUTEX_INIT(m)   ((void)0)
#define MUTEX_DESTROY(m) ((void)0)
#define MUTEX_LOCK(m)   pthread_mutex_lock(&(m))
#define MUTEX_UNLOCK(m) pthread_mutex_unlock(&(m))
#define SLEEP_MS(ms)    usleep((ms) * 1000u)
#else
static CRITICAL_SECTION g_llm_mutex, g_metrics_mutex, g_rate_mutex;
#define MUTEX_INIT(m)   InitializeCriticalSection(&(m))
#define MUTEX_DESTROY(m) DeleteCriticalSection(&(m))
#define MUTEX_LOCK(m)   EnterCriticalSection(&(m))
#define MUTEX_UNLOCK(m) LeaveCriticalSection(&(m))
#define SLEEP_MS(ms)    Sleep(ms)
#endif

#define MAX_REQUEST      32768u
#define MAX_BODY         (16u * 1024u)
#define PROMPT_CAP       40000u
#define MAX_RESPONSE     65536u
#define RATE_TABLE_SIZE  512u
#define RATE_WINDOW_SEC  60

typedef struct {
    int max_tokens;
    int max_clients;
    int timeout_sec;
    int rate_limit;
    int trust_proxy;
} Config;

static Config g_cfg;
static volatile sig_atomic_t g_stop = 0;
static socket_t g_listen_socket = INVALID_SOCKET_VALUE;
static LlmContext *g_llm = NULL;
static const char *g_system_prompt =
    "Tu es Lumen, une IA généraliste autonome. Réponds clairement, honnêtement "
    "et utilement, dans la langue de l'utilisateur. N'invente pas les faits.";
static unsigned long g_requests = 0, g_errors = 0, g_generated_tokens = 0, g_active = 0;

static int env_int(const char *name, int def, int lo, int hi) {
    const char *e = getenv(name);
    if (!e || !*e) return def;
    char *end = NULL;
    long v = strtol(e, &end, 10);
    if (*end || v < lo || v > hi) return def;
    return (int)v;
}

static void metric_add(unsigned long *v, unsigned long n) {
    MUTEX_LOCK(g_metrics_mutex); *v += n; MUTEX_UNLOCK(g_metrics_mutex);
}
static void metric_sub(unsigned long *v, unsigned long n) {
    MUTEX_LOCK(g_metrics_mutex); *v -= n; MUTEX_UNLOCK(g_metrics_mutex);
}

/* ------------------------------------------------------------------ */
/* Rate limiting par IP                                                */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t ip; time_t window_start; unsigned count; int used; } RateEntry;
static RateEntry g_rate_table[RATE_TABLE_SIZE];

static int rate_limit_check(uint32_t ip) {
    time_t now = time(NULL);
    int found = -1, reusable = -1, oldest = 0;
    MUTEX_LOCK(g_rate_mutex);
    for (unsigned i = 0; i < RATE_TABLE_SIZE; i++) {
        RateEntry *e = &g_rate_table[i];
        if (e->used && e->ip == ip) { found = (int)i; break; }
        if (reusable < 0 && (!e->used || now - e->window_start >= RATE_WINDOW_SEC)) reusable = (int)i;
        if (e->window_start < g_rate_table[oldest].window_start) oldest = (int)i;
    }
    int slot = found >= 0 ? found : (reusable >= 0 ? reusable : oldest);
    if (found < 0) g_rate_table[slot] = (RateEntry){ .ip = ip, .window_start = now, .count = 0, .used = 1 };
    RateEntry *e = &g_rate_table[slot];
    if (now - e->window_start >= RATE_WINDOW_SEC) { e->window_start = now; e->count = 0; }
    e->count++;
    int ok = e->count <= (unsigned)g_cfg.rate_limit;
    MUTEX_UNLOCK(g_rate_mutex);
    return ok;
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    if (g_listen_socket != INVALID_SOCKET_VALUE) {
        CLOSESOCKET(g_listen_socket);
        g_listen_socket = INVALID_SOCKET_VALUE;
    }
}

/* ------------------------------------------------------------------ */
/* HTTP bas niveau                                                     */
/* ------------------------------------------------------------------ */
static int send_all(socket_t s, const char *buf, size_t len) {
    while (len) {
#ifdef _WIN32
        int n = send(s, buf, (int)(len > 65536u ? 65536u : len), 0);
#else
        ssize_t n = send(s, buf, len, MSG_NOSIGNAL); /* pas de SIGPIPE si le client part */
#endif
        if (n <= 0) return -1;
        buf += n; len -= (size_t)n;
    }
    return 0;
}

static const char *status_text(int s) {
    switch (s) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "Internal Server Error";
    }
}

static void http_response(socket_t c, int status, const char *type, const char *body, size_t len) {
    char h[768];
    int n = snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Authorization, Content-Type\r\n\r\n",
        status, status_text(status), type, len);
    if (n <= 0 || (size_t)n >= sizeof(h)) return;
    if (send_all(c, h, (size_t)n) == 0 && body && len) send_all(c, body, len);
}

static void send_json(socket_t c, int status, cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) { http_response(c, 500, "application/json", "{\"error\":\"memoire\"}", 19); return; }
    http_response(c, status, "application/json; charset=utf-8", s, strlen(s));
    free(s);
}

static void send_error(socket_t c, int status, const char *msg) {
    if (status >= 400) metric_add(&g_errors, 1);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    send_json(c, status, o);
}

static int parse_header(const char *req, const char *name, char *out, size_t cap) {
    const char *p = req;
    size_t nl = strlen(name);
    while ((p = strstr(p, "\r\n"))) {
        p += 2;
        if (p[0] == '\r') break; /* fin des en-têtes */
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ') ++p;
            size_t n = 0;
            while (p[n] && p[n] != '\r' && p[n] != '\n' && n + 1 < cap) ++n;
            memcpy(out, p, n); out[n] = '\0';
            return 1;
        }
    }
    return 0;
}

static int consteq(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b), n = la > lb ? la : lb;
    unsigned char r = (unsigned char)(la != lb);
    for (size_t i = 0; i < n; i++) {
        unsigned char x = i < la ? (unsigned char)a[i] : 0, y = i < lb ? (unsigned char)b[i] : 0;
        r |= (unsigned char)(x ^ y);
    }
    return r == 0;
}

static int authorized(const char *req) {
    const char *expected = getenv("LUMEN_API_KEY");
    if (!expected || !expected[0]) return 1;
    char auth[512];
    if (!parse_header(req, "Authorization", auth, sizeof(auth))) return 0;
    return strncmp(auth, "Bearer ", 7) == 0 && consteq(auth + 7, expected);
}

/* Retourne 0 OK, -1 erreur/déconnexion, -2 trop gros. */
static int read_request(socket_t c, char *buf, size_t cap, size_t *out_len) {
    size_t used = 0, content_length = 0;
    char *end = NULL;
    for (;;) {
        if (used + 1 >= cap) return -2;
#ifdef _WIN32
        int n = recv(c, buf + used, (int)(cap - used - 1), 0);
#else
        ssize_t n = recv(c, buf + used, cap - used - 1, 0);
#endif
        if (n <= 0) return -1;
        used += (size_t)n; buf[used] = '\0';
        if (!end && (end = strstr(buf, "\r\n\r\n"))) {
            char cl[32];
            if (parse_header(buf, "Content-Length", cl, sizeof(cl))) {
                char *ep = NULL;
                unsigned long v = strtoul(cl, &ep, 10);
                if (ep == cl || *ep || v > MAX_BODY) return -2;
                content_length = (size_t)v;
            }
        }
        if (end && used - (size_t)(end + 4 - buf) >= content_length) break;
    }
    *out_len = used;
    return 0;
}

static const char *body_ptr(const char *req) {
    const char *p = strstr(req, "\r\n\r\n");
    return p ? p + 4 : "";
}

static int parse_query_param(const char *path, const char *key, char *out, size_t cap) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *q = strchr(path, '?');
    if (!q) return 0;
    const char *p = q;
    size_t nl = strlen(needle);
    while ((p = strstr(p, needle))) {
        if (p[-1] == '?' || p[-1] == '&') break;
        p += nl;
    }
    if (!p) return 0;
    p += nl;
    size_t w = 0;
    while (*p && *p != '&' && w + 1 < cap) {
        if (*p == '+') out[w++] = ' ';
        else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = { p[1], p[2], 0 };
            out[w++] = (char)strtol(h, NULL, 16);
            p += 2;
        } else out[w++] = *p;
        ++p;
    }
    out[w] = '\0';
    return w > 0;
}

/* IP réelle du client : X-Real-IP / X-Forwarded-For uniquement si la connexion
 * vient du proxy local ET LUMEN_TRUST_PROXY=1 (sinon n'importe qui pourrait
 * usurper son IP pour contourner le rate limiting). */
static uint32_t client_ip(const char *req, uint32_t peer) {
    if (!g_cfg.trust_proxy || peer != htonl(INADDR_LOOPBACK)) return peer;
    char h[128];
    if (!parse_header(req, "X-Real-IP", h, sizeof(h)) && !parse_header(req, "X-Forwarded-For", h, sizeof(h))) return peer;
    char *comma = strchr(h, ','); if (comma) *comma = '\0';
    struct in_addr a;
    return inet_pton(AF_INET, h, &a) == 1 ? a.s_addr : peer;
}

/* ------------------------------------------------------------------ */
/* Construction du prompt (ChatML)                                     */
/* ------------------------------------------------------------------ */
typedef struct { char *b; size_t cap, len; int over; } Buf;

static void buf_puts(Buf *B, const char *s) {
    size_t n = strlen(s);
    if (B->len + n + 1 > B->cap) { B->over = 1; return; }
    memcpy(B->b + B->len, s, n); B->len += n; B->b[B->len] = '\0';
}

/* Retourne 0 si OK, -1 si messages invalides, -2 si trop long. */
static int build_prompt_from_messages(cJSON *msgs, char *out, size_t cap, const char **last_user) {
    Buf B = { out, cap, 0, 0 };
    out[0] = '\0';
    int has_system = 0, has_user = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, msgs) {
        cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role");
        cJSON *ct = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (cJSON_IsString(r) && strcmp(r->valuestring, "system") == 0 && cJSON_IsString(ct)) has_system = 1;
    }
    if (!has_system) {
        buf_puts(&B, "<|im_start|>system\n"); buf_puts(&B, g_system_prompt); buf_puts(&B, "<|im_end|>\n");
    }
    cJSON_ArrayForEach(m, msgs) {
        cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "role");
        cJSON *ct = cJSON_GetObjectItemCaseSensitive(m, "content");
        if (!cJSON_IsString(r) || !cJSON_IsString(ct)) continue;
        const char *role = r->valuestring;
        if (strcmp(role, "system") && strcmp(role, "user") && strcmp(role, "assistant")) continue;
        lumen_sanitize_text(ct->valuestring);
        buf_puts(&B, "<|im_start|>"); buf_puts(&B, role); buf_puts(&B, "\n");
        buf_puts(&B, ct->valuestring); buf_puts(&B, "<|im_end|>\n");
        if (strcmp(role, "user") == 0) { has_user = 1; *last_user = ct->valuestring; }
    }
    buf_puts(&B, "<|im_start|>assistant\n");
    if (B.over) return -2;
    return has_user ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Streaming SSE                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    socket_t c;
    int openai;
    int failed;
    unsigned char pend[8];  /* octets UTF-8 incomplets reportés au token suivant */
    size_t npend;
    char *acc; size_t acc_len;
} StreamCtx;

static size_t utf8_complete_len(const unsigned char *s, size_t n) {
    size_t i = n;
    for (int back = 0; i > 0 && back < 4; back++) {
        i--;
        unsigned char ch = s[i];
        if ((ch & 0xC0) == 0x80) continue;
        size_t need = ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 1;
        return (n - i >= need) ? n : i;
    }
    return n;
}

static int sse_send(StreamCtx *sc, cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return -1;
    size_t n = strlen(s);
    char *frame = malloc(n + 8);
    if (!frame) { free(s); return -1; }
    int len = snprintf(frame, n + 8, "data: %s\n\n", s);
    free(s);
    int rc = len > 0 ? send_all(sc->c, frame, (size_t)len) : -1;
    free(frame);
    return rc;
}

static cJSON *stream_chunk(int openai, const char *text, int final) {
    cJSON *o = cJSON_CreateObject();
    if (!openai) { cJSON_AddStringToObject(o, "token", text ? text : ""); return o; }
    cJSON_AddStringToObject(o, "id", "chatcmpl-lumen");
    cJSON_AddStringToObject(o, "object", "chat.completion.chunk");
    cJSON_AddNumberToObject(o, "created", (double)time(NULL));
    cJSON_AddStringToObject(o, "model", "lumen-local");
    cJSON *choices = cJSON_AddArrayToObject(o, "choices");
    cJSON *ch = cJSON_CreateObject();
    cJSON_AddNumberToObject(ch, "index", 0);
    cJSON *delta = cJSON_AddObjectToObject(ch, "delta");
    if (final) cJSON_AddStringToObject(ch, "finish_reason", "stop");
    else       cJSON_AddStringToObject(delta, "content", text ? text : "");
    cJSON_AddItemToArray(choices, ch);
    return o;
}

static int sse_callback(const char *piece, size_t len, void *userdata) {
    StreamCtx *sc = (StreamCtx *)userdata;
    if (g_stop) return -1;
    if (sc->acc && sc->acc_len + len < MAX_RESPONSE) {
        memcpy(sc->acc + sc->acc_len, piece, len); sc->acc_len += len; sc->acc[sc->acc_len] = '\0';
    }
    /* Un token peut couper un caractère UTF-8 : on n'envoie que des séquences complètes. */
    unsigned char tmp[sizeof(sc->pend) + 520];
    if (len > 512) len = 512;
    memcpy(tmp, sc->pend, sc->npend);
    memcpy(tmp + sc->npend, piece, len);
    size_t total = sc->npend + len;
    size_t ok = utf8_complete_len(tmp, total);
    sc->npend = total - ok;
    memcpy(sc->pend, tmp + ok, sc->npend);
    if (ok == 0) return 0;
    char text[sizeof(tmp) + 1];
    memcpy(text, tmp, ok); text[ok] = '\0';
    if (sse_send(sc, stream_chunk(sc->openai, text, 0)) != 0) { sc->failed = 1; return -1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* /v1/chat et /v1/chat/completions                                    */
/* ------------------------------------------------------------------ */
static void handle_chat(socket_t c, const char *body, int openai) {
    if (!g_llm) { send_error(c, 503, "modele LLM indisponible (LLM_MODEL_PATH)"); return; }
    cJSON *root = cJSON_Parse(body);
    if (!root) { send_error(c, 400, "JSON invalide"); return; }

    int stream = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "stream"));
    int max_tokens = g_cfg.max_tokens;
    cJSON *mt = cJSON_GetObjectItemCaseSensitive(root, "max_tokens");
    if (cJSON_IsNumber(mt) && mt->valuedouble >= 1 && mt->valuedouble < max_tokens) max_tokens = (int)mt->valuedouble;

    char sid[LUMEN_SESSION_ID_LEN] = {0};
    cJSON *sj = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (cJSON_IsString(sj)) snprintf(sid, sizeof(sid), "%s", sj->valuestring);

    char *full = malloc(PROMPT_CAP);
    if (!full) { cJSON_Delete(root); send_error(c, 500, "memoire"); return; }

    const char *last_user = NULL;
    LumenSession *sess = NULL;
    cJSON *msgs = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (cJSON_IsArray(msgs) && cJSON_GetArraySize(msgs) > 0) {
        /* Mode OpenAI : le client fournit tout l'historique, pas de session serveur. */
        int rc = build_prompt_from_messages(msgs, full, PROMPT_CAP, &last_user);
        if (rc != 0) { free(full); cJSON_Delete(root); send_error(c, rc == -2 ? 413 : 400, rc == -2 ? "prompt trop long" : "aucun message user"); return; }
    } else {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "prompt");
        if (!cJSON_IsString(p)) p = cJSON_GetObjectItemCaseSensitive(root, "input");
        if (!cJSON_IsString(p) || !p->valuestring[0]) { free(full); cJSON_Delete(root); send_error(c, 400, "prompt/messages requis"); return; }
        if (strlen(p->valuestring) > 24000u) { free(full); cJSON_Delete(root); send_error(c, 413, "prompt trop long"); return; }
        lumen_sanitize_text(p->valuestring);
        last_user = p->valuestring;
        if (sid[0]) {
            sess = lumen_session_get(sid, 1);
            if (!sess) { free(full); cJSON_Delete(root); send_error(c, 429, "limite de sessions atteinte"); return; }
            lumen_session_build_prompt(sess, g_system_prompt, last_user, full, PROMPT_CAP);
        } else {
            snprintf(full, PROMPT_CAP, "<|im_start|>system\n%s<|im_end|>\n<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",
                     g_system_prompt, last_user);
        }
    }

    if (stream) {
        const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                          "X-Accel-Buffering: no\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
        send_all(c, hdr, strlen(hdr));
        StreamCtx sc = { .c = c, .openai = openai };
        sc.acc = malloc(MAX_RESPONSE);
        if (sc.acc) sc.acc[0] = '\0';
        MUTEX_LOCK(g_llm_mutex);
        int n = llm_generate_stream(g_llm, full, max_tokens, sse_callback, &sc);
        MUTEX_UNLOCK(g_llm_mutex);
        if (n >= 0 && !sc.failed) {
            metric_add(&g_generated_tokens, (unsigned long)n);
            if (openai) sse_send(&sc, stream_chunk(1, NULL, 1));
            send_all(c, "data: [DONE]\n\n", 14);
            if (sess && sc.acc && sc.acc[0]) {
                lumen_session_add(sess, "user", last_user);
                lumen_sanitize_text(sc.acc);
                lumen_session_add(sess, "assistant", sc.acc);
            }
        } else {
            metric_add(&g_errors, 1);
            if (!sc.failed) send_all(c, "data: {\"error\":\"generation echouee\"}\n\n", 37);
        }
        free(sc.acc); free(full); cJSON_Delete(root);
        return;
    }

    char *out = malloc(MAX_RESPONSE / 2);
    if (!out) { free(full); cJSON_Delete(root); send_error(c, 500, "memoire"); return; }
    MUTEX_LOCK(g_llm_mutex);
    int n = llm_generate(g_llm, full, out, MAX_RESPONSE / 2, max_tokens);
    MUTEX_UNLOCK(g_llm_mutex);
    free(full);
    if (n < 0) { free(out); cJSON_Delete(root); send_error(c, 500, "generation echouee"); return; }
    metric_add(&g_generated_tokens, (unsigned long)n);
    if (sess) {
        lumen_session_add(sess, "user", last_user);
        lumen_sanitize_text(out);
        lumen_session_add(sess, "assistant", out);
    }
    cJSON *res = cJSON_CreateObject();
    if (openai) {
        cJSON_AddStringToObject(res, "id", "chatcmpl-lumen");
        cJSON_AddStringToObject(res, "object", "chat.completion");
        cJSON_AddNumberToObject(res, "created", (double)time(NULL));
        cJSON_AddStringToObject(res, "model", "lumen-local");
        cJSON *choices = cJSON_AddArrayToObject(res, "choices");
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddNumberToObject(ch, "index", 0);
        cJSON *msg = cJSON_AddObjectToObject(ch, "message");
        cJSON_AddStringToObject(msg, "role", "assistant");
        cJSON_AddStringToObject(msg, "content", out);
        cJSON_AddStringToObject(ch, "finish_reason", n >= max_tokens ? "length" : "stop");
        cJSON_AddItemToArray(choices, ch);
        cJSON *usage = cJSON_AddObjectToObject(res, "usage");
        cJSON_AddNumberToObject(usage, "completion_tokens", n);
    } else {
        cJSON_AddStringToObject(res, "text", out);
        if (sid[0]) cJSON_AddStringToObject(res, "session_id", sid);
    }
    free(out);
    cJSON_Delete(root);
    send_json(c, 200, res);
}

/* ------------------------------------------------------------------ */
/* Routage                                                             */
/* ------------------------------------------------------------------ */
static void handle_request(socket_t c, char *req, uint32_t peer_ip) {
    metric_add(&g_requests, 1);
    char method[16] = {0}, path[2048] = {0};
    if (sscanf(req, "%15s %2047s", method, path) != 2) { send_error(c, 400, "requete invalide"); return; }

    /* Le préflight CORS n'envoie jamais Authorization : il doit passer avant l'auth. */
    if (strcmp(method, "OPTIONS") == 0) { http_response(c, 204, "text/plain", NULL, 0); return; }

    char route[2048];
    snprintf(route, sizeof(route), "%s", path);
    char *qm = strchr(route, '?'); if (qm) *qm = '\0';
    int is_get = strcmp(method, "GET") == 0, is_post = strcmp(method, "POST") == 0;

    /* /health reste public : sondes de supervision et scripts de déploiement. */
    if (is_get && strcmp(route, "/health") == 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "ok");
        cJSON_AddStringToObject(o, "name", "Lumen");
        cJSON_AddStringToObject(o, "version", LUMEN_VERSION);
        cJSON_AddBoolToObject(o, "llm", g_llm != NULL);
        send_json(c, 200, o);
        return;
    }

    if (!authorized(req)) { send_error(c, 401, "unauthorized"); return; }
    if (!rate_limit_check(client_ip(req, peer_ip))) { send_error(c, 429, "trop de requetes"); return; }

    if (is_get && strcmp(route, "/metrics") == 0) {
        unsigned long r, e, t, a;
        MUTEX_LOCK(g_metrics_mutex); r = g_requests; e = g_errors; t = g_generated_tokens; a = g_active; MUTEX_UNLOCK(g_metrics_mutex);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "requests_total", (double)r);
        cJSON_AddNumberToObject(o, "errors_total", (double)e);
        cJSON_AddNumberToObject(o, "generated_tokens", (double)t);
        cJSON_AddNumberToObject(o, "active_connections", (double)a);
        send_json(c, 200, o);
        return;
    }
    if (is_get && strcmp(route, "/v1/models") == 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "object", "list");
        cJSON *data = cJSON_AddArrayToObject(o, "data");
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "id", "lumen-local");
        cJSON_AddStringToObject(m, "object", "model");
        cJSON_AddStringToObject(m, "owned_by", "lumen");
        cJSON_AddItemToArray(data, m);
        send_json(c, 200, o);
        return;
    }
    if (is_get && strcmp(route, "/v1/search") == 0) {
        char q[1024];
        if (!parse_query_param(path, "q", q, sizeof(q))) { send_error(c, 400, "q requis"); return; }
        const char *key = getenv("BRAVE_API_KEY");
        if (!key || !key[0]) { send_error(c, 501, "recherche web non configuree"); return; }
        SearchResults *sr = malloc(sizeof(*sr));
        if (!sr) { send_error(c, 500, "memoire"); return; }
        if (search_web(key, q, sr) != 0) { free(sr); send_error(c, 502, "recherche echouee"); return; }
        cJSON *o = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(o, "results");
        for (int i = 0; i < sr->count; i++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "title", sr->items[i].title);
            cJSON_AddStringToObject(it, "url", sr->items[i].url);
            cJSON_AddStringToObject(it, "description", sr->items[i].description);
            cJSON_AddItemToArray(arr, it);
        }
        free(sr);
        send_json(c, 200, o);
        return;
    }
    if (is_post && strcmp(route, "/v1/session/reset") == 0) {
        char sid[LUMEN_SESSION_ID_LEN] = {0};
        cJSON *root = cJSON_Parse(body_ptr(req));
        if (root) {
            cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "session_id");
            if (cJSON_IsString(s)) snprintf(sid, sizeof(sid), "%s", s->valuestring);
            cJSON_Delete(root);
        }
        if (!sid[0]) parse_query_param(path, "session_id", sid, sizeof(sid));
        if (!sid[0]) { send_error(c, 400, "session_id requis"); return; }
        lumen_session_reset(sid); /* idempotent : reset d'une session absente = succès */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "reset");
        send_json(c, 200, o);
        return;
    }
    if (is_post && (strcmp(route, "/v1/chat") == 0 || strcmp(route, "/v1/chat/completions") == 0)) {
        handle_chat(c, body_ptr(req), strcmp(route, "/v1/chat/completions") == 0);
        return;
    }
    if (is_get && strcmp(route, "/") == 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", "Lumen");
        cJSON_AddStringToObject(o, "type", "general-purpose-ai");
        cJSON_AddStringToObject(o, "version", LUMEN_VERSION);
        send_json(c, 200, o);
        return;
    }
    if (!is_get && !is_post) { send_error(c, 405, "methode non autorisee"); return; }
    send_error(c, 404, "route introuvable");
}

/* ------------------------------------------------------------------ */
/* Threads clients                                                     */
/* ------------------------------------------------------------------ */
typedef struct { socket_t c; uint32_t ip; } ConnArg;

static void client_worker(ConnArg a) {
    char *req = malloc(MAX_REQUEST);
    if (req) {
        size_t len = 0;
        int rc = read_request(a.c, req, MAX_REQUEST, &len);
        if (rc == -2) send_error(a.c, 413, "requete trop grande");
        else if (rc != 0) send_error(a.c, 400, "requete invalide");
        else handle_request(a.c, req, a.ip);
        free(req);
    }
    CLOSESOCKET(a.c);
    metric_sub(&g_active, 1);
}

#ifndef _WIN32
static void *client_thread(void *arg) { ConnArg a = *(ConnArg *)arg; free(arg); client_worker(a); return NULL; }
#else
static DWORD WINAPI client_thread(LPVOID arg) { ConnArg a = *(ConnArg *)arg; free(arg); client_worker(a); return 0; }
#endif

static void set_socket_timeouts(socket_t c, int sec) {
#ifdef _WIN32
    DWORD tmo = (DWORD)sec * 1000u;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
#else
    struct timeval tmo = { .tv_sec = sec, .tv_usec = 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
#endif
}

int server_run(int port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
#endif
    MUTEX_INIT(g_llm_mutex); MUTEX_INIT(g_metrics_mutex); MUTEX_INIT(g_rate_mutex);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    g_cfg.max_tokens  = env_int("LUMEN_MAX_TOKENS", 1024, 1, 32768);
    g_cfg.max_clients = env_int("LUMEN_MAX_CLIENTS", 128, 1, 4096);
    g_cfg.timeout_sec = env_int("LUMEN_SOCKET_TIMEOUT_SEC", 15, 1, 600);
    g_cfg.rate_limit  = env_int("LUMEN_RATE_LIMIT_PER_MIN", 120, 1, 1000000);
    g_cfg.trust_proxy = env_int("LUMEN_TRUST_PROXY", 0, 0, 1);

    if (search_global_init() != 0) fprintf(stderr, "[server] curl indisponible : /v1/search désactivé\n");
    lumen_sessions_init();

    const char *model = getenv("LLM_MODEL_PATH");
    if (model && model[0]) {
        int ctx = env_int("LUMEN_CONTEXT_SIZE", 8192, 512, 131072);
        int gpu = env_int("LUMEN_GPU_LAYERS", 0, 0, 1000);
        g_llm = llm_load(model, ctx, gpu);
        if (!g_llm) fprintf(stderr, "[server] modèle LLM indisponible : %s\n", model);
    } else {
        fprintf(stderr, "[server] LLM_MODEL_PATH non défini : le chat renverra 503\n");
    }

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    g_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_socket == INVALID_SOCKET_VALUE) return -1;
    int yes = 1;
    setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    const char *bind_addr = getenv("LUMEN_BIND");
    if (!bind_addr || !bind_addr[0]) bind_addr = "127.0.0.1";
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "[server] LUMEN_BIND invalide : %s\n", bind_addr);
        CLOSESOCKET(g_listen_socket); return -1;
    }
    if (bind(g_listen_socket, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(g_listen_socket, 64) != 0) {
        fprintf(stderr, "[server] bind/listen impossible sur %s:%d\n", bind_addr, port);
        CLOSESOCKET(g_listen_socket); return -1;
    }
    const char *key = getenv("LUMEN_API_KEY");
    fprintf(stderr, "Lumen %s — http://%s:%d — auth %s\n", LUMEN_VERSION, bind_addr, port, key && key[0] ? "activée" : "DÉSACTIVÉE (mode local)");
    if ((!key || !key[0]) && strcmp(bind_addr, "127.0.0.1") != 0)
        fprintf(stderr, "[server] ATTENTION : écoute hors loopback sans LUMEN_API_KEY\n");

    while (!g_stop) {
        struct sockaddr_in ca;
#ifdef _WIN32
        int alen = sizeof(ca);
#else
        socklen_t alen = sizeof(ca);
#endif
        socket_t c = accept(g_listen_socket, (struct sockaddr *)&ca, &alen);
        if (c == INVALID_SOCKET_VALUE) { if (g_stop) break; continue; }

        unsigned long active;
        MUTEX_LOCK(g_metrics_mutex); active = g_active; MUTEX_UNLOCK(g_metrics_mutex);
        if (active >= (unsigned long)g_cfg.max_clients) {
            const char *msg = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nRetry-After: 2\r\nConnection: close\r\n\r\n";
            send_all(c, msg, strlen(msg));
            CLOSESOCKET(c);
            continue;
        }
        set_socket_timeouts(c, g_cfg.timeout_sec);
        ConnArg *arg = malloc(sizeof(*arg));
        if (!arg) { CLOSESOCKET(c); continue; }
        arg->c = c; arg->ip = ca.sin_addr.s_addr;
        metric_add(&g_active, 1);
#ifdef _WIN32
        HANDLE th = CreateThread(NULL, 0, client_thread, arg, 0, NULL);
        if (!th) { free(arg); CLOSESOCKET(c); metric_sub(&g_active, 1); } else CloseHandle(th);
#else
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, arg) != 0) { free(arg); CLOSESOCKET(c); metric_sub(&g_active, 1); }
        else pthread_detach(th);
#endif
    }

    if (g_listen_socket != INVALID_SOCKET_VALUE) { CLOSESOCKET(g_listen_socket); g_listen_socket = INVALID_SOCKET_VALUE; }
    /* Laisse 10 s aux requêtes en cours (le streaming s'arrête dès g_stop). */
    for (int i = 0; i < 100; i++) {
        unsigned long a; MUTEX_LOCK(g_metrics_mutex); a = g_active; MUTEX_UNLOCK(g_metrics_mutex);
        if (!a) break;
        SLEEP_MS(100);
    }
    MUTEX_LOCK(g_llm_mutex); llm_free(g_llm); g_llm = NULL; MUTEX_UNLOCK(g_llm_mutex);
    lumen_sessions_clear();
    search_global_cleanup();
    MUTEX_DESTROY(g_llm_mutex); MUTEX_DESTROY(g_metrics_mutex); MUTEX_DESTROY(g_rate_mutex);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
