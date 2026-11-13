#ifndef LLM_H
#define LLM_H

#include <stddef.h>

typedef struct LlmContext LlmContext;
typedef int (*LlmTokenCallback)(const char *piece, size_t len, void *userdata);

LlmContext *llm_load(const char *model_path, int n_ctx, int n_gpu_layers);
int llm_generate(LlmContext *ctx, const char *prompt, char *out_buf, size_t out_size, int max_tokens);
int llm_generate_stream(LlmContext *ctx, const char *prompt, int max_tokens, LlmTokenCallback cb, void *userdata);
void llm_free(LlmContext *ctx);

#endif
