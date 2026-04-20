/* ==============================================================================
 * sindarin-pkg-curl/curl.sn.c — HTTP client implementation using libcurl
 * ==============================================================================
 * Implements CurlClient and CurlResponse via the libcurl easy interface.
 * Each request creates a fresh easy handle; CurlClient holds persistent
 * options (custom headers, timeout) in a heap-allocated internal struct.
 * ============================================================================== */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
    #define strncasecmp _strnicmp
#endif

#define CURL_STATICLIB
#include <curl/curl.h>

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

typedef __sn__CurlResponse RtCurlResponse;
typedef __sn__CurlClient   RtCurlClient;

/* ============================================================================
 * Global Init (one-time, not thread-safe at init time — init early)
 * ============================================================================ */

static int curl_initialized = 0;

static void ensure_curl_initialized(void)
{
    if (!curl_initialized) {
        CURLcode rc = curl_global_init(CURL_GLOBAL_ALL);
        if (rc != CURLE_OK) {
            fprintf(stderr, "curl_global_init failed: %s\n", curl_easy_strerror(rc));
            exit(1);
        }
        curl_initialized = 1;
    }
}

/* ============================================================================
 * Growable Buffer (write/header callbacks)
 * ============================================================================ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} CurlBuffer;

static void curl_buffer_init(CurlBuffer *b)
{
    b->data = (char *)malloc(4096);
    b->len  = 0;
    b->cap  = 4096;
    if (b->data) b->data[0] = '\0';
}

static void curl_buffer_free(CurlBuffer *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t     bytes = size * nmemb;
    CurlBuffer *b    = (CurlBuffer *)userdata;

    if (b->len + bytes + 1 > b->cap) {
        size_t new_cap  = b->cap * 2 + bytes;
        char  *new_data = (char *)realloc(b->data, new_cap);
        if (!new_data) {
            fprintf(stderr, "curl_write_cb: realloc failed\n");
            return 0;
        }
        b->data = new_data;
        b->cap  = new_cap;
    }

    memcpy(b->data + b->len, ptr, bytes);
    b->len        += bytes;
    b->data[b->len] = '\0';
    return bytes;
}

/* ============================================================================
 * CurlClientInternal — per-client persistent state
 * ============================================================================ */

typedef struct {
    struct curl_slist *headers;    /* custom headers for every request */
    long               timeout_ms; /* 0 = no timeout */
} CurlClientInternal;

/* ============================================================================
 * CurlResponse helpers
 * ============================================================================ */

static RtCurlResponse *make_response(int status, CurlBuffer *body, CurlBuffer *hdrs)
{
    RtCurlResponse *r = __sn__CurlResponse__new();
    r->status_code = (long long)status;
    r->body_str    = strdup(body->data ? body->data : "");
    r->headers_str = strdup(hdrs->data ? hdrs->data : "");
    return r;
}

long long sn_curl_response_status(RtCurlResponse *r)
{
    if (!r) return 0;
    return r->status_code;
}

char *sn_curl_response_body(RtCurlResponse *r)
{
    if (!r || !r->body_str) return strdup("");
    return strdup((char *)r->body_str);
}

char *sn_curl_response_headers(RtCurlResponse *r)
{
    if (!r || !r->headers_str) return strdup("");
    return strdup((char *)r->headers_str);
}

/* Linear scan through raw headers for "Name: value\r\n" (case-insensitive). */
char *sn_curl_response_header(RtCurlResponse *r, char *name)
{
    if (!r || !r->headers_str || !name) return strdup("");

    const char *haystack = (const char *)r->headers_str;
    size_t      name_len = strlen(name);
    const char *p        = haystack;

    while (*p) {
        if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *val = p + name_len + 1;
            while (*val == ' ' || *val == '\t') val++;
            const char *end = val;
            while (*end && *end != '\r' && *end != '\n') end++;
            size_t len    = (size_t)(end - val);
            char  *result = (char *)malloc(len + 1);
            if (!result) return strdup("");
            memcpy(result, val, len);
            result[len] = '\0';
            return result;
        }
        /* Advance to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return strdup("");
}

void sn_curl_response_dispose(RtCurlResponse *r)
{
    if (!r) return;
    free(r->body_str);
    free(r->headers_str);
    r->body_str    = NULL;
    r->headers_str = NULL;
}

/* ============================================================================
 * Header list builder — merges content-type + client custom headers
 * ============================================================================ */

static struct curl_slist *build_headers(CurlClientInternal *internal,
                                        const char         *content_type)
{
    struct curl_slist *list = NULL;

    if (content_type && content_type[0]) {
        size_t len = strlen("Content-Type: ") + strlen(content_type) + 1;
        char  *hdr = (char *)malloc(len);
        if (!hdr) { fprintf(stderr, "build_headers: malloc failed\n"); exit(1); }
        snprintf(hdr, len, "Content-Type: %s", content_type);
        list = curl_slist_append(list, hdr);
        free(hdr);
    }

    if (internal && internal->headers) {
        struct curl_slist *p = internal->headers;
        while (p) {
            list = curl_slist_append(list, p->data);
            p = p->next;
        }
    }

    return list;
}

/* ============================================================================
 * Internal: init easy handle, apply client options, perform, return response
 * ============================================================================ */

static RtCurlResponse *do_request(CurlClientInternal *internal,
                                  const char         *url,
                                  const char         *method,
                                  const char         *content_type,
                                  const char         *body_str)
{
    CURL *easy = curl_easy_init();
    if (!easy) {
        fprintf(stderr, "curl_easy_init failed\n");
        exit(1);
    }

    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "sindarin-pkg-curl/1.0");

    if (internal && internal->timeout_ms > 0)
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, internal->timeout_ms);

    /* Method + body */
    if (strcmp(method, "GET") == 0) {
        curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body_str ? body_str : "");
        if (body_str)
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(body_str));
    } else {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);
        if (body_str && body_str[0]) {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body_str);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(body_str));
        }
    }

    /* Headers */
    struct curl_slist *hdr_list = build_headers(internal, content_type);
    if (hdr_list)
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdr_list);

    /* Capture response */
    CurlBuffer resp_body, resp_hdrs;
    curl_buffer_init(&resp_body);
    curl_buffer_init(&resp_hdrs);

    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA,     &resp_body);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, curl_write_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA,    &resp_hdrs);

    CURLcode rc = curl_easy_perform(easy);
    if (rc != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform failed: %s\n", curl_easy_strerror(rc));
        curl_buffer_free(&resp_body);
        curl_buffer_free(&resp_hdrs);
        curl_easy_cleanup(easy);
        if (hdr_list) curl_slist_free_all(hdr_list);
        exit(1);
    }

    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);

    RtCurlResponse *resp = make_response((int)status, &resp_body, &resp_hdrs);

    curl_buffer_free(&resp_body);
    curl_buffer_free(&resp_hdrs);
    curl_easy_cleanup(easy);
    if (hdr_list) curl_slist_free_all(hdr_list);

    return resp;
}

/* ============================================================================
 * CurlClient
 * ============================================================================ */

RtCurlClient *sn_curl_client_new(void)
{
    ensure_curl_initialized();

    RtCurlClient *c = __sn__CurlClient__new();

    CurlClientInternal *internal = (CurlClientInternal *)calloc(1, sizeof(CurlClientInternal));
    if (!internal) {
        fprintf(stderr, "sn_curl_client_new: internal allocation failed\n");
        exit(1);
    }

    c->internal = (long long)(uintptr_t)internal;
    return c;
}

void sn_curl_client_set_timeout(RtCurlClient *client, long long ms)
{
    if (!client) return;
    CurlClientInternal *internal = (CurlClientInternal *)(uintptr_t)client->internal;
    if (internal) internal->timeout_ms = (long)ms;
}

void sn_curl_client_set_header(RtCurlClient *client, char *name, char *value)
{
    if (!client || !name || !value) return;
    CurlClientInternal *internal = (CurlClientInternal *)(uintptr_t)client->internal;
    if (!internal) return;

    size_t len = strlen(name) + 2 + strlen(value) + 1;
    char  *hdr = (char *)malloc(len);
    if (!hdr) { fprintf(stderr, "sn_curl_client_set_header: malloc failed\n"); exit(1); }
    snprintf(hdr, len, "%s: %s", name, value);
    internal->headers = curl_slist_append(internal->headers, hdr);
    free(hdr);
}

RtCurlResponse *sn_curl_client_get(RtCurlClient *client, char *url)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request(internal, url, "GET", NULL, NULL);
}

RtCurlResponse *sn_curl_client_post(RtCurlClient *client, char *url,
                                    char *content_type, char *body)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request(internal, url, "POST", content_type, body);
}

RtCurlResponse *sn_curl_client_put(RtCurlClient *client, char *url,
                                   char *content_type, char *body)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request(internal, url, "PUT", content_type, body);
}

RtCurlResponse *sn_curl_client_patch(RtCurlClient *client, char *url,
                                     char *content_type, char *body)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request(internal, url, "PATCH", content_type, body);
}

RtCurlResponse *sn_curl_client_delete(RtCurlClient *client, char *url)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request(internal, url, "DELETE", NULL, NULL);
}

void sn_curl_client_dispose(RtCurlClient *client)
{
    if (!client) return;
    CurlClientInternal *internal = (CurlClientInternal *)(uintptr_t)client->internal;
    if (internal) {
        if (internal->headers) curl_slist_free_all(internal->headers);
        free(internal);
    }
    client->internal = 0;
}
