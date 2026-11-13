#include "session.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <pthread.h>
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&g_mutex)
#define UNLOCK() pthread_mutex_unlock(&g_mutex)
#else
#include <windows.h>
static CRITICAL_SECTION g_mutex;
static int g_mutex_ready = 0;
#define LOCK() do { if (!g_mutex_ready) { InitializeCriticalSection(&g_mutex); g_mutex_ready = 1; } EnterCriticalSection(&g_mutex); } while (0)
#define UNLOCK() LeaveCriticalSection(&g_mutex)
#endif
static LumenSession g_sessions[LUMEN_SESSION_MAX];
void lumen_sessions_init(void) { LOCK(); memset(g_sessions, 0, sizeof(g_sessions)); UNLOCK(); }
#define SESSION_EVICT_IDLE_SEC 300
/* Retourne la session (créée si besoin). Quand la table est pleine, la session la plus
 * ancienne inactive depuis plus de 5 min est recyclée ; sinon NULL (le serveur répond 429). */
LumenSession *lumen_session_get(const char *id, int create) {
    if (!id || !id[0]) return NULL;
    time_t now = time(NULL);
    LOCK();
    int free_slot = -1, oldest = -1;
    for (int i=0;i<LUMEN_SESSION_MAX;i++) {
        if (g_sessions[i].used) {
            if (strcmp(g_sessions[i].id,id)==0) { g_sessions[i].last_used=now; UNLOCK(); return &g_sessions[i]; }
            if (oldest<0 || g_sessions[i].last_used<g_sessions[oldest].last_used) oldest=i;
        } else if (free_slot<0) free_slot=i;
    }
    if (!create) { UNLOCK(); return NULL; }
    int slot = free_slot;
    if (slot<0 && oldest>=0 && now-g_sessions[oldest].last_used>=SESSION_EVICT_IDLE_SEC) slot=oldest;
    if (slot<0) { UNLOCK(); return NULL; }
    memset(&g_sessions[slot],0,sizeof(g_sessions[slot]));
    g_sessions[slot].used=1; g_sessions[slot].last_used=now;
    snprintf(g_sessions[slot].id,sizeof(g_sessions[slot].id),"%s",id);
    UNLOCK();
    return &g_sessions[slot];
}
void lumen_sanitize_text(char *s){
    if(!s) return;
    for(;*s;++s) if(s[0]=='<' && s[1]=='|') s[0]='(';
}
void lumen_session_add(LumenSession *s,const char *role,const char *content){
    if(!s||!role||!content) return;
    LOCK();
    if(s->count>=LUMEN_SESSION_MAX_TURNS){memmove(&s->messages[0],&s->messages[1],(LUMEN_SESSION_MAX_TURNS-1)*sizeof(s->messages[0]));s->count=LUMEN_SESSION_MAX_TURNS-1;}
    LumenSessionMessage *m=&s->messages[s->count++]; snprintf(m->role,sizeof(m->role),"%s",role); snprintf(m->content,sizeof(m->content),"%s",content); UNLOCK();
}
void lumen_session_build_prompt(const LumenSession *s,const char *system,const char *user,char *out,size_t cap){
    if(!out||cap==0) return;
    LOCK();
    size_t used=0; out[0]=0;
#define APP(...) do { if(used<cap){ int n=snprintf(out+used,cap-used,__VA_ARGS__); if(n<0){out[0]=0;UNLOCK();return;} if((size_t)n>=cap-used){used=cap-1;out[used]=0;}else used+=(size_t)n; } }while(0)
    APP("<|im_start|>system\n%s\n<|im_end|>\n",system?system:"Tu es Lumen, une IA généraliste autonome.");
    if(s) for(int i=0;i<s->count&&used<cap;i++) APP("<|im_start|>%s\n%s<|im_end|>\n",s->messages[i].role,s->messages[i].content);
    APP("<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",user?user:"");
#undef APP
    UNLOCK();
}
int lumen_session_reset(const char *id){ if(!id||!id[0])return -1; LOCK(); for(int i=0;i<LUMEN_SESSION_MAX;i++) if(g_sessions[i].used&&strcmp(g_sessions[i].id,id)==0){memset(&g_sessions[i],0,sizeof(g_sessions[i]));UNLOCK();return 1;} UNLOCK();return 0; }
void lumen_sessions_clear(void){ LOCK(); memset(g_sessions,0,sizeof(g_sessions)); UNLOCK(); }
