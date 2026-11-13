#ifndef LUMEN_SESSION_H
#define LUMEN_SESSION_H
#include <stddef.h>
#include <time.h>
#define LUMEN_SESSION_ID_LEN 96
#define LUMEN_SESSION_MSG_LEN 4096
#define LUMEN_SESSION_MAX_TURNS 12
#define LUMEN_SESSION_MAX 256
typedef struct { char role[16]; char content[LUMEN_SESSION_MSG_LEN]; } LumenSessionMessage;
typedef struct { int used; char id[LUMEN_SESSION_ID_LEN]; LumenSessionMessage messages[LUMEN_SESSION_MAX_TURNS]; int count; time_t last_used; } LumenSession;
void lumen_sessions_init(void);
LumenSession *lumen_session_get(const char *id, int create);
void lumen_session_add(LumenSession *session, const char *role, const char *content);
void lumen_session_build_prompt(const LumenSession *session, const char *system, const char *user, char *out, size_t out_size);
int lumen_session_reset(const char *id);
void lumen_sessions_clear(void);
/* Neutralise les balises de contrôle ChatML (<|...|>) dans un texte non fiable, en place. */
void lumen_sanitize_text(char *s);
#endif
