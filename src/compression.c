/* CompressionStream and DecompressionStream, over the same zlib node:zlib
   uses.
 *
 * These two globals are WinterTC -- tests/fixtures/wintertc_surface.mjs
 * counts them among the 62 names of the Minimum Common API -- but their
 * native primitives used to live in src/node.c and be installed by
 * sxn_install_node_compat, so a build without the node layer lost them. They
 * are the runtime's, and they are installed with the rest of the runtime's
 * primitives now.
 *
 * bootstrap.js's compressionTransform() is the JavaScript half; it reaches
 * these through __sxnZlibStreamNew/__sxnZlibStreamPush. */

#include <quickjs.h>
#include <zlib.h>

#include <stdbool.h>

/* Streaming zlib: one z_stream per object, alive as long as the object is.
   node:zlib's one-shot deflate/inflate (src/node.c) keeps a single reset
   stream for the whole call, which is right for one buffer in and one out and
   wrong for a chunk at a time. The two share no state, which is what let this
   pair move here. */
static JSClassID sxn_zstream_class_id;

typedef struct { z_stream zs; bool compress, open; } SxnZStream;

static void sxn_zstream_finalizer(JSRuntime *rt, JSValue val) {
    SxnZStream *s = JS_GetOpaque(val, sxn_zstream_class_id);
    if (!s) return;
    if (s->open) { if (s->compress) deflateEnd(&s->zs); else inflateEnd(&s->zs); }
    js_free_rt(rt, s);
}

static JSClassDef sxn_zstream_class_def = {
    .class_name = "ZlibStream",
    .finalizer = sxn_zstream_finalizer,
};

/* __sxnZlibStreamNew(windowBits, level, decompress) */
static JSValue js_zlib_stream_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t window_bits = 15, level = Z_DEFAULT_COMPRESSION;
    if (argc > 0) JS_ToInt32(ctx, &window_bits, argv[0]);
    if (argc > 1) JS_ToInt32(ctx, &level, argv[1]);
    bool decompress = argc > 2 && JS_ToBool(ctx, argv[2]);

    SxnZStream *s = js_mallocz(ctx, sizeof(*s));
    if (!s) return JS_EXCEPTION;
    s->compress = !decompress;
    int rc = decompress ? inflateInit2(&s->zs, window_bits)
                        : deflateInit2(&s->zs, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) { js_free(ctx, s); return JS_ThrowInternalError(ctx, "zlib init failed: %d", rc); }
    s->open = true;

    JS_NewClassID(JS_GetRuntime(ctx), &sxn_zstream_class_id);
    JS_NewClass(JS_GetRuntime(ctx), sxn_zstream_class_id, &sxn_zstream_class_def);
    JSValue obj = JS_NewObjectClass(ctx, sxn_zstream_class_id);
    if (JS_IsException(obj)) { sxn_zstream_finalizer(JS_GetRuntime(ctx), obj); return obj; }
    JS_SetOpaque(obj, s);
    return obj;
}

/* __sxnZlibStreamPush(stream, bytes, finish) -> Uint8Array of whatever came out */
static JSValue js_zlib_stream_push(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    SxnZStream *s = argc > 0 ? JS_GetOpaque(argv[0], sxn_zstream_class_id) : NULL;
    if (!s || !s->open) return JS_ThrowTypeError(ctx, "not an open zlib stream");
    size_t in_len = 0;
    uint8_t *in = NULL;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        in = JS_GetUint8Array(ctx, &in_len, argv[1]);
        if (!in) return JS_ThrowTypeError(ctx, "zlib expects bytes");
    }
    bool finish = argc > 2 && JS_ToBool(ctx, argv[2]);

    size_t cap = in_len + 64, len = 0;
    uint8_t *out = js_malloc(ctx, cap);
    if (!out) return JS_EXCEPTION;
    s->zs.next_in = in;
    s->zs.avail_in = (uInt)in_len;
    int rc = Z_OK;
    do {
        if (len == cap) {
            uint8_t *grown = js_realloc(ctx, out, cap * 2);
            if (!grown) { js_free(ctx, out); return JS_EXCEPTION; }
            out = grown; cap *= 2;
        }
        s->zs.next_out = out + len;
        s->zs.avail_out = (uInt)(cap - len);
        rc = s->compress ? deflate(&s->zs, finish ? Z_FINISH : Z_NO_FLUSH)
                         : inflate(&s->zs, finish ? Z_FINISH : Z_NO_FLUSH);
        len = cap - s->zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            js_free(ctx, out);
            return JS_ThrowInternalError(ctx, "zlib %s failed: %d", s->compress ? "deflate" : "inflate", rc);
        }
        if (rc == Z_BUF_ERROR && !finish) break;
    } while (s->zs.avail_out == 0 || (finish && rc != Z_STREAM_END));

    if (finish) {
        if (!s->compress && rc != Z_STREAM_END) {
            js_free(ctx, out);
            return JS_ThrowInternalError(ctx, "unexpected end of compressed data");
        }
        if (s->compress) deflateEnd(&s->zs); else inflateEnd(&s->zs);
        s->open = false;
    }
    JSValue bytes = JS_NewUint8ArrayCopy(ctx, out, len);
    js_free(ctx, out);
    return bytes;
}

/* Called from sxn_install_runtime (src/network.c), beside the other
   primitives bootstrap.js consumes. */
void sxn_install_compression(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__sxnZlibStreamNew",
                      JS_NewCFunction(ctx, js_zlib_stream_new, "__sxnZlibStreamNew", 3));
    JS_SetPropertyStr(ctx, global, "__sxnZlibStreamPush",
                      JS_NewCFunction(ctx, js_zlib_stream_push, "__sxnZlibStreamPush", 3));
    JS_FreeValue(ctx, global);
}
