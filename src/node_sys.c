/* The system primitives the node: builtins are built on, and nothing else.
 *
 * fs.stat and fs/promises.writeFile, node:dgram's UDP sockets,
 * child_process's spawn, node:dns's lookup, and node:os -- every one of them
 * reached only from src/node_compat.js, every one of them libuv.
 *
 * They used to live in src/network.c beside the WinterTC surface, which made
 * that file look far more dependent on libuv and on Node than it is: none of
 * these names appears anywhere in src/bootstrap.js. Splitting them out is
 * what lets the runtime half be built without the node half.
 *
 * Registered by sxn_install_node_sys, which sxn_install_node_compat calls. */

#include <quickjs.h>
#include <quickjs-libc.h>
#include <uv.h>

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

/* One process-wide loop, the same one src/network.c drives. */
static uv_loop_t *sxn_loop(void) { return uv_default_loop(); }

/* fs/promises.writeFile's primitive: the same uv_fs_open/.../close shape as
   Sxn.file's reader in src/network.c, just OPEN|WRITE|CLOSE instead of
   OPEN|READ*|CLOSE. Consumed by node_compat.js; no promise-based write
   primitive existed to reuse. */
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
    /* The caller passes Stats.prototype, so the object comes back already
       being a Stats -- node:fs used to copy every field of this onto a fresh
       one with a for-in loop. */
    JSValue out = (argc > 2 && JS_IsObject(argv[2])) ? JS_NewObjectProto(ctx, argv[2]) : JS_NewObject(ctx);
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
    /* Node's Stats carries the same four times over again as Dates. */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue date_class = JS_GetPropertyStr(ctx, global, "Date");
    JS_FreeValue(ctx, global);
    static const char *time_names[4] = { "atime", "mtime", "ctime", "birthtime" };
    double times[4] = {
        st->st_atim.tv_sec * 1000.0 + st->st_atim.tv_nsec / 1e6,
        st->st_mtim.tv_sec * 1000.0 + st->st_mtim.tv_nsec / 1e6,
        st->st_ctim.tv_sec * 1000.0 + st->st_ctim.tv_nsec / 1e6,
        st->st_birthtim.tv_sec * 1000.0 + st->st_birthtim.tv_nsec / 1e6,
    };
    for (int i = 0; i < 4; i++) {
        JSValue ms = JS_NewFloat64(ctx, times[i]);
        JSValueConst args[1] = { ms };
        JS_SetPropertyStr(ctx, out, time_names[i], JS_CallConstructor(ctx, date_class, 1, args));
        JS_FreeValue(ctx, ms);
    }
    JS_FreeValue(ctx, date_class);
    JS_FreeCString(ctx, path);
    uv_fs_req_cleanup(&req);
    return out;
}

/* fs's open/access flags, taken from this platform's own headers rather than
   written down: O_CREAT alone is 0x200 on macOS and 0x40 on Linux. */
static JSValue js_fs_constants(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue out = JS_NewObject(ctx);
#define SXN_CONST(name, value) JS_SetPropertyStr(ctx, out, name, JS_NewInt32(ctx, (int32_t)(value)))
    SXN_CONST("O_RDONLY", O_RDONLY);
    SXN_CONST("O_WRONLY", O_WRONLY);
    SXN_CONST("O_RDWR", O_RDWR);
    SXN_CONST("O_CREAT", O_CREAT);
    SXN_CONST("O_EXCL", O_EXCL);
    SXN_CONST("O_TRUNC", O_TRUNC);
    SXN_CONST("O_APPEND", O_APPEND);
    SXN_CONST("F_OK", 0);
    SXN_CONST("R_OK", 4);
    SXN_CONST("W_OK", 2);
    SXN_CONST("X_OK", 1);
    SXN_CONST("S_IFMT", 0170000);
    SXN_CONST("S_IFREG", 0100000);
    SXN_CONST("S_IFDIR", 0040000);
    SXN_CONST("S_IFLNK", 0120000);
    SXN_CONST("COPYFILE_EXCL", 1);
#undef SXN_CONST
    return out;
}

/* ---------------- UDP (node:dgram) ----------------
   A uv_udp_t on the same loop everything else runs on, with the socket's
   callbacks handed straight to JavaScript. node_compat.js puts the
   EventEmitter shape around it. */
static JSClassID sxn_udp_class_id;

typedef struct {
    uv_udp_t handle;
    JSContext *ctx;
    JSValue on_message;
    bool open, reading;
} SxnUdp;

static void sxn_udp_closed(uv_handle_t *handle) { free(handle->data); }

static void sxn_udp_finalizer(JSRuntime *rt, JSValue val) {
    SxnUdp *u = JS_GetOpaque(val, sxn_udp_class_id);
    if (!u) return;
    JS_FreeValueRT(rt, u->on_message);
    u->on_message = JS_UNDEFINED;
    if (u->open) {
        u->open = false;
        u->handle.data = u;
        uv_close((uv_handle_t *)&u->handle, sxn_udp_closed);
    } else {
        free(u);
    }
}

static JSClassDef sxn_udp_class_def = {
    .class_name = "UdpSocket",
    .finalizer = sxn_udp_finalizer,
};

static void sxn_udp_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
    (void)handle;
    buf->base = malloc(suggested);
    buf->len = buf->base ? suggested : 0;
}

static void sxn_udp_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                         const struct sockaddr *addr, unsigned flags) {
    (void)flags;
    SxnUdp *u = handle->data;
    if (nread > 0 && addr && u && JS_IsFunction(u->ctx, u->on_message)) {
        JSContext *ctx = u->ctx;
        char text[INET6_ADDRSTRLEN] = {0};
        int port = 0;
        if (addr->sa_family == AF_INET6) {
            uv_ip6_name((struct sockaddr_in6 *)addr, text, sizeof(text));
            port = ntohs(((struct sockaddr_in6 *)addr)->sin6_port);
        } else {
            uv_ip4_name((struct sockaddr_in *)addr, text, sizeof(text));
            port = ntohs(((struct sockaddr_in *)addr)->sin_port);
        }
        JSValue args[3];
        args[0] = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)buf->base, (size_t)nread);
        args[1] = JS_NewString(ctx, text);
        args[2] = JS_NewInt32(ctx, port);
        JSValue r = JS_Call(ctx, u->on_message, JS_UNDEFINED, 3, (JSValueConst *)args);
        if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
        for (int i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
    }
    free(buf->base);
}

/* __sxnUdpOpen(ipv6, onMessage) */
static JSValue js_udp_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    SxnUdp *u = calloc(1, sizeof(*u));
    if (!u) return JS_ThrowOutOfMemory(ctx);
    u->ctx = ctx;
    u->on_message = argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
    if (uv_udp_init(sxn_loop(), &u->handle) != 0) {
        JS_FreeValue(ctx, u->on_message);
        free(u);
        return JS_ThrowInternalError(ctx, "udp init failed");
    }
    u->handle.data = u;
    u->open = true;
    JS_NewClassID(JS_GetRuntime(ctx), &sxn_udp_class_id);
    JS_NewClass(JS_GetRuntime(ctx), sxn_udp_class_id, &sxn_udp_class_def);
    JSValue obj = JS_NewObjectClass(ctx, sxn_udp_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, u);
    return obj;
}

static SxnUdp *sxn_udp_of(JSContext *ctx, JSValueConst val) {
    SxnUdp *u = JS_GetOpaque(val, sxn_udp_class_id);
    return (u && u->open) ? u : NULL;
}

/* __sxnUdpBind(socket, port, address) -> the port actually bound */
static JSValue js_udp_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    SxnUdp *u = argc > 0 ? sxn_udp_of(ctx, argv[0]) : NULL;
    if (!u) return JS_ThrowTypeError(ctx, "not an open udp socket");
    int32_t port = 0;
    if (argc > 1) JS_ToInt32(ctx, &port, argv[1]);
    const char *address = argc > 2 && JS_IsString(argv[2]) ? JS_ToCString(ctx, argv[2]) : NULL;
    struct sockaddr_storage addr;
    int rc = uv_ip4_addr(address ? address : "0.0.0.0", port, (struct sockaddr_in *)&addr);
    if (rc != 0) rc = uv_ip6_addr(address ? address : "::", port, (struct sockaddr_in6 *)&addr);
    if (address) JS_FreeCString(ctx, address);
    if (rc == 0) rc = uv_udp_bind(&u->handle, (const struct sockaddr *)&addr, 0);
    if (rc != 0) return JS_ThrowInternalError(ctx, "udp bind failed: %s", uv_strerror(rc));
    if (!u->reading) {
        rc = uv_udp_recv_start(&u->handle, sxn_udp_alloc, sxn_udp_recv);
        if (rc != 0) return JS_ThrowInternalError(ctx, "udp listen failed: %s", uv_strerror(rc));
        u->reading = true;
    }
    struct sockaddr_storage bound;
    int len = sizeof(bound);
    if (uv_udp_getsockname(&u->handle, (struct sockaddr *)&bound, &len) != 0)
        return JS_NewInt32(ctx, port);
    return JS_NewInt32(ctx, bound.ss_family == AF_INET6
        ? ntohs(((struct sockaddr_in6 *)&bound)->sin6_port)
        : ntohs(((struct sockaddr_in *)&bound)->sin_port));
}

static void sxn_udp_sent(uv_udp_send_t *req, int status) { (void)status; free(req); }

/* __sxnUdpSend(socket, bytes, port, address) */
static JSValue js_udp_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    SxnUdp *u = argc > 0 ? sxn_udp_of(ctx, argv[0]) : NULL;
    if (!u) return JS_ThrowTypeError(ctx, "not an open udp socket");
    size_t len = 0;
    uint8_t *bytes = argc > 1 ? JS_GetUint8Array(ctx, &len, argv[1]) : NULL;
    if (!bytes) return JS_ThrowTypeError(ctx, "udp send expects bytes");
    int32_t port = 0;
    if (argc > 2) JS_ToInt32(ctx, &port, argv[2]);
    const char *address = argc > 3 && JS_IsString(argv[3]) ? JS_ToCString(ctx, argv[3]) : NULL;
    struct sockaddr_storage addr;
    int rc = uv_ip4_addr(address ? address : "127.0.0.1", port, (struct sockaddr_in *)&addr);
    if (rc != 0) rc = uv_ip6_addr(address ? address : "::1", port, (struct sockaddr_in6 *)&addr);
    if (address) JS_FreeCString(ctx, address);
    if (rc != 0) return JS_ThrowTypeError(ctx, "udp send: bad address");
    /* uv_udp_send copies nothing, so the write has to finish before the
       caller's bytes can move: uv_udp_try_send does it inline, and the
       queued path gets its own copy. */
    uv_buf_t buf = uv_buf_init((char *)bytes, (unsigned int)len);
    rc = uv_udp_try_send(&u->handle, &buf, 1, (const struct sockaddr *)&addr);
    if (rc >= 0) return JS_NewInt32(ctx, rc);
    uv_udp_send_t *req = malloc(sizeof(*req) + len);
    if (!req) return JS_ThrowOutOfMemory(ctx);
    char *copy = (char *)(req + 1);
    memcpy(copy, bytes, len);
    uv_buf_t queued = uv_buf_init(copy, (unsigned int)len);
    rc = uv_udp_send(req, &u->handle, &queued, 1, (const struct sockaddr *)&addr, sxn_udp_sent);
    if (rc != 0) { free(req); return JS_ThrowInternalError(ctx, "udp send failed: %s", uv_strerror(rc)); }
    return JS_NewInt32(ctx, (int32_t)len);
}

/* __sxnUdpClose(socket) */
static JSValue js_udp_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    SxnUdp *u = argc > 0 ? JS_GetOpaque(argv[0], sxn_udp_class_id) : NULL;
    if (!u || !u->open) return JS_UNDEFINED;
    JS_FreeValue(ctx, u->on_message);
    u->on_message = JS_UNDEFINED;
    u->open = false;
    u->handle.data = u;
    uv_close((uv_handle_t *)&u->handle, sxn_udp_closed);
    JS_SetOpaque(argv[0], NULL);
    return JS_UNDEFINED;
}

/* ---------------- process spawning (node:child_process) ----------------
   One child, run to completion on a loop of its own so that nothing else
   queued on the default loop runs re-entrantly while we wait. That makes
   this the synchronous form; the asynchronous forms in node_compat.js are
   built on it, and say so. */
typedef struct { char *data; size_t len, cap; } SxnGrow;

static void sxn_grow_push(SxnGrow *b, const char *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 8192;
        while (want < b->len + n) want *= 2;
        char *next = realloc(b->data, want);
        if (!next) return;
        b->data = next; b->cap = want;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void sxn_spawn_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
    (void)handle;
    buf->base = malloc(suggested);
    buf->len = buf->base ? suggested : 0;
}

static void sxn_spawn_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    if (nread > 0) sxn_grow_push((SxnGrow *)stream->data, buf->base, (size_t)nread);
    else if (nread < 0) uv_close((uv_handle_t *)stream, NULL);
    free(buf->base);
}

typedef struct { int64_t status; int signal; } SxnExit;

static void sxn_spawn_exit(uv_process_t *proc, int64_t status, int signal) {
    SxnExit *out = proc->data;
    out->status = status; out->signal = signal;
    uv_close((uv_handle_t *)proc, NULL);
}

/* A loop that is closed while a handle is still registered leaves libuv's
   own child-process bookkeeping behind it, and the next uv_spawn walks into
   what is left: on Linux that is a segfault, not a leak. So everything still
   open is closed, the loop is run until those closes complete, and only then
   is it closed. */
static void sxn_spawn_close_walk(uv_handle_t *handle, void *arg) {
    (void)arg;
    if (!uv_is_closing(handle)) uv_close(handle, NULL);
}

static void sxn_spawn_teardown(uv_loop_t *loop) {
    uv_walk(loop, sxn_spawn_close_walk, NULL);
    while (uv_run(loop, UV_RUN_DEFAULT) != 0) { }
    uv_loop_close(loop);
}

static void sxn_spawn_written(uv_write_t *req, int status) {
    (void)status;
    uv_close((uv_handle_t *)req->handle, NULL);
    free(req);
}

/* __sxnSpawnSync(file, args, options) -> { pid, status, signal, stdout, stderr, error } */
static JSValue js_spawn_sync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "spawn needs a command");
    const char *file = JS_ToCString(ctx, argv[0]);
    if (!file) return JS_EXCEPTION;

    uint32_t nargs = 0;
    if (argc > 1 && JS_IsArray(argv[1])) {
        JSValue len = JS_GetPropertyStr(ctx, argv[1], "length");
        JS_ToUint32(ctx, &nargs, len);
        JS_FreeValue(ctx, len);
    }
    char **args = calloc(nargs + 2, sizeof(char *));
    args[0] = (char *)file;
    for (uint32_t i = 0; i < nargs; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[1], i);
        const char *s = JS_ToCString(ctx, item);
        args[i + 1] = s ? strdup(s) : strdup("");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, item);
    }

    char *cwd = NULL, **env = NULL;
    const char *input = NULL;
    size_t input_len = 0;
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[2], "cwd");
        if (JS_IsString(v)) { const char *s = JS_ToCString(ctx, v); if (s) { cwd = strdup(s); JS_FreeCString(ctx, s); } }
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "input");
        if (JS_IsString(v)) input = JS_ToCStringLen(ctx, &input_len, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[2], "env");
        if (JS_IsObject(v)) {
            JSPropertyEnum *props = NULL; uint32_t count = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &count, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                env = calloc(count + 1, sizeof(char *));
                uint32_t written = 0;
                for (uint32_t i = 0; i < count; i++) {
                    JSValue val = JS_GetProperty(ctx, v, props[i].atom);
                    const char *key = JS_AtomToCString(ctx, props[i].atom);
                    const char *sval = JS_ToCString(ctx, val);
                    if (key && sval) {
                        size_t n = strlen(key) + strlen(sval) + 2;
                        char *entry = malloc(n);
                        snprintf(entry, n, "%s=%s", key, sval);
                        env[written++] = entry;
                    }
                    if (key) JS_FreeCString(ctx, key);
                    if (sval) JS_FreeCString(ctx, sval);
                    JS_FreeValue(ctx, val);
                    JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, v);
    }

    uv_loop_t loop;
    uv_loop_init(&loop);
    SxnGrow out = {0}, err = {0};
    uv_pipe_t in_pipe, out_pipe, err_pipe;
    uv_pipe_init(&loop, &in_pipe, 0);
    uv_pipe_init(&loop, &out_pipe, 0);
    uv_pipe_init(&loop, &err_pipe, 0);
    out_pipe.data = &out;
    err_pipe.data = &err;

    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
    stdio[0].data.stream = (uv_stream_t *)&in_pipe;
    stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[1].data.stream = (uv_stream_t *)&out_pipe;
    stdio[2].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[2].data.stream = (uv_stream_t *)&err_pipe;

    SxnExit exit_state = { -1, 0 };
    uv_process_t proc;
    proc.data = &exit_state;
    uv_process_options_t options;
    memset(&options, 0, sizeof(options));
    options.file = file;
    options.args = args;
    options.cwd = cwd;
    options.env = env;
    options.stdio = stdio;
    options.stdio_count = 3;
    options.exit_cb = sxn_spawn_exit;

    int rc = uv_spawn(&loop, &proc, &options);
    JSValue result = JS_NewObject(ctx);
    if (rc != 0) {
        uv_close((uv_handle_t *)&in_pipe, NULL);
        uv_close((uv_handle_t *)&out_pipe, NULL);
        uv_close((uv_handle_t *)&err_pipe, NULL);
        sxn_spawn_teardown(&loop);
        JS_SetPropertyStr(ctx, result, "error", JS_NewString(ctx, uv_strerror(rc)));
        JS_SetPropertyStr(ctx, result, "errno", JS_NewString(ctx, uv_err_name(rc)));
        JS_SetPropertyStr(ctx, result, "status", JS_NULL);
    } else {
        uv_read_start((uv_stream_t *)&out_pipe, sxn_spawn_alloc, sxn_spawn_read);
        uv_read_start((uv_stream_t *)&err_pipe, sxn_spawn_alloc, sxn_spawn_read);
        if (input) {
            uv_write_t *req = calloc(1, sizeof(*req));
            uv_buf_t buf = uv_buf_init((char *)input, (unsigned int)input_len);
            if (!req || uv_write(req, (uv_stream_t *)&in_pipe, &buf, 1, sxn_spawn_written) != 0) {
                free(req);
                uv_close((uv_handle_t *)&in_pipe, NULL);
            }
        } else {
            uv_close((uv_handle_t *)&in_pipe, NULL);
        }
        uv_run(&loop, UV_RUN_DEFAULT);
        sxn_spawn_teardown(&loop);
        JS_SetPropertyStr(ctx, result, "pid", JS_NewInt32(ctx, proc.pid));
        JS_SetPropertyStr(ctx, result, "status",
                          exit_state.signal ? JS_NULL : JS_NewInt64(ctx, exit_state.status));
        JS_SetPropertyStr(ctx, result, "signal",
                          exit_state.signal ? JS_NewInt32(ctx, exit_state.signal) : JS_NULL);
    }
    JS_SetPropertyStr(ctx, result, "stdout",
                      JS_NewUint8ArrayCopy(ctx, (const uint8_t *)(out.data ? out.data : ""), out.len));
    JS_SetPropertyStr(ctx, result, "stderr",
                      JS_NewUint8ArrayCopy(ctx, (const uint8_t *)(err.data ? err.data : ""), err.len));

    if (input) JS_FreeCString(ctx, input);
    free(out.data); free(err.data);
    for (uint32_t i = 0; i < nargs; i++) free(args[i + 1]);
    free(args);
    free(cwd);
    if (env) { for (char **e = env; *e; e++) free(*e); free(env); }
    JS_FreeCString(ctx, file);
    return result;
}

/* __sxnDnsLookup(hostname, family) -> [{ address, family }, ...]
   uv_getaddrinfo with no callback resolves on this thread, which is what
   node:dns's callback forms then hand back on a later tick. */
static JSValue js_dns_lookup(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "lookup needs a hostname");
    const char *host = JS_ToCString(ctx, argv[0]);
    if (!host) return JS_EXCEPTION;
    int32_t family = 0;
    if (argc > 1) JS_ToInt32(ctx, &family, argv[1]);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family == 4 ? AF_INET : family == 6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    uv_getaddrinfo_t req;
    int rc = uv_getaddrinfo(uv_default_loop(), &req, NULL, host, NULL, &hints);
    JS_FreeCString(ctx, host);
    if (rc != 0) {
        JSValue error = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, uv_strerror(rc)));
        JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, rc == UV_EAI_NONAME ? "ENOTFOUND" : uv_err_name(rc)));
        return JS_Throw(ctx, error);
    }
    JSValue list = JS_NewArray(ctx);
    uint32_t n = 0;
    for (struct addrinfo *ai = req.addrinfo; ai; ai = ai->ai_next) {
        char text[INET6_ADDRSTRLEN] = {0};
        int is6 = ai->ai_family == AF_INET6;
        if (is6) uv_ip6_name((struct sockaddr_in6 *)ai->ai_addr, text, sizeof(text));
        else if (ai->ai_family == AF_INET) uv_ip4_name((struct sockaddr_in *)ai->ai_addr, text, sizeof(text));
        else continue;
        JSValue entry = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, entry, "address", JS_NewString(ctx, text));
        JS_SetPropertyStr(ctx, entry, "family", JS_NewInt32(ctx, is6 ? 6 : 4));
        JS_SetPropertyUint32(ctx, list, n++, entry);
    }
    uv_freeaddrinfo(req.addrinfo);
    return list;
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

/* Called from sxn_install_node_compat (src/node.c). These names are only ever
   read by node_compat.js, so they are installed with it rather than with the
   runtime's own primitives. */
void sxn_install_node_sys(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__sxnWriteFileAsync", JS_NewCFunction(ctx, sxn_file_write_async, "__sxnWriteFileAsync", 2));
    JS_SetPropertyStr(ctx, global, "__sxnStat", JS_NewCFunction(ctx, sxn_stat, "__sxnStat", 3));
    JS_SetPropertyStr(ctx, global, "__sxnFsConstants", JS_NewCFunction(ctx, js_fs_constants, "__sxnFsConstants", 0));
    JS_SetPropertyStr(ctx, global, "__sxnUdpOpen", JS_NewCFunction(ctx, js_udp_open, "__sxnUdpOpen", 2));
    JS_SetPropertyStr(ctx, global, "__sxnUdpBind", JS_NewCFunction(ctx, js_udp_bind, "__sxnUdpBind", 3));
    JS_SetPropertyStr(ctx, global, "__sxnUdpSend", JS_NewCFunction(ctx, js_udp_send, "__sxnUdpSend", 4));
    JS_SetPropertyStr(ctx, global, "__sxnUdpClose", JS_NewCFunction(ctx, js_udp_close, "__sxnUdpClose", 1));
    JS_SetPropertyStr(ctx, global, "__sxnSpawnSync", JS_NewCFunction(ctx, js_spawn_sync, "__sxnSpawnSync", 3));
    JS_SetPropertyStr(ctx, global, "__sxnDnsLookup", JS_NewCFunction(ctx, js_dns_lookup, "__sxnDnsLookup", 2));
    JS_SetPropertyStr(ctx, global, "__sxnOsHostname", JS_NewCFunction(ctx, sxn_os_hostname, "__sxnOsHostname", 0));
    JS_SetPropertyStr(ctx, global, "__sxnOsHomedir", JS_NewCFunctionMagic(ctx, sxn_os_dir, "__sxnOsHomedir", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "__sxnOsTmpdir", JS_NewCFunctionMagic(ctx, sxn_os_dir, "__sxnOsTmpdir", 0, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "__sxnOsUname", JS_NewCFunction(ctx, sxn_os_uname, "__sxnOsUname", 0));
    JS_SetPropertyStr(ctx, global, "__sxnOsNumbers", JS_NewCFunction(ctx, sxn_os_numbers, "__sxnOsNumbers", 0));
    JS_SetPropertyStr(ctx, global, "__sxnOsCpus", JS_NewCFunction(ctx, sxn_os_cpus, "__sxnOsCpus", 0));
    JS_SetPropertyStr(ctx, global, "__sxnOsInterfaces", JS_NewCFunction(ctx, sxn_os_interfaces, "__sxnOsInterfaces", 0));
    JS_SetPropertyStr(ctx, global, "__sxnPid", JS_NewInt32(ctx, (int32_t)uv_os_getpid()));
    JS_FreeValue(ctx, global);
}
