#ifndef SEARCH_H
#define SEARCH_H

#include <stddef.h>

#define SEARCH_MAX_RESULTS 5
#define SEARCH_TITLE_LEN   256
#define SEARCH_URL_LEN     512
#define SEARCH_DESC_LEN    1024

typedef struct {
    char title[SEARCH_TITLE_LEN];
    char url[SEARCH_URL_LEN];
    char description[SEARCH_DESC_LEN];
} SearchResult;

typedef struct {
    SearchResult items[SEARCH_MAX_RESULTS];
    int count;
} SearchResults;

/* Initialise curl (à appeler une fois au démarrage du programme) */
int search_global_init(void);

/* Libère curl (à appeler avant de quitter le programme) */
void search_global_cleanup(void);

/*
 * Effectue une recherche web via l'API Brave Search.
 * api_key : clé d'API Brave (https://api.search.brave.com)
 * query   : requête utilisateur
 * out     : structure remplie avec les résultats
 * Retourne 0 en cas de succès, -1 en cas d'erreur.
 */
int search_web(const char *api_key, const char *query, SearchResults *out);

/*
 * Construit un contexte texte à partir des résultats,
 * prêt à être injecté dans le prompt du LLM (pattern RAG).
 * buf/buf_size : buffer de sortie fourni par l'appelant.
 */
void search_build_context(const SearchResults *results, char *buf, size_t buf_size);

#endif /* SEARCH_H */
