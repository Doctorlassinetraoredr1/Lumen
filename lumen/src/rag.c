/*
 * rag.c — RAG local en C pur (Lumen 5.x)
 * Indexation de documents + recherche hybride BM25 + cosinus
 * 100 % portable Windows / Linux
 *
 * Améliorations vs version précédente :
 *  - Scoring lexical BM25 (au lieu d'un simple ratio de mots)
 *  - Similarité cosinus réelle quand les embeddings sont présents
 *  - Recherche hybride (alpha * sémantique + (1-alpha) * BM25)
 *  - Indexation récursive optionnelle
 *  - Stopwords FR/EN, normalisation accents légers
 *  - Déduplication basique des chunks quasi-identiques
 *  - Stats corpus (avgdl) pour BM25 recalculées au load
 *  - Hook RagEmbedFn pour brancher un vrai modèle d'embeddings
 */

#include "rag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ============================================================
 * Paramètres BM25
 * ============================================================ */
#define BM25_K1   1.5f
#define BM25_B    0.75f

/* ============================================================
 * Stopwords multilingues — focus Afrique
 * FR/EN/PT (langues officielles) + arabe + langues africaines majeures
 * (swahili, haoussa, yoruba, igbo, zoulou, xhosa, afrikaans,
 *  somali, wolof, bambara, lingala, amharique romanisé, etc.)
 * Lookup O(1) via table de hachage ouverte.
 * ============================================================ */
static const char *STOPWORDS[] = {
    /* —— Français —— */
    "le","la","les","un","une","des","de","du","au","aux","et","ou","mais",
    "donc","or","ni","car","que","qui","quoi","dont","où","ce","cet","cette",
    "ces","mon","ton","son","ma","ta","sa","mes","tes","ses","notre","votre",
    "leur","nos","vos","leurs","je","tu","il","elle","on","nous","vous","ils",
    "elles","me","te","se","lui","y","en","ne","pas","plus","moins","très",
    "trop","peu","beaucoup","tout","tous","toute","toutes","avec","sans",
    "sous","sur","dans","par","pour","entre","vers","chez","depuis","pendant",
    "avant","après","est","sont","être","avoir","fait","faire","peut","doit",
    "comme","aussi","alors","ainsi","encore","déjà","ici","là","si","quand",
    "comment","pourquoi","parce","cest","c'est","d'un","d'une","l'on","qu'il",
    /* —— English —— */
    "the","a","an","and","or","but","in","on","at","to","for","of","with",
    "by","from","as","is","are","was","were","be","been","being","have","has",
    "had","do","does","did","will","would","could","should","may","might",
    "this","that","these","those","it","its","they","them","their","we","you",
    "he","she","his","her","not","no","yes","so","if","then","than","too",
    "very","just","also","only","about","into","over","after","before","up",
    "down","out","off","can","all","any","each","few","more","most","other",
    "some","such","than","too","very",
    /* —— Português (Angola, Moçambique, Guiné-Bissau, Cabo Verde, STP) —— */
    "o","a","os","as","um","uma","uns","umas","de","do","da","dos","das",
    "em","no","na","nos","nas","por","para","com","sem","sobre","entre",
    "e","ou","mas","que","quem","qual","quando","onde","como","porque",
    "se","nao","não","sim","mais","menos","muito","pouco","todo","toda",
    "todos","todas","este","esta","esse","essa","aquele","aquela","ele",
    "ela","eles","elas","eu","tu","voce","você","nos","nós","vos","já",
    "ainda","tambem","também","ser","estar","ter","fazer","poder","ir",
    /* —— العربية / Arabic (Afrique du Nord, Sahel) —— */
    "في","من","على","إلى","الى","عن","مع","هذا","هذه","ذلك","تلك",
    "التي","الذي","الذين","اللذين","كان","كانت","يكون","تكون","ما","لا",
    "أن","ان","إن","او","أو","و","يا","كل","بعض","أي","اي","لم","لن",
    "قد","حتى","بعد","قبل","عند","منذ","بين","تحت","فوق","أمام","امام",
    "هناك","هنا","حيث","كيف","لماذا","متى","هل","نعم","كلا","ليس",
    "أنا","انا","أنت","انت","هو","هي","نحن","هم","هن","أنتما","انتم",
    /* —— Kiswahili / Swahili (Afrique de l'Est) —— */
    "na","ya","wa","za","la","cha","vya","kwa","ni","si","huo","hio",
    "hii","hao","hayo","hicho","hivyo","yeye","sisi","nyinyi","wao",
    "mimi","wewe","yake","yetu","yenu","yao","katika","kama","lakini",
    "au","ama","pia","tu","sana","zaidi","kidogo","wote","zote","yote",
    "baada","kabla","tangu","hadi","bila","pamoja","sasa","leo",
    "jana","kesho","hapa","pale","kule","wapi","lini","kwa","nini","je",
    "ndiyo","hapana","kwamba","kuwa","alikuwa","walikuwa","nitakuwa",
    /* —— Hausa (Afrique de l'Ouest) —— */
    "da","a","na","ta","ya","ce","ne","ba","ko","sai","kuma","amma",
    "idan","lokacin","saboda","don","ga","cikin","akan","kafin","bayan",
    "yana","tana","suna","muna","kuna","ina","kai","ke","shi","ita",
    "mu","ku","su","wannan","wancan","wadannan","duk","dukkan","kowa",
    "komai","yaushe","ina","yaya","me","wa","to","i","a'a","eh",
    /* —— Yorùbá (Nigeria, Bénin) —— */
    "ni","ti","ati","si","fun","pe","je","ko","ki","ba","wa","re",
    "mi","o","won","awon","eyi","yen","naa","gbogbo","pupo","die",
    "sugbon","tabi","bi","nigba","nitori","lati","de","lori","ninu",
    "bebe","bayi","nibo","kilode","bawo","se","ti","mo","a","e","won",
    /* —— Igbo (Nigeria) —— */
    "na","nke","nye","ga","ka","ma","maka","ya","ha","anyi","gi","m",
    "o","nkea","ahụ","niile","ọtụtụ","obere","mana","maọbụ","mgbe",
    "tupu","mgbe","ebe","otú","gịnị","onye","ee","mba","bụ","dị",
    /* —— isiZulu (Afrique du Sud) —— */
    "na","nga","ku","ka","ya","wa","ba","la","sa","tha","ukuthi","uma",
    "kodwa","futhi","noma","ngoba","ngaphambi","ngemva","kulo","kule",
    "lo","le","laba","lokhu","konke","kakhulu","kancane","yebo","cha",
    "mina","wena","yena","thina","nina","bona","ngi","u","si","ni",
    /* —— isiXhosa (Afrique du Sud) —— */
    "na","nga","ku","ka","ya","wa","ba","ukuba","kodwa","kwaye","okanye",
    "kuba","phambi","emva","apha","aphaya","yonke","kakhulu","ewe","hayi",
    "mna","wena","yena","thina","nina","bona","ndi","u","si","ni",
    /* —— Afrikaans (Afrique du Sud / Namibie) —— */
    "die","n","'n","en","of","maar","in","op","met","van","vir","is",
    "was","wees","het","nie","wel","dit","hierdie","daardie","hy","sy",
    "ons","julle","hulle","ek","jy","u","my","jou","haar","hul","ook",
    "net","baie","min","alles","iemand","iets","waar","wanneer","hoe",
    "waarom","want","sodat","voordat","nadat","sonder","tussen","oor",
    /* —— Soomaali / Somali —— */
    "oo","ee","ah","waa","waxa","ku","ka","la","iyo","ama","laakiin",
    "haddi","marka","sabab","u","in","ay","uu","aan","aad","ayay",
    "wax","kasta","badan","yar","halkan","halkaas","xaggee","goorma",
    "sidee","maxaa","haan","maya","waaye","ahaa","yimid","tagay",
    /* —— Wolof (Sénégal, Gambie) —— */
    "ci","ak","bu","bi","ba","yi","yu","ni","lu","ku","nu","leen",
    "ma","la","na","woon","te","wall","ndax","ndaxte","kon","léegi",
    "démb","suba","fii","foofu","fan","kañ","naka","lan","waaw","déedéet",
    /* —— Bamanankan / Bambara (Mali, Burkina, Guinée) —— */
    "ye","ka","ni","ani","walima","nga","sabu","kama","fen","mɔgɔ",
    "a","ne","e","an","aw","olu","nin","o","bɛɛ","caman","dɔɔnin",
    "yan","yen","min","joli","cogo","mun","awɔ","ayi","bɛ","tɛ",
    /* —— Lingala (RDC, Congo, RCA) —— */
    "na","ya","la","te","mpe","kasi","soki","mpo","ya","oyo","wana",
    "nyonso","mingi","moke","awa","kuna","wapi","ntango","ndenge","nini",
    "nani","ee","te","ngai","yo","ye","biso","bino","bango","ezali",
    /* —— Amharique romanisé (Éthiopie) — formes courantes ASCII —— */
    "ena","weym","ke","be","le","ye","ne","nat","nachu",
    "ine","ante","anci","isu","iswa","igna","inante","inesu","yih",
    "ya","hullu","bezat","tinish","ezih","eziya","meche","indet",
    "min","awo","ay","neber",
    NULL
};

/* Table de hachage ouverte pour lookup O(1) des stopwords */
#define RAG_SW_HASH_SIZE 4096
static void normalize_token(char *s);
static const char *sw_hash[RAG_SW_HASH_SIZE];
static int sw_hash_ready = 0;

static unsigned rag_hash_str(const char *s)
{
    /* FNV-1a 32-bit */
    unsigned h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)(*s);
        h *= 16777619u;
    }
    return h;
}

static char *sw_store_copy(const char *w)
{
    size_t n = strlen(w);
    char *c = (char *)malloc(n + 1);
    if (c) memcpy(c, w, n + 1);
    return c;
}

/* Les stopwords sont stockés normalisés (sans accents, minuscules), comme les
 * tokens de requête : sinon « très », « où », « já »… ne seraient jamais filtrés. */
static void sw_hash_init(void)
{
    if (sw_hash_ready) return;
    memset(sw_hash, 0, sizeof(sw_hash));
    for (int i = 0; STOPWORDS[i]; i++) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s", STOPWORDS[i]);
        normalize_token(tmp);
        if (!tmp[0]) continue;
        unsigned h = rag_hash_str(tmp) % RAG_SW_HASH_SIZE;
        for (int probe = 0; probe < RAG_SW_HASH_SIZE; probe++) {
            unsigned slot = (h + (unsigned)probe) % RAG_SW_HASH_SIZE;
            if (!sw_hash[slot]) { sw_hash[slot] = sw_store_copy(tmp); break; }
            if (strcmp(sw_hash[slot], tmp) == 0) break; /* doublon */
        }
    }
    sw_hash_ready = 1;
}

static int is_stopword(const char *w)
{
    if (!w || !*w) return 1;
    size_t len = strlen(w);
    if (len <= 1) return 1;
    if (len >= 64) return 0;
    if (!sw_hash_ready) sw_hash_init();
    char tmp[64];
    memcpy(tmp, w, len + 1);
    normalize_token(tmp);
    unsigned h = rag_hash_str(tmp) % RAG_SW_HASH_SIZE;
    for (int probe = 0; probe < RAG_SW_HASH_SIZE; probe++) {
        unsigned slot = (h + (unsigned)probe) % RAG_SW_HASH_SIZE;
        if (!sw_hash[slot]) return 0;
        if (strcmp(sw_hash[slot], tmp) == 0) return 1;
    }
    return 0;
}

/* ============================================================
 * Utilitaires texte
 * ============================================================ */

static void to_lower(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* Normalisation légère : minuscules + suppression accents courants (latin1/utf8 simple) */
static void normalize_token(char *s)
{
    to_lower(s);
    char *src = s, *dst = s;
    while (*src) {
        unsigned char c = (unsigned char)*src;
        if (c == 0xC5 && src[1] && ((unsigned char)src[1] == 0x93 || (unsigned char)src[1] == 0x92)) {
            *dst++ = 'o'; *dst++ = 'e'; src += 2; continue;          /* œ Œ */
        }
        if (c == 0xC3 && src[1]) {
            unsigned char n = (unsigned char)src[1];
            if (n >= 0x80 && n <= 0x9E) n = (unsigned char)(n + 0x20); /* À-Þ → à-þ */
            char repl = 0;
            if (n >= 0xA0 && n <= 0xA5) repl = 'a';      /* à á â ã ä å */
            else if (n == 0xA7) repl = 'c';               /* ç */
            else if (n >= 0xA8 && n <= 0xAB) repl = 'e'; /* è é ê ë */
            else if (n >= 0xAC && n <= 0xAF) repl = 'i'; /* ì í î ï */
            else if (n == 0xB1) repl = 'n';               /* ñ */
            else if (n >= 0xB2 && n <= 0xB6) repl = 'o'; /* ò ó ô õ ö */
            else if (n >= 0xB9 && n <= 0xBC) repl = 'u'; /* ù ú û ü */
            else if (n == 0xBD || n == 0xBF) repl = 'y'; /* ý ÿ */
            if (repl) { *dst++ = repl; src += 2; continue; }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/* Octet appartenant à un mot : alphanumérique ASCII ou octet UTF-8 (lettres non latines, ɔ, ɛ…). */
static int is_word_byte(unsigned char c) { return isalnum(c) || c >= 0x80; }

static int ext_eq(const char *ext, const char *want)
{
    for (; *ext && *want; ext++, want++)
        if (tolower((unsigned char)*ext) != *want) return 0;
    return *ext == *want;
}

static int is_text_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return 0;
    static const char *exts[] = { ".txt", ".md", ".csv", ".log", ".json", ".rst", ".tex", NULL };
    for (int i = 0; exts[i]; i++) if (ext_eq(ext, exts[i])) return 1;
    return 0;
}

/* Compte approximatif de tokens (mots) dans un texte */
static int count_tokens(const char *text)
{
    if (!text || !*text) return 0;
    char buf[RAG_CHUNK_SIZE + 64];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    to_lower(buf);
    int n = 0;
    char *tok = strtok(buf, " \t\n\r.,;:!?\"'()[]{}«»/\\|-+*=<>@#%&");
    while (tok) {
        if (!is_stopword(tok)) n++;
        tok = strtok(NULL, " \t\n\r.,;:!?\"'()[]{}«»/\\|-+*=<>@#%&");
    }
    return n > 0 ? n : 1;
}

/* ============================================================
 * BM25
 * ============================================================ */

/* Compte réellement df (document frequency) en pré-passant le corpus */
static void compute_df(const RagIndex *idx, const char **qtokens, int nq, int *df_out)
{
    for (int qi = 0; qi < nq; qi++) df_out[qi] = 0;
    for (int i = 0; i < idx->count; i++) {
        char cbuf[RAG_CHUNK_SIZE + 64];
        strncpy(cbuf, idx->chunks[i].text, sizeof(cbuf) - 1);
        cbuf[sizeof(cbuf) - 1] = '\0';
        normalize_token(cbuf);
        for (int qi = 0; qi < nq; qi++) {
            const char *term = qtokens[qi];
            if (!term) continue;
            size_t tlen = strlen(term);
            const char *p = cbuf;
            while ((p = strstr(p, term)) != NULL) {
                int left_ok  = (p == cbuf) || !is_word_byte((unsigned char)p[-1]);
                int right_ok = !is_word_byte((unsigned char)p[tlen]);
                if (left_ok && right_ok) { df_out[qi]++; break; }
                p += tlen;
            }
        }
    }
}

static float bm25_score_with_df(const char *chunk_text, int chunk_len,
                                const char **qtokens, int nq,
                                const int *df, float avgdl, int N)
{
    if (nq == 0 || !chunk_text || avgdl <= 0.0f || N <= 0) return 0.0f;

    char cbuf[RAG_CHUNK_SIZE + 64];
    strncpy(cbuf, chunk_text, sizeof(cbuf) - 1);
    cbuf[sizeof(cbuf) - 1] = '\0';
    normalize_token(cbuf);

    float score = 0.0f;
    float dl = (float)(chunk_len > 0 ? chunk_len : 1);

    for (int qi = 0; qi < nq; qi++) {
        const char *term = qtokens[qi];
        if (!term || !*term) continue;

        int tf = 0;
        size_t tlen = strlen(term);
        const char *p = cbuf;
        while ((p = strstr(p, term)) != NULL) {
            int left_ok  = (p == cbuf) || !is_word_byte((unsigned char)p[-1]);
            int right_ok = !is_word_byte((unsigned char)p[tlen]);
            if (left_ok && right_ok) tf++;
            p += tlen;
        }
        if (tf == 0) continue;

        float dfi = (float)(df[qi] > 0 ? df[qi] : 1);
        float idf = logf(((float)N - dfi + 0.5f) / (dfi + 0.5f) + 1.0f);
        if (idf < 0.0f) idf = 0.0f;

        float tf_norm = (tf * (BM25_K1 + 1.0f)) /
                        (tf + BM25_K1 * (1.0f - BM25_B + BM25_B * (dl / avgdl)));
        score += idf * tf_norm;
    }
    return score;
}

/* ============================================================
 * Vecteurs — produit scalaire / cosinus / L2 (optimisés)
 *
 * Stratégie :
 *  1. Pré-normaliser les embeddings à l'indexation → cosinus = dot product
 *  2. Dot product en float32, boucle déroulée x4, restrict
 *  3. SSE/AVX optionnel si le compilateur le propose (__SSE__ / __AVX__)
 * ============================================================ */

#if defined(__AVX__)
#include <immintrin.h>
#define RAG_HAVE_AVX 1
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAG_HAVE_SSE 1
#endif

/* Somme horizontale portable (évite _mm_hadd_ps / SSE3) */
#if defined(RAG_HAVE_SSE) || defined(RAG_HAVE_AVX)
static inline float rag_hsum128(__m128 v)
{
    __m128 shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
    __m128 sums = _mm_add_ps(v, shuf);
    shuf = _mm_movehl_ps(shuf, sums);
    sums = _mm_add_ss(sums, shuf);
    return _mm_cvtss_f32(sums);
}
#endif
#if defined(RAG_HAVE_AVX)
static inline float rag_hsum256(__m256 v)
{
    __m128 low  = _mm256_castps256_ps128(v);
    __m128 high = _mm256_extractf128_ps(v, 1);
    return rag_hsum128(_mm_add_ps(low, high));
}
#endif

float rag_l2_normalize(float *v, int dim)
{
    if (!v || dim <= 0) return 0.0f;
    float sum = 0.0f;
    int i = 0;
#if defined(RAG_HAVE_AVX)
    {
        __m256 acc = _mm256_setzero_ps();
        for (; i + 8 <= dim; i += 8) {
            __m256 x = _mm256_loadu_ps(v + i);
            acc = _mm256_add_ps(acc, _mm256_mul_ps(x, x));
        }
        sum = rag_hsum256(acc);
    }
#elif defined(RAG_HAVE_SSE)
    {
        __m128 acc = _mm_setzero_ps();
        for (; i + 4 <= dim; i += 4) {
            __m128 x = _mm_loadu_ps(v + i);
            acc = _mm_add_ps(acc, _mm_mul_ps(x, x));
        }
        sum = rag_hsum128(acc);
    }
#else
    for (; i + 4 <= dim; i += 4) {
        sum += v[i]*v[i] + v[i+1]*v[i+1] + v[i+2]*v[i+2] + v[i+3]*v[i+3];
    }
#endif
    for (; i < dim; i++)
        sum += v[i] * v[i];

    if (sum <= 1e-12f) return 0.0f;
    float norm = sqrtf(sum);
    float inv  = 1.0f / norm;
    for (i = 0; i < dim; i++)
        v[i] *= inv;
    return norm;
}

#ifndef RAG_RESTRICT
#  if defined(__GNUC__) || defined(__clang__)
#    define RAG_RESTRICT __restrict__
#  elif defined(_MSC_VER)
#    define RAG_RESTRICT __restrict
#  else
#    define RAG_RESTRICT
#  endif
#endif

float rag_dot_product(const float *RAG_RESTRICT a, const float *RAG_RESTRICT b, int dim)
{
    if (!a || !b || dim <= 0) return 0.0f;
    int i = 0;
#if defined(RAG_HAVE_AVX)
    {
        __m256 acc = _mm256_setzero_ps();
        for (; i + 8 <= dim; i += 8) {
            __m256 xa = _mm256_loadu_ps(a + i);
            __m256 xb = _mm256_loadu_ps(b + i);
#if defined(__FMA__)
            acc = _mm256_fmadd_ps(xa, xb, acc);
#else
            acc = _mm256_add_ps(acc, _mm256_mul_ps(xa, xb));
#endif
        }
        float partial = rag_hsum256(acc);
        for (; i < dim; i++)
            partial += a[i] * b[i];
        return partial;
    }
#elif defined(RAG_HAVE_SSE)
    {
        __m128 acc = _mm_setzero_ps();
        for (; i + 4 <= dim; i += 4) {
            __m128 xa = _mm_loadu_ps(a + i);
            __m128 xb = _mm_loadu_ps(b + i);
            acc = _mm_add_ps(acc, _mm_mul_ps(xa, xb));
        }
        float partial = rag_hsum128(acc);
        for (; i < dim; i++)
            partial += a[i] * b[i];
        return partial;
    }
#else
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    for (; i + 4 <= dim; i += 4) {
        s0 += a[i]     * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    float partial = s0 + s1 + s2 + s3;
    for (; i < dim; i++)
        partial += a[i] * b[i];
    return partial;
#endif
}

float rag_cosine_similarity(const float *a, const float *b, int dim)
{
    if (!a || !b || dim <= 0) return 0.0f;

    int i = 0;
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f; /* dot, ||a||², ||b||² */
#if defined(RAG_HAVE_AVX)
    {
        __m256 dacc = _mm256_setzero_ps();
        __m256 aacc = _mm256_setzero_ps();
        __m256 bacc = _mm256_setzero_ps();
        for (; i + 8 <= dim; i += 8) {
            __m256 xa = _mm256_loadu_ps(a + i);
            __m256 xb = _mm256_loadu_ps(b + i);
#if defined(__FMA__)
            dacc = _mm256_fmadd_ps(xa, xb, dacc);
            aacc = _mm256_fmadd_ps(xa, xa, aacc);
            bacc = _mm256_fmadd_ps(xb, xb, bacc);
#else
            dacc = _mm256_add_ps(dacc, _mm256_mul_ps(xa, xb));
            aacc = _mm256_add_ps(aacc, _mm256_mul_ps(xa, xa));
            bacc = _mm256_add_ps(bacc, _mm256_mul_ps(xb, xb));
#endif
        }
        s0 = rag_hsum256(dacc);
        s1 = rag_hsum256(aacc);
        s2 = rag_hsum256(bacc);
    }
#elif defined(RAG_HAVE_SSE)
    {
        __m128 dacc = _mm_setzero_ps();
        __m128 aacc = _mm_setzero_ps();
        __m128 bacc = _mm_setzero_ps();
        for (; i + 4 <= dim; i += 4) {
            __m128 xa = _mm_loadu_ps(a + i);
            __m128 xb = _mm_loadu_ps(b + i);
            dacc = _mm_add_ps(dacc, _mm_mul_ps(xa, xb));
            aacc = _mm_add_ps(aacc, _mm_mul_ps(xa, xa));
            bacc = _mm_add_ps(bacc, _mm_mul_ps(xb, xb));
        }
        s0 = rag_hsum128(dacc);
        s1 = rag_hsum128(aacc);
        s2 = rag_hsum128(bacc);
    }
#endif
    for (; i < dim; i++) {
        float ai = a[i], bi = b[i];
        s0 += ai * bi;
        s1 += ai * ai;
        s2 += bi * bi;
    }
    if (s1 <= 1e-12f || s2 <= 1e-12f) return 0.0f;
    return s0 / (sqrtf(s1) * sqrtf(s2));
}

/* ============================================================
 * Tokenisation requête
 * ============================================================ */

#define RAG_MAX_QTOKENS 64

static int tokenize_query(const char *query, char tokbuf[][64], const char **out, int max_tok)
{
    char q[512];
    strncpy(q, query, sizeof(q) - 1);
    q[sizeof(q) - 1] = '\0';
    normalize_token(q);

    int n = 0;
    char *token = strtok(q, " \t\n\r.,;:!?\"'()[]{}«»/\\|-+*=<>@#%&");
    while (token && n < max_tok) {
        normalize_token(token);
        if (!is_stopword(token) && strlen(token) > 1) {
            strncpy(tokbuf[n], token, 63);
            tokbuf[n][63] = '\0';
            out[n] = tokbuf[n];
            n++;
        }
        token = strtok(NULL, " \t\n\r.,;:!?\"'()[]{}«»/\\|-+*=<>@#%&");
    }
    return n;
}

/* ============================================================
 * Stats corpus
 * ============================================================ */

static void recompute_corpus_stats(RagIndex *idx)
{
    if (!idx || idx->count <= 0) {
        if (idx) { idx->avg_chunk_len = 1.0f; idx->doc_freq_ready = 0; }
        return;
    }
    double sum = 0.0;
    for (int i = 0; i < idx->count; i++) {
        if (idx->chunks[i].token_count <= 0)
            idx->chunks[i].token_count = count_tokens(idx->chunks[i].text);
        sum += (double)idx->chunks[i].token_count;
    }
    idx->avg_chunk_len = (float)(sum / (double)idx->count);
    if (idx->avg_chunk_len < 1.0f) idx->avg_chunk_len = 1.0f;
    idx->doc_freq_ready = 1;
}

/* ============================================================
 * API publique
 * ============================================================ */

void rag_index_init(RagIndex *idx)
{
    if (!idx) return;
    memset(idx, 0, sizeof(RagIndex));
    idx->embed_dim = RAG_EMBED_DIM;
    idx->avg_chunk_len = 1.0f;
    idx->embeddings_normalized = 0;
}

static int texts_nearly_equal(const char *a, const char *b)
{
    if (!a || !b) return 0;
    /* Compare les 120 premiers caractères non-espace */
    int matched = 0, checked = 0;
    while (*a && *b && checked < 120) {
        while (*a && isspace((unsigned char)*a)) a++;
        while (*b && isspace((unsigned char)*b)) b++;
        if (!*a || !*b) break;
        if (tolower((unsigned char)*a) == tolower((unsigned char)*b)) matched++;
        a++; b++; checked++;
    }
    return checked > 40 && matched * 100 / checked > 92;
}

static int add_chunk(RagIndex *idx, const char *text, const char *source)
{
    if (idx->count >= RAG_MAX_CHUNKS) return -1;

    /* Déduplication simple contre les derniers chunks (même source) */
    for (int i = idx->count - 1; i >= 0 && i >= idx->count - 8; i--) {
        if (strcmp(idx->chunks[i].source, source) == 0 &&
            texts_nearly_equal(idx->chunks[i].text, text))
            return 0; /* skip duplicate */
    }

    RagChunk *c = &idx->chunks[idx->count];
    memset(c, 0, sizeof(RagChunk));

    strncpy(c->text, text, RAG_CHUNK_SIZE);
    c->text[RAG_CHUNK_SIZE] = '\0';

    strncpy(c->source, source, RAG_MAX_PATH - 1);
    c->source[RAG_MAX_PATH - 1] = '\0';

    c->has_embedding = 0;
    c->token_count = count_tokens(c->text);
    idx->count++;
    return 0;
}

/* Découpe un texte en chunks avec overlap, en préférant les frontières de paragraphe/phrase */
static void chunk_text(RagIndex *idx, const char *text, const char *source)
{
    size_t len = strlen(text);
    size_t start = 0;

    while (start < len && idx->count < RAG_MAX_CHUNKS) {
        size_t end = start + RAG_CHUNK_SIZE;
        if (end > len) end = len;

        if (end < len) {
            size_t best = end;
            /* Priorité 1 : double saut de ligne (paragraphe) */
            for (size_t i = end; i > start + RAG_CHUNK_SIZE / 3; i--) {
                if (text[i] == '\n' && i > 0 && text[i - 1] == '\n') {
                    best = i + 1;
                    break;
                }
            }
            if (best == end) {
                /* Priorité 2 : fin de phrase */
                for (size_t i = end; i > start + RAG_CHUNK_SIZE / 2; i--) {
                    if (text[i] == '.' || text[i] == '!' || text[i] == '?' || text[i] == '\n') {
                        best = i + 1;
                        break;
                    }
                }
            }
            if (best == end) {
                /* Priorité 3 : espace */
                for (size_t i = end; i > start + RAG_CHUNK_SIZE / 2; i--) {
                    if (text[i] == ' ') {
                        best = i + 1;
                        break;
                    }
                }
            }
            end = best;
        }
        /* Recule si on coupe au milieu d'un caractère UTF-8 (octet de continuation 10xxxxxx). */
        while (end > start + 1 && end < len && ((unsigned char)text[end] & 0xC0) == 0x80) end--;

        char buf[RAG_CHUNK_SIZE + 64];
        size_t clen = end - start;
        if (clen > RAG_CHUNK_SIZE) clen = RAG_CHUNK_SIZE;
        memcpy(buf, text + start, clen);
        buf[clen] = '\0';

        int meaningful = 0;
        for (size_t i = 0; i < clen; i++) {
            if (!isspace((unsigned char)buf[i])) { meaningful = 1; break; }
        }
        if (meaningful) {
            add_chunk(idx, buf, source);
        }

        if (end >= len) break;
        start = (end > RAG_CHUNK_OVERLAP) ? end - RAG_CHUNK_OVERLAP : end;
        while (start < end && ((unsigned char)text[start] & 0xC0) == 0x80) start++; /* overlap : début de caractère */
    }
}

static int index_file(RagIndex *idx, const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 12 * 1024 * 1024) { /* limite 12 Mo par fichier */
        fclose(f);
        return -1;
    }

    char *content = (char *)malloc((size_t)sz + 1);
    if (!content) {
        fclose(f);
        return -1;
    }

    size_t read = fread(content, 1, (size_t)sz, f);
    fclose(f);
    content[read] = '\0';

    const char *name = filepath;
    const char *p = strrchr(filepath, '/');
#ifdef _WIN32
    const char *p2 = strrchr(filepath, '\\');
    if (p2 && (!p || p2 > p)) p = p2;
#endif
    if (p) name = p + 1;

    (void)name;
    chunk_text(idx, content, filepath); /* chemin complet : distingue deux fichiers de même nom */
    free(content);
    return 0;
}

#ifdef _WIN32
static int index_folder_win(RagIndex *idx, const char *folder_path, int recursive)
{
    char pattern[RAG_MAX_PATH + 16];
    snprintf(pattern, sizeof(pattern), "%s\\*", folder_path);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;

        char full[RAG_MAX_PATH];
        int fn = snprintf(full, sizeof(full), "%s\\%s", folder_path, fd.cFileName);
        if (fn < 0 || (size_t)fn >= sizeof(full)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue; /* jonctions/symlinks */

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recursive) index_folder_win(idx, full, 1);
        } else if (is_text_file(fd.cFileName)) {
            index_file(idx, full);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}
#else
static int index_folder_unix(RagIndex *idx, const char *folder_path, int recursive)
{
    DIR *dir = opendir(folder_path);
    if (!dir) return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char full[RAG_MAX_PATH];
        int fn = snprintf(full, sizeof(full), "%s/%s", folder_path, ent->d_name);
        if (fn < 0 || (size_t)fn >= sizeof(full)) continue; /* chemin trop long : on ignore plutôt que d'ouvrir un chemin tronqué */

        struct stat st;
        if (lstat(full, &st) != 0) continue; /* lstat : les liens symboliques sont ignorés (évite les boucles) */

        if (S_ISDIR(st.st_mode)) {
            if (recursive) index_folder_unix(idx, full, 1);
        } else if (S_ISREG(st.st_mode) && is_text_file(ent->d_name)) {
            index_file(idx, full);
        }
    }
    closedir(dir);
    return 0;
}
#endif

int rag_index_folder(RagIndex *idx, const char *folder_path, int recursive)
{
    if (!idx || !folder_path) return -1;

    int before = idx->count;

#ifdef _WIN32
    if (index_folder_win(idx, folder_path, recursive) != 0) return -1;
#else
    if (index_folder_unix(idx, folder_path, recursive) != 0) return -1;
#endif

    recompute_corpus_stats(idx);
    if (idx->count >= RAG_MAX_CHUNKS)
        fprintf(stderr, "[rag] index plein (%d chunks) : les documents restants sont ignorés. "
                        "Augmentez RAG_MAX_CHUNKS dans rag.h.\n", RAG_MAX_CHUNKS);
    return idx->count - before;
}

/* Format de sauvegarde :
 * - Header : "LUMRAG04" + count + embed_dim + avg_chunk_len + embeddings_normalized
 * - Pour chaque chunk : source + text + has_embedding + token_count + embedding[]
 * Lecture rétro-compatible LUMRAG02 / LUMRAG03.
 */
int rag_index_save(const RagIndex *idx, const char *path)
{
    if (!idx || !path || idx->count < 0 || idx->count > RAG_MAX_CHUNKS ||
        idx->embed_dim <= 0 || idx->embed_dim > RAG_EMBED_DIM) return -1;

    char tmp_path[RAG_MAX_PATH + 8];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path)) return -1;
    FILE *f = fopen(tmp_path, "wb"); /* écriture atomique : un crash ne corrompt pas l'index existant */
    if (!f) return -1;

    const char magic[8] = "LUMRAG04";
    uint32_t count = (uint32_t)idx->count;
    uint32_t dim = (uint32_t)idx->embed_dim;
    float avgdl = idx->avg_chunk_len;
    uint8_t norm = idx->embeddings_normalized ? 1u : 0u;
    int ok = 1;
    ok &= fwrite(magic, 1, sizeof(magic), f) == sizeof(magic);
    ok &= fwrite(&count, sizeof(count), 1, f) == 1;
    ok &= fwrite(&dim, sizeof(dim), 1, f) == 1;
    ok &= fwrite(&avgdl, sizeof(avgdl), 1, f) == 1;
    ok &= fwrite(&norm, sizeof(norm), 1, f) == 1;

    for (uint32_t i = 0; ok && i < count; i++) {
        const RagChunk *c = &idx->chunks[i];
        size_t slen0 = strlen(c->source), tlen0 = strlen(c->text);
        if (slen0 >= RAG_MAX_PATH || tlen0 > RAG_CHUNK_SIZE + 63) { ok = 0; break; }
        uint32_t slen = (uint32_t)slen0;
        uint32_t tlen = (uint32_t)tlen0;
        uint8_t has = c->has_embedding ? 1u : 0u;
        int32_t tc = (int32_t)c->token_count;
        ok &= fwrite(&slen, sizeof(slen), 1, f) == 1;
        ok &= fwrite(c->source, 1, slen, f) == slen;
        ok &= fwrite(&tlen, sizeof(tlen), 1, f) == 1;
        ok &= fwrite(c->text, 1, tlen, f) == tlen;
        ok &= fwrite(&has, sizeof(has), 1, f) == 1;
        ok &= fwrite(&tc, sizeof(tc), 1, f) == 1;
        if (has) ok &= fwrite(c->embedding, sizeof(float), dim, f) == dim;
    }

    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp_path); return -1; }
#ifdef _WIN32
    if (!MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING)) { remove(tmp_path); return -1; }
#else
    if (rename(tmp_path, path) != 0) { remove(tmp_path); return -1; }
#endif
    return 0;
}

static int rag_index_load_impl(RagIndex *idx, const char *path)
{
    if (!idx || !path) return -1;
    rag_index_init(idx);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char magic[8];
    uint32_t count = 0, dim = 0;
    float avgdl = 1.0f;
    uint8_t norm = 0;
    int ok = fread(magic, 1, sizeof(magic), f) == sizeof(magic);

    /* Compatibilité LUMRAG02 → LUMRAG04 */
    int version = 0;
    if (ok && memcmp(magic, "LUMRAG04", 8) == 0) version = 4;
    else if (ok && memcmp(magic, "LUMRAG03", 8) == 0) version = 3;
    else if (ok && memcmp(magic, "LUMRAG02", 8) == 0) version = 2;
    else { fclose(f); return -1; }

    ok = ok && fread(&count, sizeof(count), 1, f) == 1;
    ok = ok && fread(&dim, sizeof(dim), 1, f) == 1;
    if (version >= 3) {
        ok = ok && fread(&avgdl, sizeof(avgdl), 1, f) == 1;
    }
    if (version >= 4) {
        ok = ok && fread(&norm, sizeof(norm), 1, f) == 1;
    }
    if (!ok || count > RAG_MAX_CHUNKS || dim == 0 || dim > RAG_EMBED_DIM) {
        fclose(f); return -1;
    }
    idx->embed_dim = (int)dim;
    idx->avg_chunk_len = avgdl > 0.0f ? avgdl : 1.0f;
    idx->embeddings_normalized = norm ? 1 : 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t slen = 0, tlen = 0; uint8_t has = 0;
        if (fread(&slen, sizeof(slen), 1, f) != 1 || slen >= RAG_MAX_PATH ||
            fread(idx->chunks[i].source, 1, slen, f) != slen) { fclose(f); return -1; }
        idx->chunks[i].source[slen] = '\0';
        if (fread(&tlen, sizeof(tlen), 1, f) != 1 || tlen > RAG_CHUNK_SIZE + 63 ||
            fread(idx->chunks[i].text, 1, tlen, f) != tlen) { fclose(f); return -1; }
        idx->chunks[i].text[tlen] = '\0';
        if (fread(&has, sizeof(has), 1, f) != 1 || has > 1) { fclose(f); return -1; }
        idx->chunks[i].has_embedding = has;

        if (version >= 3) {
            int32_t tc = 0;
            if (fread(&tc, sizeof(tc), 1, f) != 1) { fclose(f); return -1; }
            idx->chunks[i].token_count = tc > 0 ? tc : count_tokens(idx->chunks[i].text);
        } else {
            idx->chunks[i].token_count = count_tokens(idx->chunks[i].text);
        }

        if (has && fread(idx->chunks[i].embedding, sizeof(float), dim, f) != dim) {
            fclose(f); return -1;
        }
        idx->count++;
    }
    fclose(f);
    recompute_corpus_stats(idx);
    return 0;
}

int rag_index_load(RagIndex *idx, const char *path)
{
    int rc = rag_index_load_impl(idx, path);
    if (rc != 0 && idx) rag_index_init(idx); /* échec : index vide cohérent, pas de chunks à moitié lus */
    return rc;
}

int rag_index_compute_embeddings(RagIndex *idx, RagEmbedFn embed_fn, void *userdata)
{
    if (!idx || !embed_fn) return -1;
    int done = 0;
    int all_norm = 1;
    for (int i = 0; i < idx->count; i++) {
        if (embed_fn(idx->chunks[i].text, idx->chunks[i].embedding,
                     idx->embed_dim, userdata) == 0) {
            /* Pré-normalisation L2 → cosinus = produit scalaire à la recherche */
            float n = rag_l2_normalize(idx->chunks[i].embedding, idx->embed_dim);
            if (n <= 0.0f) {
                idx->chunks[i].has_embedding = 0;
                all_norm = 0;
            } else {
                idx->chunks[i].has_embedding = 1;
                done++;
            }
        } else {
            idx->chunks[i].has_embedding = 0;
            all_norm = 0;
        }
    }
    idx->embeddings_normalized = (done > 0 && all_norm && done == idx->count) ? 1 : 0;
    return done;
}

int rag_search(const RagIndex *idx, const char *query,
               int top_k, int *out_indices, float *out_scores,
               RagEmbedFn embed_fn, void *userdata)
{
    if (!idx || !query || !out_indices || top_k <= 0) return 0;
    if (idx->count == 0) return 0;
    if (top_k > idx->count) top_k = idx->count;

    /* Tokenise la requête */
    char tokbuf[RAG_MAX_QTOKENS][64];
    const char *qtokens[RAG_MAX_QTOKENS];
    int nq = tokenize_query(query, tokbuf, qtokens, RAG_MAX_QTOKENS);

    /* Embedding de la requête (si possible) */
    float query_emb[RAG_EMBED_DIM];
    int use_embed = 0;
    int use_dot   = 0; /* 1 si embeddings pré-normalisés → cos = dot */
    if (embed_fn) {
        int all_have = 1;
        for (int i = 0; i < idx->count; i++) {
            if (!idx->chunks[i].has_embedding) { all_have = 0; break; }
        }
        if (all_have && embed_fn(query, query_emb, idx->embed_dim, userdata) == 0) {
            use_embed = 1;
            if (idx->embeddings_normalized) {
                /* Même espace unitaire que les chunks */
                if (rag_l2_normalize(query_emb, idx->embed_dim) > 0.0f)
                    use_dot = 1;
            }
        }
    }

    /* Document frequency pour BM25 */
    int df[RAG_MAX_QTOKENS];
    if (nq > 0) compute_df(idx, qtokens, nq, df);

    float avgdl = idx->avg_chunk_len > 0.0f ? idx->avg_chunk_len : 1.0f;
    int N = idx->count;

    typedef struct { int idx; float score; } Scored;
    Scored *scores = (Scored *)malloc(sizeof(Scored) * (size_t)idx->count);
    if (!scores) return 0;

    /* Tableaux temporaires pour normalisation min-max */
    float *raw_bm25 = (float *)malloc(sizeof(float) * (size_t)idx->count);
    float *raw_cos  = (float *)malloc(sizeof(float) * (size_t)idx->count);
    if (!raw_bm25 || !raw_cos) {
        free(raw_bm25); free(raw_cos); free(scores);
        return 0;
    }

    float max_bm25 = 0.0f;
    float max_cos  = 0.0f;

    for (int i = 0; i < idx->count; i++) {
        scores[i].idx = i;
        float bm25 = 0.0f;
        if (nq > 0) {
            bm25 = bm25_score_with_df(idx->chunks[i].text,
                                      idx->chunks[i].token_count,
                                      qtokens, nq, df, avgdl, N);
        }
        float cos = 0.0f;
        if (use_embed) {
            if (use_dot) {
                /* Vecteurs unitaires : cosinus = produit scalaire (chemin rapide) */
                cos = rag_dot_product(query_emb, idx->chunks[i].embedding, idx->embed_dim);
            } else {
                cos = rag_cosine_similarity(query_emb, idx->chunks[i].embedding, idx->embed_dim);
            }
            if (cos < 0.0f) cos = 0.0f;
        }
        raw_bm25[i] = bm25;
        raw_cos[i]  = cos;
        if (bm25 > max_bm25) max_bm25 = bm25;
        if (cos  > max_cos)  max_cos  = cos;
    }

    for (int i = 0; i < idx->count; i++) {
        float n_bm25 = (max_bm25 > 1e-6f) ? (raw_bm25[i] / max_bm25) : 0.0f;
        float n_cos  = (max_cos  > 1e-6f) ? (raw_cos[i]  / max_cos)  : raw_cos[i];

        if (use_embed) {
            scores[i].score = RAG_HYBRID_ALPHA * n_cos + (1.0f - RAG_HYBRID_ALPHA) * n_bm25;
        } else {
            scores[i].score = n_bm25;
        }
    }

    free(raw_bm25);
    free(raw_cos);

    /* Sélection top-k (partial selection sort) */
    for (int i = 0; i < top_k; i++) {
        int best = i;
        for (int j = i + 1; j < idx->count; j++) {
            if (scores[j].score > scores[best].score) best = j;
        }
        if (best != i) {
            Scored tmp = scores[i];
            scores[i] = scores[best];
            scores[best] = tmp;
        }
    }

    int n = 0;
    for (int i = 0; i < top_k; i++) {
        if (scores[i].score < RAG_MIN_SCORE) break;
        out_indices[n] = scores[i].idx;
        if (out_scores) out_scores[n] = scores[i].score;
        n++;
    }

    free(scores);
    return n;
}

void rag_build_context(const RagIndex *idx, const int *indices, int n,
                       char *buf, size_t buf_size)
{
    if (!idx || !indices || !buf || buf_size == 0) return;
    buf[0] = '\0';
    size_t used = 0;

    for (int i = 0; i < n; i++) {
        int ci = indices[i];
        if (ci < 0 || ci >= idx->count) continue;

        int written = snprintf(buf + used, buf_size - used,
                               "[Document %d — %s]\n%s\n\n",
                               i + 1,
                               idx->chunks[ci].source,
                               idx->chunks[ci].text);
        if (written < 0 || (size_t)written >= buf_size - used) break;
        used += (size_t)written;
    }
    /* Un document indexé ne doit pas pouvoir injecter <|im_start|>/<|im_end|> dans le prompt. */
    for (size_t i = 0; buf[i]; i++)
        if (buf[i] == '<' && buf[i + 1] == '|') buf[i] = '(';
}
