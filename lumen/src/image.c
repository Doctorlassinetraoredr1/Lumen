#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#ifdef _WIN32
/* Échappe un argument selon les règles de découpage de ligne de commande
 * Windows (celles utilisées par CommandLineToArgvW / le CRT du processus
 * cible), puis l'entoure de guillemets.
 *
 * Sans cela, un prompt utilisateur contenant un simple '"' pouvait rompre
 * les frontières d'arguments de la ligne de commande construite par
 * snprintf et permettre d'injecter des options supplémentaires au binaire
 * sd (ex: prompt = "x\" -o C:\\autre\\fichier --steps 1 \"") : une forme
 * d'injection d'arguments, même sans passer par un interpréteur shell. */
static int win_append_quoted_arg(char *out, size_t cap, size_t *used, const char *arg) {
    if (*used >= cap) return -1;
    out[(*used)++] = '"';
    for (const char *p = arg; *p; ++p) {
        size_t backslashes = 0;
        while (*p == '\\') { ++backslashes; ++p; }
        if (*p == '"' || *p == '\0') {
            /* Les backslashes précédant un guillemet (ou la fin) doivent
             * être doublés, puis on échappe le guillemet lui-même. */
            for (size_t i = 0; i < backslashes * 2; i++) {
                if (*used >= cap) return -1;
                out[(*used)++] = '\\';
            }
            if (*p == '\0') break;
            if (*used + 2 > cap) return -1;
            out[(*used)++] = '\\';
            out[(*used)++] = '"';
        } else {
            for (size_t i = 0; i < backslashes; i++) {
                if (*used >= cap) return -1;
                out[(*used)++] = '\\';
            }
            if (*used >= cap) return -1;
            out[(*used)++] = *p;
        }
    }
    if (*used + 2 > cap) return -1;
    out[(*used)++] = '"';
    out[(*used)] = '\0';
    return 0;
}
#endif

int image_generate(const char *sd_binary_path, const char *model_path,
                   const char *prompt, const char *output_path) {
    if (!sd_binary_path || !model_path || !prompt || !output_path || !prompt[0]) return -1;
    if (access(sd_binary_path, F_OK) != 0) return -1;
#ifdef _WIN32
    char cmd[8192];
    size_t used = 0;
    if (win_append_quoted_arg(cmd, sizeof(cmd), &used, sd_binary_path) != 0) return -1;
    static const char *flags[] = { " -m", " -p", " -o" };
    const char *values[] = { model_path, prompt, output_path };
    for (int i = 0; i < 3; i++) {
        if (used + strlen(flags[i]) + 1 >= sizeof(cmd)) return -1;
        memcpy(cmd + used, flags[i], strlen(flags[i])); used += strlen(flags[i]);
        cmd[used++] = ' ';
        if (win_append_quoted_arg(cmd, sizeof(cmd), &used, values[i]) != 0) return -1;
    }
    static const char *tail = " --steps 20";
    if (used + strlen(tail) + 1 >= sizeof(cmd)) return -1;
    memcpy(cmd + used, tail, strlen(tail)); used += strlen(tail); cmd[used] = '\0';

    STARTUPINFOA si; PROCESS_INFORMATION pi; ZeroMemory(&si, sizeof(si)); ZeroMemory(&pi, sizeof(pi)); si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE); DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return code == 0 ? 0 : -1;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(sd_binary_path, sd_binary_path, "-m", model_path, "-p", prompt, "-o", output_path, "--steps", "20", (char *)NULL);
        _exit(127);
    }
    int status = 0; if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
#endif
}
