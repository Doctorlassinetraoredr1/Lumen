#include "search.h"
#include <curl/curl.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  *data;
    size_t size;
} MemBlock;

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemBlock *mem = (MemBlock *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "[search] realloc échoué (out of memory)\n");
        return 0;
    }
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

int search_global_init(void) {
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        fprintf(stderr, "[search] curl_global_init a échoué: %s\n", curl_easy_strerror(rc));
        return -1;
    }
    return 0;
}

void search_global_cleanup(void) {
    curl_global_cleanup();
}

static char *safe_strdup_field(const cJSON *obj, const char *field) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, field);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return "";
}

int search_web(const char *api_key, const char *query, SearchResults *out) {
    if (!api_key || !query || !out) {
        fprintf(stderr, "[search] paramètres invalides\n");
        return -1;
    }

    memset(out, 0, sizeof(SearchResults));

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[search] curl_easy_init a échoué\n");
        return -1;
    }

    /* Encodage URL de la requête */
    char *encoded_query = curl_easy_escape(curl, query, 0);
    if (!encoded_query) {
        curl_easy_cleanup(curl);
        return -1;
    }

    char url[1024];
    snprintf(url, sizeof(url),
             "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d",
             encoded_query, SEARCH_MAX_RESULTS);
    curl_free(encoded_query);

    MemBlock chunk = { .data = malloc(1), .size = 0 };
    if (!chunk.data) {
        curl_easy_cleanup(curl);
        return -1;
    }
    chunk.data[0] = '\0';

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "X-Subscription-Token: %s", api_key);
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Lumen-client/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); /* indispensable avec des threads */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    int ret = 0;

    if (res != CURLE_OK) {
        fprintf(stderr, "[search] erreur réseau: %s\n", curl_easy_strerror(res));
        ret = -1;
        goto cleanup;
    }

    if (http_code != 200) {
        fprintf(stderr, "[search] réponse HTTP inattendue: %ld\n", http_code);
        ret = -1;
        goto cleanup;
    }

    cJSON *json = cJSON_Parse(chunk.data);
    if (!json) {
        const char *err = cJSON_GetErrorPtr();
        fprintf(stderr, "[search] JSON invalide: %s\n", err ? err : "inconnu");
        ret = -1;
        goto cleanup;
    }

    cJSON *web = cJSON_GetObjectItemCaseSensitive(json, "web");
    cJSON *results = cJSON_GetObjectItemCaseSensitive(web, "results");

    if (cJSON_IsArray(results)) {
        cJSON *item = NULL;
        int i = 0;
        cJSON_ArrayForEach(item, results) {
            if (i >= SEARCH_MAX_RESULTS) break;

            const char *title = safe_strdup_field(item, "title");
            const char *urlf  = safe_strdup_field(item, "url");
            const char *desc  = safe_strdup_field(item, "description");

            snprintf(out->items[i].title, SEARCH_TITLE_LEN, "%s", title);
            snprintf(out->items[i].url, SEARCH_URL_LEN, "%s", urlf);
            snprintf(out->items[i].description, SEARCH_DESC_LEN, "%s", desc);

            i++;
        }
        out->count = i;
    }

    cJSON_Delete(json);

cleanup:
    free(chunk.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ret;
}

void search_build_context(const SearchResults *results, char *buf, size_t buf_size) {
    if (!results || !buf || buf_size == 0) return;

    buf[0] = '\0';
    size_t used = 0;

    for (int i = 0; i < results->count; i++) {
        int written = snprintf(buf + used, buf_size - used,
                                "[Source %d] %s\nURL: %s\n%s\n\n",
                                i + 1,
                                results->items[i].title,
                                results->items[i].url,
                                results->items[i].description);
        if (written < 0 || (size_t)written >= buf_size - used) {
            break; /* buffer plein, on tronque proprement */
        }
        used += (size_t)written;
    }
    /* Les extraits web sont non fiables : on neutralise les balises de rôle ChatML. */
    for (size_t i = 0; buf[i]; i++)
        if (buf[i] == '<' && buf[i + 1] == '|') buf[i] = '(';
}
