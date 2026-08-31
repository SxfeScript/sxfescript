#include <quickjs.h>
#include <quickjs-libc.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <curl/curl.h>
#include <uv.h>
/* UV_TCP_REUSEPORT arrived in libuv 1.49; older versions get the socket
   option set by hand below. */
#if UV_VERSION_MAJOR > 1 || (UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 49)
#define SXN_UV_HAS_REUSEPORT 1
#else
#define SXN_UV_HAS_REUSEPORT 0
#endif
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "sxfe.h"
#include "sxn_bootstrap.h"

/* Sxn.ffi lives in src/ffi.c: calling native code is an engine capability
   rather than a Node emulation, so it sits beside the runtime's own surface
   and not in the node: layer. See the header comment there. */
JSValue sxn_ffi(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
void sxn_ffi_init(JSContext *ctx);

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
/* struct sockaddr_in/sockaddr are the only things this file needs from the
   BSD sockets headers (see the uv_tcp_bind call below) - everything else
   goes through libuv, which owns its own Winsock init. */
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <sys/stat.h>

#define countof(x) (sizeof(x) / sizeof((x)[0]))

/* One process-wide loop, shared by the server socket and the async file
   reader; main.c drives it via sxn_run_event_loop after top-level eval. */
static uv_loop_t *sxn_loop(void) { return uv_default_loop(); }

/* --- curl_multi driven by sxn_loop(), the standard "hiperfifo" pattern:
   CURLMOPT_SOCKETFUNCTION/CURLMOPT_TIMERFUNCTION let libcurl drive
   uv_poll_t/uv_timer_t handles instead of blocking inside
   curl_easy_perform, so fetch() no longer stalls the runtime while a
   response streams in. One multi handle for the whole process, matching
   the one-loop-per-process design of sxn_loop(). */
static CURLM *sxn_curl_multi = NULL;
static uv_timer_t sxn_curl_timer;

typedef struct CurlSocketCtx { uv_poll_t poll_handle; curl_socket_t sockfd; } CurlSocketCtx;

static void sxn_curl_check_multi_info(void);

static void curl_socket_close_cb(uv_handle_t *handle) { free(handle->data); }

static void curl_poll_cb(uv_poll_t *handle, int status, int events) {
    CurlSocketCtx *sc = (CurlSocketCtx *)handle->data;
    int flags = 0, running = 0;
    if (status < 0) flags = CURL_CSELECT_ERR;
    else {
        if (events & UV_READABLE) flags |= CURL_CSELECT_IN;
        if (events & UV_WRITABLE) flags |= CURL_CSELECT_OUT;
    }
    curl_multi_socket_action(sxn_curl_multi, sc->sockfd, flags, &running);
    sxn_curl_check_multi_info();
}

static int curl_handle_socket_cb(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp) {
    (void)easy; (void)userp;
    if (action == CURL_POLL_REMOVE) {
        if (socketp) {
            CurlSocketCtx *sc = (CurlSocketCtx *)socketp;
            uv_poll_stop(&sc->poll_handle);
            uv_close((uv_handle_t *)&sc->poll_handle, curl_socket_close_cb);
            curl_multi_assign(sxn_curl_multi, s, NULL);
        }
        return 0;
    }
    CurlSocketCtx *sc = (CurlSocketCtx *)socketp;
    if (!sc) {
        sc = malloc(sizeof(*sc));
        uv_poll_init_socket(sxn_loop(), &sc->poll_handle, s);
        sc->poll_handle.data = sc; sc->sockfd = s;
        curl_multi_assign(sxn_curl_multi, s, sc);
    }
    int events = 0;
    if (action != CURL_POLL_IN) events |= UV_WRITABLE;
    if (action != CURL_POLL_OUT) events |= UV_READABLE;
    uv_poll_start(&sc->poll_handle, events, curl_poll_cb);
    return 0;
}

static void curl_timeout_cb(uv_timer_t *handle) {
    (void)handle;
    int running = 0;
    curl_multi_socket_action(sxn_curl_multi, CURL_SOCKET_TIMEOUT, 0, &running);
    sxn_curl_check_multi_info();
}

static int curl_start_timeout_cb(CURLM *multi, long timeout_ms, void *userp) {
    (void)multi; (void)userp;
    if (timeout_ms < 0) { uv_timer_stop(&sxn_curl_timer); return 0; }
    if (timeout_ms == 0) timeout_ms = 1; /* uv_timer_start(0) fires immediately; force one loop tick instead */
    uv_timer_start(&sxn_curl_timer, curl_timeout_cb, (uint64_t)timeout_ms, 0);
    return 0;
}

static void sxn_curl_multi_init(void) {
    if (sxn_curl_multi) return;
    sxn_curl_multi = curl_multi_init();
    uv_timer_init(sxn_loop(), &sxn_curl_timer);
    curl_multi_setopt(sxn_curl_multi, CURLMOPT_SOCKETFUNCTION, curl_handle_socket_cb);
    curl_multi_setopt(sxn_curl_multi, CURLMOPT_TIMERFUNCTION, curl_start_timeout_cb);
}

/* Grow-as-you-go byte buffer used to assemble a whole HTTP response before
   handing it to a single uv_write, mirroring the byte stream the old
   blocking send_all() calls produced. */
typedef struct DynBuf { char *data; size_t length, cap; } DynBuf;
/* Header names are case-insensitive; strcasecmp is not in C17's standard
   library, so compare explicitly rather than depend on a POSIX extension. */
static int sxn_strcasecmp(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Make room for `want` bytes in one go. A 1MB request body arriving through
   a buffer that doubles from 256 bytes is copied about a dozen times on the
   way in; when Content-Length says how big it will be, it is copied once. */
static void dynbuf_reserve(DynBuf *buf, size_t want) {
    if (want <= buf->cap) return;
    /* Grow geometrically as well as to what was asked for: the read path
       asks for "what I have plus another read", and honouring that exactly
       would copy the whole buffer on every read. */
    size_t cap = buf->cap ? buf->cap * 2 : 256;
    if (cap < want) cap = want;
    buf->data = realloc(buf->data, cap);
    buf->cap = cap;
}

static void dynbuf_append(DynBuf *buf, const void *data, size_t n) {
    dynbuf_reserve(buf, buf->length + n);
    memcpy(buf->data + buf->length, data, n); buf->length += n;
}
static void dynbuf_puts(DynBuf *buf, const char *s) { dynbuf_append(buf, s, strlen(s)); }

/* Forward declarations for the SxnChunkView borrow-lock machinery (full
   definitions further down, alongside FetchState/ChunkNode -- see that
   section's doc comment). conn_read_cb below needs to detect and
   zero-copy-append a BorrowedChunk passed as a served response body (the
   file-serving primitive added alongside Sxn.file(path).readBorrowed()),
   reusing the exact same borrow-guarded mechanism built for streamed fetch
   chunks rather than inventing a second one. */
typedef struct SxnChunkViewState SxnChunkViewState;
static JSClassID sxn_chunkview_class_id;
/* The object returned by Sxn.serve; its opaque is the uv_tcp_t listener. */
static JSClassID sxn_serverhandle_class_id;
static JSClassDef sxn_serverhandle_class = { "ServerHandle", NULL, NULL, NULL, NULL };
static JSValue sxn_chunkview_acquire(JSContext *ctx, SxnChunkViewState *st, int exclusive, JSValue *out_buffer);
static void sxn_chunkview_release_shared(SxnChunkViewState *st);

static const char *reason(int status) {
    switch (status) { case 200: return "OK"; case 201: return "Created"; case 204: return "No Content";
        case 400: return "Bad Request"; case 404: return "Not Found"; case 500: return "Internal Server Error";
        default: return "Response"; }
}

static JSValue sxn_memory_usage(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSMemoryUsage mem;
    JSGCStats gc;
    JS_ComputeMemoryUsage(rt, &mem);
    JS_GetGCStats(rt, &gc);
    JSValue result = JS_NewObject(ctx);
    if (JS_IsException(result)) return result;
#define MEM_FIELD(name, value) \
    JS_SetPropertyStr(ctx, result, name, JS_NewInt64(ctx, (int64_t)(value)))
    MEM_FIELD("mallocSize", mem.malloc_size);
    MEM_FIELD("mallocCount", mem.malloc_count);
    MEM_FIELD("memoryUsed", mem.memory_used_size);
    MEM_FIELD("objects", mem.obj_count);
    MEM_FIELD("strings", mem.str_count);
    MEM_FIELD("atoms", mem.atom_count);
    MEM_FIELD("shapes", mem.shape_count);
    MEM_FIELD("bytecodeBytes", mem.js_func_code_size);
    MEM_FIELD("gcCount", gc.count);
    MEM_FIELD("gcTotalNs", gc.total_ns);
    MEM_FIELD("gcLastNs", gc.last_ns);
    MEM_FIELD("gcMaxNs", gc.max_ns);
#undef MEM_FIELD
    return result;
}

static char *header_value(char *request, const char *name) {
    size_t length = strlen(name);
    for (char *line = strstr(request, "\r\n"); line && line[2] && line[2] != '\r';) {
        line += 2;
        if (!strncasecmp(line, name, length) && line[length] == ':') {
            char *value = line + length + 1; while (*value == ' ' || *value == '\t') ++value;
            char *end = strstr(value, "\r\n"); if (end) *end = 0; return value;
        }
        line = strstr(line, "\r\n");
    }
    return NULL;
}

static void websocket_handshake(DynBuf *out, const char *key) {
    char joined[256], encoded[64], response[512];
    unsigned char digest[SHA_DIGEST_LENGTH];
    snprintf(joined, sizeof(joined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    SHA1((unsigned char *)joined, strlen(joined), digest);
    EVP_EncodeBlock((unsigned char *)encoded, digest, SHA_DIGEST_LENGTH);
    int n = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", encoded);
    dynbuf_append(out, response, (size_t)n);
}

static void websocket_text(DynBuf *out, const char *text) {
    size_t length = strlen(text); unsigned char header[10]; size_t h = 0;
    header[h++] = 0x81;
    if (length < 126) header[h++] = (unsigned char)length;
    else if (length <= 65535) { header[h++] = 126; header[h++] = (length >> 8) & 255; header[h++] = length & 255; }
    else return;
    dynbuf_append(out, header, h); dynbuf_append(out, text, length);
}

/* Shared by every accepted connection: the JS handler is looked up once per
   Sxn.serve() call and kept alive (JS_DupValue) for the server's lifetime. */
typedef struct ConnState ConnState;
typedef struct ServeState { JSContext *ctx; JSValue handler; ConnState *conns; } ServeState;

/* Per-connection state: outlives the read/parse/dispatch step so the
   assembled response survives until the uv_write completes. */
struct ConnState {
    ServeState *serve; uv_tcp_t handle; char *write_data;
    /* Held across an async handler: the upgrade bits belong to the request
       that is still being answered, and the read buffer is long gone. */
    char *pending_upgrade; char *pending_ws_key;
    /* A request arrives over as many reads as the kernel feels like giving
       us; only a small one fits in the first. This accumulates them until
       the head and Content-Length bytes of body are both in hand. */
    DynBuf in;
    /* Keep-alive: whether this connection survives the response being
       written, and how many bytes of `in` the answered request used -- what
       is left after them is the next, pipelined request. */
    int keep_alive; size_t consumed;
    /* Every live connection of a server, so stop() can close them: a
       keep-alive connection outlives the request that opened it, and would
       otherwise hold the loop open after its server was stopped. */
    ConnState *prev, *next;
};

/* A request bigger than this is refused rather than buffered: the whole
   thing is held in memory before the handler sees it. */
#define SXN_MAX_REQUEST_BYTES (64u * 1024u * 1024u)

static void conn_deliver(JSContext *ctx, ConnState *conn, JSValue result,
                         const char *upgrade, const char *ws_key);

/* Resolution and rejection of a handler's promise. The connection travels
   through the closure as an integer because it outlives the JS values. */
static void conn_finish_pending(JSContext *ctx, ConnState *conn, JSValue result) {
    conn_deliver(ctx, conn, result, conn->pending_upgrade, conn->pending_ws_key);
    free(conn->pending_upgrade); conn->pending_upgrade = NULL;
    free(conn->pending_ws_key); conn->pending_ws_key = NULL;
}

static JSValue conn_promise_done(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv,
                                 int magic, JSValueConst *func_data) {
    (void)this_val; (void)magic;
    int64_t p = 0; JS_ToInt64(ctx, &p, func_data[0]);
    conn_finish_pending(ctx, (ConnState *)(intptr_t)p,
                        argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED);
    return JS_UNDEFINED;
}

static JSValue conn_promise_fail(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv,
                                 int magic, JSValueConst *func_data) {
    (void)this_val; (void)magic; (void)argc; (void)argv;
    int64_t p = 0; JS_ToInt64(ctx, &p, func_data[0]);
    /* A rejected handler is a 500, same as a thrown one. */
    conn_finish_pending(ctx, (ConnState *)(intptr_t)p, JS_EXCEPTION);
    return JS_UNDEFINED;
}

/* Unlink first, then close: the close callback may run after the server it
   belonged to is gone. */
static void conn_shutdown(ConnState *conn, uv_close_cb cb) {
    if (conn->serve) {
        if (conn->prev) conn->prev->next = conn->next; else conn->serve->conns = conn->next;
        if (conn->next) conn->next->prev = conn->prev;
        conn->serve = NULL; conn->prev = conn->next = NULL;
    }
    uv_close((uv_handle_t *)&conn->handle, cb);
}

static void conn_close_cb(uv_handle_t *handle) {
    ConnState *conn = (ConnState *)handle->data;
    free(conn->write_data); free(conn->in.data); free(conn);
}

static void conn_try_dispatch(ConnState *conn);
static void conn_alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf);
static void conn_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);

/* One response is written. On a keep-alive exchange the connection is reused
   for the next request rather than closed -- without this every request paid
   for a new TCP connection, which under a load test exhausted the client's
   ephemeral ports long before the server was the bottleneck. */
static void conn_write_cb(uv_write_t *req, int status) {
    ConnState *conn = (ConnState *)req->data; free(req);
    if (status < 0 || !conn->keep_alive) {
        conn_shutdown(conn, conn_close_cb);
        return;
    }
    free(conn->write_data); conn->write_data = NULL;
    /* Whatever follows the request just answered is the next one, already
       here: a pipelining client sends without waiting. */
    size_t left = conn->in.length > conn->consumed ? conn->in.length - conn->consumed : 0;
    if (left) memmove(conn->in.data, conn->in.data + conn->consumed, left);
    conn->in.length = left;
    if (conn->in.data) conn->in.data[left] = 0;
    conn->consumed = 0;
    uv_read_start((uv_stream_t *)&conn->handle, conn_alloc_cb, conn_read_cb);
    if (left) conn_try_dispatch(conn);
}

/* Read straight into the connection's own buffer. It used to allocate 64KB
   for every read and copy it in afterwards: a 1MB request meant sixteen
   allocations, sixteen frees and a megabyte of copying that the kernel could
   have done into the right place to begin with. */
static void conn_alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
    (void)suggested;
    ConnState *conn = (ConnState *)handle->data;
    size_t room = 65536;
    dynbuf_reserve(&conn->in, conn->in.length + room + 1); /* +1 for a trailing NUL */
    if (!conn->in.data) { buf->base = NULL; buf->len = 0; return; }
    buf->base = conn->in.data + conn->in.length;
    buf->len = conn->in.cap - conn->in.length - 1;
}

/* arcsx: turn a handler's return value into bytes and write them. Split out
   of conn_read_cb so a handler may also return a promise -- node:http's
   (req, res) model finishes the response long after the handler returns, and
   a promise is the only way to express that here. */
static void conn_deliver(JSContext *ctx, ConnState *conn, JSValue result,
                         const char *upgrade, const char *ws_key) {
    uv_stream_t *stream = (uv_stream_t *)&conn->handle;

    DynBuf out = {0};
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, result);
        conn->keep_alive = 0;
        dynbuf_puts(&out, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    } else {
        JSValue mode_value = JS_GetPropertyStr(ctx, result, "mode"); const char *mode = JS_ToCString(ctx, mode_value);
        /* An upgrade and an SSE stream both own the connection until the
           client goes away; neither can be followed by another request. */
        if (mode && (!strcmp(mode, "websocket") || !strcmp(mode, "sse"))) conn->keep_alive = 0;
        if (mode && !strcmp(mode, "websocket") && upgrade && ws_key) {
            websocket_handshake(&out, ws_key);
            JSValue messages = JS_GetPropertyStr(ctx, result, "wsMessages"); uint32_t length = 0; JSValue size = JS_GetPropertyStr(ctx, messages, "length"); JS_ToUint32(ctx, &length, size); JS_FreeValue(ctx, size);
            for (uint32_t i = 0; i < length; ++i) { JSValue item = JS_GetPropertyUint32(ctx, messages, i); const char *text = JS_ToCString(ctx, item); if (text) websocket_text(&out, text); JS_FreeCString(ctx, text); JS_FreeValue(ctx, item); }
            JS_FreeValue(ctx, messages);
        } else if (mode && !strcmp(mode, "sse")) {
            dynbuf_puts(&out, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
            JSValue events = JS_GetPropertyStr(ctx, result, "events"); uint32_t length = 0; JSValue size = JS_GetPropertyStr(ctx, events, "length"); JS_ToUint32(ctx, &length, size); JS_FreeValue(ctx, size);
            for (uint32_t i = 0; i < length; ++i) {
                JSValue event = JS_GetPropertyUint32(ctx, events, i); JSValue data = JS_GetPropertyStr(ctx, event, "data"); JSValue type = JS_GetPropertyStr(ctx, event, "event"); JSValue id = JS_GetPropertyStr(ctx, event, "id");
                const char *text = JS_ToCString(ctx, data), *event_name = (JS_IsUndefined(type) || JS_IsNull(type)) ? NULL : JS_ToCString(ctx, type);
                /* An absent id must be left out, not written as the string
                   "undefined" -- JS_ToCString stringifies undefined. */
                const char *event_id = (JS_IsUndefined(id) || JS_IsNull(id)) ? NULL : JS_ToCString(ctx, id);
                char full[4096]; int n;
                if (event_id) n = snprintf(full, sizeof(full), "event: %s\nid: %s\ndata: %s\n\n", event_name ? event_name : "message", event_id, text ? text : "");
                else n = snprintf(full, sizeof(full), "%s%s%sdata: %s\n\n", event_name ? "event: " : "", event_name ? event_name : "", event_name ? "\n" : "", text ? text : "");
                dynbuf_append(&out, full, (size_t)n);
                JS_FreeCString(ctx, text); JS_FreeCString(ctx, event_name); JS_FreeCString(ctx, event_id); JS_FreeValue(ctx, data); JS_FreeValue(ctx, type); JS_FreeValue(ctx, id); JS_FreeValue(ctx, event);
            }
            JS_FreeValue(ctx, events);
        } else {
            int32_t status = 200; JSValue status_value = JS_GetPropertyStr(ctx, result, "statusCode"); JS_ToInt32(ctx, &status, status_value); JS_FreeValue(ctx, status_value);
            JSValue body_value = JS_GetPropertyStr(ctx, result, "body");

            /* Binary-safe body handling: the old code below unconditionally
               JS_ToCString+strlen'd the body, which silently mangled (wrong
               bytes, via JS's string-conversion of a typed array) and
               truncated (strlen stops at the first 0x00) any binary
               response -- there was no correct way to serve non-text
               content. Checked cheapest/most-specific first:
                 1. a BorrowedChunk (Sxn.file(path).readBorrowed()'s result)
                    -- zero-copy: acquire a shared borrow, copy its bytes
                    straight into `out`, release immediately. Reuses the
                    exact SxnBorrow machinery Task 4 built for streamed
                    fetch chunks -- see this file's borrow-lock doc comment.
                 2. a Blob (bootstrap.js) -- bytes live in a plain Uint8Array.
                 3. a raw Uint8Array/Uint8ClampedArray/ArrayBuffer.
               Anything else (the common case) falls through to the
               original JS_ToCString string path, unchanged. */
            const uint8_t *body_bytes = NULL; size_t body_len = 0;
            const char *body_text = NULL;
            const char *content_type = "text/plain; charset=utf-8";
            int is_binary = 0;
            SxnChunkViewState *borrowed_cv = JS_GetOpaque(body_value, sxn_chunkview_class_id);
            JSValue borrow_u8 = JS_UNDEFINED, borrow_buffer = JS_UNDEFINED;
            JSValue blob_bytes_value = JS_UNDEFINED, blob_type_value = JS_UNDEFINED;
            const char *blob_type_cstr = NULL;

            if (borrowed_cv) {
                JSValue u8 = sxn_chunkview_acquire(ctx, borrowed_cv, 0 /* shared: read-only, allows concurrent borrows across requests */, &borrow_buffer);
                if (!JS_IsException(u8)) {
                    size_t n = 0; uint8_t *p = JS_GetUint8Array(ctx, &n, u8);
                    body_bytes = p; body_len = n; is_binary = 1; content_type = "application/octet-stream";
                    borrow_u8 = u8;
                } else {
                    JS_FreeValue(ctx, JS_GetException(ctx)); /* conflict/retired: surface as 500, don't stringify the BorrowedChunk object */
                    status = 500; is_binary = 1;
                }
            } else {
                JSValue global = JS_GetGlobalObject(ctx);
                JSValue blob_ctor = JS_GetPropertyStr(ctx, global, "Blob");
                JS_FreeValue(ctx, global);
                int is_blob = JS_IsFunction(ctx, blob_ctor) && JS_IsInstanceOf(ctx, body_value, blob_ctor) > 0;
                JS_FreeValue(ctx, blob_ctor);
                if (is_blob) {
                    blob_bytes_value = JS_GetPropertyStr(ctx, body_value, "_bytes");
                    blob_type_value = JS_GetPropertyStr(ctx, body_value, "type");
                    size_t n = 0; uint8_t *p = JS_GetUint8Array(ctx, &n, blob_bytes_value);
                    if (p) {
                        body_bytes = p; body_len = n; is_binary = 1;
                        blob_type_cstr = JS_ToCString(ctx, blob_type_value);
                        content_type = (blob_type_cstr && blob_type_cstr[0]) ? blob_type_cstr : "application/octet-stream";
                    }
                } else {
                    int tt = JS_GetTypedArrayType(body_value);
                    if (tt == JS_TYPED_ARRAY_UINT8 || tt == JS_TYPED_ARRAY_UINT8C) {
                        size_t n = 0; uint8_t *p = JS_GetUint8Array(ctx, &n, body_value);
                        if (p) { body_bytes = p; body_len = n; is_binary = 1; content_type = "application/octet-stream"; }
                    } else if (JS_IsArrayBuffer(body_value)) {
                        size_t n = 0; uint8_t *p = JS_GetArrayBuffer(ctx, &n, body_value);
                        if (p) { body_bytes = p; body_len = n; is_binary = 1; content_type = "application/octet-stream"; }
                    }
                }
            }

            if (!is_binary) { body_text = JS_ToCString(ctx, body_value); body_len = body_text ? strlen(body_text) : 0; }

            /* A response may carry a `headers` object. Content-Type from it
               wins over the default and the Blob-derived one; Content-Length
               and Connection stay ours because they describe this framing,
               not the payload. Values are emitted in insertion order, so
               repeated names (Set-Cookie) can be passed as an array. */
            JSValue hdrs = JS_GetPropertyStr(ctx, result, "headers");
            char *extra = NULL; size_t extra_len = 0;
            const char *ct_override = NULL;
            JSValue ct_value = JS_UNDEFINED;
            if (JS_IsObject(hdrs)) {
                JSPropertyEnum *tab = NULL; uint32_t count = 0;
                if (!JS_GetOwnPropertyNames(ctx, &tab, &count, hdrs,
                                            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
                    DynBuf hb = {0};
                    for (uint32_t hi = 0; hi < count; hi++) {
                        const char *key = JS_AtomToCString(ctx, tab[hi].atom);
                        JSValue v = JS_GetProperty(ctx, hdrs, tab[hi].atom);
                        if (key && !JS_IsUndefined(v) && !JS_IsNull(v)) {
                            if (!sxn_strcasecmp(key, "content-type")) {
                                JS_FreeValue(ctx, ct_value);
                                ct_value = JS_DupValue(ctx, v);
                                ct_override = JS_ToCString(ctx, ct_value);
                            } else if (sxn_strcasecmp(key, "content-length") &&
                                       sxn_strcasecmp(key, "connection")) {
                                /* An array value repeats the header. */
                                int64_t alen = -1;
                                if (JS_IsArray(v)) JS_GetLength(ctx, v, &alen);
                                for (int64_t ai = 0; ai < (alen < 0 ? 1 : alen); ai++) {
                                    JSValue one = alen < 0 ? JS_DupValue(ctx, v)
                                                           : JS_GetPropertyInt64(ctx, v, ai);
                                    const char *val = JS_ToCString(ctx, one);
                                    if (val) {
                                        dynbuf_append(&hb, key, strlen(key));
                                        dynbuf_append(&hb, ": ", 2);
                                        dynbuf_append(&hb, val, strlen(val));
                                        dynbuf_append(&hb, "\r\n", 2);
                                        JS_FreeCString(ctx, val);
                                    }
                                    JS_FreeValue(ctx, one);
                                }
                            }
                        }
                        JS_FreeValue(ctx, v);
                        if (key) JS_FreeCString(ctx, key);
                        JS_FreeAtom(ctx, tab[hi].atom);
                    }
                    js_free(ctx, tab);
                    extra = hb.data; extra_len = hb.length;
                }
            }
            JS_FreeValue(ctx, hdrs);
            if (ct_override) content_type = ct_override;

            /* A HEAD response carries the length the body would have had
               and no body at all, which is the one thing a handler cannot
               express by returning bytes. */
            JSValue nobody_value = JS_GetPropertyStr(ctx, result, "bodyOmitted");
            int body_omitted = JS_ToBool(ctx, nobody_value);
            JS_FreeValue(ctx, nobody_value);

            char head[512]; int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: %s\r\nContent-Type: %s\r\n", status, reason(status), body_len, conn->keep_alive ? "keep-alive" : "close", content_type);
            dynbuf_append(&out, head, (size_t)n);
            if (extra && extra_len) dynbuf_append(&out, extra, extra_len);
            dynbuf_append(&out, "\r\n", 2);
            free(extra);
            if (ct_override) JS_FreeCString(ctx, ct_override);
            JS_FreeValue(ctx, ct_value);
            if (body_omitted) { /* HEAD: the length above, and nothing after it */ }
            else if (is_binary) { if (body_bytes) dynbuf_append(&out, body_bytes, body_len); }
            else if (body_text) dynbuf_append(&out, body_text, body_len);

            if (borrowed_cv && !JS_IsUndefined(borrow_u8)) {
                JS_FreeValue(ctx, borrow_u8);
                JS_DetachArrayBuffer(ctx, borrow_buffer); /* ends this borrow's visibility, same as withSharedBorrow */
                JS_FreeValue(ctx, borrow_buffer);
                sxn_chunkview_release_shared(borrowed_cv);
            }
            if (blob_type_cstr) JS_FreeCString(ctx, blob_type_cstr);
            JS_FreeValue(ctx, blob_type_value);
            JS_FreeValue(ctx, blob_bytes_value);
            if (body_text) JS_FreeCString(ctx, body_text);
            JS_FreeValue(ctx, body_value);
        }
        JS_FreeCString(ctx, mode); JS_FreeValue(ctx, mode_value); JS_FreeValue(ctx, result);
    }

    conn->write_data = out.data;
    uv_write_t *write_req = malloc(sizeof(*write_req)); write_req->data = conn;
    uv_buf_t write_buf = uv_buf_init(out.data, (unsigned)out.length);
    uv_write(write_req, stream, &write_buf, 1, conn_write_cb);
}

/* One complete request, parsed and handed to the handler. `request` is NUL
   terminated for the header scanning below, and `length` is what bounds the
   body -- a body may legitimately contain a NUL byte, so its length never
   comes from strlen. */
/* The connection's read buffer, once it has been handed to JavaScript. */
static void sxn_free_read_buffer(JSRuntime *rt, void *opaque, void *ptr) {
    (void)rt; (void)opaque;
    free(ptr);
}

/* `body`, materialised only if something reads it. The bytes belong to the
   Uint8Array in func_data, so this cannot outlive them. */
static JSValue conn_body_string(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv,
                                int magic, JSValueConst *func_data) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    size_t size = 0;
    uint8_t *bytes = JS_GetUint8Array(ctx, &size, func_data[0]);
    int64_t offset = 0, length = 0;
    JS_ToInt64(ctx, &offset, func_data[1]);
    JS_ToInt64(ctx, &length, func_data[2]);
    if (!bytes || (size_t)(offset + length) > size) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, (const char *)bytes + offset, (size_t)length);
}

/* JSON straight from the bytes, with no string in between: a 1MB request
   body would otherwise be copied into a JavaScript string first, and the
   parser only wants the bytes. */
static JSValue sxn_parse_json_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    size_t size = 0;
    uint8_t *bytes = argc > 0 ? JS_GetUint8Array(ctx, &size, argv[0]) : NULL;
    if (!bytes) return JS_ThrowTypeError(ctx, "expected a Uint8Array");
    int64_t offset = 0, length = (int64_t)size;
    if (argc > 1) JS_ToInt64(ctx, &offset, argv[1]);
    if (argc > 2) JS_ToInt64(ctx, &length, argv[2]);
    if (offset < 0 || length < 0 || (size_t)(offset + length) > size)
        return JS_ThrowRangeError(ctx, "body slice out of range");
    return JS_ParseJSON(ctx, (const char *)bytes + offset, (size_t)length, "<body>");
}

static void conn_dispatch_request(ConnState *conn, char *request, size_t length) {
    JSContext *ctx = conn->serve->ctx;
    char method[16] = {0}, url[4096] = {0}; sscanf(request, "%15s %4095s", method, url);
    char *head_end = strstr(request, "\r\n\r\n");
    char *body = head_end ? head_end + 4 : request + length;
    size_t body_len = (size_t)((request + length) - body);
    char *ws_key = header_value(request, "Sec-WebSocket-Key");
    char *upgrade = header_value(request, "Upgrade");
    JSValue req_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, req_obj, "method", JS_NewString(ctx, method));
    JS_SetPropertyStr(ctx, req_obj, "url", JS_NewString(ctx, url));
    if (body_len == 0) {
        JS_SetPropertyStr(ctx, req_obj, "body", JS_NewString(ctx, ""));
    } else {
        /* Hand the body over as bytes JavaScript owns. When nothing else is
           in the buffer -- which is every request that is not pipelined --
           the buffer itself goes, so a megabyte of body is not copied into a
           string that the handler may not even read. */
        JSValue bytes;
        size_t offset = (size_t)(body - request);
        if (conn->in.data == request && conn->in.length == length) {
            conn->in.data[length] = 0;   /* the parser may look one past the end */
            bytes = JS_NewUint8Array(ctx, (uint8_t *)request, length + 1,
                                     sxn_free_read_buffer, NULL, false);
            conn->in.data = NULL; conn->in.length = 0; conn->in.cap = 0;
        } else {
            /* Another request is already in the buffer behind this one, so
               the buffer cannot be handed over. The copy carries a trailing
               NUL for the same reason the transferred buffer does: the JSON
               parser may look one byte past the body. */
            uint8_t *copy = malloc(length + 1);
            if (!copy) { JS_FreeValue(ctx, req_obj); return; }
            memcpy(copy, request, length);
            copy[length] = 0;
            bytes = JS_NewUint8Array(ctx, copy, length + 1, sxn_free_read_buffer, NULL, false);
        }
        JSValue offset_value = JS_NewInt64(ctx, (int64_t)offset);
        JSValue length_value = JS_NewInt64(ctx, (int64_t)body_len);
        JS_SetPropertyStr(ctx, req_obj, "bodyBytes", JS_DupValue(ctx, bytes));
        JS_SetPropertyStr(ctx, req_obj, "bodyOffset", JS_DupValue(ctx, offset_value));
        JS_SetPropertyStr(ctx, req_obj, "bodyLength", JS_DupValue(ctx, length_value));
        /* `body` stays a string for node:http, built only if it is read. */
        JSValueConst data[3] = { bytes, offset_value, length_value };
        JSValue getter = JS_NewCFunctionData(ctx, conn_body_string, 0, 0, 3, data);
        JSAtom body_atom = JS_NewAtom(ctx, "body");
        JS_DefinePropertyGetSet(ctx, req_obj, body_atom, getter, JS_UNDEFINED, JS_PROP_C_W_E);
        JS_FreeAtom(ctx, body_atom);
        JS_FreeValue(ctx, bytes);
        JS_FreeValue(ctx, offset_value);
        JS_FreeValue(ctx, length_value);
    }
    /* Every request header, lowercased, the way Node presents them. Only
       `upgrade` used to be exposed, so a handler could not read an
       Authorization, Content-Type or Cookie header at all. */
    JSValue headers = JS_NewObject(ctx);
    {
        const char *line = strstr(request, "\r\n");
        while (line) {
            line += 2;
            if (line[0] == '\r' || line[0] == 0) break;      /* end of the head */
            const char *colon = strchr(line, ':');
            const char *eol = strstr(line, "\r\n");
            if (!colon || (eol && colon > eol)) { line = eol; continue; }
            size_t nlen = (size_t)(colon - line);
            char name[256];
            if (nlen >= sizeof(name)) { line = eol; continue; }
            for (size_t i = 0; i < nlen; i++) {
                char c = line[i];
                name[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            }
            name[nlen] = 0;
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vlen = eol ? (size_t)(eol - v) : strlen(v);
            JSValue existing = JS_GetPropertyStr(ctx, headers, name);
            if (JS_IsString(existing)) {
                /* A repeated header joins with ", ", except set-cookie. */
                const char *prev = JS_ToCString(ctx, existing);
                size_t plen = prev ? strlen(prev) : 0;
                char *joined = malloc(plen + vlen + 3);
                if (joined) {
                    memcpy(joined, prev ? prev : "", plen);
                    memcpy(joined + plen, ", ", 2);
                    memcpy(joined + plen + 2, v, vlen);
                    joined[plen + 2 + vlen] = 0;
                    JS_SetPropertyStr(ctx, headers, name, JS_NewString(ctx, joined));
                    free(joined);
                }
                if (prev) JS_FreeCString(ctx, prev);
            } else {
                JS_SetPropertyStr(ctx, headers, name, JS_NewStringLen(ctx, v, vlen));
            }
            JS_FreeValue(ctx, existing);
            line = eol;
        }
    }
    if (upgrade) JS_SetPropertyStr(ctx, headers, "upgrade", JS_NewString(ctx, upgrade));
    JS_SetPropertyStr(ctx, req_obj, "headers", headers);
    JSValue result = JS_Call(ctx, conn->serve->handler, JS_UNDEFINED, 1, &req_obj); JS_FreeValue(ctx, req_obj);

    /* A handler may return a promise; wait for it before writing. */
    if (!JS_IsException(result) && JS_PromiseState(ctx, result) != -1) {
        conn->pending_upgrade = upgrade ? strdup(upgrade) : NULL;
        conn->pending_ws_key = ws_key ? strdup(ws_key) : NULL;
        JSValue data = JS_NewInt64(ctx, (int64_t)(intptr_t)conn);
        JSValueConst d[1] = { data };
        JSValue on_ok = JS_NewCFunctionData(ctx, conn_promise_done, 1, 0, 1, d);
        JSValue on_err = JS_NewCFunctionData(ctx, conn_promise_fail, 1, 0, 1, d);
        JSValue chained = JS_Invoke(ctx, result, JS_NewAtom(ctx, "then"), 1, (JSValueConst *)&on_ok);
        JSValue caught = JS_Invoke(ctx, chained, JS_NewAtom(ctx, "catch"), 1, (JSValueConst *)&on_err);
        JS_FreeValue(ctx, caught); JS_FreeValue(ctx, chained);
        JS_FreeValue(ctx, on_ok); JS_FreeValue(ctx, on_err);
        JS_FreeValue(ctx, data); JS_FreeValue(ctx, result);
        return;
    }
    conn_deliver(ctx, conn, result, upgrade, ws_key);
}

/* HTTP/1.1 keeps a connection open unless the request says otherwise;
   HTTP/1.0 closes it unless the request asks for keep-alive. */
static int request_keeps_alive(const char *head, const char *head_end) {
    int one_one = strstr(head, "HTTP/1.1") != NULL && strstr(head, "HTTP/1.1") < head_end;
    const char *line = strstr(head, "\r\n");
    while (line && line + 2 < head_end) {
        line += 2;
        if (!strncasecmp(line, "Connection:", 11)) {
            const char *value = line + 11;
            while (*value == ' ' || *value == '\t') ++value;
            if (!strncasecmp(value, "close", 5)) return 0;
            if (!strncasecmp(value, "keep-alive", 10)) return 1;
            break;
        }
        line = strstr(line, "\r\n");
    }
    return one_one;
}

/* Content-Length, or -1 when the head does not carry one. Its own scan
   rather than header_value's, which returns an interior pointer and
   overwrites the line's CRLF with a NUL -- fine once the request is complete
   and about to be parsed, wrong while it is still being read. */
static long long request_content_length(const char *head, const char *head_end) {
    const char *line = strstr(head, "\r\n");
    while (line && line + 2 < head_end) {
        line += 2;
        if (!strncasecmp(line, "Content-Length:", 15)) {
            const char *value = line + 15;
            while (*value == ' ' || *value == '\t') ++value;
            long long length = strtoll(value, NULL, 10);
            return length < 0 ? -1 : length;
        }
        line = strstr(line, "\r\n");
    }
    return -1;
}

/* Dispatch as soon as a whole request is in the buffer; otherwise wait for
   more reads. Called after every read, and again after a response is written
   in case the client pipelined the next request behind the last one. */
static void conn_try_dispatch(ConnState *conn) {
    uv_stream_t *stream = (uv_stream_t *)&conn->handle;
    if (conn->in.length > SXN_MAX_REQUEST_BYTES) {
        uv_read_stop(stream);
        conn->keep_alive = 0;
        conn_deliver(conn->serve->ctx, conn, JS_EXCEPTION, NULL, NULL);
        return;
    }
    /* Wait for the whole head, then for the whole body it announces. Without
       this a request larger than one read -- anything past about 64KB -- was
       handed to the handler truncated. */
    char *head_end = strstr(conn->in.data, "\r\n\r\n");
    if (!head_end) return;
    long long content_length = request_content_length(conn->in.data, head_end);
    size_t head_bytes = (size_t)(head_end + 4 - conn->in.data);
    size_t body_bytes = content_length > 0 ? (size_t)content_length : 0;
    if (conn->in.length - head_bytes < body_bytes) {
        /* Now that the length is known, take the room for it at once. */
        dynbuf_reserve(&conn->in, head_bytes + body_bytes + 1);
        return;
    }

    uv_read_stop(stream);
    conn->keep_alive = request_keeps_alive(conn->in.data, head_end);
    conn->consumed = head_bytes + body_bytes;
    conn_dispatch_request(conn, conn->in.data, conn->consumed);
}

static void conn_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    ConnState *conn = (ConnState *)stream->data;
    (void)buf;   /* it is a window into conn->in, not a buffer of its own */
    if (nread <= 0) {
        uv_read_stop(stream);
        conn_shutdown(conn, conn_close_cb);
        return;
    }
    conn->in.length += (size_t)nread;
    /* NUL terminated for the header parsing, which is all string work; the
       body is bounded by the length instead. */
    conn->in.data[conn->in.length] = 0;
    conn_try_dispatch(conn);
}

static void on_connection_cb(uv_stream_t *server_handle, int status) {
    if (status < 0) return;
    ServeState *serve = (ServeState *)server_handle->data;
    ConnState *conn = calloc(1, sizeof(*conn)); conn->serve = serve;
    uv_tcp_init(sxn_loop(), &conn->handle); conn->handle.data = conn;
    if (uv_accept(server_handle, (uv_stream_t *)&conn->handle) == 0) {
        /* No Nagle: a reply is written in one go and wants to leave now, not
           when the kernel has collected enough to be worth a packet. */
        uv_tcp_nodelay(&conn->handle, 1);
        conn->next = serve->conns;
        if (serve->conns) serve->conns->prev = conn;
        serve->conns = conn;
        uv_read_start((uv_stream_t *)&conn->handle, conn_alloc_cb, conn_read_cb);
    } else {
        conn->serve = NULL;
        uv_close((uv_handle_t *)&conn->handle, conn_close_cb);
    }
}

/* Stops the listener a serve() call created. The handle is closed
   asynchronously, so the ServeState is released from the close callback
   rather than here. Idempotent: a second stop() is a no-op. */
static void serve_close_cb(uv_handle_t *h) {
    ServeState *serve = (ServeState *)h->data;
    if (serve) {
        JS_FreeValue(serve->ctx, serve->handler);
        free(serve);
    }
    free(h);
}

static JSValue js_serve_stop(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv,
                             int magic, JSValueConst *func_data) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    void *ptr = JS_GetOpaque(func_data[0], sxn_serverhandle_class_id);
    if (ptr) {
        JS_SetOpaque(func_data[0], NULL);
        ServeState *serve = (ServeState *)((uv_handle_t *)ptr)->data;
        while (serve && serve->conns) conn_shutdown(serve->conns, conn_close_cb);
        uv_close((uv_handle_t *)ptr, serve_close_cb);
    }
    return JS_UNDEFINED;
}

static JSValue js_serve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t port = 3000;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "serve(options, handler) requires a handler");
    JSValue port_value = JS_GetPropertyStr(ctx, argv[0], "port"); JS_ToInt32(ctx, &port, port_value); JS_FreeValue(ctx, port_value);

    /* The address to bind. Loopback by default -- a server nobody asked to
       expose should not be reachable from the network -- but a real
       deployment needs 0.0.0.0, and until this was read the option was
       accepted and ignored. `host` is Node's name for it, `hostname` is
       Bun's and Deno's; both work. */
    char host[256] = "127.0.0.1";
    {
        JSValue h = JS_GetPropertyStr(ctx, argv[0], "hostname");
        if (JS_IsUndefined(h)) {
            JS_FreeValue(ctx, h);
            h = JS_GetPropertyStr(ctx, argv[0], "host");
        }
        if (!JS_IsUndefined(h) && !JS_IsNull(h)) {
            const char *str = JS_ToCString(ctx, h);
            if (str) {
                snprintf(host, sizeof(host), "%s", str);
                JS_FreeCString(ctx, str);
            }
        }
        JS_FreeValue(ctx, h);
    }
    /* SO_REUSEPORT: several processes bind the same port and the kernel
       spreads incoming connections across them. This runtime has no threads
       and no cluster module, so running N copies of a server is the way to
       use N cores, and this is what makes that possible. Linux, the BSDs,
       Solaris and AIX only -- macOS's SO_REUSEPORT does not distribute, and
       libuv reports ENOTSUP there rather than silently giving the last
       binder everything. */
    bool reuse_port = false;
    {
        JSValue r = JS_GetPropertyStr(ctx, argv[0], "reusePort");
        reuse_port = JS_ToBool(ctx, r);
        JS_FreeValue(ctx, r);
    }

    ServeState *serve = calloc(1, sizeof(*serve)); serve->ctx = ctx; serve->handler = JS_DupValue(ctx, argv[1]);
    uv_tcp_t *server = malloc(sizeof(*server));
    uv_tcp_init(sxn_loop(), server); server->data = serve;
    struct sockaddr_storage address;
    int rc;
    /* The one name worth resolving without a DNS lookup, and the one people
       actually write. */
    if (!strcmp(host, "localhost"))
        snprintf(host, sizeof(host), "127.0.0.1");
    if (uv_ip4_addr(host, port, (struct sockaddr_in *)&address) == 0)
        rc = 0;
    else if (uv_ip6_addr(host, port, (struct sockaddr_in6 *)&address) == 0)
        rc = 0;
    else {
        free(server); JS_FreeValue(ctx, serve->handler); free(serve);
        return JS_ThrowTypeError(ctx, "serve: '%s' is not an IP address to bind", host);
    }
    if (!reuse_port) {
        rc = uv_tcp_bind(server, (const struct sockaddr *)&address, 0);
    } else {
#if SXN_UV_HAS_REUSEPORT
        rc = uv_tcp_bind(server, (const struct sockaddr *)&address, UV_TCP_REUSEPORT);
#elif defined(SO_REUSEPORT) && defined(__linux__)
        /* libuv older than 1.49 has no flag for it, so the socket is made
           and configured here and handed over. Linux only: this is where
           SO_REUSEPORT distributes connections rather than handing the last
           binder everything. */
        int fd = socket(address.ss_family, SOCK_STREAM, 0);
        int on = 1;
        if (fd < 0) rc = UV_ENOTSUP;
        else if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0
                 || setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0
                 || bind(fd, (const struct sockaddr *)&address,
                         address.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6)
                                                       : sizeof(struct sockaddr_in)) != 0) {
            close(fd);
            rc = UV_ENOTSUP;
        } else {
            rc = uv_tcp_open(server, fd);
            if (rc != 0) close(fd);
        }
#else
        rc = UV_ENOTSUP;
#endif
    }
    /* 511, the same backlog Node uses: at 64 a burst of concurrent clients got
       connection-refused rather than queued. */
    if (rc == 0) rc = uv_listen((uv_stream_t *)server, 511, on_connection_cb);
    if (rc != 0) {
        free(server); JS_FreeValue(ctx, serve->handler); free(serve);
        if (reuse_port && rc == UV_ENOTSUP)
            return JS_ThrowInternalError(ctx, "listen on %s:%d: reusePort is not supported on this platform", host, port);
        return JS_ThrowInternalError(ctx, "listen on %s:%d: %s", host, port, uv_strerror(rc));
    }
    /* `port: 0` asks the OS to choose a free port, which is the only way to
       write a test or an example that can't collide with whatever else is
       already listening. Read back what it chose: reporting the requested 0
       left `handle.port` and `handle.url` naming a port nothing can connect
       to, so the documented `Sxn.serve({ port: 0 }, ...)` was unusable. */
    struct sockaddr_storage bound;
    int bound_len = (int)sizeof(bound);
    if (uv_tcp_getsockname(server, (struct sockaddr *)&bound, &bound_len) == 0)
        port = ntohs(bound.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&bound)->sin6_port
                                                 : ((struct sockaddr_in *)&bound)->sin_port);

    /* Hand back a handle: without one a server can never be stopped, which
       makes it impossible to run a server and anything else in one process. */
    JSValue handle = JS_NewObjectClass(ctx, sxn_serverhandle_class_id);
    if (JS_IsException(handle)) return handle;
    JS_SetOpaque(handle, server);
    JSValueConst data[1] = { handle };
    JS_SetPropertyStr(ctx, handle, "stop",
                      JS_NewCFunctionData(ctx, js_serve_stop, 0, 0, 1, data));
    JS_SetPropertyStr(ctx, handle, "port", JS_NewInt32(ctx, port));
    JS_SetPropertyStr(ctx, handle, "hostname", JS_NewString(ctx, host));
    {
        /* A bare IPv6 address needs brackets in a URL. */
        char u[320];
        snprintf(u, sizeof(u), strchr(host, ':') ? "http://[%s]:%d" : "http://%s:%d", host, port);
        JS_SetPropertyStr(ctx, handle, "url", JS_NewString(ctx, u));
    }
    return handle;
}

/* --- Streaming fetch() -----------------------------------------------
   FetchState is shared between the curl_multi callbacks (which own one
   refcount share until the transfer finishes or is torn down) and the
   native "stream" object exposed to JS as response.body (which owns
   another share, released by its GC finalizer). Response head data
   (status/headers) is pure bookkeeping done here in C because it has to
   be assembled from libcurl callbacks; Headers/Response/fetch() itself
   are plain JS in bootstrap.js since that's pure spec logic. */
/* --- Borrow lock: guards native chunk memory handed to JS zero-copy -----
   (Task 4.) Nothing in this codebase constructs an SX20xx diagnostic
   before this file: include/sxfe.h's SX2001-SX2004 were unused enum
   values (only referenced by frontend.c's name-lookup switch), and there
   is no compile-time borrow checker anywhere -- this is the first real
   runtime use of one of them (SX2002_BORROW_CONFLICT, in
   sxn_throw_borrow_conflict below), and it stays a runtime notion only.

   Single-threaded-cooperative, like the rest of this file: fetch_write_cb
   (libcurl's data callback) only ever runs from inside curl_multi_socket_action,
   which is only ever called from the uv_poll/uv_timer callbacks driven by
   sxn_run_event_loop's uv_run(loop, UV_RUN_ONCE) below -- see the hiperfifo
   wiring above sxn_curl_multi. Every JS-side acquire/release call likewise
   only runs on that same main-thread call stack (QuickJS is not
   reentered from any other thread anywhere in this codebase). So a plain
   int flag/counter, unguarded by atomics or an OS mutex, is a correct and
   sufficient lock -- there is no concurrent OS thread that could ever
   observe or mutate it mid-update. */
typedef struct SxnBorrow {
    int exclusive;      /* 1 while an exclusive (read/write) borrow is held */
    int shared_count;   /* number of concurrently outstanding shared (read-only) borrows */
} SxnBorrow;

static int sxn_borrow_busy(const SxnBorrow *b) { return b->exclusive || b->shared_count > 0; }
static int sxn_borrow_acquire_shared(SxnBorrow *b) { if (b->exclusive) return 0; b->shared_count++; return 1; }
static int sxn_borrow_acquire_exclusive(SxnBorrow *b) { if (sxn_borrow_busy(b)) return 0; b->exclusive = 1; return 1; }
static void sxn_borrow_release_shared(SxnBorrow *b) { if (b->shared_count > 0) b->shared_count--; }
static void sxn_borrow_release_exclusive(SxnBorrow *b) { b->exclusive = 0; }

typedef struct ChunkNode {
    struct ChunkNode *next;
    uint8_t *data;
    size_t len;
    /* Only meaningful once this node has been handed to JS zero-copy via
       readBorrowed() (see SxnChunkView, below stream_cancel) -- a node
       still sitting in fs->head/fs->tail (the plain, copy-based read()
       path, unchanged by Task 4) never touches these. */
    SxnBorrow borrow;
    struct FetchState *owner;  /* fs this node is registered with, for fetch_chunks_free's deferred-free walk; NULL once that walk has run */
    int pending_free;          /* fetch_chunks_free wanted to free this node's data but a borrow was outstanding; a matching release() finishes the job instead */
    int retired;                /* data already freed (immediately, or via a deferred release) */
} ChunkNode;

/* The JS-visible handle readBorrowed() hands back. Deliberately not a
   plain Uint8Array: its bytes are reachable only while a borrow is held
   (withSharedBorrow/withExclusiveBorrow hand a scope-limited Uint8Array to
   the callback and detach it the instant the callback returns;
   acquireExclusive/acquireShared+release hand back a view that stays live
   until release() detaches it). That is what makes "hold a borrow across
   an await" a well-defined thing to test, and what makes
   fetch_chunks_free's defer-until-released logic below load-bearing:
   outside of an active borrow there is no way for JS to read node->data,
   so once no borrow is outstanding it is genuinely safe to free. */
typedef struct SxnChunkViewState {
    ChunkNode *node;
    JSValue held_buffer;   /* JS_UNDEFINED unless acquireExclusive()/acquireShared() is outstanding */
    int held_exclusive;
} SxnChunkViewState;

typedef struct FetchState {
    JSContext *ctx;
    CURL *easy;
    struct curl_slist *req_headers;
    char *request_body; size_t request_body_len;
    char *url;

    long status;
    char status_text[128];
    JSValue header_pairs;      /* flat [name, value, ...] JS array */
    int response_started;
    JSValue resolve_fetch, reject_fetch;

    ChunkNode *head, *tail;
    ChunkNode *borrowed;         /* chunks delivered via readBorrowed(), tracked so cancel()/teardown can find & defer-free them (see fetch_chunks_free) */
    JSValue pending_read_resolve, pending_read_reject;
    int has_pending_read;
    int pending_read_borrowed;   /* the outstanding pending read is a readBorrowed() call, not a plain read() */
    int done, errored, aborted;
    char error_msg[256];

    int refcount;
} FetchState;

static JSClassID sxn_stream_class_id;
/* sxn_chunkview_class_id is forward-declared near the top of this file (see
   conn_read_cb's needs); this is its one definition. */

static void fetch_state_incref(FetchState *fs) { fs->refcount++; }

static void fetch_chunks_free(FetchState *fs) {
    while (fs->head) { ChunkNode *n = fs->head; fs->head = n->next; free(n->data); free(n); }
    fs->tail = NULL;
    /* Chunks delivered zero-copy via readBorrowed() (SxnChunkView, below):
       this is the actual hazard Task 4 guards against -- a JS script can
       be mid-borrow (acquireExclusive()/withExclusiveBorrow()) on one of
       these the moment cancel()/abort()/final teardown reaches here. Free
       immediately only if unborrowed; otherwise defer to whichever
       release() brings that specific node's borrow count back to zero --
       tracked per node, so a borrow on one chunk never blocks freeing any
       other. */
    ChunkNode *n = fs->borrowed;
    fs->borrowed = NULL;
    while (n) {
        ChunkNode *next = n->next;
        n->owner = NULL; /* fs is done tracking it either way */
        if (sxn_borrow_busy(&n->borrow)) n->pending_free = 1;
        else { free(n->data); n->data = NULL; n->retired = 1; }
        n = next;
    }
}

static void fetch_state_decref(FetchState *fs) {
    if (--fs->refcount > 0) return;
    fetch_chunks_free(fs);
    JS_FreeValue(fs->ctx, fs->header_pairs);
    JS_FreeValue(fs->ctx, fs->resolve_fetch);
    JS_FreeValue(fs->ctx, fs->reject_fetch);
    JS_FreeValue(fs->ctx, fs->pending_read_resolve);
    JS_FreeValue(fs->ctx, fs->pending_read_reject);
    free(fs->request_body); free(fs->url);
    if (fs->req_headers) curl_slist_free_all(fs->req_headers);
    free(fs);
}

static JSValue make_named_error(JSContext *ctx, const char *name, const char *message) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, message));
    return err;
}

/* node->data's ownership never passes to QuickJS's ArrayBuffer machinery --
   it stays with the ChunkNode until fetch_chunks_free or the SxnChunkView
   wrapper's finalizer frees it (whichever runs last), so every zero-copy
   ArrayBuffer/Uint8Array built over it uses this no-op free_func. */
static void sxn_chunk_noop_free(JSRuntime *rt, void *opaque, void *ptr) { (void)rt; (void)opaque; (void)ptr; }

/* Takes ownership of `data` (n bytes this file already controls -- either a
   dequeued ChunkNode's buffer or a fresh copy of libcurl's transient
   write-callback buffer) and hands back a native SxnChunkView wrapper,
   registering the backing node with fs so cancel()/teardown can find and
   (if borrowed) defer-free it. */
static JSValue sxn_make_borrowed_chunk(JSContext *ctx, FetchState *fs, uint8_t *data, size_t len) {
    ChunkNode *node = calloc(1, sizeof(*node));
    node->data = data; node->len = len; node->owner = fs;
    node->next = fs->borrowed; fs->borrowed = node;
    SxnChunkViewState *st = calloc(1, sizeof(*st));
    st->node = node; st->held_buffer = JS_UNDEFINED;
    JSValue view = JS_NewObjectClass(ctx, sxn_chunkview_class_id);
    JS_SetOpaque(view, st);
    return view;
}

/* File-backed sibling of sxn_make_borrowed_chunk above: no FetchState owns
   this node (owner stays NULL, never linked into any fs->borrowed list --
   there is no stream cancel()/teardown event that needs to find and
   defer-free it). Its only teardown path is its own GC finalizer, which is
   safe by the same reasoning as sxn_chunkview_finalizer's doc comment: a
   borrow can only be outstanding while the JS BorrowedChunk object is
   reachable, so GC can never collect it mid-borrow. This is what lets
   Sxn.file(path).readBorrowed() hand back a chunk that can be borrowed
   (shared) once per served request, indefinitely, without re-reading the
   file -- the actual "safe zero-copy file serving" win. */
static JSValue sxn_make_owned_borrowed_chunk(JSContext *ctx, uint8_t *data, size_t len) {
    ChunkNode *node = calloc(1, sizeof(*node));
    node->data = data; node->len = len; node->owner = NULL; node->next = NULL;
    SxnChunkViewState *st = calloc(1, sizeof(*st));
    st->node = node; st->held_buffer = JS_UNDEFINED;
    JSValue view = JS_NewObjectClass(ctx, sxn_chunkview_class_id);
    JS_SetOpaque(view, st);
    return view;
}

/* Stops the transfer and releases curl's ownership share of fs. Idempotent
   (guarded by fs->easy) so it is safe from fetch_cancel, stream_cancel and
   normal completion alike. */
static void fetch_teardown_curl(FetchState *fs) {
    if (!fs->easy) return;
    curl_multi_remove_handle(sxn_curl_multi, fs->easy);
    curl_easy_cleanup(fs->easy);
    fs->easy = NULL;
    fetch_state_decref(fs);
}

/* Consumes result_or_error (one ref). */
static void fetch_settle_pending_read(FetchState *fs, JSValue result_or_error, int is_error) {
    JSContext *ctx = fs->ctx;
    JSValue fn = is_error ? fs->pending_read_reject : fs->pending_read_resolve;
    JSValue args[1] = { result_or_error };
    JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, args);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, result_or_error);
    JS_FreeValue(ctx, fs->pending_read_resolve); JS_FreeValue(ctx, fs->pending_read_reject);
    fs->pending_read_resolve = JS_UNDEFINED; fs->pending_read_reject = JS_UNDEFINED;
    fs->has_pending_read = 0;
}

/* Real cancellation: tears down the libcurl handle (closing the socket),
   not just a JS-side flag. Used by both AbortSignal ('abort' -> __abort)
   and would-be direct callers; idempotent via the aborted/done guard. */
static void fetch_cancel(FetchState *fs) {
    if (fs->aborted || fs->done) return;
    fs->aborted = 1; fs->done = 1;
    JSContext *ctx = fs->ctx;
    JSValue err = make_named_error(ctx, "AbortError", "The operation was aborted.");
    if (!fs->response_started) {
        JSValue args[1] = { err };
        JSValue ret = JS_Call(ctx, fs->reject_fetch, JS_UNDEFINED, 1, args); JS_FreeValue(ctx, ret);
    }
    if (fs->has_pending_read) {
        fetch_settle_pending_read(fs, JS_DupValue(ctx, err), 1);
    } else {
        fs->errored = 1;
        snprintf(fs->error_msg, sizeof(fs->error_msg), "The operation was aborted.");
    }
    JS_FreeValue(ctx, err);
    fetch_chunks_free(fs);
    fetch_teardown_curl(fs);
}

static void start_response(FetchState *fs) {
    fs->response_started = 1;
    JSContext *ctx = fs->ctx;
    JSValue head = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, head, "status", JS_NewInt32(ctx, (int)fs->status));
    JS_SetPropertyStr(ctx, head, "statusText", JS_NewString(ctx, fs->status_text));
    const char *effective_url = fs->url;
    char *curl_url = NULL;
    if (fs->easy && curl_easy_getinfo(fs->easy, CURLINFO_EFFECTIVE_URL, &curl_url) == CURLE_OK && curl_url)
        effective_url = curl_url;
    JS_SetPropertyStr(ctx, head, "url", JS_NewString(ctx, effective_url));
    if (JS_IsUndefined(fs->header_pairs)) fs->header_pairs = JS_NewArray(ctx);
    JS_SetPropertyStr(ctx, head, "headers", fs->header_pairs);
    fs->header_pairs = JS_UNDEFINED; /* ownership transferred into head.headers */
    JSValue args[1] = { head };
    JSValue ret = JS_Call(ctx, fs->resolve_fetch, JS_UNDEFINED, 1, args); JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, head);
}

static size_t fetch_header_cb(char *buf, size_t size, size_t nitems, void *userdata) {
    FetchState *fs = userdata;
    size_t n = size * nitems;
    JSContext *ctx = fs->ctx;
    if (n >= 5 && !strncmp(buf, "HTTP/", 5)) {
        /* Start of a header block: redirects mean this fires more than
           once, so reset and keep only the most recent (final) block. */
        JS_FreeValue(ctx, fs->header_pairs);
        fs->header_pairs = JS_NewArray(ctx);
        long code = 0; int consumed = 0;
        sscanf(buf, "%*s %ld%n", &code, &consumed);
        fs->status = code;
        fs->status_text[0] = 0;
        if (consumed > 0 && (size_t)consumed < n) {
            size_t start = (size_t)consumed; while (start < n && buf[start] == ' ') start++;
            size_t end = n; while (end > start && (buf[end - 1] == '\r' || buf[end - 1] == '\n')) end--;
            size_t len = end > start ? end - start : 0;
            if (len >= sizeof(fs->status_text)) len = sizeof(fs->status_text) - 1;
            memcpy(fs->status_text, buf + start, len); fs->status_text[len] = 0;
        }
        return n;
    }
    if (n <= 2) return n; /* blank line: end of this header block */
    char *colon = memchr(buf, ':', n);
    if (!colon) return n;
    size_t namelen = (size_t)(colon - buf);
    char *v = colon + 1; size_t vlen = (size_t)((buf + n) - v);
    while (vlen && (*v == ' ' || *v == '\t')) { v++; vlen--; }
    while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n')) vlen--;
    if (JS_IsUndefined(fs->header_pairs)) fs->header_pairs = JS_NewArray(ctx);
    uint32_t idx = 0; JSValue lenv = JS_GetPropertyStr(ctx, fs->header_pairs, "length"); JS_ToUint32(ctx, &idx, lenv); JS_FreeValue(ctx, lenv);
    JS_SetPropertyUint32(ctx, fs->header_pairs, idx, JS_NewStringLen(ctx, buf, namelen));
    JS_SetPropertyUint32(ctx, fs->header_pairs, idx + 1, JS_NewStringLen(ctx, v, vlen));
    return n;
}

static size_t fetch_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    FetchState *fs = userdata;
    size_t n = size * nmemb;
    if (fs->aborted) return 0; /* CURLE_WRITE_ERROR; harmless, handle is already being torn down */
    JSContext *ctx = fs->ctx;
    if (!fs->response_started) start_response(fs);
    if (fs->has_pending_read) {
        JSValue chunk;
        if (fs->pending_read_borrowed) {
            /* ptr is libcurl's own transient buffer -- not safe to alias
               past this callback's return (libcurl owns and reuses it), so
               this one copy is unavoidable regardless of borrowing. The
               zero-copy win readBorrowed() provides is cutting the SECOND
               copy (native buffer -> JS-owned array) that the plain
               JS_NewUint8ArrayCopy below still does. */
            uint8_t *copy = malloc(n ? n : 1); memcpy(copy, ptr, n);
            chunk = sxn_make_borrowed_chunk(ctx, fs, copy, n);
        } else {
            chunk = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)ptr, n);
        }
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "value", chunk);
        JS_SetPropertyStr(ctx, result, "done", JS_FALSE);
        fs->pending_read_borrowed = 0;
        fetch_settle_pending_read(fs, result, 0);
    } else {
        ChunkNode *node = malloc(sizeof(*node));
        node->data = malloc(n ? n : 1); memcpy(node->data, ptr, n); node->len = n; node->next = NULL;
        if (fs->tail) fs->tail->next = node; else fs->head = node;
        fs->tail = node;
    }
    return n;
}

static void sxn_curl_check_multi_info(void) {
    CURLMsg *msg; int pending;
    while ((msg = curl_multi_info_read(sxn_curl_multi, &pending))) {
        if (msg->msg != CURLMSG_DONE) continue;
        FetchState *fs = NULL;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &fs);
        if (!fs || fs->aborted) continue; /* already torn down by fetch_cancel/stream_cancel */
        JSContext *ctx = fs->ctx;
        CURLcode code = msg->data.result;
        if (!fs->response_started) {
            if (code == CURLE_OK) {
                start_response(fs);
            } else {
                JSValue err = make_named_error(ctx, "TypeError", curl_easy_strerror(code));
                JSValue args[1] = { err };
                JSValue ret = JS_Call(ctx, fs->reject_fetch, JS_UNDEFINED, 1, args); JS_FreeValue(ctx, ret);
                JS_FreeValue(ctx, err);
            }
        }
        fs->done = 1;
        if (fs->has_pending_read) {
            if (code == CURLE_OK) {
                JSValue result = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, result, "value", JS_UNDEFINED);
                JS_SetPropertyStr(ctx, result, "done", JS_TRUE);
                fetch_settle_pending_read(fs, result, 0);
            } else {
                fetch_settle_pending_read(fs, make_named_error(ctx, "TypeError", curl_easy_strerror(code)), 1);
            }
        } else if (code != CURLE_OK && fs->response_started) {
            fs->errored = 1;
            snprintf(fs->error_msg, sizeof(fs->error_msg), "%s", curl_easy_strerror(code));
        }
        fetch_teardown_curl(fs);
    }
}

static JSValue stream_get_reader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    if (!JS_GetOpaque2(ctx, this_val, sxn_stream_class_id)) return JS_EXCEPTION;
    return JS_DupValue(ctx, this_val); /* single-reader simplification: the stream is its own reader */
}

static JSValue stream_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    FetchState *fs = JS_GetOpaque2(ctx, this_val, sxn_stream_class_id);
    if (!fs) return JS_EXCEPTION;
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) return promise;
    if (fs->head) {
        ChunkNode *node = fs->head; fs->head = node->next; if (!fs->head) fs->tail = NULL;
        JSValue chunk = JS_NewUint8ArrayCopy(ctx, node->data, node->len);
        free(node->data); free(node);
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "value", chunk);
        JS_SetPropertyStr(ctx, result, "done", JS_FALSE);
        JSValue args[1] = { result };
        JSValue ret = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, args); JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, result);
    } else if (fs->errored) {
        JSValue err = make_named_error(ctx, fs->aborted ? "AbortError" : "TypeError", fs->error_msg);
        JSValue args[1] = { err };
        JSValue ret = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, args); JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, err);
        fs->errored = 0;
    } else if (fs->done) {
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "value", JS_UNDEFINED);
        JS_SetPropertyStr(ctx, result, "done", JS_TRUE);
        JSValue args[1] = { result };
        JSValue ret = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, args); JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, result);
    } else {
        fs->pending_read_resolve = funcs[0]; fs->pending_read_reject = funcs[1];
        fs->has_pending_read = 1; fs->pending_read_borrowed = 0;
        return promise;
    }
    JS_FreeValue(ctx, funcs[0]); JS_FreeValue(ctx, funcs[1]);
    return promise;
}

/* Additive zero-copy sibling of read() above: hands back a borrow-guarded
   BorrowedChunk (SxnChunkView) instead of a copied Uint8Array, so read()
   itself -- and everything built on it, including Response.body's default
   consumption -- is completely unaffected by Task 4. */
static JSValue stream_read_borrowed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    FetchState *fs = JS_GetOpaque2(ctx, this_val, sxn_stream_class_id);
    if (!fs) return JS_EXCEPTION;
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) return promise;
    if (fs->head) {
        ChunkNode *n = fs->head; fs->head = n->next; if (!fs->head) fs->tail = NULL;
        JSValue view = sxn_make_borrowed_chunk(ctx, fs, n->data, n->len);
        free(n); /* only the queue wrapper struct -- data ownership moved into the new borrowed node */
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "value", view);
        JS_SetPropertyStr(ctx, result, "done", JS_FALSE);
        JSValue args[1] = { result };
        JSValue ret = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, args); JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, result);
    } else if (fs->errored) {
        JSValue err = make_named_error(ctx, fs->aborted ? "AbortError" : "TypeError", fs->error_msg);
        JSValue args[1] = { err };
        JSValue ret = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, args); JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, err);
        fs->errored = 0;
    } else if (fs->done) {
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "value", JS_UNDEFINED);
        JS_SetPropertyStr(ctx, result, "done", JS_TRUE);
        JSValue args[1] = { result };
        JSValue ret = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, args); JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, result);
    } else {
        fs->pending_read_resolve = funcs[0]; fs->pending_read_reject = funcs[1];
        fs->has_pending_read = 1; fs->pending_read_borrowed = 1;
        return promise;
    }
    JS_FreeValue(ctx, funcs[0]); JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static JSValue stream_cancel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    FetchState *fs = JS_GetOpaque2(ctx, this_val, sxn_stream_class_id);
    if (fs && !fs->done) { fs->done = 1; fetch_chunks_free(fs); fetch_teardown_curl(fs); }
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (!JS_IsException(promise)) {
        JSValue args[1] = { JS_UNDEFINED };
        JSValue ret = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, args); JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, funcs[0]); JS_FreeValue(ctx, funcs[1]);
    return promise;
}

/* Bound to AbortSignal's 'abort' listener from fetch()'s JS wrapper; the
   only path that actually reaches into libcurl and tears the transfer
   down, as opposed to merely flipping a JS-visible flag. */
static JSValue stream_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    FetchState *fs = JS_GetOpaque2(ctx, this_val, sxn_stream_class_id);
    if (fs) fetch_cancel(fs);
    return JS_UNDEFINED;
}

static void sxn_stream_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    FetchState *fs = JS_GetOpaque(val, sxn_stream_class_id);
    if (fs) fetch_state_decref(fs);
}

static JSClassDef sxn_stream_class_def = { "SxnBodyStream", .finalizer = sxn_stream_finalizer };

static const JSCFunctionListEntry sxn_stream_proto_funcs[] = {
    JS_CFUNC_DEF("getReader", 0, stream_get_reader),
    JS_CFUNC_DEF("read", 0, stream_read),
    JS_CFUNC_DEF("readBorrowed", 0, stream_read_borrowed),
    JS_CFUNC_DEF("cancel", 1, stream_cancel),
    JS_CFUNC_DEF("__abort", 0, stream_abort),
};

/* --- SxnChunkView ("BorrowedChunk"): the JS-visible handle for a
   readBorrowed() chunk. See SxnChunkViewState's doc comment above
   FetchState for why bytes are only reachable while a borrow is held. */

static JSValue sxn_throw_borrow_conflict(JSContext *ctx, const char *action) {
    JSValue err = make_named_error(ctx, "BorrowConflictError", action);
    JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, sxfe_diagnostic_name(SX2002_BORROW_CONFLICT)));
    return JS_Throw(ctx, err);
}

static void sxn_chunkview_finalizer(JSRuntime *rt, JSValue val) {
    SxnChunkViewState *st = JS_GetOpaque(val, sxn_chunkview_class_id);
    if (!st) return;
    ChunkNode *node = st->node;
    if (node) {
        /* A borrow can never be outstanding here: it is only ever held
           synchronously inside a call reachable from this very object, so
           GC cannot be collecting us mid-borrow. */
        if (node->owner) {
            ChunkNode **link = &node->owner->borrowed;
            while (*link && *link != node) link = &(*link)->next;
            if (*link) *link = node->next;
        }
        if (!node->retired) free(node->data);
        free(node);
    }
    if (!JS_IsUndefined(st->held_buffer)) JS_FreeValueRT(rt, st->held_buffer);
    free(st);
}

static JSClassDef sxn_chunkview_class_def = { "BorrowedChunk", .finalizer = sxn_chunkview_finalizer };

/* Acquires (shared or exclusive) and hands back a zero-copy Uint8Array plus
   its backing ArrayBuffer (the latter is what callers detach to end the
   borrow's visibility). Returns JS_EXCEPTION (SX2002_BORROW_CONFLICT
   already thrown) on conflict, or a plain TypeError if the node has
   already been retired (native-side freed by fetch_chunks_free while
   unborrowed). */
static JSValue sxn_chunkview_acquire(JSContext *ctx, SxnChunkViewState *st, int exclusive, JSValue *out_buffer) {
    ChunkNode *node = st->node;
    if (!node || node->retired) { JS_ThrowTypeError(ctx, "chunk is no longer available (stream was cancelled or torn down)"); return JS_EXCEPTION; }
    int ok = exclusive ? sxn_borrow_acquire_exclusive(&node->borrow) : sxn_borrow_acquire_shared(&node->borrow);
    if (!ok) return sxn_throw_borrow_conflict(ctx, exclusive ? "acquireExclusive: chunk already borrowed" : "acquireShared: chunk already exclusively borrowed");
    JSValue u8 = JS_NewUint8Array(ctx, node->data, node->len, sxn_chunk_noop_free, NULL, false);
    *out_buffer = JS_GetTypedArrayBuffer(ctx, u8, NULL, NULL, NULL);
    return u8;
}

/* Finishes a release: if fetch_chunks_free already wanted this node's data
   freed (deferred because a borrow was outstanding then), and the borrow
   count has now genuinely reached zero, do it now. */
static void sxn_chunkview_release_common(ChunkNode *node) {
    if (node->pending_free && !sxn_borrow_busy(&node->borrow)) {
        node->pending_free = 0;
        free(node->data); node->data = NULL; node->retired = 1;
    }
}

/* conn_read_cb's forward-declared release helper (see the top of this file):
   releases a shared borrow acquired via sxn_chunkview_acquire, mirroring the
   shared-release half of sxn_chunkview_with_borrow. Its caller detaches the
   ArrayBuffer view itself (a public API, no incomplete-type issue) before
   calling this. */
static void sxn_chunkview_release_shared(SxnChunkViewState *st) {
    sxn_borrow_release_shared(&st->node->borrow);
    sxn_chunkview_release_common(st->node);
}

static JSValue sxn_chunkview_with_borrow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int exclusive) {
    SxnChunkViewState *st = JS_GetOpaque2(ctx, this_val, sxn_chunkview_class_id);
    if (!st) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "expected a function");
    JSValue buffer;
    JSValue u8 = sxn_chunkview_acquire(ctx, st, exclusive, &buffer);
    if (JS_IsException(u8)) return JS_EXCEPTION;
    JSValue args[1] = { u8 };
    JSValue ret = JS_Call(ctx, argv[0], JS_UNDEFINED, 1, args);
    int had_exception = JS_IsException(ret);
    JSValue exc = had_exception ? JS_GetException(ctx) : JS_UNDEFINED;
    JS_FreeValue(ctx, ret); JS_FreeValue(ctx, u8);
    JS_DetachArrayBuffer(ctx, buffer); /* ends the borrow's visibility: fn can't retain a live reference past this call */
    JS_FreeValue(ctx, buffer);
    if (exclusive) sxn_borrow_release_exclusive(&st->node->borrow); else sxn_borrow_release_shared(&st->node->borrow);
    sxn_chunkview_release_common(st->node);
    if (had_exception) return JS_Throw(ctx, exc);
    return JS_UNDEFINED;
}

static JSValue sxn_chunkview_with_shared(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return sxn_chunkview_with_borrow(ctx, this_val, argc, argv, 0); }
static JSValue sxn_chunkview_with_exclusive(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return sxn_chunkview_with_borrow(ctx, this_val, argc, argv, 1); }

/* Explicit acquire()/release() pair, for a borrow that needs to span an
   await -- the with*Borrow pair above can't do that by construction (fn
   runs and is released synchronously), which is deliberate: it makes
   "holding a borrow with no clear release point" hard to write by
   accident. This explicit pair is for the cases that genuinely need to
   span a suspension point. */
static JSValue sxn_chunkview_acquire_x(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int exclusive) {
    (void)argc; (void)argv;
    SxnChunkViewState *st = JS_GetOpaque2(ctx, this_val, sxn_chunkview_class_id);
    if (!st) return JS_EXCEPTION;
    if (!JS_IsUndefined(st->held_buffer)) return sxn_throw_borrow_conflict(ctx, "this BorrowedChunk already has an outstanding acquire(); call release() first");
    JSValue buffer;
    JSValue u8 = sxn_chunkview_acquire(ctx, st, exclusive, &buffer);
    if (JS_IsException(u8)) return JS_EXCEPTION;
    st->held_buffer = buffer; st->held_exclusive = exclusive;
    return u8;
}
static JSValue sxn_chunkview_acquire_shared_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return sxn_chunkview_acquire_x(ctx, this_val, argc, argv, 0); }
static JSValue sxn_chunkview_acquire_exclusive_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return sxn_chunkview_acquire_x(ctx, this_val, argc, argv, 1); }

static JSValue sxn_chunkview_release_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    SxnChunkViewState *st = JS_GetOpaque2(ctx, this_val, sxn_chunkview_class_id);
    if (!st) return JS_EXCEPTION;
    if (JS_IsUndefined(st->held_buffer)) return JS_ThrowTypeError(ctx, "release(): no outstanding acquire() on this BorrowedChunk");
    JS_DetachArrayBuffer(ctx, st->held_buffer);
    JS_FreeValue(ctx, st->held_buffer); st->held_buffer = JS_UNDEFINED;
    if (st->held_exclusive) sxn_borrow_release_exclusive(&st->node->borrow); else sxn_borrow_release_shared(&st->node->borrow);
    sxn_chunkview_release_common(st->node);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry sxn_chunkview_proto_funcs[] = {
    JS_CFUNC_DEF("withSharedBorrow", 1, sxn_chunkview_with_shared),
    JS_CFUNC_DEF("withExclusiveBorrow", 1, sxn_chunkview_with_exclusive),
    JS_CFUNC_DEF("acquireShared", 0, sxn_chunkview_acquire_shared_fn),
    JS_CFUNC_DEF("acquireExclusive", 0, sxn_chunkview_acquire_exclusive_fn),
    JS_CFUNC_DEF("release", 0, sxn_chunkview_release_fn),
};

/* Low-level primitive wrapped by fetch()/Request/Response/Headers in
   bootstrap.js. Returns { promise, stream } synchronously: promise
   resolves with response head info once headers arrive, stream is the
   body reader (see sxn_stream_proto_funcs) usable immediately and also
   used internally to wire AbortSignal. */
static JSValue js_sxn_fetch_raw(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    sxn_curl_multi_init();
    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!url) return JS_ThrowTypeError(ctx, "fetch requires a URL");
    const char *method = (argc > 1 && !JS_IsUndefined(argv[1])) ? JS_ToCString(ctx, argv[1]) : NULL;

    struct curl_slist *headers = NULL;
    if (argc > 2 && JS_IsArray(argv[2])) {
        uint32_t len = 0; JSValue lenv = JS_GetPropertyStr(ctx, argv[2], "length"); JS_ToUint32(ctx, &len, lenv); JS_FreeValue(ctx, lenv);
        for (uint32_t i = 0; i + 1 < len; i += 2) {
            JSValue nv = JS_GetPropertyUint32(ctx, argv[2], i);
            JSValue vv = JS_GetPropertyUint32(ctx, argv[2], i + 1);
            const char *name = JS_ToCString(ctx, nv), *value = JS_ToCString(ctx, vv);
            if (name && value) {
                char line[1024]; snprintf(line, sizeof(line), "%s: %s", name, value);
                headers = curl_slist_append(headers, line);
            }
            JS_FreeCString(ctx, name); JS_FreeCString(ctx, value);
            JS_FreeValue(ctx, nv); JS_FreeValue(ctx, vv);
        }
    }

    /* argv[4], when given, is the redirect mode. */
    long follow = 1;
    if (argc > 4 && JS_IsString(argv[4])) {
        const char *mode = JS_ToCString(ctx, argv[4]);
        if (mode) { follow = strcmp(mode, "follow") == 0; JS_FreeCString(ctx, mode); }
    }
    char *request_body = NULL; size_t request_body_len = 0;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
        /* Counted, not NUL terminated: a request body may contain a 0x00
           byte, and strlen would send everything before it and drop the
           rest. */
        size_t len = 0;
        const char *b = JS_ToCStringLen(ctx, &len, argv[3]);
        if (b) { request_body = malloc(len + 1); memcpy(request_body, b, len); request_body[len] = 0; request_body_len = len; }
        JS_FreeCString(ctx, b);
    }

    CURL *easy = curl_easy_init();
    if (!easy) {
        JS_FreeCString(ctx, url); JS_FreeCString(ctx, method); free(request_body);
        if (headers) curl_slist_free_all(headers);
        return JS_ThrowInternalError(ctx, "curl initialization failed");
    }

    FetchState *fs = calloc(1, sizeof(*fs));
    fs->ctx = ctx; fs->easy = easy; fs->refcount = 1;
    fs->url = strdup(url);
    fs->req_headers = headers;
    fs->request_body = request_body; fs->request_body_len = request_body_len;
    fs->header_pairs = JS_UNDEFINED;
    fs->pending_read_resolve = JS_UNDEFINED; fs->pending_read_reject = JS_UNDEFINED;

    curl_easy_setopt(easy, CURLOPT_URL, url);
    /* Fetch's `redirect` option: "follow" is the default, "manual" hands the
       3xx back as it arrived, and "error" is rejected by the JS wrapper --
       both need the transfer to stop at the first response. */
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "sxn/0.0.1");
    curl_easy_setopt(easy, CURLOPT_PRIVATE, fs);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, fetch_header_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, fs);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, fetch_write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, fs);
    if (headers) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    if (method) curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);
    /* A HEAD response carries the length of a body it does not send, so the
       transfer has to be told there is nothing to wait for. Without this a
       fetch of a HEAD hung until the server closed the connection. */
    if (method && !sxn_strcasecmp(method, "HEAD")) curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    if (request_body) {
        /* POSTFIELDSIZE must be set BEFORE COPYPOSTFIELDS: libcurl reads the
           size at the moment the fields are copied. Setting it afterwards
           left curl expecting an upload it had no read callback for, so it
           connected and then sent nothing at all -- every POST, PUT and PATCH
           hung until it timed out. */
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)request_body_len);
        curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, request_body);
    }
    JS_FreeCString(ctx, url); JS_FreeCString(ctx, method);

    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    fs->resolve_fetch = funcs[0]; fs->reject_fetch = funcs[1];

    JSValue stream_obj = JS_NewObjectClass(ctx, sxn_stream_class_id);
    fetch_state_incref(fs); /* the JS wrapper object's own share, released by its finalizer */
    JS_SetOpaque(stream_obj, fs);

    curl_multi_add_handle(sxn_curl_multi, easy);

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "promise", promise);
    JS_SetPropertyStr(ctx, out, "stream", stream_obj);
    return out;
}

/* --- setTimeout/setInterval: real uv_timer_t handles on sxn_loop() (the
   same loop the curl_multi/serve/fs primitives above already share), not a
   second timer mechanism and not queueMicrotask/busy-poll based -- so the
   process genuinely sleeps between fires. bootstrap.js's setTimeout/
   setInterval/clearTimeout/clearInterval wrap this with Node's delay/arg
   handling; extra args are just closed over there, so this primitive only
   needs to know the callback, the delay, and whether to repeat. */
typedef struct SxnTimer {
    uv_timer_t handle;
    JSContext *ctx;
    JSValue callback;
    int64_t id;
    int repeat;
    struct SxnTimer *prev, *next;
} SxnTimer;

static SxnTimer *sxn_timers_head = NULL;
static int64_t sxn_timer_next_id = 1;

static void sxn_timer_close_cb(uv_handle_t *handle) {
    SxnTimer *timer = (SxnTimer *)handle->data;
    JS_FreeValue(timer->ctx, timer->callback);
    free(timer);
}

/* Unlinks from the active-timer list and tears the handle down; every call
   site (clearTimer, and a fired one-shot's own callback below) reaches this
   at most once per timer, since a cleared/finished timer is no longer in
   the list for a second clearTimer call to find. */
static void sxn_timer_stop(SxnTimer *timer) {
    if (timer->prev) timer->prev->next = timer->next; else sxn_timers_head = timer->next;
    if (timer->next) timer->next->prev = timer->prev;
    uv_timer_stop(&timer->handle);
    uv_close((uv_handle_t *)&timer->handle, sxn_timer_close_cb);
}

static void sxn_timer_cb(uv_timer_t *handle) {
    SxnTimer *timer = (SxnTimer *)handle->data;
    JSContext *ctx = timer->ctx;
    JSValue ret = JS_Call(ctx, timer->callback, JS_UNDEFINED, 0, NULL);
    /* A throwing callback must not crash the process or corrupt engine
       state -- report it the same way main.c reports an uncaught top-level
       exception, then keep going (matches js_serve/fetch's convention of
       never letting a JS_Call exception propagate past this dispatch). */
    if (JS_IsException(ret)) js_std_dump_error(ctx);
    JS_FreeValue(ctx, ret);
    if (!timer->repeat) sxn_timer_stop(timer); /* a repeat timer keeps firing on libuv's own schedule until cleared */
}

static JSValue sxn_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "expected a function");
    double delay_ms = 0;
    if (argc > 1 && JS_ToFloat64(ctx, &delay_ms, argv[1]) < 0) return JS_EXCEPTION;
    if (!(delay_ms >= 0)) delay_ms = 0;
    int repeat = argc > 2 && JS_ToBool(ctx, argv[2]);

    SxnTimer *timer = calloc(1, sizeof(*timer));
    timer->ctx = ctx;
    timer->callback = JS_DupValue(ctx, argv[0]);
    timer->id = sxn_timer_next_id++;
    timer->repeat = repeat;
    timer->next = sxn_timers_head;
    if (sxn_timers_head) sxn_timers_head->prev = timer;
    timer->prev = NULL;
    sxn_timers_head = timer;

    uv_timer_init(sxn_loop(), &timer->handle);
    timer->handle.data = timer;
    uint64_t ms = (uint64_t)delay_ms;
    uv_timer_start(&timer->handle, sxn_timer_cb, ms, repeat ? ms : 0);
    return JS_NewInt64(ctx, timer->id);
}

static JSValue sxn_clear_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    int64_t id;
    if (JS_ToInt64(ctx, &id, argv[0])) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_UNDEFINED; } /* non-numeric id: no-op, never throw */
    for (SxnTimer *t = sxn_timers_head; t; t = t->next) {
        if (t->id == id) { sxn_timer_stop(t); break; } /* not found (already fired/bogus id): no-op */
    }
    return JS_UNDEFINED;
}

static uint64_t sxn_time_origin_ns;

static JSValue sxn_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)(uv_hrtime() - sxn_time_origin_ns) / 1e6);
}

static JSValue sxn_random_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int64_t n = 0;
    if (argc < 1 || JS_ToInt64(ctx, &n, argv[0]) || n < 0 || n > (1 << 20))
        return JS_ThrowRangeError(ctx, "invalid byte length for getRandomValues");
    uint8_t *buf = malloc(n ? (size_t)n : 1);
    if (RAND_bytes(buf, (int)n) != 1) { free(buf); return JS_ThrowInternalError(ctx, "RAND_bytes failed"); }
    JSValue result = JS_NewUint8ArrayCopy(ctx, buf, (size_t)n);
    free(buf);
    return result;
}


/* HMAC, from the library that already does the digests. It was built here in
   JavaScript out of two padded key buffers and three digest calls, with a
   Uint8Array allocated per update. */

/* Buffer#compare: Node orders by unsigned byte value, then by length. A
   JavaScript loop compared a byte at a time; memcmp compares a word at a
   time and is what the C library is for. */
static JSValue sxn_bytes_compare(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    size_t a_len = 0, b_len = 0;
    uint8_t *a = argc > 0 ? JS_GetUint8Array(ctx, &a_len, argv[0]) : NULL;
    uint8_t *b = argc > 1 ? JS_GetUint8Array(ctx, &b_len, argv[1]) : NULL;
    if (!a || !b) return JS_ThrowTypeError(ctx, "compare expects two Uint8Arrays");
    size_t n = a_len < b_len ? a_len : b_len;
    int rc = n ? memcmp(a, b, n) : 0;
    if (rc == 0) rc = a_len == b_len ? 0 : (a_len < b_len ? -1 : 1);
    return JS_NewInt32(ctx, rc < 0 ? -1 : (rc > 0 ? 1 : 0));
}

static JSValue sxn_hmac(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *algo = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!algo) return JS_EXCEPTION;
    size_t key_len = 0, data_len = 0;
    uint8_t *key = argc > 1 ? JS_GetUint8Array(ctx, &key_len, argv[1]) : NULL;
    uint8_t *data = argc > 2 ? JS_GetUint8Array(ctx, &data_len, argv[2]) : NULL;
    if (!key || !data) { JS_FreeCString(ctx, algo); return JS_ThrowTypeError(ctx, "hmac expects two Uint8Arrays"); }
    char normalized[32]; size_t j = 0;
    for (size_t i = 0; algo[i] && j + 1 < sizeof(normalized); i++)
        if (algo[i] != '-') normalized[j++] = (char)tolower((unsigned char)algo[i]);
    normalized[j] = 0;
    const EVP_MD *md = EVP_get_digestbyname(normalized);
    JS_FreeCString(ctx, algo);
    if (!md) return JS_ThrowTypeError(ctx, "unsupported digest algorithm");
    uint8_t out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    /* An empty key is legal and HMAC() wants a non-NULL pointer for it. */
    if (!HMAC(md, key_len ? (const void *)key : (const void *)"", (int)key_len,
              data, data_len, out, &out_len))
        return JS_ThrowInternalError(ctx, "hmac failed");
    return JS_NewUint8ArrayCopy(ctx, out, out_len);
}

/* Comparison that takes the same time whether the bytes match or not, which
   is the whole point of it and is not something JavaScript can promise. */
static JSValue sxn_timing_safe_equal(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    size_t a_len = 0, b_len = 0;
    uint8_t *a = argc > 0 ? JS_GetUint8Array(ctx, &a_len, argv[0]) : NULL;
    uint8_t *b = argc > 1 ? JS_GetUint8Array(ctx, &b_len, argv[1]) : NULL;
    if (!a || !b) return JS_ThrowTypeError(ctx, "timingSafeEqual expects two Uint8Arrays");
    if (a_len != b_len) return JS_ThrowRangeError(ctx, "input length mismatch");
    return JS_NewBool(ctx, CRYPTO_memcmp(a, b, a_len) == 0);
}

static JSValue sxn_digest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *algo = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!algo) return JS_EXCEPTION;
    size_t inlen = 0;
    uint8_t *input = argc > 1 ? JS_GetUint8Array(ctx, &inlen, argv[1]) : NULL;
    if (!input) { JS_FreeCString(ctx, algo); return JS_ThrowTypeError(ctx, "digest expects a Uint8Array"); }
    char normalized[32]; size_t j = 0;
    for (size_t i = 0; algo[i] && j + 1 < sizeof(normalized); i++)
        if (algo[i] != '-') normalized[j++] = (char)tolower((unsigned char)algo[i]);
    normalized[j] = 0;
    const EVP_MD *md = EVP_get_digestbyname(normalized);
    JS_FreeCString(ctx, algo);
    if (!md) return JS_ThrowTypeError(ctx, "unsupported digest algorithm");
    uint8_t out[EVP_MAX_MD_SIZE]; unsigned int outlen = 0;
    if (!EVP_Digest(input, inlen, out, &outlen, md, NULL)) return JS_ThrowInternalError(ctx, "digest failed");
    return JS_NewUint8ArrayCopy(ctx, out, outlen);
}

/* TextEncoder.encode() native primitive (bootstrap.js's TextEncoder wraps
   this). JS_NewUint8ArrayFromString transcodes straight into the Uint8Array's
   backing store in one pass -- no intermediate JSString, no second copy
   (the previous JS_ToCStringLen2 + JS_NewUint8ArrayCopy pair wrote the bytes
   twice: once into a throwaway transcoded string, once copying that into the
   final buffer). Encoding (surrogate-pair combining, unmatched-surrogate
   handling) is byte-identical to before -- same transcode loop, just with
   the write going directly to its final home. */
/* Bound directly as TextEncoder.prototype.encode (see bootstrap.js) -- the
   undefined->empty check lives here so no JS wrapper frame runs per call.
   Non-strings coerce via ToString inside JS_NewUint8ArrayFromString; note
   symbols therefore throw TypeError, matching Node/WHATWG TextEncoder
   (the old JS wrapper's String() would have stringified them). */
static JSValue sxn_utf8_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_NewUint8ArrayCopy(ctx, (const uint8_t *)"", 0);
    return JS_NewUint8ArrayFromString(ctx, argv[0]);
}

/* TextEncoder.encodeInto() native primitive. The JS version allocated a whole
   encoded copy via encode(), then a subarray view, then copied into the
   destination -- the exact allocation encodeInto exists to avoid. This writes
   straight into the caller's buffer.

   It also fixes two spec bugs in that version: `read` must be the number of
   UTF-16 code units actually consumed (not the whole input length) when the
   destination fills up, and a partial UTF-8 sequence must never be written --
   encoding stops at the last code point that fits whole. */
static JSValue sxn_utf8_encode_into(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "encodeInto(source, destination) requires two arguments");
    size_t dest_len = 0;
    uint8_t *dest = JS_GetUint8Array(ctx, &dest_len, argv[1]);
    if (!dest) return JS_EXCEPTION;
    size_t src_len = 0;
    const uint16_t *src = JS_ToCStringLenUTF16(ctx, &src_len, argv[0]);
    if (!src) return JS_EXCEPTION;

    size_t read = 0, written = 0;
    while (read < src_len) {
        uint32_t c = src[read];
        size_t consumed = 1, need;
        if (c < 0x80) need = 1;
        else if (c < 0x800) need = 2;
        else if (c >= 0xd800 && c <= 0xdbff && read + 1 < src_len &&
                 src[read + 1] >= 0xdc00 && src[read + 1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (src[read + 1] - 0xdc00);
            consumed = 2; need = 4;
        } else {
            /* Unpaired surrogate -> U+FFFD, matching encode() and the WHATWG
               encoding standard. */
            if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
            need = 3;
        }
        if (written + need > dest_len) break; /* never split a code point */
        switch (need) {
        case 1: dest[written++] = (uint8_t)c; break;
        case 2: dest[written++] = (uint8_t)(0xc0 | (c >> 6));
                dest[written++] = (uint8_t)(0x80 | (c & 0x3f)); break;
        case 3: dest[written++] = (uint8_t)(0xe0 | (c >> 12));
                dest[written++] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
                dest[written++] = (uint8_t)(0x80 | (c & 0x3f)); break;
        default: dest[written++] = (uint8_t)(0xf0 | (c >> 18));
                dest[written++] = (uint8_t)(0x80 | ((c >> 12) & 0x3f));
                dest[written++] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
                dest[written++] = (uint8_t)(0x80 | (c & 0x3f)); break;
        }
        read += consumed;
    }
    JS_FreeCStringUTF16(ctx, src);
    JSValue res = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res, "read", JS_NewUint32(ctx, (uint32_t)read));
    JS_SetPropertyStr(ctx, res, "written", JS_NewUint32(ctx, (uint32_t)written));
    return res;
}

/* Buffer.from(string, "utf-8") primitive: same one-pass encode as
   sxn_utf8_encode, but goes straight to the bare ArrayBuffer via
   JS_NewArrayBufferFromString instead of building (and discarding) a
   Uint8Array wrapper -- node_compat.js does `new Buffer(arrayBuffer)`
   directly, so there's no use for a wrapper here. */
static JSValue sxn_utf8_encode_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    JSValueConst input = argc > 0 ? argv[0] : JS_UNDEFINED;
    return JS_NewArrayBufferFromString(ctx, input);
}

/* TextDecoder.decode() native primitive. Mirrors the previous hand-rolled
   JS decode loop byte-for-byte (same fatal/replacement-char/truncation
   rules) so behavior -- including the {stream:true} partial-sequence
   buffering -- is unchanged; only the per-byte JS loop and per-iteration
   string concatenation are replaced with a single C pass into a UTF-16
   buffer. bootstrap.js still owns merging a prior _pending buffer into
   `bytes` before calling this, since that happens once per decode() call
   (not once per byte) and isn't the hot path. Returns {text, pending}. */
static JSValue sxn_utf8_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "decode expects a Uint8Array");
    size_t len = 0;
    uint8_t *bytes = JS_GetUint8Array(ctx, &len, argv[0]);
    if (!bytes && len == 0) {
        /* JS_GetUint8Array returns NULL+0 both on error (already threw) and
           in principle for a genuinely empty view; in this codebase's
           existing zero-length Uint8Arrays always carry a non-NULL data
           pointer (see js_array_buffer_constructor3's max_int(len,1)
           allocation), so NULL here reliably means an exception is pending. */
        return JS_EXCEPTION;
    }
    int fatal = argc > 1 && JS_ToBool(ctx, argv[1]);
    int stream = argc > 2 && JS_ToBool(ctx, argv[2]);

    uint16_t *out = len ? malloc(len * sizeof(uint16_t)) : NULL;
    if (len && !out) return JS_ThrowOutOfMemory(ctx);
    size_t out_len = 0;
    size_t i = 0;
    size_t pending_start = (size_t)-1;
    while (i < len) {
        uint8_t b0 = bytes[i];
        uint32_t cp;
        size_t seqlen;
        if (b0 < 0x80) { cp = b0; seqlen = 1; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; seqlen = 2; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; seqlen = 3; }
        else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; seqlen = 4; }
        else {
            if (fatal) { free(out); return JS_ThrowTypeError(ctx, "invalid UTF-8"); }
            out[out_len++] = 0xFFFD; i++; continue;
        }
        if (i + seqlen > len) {
            if (stream) { pending_start = i; i = len; break; }
            if (fatal) { free(out); return JS_ThrowTypeError(ctx, "truncated UTF-8"); }
            out[out_len++] = 0xFFFD; i = len; break;
        }
        int ok = 1;
        for (size_t k = 1; k < seqlen; k++) {
            uint8_t b = bytes[i + k];
            if ((b & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (b & 0x3F);
        }
        if (!ok) {
            if (fatal) { free(out); return JS_ThrowTypeError(ctx, "invalid UTF-8"); }
            out[out_len++] = 0xFFFD; i++; continue;
        }
        i += seqlen;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            out[out_len++] = (uint16_t)(0xD800 + (cp >> 10));
            out[out_len++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out[out_len++] = (uint16_t)cp;
        }
    }
    JSValue text = JS_NewStringUTF16(ctx, out, out_len);
    JSValue pending = pending_start != (size_t)-1
        ? JS_NewUint8ArrayCopy(ctx, bytes + pending_start, len - pending_start)
        : JS_NewUint8ArrayCopy(ctx, NULL, 0);
    free(out);
    if (JS_IsException(text) || JS_IsException(pending)) {
        JS_FreeValue(ctx, text); JS_FreeValue(ctx, pending);
        return JS_EXCEPTION;
    }
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "text", text);
    JS_SetPropertyStr(ctx, result, "pending", pending);
    return result;
}

/* Non-streaming decode that returns the string directly. sxn_utf8_decode
   returns {text, pending} because TextDecoder needs the partial-sequence
   tail; Buffer#toString("utf-8") never does, and was allocating a result
   object per call only to read .text back off it. */
static JSValue sxn_utf8_decode_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue r = sxn_utf8_decode(ctx, this_val, argc, argv);
    if (JS_IsException(r)) return r;
    JSValue text = JS_GetPropertyStr(ctx, r, "text");
    JS_FreeValue(ctx, r);
    return text;
}

/* Sxn.file(path).text() reads via libuv's thread pool (uv_fs_open/read/close)
   and resolves a real Promise from the completion callback, so the caller's
   synchronous code keeps running while the read happens off-thread. This is
   the primitive a future `node:fs/promises` polyfill (Task 3/5) would wrap.

   .arrayBuffer() and .readBorrowed() (new) share this exact open/read/close
   state machine -- only the completion callback's final resolution differs
   (`mode`), per the instruction to follow this pattern rather than
   reimplement async file I/O. .text() UTF-8-decodes via JS_NewStringLen;
   the other two hand off fr->data's raw bytes with no decode step, either
   as a plain ArrayBuffer or as a borrow-guarded BorrowedChunk (reusing the
   SxnChunkView machinery from the streaming-fetch borrow lock above). */
typedef enum FileReadMode { FILE_READ_TEXT, FILE_READ_BYTES, FILE_READ_BORROWED } FileReadMode;

typedef struct FileReadReq {
    uv_fs_t req;
    JSContext *ctx;
    JSValue resolve, reject;
    uv_file fd;
    char *data; size_t length, cap;
    char chunk[65536];
    FileReadMode mode;
} FileReadReq;

static void file_read_reject(FileReadReq *fr, int uv_errno) {
    JSValue error = JS_NewError(fr->ctx);
    JS_SetPropertyStr(fr->ctx, error, "message", JS_NewString(fr->ctx, uv_strerror(uv_errno)));
    JS_Call(fr->ctx, fr->reject, JS_UNDEFINED, 1, &error);
    JS_FreeValue(fr->ctx, error);
    JS_FreeValue(fr->ctx, fr->resolve); JS_FreeValue(fr->ctx, fr->reject);
    free(fr->data); free(fr);
}

/* Ownership-transfer free_func for FILE_READ_BYTES: fr->data was malloc'd by
   file_read_on_read below and handed directly to the ArrayBuffer with no
   copy, so releasing it is just a plain free(). */
static void sxn_filebuf_free(JSRuntime *rt, void *opaque, void *ptr) { (void)rt; (void)opaque; free(ptr); }

static void file_read_on_close(uv_fs_t *req) {
    FileReadReq *fr = (FileReadReq *)req->data; uv_fs_req_cleanup(req);
    JSValue value;
    if (fr->mode == FILE_READ_TEXT) {
        value = JS_NewStringLen(fr->ctx, fr->data ? fr->data : "", fr->length);
    } else {
        if (!fr->data) fr->data = malloc(1); /* guarantee a real owned allocation even for a zero-length file */
        value = fr->mode == FILE_READ_BYTES
            ? JS_NewArrayBuffer(fr->ctx, (uint8_t *)fr->data, fr->length, sxn_filebuf_free, NULL, false)
            : sxn_make_owned_borrowed_chunk(fr->ctx, (uint8_t *)fr->data, fr->length);
        fr->data = NULL; /* ownership transferred either way */
    }
    JS_Call(fr->ctx, fr->resolve, JS_UNDEFINED, 1, &value);
    JS_FreeValue(fr->ctx, value); JS_FreeValue(fr->ctx, fr->resolve); JS_FreeValue(fr->ctx, fr->reject);
    free(fr->data); free(fr);
}

static void file_read_on_read(uv_fs_t *req) {
    FileReadReq *fr = (FileReadReq *)req->data; ssize_t n = req->result; uv_fs_req_cleanup(req);
    if (n < 0) { uv_fs_close(sxn_loop(), &fr->req, fr->fd, NULL); file_read_reject(fr, (int)n); return; }
    if (n == 0) { uv_fs_close(sxn_loop(), &fr->req, fr->fd, file_read_on_close); return; }
    if (fr->length + (size_t)n > fr->cap) {
        fr->cap = fr->cap ? fr->cap * 2 : 65536; while (fr->cap < fr->length + (size_t)n) fr->cap *= 2;
        fr->data = realloc(fr->data, fr->cap);
    }
    memcpy(fr->data + fr->length, fr->chunk, (size_t)n); fr->length += (size_t)n;
    uv_buf_t buf = uv_buf_init(fr->chunk, sizeof(fr->chunk));
    uv_fs_read(sxn_loop(), &fr->req, fr->fd, &buf, 1, -1, file_read_on_read);
}

static void file_read_on_open(uv_fs_t *req) {
    FileReadReq *fr = (FileReadReq *)req->data; int result = (int)req->result; uv_fs_req_cleanup(req);
    if (result < 0) { file_read_reject(fr, result); return; }
    fr->fd = result;
    uv_buf_t buf = uv_buf_init(fr->chunk, sizeof(fr->chunk));
    uv_fs_read(sxn_loop(), &fr->req, fr->fd, &buf, 1, -1, file_read_on_read);
}

static JSValue sxn_file_read_start(JSContext *ctx, JSValueConst this_val, FileReadMode mode) {
    JSValue path_value = JS_GetPropertyStr(ctx, this_val, "path");
    const char *path = JS_ToCString(ctx, path_value); JS_FreeValue(ctx, path_value);
    if (!path) return JS_EXCEPTION;
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { JS_FreeCString(ctx, path); return promise; }
    FileReadReq *fr = calloc(1, sizeof(*fr));
    fr->ctx = ctx; fr->resolve = funcs[0]; fr->reject = funcs[1]; fr->req.data = fr; fr->mode = mode;
    int rc = uv_fs_open(sxn_loop(), &fr->req, path, O_RDONLY, 0, file_read_on_open);
    JS_FreeCString(ctx, path);
    if (rc < 0) { uv_fs_req_cleanup(&fr->req); file_read_reject(fr, rc); }
    return promise;
}

static JSValue sxn_file_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    return sxn_file_read_start(ctx, this_val, FILE_READ_TEXT);
}

/* Sxn.file(path).arrayBuffer(): raw bytes, no UTF-8 decode step -- the
   binary-file-read primitive Task 3 was missing entirely (Sxn.file's only
   existing reader was .text(), always UTF-8-decoded). */
static JSValue sxn_file_array_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    return sxn_file_read_start(ctx, this_val, FILE_READ_BYTES);
}

/* Sxn.file(path).readBorrowed(): same raw bytes as .arrayBuffer(), but
   handed back as a borrow-guarded BorrowedChunk (see
   sxn_make_owned_borrowed_chunk) instead of a plain ArrayBuffer -- read the
   file once, then reuse the same native buffer to serve it zero-copy across
   many Sxn.serve() requests (conn_read_cb's BorrowedChunk body path above)
   without re-reading from disk or re-copying into a fresh JS allocation
   each time. */
static JSValue sxn_file_read_borrowed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    return sxn_file_read_start(ctx, this_val, FILE_READ_BORROWED);
}

/* Sxn's async write counterpart to sxn_file_text above -- same
   uv_fs_open/.../close shape, just OPEN|WRITE|CLOSE instead of
   OPEN|READ*|CLOSE. Consumed by node_compat.js's fs/promises.writeFile
   (Task 5); no existing promise-based write primitive existed to reuse. */
typedef struct FileWriteReq {
    uv_fs_t req;
    JSContext *ctx;
    JSValue resolve, reject;
    uv_file fd;
    char *data;
    uv_buf_t buf;
} FileWriteReq;

static void file_write_reject(FileWriteReq *fw, int uv_errno) {
    JSValue error = JS_NewError(fw->ctx);
    JS_SetPropertyStr(fw->ctx, error, "message", JS_NewString(fw->ctx, uv_strerror(uv_errno)));
    JS_Call(fw->ctx, fw->reject, JS_UNDEFINED, 1, &error);
    JS_FreeValue(fw->ctx, error);
    JS_FreeValue(fw->ctx, fw->resolve); JS_FreeValue(fw->ctx, fw->reject);
    free(fw->data); free(fw);
}

static void file_write_on_close(uv_fs_t *req) {
    FileWriteReq *fw = (FileWriteReq *)req->data; uv_fs_req_cleanup(req);
    JSValue undef = JS_UNDEFINED;
    JS_Call(fw->ctx, fw->resolve, JS_UNDEFINED, 1, &undef);
    JS_FreeValue(fw->ctx, fw->resolve); JS_FreeValue(fw->ctx, fw->reject);
    free(fw->data); free(fw);
}

static void file_write_on_write(uv_fs_t *req) {
    FileWriteReq *fw = (FileWriteReq *)req->data; ssize_t n = req->result; uv_fs_req_cleanup(req);
    if (n < 0) { uv_fs_close(sxn_loop(), &fw->req, fw->fd, NULL); file_write_reject(fw, (int)n); return; }
    uv_fs_close(sxn_loop(), &fw->req, fw->fd, file_write_on_close);
}

static void file_write_on_open(uv_fs_t *req) {
    FileWriteReq *fw = (FileWriteReq *)req->data; int result = (int)req->result; uv_fs_req_cleanup(req);
    if (result < 0) { file_write_reject(fw, result); return; }
    fw->fd = result;
    uv_fs_write(sxn_loop(), &fw->req, fw->fd, &fw->buf, 1, 0, file_write_on_write);
}

static JSValue sxn_file_write_async(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "expects (path, data)");
    const char *path = JS_ToCString(ctx, argv[0]);
    size_t length = 0;
    const char *data = JS_ToCStringLen(ctx, &length, argv[1]);
    if (!path || !data) { JS_FreeCString(ctx, path); JS_FreeCString(ctx, data); return JS_EXCEPTION; }
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { JS_FreeCString(ctx, path); JS_FreeCString(ctx, data); return promise; }
    FileWriteReq *fw = calloc(1, sizeof(*fw));
    fw->ctx = ctx; fw->resolve = funcs[0]; fw->reject = funcs[1]; fw->req.data = fw;
    fw->data = malloc(length ? length : 1);
    memcpy(fw->data, data, length);
    fw->buf = uv_buf_init(fw->data, (unsigned int)length);
    JS_FreeCString(ctx, data);
    int rc = uv_fs_open(sxn_loop(), &fw->req, path, O_WRONLY | O_CREAT | O_TRUNC, 0644, file_write_on_open);
    JS_FreeCString(ctx, path);
    if (rc < 0) { uv_fs_req_cleanup(&fw->req); file_write_reject(fw, rc); }
    return promise;
}

static JSValue sxn_file_exists(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue path_value = JS_GetPropertyStr(ctx, this_val, "path");
    const char *path = JS_ToCString(ctx, path_value); struct stat info;
    int exists = path && stat(path, &info) == 0;
    JS_FreeCString(ctx, path); JS_FreeValue(ctx, path_value); return JS_NewBool(ctx, exists);
}

static JSValue sxn_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *path = argc ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!path) return JS_ThrowTypeError(ctx, "Sxn.file(path) requires a path");
    struct stat info; int exists = stat(path, &info) == 0;
    JSValue file = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, file, "path", JS_NewString(ctx, path));
    JS_SetPropertyStr(ctx, file, "size", JS_NewInt64(ctx, exists ? (int64_t)info.st_size : 0));
    JS_SetPropertyStr(ctx, file, "type", JS_NewString(ctx, "application/octet-stream"));
    JS_SetPropertyStr(ctx, file, "text", JS_NewCFunction(ctx, sxn_file_text, "text", 0));
    JS_SetPropertyStr(ctx, file, "arrayBuffer", JS_NewCFunction(ctx, sxn_file_array_buffer, "arrayBuffer", 0));
    JS_SetPropertyStr(ctx, file, "readBorrowed", JS_NewCFunction(ctx, sxn_file_read_borrowed, "readBorrowed", 0));
    JS_SetPropertyStr(ctx, file, "exists", JS_NewCFunction(ctx, sxn_file_exists, "exists", 0));
    JS_FreeCString(ctx, path); return file;
}

/* The only way to reach fd 2 from JS: console.error/warn and
   process.stderr are built on this, so a diagnostic does not land in the
   program's own stdout. */
/* node:os, from libuv rather than from guesses. The JS layer used to answer
   "localhost" for the hostname and 0 for the memory sizes, which is worse
   than not answering: a program cannot tell a stub from the truth. */
/* node:fs's stat, from libuv. The JS layer had no way to ask a file's size
   or kind at all, so `stat` and everything built on it -- a static file
   server, a build step that skips unchanged files -- was out of reach. */
static JSValue sxn_stat(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *path = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!path) return JS_ThrowTypeError(ctx, "stat(path) requires a path");
    bool follow = argc < 2 || JS_ToBool(ctx, argv[1]);
    uv_fs_t req;
    int rc = follow ? uv_fs_stat(NULL, &req, path, NULL) : uv_fs_lstat(NULL, &req, path, NULL);
    if (rc != 0) {
        JSValue error = JS_ThrowInternalError(ctx, "%s: %s", uv_strerror(rc), path);
        JSValue exception = JS_GetException(ctx);
        JS_SetPropertyStr(ctx, exception, "code", JS_NewString(ctx, uv_err_name(rc)));
        JS_SetPropertyStr(ctx, exception, "path", JS_NewString(ctx, path));
        JS_Throw(ctx, exception);
        JS_FreeCString(ctx, path);
        uv_fs_req_cleanup(&req);
        return error;
    }
    const uv_stat_t *st = &req.statbuf;
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "dev", JS_NewFloat64(ctx, (double)st->st_dev));
    JS_SetPropertyStr(ctx, out, "ino", JS_NewFloat64(ctx, (double)st->st_ino));
    JS_SetPropertyStr(ctx, out, "mode", JS_NewInt64(ctx, (int64_t)st->st_mode));
    JS_SetPropertyStr(ctx, out, "nlink", JS_NewFloat64(ctx, (double)st->st_nlink));
    JS_SetPropertyStr(ctx, out, "uid", JS_NewInt64(ctx, (int64_t)st->st_uid));
    JS_SetPropertyStr(ctx, out, "gid", JS_NewInt64(ctx, (int64_t)st->st_gid));
    JS_SetPropertyStr(ctx, out, "size", JS_NewFloat64(ctx, (double)st->st_size));
    JS_SetPropertyStr(ctx, out, "blksize", JS_NewFloat64(ctx, (double)st->st_blksize));
    JS_SetPropertyStr(ctx, out, "blocks", JS_NewFloat64(ctx, (double)st->st_blocks));
    JS_SetPropertyStr(ctx, out, "atimeMs", JS_NewFloat64(ctx, st->st_atim.tv_sec * 1000.0 + st->st_atim.tv_nsec / 1e6));
    JS_SetPropertyStr(ctx, out, "mtimeMs", JS_NewFloat64(ctx, st->st_mtim.tv_sec * 1000.0 + st->st_mtim.tv_nsec / 1e6));
    JS_SetPropertyStr(ctx, out, "ctimeMs", JS_NewFloat64(ctx, st->st_ctim.tv_sec * 1000.0 + st->st_ctim.tv_nsec / 1e6));
    JS_SetPropertyStr(ctx, out, "birthtimeMs", JS_NewFloat64(ctx, st->st_birthtim.tv_sec * 1000.0 + st->st_birthtim.tv_nsec / 1e6));
    JS_FreeCString(ctx, path);
    uv_fs_req_cleanup(&req);
    return out;
}

static JSValue sxn_os_hostname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char name[UV_MAXHOSTNAMESIZE];
    size_t size = sizeof(name);
    if (uv_os_gethostname(name, &size) != 0) return JS_NewString(ctx, "localhost");
    return JS_NewString(ctx, name);
}

static JSValue sxn_os_dir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val; (void)argc; (void)argv;
    char path[4096];
    size_t size = sizeof(path);
    int rc = magic == 0 ? uv_os_homedir(path, &size) : uv_os_tmpdir(path, &size);
    if (rc != 0) return JS_NewString(ctx, magic == 0 ? "/" : "/tmp");
    return JS_NewString(ctx, path);
}

static JSValue sxn_os_uname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    uv_utsname_t name;
    JSValue out = JS_NewObject(ctx);
    if (uv_os_uname(&name) != 0) return out;
    JS_SetPropertyStr(ctx, out, "sysname", JS_NewString(ctx, name.sysname));
    JS_SetPropertyStr(ctx, out, "release", JS_NewString(ctx, name.release));
    JS_SetPropertyStr(ctx, out, "version", JS_NewString(ctx, name.version));
    JS_SetPropertyStr(ctx, out, "machine", JS_NewString(ctx, name.machine));
    return out;
}

static JSValue sxn_os_numbers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    double avg[3] = {0, 0, 0};
    uv_loadavg(avg);
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "totalmem", JS_NewFloat64(ctx, (double)uv_get_total_memory()));
    JS_SetPropertyStr(ctx, out, "freemem", JS_NewFloat64(ctx, (double)uv_get_free_memory()));
    JS_SetPropertyStr(ctx, out, "uptime", JS_NewFloat64(ctx, ({ double up = 0; uv_uptime(&up); up; })));
    JS_SetPropertyStr(ctx, out, "parallelism", JS_NewInt32(ctx, (int32_t)uv_available_parallelism()));
    JSValue load = JS_NewArray(ctx);
    for (int i = 0; i < 3; i++) JS_SetPropertyUint32(ctx, load, i, JS_NewFloat64(ctx, avg[i]));
    JS_SetPropertyStr(ctx, out, "loadavg", load);
    return out;
}

static JSValue sxn_os_cpus(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    uv_cpu_info_t *info = NULL;
    int count = 0;
    JSValue out = JS_NewArray(ctx);
    if (uv_cpu_info(&info, &count) != 0) return out;
    for (int i = 0; i < count; i++) {
        JSValue cpu = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, cpu, "model", JS_NewString(ctx, info[i].model));
        JS_SetPropertyStr(ctx, cpu, "speed", JS_NewInt32(ctx, info[i].speed));
        JSValue times = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, times, "user", JS_NewFloat64(ctx, (double)info[i].cpu_times.user));
        JS_SetPropertyStr(ctx, times, "nice", JS_NewFloat64(ctx, (double)info[i].cpu_times.nice));
        JS_SetPropertyStr(ctx, times, "sys", JS_NewFloat64(ctx, (double)info[i].cpu_times.sys));
        JS_SetPropertyStr(ctx, times, "idle", JS_NewFloat64(ctx, (double)info[i].cpu_times.idle));
        JS_SetPropertyStr(ctx, times, "irq", JS_NewFloat64(ctx, (double)info[i].cpu_times.irq));
        JS_SetPropertyStr(ctx, cpu, "times", times);
        JS_SetPropertyUint32(ctx, out, (uint32_t)i, cpu);
    }
    uv_free_cpu_info(info, count);
    return out;
}

/* How many leading one-bits a netmask has, which is what a CIDR suffix is. */
static int sxn_mask_prefix(const uint8_t *bytes, int len) {
    int bits = 0;
    for (int i = 0; i < len; i++) {
        if (bytes[i] == 0xff) { bits += 8; continue; }
        uint8_t b = bytes[i];
        while (b & 0x80) { bits++; b <<= 1; }
        break;
    }
    return bits;
}

static JSValue sxn_os_interfaces(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    uv_interface_address_t *addresses = NULL;
    int count = 0;
    JSValue out = JS_NewObject(ctx);
    if (uv_interface_addresses(&addresses, &count) != 0) return out;
    for (int i = 0; i < count; i++) {
        uv_interface_address_t *a = &addresses[i];
        bool v6 = a->address.address4.sin_family == AF_INET6;
        char address[INET6_ADDRSTRLEN] = {0}, netmask[INET6_ADDRSTRLEN] = {0};
        int prefix;
        if (v6) {
            uv_ip6_name(&a->address.address6, address, sizeof(address));
            uv_ip6_name(&a->netmask.netmask6, netmask, sizeof(netmask));
            prefix = sxn_mask_prefix((const uint8_t *)&a->netmask.netmask6.sin6_addr, 16);
        } else {
            uv_ip4_name(&a->address.address4, address, sizeof(address));
            uv_ip4_name(&a->netmask.netmask4, netmask, sizeof(netmask));
            prefix = sxn_mask_prefix((const uint8_t *)&a->netmask.netmask4.sin_addr, 4);
        }
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 (unsigned char)a->phys_addr[0], (unsigned char)a->phys_addr[1],
                 (unsigned char)a->phys_addr[2], (unsigned char)a->phys_addr[3],
                 (unsigned char)a->phys_addr[4], (unsigned char)a->phys_addr[5]);
        char cidr[INET6_ADDRSTRLEN + 8];
        snprintf(cidr, sizeof(cidr), "%s/%d", address, prefix);

        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "address", JS_NewString(ctx, address));
        JS_SetPropertyStr(ctx, entry, "netmask", JS_NewString(ctx, netmask));
        JS_SetPropertyStr(ctx, entry, "family", JS_NewString(ctx, v6 ? "IPv6" : "IPv4"));
        JS_SetPropertyStr(ctx, entry, "mac", JS_NewString(ctx, mac));
        JS_SetPropertyStr(ctx, entry, "internal", JS_NewBool(ctx, a->is_internal));
        JS_SetPropertyStr(ctx, entry, "cidr", JS_NewString(ctx, cidr));
        if (v6)
            JS_SetPropertyStr(ctx, entry, "scopeid", JS_NewInt32(ctx, (int32_t)a->address.address6.sin6_scope_id));

        JSValue list = JS_GetPropertyStr(ctx, out, a->name);
        if (!JS_IsArray(list)) {
            JS_FreeValue(ctx, list);
            list = JS_NewArray(ctx);
            JS_SetPropertyStr(ctx, out, a->name, JS_DupValue(ctx, list));
        }
        uint32_t length = 0;
        JSValue size = JS_GetPropertyStr(ctx, list, "length");
        JS_ToUint32(ctx, &length, size);
        JS_FreeValue(ctx, size);
        JS_SetPropertyUint32(ctx, list, length, entry);
        JS_FreeValue(ctx, list);
    }
    uv_free_interface_addresses(addresses, count);
    return out;
}

static JSValue sxn_write_stderr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    size_t length = 0;
    const char *text = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!text) return JS_EXCEPTION;
    fwrite(text, 1, length, stderr);
    fflush(stderr);
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

static JSValue sxn_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "Sxn.write(destination, data) requires two arguments");
    const char *path = JS_ToCString(ctx, argv[0]); size_t length = 0;
    const char *data = JS_ToCStringLen(ctx, &length, argv[1]);
    if (!path || !data) { JS_FreeCString(ctx, path); JS_FreeCString(ctx, data); return JS_EXCEPTION; }
    FILE *file = fopen(path, "wb");
    if (!file) { JSValue error = JS_ThrowInternalError(ctx, "cannot write '%s': %s", path, strerror(errno)); JS_FreeCString(ctx, path); JS_FreeCString(ctx, data); return error; }
    size_t written = fwrite(data, 1, length, file); int failed = fclose(file) != 0 || written != length;
    JS_FreeCString(ctx, path); JS_FreeCString(ctx, data);
    if (failed) return JS_ThrowInternalError(ctx, "file write failed");
    return JS_NewInt64(ctx, (int64_t)written);
}

int sxn_install_network(JSContext *ctx) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return -1;
    sxn_time_origin_ns = uv_hrtime();

    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &sxn_stream_class_id);
    JS_NewClass(rt, sxn_stream_class_id, &sxn_stream_class_def);
    JSValue stream_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, stream_proto, sxn_stream_proto_funcs, countof(sxn_stream_proto_funcs));
    JS_SetClassProto(ctx, sxn_stream_class_id, stream_proto);

    JS_NewClassID(rt, &sxn_chunkview_class_id);
    JS_NewClass(rt, sxn_chunkview_class_id, &sxn_chunkview_class_def);
    JS_NewClassID(rt, &sxn_serverhandle_class_id);
    JS_NewClass(rt, sxn_serverhandle_class_id, &sxn_serverhandle_class);
    JSValue chunkview_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, chunkview_proto, sxn_chunkview_proto_funcs, countof(sxn_chunkview_proto_funcs));
    JS_SetClassProto(ctx, sxn_chunkview_class_id, chunkview_proto);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue runtime = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, runtime, "version", JS_NewString(ctx, "0.0.1"));
    JS_SetPropertyStr(ctx, runtime, "serve", JS_NewCFunction(ctx, js_serve, "serve", 2));
    JS_SetPropertyStr(ctx, runtime, "file", JS_NewCFunction(ctx, sxn_file, "file", 1));
    JS_SetPropertyStr(ctx, runtime, "write", JS_NewCFunction(ctx, sxn_write, "write", 2));
    JS_SetPropertyStr(ctx, runtime, "memoryUsage", JS_NewCFunction(ctx, sxn_memory_usage, "memoryUsage", 0));
    sxn_ffi_init(ctx);
    JS_SetPropertyStr(ctx, runtime, "ffi", JS_NewCFunction(ctx, sxn_ffi, "ffi", 4));
    JS_SetPropertyStr(ctx, global, "Sxn", runtime);
    JS_SetPropertyStr(ctx, global, "__sxnServe", JS_NewCFunction(ctx, js_serve, "__sxnServe", 2));

    /* Native primitives consumed only by bootstrap.js (fetch's raw
       networking primitive, the monotonic clock, and OpenSSL-backed
       crypto); bootstrap.js builds the spec-shaped globals on top of them
       and installs fetch/TextEncoder/URL/Headers/etc. on `global` itself. */
    JS_SetPropertyStr(ctx, global, "__sxnFetchRaw", JS_NewCFunction(ctx, js_sxn_fetch_raw, "__sxnFetchRaw", 5));
    /* Named "now" because bootstrap.js binds this straight onto performance
       rather than wrapping it, so this is the function user code sees. */
    JS_SetPropertyStr(ctx, global, "__sxnParseJSONBytes", JS_NewCFunction(ctx, sxn_parse_json_bytes, "__sxnParseJSONBytes", 3));
    JS_SetPropertyStr(ctx, global, "__sxnStat", JS_NewCFunction(ctx, sxn_stat, "__sxnStat", 2));
    JS_SetPropertyStr(ctx, global, "__sxnOsHostname", JS_NewCFunction(ctx, sxn_os_hostname, "__sxnOsHostname", 0));
    JS_SetPropertyStr(ctx, global, "__sxnOsHomedir", JS_NewCFunctionMagic(ctx, sxn_os_dir, "__sxnOsHomedir", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "__sxnOsTmpdir", JS_NewCFunctionMagic(ctx, sxn_os_dir, "__sxnOsTmpdir", 0, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "__sxnOsUname", JS_NewCFunction(ctx, sxn_os_uname, "__sxnOsUname", 0));
    JS_SetPropertyStr(ctx, global, "__sxnOsNumbers", JS_NewCFunction(ctx, sxn_os_numbers, "__sxnOsNumbers", 0));
    JS_SetPropertyStr(ctx, global, "__sxnOsCpus", JS_NewCFunction(ctx, sxn_os_cpus, "__sxnOsCpus", 0));
    JS_SetPropertyStr(ctx, global, "__sxnOsInterfaces", JS_NewCFunction(ctx, sxn_os_interfaces, "__sxnOsInterfaces", 0));
    JS_SetPropertyStr(ctx, global, "__sxnPid", JS_NewInt32(ctx, (int32_t)uv_os_getpid()));
    JS_SetPropertyStr(ctx, global, "__sxnWriteStderr", JS_NewCFunction(ctx, sxn_write_stderr, "__sxnWriteStderr", 1));
    JS_SetPropertyStr(ctx, global, "__sxnNow", JS_NewCFunction(ctx, sxn_now, "now", 0));
#ifdef SXN_ABLATE_FUSION
    JS_SetPropertyStr(ctx, global, "__ablNop",
                      JS_NewCFunction(ctx, sxn_abl_nop, "__ablNop", 2));
    JS_SetPropertyStr(ctx, global, "__ablEncodeLen",
                      JS_NewCFunction(ctx, sxn_abl_encode_len, "__ablEncodeLen", 1));
    JS_SetPropertyStr(ctx, global, "__ablBufLenConcat",
                      JS_NewCFunction(ctx, sxn_abl_buf_len_concat, "__ablBufLenConcat", 2));
    JS_SetPropertyStr(ctx, global, "__ablEmit",
                      JS_NewCFunction(ctx, sxn_abl_emit, "__ablEmit", 2));
#endif
    JS_SetPropertyStr(ctx, global, "__sxnRandomBytes", JS_NewCFunction(ctx, sxn_random_bytes, "__sxnRandomBytes", 1));
    JS_SetPropertyStr(ctx, global, "__sxnBytesCompare", JS_NewCFunction(ctx, sxn_bytes_compare, "__sxnBytesCompare", 2));
    JS_SetPropertyStr(ctx, global, "__sxnHmac", JS_NewCFunction(ctx, sxn_hmac, "__sxnHmac", 3));
    JS_SetPropertyStr(ctx, global, "__sxnTimingSafeEqual", JS_NewCFunction(ctx, sxn_timing_safe_equal, "__sxnTimingSafeEqual", 2));
    JS_SetPropertyStr(ctx, global, "__sxnDigest", JS_NewCFunction(ctx, sxn_digest, "__sxnDigest", 2));
    /* Consumed only by bootstrap.js's TextEncoder/TextDecoder. */
    JS_SetPropertyStr(ctx, global, "__sxnUtf8Encode", JS_NewCFunction(ctx, sxn_utf8_encode, "__sxnUtf8Encode", 1));
    JS_SetPropertyStr(ctx, global, "__sxnUtf8EncodeInto", JS_NewCFunction(ctx, sxn_utf8_encode_into, "__sxnUtf8EncodeInto", 2));
    JS_SetPropertyStr(ctx, global, "__sxnUtf8Decode", JS_NewCFunction(ctx, sxn_utf8_decode, "__sxnUtf8Decode", 3));
    JS_SetPropertyStr(ctx, global, "__sxnUtf8DecodeText", JS_NewCFunction(ctx, sxn_utf8_decode_text, "__sxnUtf8DecodeText", 1));
    /* Consumed only by node_compat.js's Buffer.from(string, "utf-8"). */
    JS_SetPropertyStr(ctx, global, "__sxnUtf8EncodeArrayBuffer", JS_NewCFunction(ctx, sxn_utf8_encode_buffer, "__sxnUtf8EncodeArrayBuffer", 1));
    /* Consumed only by node_compat.js's fs/promises.writeFile (Task 5). */
    JS_SetPropertyStr(ctx, global, "__sxnWriteFileAsync", JS_NewCFunction(ctx, sxn_file_write_async, "__sxnWriteFileAsync", 2));
    /* Consumed only by bootstrap.js's setTimeout/setInterval/clearTimeout/clearInterval. */
    JS_SetPropertyStr(ctx, global, "__sxnSetTimer", JS_NewCFunction(ctx, sxn_set_timer, "__sxnSetTimer", 3));
    JS_SetPropertyStr(ctx, global, "__sxnClearTimer", JS_NewCFunction(ctx, sxn_clear_timer, "__sxnClearTimer", 1));
    JS_FreeValue(ctx, global);

    /* Compiled by qjsc during the build, so startup reads a ready function
       instead of parsing the file again on every launch. The source carries
       its own "use strict" because that used to come from an eval flag. */
    JSValue bootstrap = JS_ReadObject(ctx, sxn_bootstrap_bc, sxn_bootstrap_bc_size,
                                      JS_READ_OBJ_BYTECODE);
    if (JS_IsException(bootstrap)) { JS_FreeValue(ctx, bootstrap); return -1; }
    bootstrap = JS_EvalFunction(ctx, bootstrap);
    if (JS_IsException(bootstrap)) { JS_FreeValue(ctx, bootstrap); return -1; }
    JS_FreeValue(ctx, bootstrap);

    /* Sxn.fetch mirrors the global fetch() bootstrap.js just installed, for
       code that reaches it through the Sxn namespace. */
    global = JS_GetGlobalObject(ctx);
    JSValue fetch_fn = JS_GetPropertyStr(ctx, global, "fetch");
    JS_SetPropertyStr(ctx, runtime, "fetch", fetch_fn);
    JS_FreeValue(ctx, global);
    return 0;
}

/* Await a top-level-await module's evaluation promise while driving BOTH job
   queues. js_std_await only drains QuickJS jobs and polls quickjs-libc's own
   os handles; it knows nothing about the uv loop that timers, Sxn.serve and
   fetch run on, so `const r = await fetch(...)` at module top level blocked
   forever -- the promise could only be settled by work that never got a
   chance to run. Mirrors js_std_await's contract: consumes obj, returns the
   fulfilled value or throws the rejection, and passes non-promises through. */
JSValue sxn_await_with_loop(JSContext *ctx, JSValue obj) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    uv_loop_t *loop = sxn_loop();
    for (;;) {
        int state = JS_PromiseState(ctx, obj);
        if (state == JS_PROMISE_FULFILLED) {
            JSValue ret = JS_PromiseResult(ctx, obj);
            JS_FreeValue(ctx, obj);
            return ret;
        }
        if (state == JS_PROMISE_REJECTED) {
            JSValue ret = JS_Throw(ctx, JS_PromiseResult(ctx, obj));
            JS_FreeValue(ctx, obj);
            return ret;
        }
        if (state != JS_PROMISE_PENDING)
            return obj;                       /* not a promise at all */

        JSContext *ctx1;
        int err = JS_ExecutePendingJob(rt, &ctx1);
        if (err < 0)
            js_std_dump_error(ctx1);
        if (err != 0)
            continue;                         /* a job ran; re-check the state */

        /* No jobs left, so only the loop can move this forward. UV_RUN_ONCE
           blocks until something is ready rather than spinning. If nothing is
           pending either, the promise can never settle -- return it and let
           the caller see a still-pending module rather than hang. */
        if (!uv_run(loop, UV_RUN_ONCE) && !JS_IsJobPending(rt))
            return obj;
    }
}

int sxn_run_event_loop(JSContext *ctx) {
    uv_loop_t *loop = sxn_loop();
    JSRuntime *rt = JS_GetRuntime(ctx);
    for (;;) {
        JSContext *ctx1; int err;
        while ((err = JS_ExecutePendingJob(rt, &ctx1)) > 0) {}
        if (err < 0) break;
        /* UV_RUN_ONCE blocks (no busy-loop) until the next batch of I/O is
           ready, then returns so we can drain any jobs it just enqueued
           before waiting on the next batch. Exits once nothing is left:
           no active/ref'd handles (e.g. no serve() was ever called) and no
           pending job, which is what keeps a plain script's exit identical
           to before this loop existed. */
        int more_handles = uv_run(loop, UV_RUN_ONCE);
        if (!more_handles && !JS_IsJobPending(rt)) break;
    }
    return JS_HasException(ctx);
}
