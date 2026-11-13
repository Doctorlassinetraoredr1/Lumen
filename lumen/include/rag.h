#ifndef RAG_H
#define RAG_H

#include <stddef.h>

#define RAG_MAX_CHUNKS        1024
#define RAG_CHUNK_SIZE        900     /* caractères approximatifs */
#define RAG_CHUNK_OVERLAP     120
#define RAG_EMBED_DIM         768     /* dimension typique (ajuste selon modèle) */
#define RAG_MAX_PATH          512
#define RAG_TOP_K             6
#define RAG_MIN_SCORE         0.05f   /* seuil minimal pour retenir un résultat */
#define RAG_HYBRID_ALPHA      0.65f   /* poids sémantique dans le score hybride (0=lex only, 1=embed only) */

typedef struct {
    char   text[RAG_CHUNK_SIZE + 64];
    char   source[RAG_MAX_PATH];
    float  embedding[RAG_EMBED_DIM];
    int    has_embedding;             /* 1 si l'embedding a été calculé */
    int    token_count;               /* nombre de tokens approximatif (BM25) */
} RagChunk;

typedef struct {
    RagChunk chunks[RAG_MAX_CHUNKS];
    int      count;
    int      embed_dim;               /* dimension réelle utilisée */
    /* Stats corpus pour BM25 (calculées à l'indexation / au load) */
    float    avg_chunk_len;           /* longueur moyenne en tokens */
    int      doc_freq_ready;          /* 1 si les stats BM25 sont valides */
    int      embeddings_normalized;   /* 1 si tous les embeddings sont L2-normalisés → cos = dot */
} RagIndex;

/* Callback optionnel pour calculer l'embedding d'un texte.
 * Doit remplir out[0..dim-1] et retourner 0 en succès, -1 en échec.
 * Si non fourni, seule la recherche lexicale (BM25) est utilisée.
 */
typedef int (*RagEmbedFn)(const char *text, float *out, int dim, void *userdata);

/* Initialise un index vide */
void rag_index_init(RagIndex *idx);

/*
 * Indexe tous les fichiers texte d'un dossier.
 * recursive != 0 : parcours récursif des sous-dossiers.
 * Retourne le nombre de chunks ajoutés, ou -1 en cas d'erreur.
 */
int rag_index_folder(RagIndex *idx, const char *folder_path, int recursive);

/*
 * Sauvegarde l'index sur disque (format binaire simple + texte).
 * Permet de ne pas ré-indexer à chaque lancement.
 */
int rag_index_save(const RagIndex *idx, const char *path);

/* Charge un index précédemment sauvegardé */
int rag_index_load(RagIndex *idx, const char *path);

/*
 * Calcule (ou recalcule) les embeddings de tous les chunks via embed_fn.
 * Retourne le nombre de chunks enrichis, ou -1 si embed_fn est NULL / erreur.
 */
int rag_index_compute_embeddings(RagIndex *idx, RagEmbedFn embed_fn, void *userdata);

/*
 * Recherche les top-k chunks les plus pertinents pour une requête.
 * - Si des embeddings sont présents ET embed_fn fourni : score hybride
 *   (cosinus query↔chunk + BM25).
 * - Sinon : BM25 lexical uniquement.
 * out_indices / out_scores : tableaux de taille au moins top_k
 * Retourne le nombre de résultats réellement trouvés (score >= RAG_MIN_SCORE).
 */
int rag_search(const RagIndex *idx, const char *query,
               int top_k, int *out_indices, float *out_scores,
               RagEmbedFn embed_fn, void *userdata);

/*
 * Construit un contexte texte prêt à injecter dans le prompt LLM
 * à partir des indices retournés par rag_search.
 */
void rag_build_context(const RagIndex *idx, const int *indices, int n,
                       char *buf, size_t buf_size);

/* Similarité cosinus entre deux vecteurs (utilitaire public) */
float rag_cosine_similarity(const float *a, const float *b, int dim);

/* Produit scalaire optimisé (boucle déroulée + SIMD si dispo) */
float rag_dot_product(const float *a, const float *b, int dim);

/* Normalise L2 un vecteur in-place. Retourne la norme avant normalisation (0 si nulle). */
float rag_l2_normalize(float *v, int dim);

#endif /* RAG_H */
