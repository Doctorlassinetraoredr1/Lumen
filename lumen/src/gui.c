/*
 * gui.c — Interface graphique native Windows (Win32 API, Unicode).
 *
 * - Toutes les API sont en version « W » : les accents et les langues africaines
 *   passent en UTF-8 vers le modèle (la version ANSI les corrompait).
 * - Le modèle est chargé UNE fois (au premier message), pas à chaque clic.
 * - Génération / recherche / RAG tournent dans un thread de travail : la
 *   fenêtre ne se fige plus.
 */

#ifdef _WIN32

#include "gui.h"
#include "llm.h"
#include "search.h"
#include "rag.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define ID_EDIT_INPUT   101
#define ID_EDIT_OUTPUT  102
#define ID_BTN_SEND     103
#define ID_BTN_SEARCH   104
#define ID_BTN_ASK      105
#define ID_BTN_CLEAR    106
#define WM_LUMEN_DONE   (WM_APP + 1)

typedef enum { JOB_CHAT, JOB_SEARCH, JOB_ASK } JobKind;
typedef struct { JobKind kind; char *text; } Job; /* text : UTF-8, appartient au Job */

static HWND  hMainWnd = NULL, hEditInput = NULL, hEditOutput = NULL;
static HWND  hBtn[4];                    /* Envoyer, Recherche, RAG Local, Effacer */
static HFONT hFont = NULL;
static volatile LONG g_busy = 0;
static LlmContext *g_lc = NULL;          /* modèle chargé une seule fois, utilisé par le thread de travail */

/* ============================================================
 * Utilitaires texte (UTF-8 <-> UTF-16)
 * ============================================================ */

static char *dupstr(const char *s)
{
    size_t n = strlen(s);
    char *c = (char *)malloc(n + 1);
    if (c) memcpy(c, s, n + 1);
    return c;
}

static char *wide_to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = (char *)malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) { free(s); return NULL; }
    return s;
}

/* UTF-8 -> UTF-16 en convertissant \n en \r\n (exigé par les contrôles EDIT). */
static wchar_t *utf8_to_wide_crlf(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { free(w); return NULL; }
    wchar_t *o = (wchar_t *)malloc(((size_t)n * 2 + 8) * sizeof(wchar_t));
    if (!o) { free(w); return NULL; }
    size_t j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) o[j++] = L'\r';
        o[j++] = w[i];
    }
    o[j] = L'\0';
    free(w);
    return o;
}

/* Texte non fiable : neutralise les balises de rôle ChatML (<|im_start|>…). */
static void sanitize(char *s)
{
    for (; *s; s++) if (s[0] == '<' && s[1] == '|') s[0] = '(';
}

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    if (!e || !*e) return def;
    char *end = NULL;
    long v = strtol(e, &end, 10);
    return (*end || v < lo || v > hi) ? def : (int)v;
}

/* Petit tampon de texte extensible. */
typedef struct { char *p; size_t len, cap; } SB;

static void sb_printf(SB *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (b->len + (size_t)n + 1 > b->cap) {
            size_t nc = (b->len + (size_t)n + 1) * 2;
            char *np = (char *)realloc(b->p, nc);
            if (!np) { va_end(ap2); return; }
            b->p = np; b->cap = nc;
        }
        vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

/* ============================================================
 * Zone de sortie / saisie
 * ============================================================ */

static void append_output(const char *utf8)
{
    if (!hEditOutput || !utf8) return;
    wchar_t *w = utf8_to_wide_crlf(utf8);
    if (!w) return;
    int len = GetWindowTextLengthW(hEditOutput);
    SendMessageW(hEditOutput, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(hEditOutput, EM_REPLACESEL, FALSE, (LPARAM)w);
    SendMessageW(hEditOutput, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n\r\n");
    SendMessageW(hEditOutput, EM_SCROLLCARET, 0, 0);
    free(w);
}

/* Retourne le texte saisi en UTF-8 (à libérer), ou NULL s'il est vide. */
static char *get_input(void)
{
    if (!hEditInput) return NULL;
    int len = GetWindowTextLengthW(hEditInput);
    if (len <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    GetWindowTextW(hEditInput, w, len + 1);
    char *s = wide_to_utf8(w);
    free(w);
    if (!s) return NULL;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = '\0';
    if (!s[0]) { free(s); return NULL; }
    return s;
}

static void set_busy(int busy)
{
    g_busy = busy ? 1 : 0;
    for (int i = 0; i < 3; i++) EnableWindow(hBtn[i], !busy);
    SetWindowTextW(hMainWnd, busy ? L"Lumen — traitement en cours…" : L"Lumen — Assistant multifonctionnel");
}

/* ============================================================
 * Travaux (exécutés dans le thread de travail)
 * ============================================================ */

static LlmContext *ensure_model(const char **err)
{
    if (g_lc) return g_lc;
    const char *path = getenv("LLM_MODEL_PATH");
    if (!path || !path[0]) { *err = "[Erreur] LLM_MODEL_PATH non définie"; return NULL; }
    g_lc = llm_load(path, env_int("LUMEN_CONTEXT_SIZE", 8192, 512, 131072),
                    env_int("LUMEN_GPU_LAYERS", 0, 0, 1000));
    if (!g_lc) *err = "[Erreur] Impossible de charger le modèle";
    return g_lc;
}

static char *generate_labeled(const char *label, const char *prompt)
{
    const char *err = NULL;
    LlmContext *lc = ensure_model(&err);
    if (!lc) return dupstr(err);
    const size_t cap = 32768;
    char *out = (char *)malloc(cap);
    if (!out) return dupstr("[Erreur] mémoire insuffisante");
    int max_tokens = env_int("LUMEN_MAX_TOKENS", 1024, 1, 32768);
    int n = llm_generate(lc, prompt, out, cap, max_tokens);
    char *res = NULL;
    if (n < 0) {
        res = dupstr("[Erreur] La génération a échoué");
    } else {
        SB b = {0};
        sb_printf(&b, "%s%s", label, out);
        res = b.p ? b.p : dupstr("[Erreur] mémoire insuffisante");
    }
    free(out);
    return res;
}

static char *run_chat(char *text)
{
    sanitize(text);
    SB p = {0};
    sb_printf(&p, "<|im_start|>system\nTu es Lumen, un assistant précis, utile et honnête. "
                  "Réponds dans la langue de l'utilisateur.<|im_end|>\n"
                  "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", text);
    if (!p.p) return dupstr("[Erreur] mémoire insuffisante");
    char *r = generate_labeled("Lumen > ", p.p);
    free(p.p);
    return r;
}

static char *run_search(const char *query)
{
    const char *key = getenv("BRAVE_API_KEY");
    if (!key || !key[0]) return dupstr("[Erreur] BRAVE_API_KEY non définie");
    SearchResults *sr = (SearchResults *)malloc(sizeof(*sr));
    if (!sr) return dupstr("[Erreur] mémoire insuffisante");
    if (search_web(key, query, sr) != 0) { free(sr); return dupstr("[Erreur] Recherche échouée"); }
    SB b = {0};
    sb_printf(&b, "Résultats :\n");
    for (int i = 0; i < sr->count; i++)
        sb_printf(&b, "[%d] %s\n    %s\n    %s\n\n", i + 1, sr->items[i].title, sr->items[i].url, sr->items[i].description);
    free(sr);
    return b.p ? b.p : dupstr("[Erreur] mémoire insuffisante");
}

static char *run_ask(char *question)
{
    const char *idx_path = getenv("RAG_INDEX_PATH");
    if (!idx_path || !idx_path[0]) idx_path = "rag_index.bin";
    /* RagIndex pèse plusieurs Mo : sur le tas, jamais sur la pile. */
    RagIndex *idx = (RagIndex *)malloc(sizeof(*idx));
    if (!idx) return dupstr("[Erreur] mémoire insuffisante pour l'index RAG");
    if (rag_index_load(idx, idx_path) != 0 || idx->count == 0) {
        free(idx);
        return dupstr("[Erreur] Index local introuvable ou vide.\nLancez d'abord : Lumen index <dossier>");
    }
    int indices[RAG_TOP_K];
    float scores[RAG_TOP_K];
    int n = rag_search(idx, question, 4, indices, scores, NULL, NULL);
    if (n == 0) { free(idx); return dupstr("Aucun passage pertinent trouvé dans vos documents."); }
    char *context = (char *)malloc(12288);
    if (!context) { free(idx); return dupstr("[Erreur] mémoire insuffisante"); }
    rag_build_context(idx, indices, n, context, 12288); /* déjà neutralisé contre l'injection ChatML */
    free(idx);

    sanitize(question);
    SB p = {0};
    sb_printf(&p, "<|im_start|>system\nRéponds uniquement à partir des documents fournis. "
                  "Cite le nom du fichier source. N'invente rien.<|im_end|>\n"
                  "<|im_start|>user\nDocuments:\n%s\n\nQuestion: %s<|im_end|>\n<|im_start|>assistant\n",
              context, question);
    free(context);
    if (!p.p) return dupstr("[Erreur] mémoire insuffisante");
    char *r = generate_labeled("Lumen (documents) > ", p.p);
    free(p.p);
    return r;
}

static DWORD WINAPI worker_thread(LPVOID arg)
{
    Job *job = (Job *)arg;
    char *result = NULL;
    switch (job->kind) {
    case JOB_CHAT:   result = run_chat(job->text);   break;
    case JOB_SEARCH: result = run_search(job->text); break;
    case JOB_ASK:    result = run_ask(job->text);    break;
    }
    free(job->text);
    free(job);
    if (!result) result = dupstr("[Erreur] échec inattendu");
    /* Le résultat appartient au thread d'interface dès l'envoi. */
    if (!PostMessageW(hMainWnd, WM_LUMEN_DONE, 0, (LPARAM)result)) free(result);
    return 0;
}

/* ============================================================
 * Actions (thread d'interface)
 * ============================================================ */

static void start_job(JobKind kind, const char *label)
{
    if (g_busy) { append_output("[Info] Une requête est déjà en cours, patientez."); return; }
    char *text = get_input();
    if (!text) return;

    SB line = {0};
    sb_printf(&line, "%s%s", label, text);
    if (line.p) append_output(line.p);
    free(line.p);
    SetWindowTextW(hEditInput, L"");

    Job *job = (Job *)malloc(sizeof(*job));
    if (!job) { free(text); append_output("[Erreur] mémoire insuffisante"); return; }
    job->kind = kind;
    job->text = text;

    set_busy(1);
    HANDLE th = CreateThread(NULL, 0, worker_thread, job, 0, NULL);
    if (!th) {
        free(job->text); free(job);
        set_busy(0);
        append_output("[Erreur] Impossible de lancer le traitement");
        return;
    }
    CloseHandle(th);
}

static void layout(int w, int h)
{
    if (w < 200 || h < 200) return;
    MoveWindow(hEditOutput, 12, 12, w - 24, h - 160, TRUE);
    MoveWindow(hEditInput, 12, h - 140, w - 24, 80, TRUE);
    for (int i = 0; i < 4; i++) MoveWindow(hBtn[i], 12 + i * 120, h - 48, 110, 34, TRUE);
}

/* ============================================================
 * Fenêtre principale
 * ============================================================ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandleW(NULL);
        hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        hEditOutput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            12, 12, 760, 420, hwnd, (HMENU)(INT_PTR)ID_EDIT_OUTPUT, hi, NULL);
        hEditInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            12, 450, 760, 80, hwnd, (HMENU)(INT_PTR)ID_EDIT_INPUT, hi, NULL);
        /* Sans EM_SETLIMIT, un EDIT multiligne refuse tout texte au-delà d'environ 32 Ko. */
        SendMessageW(hEditOutput, EM_SETLIMIT, 0, 0);
        SendMessageW(hEditInput,  EM_SETLIMIT, 0, 0);

        static const wchar_t *labels[4] = { L"Envoyer", L"Recherche", L"RAG Local", L"Effacer" };
        static const int ids[4] = { ID_BTN_SEND, ID_BTN_SEARCH, ID_BTN_ASK, ID_BTN_CLEAR };
        for (int i = 0; i < 4; i++) {
            hBtn[i] = CreateWindowExW(0, L"BUTTON", labels[i],
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == 0 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                12 + i * 120, 535, 110, 34, hwnd, (HMENU)(INT_PTR)ids[i], hi, NULL);
        }
        if (hFont) {
            SendMessageW(hEditOutput, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(hEditInput,  WM_SETFONT, (WPARAM)hFont, TRUE);
            for (int i = 0; i < 4; i++) SendMessageW(hBtn[i], WM_SETFONT, (WPARAM)hFont, TRUE);
        }
        append_output("Lumen — interface native Windows\n"
                      "Entrez votre message puis cliquez sur Envoyer (ou Ctrl+Entrée).\n"
                      "Recherche = web (BRAVE_API_KEY) · RAG Local = vos documents indexés.\n"
                      "Le modèle se charge au premier message.");
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_SEND:   start_job(JOB_CHAT,   "Vous > ");            break;
        case ID_BTN_SEARCH: start_job(JOB_SEARCH, "Recherche > ");       break;
        case ID_BTN_ASK:    start_job(JOB_ASK,    "Question locale > "); break;
        case ID_BTN_CLEAR:  SetWindowTextW(hEditOutput, L"");            break;
        }
        break;

    case WM_LUMEN_DONE: {
        char *res = (char *)lParam;
        if (res) { append_output(res); free(res); }
        set_busy(0);
        SetFocus(hEditInput);
        break;
    }

    case WM_SIZE:
        if (hEditOutput && hEditInput) layout(LOWORD(lParam), HIWORD(lParam));
        break;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = 640;
        mmi->ptMinTrackSize.y = 500;
        break;
    }

    case WM_DESTROY:
        if (hFont) DeleteObject(hFont);
        /* Si un thread de travail tourne encore, il se termine avec le processus ;
         * sinon on libère proprement le modèle. */
        if (!g_busy && g_lc) { llm_free(g_lc); g_lc = NULL; }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int gui_run(void)
{
    HINSTANCE hInstance = GetModuleHandleW(NULL);
    search_global_init(); /* une seule fois pour toute la session (curl) */

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"LumenMainWindow";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Échec RegisterClassEx", L"Lumen", MB_ICONERROR);
        return -1;
    }

    hMainWnd = CreateWindowExW(0, L"LumenMainWindow", L"Lumen — Assistant multifonctionnel",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 640,
                               NULL, NULL, hInstance, NULL);
    if (!hMainWnd) {
        MessageBoxW(NULL, L"Échec CreateWindow", L"Lumen", MB_ICONERROR);
        return -1;
    }

    ShowWindow(hMainWnd, SW_SHOW);
    UpdateWindow(hMainWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* Ctrl+Entrée dans la zone de saisie = Envoyer (Entrée seule insère un saut de ligne). */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            msg.hwnd == hEditInput && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hMainWnd, WM_COMMAND, MAKEWPARAM(ID_BTN_SEND, BN_CLICKED), 0);
            continue;
        }
        if (!IsDialogMessageW(hMainWnd, &msg)) { /* Tab / navigation clavier entre les contrôles */
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    search_global_cleanup();
    return 0;
}

#else /* !_WIN32 */

#include "gui.h"
#include <stdio.h>

int gui_run(void)
{
    fprintf(stderr, "L'interface graphique Win32 n'est disponible que sous Windows.\n");
    fprintf(stderr, "Sous Linux, utilisez le mode interactif ou le serveur HTTP.\n");
    return -1;
}

#endif /* _WIN32 */
