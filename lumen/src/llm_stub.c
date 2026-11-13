#include "llm.h"
#include <stddef.h>
struct LlmContext { int disabled; };
LlmContext *llm_load(const char *model_path, int n_ctx, int n_gpu_layers){(void)model_path;(void)n_ctx;(void)n_gpu_layers;return NULL;}
int llm_generate(LlmContext *ctx,const char *prompt,char *out,size_t cap,int max_tokens){(void)ctx;(void)prompt;(void)out;(void)cap;(void)max_tokens;return -1;}
int llm_generate_stream(LlmContext *ctx,const char *prompt,int max_tokens,LlmTokenCallback cb,void *userdata){(void)ctx;(void)prompt;(void)max_tokens;(void)cb;(void)userdata;return -1;}
void llm_free(LlmContext *ctx){(void)ctx;}
