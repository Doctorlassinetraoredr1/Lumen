#include "llm.h"
#include "llama.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct LlmContext {
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
};

LlmContext *llm_load(const char *model_path, int n_ctx, int n_gpu_layers) {
    if (!model_path || !model_path[0]) return NULL;
    ggml_backend_load_all();
    LlmContext *lc = calloc(1, sizeof(*lc));
    if (!lc) return NULL;
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = n_gpu_layers;
    lc->model = llama_model_load_from_file(model_path, mp);
    if (!lc->model) { fprintf(stderr, "[llm] impossible de charger le modèle: %s\n", model_path); free(lc); return NULL; }
    lc->vocab = llama_model_get_vocab(lc->model);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = n_ctx > 0 ? (uint32_t)n_ctx : 4096u;
    cp.n_batch = cp.n_ctx < 512u ? cp.n_ctx : 512u; /* le prompt est décodé par paquets de n_batch */
    cp.n_ubatch = cp.n_batch;
    lc->ctx = llama_init_from_model(lc->model, cp);
    if (!lc->ctx) { fprintf(stderr, "[llm] impossible de créer le contexte\n"); llama_model_free(lc->model); free(lc); return NULL; }
    return lc;
}

static int llm_run(LlmContext *lc, const char *prompt, char *out_buf, size_t out_size,
                   int max_tokens, LlmTokenCallback cb, void *userdata) {
    if (!lc || !lc->ctx || !lc->vocab || !prompt || max_tokens <= 0) return -1;
    if (out_buf && out_size) out_buf[0] = '\0';
    llama_memory_clear(llama_get_memory(lc->ctx), true);
    size_t prompt_len = strlen(prompt);
    if (prompt_len > (size_t)INT32_MAX) return -1;
    int32_t capacity = (int32_t)(prompt_len + 32u);
    if (capacity < 256) capacity = 256;
    llama_token *tokens = malloc((size_t)capacity * sizeof(*tokens));
    if (!tokens) return -1;
    int32_t n_prompt = llama_tokenize(lc->vocab, prompt, (int32_t)prompt_len, tokens, capacity, true, true);
    if (n_prompt < 0) {
        int32_t required = -n_prompt;
        if (required <= 0) { free(tokens); return -1; }
        llama_token *tmp = realloc(tokens, (size_t)required * sizeof(*tokens));
        if (!tmp) { free(tokens); return -1; }
        tokens = tmp;
        n_prompt = llama_tokenize(lc->vocab, prompt, (int32_t)prompt_len, tokens, required, true, true);
    }
    if (n_prompt <= 0 || (uint32_t)n_prompt >= llama_n_ctx(lc->ctx)) { free(tokens); return -1; }
    /* Le prompt peut dépasser n_batch : on le décode par paquets (sinon llama_decode échoue). */
    const int32_t n_batch = (int32_t)llama_n_batch(lc->ctx);
    for (int32_t off = 0; off < n_prompt; off += n_batch) {
        int32_t n = n_prompt - off < n_batch ? n_prompt - off : n_batch;
        if (llama_decode(lc->ctx, llama_batch_get_one(tokens + off, n)) != 0) { free(tokens); return -1; }
    }

    /* Échantillonnage : top-k / top-p / température (LUMEN_TEMPERATURE, 0 = glouton). */
    float temp = 0.7f;
    const char *te = getenv("LUMEN_TEMPERATURE");
    if (te && te[0]) temp = (float)atof(te);
    struct llama_sampler *sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!sampler) { free(tokens); return -1; }
    if (temp > 0.0f) {
        llama_sampler_chain_add(sampler, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(sampler, llama_sampler_init_top_p(0.95f, 1));
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(temp));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
    } else {
        llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
    }

    const int32_t n_ctx = (int32_t)llama_n_ctx(lc->ctx);
    size_t written = 0;
    int generated = 0;
    for (int i = 0; i < max_tokens; ++i) {
        if (n_prompt + generated >= n_ctx) break; /* contexte plein */
        llama_token tok = llama_sampler_sample(sampler, lc->ctx, -1);
        if (llama_vocab_is_eog(lc->vocab, tok)) break;
        char piece[512];
        int piece_len = llama_token_to_piece(lc->vocab, tok, piece, (int)sizeof(piece), 0, true);
        if (piece_len < 0) break;
        if (out_buf && out_size) {
            if (written + (size_t)piece_len >= out_size) break;
            memcpy(out_buf + written, piece, (size_t)piece_len);
            written += (size_t)piece_len;
            out_buf[written] = '\0';
        }
        if (cb && cb(piece, (size_t)piece_len, userdata) != 0) break;
        ++generated;
        if (llama_decode(lc->ctx, llama_batch_get_one(&tok, 1)) != 0) break;
    }
    llama_sampler_free(sampler);
    free(tokens);
    return generated;
}

int llm_generate(LlmContext *lc, const char *prompt, char *out_buf, size_t out_size, int max_tokens) {
    if (!out_buf || out_size == 0) return -1;
    return llm_run(lc, prompt, out_buf, out_size, max_tokens, NULL, NULL);
}

int llm_generate_stream(LlmContext *lc, const char *prompt, int max_tokens,
                        LlmTokenCallback cb, void *userdata) {
    if (!cb) return -1;
    return llm_run(lc, prompt, NULL, 0, max_tokens, cb, userdata);
}

void llm_free(LlmContext *lc) {
    if (!lc) return;
    if (lc->ctx) llama_free(lc->ctx);
    if (lc->model) llama_model_free(lc->model);
    free(lc);
}
