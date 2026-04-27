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
 * Closure calling convention helpers
 *
 * Sindarin closure layout (from sn_core.h):
 *   [0                          ]  void *fn       — function pointer
 *   [sizeof(void *)             ]  size_t size    — total closure bytes
 *   [sizeof(void *)+sizeof(sz)  ]  void *cleanup  — cleanup function (or NULL)
 *   [remainder                  ]  captured variables
 * The closure pointer itself is always passed as the first argument.
 * ============================================================================ */

/* Call a fn(str): bool closure. Caller retains ownership of arg. */
static inline bool sn_closure_call_bool_str(void *closure, char *arg)
{
    typedef bool (*Fn)(void *, char *);
    return ((Fn)*(void **)closure)(closure, arg);
}

/* Call a fn(): str closure. Returned char * is heap-allocated (caller frees). */
static inline char *sn_closure_call_str(void *closure)
{
    typedef char *(*Fn)(void *);
    return ((Fn)*(void **)closure)(closure);
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
 * Streaming contexts
 * ============================================================================ */

typedef struct {
    void *on_chunk;   /* Sindarin closure: fn(str): bool — false aborts */
    int   aborted;
} StreamCtx;

static size_t curl_write_stream_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t     bytes = size * nmemb;
    StreamCtx *ctx   = (StreamCtx *)userdata;

    if (ctx->aborted) return 0;

    char *chunk = (char *)malloc(bytes + 1);
    if (!chunk) return 0;
    memcpy(chunk, ptr, bytes);
    chunk[bytes] = '\0';

    bool keep_going = sn_closure_call_bool_str(ctx->on_chunk, chunk);
    free(chunk);

    if (!keep_going) {
        ctx->aborted = 1;
        return 0;  /* libcurl treats short write as abort (CURLE_WRITE_ERROR) */
    }
    return bytes;
}

typedef struct {
    void   *produce;       /* Sindarin closure: fn(): str — "" signals EOF */
    char   *pending;       /* leftover chunk from last produce() call */
    size_t  pending_off;
    size_t  pending_len;
    int     eof;
} UploadCtx;

static size_t curl_read_stream_cb(char *buffer, size_t size, size_t nmemb, void *userdata)
{
    UploadCtx *ctx   = (UploadCtx *)userdata;
    size_t     want  = size * nmemb;
    size_t     total = 0;

    while (total < want) {
        if (ctx->pending && ctx->pending_off < ctx->pending_len) {
            size_t avail = ctx->pending_len - ctx->pending_off;
            size_t take  = avail < (want - total) ? avail : (want - total);
            memcpy(buffer + total, ctx->pending + ctx->pending_off, take);
            ctx->pending_off += take;
            total            += take;
            continue;
        }

        if (ctx->pending) {
            free(ctx->pending);
            ctx->pending     = NULL;
            ctx->pending_off = 0;
            ctx->pending_len = 0;
        }

        if (ctx->eof) break;

        char *chunk = sn_closure_call_str(ctx->produce);
        if (!chunk || chunk[0] == '\0') {
            if (chunk) free(chunk);
            ctx->eof = 1;
            break;
        }
        ctx->pending     = chunk;
        ctx->pending_len = strlen(chunk);
        ctx->pending_off = 0;
    }

    return total;
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
        /* Transport-layer failure (timeout, connection refused, DNS, TLS
         * handshake, etc). DO NOT exit(1) — the caller has a valid use
         * for gracefully degrading on a soft HTTP error (e.g. retry,
         * leave diagnostic NULL, circuit-break). Return a response with
         * status_code=0 and the curl error message as the body so the
         * caller can branch on status != 200 and inspect the reason.
         *
         * History: this path used to exit(1) on every perform error,
         * which meant a single Ollama timeout would hard-kill the whole
         * process even though the consuming code already had a
         * graceful-degradation branch ready. */
        const char *errmsg = curl_easy_strerror(rc);
        fprintf(stderr, "curl_easy_perform failed: %s\n", errmsg);

        RtCurlResponse *err_resp = __sn__CurlResponse__new();
        err_resp->status_code = 0;
        err_resp->body_str    = strdup(errmsg ? errmsg : "curl_easy_perform failed");
        err_resp->headers_str = strdup("");

        curl_buffer_free(&resp_body);
        curl_buffer_free(&resp_hdrs);
        curl_easy_cleanup(easy);
        if (hdr_list) curl_slist_free_all(hdr_list);
        return err_resp;
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
 * Streaming variants — body delivered to/produced by Sindarin closures.
 * Returned CurlResponse has status() + headers() populated; body() is empty.
 * ============================================================================ */

static RtCurlResponse *do_request_stream(CurlClientInternal *internal,
                                         const char         *url,
                                         const char         *method,
                                         const char         *content_type,
                                         const char         *body_str,
                                         void               *on_chunk_closure)
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

    struct curl_slist *hdr_list = build_headers(internal, content_type);
    if (hdr_list)
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdr_list);

    StreamCtx stream_ctx;
    stream_ctx.on_chunk = on_chunk_closure;
    stream_ctx.aborted  = 0;

    CurlBuffer resp_hdrs;
    curl_buffer_init(&resp_hdrs);

    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write_stream_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA,     &stream_ctx);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, curl_write_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA,    &resp_hdrs);

    CURLcode rc = curl_easy_perform(easy);

    /* CURLE_WRITE_ERROR is the expected result of a caller-requested abort —
     * surface a normal response (with whatever status libcurl already saw)
     * rather than treating the deliberate abort as a transport failure. */
    if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && stream_ctx.aborted)) {
        const char *errmsg = curl_easy_strerror(rc);
        fprintf(stderr, "curl_easy_perform failed: %s\n", errmsg);

        RtCurlResponse *err_resp = __sn__CurlResponse__new();
        err_resp->status_code = 0;
        err_resp->body_str    = strdup(errmsg ? errmsg : "curl_easy_perform failed");
        err_resp->headers_str = strdup("");

        curl_buffer_free(&resp_hdrs);
        curl_easy_cleanup(easy);
        if (hdr_list) curl_slist_free_all(hdr_list);
        return err_resp;
    }

    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);

    RtCurlResponse *resp = __sn__CurlResponse__new();
    resp->status_code = (long long)status;
    resp->body_str    = strdup("");
    resp->headers_str = strdup(resp_hdrs.data ? resp_hdrs.data : "");

    curl_buffer_free(&resp_hdrs);
    curl_easy_cleanup(easy);
    if (hdr_list) curl_slist_free_all(hdr_list);

    return resp;
}

static RtCurlResponse *do_request_upload_stream(CurlClientInternal *internal,
                                                const char         *url,
                                                const char         *method,
                                                const char         *content_type,
                                                void               *produce_closure)
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

    /* CURLOPT_UPLOAD turns this into a chunked PUT-shaped transfer; for
     * POST/PATCH we override with CUSTOMREQUEST below. Without an explicit
     * Content-Length, libcurl uses chunked transfer-encoding automatically. */
    curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);
    if (strcmp(method, "PUT") != 0)
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);

    UploadCtx upload_ctx;
    upload_ctx.produce     = produce_closure;
    upload_ctx.pending     = NULL;
    upload_ctx.pending_off = 0;
    upload_ctx.pending_len = 0;
    upload_ctx.eof         = 0;

    curl_easy_setopt(easy, CURLOPT_READFUNCTION, curl_read_stream_cb);
    curl_easy_setopt(easy, CURLOPT_READDATA,     &upload_ctx);

    struct curl_slist *hdr_list = build_headers(internal, content_type);
    if (hdr_list)
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdr_list);

    CurlBuffer resp_body, resp_hdrs;
    curl_buffer_init(&resp_body);
    curl_buffer_init(&resp_hdrs);

    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA,     &resp_body);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, curl_write_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA,    &resp_hdrs);

    CURLcode rc = curl_easy_perform(easy);

    if (upload_ctx.pending) free(upload_ctx.pending);

    if (rc != CURLE_OK) {
        const char *errmsg = curl_easy_strerror(rc);
        fprintf(stderr, "curl_easy_perform failed: %s\n", errmsg);

        RtCurlResponse *err_resp = __sn__CurlResponse__new();
        err_resp->status_code = 0;
        err_resp->body_str    = strdup(errmsg ? errmsg : "curl_easy_perform failed");
        err_resp->headers_str = strdup("");

        curl_buffer_free(&resp_body);
        curl_buffer_free(&resp_hdrs);
        curl_easy_cleanup(easy);
        if (hdr_list) curl_slist_free_all(hdr_list);
        return err_resp;
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

/* ---- Download streaming (response body delivered to onChunk) ---- */

RtCurlResponse *sn_curl_client_get_stream(RtCurlClient *client, char *url,
                                          void *on_chunk)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request_stream(internal, url, "GET", NULL, NULL, on_chunk);
}

RtCurlResponse *sn_curl_client_post_stream(RtCurlClient *client, char *url,
                                           char *content_type, char *body,
                                           void *on_chunk)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request_stream(internal, url, "POST", content_type, body, on_chunk);
}

RtCurlResponse *sn_curl_client_put_stream(RtCurlClient *client, char *url,
                                          char *content_type, char *body,
                                          void *on_chunk)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request_stream(internal, url, "PUT", content_type, body, on_chunk);
}

RtCurlResponse *sn_curl_client_patch_stream(RtCurlClient *client, char *url,
                                            char *content_type, char *body,
                                            void *on_chunk)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request_stream(internal, url, "PATCH", content_type, body, on_chunk);
}

RtCurlResponse *sn_curl_client_delete_stream(RtCurlClient *client, char *url,
                                             void *on_chunk)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request_stream(internal, url, "DELETE", NULL, NULL, on_chunk);
}

/* ---- Upload streaming (request body produced by produceChunk) ---- */

RtCurlResponse *sn_curl_client_post_chunked(RtCurlClient *client, char *url,
                                            char *content_type, void *produce)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request_upload_stream(internal, url, "POST", content_type, produce);
}

RtCurlResponse *sn_curl_client_put_chunked(RtCurlClient *client, char *url,
                                           char *content_type, void *produce)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request_upload_stream(internal, url, "PUT", content_type, produce);
}

RtCurlResponse *sn_curl_client_patch_chunked(RtCurlClient *client, char *url,
                                             char *content_type, void *produce)
{
    CurlClientInternal *internal = client ? (CurlClientInternal *)(uintptr_t)client->internal : NULL;
    return do_request_upload_stream(internal, url, "PATCH", content_type, produce);
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
