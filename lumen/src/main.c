/*
 * Lumen — Assistant multifonctionnel avancé en C pur
 * Version 4.0 native production
 *
 * Capacités :
 *   - Mode interactif avec mémoire de conversation
 *   - Boucle agentique (recherche → raisonnement → réponse)
 *   - System prompts de haute qualité multilingues
 *   - RAG web + synthèse
 *   - Génération d’images
 *   - 100 % natif Windows / Linux, zéro virtualisation
 *
 * Commandes :
 *   Lumen                     → mode interactif
 *   Lumen interact
 *   Lumen search  "requête"
 *   Lumen chat    "prompt"
 *   Lumen rag     "question"          (RAG web)
 *   Lumen index   <dossier>           (indexe documents locaux)
 *   Lumen ask     "question"          (RAG local sur documents indexés)
 *   Lumen image   "description" out.png
 *   Lumen agent   "objectif complexe"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "search.h"
#include "llm.h"
#include "image.h"
#include "rag.h"
#include "server.h"
#include "gui.h"

/* ============================================================
 * Constantes & structures de mémoire
 * ============================================================ */

#define MAX_HISTORY_TURNS   12
#define MAX_MSG_LEN         4096
#define MAX_PROMPT_LEN      32768
#define MAX_CONTEXT_LEN     12288

typedef struct {
    char role[16];          /* "user" ou "assistant" */
    char content[MAX_MSG_LEN];
} Message;

typedef struct {
    Message turns[MAX_HISTORY_TURNS];
    int count;
} ConversationMemory;

static void appendf(char *out, size_t out_size, size_t *used, const char *fmt, ...)
{
    if (!out || !used || *used >= out_size) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *used, out_size - *used, fmt, ap);
    va_end(ap);
    if (n < 0) { *used = out_size; return; }
    if ((size_t)n >= out_size - *used) *used = out_size;
    else *used += (size_t)n;
}

/* ============================================================
 * System prompts de haute qualité
 * ============================================================ */

static const char *SYSTEM_PROMPT_FR =
    "Tu es Lumen, une IA généraliste autonome, compétente, précise et utile.\n"
    "Tu réponds dans la langue de l'utilisateur (français, anglais, arabe, portugais, "
    "swahili, haoussa, yoruba, igbo, zoulou, xhosa, afrikaans, somali, wolof, bambara, "
    "lingala, amharique, ou toute autre langue africaine ou mondiale).\n"
    "Tu es honnête : si tu ne sais pas, tu le dis clairement.\n"
    "Tu cites tes sources quand tu utilises des informations externes.\n"
    "Tu es concis quand c'est possible, détaillé quand c'est nécessaire.\n"
    "Tu adaptes ton ton au contexte (professionnel, pédagogique, créatif...).\n"
    "Tu n'inventes jamais de faits. Tu préfères dire « je n'ai pas cette information ».\n"
    "Tu respectes les cultures et contextes africains lorsque c'est pertinent.\n";

static const char *SYSTEM_PROMPT_AGENT =
    "Tu es un agent intelligent capable d'utiliser des outils.\n"
    "Quand tu as besoin d'informations récentes ou factuelles, tu peux demander une recherche.\n"
    "Format de réponse quand tu veux une recherche :\n"
    "  RECHERCHE: <ta requête précise>\n"
    "Sinon, réponds directement à l'utilisateur de façon claire et utile.\n"
    "Après avoir reçu des résultats de recherche, synthétise-les et réponds.\n";

/* ============================================================
 * Utilitaires
 * ============================================================ */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "\n  Lumen — IA généraliste autonome (C pur)\n\n"
        "  Usage : %s [commande] [arguments]\n\n"
        "  Commandes :\n"
        "    (aucune) ou interact          Mode conversation interactive\n"
        "    search  \"requête\"             Recherche web pure\n"
        "    chat    \"prompt\"              Génération LLM simple\n"
        "    rag     \"question\"            RAG web (recherche + synthèse)\n"
        "    index   <dossier>             Indexe les documents locaux (.txt/.md)\n"
        "    ask     \"question\"            RAG local sur documents indexés\n"
        "    agent   \"objectif\"            Boucle agentique multi-étapes\n"
        "    image   \"description\" out.png Génération d'image\n"
        "    serve   [port]                Serveur HTTP natif (défaut 8080)\n"
        "    gui                          Interface graphique native Windows (x64)\n\n"
        "  Variables d'environnement :\n"
        "    BRAVE_API_KEY, LLM_MODEL_PATH, SD_BINARY_PATH, SD_MODEL_PATH\n"
        "    RAG_INDEX_PATH (défaut : rag_index.bin)\n\n",
        prog);
}

static void trim(char *s)
{
    if (!s) return;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void memory_add(ConversationMemory *mem, const char *role, const char *content)
{
    if (mem->count >= MAX_HISTORY_TURNS) {
        /* Décale tout d'un cran (oublie le plus ancien) */
        memmove(&mem->turns[0], &mem->turns[1],
                (MAX_HISTORY_TURNS - 1) * sizeof(Message));
        mem->count = MAX_HISTORY_TURNS - 1;
    }

    Message *m = &mem->turns[mem->count];
    strncpy(m->role, role, sizeof(m->role) - 1);
    m->role[sizeof(m->role) - 1] = '\0';

    strncpy(m->content, content, sizeof(m->content) - 1);
    m->content[sizeof(m->content) - 1] = '\0';

    mem->count++;
}

static void memory_build_prompt(const ConversationMemory *mem,
                                const char *system,
                                const char *extra_context,
                                char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    size_t used = 0;

    appendf(out, out_size, &used, "<|im_start|>system\n%s\n",
            system ? system : SYSTEM_PROMPT_FR);
    if (extra_context && extra_context[0])
        appendf(out, out_size, &used, "\nContexte supplémentaire :\n%s\n", extra_context);
    appendf(out, out_size, &used, "<|im_end|>\n");

    for (int i = 0; i < mem->count && used < out_size; i++) {
        appendf(out, out_size, &used, "<|im_start|>%s\n%s<|im_end|>\n",
                mem->turns[i].role, mem->turns[i].content);
    }
    appendf(out, out_size, &used, "<|im_start|>assistant\n");
}


/* ============================================================
 * Commandes de base
 * ============================================================ */

static int cmd_search(const char *query)
{
    const char *api_key = getenv("BRAVE_API_KEY");
    if (!api_key) {
        fprintf(stderr, "Erreur : BRAVE_API_KEY non définie\n");
        return 1;
    }

    if (search_global_init() != 0) return 1;

    SearchResults results;
    if (search_web(api_key, query, &results) != 0) {
        fprintf(stderr, "La recherche a échoué\n");
        search_global_cleanup();
        return 1;
    }

    for (int i = 0; i < results.count; i++) {
        printf("[%d] %s\n    %s\n    %s\n\n",
               i + 1,
               results.items[i].title,
               results.items[i].url,
               results.items[i].description);
    }

    search_global_cleanup();
    return 0;
}

static int cmd_chat(const char *prompt)
{
    const char *model_path = getenv("LLM_MODEL_PATH");
    if (!model_path) {
        fprintf(stderr, "Erreur : LLM_MODEL_PATH non définie\n");
        return 1;
    }

    LlmContext *lc = llm_load(model_path, 8192, 0);
    if (!lc) return 1;

    char full_prompt[MAX_PROMPT_LEN];
    snprintf(full_prompt, sizeof(full_prompt),
             "<|im_start|>system\n%s<|im_end|>\n"
             "<|im_start|>user\n%s<|im_end|>\n"
             "<|im_start|>assistant\n",
             SYSTEM_PROMPT_FR, prompt);

    char out[8192];
    int n = llm_generate(lc, full_prompt, out, sizeof(out), 1024);
    llm_free(lc);

    if (n < 0) {
        fprintf(stderr, "La génération a échoué\n");
        return 1;
    }

    printf("%s\n", out);
    return 0;
}

static int cmd_rag(const char *question)
{
    const char *api_key    = getenv("BRAVE_API_KEY");
    const char *model_path = getenv("LLM_MODEL_PATH");
    if (!api_key || !model_path) {
        fprintf(stderr, "Erreur : BRAVE_API_KEY et LLM_MODEL_PATH sont requis\n");
        return 1;
    }

    if (search_global_init() != 0) return 1;

    SearchResults results;
    if (search_web(api_key, question, &results) != 0) {
        fprintf(stderr, "La recherche a échoué\n");
        search_global_cleanup();
        return 1;
    }
    search_global_cleanup();

    char context[MAX_CONTEXT_LEN];
    search_build_context(&results, context, sizeof(context));

    char prompt[MAX_PROMPT_LEN];
    snprintf(prompt, sizeof(prompt),
             "<|im_start|>system\n%s\n"
             "Tu dois te baser UNIQUEMENT sur les sources fournies ci-dessous.\n"
             "Cite-les sous la forme [Source N].\n"
             "Si les sources sont insuffisantes, dis-le clairement.<|im_end|>\n"
             "<|im_start|>user\n"
             "Sources :\n%s\n\n"
             "Question : %s<|im_end|>\n"
             "<|im_start|>assistant\n",
             SYSTEM_PROMPT_FR, context, question);

    LlmContext *lc = llm_load(model_path, 8192, 0);
    if (!lc) return 1;

    char out[8192];
    int n = llm_generate(lc, prompt, out, sizeof(out), 1024);
    llm_free(lc);

    if (n < 0) {
        fprintf(stderr, "La synthèse a échoué\n");
        return 1;
    }

    printf("%s\n", out);
    return 0;
}

static int cmd_image(const char *prompt, const char *output_path)
{
    const char *sd_bin   = getenv("SD_BINARY_PATH");
    const char *sd_model = getenv("SD_MODEL_PATH");
    if (!sd_bin || !sd_model) {
        fprintf(stderr, "Erreur : SD_BINARY_PATH et SD_MODEL_PATH sont requis\n");
        return 1;
    }

    printf("Génération de l'image en cours...\n");
    if (image_generate(sd_bin, sd_model, prompt, output_path) != 0) {
        fprintf(stderr, "La génération d'image a échoué\n");
        return 1;
    }

    printf("Image générée avec succès : %s\n", output_path);
    return 0;
}

/* ============================================================
 * RAG local (documents)
 * ============================================================ */

static const char *rag_index_path(void)
{
    const char *p = getenv("RAG_INDEX_PATH");
    return p ? p : "rag_index.bin";
}

static int cmd_index(const char *folder)
{
    /* RagIndex pèse plusieurs Mo (RAG_MAX_CHUNKS * ~4.4 Ko) : allocation sur
     * le tas obligatoire, une variable locale de cette taille dépasserait la
     * pile par défaut sur de nombreuses plateformes (1 Mo sous Windows). */
    RagIndex *idx = malloc(sizeof(*idx));
    if (!idx) { fprintf(stderr, "Erreur : mémoire insuffisante pour l'index RAG\n"); return 1; }
    rag_index_init(idx);

    printf("Indexation de « %s » (récursif)...\n", folder);
    int n = rag_index_folder(idx, folder, 1 /* recursive */);
    if (n < 0) {
        fprintf(stderr, "Impossible d'ouvrir le dossier ou erreur de lecture\n");
        free(idx);
        return 1;
    }

    printf("→ %d chunks créés à partir des fichiers texte.\n", n);

    if (rag_index_save(idx, rag_index_path()) != 0) {
        fprintf(stderr, "Échec de la sauvegarde de l'index\n");
        free(idx);
        return 1;
    }

    printf("Index sauvegardé dans « %s »\n", rag_index_path());
    free(idx);
    return 0;
}

static int cmd_ask(const char *question)
{
    const char *model_path = getenv("LLM_MODEL_PATH");
    if (!model_path) {
        fprintf(stderr, "Erreur : LLM_MODEL_PATH est requis\n");
        return 1;
    }

    /* Voir la remarque dans cmd_index : RagIndex doit être sur le tas. */
    RagIndex *idx = malloc(sizeof(*idx));
    if (!idx) { fprintf(stderr, "Erreur : mémoire insuffisante pour l'index RAG\n"); return 1; }

    if (rag_index_load(idx, rag_index_path()) != 0) {
        fprintf(stderr, "Impossible de charger l'index (« %s »).\n"
                        "Lancez d'abord : Lumen index <dossier>\n", rag_index_path());
        free(idx);
        return 1;
    }

    if (idx->count == 0) {
        fprintf(stderr, "L'index est vide.\n");
        free(idx);
        return 1;
    }

    int indices[RAG_TOP_K];
    float scores[RAG_TOP_K];
    /* embed_fn = NULL → BM25 lexical uniquement (branchable plus tard sur un modèle d'embeddings) */
    int n = rag_search(idx, question, RAG_TOP_K, indices, scores, NULL, NULL);

    if (n == 0) {
        printf("Aucun passage pertinent trouvé dans vos documents.\n");
        free(idx);
        return 0;
    }

    printf("→ %d passages pertinents trouvés (BM25/hybride) :\n", n);
    for (int i = 0; i < n; i++) {
        printf("   [%d] %s (score %.3f)\n", i + 1,
               idx->chunks[indices[i]].source, scores[i]);
    }
    printf("\n");

    char context[MAX_CONTEXT_LEN];
    rag_build_context(idx, indices, n, context, sizeof(context));
    free(idx); /* le contexte texte est déjà copié dans `context`, l'index n'est plus nécessaire */

    char prompt[MAX_PROMPT_LEN];
    snprintf(prompt, sizeof(prompt),
             "<|im_start|>system\n%s\n"
             "Tu dois répondre UNIQUEMENT à partir des extraits de documents fournis.\n"
             "Cite le nom du document source. Si l'information n'est pas présente, dis-le clairement.\n"
             "N'invente rien.<|im_end|>\n"
             "<|im_start|>user\n"
             "Documents :\n%s\n\n"
             "Question : %s<|im_end|>\n"
             "<|im_start|>assistant\n",
             SYSTEM_PROMPT_FR, context, question);

    LlmContext *lc = llm_load(model_path, 8192, 0);
    if (!lc) return 1;

    char out[8192];
    int gen = llm_generate(lc, prompt, out, sizeof(out), 1024);
    llm_free(lc);

    if (gen < 0) {
        fprintf(stderr, "La génération a échoué\n");
        return 1;
    }

    printf("%s\n", out);
    return 0;
}

/* ============================================================
 * Mode agentique (boucle multi-étapes)
 * ============================================================ */

static int cmd_agent(const char *objective)
{
    const char *api_key    = getenv("BRAVE_API_KEY");
    const char *model_path = getenv("LLM_MODEL_PATH");
    if (!model_path) {
        fprintf(stderr, "Erreur : LLM_MODEL_PATH est requis\n");
        return 1;
    }

    LlmContext *lc = llm_load(model_path, 8192, 0);
    if (!lc) return 1;

    ConversationMemory mem = {0};
    memory_add(&mem, "user", objective);

    char context[MAX_CONTEXT_LEN] = {0};
    int max_steps = 5;

    printf("\n=== Mode Agent — Objectif : %s ===\n\n", objective);

    for (int step = 0; step < max_steps; step++) {
        char prompt[MAX_PROMPT_LEN];
        memory_build_prompt(&mem, SYSTEM_PROMPT_AGENT, context, prompt, sizeof(prompt));

        char response[8192];
        int n = llm_generate(lc, prompt, response, sizeof(response), 768);
        if (n < 0) {
            fprintf(stderr, "Échec de génération à l'étape %d\n", step + 1);
            break;
        }

        trim(response);
        printf("Étape %d :\n%s\n\n", step + 1, response);

        /* Détection d'une demande de recherche */
        if (strncmp(response, "RECHERCHE:", 10) == 0 ||
            strstr(response, "RECHERCHE:") != NULL)
        {
            char *req = strstr(response, "RECHERCHE:");
            if (req) {
                req += 10;
                while (*req && isspace((unsigned char)*req)) req++;
                trim(req);

                if (api_key && req[0]) {
                    printf("→ Recherche en cours : \"%s\"\n", req);
                    if (search_global_init() == 0) {
                        SearchResults results;
                        if (search_web(api_key, req, &results) == 0) {
                            search_build_context(&results, context, sizeof(context));
                            printf("→ %d sources trouvées.\n\n", results.count);
                        }
                        search_global_cleanup();
                    }
                    /* On ajoute la demande et on continue la boucle */
                    memory_add(&mem, "assistant", response);
                    char tool_msg[MAX_MSG_LEN];
                    snprintf(tool_msg, sizeof(tool_msg), "Résultats de la recherche :\n%.*s",
                             (int)(sizeof(tool_msg) - 32), context);
                    memory_add(&mem, "user", tool_msg);
                    continue;
                }
            }
        }

        /* Sinon c'est une réponse finale */
        memory_add(&mem, "assistant", response);
        break;
    }

    llm_free(lc);
    return 0;
}

/* ============================================================
 * Mode interactif (conversation continue avec mémoire)
 * ============================================================ */

static int cmd_interact(void)
{
    const char *model_path = getenv("LLM_MODEL_PATH");
    if (!model_path) {
        fprintf(stderr, "Erreur : LLM_MODEL_PATH non définie\n");
        return 1;
    }

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║              Lumen 5.0 — Mode interactif                      ║\n");
    printf("║  Commandes spéciales :                                   ║\n");
    printf("║    /search <requête>   → recherche web                   ║\n");
    printf("║    /ask    <question>  → question sur vos documents      ║\n");
    printf("║    /index  <dossier>   → indexe un dossier de documents  ║\n");
    printf("║    /image  <desc>      → génère une image                ║\n");
    printf("║    /clear              → efface la mémoire               ║\n");
    printf("║    /quit ou /exit      → quitter                         ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    LlmContext *lc = llm_load(model_path, 8192, 0);
    if (!lc) return 1;

    ConversationMemory mem = {0};
    char line[MAX_MSG_LEN];

    while (1) {
        printf("Vous > ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        trim(line);
        if (line[0] == '\0') continue;

        /* Commandes spéciales */
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
            printf("Au revoir.\n");
            break;
        }
        if (strcmp(line, "/clear") == 0) {
            mem.count = 0;
            printf("(mémoire effacée)\n\n");
            continue;
        }
        if (strncmp(line, "/search ", 8) == 0) {
            cmd_search(line + 8);
            printf("\n");
            continue;
        }
        if (strncmp(line, "/image ", 7) == 0) {
            char outname[64];
            snprintf(outname, sizeof(outname), "image_%ld.png", (long)time(NULL));
            cmd_image(line + 7, outname);
            printf("\n");
            continue;
        }
        if (strncmp(line, "/ask ", 5) == 0) {
            cmd_ask(line + 5);
            printf("\n");
            continue;
        }
        if (strncmp(line, "/index ", 7) == 0) {
            cmd_index(line + 7);
            printf("\n");
            continue;
        }

        /* Conversation normale */
        memory_add(&mem, "user", line);

        char prompt[MAX_PROMPT_LEN];
        memory_build_prompt(&mem, SYSTEM_PROMPT_FR, NULL, prompt, sizeof(prompt));

        char response[8192];
        int n = llm_generate(lc, prompt, response, sizeof(response), 1024);
        if (n < 0) {
            fprintf(stderr, "Erreur de génération\n");
            continue;
        }

        trim(response);
        printf("\nLumen > %s\n\n", response);
        memory_add(&mem, "assistant", response);
    }

    llm_free(lc);
    return 0;
}

/* ============================================================
 * Point d'entrée
 * ============================================================ */

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); /* accents corrects dans la console */
    SetConsoleCP(CP_UTF8);
#endif
    /* Mode interactif par défaut */
    if (argc < 2) {
        return cmd_interact();
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "interact") == 0 || strcmp(cmd, "i") == 0) {
        return cmd_interact();
    }
    else if (strcmp(cmd, "search") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        return cmd_search(argv[2]);
    }
    else if (strcmp(cmd, "chat") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        return cmd_chat(argv[2]);
    }
    else if (strcmp(cmd, "rag") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        return cmd_rag(argv[2]);
    }
    else if (strcmp(cmd, "index") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        return cmd_index(argv[2]);
    }
    else if (strcmp(cmd, "ask") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        return cmd_ask(argv[2]);
    }
    else if (strcmp(cmd, "agent") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        return cmd_agent(argv[2]);
    }
    else if (strcmp(cmd, "image") == 0) {
        if (argc < 4) { print_usage(argv[0]); return 1; }
        return cmd_image(argv[2], argv[3]);
    }
    else if (strcmp(cmd, "serve") == 0 || strcmp(cmd, "server") == 0) {
        int port = 8080;
        const char *pe = getenv("LUMEN_PORT");
        if (pe && atoi(pe) > 0 && atoi(pe) <= 65535) port = atoi(pe);
        if (argc >= 3) {
            port = atoi(argv[2]);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "Port invalide : %s\n", argv[2]);
                return 1;
            }
        }
        return server_run(port);
    }
    else if (strcmp(cmd, "gui") == 0 || strcmp(cmd, "ui") == 0) {
        return gui_run();
    }
    else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "Commande inconnue : %s\n", cmd);
    print_usage(argv[0]);
    return 1;
}
