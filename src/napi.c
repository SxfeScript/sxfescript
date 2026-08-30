/* Node-API on QuickJS: enough of it to load and run a real `.node` addon.
 *
 * This is the Node-compatibility side of the fence, not the runtime side.
 * Sxn.ffi (src/ffi.c) is an engine capability -- Rayact embeds this engine
 * and loads native code in its own core. Node-API is the opposite: it exists
 * only to run npm's compiled addons, Rayact has its own module ABI and wants
 * none of it, and a mobile build drops this file entirely (SXN_ENABLE_NAPI).
 *
 * The shape of the problem is worth stating, because it is the reverse of
 * FFI. An addon exports one symbol, `napi_register_module_v1`, and imports
 * around seventy `napi_*` functions that the *host* must provide. So loading
 * one is not a matter of calling into a library; it is a matter of being the
 * library it calls into. That is why these are ordinary exported C functions
 * and why the executable is linked with its dynamic symbols exported.
 *
 * A napi_value is a JSValue owned by the innermost handle scope. QuickJS is
 * refcounted rather than tracing, so a scope is a plain array of values it
 * releases on close -- simpler than the equivalent on V8, and it means an
 * addon that leaks handles leaks memory rather than corrupting anything.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>
#include <assert.h>

#include "quickjs.h"
#include <uv.h>

#include "js_native_api.h"
#include "node_api.h"

/* ------------------------------------------------------------------ env */

/* A napi_value is a pointer to the slot holding its JSValue, so the slots
   must never move. A growable array would relocate every handle the addon is
   still holding the moment it needed one more -- which is exactly what a
   large addon does. Blocks are allocated and never resized. */
#define NAPI_SCOPE_BLOCK 64
typedef struct NapiBlock {
    struct NapiBlock *next;
    size_t len;
    JSValue values[NAPI_SCOPE_BLOCK];
} NapiBlock;

/* A handle used after its scope closed is a read of freed memory, and the
   addon is the only one who can be wrong about it. Release cannot afford to
   check; the assertions build can, so there it keeps the blocks and stamps
   every slot, and the next read of one says so instead of returning
   whatever now lives at that address. This is the whole point of shipping
   two builds: the checked one finds it, the fast one costs nothing. */
#ifndef NDEBUG
#define NAPI_DEAD_HANDLE JS_MKVAL(JS_TAG_UNINITIALIZED, 0x5ca1e)
static NapiBlock *napi_dead_blocks;
static bool napi_is_dead(JSValue v) {
    return JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED &&
           JS_VALUE_GET_INT(v) == 0x5ca1e;
}
#endif

typedef struct NapiScope {
    struct NapiScope *parent;
    NapiBlock *blocks;          /* newest first; the one being filled */
    JSValue escaped;            /* escapable scopes: at most one, per spec */
    bool did_escape;
} NapiScope;

struct napi_env__ {
    JSContext *ctx;
    NapiScope *scope;
    JSValue pending;            /* the exception an addon has thrown but not observed */
    napi_extended_error_info err;
    char err_message[256];
    uv_loop_t *loop;
};

static napi_env sxn_env;        /* one context, one env */

/* Every entry point returns through here, so the "last error" an addon reads
   after a failure is always the failure it just had. */
static napi_status napi_set_error(napi_env env, napi_status code, const char *msg) {
    if (!env) return code;
    env->err.error_code = code;
    env->err.engine_error_code = 0;
    env->err.engine_reserved = NULL;
    if (msg) {
        snprintf(env->err_message, sizeof(env->err_message), "%s", msg);
        env->err.error_message = env->err_message;
    } else {
        env->err.error_message = NULL;
    }
    return code;
}
#define NAPI_OK(env)            napi_set_error((env), napi_ok, NULL)
#define NAPI_FAIL(env, c, m)    napi_set_error((env), (c), (m))
#define CHECK_ENV(env)          do { if (!(env)) return napi_invalid_arg; } while (0)
#define CHECK_ARG(env, a)       do { if (!(a)) return NAPI_FAIL((env), napi_invalid_arg, #a " is null"); } while (0)

/* A JSValue becomes a napi_value by being parked in the current scope. The
   scope owns the reference; the addon holds only an index into it. */
static napi_value napi_hold(napi_env env, JSValue v) {
    NapiScope *s = env->scope;
    if (!s) { JS_FreeValue(env->ctx, v); return NULL; }
    NapiBlock *b = s->blocks;
    if (!b || b->len == NAPI_SCOPE_BLOCK) {
        b = malloc(sizeof(*b));
        if (!b) { JS_FreeValue(env->ctx, v); return NULL; }
        b->next = s->blocks; b->len = 0;
        s->blocks = b;
    }
    b->values[b->len] = v;
    return (napi_value)&b->values[b->len++];
}
static JSValue napi_val(napi_value v) {
    if (!v) return JS_UNDEFINED;
#ifndef NDEBUG
    assert(!napi_is_dead(*(JSValue *)v) &&
           "a native addon used a napi_value after its handle scope closed");
#endif
    return *(JSValue *)v;
}

/* An exception thrown by JS during a call the addon made is stashed rather
   than left on the context, because the addon is expected to ask for it with
   napi_get_and_clear_last_exception and QuickJS's own pending exception would
   otherwise surface at the wrong boundary. */
static napi_status napi_catch(napi_env env) {
    JSValue e = JS_GetException(env->ctx);
    if (JS_IsUninitialized(e) || JS_IsNull(e)) { JS_FreeValue(env->ctx, e); return NAPI_OK(env); }
    JS_FreeValue(env->ctx, env->pending);
    env->pending = e;
    return NAPI_FAIL(env, napi_pending_exception, "an exception is pending");
}
#define NAPI_TRY(env, v) do { if (JS_IsException(v)) return napi_catch(env); } while (0)

static napi_status napi_push_scope(napi_env env, NapiScope *s) {
    memset(s, 0, sizeof(*s));
    s->parent = env->scope;
    s->escaped = JS_UNINITIALIZED;
    env->scope = s;
    return NAPI_OK(env);
}
static void napi_pop_scope(napi_env env) {
    NapiScope *s = env->scope;
    if (!s) return;
    for (NapiBlock *b = s->blocks; b; ) {
        NapiBlock *next = b->next;
        for (size_t i = 0; i < b->len; i++) JS_FreeValue(env->ctx, b->values[i]);
#ifndef NDEBUG
        for (size_t i = 0; i < NAPI_SCOPE_BLOCK; i++) b->values[i] = NAPI_DEAD_HANDLE;
        b->next = napi_dead_blocks;
        napi_dead_blocks = b;       /* held, so a stale read lands on the stamp */
#else
        free(b);
#endif
        b = next;
    }
    env->scope = s->parent;
}

/* --------------------------------------------------------------- errors */

napi_status napi_get_last_error_info(node_api_basic_env env,
                                     const napi_extended_error_info **result) {
    CHECK_ENV(env); CHECK_ARG((napi_env)env, result);
    *result = &((napi_env)env)->err;
    return napi_ok;
}
napi_status napi_throw(napi_env env, napi_value error) {
    CHECK_ENV(env);
    JS_Throw(env->ctx, JS_DupValue(env->ctx, napi_val(error)));
    return napi_catch(env);
}
static napi_status napi_throw_kind(napi_env env, const char *ctor,
                                   const char *code, const char *msg) {
    JSValue global = JS_GetGlobalObject(env->ctx);
    JSValue c = JS_GetPropertyStr(env->ctx, global, ctor);
    JS_FreeValue(env->ctx, global);
    JSValue m = JS_NewString(env->ctx, msg ? msg : "");
    JSValue e = JS_CallConstructor(env->ctx, c, 1, (JSValueConst *)&m);
    JS_FreeValue(env->ctx, m);
    JS_FreeValue(env->ctx, c);
    if (JS_IsException(e)) return napi_catch(env);
    if (code) JS_SetPropertyStr(env->ctx, e, "code", JS_NewString(env->ctx, code));
    JS_Throw(env->ctx, e);
    return napi_catch(env);
}
napi_status napi_throw_error(napi_env env, const char *code, const char *msg) {
    CHECK_ENV(env); return napi_throw_kind(env, "Error", code, msg);
}
napi_status napi_throw_type_error(napi_env env, const char *code, const char *msg) {
    CHECK_ENV(env); return napi_throw_kind(env, "TypeError", code, msg);
}
napi_status napi_throw_range_error(napi_env env, const char *code, const char *msg) {
    CHECK_ENV(env); return napi_throw_kind(env, "RangeError", code, msg);
}
static napi_status napi_make_error(napi_env env, const char *ctor, napi_value code,
                                   napi_value msg, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue global = JS_GetGlobalObject(env->ctx);
    JSValue c = JS_GetPropertyStr(env->ctx, global, ctor);
    JS_FreeValue(env->ctx, global);
    JSValue m = JS_DupValue(env->ctx, napi_val(msg));
    JSValue e = JS_CallConstructor(env->ctx, c, 1, (JSValueConst *)&m);
    JS_FreeValue(env->ctx, m);
    JS_FreeValue(env->ctx, c);
    NAPI_TRY(env, e);
    if (code) JS_SetPropertyStr(env->ctx, e, "code", JS_DupValue(env->ctx, napi_val(code)));
    *result = napi_hold(env, e);
    return NAPI_OK(env);
}
napi_status napi_create_error(napi_env env, napi_value code, napi_value msg, napi_value *r) {
    return napi_make_error(env, "Error", code, msg, r);
}
napi_status napi_create_type_error(napi_env env, napi_value code, napi_value msg, napi_value *r) {
    return napi_make_error(env, "TypeError", code, msg, r);
}
napi_status napi_create_range_error(napi_env env, napi_value code, napi_value msg, napi_value *r) {
    return napi_make_error(env, "RangeError", code, msg, r);
}
napi_status napi_is_exception_pending(napi_env env, bool *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    *result = !JS_IsUninitialized(env->pending);
    return NAPI_OK(env);
}
napi_status napi_get_and_clear_last_exception(napi_env env, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    if (JS_IsUninitialized(env->pending)) {
        JSValue u = JS_UNDEFINED;
        *result = napi_hold(env, u);
    } else {
        *result = napi_hold(env, env->pending);
        env->pending = JS_UNINITIALIZED;
    }
    return NAPI_OK(env);
}
napi_status napi_fatal_exception(napi_env env, napi_value err) {
    CHECK_ENV(env);
    const char *s = JS_ToCString(env->ctx, napi_val(err));
    fprintf(stderr, "sxn: uncaught exception from a native addon: %s\n", s ? s : "?");
    if (s) JS_FreeCString(env->ctx, s);
    return NAPI_OK(env);
}
void napi_fatal_error(const char *location, size_t loc_len,
                      const char *message, size_t msg_len) {
    (void)loc_len; (void)msg_len;
    fprintf(stderr, "sxn: fatal error in a native addon at %s: %s\n",
            location ? location : "?", message ? message : "?");
    abort();
}

/* --------------------------------------------------------------- scopes */

napi_status napi_open_handle_scope(napi_env env, napi_handle_scope *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    NapiScope *s = malloc(sizeof(*s));
    if (!s) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    napi_push_scope(env, s);
    *result = (napi_handle_scope)s;
    return NAPI_OK(env);
}
napi_status napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
    CHECK_ENV(env);
    if ((NapiScope *)scope != env->scope)
        return NAPI_FAIL(env, napi_handle_scope_mismatch, "handle scopes closed out of order");
    napi_pop_scope(env);
    free(scope);
    return NAPI_OK(env);
}
napi_status napi_open_escapable_handle_scope(napi_env env, napi_escapable_handle_scope *result) {
    return napi_open_handle_scope(env, (napi_handle_scope *)result);
}
napi_status napi_close_escapable_handle_scope(napi_env env, napi_escapable_handle_scope scope) {
    return napi_close_handle_scope(env, (napi_handle_scope)scope);
}
napi_status napi_escape_handle(napi_env env, napi_escapable_handle_scope scope,
                               napi_value escapee, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    NapiScope *s = (NapiScope *)scope;
    if (!s || s->did_escape)
        return NAPI_FAIL(env, napi_escape_called_twice, "escape_handle called twice");
    s->did_escape = true;
    /* The value has to outlive this scope, so it is re-parked in the parent. */
    NapiScope *inner = env->scope;
    env->scope = s->parent;
    *result = napi_hold(env, JS_DupValue(env->ctx, napi_val(escapee)));
    env->scope = inner;
    return NAPI_OK(env);
}

/* ----------------------------------------------------------- references */

/* A reference keeps a value alive past its scope. Strong is a held JSValue;
   a weak reference (count 0) is not tracked here -- QuickJS has no weak
   handle that can be resurrected, so it is kept strong and reported as such,
   which leaks rather than dangles. */
typedef struct NapiRef {
    JSValue value;
    uint32_t count;
    struct NapiRef *next, **prev;   /* every live one, so teardown can free them */
} NapiRef;
static NapiRef *napi_refs;

napi_status napi_create_reference(napi_env env, napi_value value,
                                  uint32_t initial, napi_ref *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    NapiRef *r = malloc(sizeof(*r));
    if (!r) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    r->value = JS_DupValue(env->ctx, napi_val(value));
    r->count = initial;
    r->next = napi_refs; r->prev = &napi_refs;
    if (napi_refs) napi_refs->prev = &r->next;
    napi_refs = r;
    *result = (napi_ref)r;
    return NAPI_OK(env);
}
napi_status napi_delete_reference(napi_env env, napi_ref ref) {
    CHECK_ENV(env); CHECK_ARG(env, ref);
    NapiRef *r = (NapiRef *)ref;
    if (r->prev) { *r->prev = r->next; if (r->next) r->next->prev = r->prev; }
    JS_FreeValue(env->ctx, r->value);
    free(r);
    return NAPI_OK(env);
}
napi_status napi_reference_ref(napi_env env, napi_ref ref, uint32_t *result) {
    CHECK_ENV(env); CHECK_ARG(env, ref);
    NapiRef *r = (NapiRef *)ref;
    r->count++;
    if (result) *result = r->count;
    return NAPI_OK(env);
}
napi_status napi_reference_unref(napi_env env, napi_ref ref, uint32_t *result) {
    CHECK_ENV(env); CHECK_ARG(env, ref);
    NapiRef *r = (NapiRef *)ref;
    if (r->count) r->count--;
    if (result) *result = r->count;
    return NAPI_OK(env);
}
napi_status napi_get_reference_value(napi_env env, napi_ref ref, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, ref); CHECK_ARG(env, result);
    NapiRef *r = (NapiRef *)ref;
    *result = napi_hold(env, JS_DupValue(env->ctx, r->value));
    return NAPI_OK(env);
}

/* ------------------------------------------------------- value creation */

napi_status napi_get_undefined(napi_env env, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue v = JS_UNDEFINED; *result = napi_hold(env, v); return NAPI_OK(env);
}
napi_status napi_get_null(napi_env env, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue v = JS_NULL; *result = napi_hold(env, v); return NAPI_OK(env);
}
napi_status napi_get_boolean(napi_env env, bool value, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    *result = napi_hold(env, JS_NewBool(env->ctx, value)); return NAPI_OK(env);
}
napi_status napi_get_global(napi_env env, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    *result = napi_hold(env, JS_GetGlobalObject(env->ctx)); return NAPI_OK(env);
}
napi_status napi_create_double(napi_env env, double v, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    *r = napi_hold(env, JS_NewFloat64(env->ctx, v)); return NAPI_OK(env);
}
napi_status napi_create_int32(napi_env env, int32_t v, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    *r = napi_hold(env, JS_NewInt32(env->ctx, v)); return NAPI_OK(env);
}
napi_status napi_create_uint32(napi_env env, uint32_t v, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    *r = napi_hold(env, JS_NewUint32(env->ctx, v)); return NAPI_OK(env);
}
napi_status napi_create_int64(napi_env env, int64_t v, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    *r = napi_hold(env, JS_NewInt64(env->ctx, v)); return NAPI_OK(env);
}
napi_status napi_create_bigint_int64(napi_env env, int64_t v, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    *r = napi_hold(env, JS_NewBigInt64(env->ctx, v)); return NAPI_OK(env);
}
napi_status napi_create_bigint_uint64(napi_env env, uint64_t v, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    *r = napi_hold(env, JS_NewBigUint64(env->ctx, v)); return NAPI_OK(env);
}
napi_status napi_create_string_utf8(napi_env env, const char *str, size_t len, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    if (len == NAPI_AUTO_LENGTH) len = str ? strlen(str) : 0;
    *r = napi_hold(env, JS_NewStringLen(env->ctx, str ? str : "", len));
    return NAPI_OK(env);
}
napi_status napi_create_string_latin1(napi_env env, const char *str, size_t len, napi_value *r) {
    /* Latin-1 is not UTF-8 above 0x7f, so widen each byte rather than hand
       the bytes to a UTF-8 reader and get replacement characters. */
    CHECK_ENV(env); CHECK_ARG(env, r);
    if (len == NAPI_AUTO_LENGTH) len = str ? strlen(str) : 0;
    char *tmp = malloc(len * 2 + 1);
    if (!tmp) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c < 0x80) tmp[n++] = (char)c;
        else { tmp[n++] = (char)(0xc0 | (c >> 6)); tmp[n++] = (char)(0x80 | (c & 0x3f)); }
    }
    *r = napi_hold(env, JS_NewStringLen(env->ctx, tmp, n));
    free(tmp);
    return NAPI_OK(env);
}
napi_status napi_create_object(napi_env env, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    *r = napi_hold(env, JS_NewObject(env->ctx)); return NAPI_OK(env);
}
napi_status napi_create_array(napi_env env, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    *r = napi_hold(env, JS_NewArray(env->ctx)); return NAPI_OK(env);
}
napi_status napi_create_array_with_length(napi_env env, size_t len, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    JSValue a = JS_NewArray(env->ctx);
    NAPI_TRY(env, a);
    JS_SetPropertyStr(env->ctx, a, "length", JS_NewInt64(env->ctx, (int64_t)len));
    *r = napi_hold(env, a);
    return NAPI_OK(env);
}
napi_status napi_create_symbol(napi_env env, napi_value desc, napi_value *r) {
    CHECK_ENV(env); CHECK_ARG(env, r);
    JSValue global = JS_GetGlobalObject(env->ctx);
    JSValue sym = JS_GetPropertyStr(env->ctx, global, "Symbol");
    JS_FreeValue(env->ctx, global);
    JSValue d = JS_DupValue(env->ctx, napi_val(desc));
    JSValue out = JS_Call(env->ctx, sym, JS_UNDEFINED, desc ? 1 : 0, (JSValueConst *)&d);
    JS_FreeValue(env->ctx, d);
    JS_FreeValue(env->ctx, sym);
    NAPI_TRY(env, out);
    *r = napi_hold(env, out);
    return NAPI_OK(env);
}

/* ------------------------------------------------------- value reading */

napi_status napi_typeof(napi_env env, napi_value value, napi_valuetype *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue v = napi_val(value);
    switch (JS_VALUE_GET_NORM_TAG(v)) {
    case JS_TAG_UNDEFINED: *result = napi_undefined; break;
    case JS_TAG_NULL:      *result = napi_null; break;
    case JS_TAG_BOOL:      *result = napi_boolean; break;
    case JS_TAG_INT:
    case JS_TAG_FLOAT64:   *result = napi_number; break;
    case JS_TAG_STRING:    *result = napi_string; break;
    case JS_TAG_SYMBOL:    *result = napi_symbol; break;
    case JS_TAG_BIG_INT:   *result = napi_bigint; break;
    case JS_TAG_OBJECT:
        *result = JS_IsFunction(env->ctx, v) ? napi_function : napi_object; break;
    default:               *result = napi_undefined; break;
    }
    return NAPI_OK(env);
}
napi_status napi_get_value_double(napi_env env, napi_value value, double *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    if (JS_ToFloat64(env->ctx, result, napi_val(value)) < 0) return napi_catch(env);
    return NAPI_OK(env);
}
napi_status napi_get_value_int32(napi_env env, napi_value value, int32_t *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    if (JS_ToInt32(env->ctx, result, napi_val(value)) < 0) return napi_catch(env);
    return NAPI_OK(env);
}
napi_status napi_get_value_uint32(napi_env env, napi_value value, uint32_t *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    if (JS_ToUint32(env->ctx, result, napi_val(value)) < 0) return napi_catch(env);
    return NAPI_OK(env);
}
napi_status napi_get_value_int64(napi_env env, napi_value value, int64_t *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    if (JS_ToInt64(env->ctx, result, napi_val(value)) < 0) return napi_catch(env);
    return NAPI_OK(env);
}
napi_status napi_get_value_bigint_int64(napi_env env, napi_value value,
                                        int64_t *result, bool *lossless) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    if (JS_ToBigInt64(env->ctx, result, napi_val(value)) < 0) return napi_catch(env);
    if (lossless) *lossless = true;
    return NAPI_OK(env);
}
napi_status napi_get_value_bigint_uint64(napi_env env, napi_value value,
                                         uint64_t *result, bool *lossless) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    int64_t s;
    if (JS_ToBigInt64(env->ctx, &s, napi_val(value)) < 0) return napi_catch(env);
    *result = (uint64_t)s;
    if (lossless) *lossless = s >= 0;
    return NAPI_OK(env);
}
napi_status napi_get_value_bool(napi_env env, napi_value value, bool *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    int b = JS_ToBool(env->ctx, napi_val(value));
    if (b < 0) return napi_catch(env);
    *result = b != 0;
    return NAPI_OK(env);
}
napi_status napi_get_value_string_utf8(napi_env env, napi_value value,
                                       char *buf, size_t bufsize, size_t *copied) {
    CHECK_ENV(env);
    size_t len = 0;
    const char *s = JS_ToCStringLen(env->ctx, &len, napi_val(value));
    if (!s) return napi_catch(env);
    if (!buf) {                       /* the sizing call: how big a buffer to allocate */
        if (copied) *copied = len;
    } else {
        size_t n = len < bufsize - 1 ? len : (bufsize ? bufsize - 1 : 0);
        if (bufsize) { memcpy(buf, s, n); buf[n] = 0; }
        if (copied) *copied = n;
    }
    JS_FreeCString(env->ctx, s);
    return NAPI_OK(env);
}
napi_status napi_get_value_string_latin1(napi_env env, napi_value value,
                                         char *buf, size_t bufsize, size_t *copied) {
    return napi_get_value_string_utf8(env, value, buf, bufsize, copied);
}
napi_status napi_coerce_to_string(napi_env env, napi_value value, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue s = JS_ToString(env->ctx, napi_val(value));
    NAPI_TRY(env, s);
    *result = napi_hold(env, s);
    return NAPI_OK(env);
}
napi_status napi_coerce_to_number(napi_env env, napi_value value, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    double d;
    if (JS_ToFloat64(env->ctx, &d, napi_val(value)) < 0) return napi_catch(env);
    *result = napi_hold(env, JS_NewFloat64(env->ctx, d));
    return NAPI_OK(env);
}
napi_status napi_coerce_to_bool(napi_env env, napi_value value, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    *result = napi_hold(env, JS_NewBool(env->ctx, JS_ToBool(env->ctx, napi_val(value)) != 0));
    return NAPI_OK(env);
}
napi_status napi_coerce_to_object(napi_env env, napi_value value, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    *result = napi_hold(env, JS_ToObject(env->ctx, napi_val(value)));
    return NAPI_OK(env);
}
napi_status napi_strict_equals(napi_env env, napi_value a, napi_value b, bool *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    *result = JS_IsStrictEqual(env->ctx, napi_val(a), napi_val(b));
    return NAPI_OK(env);
}
napi_status napi_is_array(napi_env env, napi_value v, bool *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    *result = JS_IsArray(napi_val(v));
    return NAPI_OK(env);
}
napi_status napi_is_error(napi_env env, napi_value v, bool *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    *result = JS_IsError(napi_val(v));
    return NAPI_OK(env);
}

/* ----------------------------------------------------------- properties */

napi_status napi_set_property(napi_env env, napi_value obj, napi_value key, napi_value value) {
    CHECK_ENV(env);
    JSAtom a = JS_ValueToAtom(env->ctx, napi_val(key));
    if (a == JS_ATOM_NULL) return napi_catch(env);
    int rc = JS_SetProperty(env->ctx, napi_val(obj), a, JS_DupValue(env->ctx, napi_val(value)));
    JS_FreeAtom(env->ctx, a);
    if (rc < 0) return napi_catch(env);
    return NAPI_OK(env);
}
napi_status napi_get_property(napi_env env, napi_value obj, napi_value key, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSAtom a = JS_ValueToAtom(env->ctx, napi_val(key));
    if (a == JS_ATOM_NULL) return napi_catch(env);
    JSValue v = JS_GetProperty(env->ctx, napi_val(obj), a);
    JS_FreeAtom(env->ctx, a);
    NAPI_TRY(env, v);
    *result = napi_hold(env, v);
    return NAPI_OK(env);
}
napi_status napi_has_property(napi_env env, napi_value obj, napi_value key, bool *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSAtom a = JS_ValueToAtom(env->ctx, napi_val(key));
    if (a == JS_ATOM_NULL) return napi_catch(env);
    int rc = JS_HasProperty(env->ctx, napi_val(obj), a);
    JS_FreeAtom(env->ctx, a);
    if (rc < 0) return napi_catch(env);
    *result = rc != 0;
    return NAPI_OK(env);
}
napi_status napi_delete_property(napi_env env, napi_value obj, napi_value key, bool *result) {
    CHECK_ENV(env);
    JSAtom a = JS_ValueToAtom(env->ctx, napi_val(key));
    if (a == JS_ATOM_NULL) return napi_catch(env);
    int rc = JS_DeleteProperty(env->ctx, napi_val(obj), a, 0);
    JS_FreeAtom(env->ctx, a);
    if (rc < 0) return napi_catch(env);
    if (result) *result = rc != 0;
    return NAPI_OK(env);
}
napi_status napi_has_own_property(napi_env env, napi_value obj, napi_value key, bool *result) {
    return napi_has_property(env, obj, key, result);
}
napi_status napi_set_named_property(napi_env env, napi_value obj, const char *name, napi_value value) {
    CHECK_ENV(env);
    if (JS_SetPropertyStr(env->ctx, napi_val(obj), name,
                          JS_DupValue(env->ctx, napi_val(value))) < 0)
        return napi_catch(env);
    return NAPI_OK(env);
}
napi_status napi_get_named_property(napi_env env, napi_value obj, const char *name, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue v = JS_GetPropertyStr(env->ctx, napi_val(obj), name);
    NAPI_TRY(env, v);
    *result = napi_hold(env, v);
    return NAPI_OK(env);
}
napi_status napi_has_named_property(napi_env env, napi_value obj, const char *name, bool *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSAtom a = JS_NewAtom(env->ctx, name);
    int rc = JS_HasProperty(env->ctx, napi_val(obj), a);
    JS_FreeAtom(env->ctx, a);
    if (rc < 0) return napi_catch(env);
    *result = rc != 0;
    return NAPI_OK(env);
}
napi_status napi_set_element(napi_env env, napi_value obj, uint32_t i, napi_value value) {
    CHECK_ENV(env);
    if (JS_SetPropertyUint32(env->ctx, napi_val(obj), i,
                             JS_DupValue(env->ctx, napi_val(value))) < 0)
        return napi_catch(env);
    return NAPI_OK(env);
}
napi_status napi_get_element(napi_env env, napi_value obj, uint32_t i, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue v = JS_GetPropertyUint32(env->ctx, napi_val(obj), i);
    NAPI_TRY(env, v);
    *result = napi_hold(env, v);
    return NAPI_OK(env);
}
napi_status napi_get_array_length(napi_env env, napi_value obj, uint32_t *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    int64_t n = 0;
    if (JS_GetLength(env->ctx, napi_val(obj), &n) < 0) return napi_catch(env);
    *result = (uint32_t)n;
    return NAPI_OK(env);
}
napi_status napi_get_property_names(napi_env env, napi_value obj, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(env->ctx, &tab, &len, napi_val(obj),
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return napi_catch(env);
    JSValue arr = JS_NewArray(env->ctx);
    for (uint32_t i = 0; i < len; i++)
        JS_SetPropertyUint32(env->ctx, arr, i, JS_AtomToValue(env->ctx, tab[i].atom));
    JS_FreePropertyEnum(env->ctx, tab, len);
    *result = napi_hold(env, arr);
    return NAPI_OK(env);
}

/* ------------------------------------------------------------ functions */

/* The bridge between a QuickJS call and an addon's napi_callback. Each call
   gets its own handle scope, so an addon that allocates values per call does
   not grow the caller's scope. */
typedef struct {
    napi_callback cb;
    void *data;
    JSValue this_ref;           /* for a constructor: the class it belongs to */
} NapiFunc;

static JSClassID napi_func_class_id;
static void napi_func_finalizer(JSRuntime *rt, JSValue val) {
    NapiFunc *f = JS_GetOpaque(val, napi_func_class_id);
    if (f) { JS_FreeValueRT(rt, f->this_ref); free(f); }
}
static JSClassDef napi_func_class = { "NapiCallback", .finalizer = napi_func_finalizer };

struct napi_callback_info__ {
    JSValueConst this_val;
    int argc;
    JSValueConst *argv;
    void *data;
    JSValue new_target;
};

static JSValue napi_call_bridge(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv,
                                int magic, JSValue *func_data) {
    (void)magic;
    napi_env env = sxn_env;
    NapiFunc *f = JS_GetOpaque(func_data[0], napi_func_class_id);
    if (!f) return JS_ThrowTypeError(ctx, "not a native callback");

    struct napi_callback_info__ info = { this_val, argc, argv, f->data, JS_UNDEFINED };
    NapiScope scope;
    napi_push_scope(env, &scope);
    napi_value out = f->cb(env, &info);
    JSValue result = out ? JS_DupValue(ctx, napi_val(out)) : JS_UNDEFINED;
    napi_pop_scope(env);

    /* An addon signals failure by leaving an exception pending rather than by
       a return code, so re-raise it on the way out. */
    if (!JS_IsUninitialized(env->pending)) {
        JSValue e = env->pending;
        env->pending = JS_UNINITIALIZED;
        JS_FreeValue(ctx, result);
        return JS_Throw(ctx, e);
    }
    return result;
}

napi_status napi_get_cb_info(napi_env env, napi_callback_info cbinfo,
                             size_t *argc, napi_value *argv,
                             napi_value *this_arg, void **data) {
    CHECK_ENV(env); CHECK_ARG(env, cbinfo);
    struct napi_callback_info__ *info = cbinfo;
    if (argv && argc) {
        size_t want = *argc;
        for (size_t i = 0; i < want; i++)
            argv[i] = i < (size_t)info->argc
                    ? napi_hold(env, JS_DupValue(env->ctx, info->argv[i]))
                    : napi_hold(env, JS_UNDEFINED);
    }
    if (argc) *argc = (size_t)info->argc;
    if (this_arg) *this_arg = napi_hold(env, JS_DupValue(env->ctx, info->this_val));
    if (data) *data = info->data;
    return NAPI_OK(env);
}
napi_status napi_get_new_target(napi_env env, napi_callback_info cbinfo, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    struct napi_callback_info__ *info = cbinfo;
    *result = JS_IsUndefined(info->new_target) ? NULL
            : napi_hold(env, JS_DupValue(env->ctx, info->new_target));
    return NAPI_OK(env);
}

static JSValue napi_make_function(napi_env env, const char *name, size_t len,
                                  napi_callback cb, void *data) {
    NapiFunc *f = malloc(sizeof(*f));
    if (!f) return JS_ThrowOutOfMemory(env->ctx);
    f->cb = cb; f->data = data; f->this_ref = JS_UNDEFINED;
    JSValue holder = JS_NewObjectClass(env->ctx, napi_func_class_id);
    if (JS_IsException(holder)) { free(f); return holder; }
    JS_SetOpaque(holder, f);
    JSValue fn = JS_NewCFunctionData(env->ctx, napi_call_bridge, 0, 0, 1, (JSValueConst *)&holder);
    JS_FreeValue(env->ctx, holder);
    if (!JS_IsException(fn) && name) {
        char buf[128];
        size_t n = (len == NAPI_AUTO_LENGTH) ? strlen(name) : len;
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, name, n); buf[n] = 0;
        JS_DefinePropertyValueStr(env->ctx, fn, "name", JS_NewString(env->ctx, buf),
                                  JS_PROP_CONFIGURABLE);
    }
    return fn;
}

napi_status napi_create_function(napi_env env, const char *name, size_t len,
                                 napi_callback cb, void *data, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, cb); CHECK_ARG(env, result);
    JSValue fn = napi_make_function(env, name, len, cb, data);
    NAPI_TRY(env, fn);
    *result = napi_hold(env, fn);
    return NAPI_OK(env);
}
napi_status napi_call_function(napi_env env, napi_value recv, napi_value func,
                               size_t argc, const napi_value *argv, napi_value *result) {
    CHECK_ENV(env);
    JSValue *args = argc ? malloc(sizeof(JSValue) * argc) : NULL;
    if (argc && !args) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    for (size_t i = 0; i < argc; i++) args[i] = napi_val(argv[i]);
    JSValue out = JS_Call(env->ctx, napi_val(func), napi_val(recv), (int)argc, (JSValueConst *)args);
    free(args);
    NAPI_TRY(env, out);
    if (result) *result = napi_hold(env, out); else JS_FreeValue(env->ctx, out);
    return NAPI_OK(env);
}
/* The async-context variant of call_function. There is no async_hooks here
   for the context to mean anything to, so it is the plain call. */
napi_status napi_make_callback(napi_env env, napi_async_context ctx_,
                               napi_value recv, napi_value func,
                               size_t argc, const napi_value *argv, napi_value *result) {
    (void)ctx_;
    return napi_call_function(env, recv, func, argc, argv, result);
}
napi_status napi_async_init(napi_env env, napi_value resource,
                            napi_value resource_name, napi_async_context *result) {
    (void)resource; (void)resource_name;
    CHECK_ENV(env); CHECK_ARG(env, result);
    *result = (napi_async_context)env;      /* an opaque token; nothing reads it */
    return NAPI_OK(env);
}
napi_status napi_async_destroy(napi_env env, napi_async_context c) {
    (void)c; CHECK_ENV(env); return NAPI_OK(env);
}

napi_status napi_new_instance(napi_env env, napi_value ctor, size_t argc,
                              const napi_value *argv, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue *args = argc ? malloc(sizeof(JSValue) * argc) : NULL;
    if (argc && !args) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    for (size_t i = 0; i < argc; i++) args[i] = napi_val(argv[i]);
    JSValue out = JS_CallConstructor(env->ctx, napi_val(ctor), (int)argc, (JSValueConst *)args);
    free(args);
    NAPI_TRY(env, out);
    *result = napi_hold(env, out);
    return NAPI_OK(env);
}

napi_status napi_define_properties(napi_env env, napi_value object,
                                   size_t count, const napi_property_descriptor *props) {
    CHECK_ENV(env);
    JSValue obj = napi_val(object);
    for (size_t i = 0; i < count; i++) {
        const napi_property_descriptor *p = &props[i];
        JSAtom key = p->utf8name ? JS_NewAtom(env->ctx, p->utf8name)
                                 : JS_ValueToAtom(env->ctx, napi_val(p->name));
        if (key == JS_ATOM_NULL) return napi_catch(env);
        int flags = 0;
        if (p->attributes & napi_writable)     flags |= JS_PROP_WRITABLE;
        if (p->attributes & napi_enumerable)   flags |= JS_PROP_ENUMERABLE;
        if (p->attributes & napi_configurable) flags |= JS_PROP_CONFIGURABLE;
        int rc;
        if (p->getter || p->setter) {
            JSValue g = p->getter ? napi_make_function(env, "get", NAPI_AUTO_LENGTH, p->getter, p->data)
                                  : JS_UNDEFINED;
            JSValue s = p->setter ? napi_make_function(env, "set", NAPI_AUTO_LENGTH, p->setter, p->data)
                                  : JS_UNDEFINED;
            rc = JS_DefinePropertyGetSet(env->ctx, obj, key, g, s, flags);
        } else {
            JSValue v = p->method
                      ? napi_make_function(env, p->utf8name, NAPI_AUTO_LENGTH, p->method, p->data)
                      : JS_DupValue(env->ctx, napi_val(p->value));
            rc = JS_DefinePropertyValue(env->ctx, obj, key, v, flags);
        }
        JS_FreeAtom(env->ctx, key);
        if (rc < 0) return napi_catch(env);
    }
    return NAPI_OK(env);
}

/* ------------------------------------------------------------ wrap/data */

/* napi_wrap attaches native state to a JS object. QuickJS has no free slot on
   an arbitrary object, so the state is parked on a non-enumerable property
   holding an opaque carrier -- which also gives the finalizer a natural home. */
typedef struct {
    void *data;
    napi_finalize finalize_cb;
    void *hint;
    napi_env env;
} NapiWrap;

static JSClassID napi_wrap_class_id;
static void napi_wrap_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    NapiWrap *w = JS_GetOpaque(val, napi_wrap_class_id);
    if (!w) return;
    if (w->finalize_cb) w->finalize_cb(w->env, w->data, w->hint);
    free(w);
}
static JSClassDef napi_wrap_class = { "NapiWrap", .finalizer = napi_wrap_finalizer };
static const char *NAPI_WRAP_KEY = "__sxn_napi_wrap";

napi_status napi_wrap(napi_env env, napi_value js_object, void *native,
                      napi_finalize finalize_cb, void *hint, napi_ref *result) {
    CHECK_ENV(env);
    NapiWrap *w = malloc(sizeof(*w));
    if (!w) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    w->data = native; w->finalize_cb = finalize_cb; w->hint = hint; w->env = env;
    JSValue holder = JS_NewObjectClass(env->ctx, napi_wrap_class_id);
    if (JS_IsException(holder)) { free(w); return napi_catch(env); }
    JS_SetOpaque(holder, w);
    if (JS_DefinePropertyValueStr(env->ctx, napi_val(js_object), NAPI_WRAP_KEY,
                                  holder, 0) < 0)
        return napi_catch(env);
    if (result) return napi_create_reference(env, js_object, 0, result);
    return NAPI_OK(env);
}
napi_status napi_unwrap(napi_env env, napi_value js_object, void **result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue holder = JS_GetPropertyStr(env->ctx, napi_val(js_object), NAPI_WRAP_KEY);
    NapiWrap *w = JS_GetOpaque(holder, napi_wrap_class_id);
    JS_FreeValue(env->ctx, holder);
    if (!w) return NAPI_FAIL(env, napi_invalid_arg, "object was not wrapped");
    *result = w->data;
    return NAPI_OK(env);
}
napi_status napi_remove_wrap(napi_env env, napi_value js_object, void **result) {
    CHECK_ENV(env);
    JSValue holder = JS_GetPropertyStr(env->ctx, napi_val(js_object), NAPI_WRAP_KEY);
    NapiWrap *w = JS_GetOpaque(holder, napi_wrap_class_id);
    if (w) { if (result) *result = w->data; w->finalize_cb = NULL; }
    JS_FreeValue(env->ctx, holder);
    JSAtom a = JS_NewAtom(env->ctx, NAPI_WRAP_KEY);
    JS_DeleteProperty(env->ctx, napi_val(js_object), a, 0);
    JS_FreeAtom(env->ctx, a);
    return NAPI_OK(env);
}
napi_status napi_create_external(napi_env env, void *data, napi_finalize finalize_cb,
                                 void *hint, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    NapiWrap *w = malloc(sizeof(*w));
    if (!w) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    w->data = data; w->finalize_cb = finalize_cb; w->hint = hint; w->env = env;
    JSValue holder = JS_NewObjectClass(env->ctx, napi_wrap_class_id);
    if (JS_IsException(holder)) { free(w); return napi_catch(env); }
    JS_SetOpaque(holder, w);
    *result = napi_hold(env, holder);
    return NAPI_OK(env);
}
napi_status napi_get_value_external(napi_env env, napi_value value, void **result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    NapiWrap *w = JS_GetOpaque(napi_val(value), napi_wrap_class_id);
    if (!w) return NAPI_FAIL(env, napi_invalid_arg, "not an external");
    *result = w->data;
    return NAPI_OK(env);
}
napi_status napi_add_finalizer(napi_env env, napi_value js_object, void *native,
                               napi_finalize finalize_cb, void *hint, napi_ref *result) {
    return napi_wrap(env, js_object, native, finalize_cb, hint, result);
}
napi_status napi_set_instance_data(napi_env env, void *data,
                                   napi_finalize finalize_cb, void *hint) {
    (void)finalize_cb; (void)hint;
    CHECK_ENV(env);
    /* Nothing else in this runtime uses the context opaque, and there is one
       addon per process here, so it is the natural single slot. */
    JS_SetContextOpaque(env->ctx, data);
    return NAPI_OK(env);
}
napi_status napi_get_instance_data(napi_env env, void **data) {
    CHECK_ENV(env); CHECK_ARG(env, data);
    *data = JS_GetContextOpaque(env->ctx);
    return NAPI_OK(env);
}

/* --------------------------------------------------------- array buffers */

napi_status napi_create_arraybuffer(napi_env env, size_t len, void **data, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue ab = JS_NewArrayBuffer(env->ctx, NULL, len, NULL, NULL, true);
    NAPI_TRY(env, ab);
    if (data) { size_t n; *data = JS_GetArrayBuffer(env->ctx, &n, ab); }
    *result = napi_hold(env, ab);
    return NAPI_OK(env);
}
napi_status napi_get_arraybuffer_info(napi_env env, napi_value ab, void **data, size_t *len) {
    CHECK_ENV(env);
    size_t n = 0;
    uint8_t *p = JS_GetArrayBuffer(env->ctx, &n, napi_val(ab));
    if (!p) return napi_catch(env);
    if (data) *data = p;
    if (len) *len = n;
    return NAPI_OK(env);
}
napi_status napi_is_arraybuffer(napi_env env, napi_value v, bool *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    size_t n;
    *result = JS_GetArrayBuffer(env->ctx, &n, napi_val(v)) != NULL;
    if (!*result) JS_FreeValue(env->ctx, JS_GetException(env->ctx));
    return NAPI_OK(env);
}
napi_status napi_is_typedarray(napi_env env, napi_value v, bool *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    size_t off, len, elem;
    JSValue ab = JS_GetTypedArrayBuffer(env->ctx, napi_val(v), &off, &len, &elem);
    *result = !JS_IsException(ab);
    if (*result) JS_FreeValue(env->ctx, ab);
    else JS_FreeValue(env->ctx, JS_GetException(env->ctx));
    return NAPI_OK(env);
}
napi_status napi_get_typedarray_info(napi_env env, napi_value ta, napi_typedarray_type *type,
                                     size_t *length, void **data,
                                     napi_value *arraybuffer, size_t *byte_offset) {
    CHECK_ENV(env);
    size_t off = 0, len = 0, elem = 1;
    JSValue ab = JS_GetTypedArrayBuffer(env->ctx, napi_val(ta), &off, &len, &elem);
    NAPI_TRY(env, ab);
    size_t total = 0;
    uint8_t *base = JS_GetArrayBuffer(env->ctx, &total, ab);
    if (type) *type = napi_uint8_array;      /* the element type addons assume for bytes */
    if (length) *length = elem ? len / elem : len;
    if (data) *data = base ? base + off : NULL;
    if (byte_offset) *byte_offset = off;
    if (arraybuffer) *arraybuffer = napi_hold(env, ab); else JS_FreeValue(env->ctx, ab);
    return NAPI_OK(env);
}

/* node_api adds Buffer, which here is the Buffer the node: layer installed. */
static JSValue napi_new_buffer(napi_env env, size_t len, void **data) {
    JSValue global = JS_GetGlobalObject(env->ctx);
    JSValue buffer = JS_GetPropertyStr(env->ctx, global, "Buffer");
    JS_FreeValue(env->ctx, global);
    JSValue alloc = JS_GetPropertyStr(env->ctx, buffer, "alloc");
    JSValue n = JS_NewInt64(env->ctx, (int64_t)len);
    JSValue buf = JS_Call(env->ctx, alloc, buffer, 1, (JSValueConst *)&n);
    JS_FreeValue(env->ctx, n);
    JS_FreeValue(env->ctx, alloc);
    JS_FreeValue(env->ctx, buffer);
    if (!JS_IsException(buf) && data) {
        size_t off, blen, elem, total;
        JSValue ab = JS_GetTypedArrayBuffer(env->ctx, buf, &off, &blen, &elem);
        if (!JS_IsException(ab)) {
            uint8_t *base = JS_GetArrayBuffer(env->ctx, &total, ab);
            *data = base ? base + off : NULL;
            JS_FreeValue(env->ctx, ab);
        }
    }
    return buf;
}
napi_status napi_create_buffer(napi_env env, size_t len, void **data, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue b = napi_new_buffer(env, len, data);
    NAPI_TRY(env, b);
    *result = napi_hold(env, b);
    return NAPI_OK(env);
}
napi_status napi_create_buffer_copy(napi_env env, size_t len, const void *src,
                                    void **data, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    void *dst = NULL;
    JSValue b = napi_new_buffer(env, len, &dst);
    NAPI_TRY(env, b);
    if (dst && src) memcpy(dst, src, len);
    if (data) *data = dst;
    *result = napi_hold(env, b);
    return NAPI_OK(env);
}
napi_status napi_is_buffer(napi_env env, napi_value v, bool *result) {
    return napi_is_typedarray(env, v, result);
}
napi_status napi_get_buffer_info(napi_env env, napi_value v, void **data, size_t *len) {
    return napi_get_typedarray_info(env, v, NULL, len, data, NULL, NULL);
}

napi_status napi_create_typedarray(napi_env env, napi_typedarray_type type, size_t length,
                                   napi_value arraybuffer, size_t byte_offset, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    static const char *names[] = {
        "Int8Array","Uint8Array","Uint8ClampedArray","Int16Array","Uint16Array",
        "Int32Array","Uint32Array","Float32Array","Float64Array","BigInt64Array","BigUint64Array",
    };
    if ((size_t)type >= sizeof(names)/sizeof(names[0]))
        return NAPI_FAIL(env, napi_invalid_arg, "unknown typed-array type");
    JSValue global = JS_GetGlobalObject(env->ctx);
    JSValue ctor = JS_GetPropertyStr(env->ctx, global, names[type]);
    JS_FreeValue(env->ctx, global);
    JSValue args[3] = { JS_DupValue(env->ctx, napi_val(arraybuffer)),
                        JS_NewInt64(env->ctx, (int64_t)byte_offset),
                        JS_NewInt64(env->ctx, (int64_t)length) };
    JSValue out = JS_CallConstructor(env->ctx, ctor, 3, (JSValueConst *)args);
    for (int i = 0; i < 3; i++) JS_FreeValue(env->ctx, args[i]);
    JS_FreeValue(env->ctx, ctor);
    NAPI_TRY(env, out);
    *result = napi_hold(env, out);
    return NAPI_OK(env);
}

/* A Buffer over memory the addon owns. QuickJS can adopt a pointer with a
   free callback, which is exactly the contract: the finalizer runs when the
   last view goes away. */
typedef struct { napi_env env; napi_finalize cb; void *hint; } NapiExtBuf;
static void napi_ext_buf_free(JSRuntime *rt, void *opaque, void *ptr) {
    (void)rt;
    NapiExtBuf *e = opaque;
    if (e) { if (e->cb) e->cb(e->env, ptr, e->hint); free(e); }
}
napi_status napi_create_external_arraybuffer(napi_env env, void *data, size_t len,
                                             napi_finalize finalize_cb, void *hint,
                                             napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    NapiExtBuf *e = malloc(sizeof(*e));
    if (!e) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    e->env = env; e->cb = finalize_cb; e->hint = hint;
    JSValue ab = JS_NewArrayBuffer(env->ctx, data, len, napi_ext_buf_free, e, false);
    if (JS_IsException(ab)) { free(e); return napi_catch(env); }
    *result = napi_hold(env, ab);
    return NAPI_OK(env);
}
napi_status napi_create_external_buffer(napi_env env, size_t len, void *data,
                                        napi_finalize finalize_cb, void *hint,
                                        napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    napi_value ab;
    napi_status st = napi_create_external_arraybuffer(env, data, len, finalize_cb, hint, &ab);
    if (st != napi_ok) return st;
    /* Node hands back a Buffer, not a bare ArrayBuffer, and addons index it. */
    JSValue global = JS_GetGlobalObject(env->ctx);
    JSValue B = JS_GetPropertyStr(env->ctx, global, "Buffer");
    JS_FreeValue(env->ctx, global);
    JSValue arg = JS_DupValue(env->ctx, napi_val(ab));
    JSValue buf = JS_Invoke(env->ctx, B, JS_NewAtom(env->ctx, "from"), 1, (JSValueConst *)&arg);
    JS_FreeValue(env->ctx, arg);
    JS_FreeValue(env->ctx, B);
    NAPI_TRY(env, buf);
    *result = napi_hold(env, buf);
    return NAPI_OK(env);
}

/* napi_define_class: a constructor function with methods on its prototype and
   statics on itself, which is what a JS class is once the sugar is gone. */
/* A class constructor cannot go through JS_NewCFunctionData: that shape never
   learns it was called with `new`, so napi_get_new_target always answered
   "no" and every addon that guards its constructor threw on `new Foo()`.
   JS_CFUNC_constructor_or_func_magic is the shape that does get told -- it
   hands the constructor its new_target, and undefined for a plain call -- but
   it carries only an int, so the callback is looked up by index. Classes are
   registered once at module init, so the table is tiny and never freed. */
static NapiFunc **napi_classes;
static int napi_class_count;

static JSValue napi_ctor_bridge(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv, int magic) {
    napi_env env = sxn_env;
    if (magic < 0 || magic >= napi_class_count) return JS_ThrowTypeError(ctx, "unknown class");
    NapiFunc *f = napi_classes[magic];

    /* N-API hands the constructor a `this` that already exists and wears the
       class's prototype; QuickJS expects the C function to make it. */
    JSValue self = JS_UNDEFINED;
    if (!JS_IsUndefined(new_target)) {
        JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(proto)) return proto;
        self = JS_NewObjectProto(ctx, proto);
        JS_FreeValue(ctx, proto);
        if (JS_IsException(self)) return self;
    }

    struct napi_callback_info__ info = { self, argc, argv, f->data,
                                         JS_DupValue(ctx, new_target) };
    NapiScope scope;
    napi_push_scope(env, &scope);
    napi_value out = f->cb(env, &info);
    JSValue result = out ? JS_DupValue(ctx, napi_val(out)) : JS_DupValue(ctx, self);
    napi_pop_scope(env);
    JS_FreeValue(ctx, info.new_target);
    JS_FreeValue(ctx, self);

    if (!JS_IsUninitialized(env->pending)) {
        JSValue e = env->pending;
        env->pending = JS_UNINITIALIZED;
        JS_FreeValue(ctx, result);
        return JS_Throw(ctx, e);
    }
    return result;
}

napi_status napi_define_class(napi_env env, const char *name, size_t name_len,
                              napi_callback constructor, void *data,
                              size_t count, const napi_property_descriptor *props,
                              napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, constructor); CHECK_ARG(env, result);
    NapiFunc *f = malloc(sizeof(*f));
    if (!f) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    f->cb = constructor; f->data = data; f->this_ref = JS_UNDEFINED;
    NapiFunc **grown = realloc(napi_classes, sizeof(*grown) * (size_t)(napi_class_count + 1));
    if (!grown) { free(f); return NAPI_FAIL(env, napi_generic_failure, "out of memory"); }
    napi_classes = grown;
    napi_classes[napi_class_count] = f;
    int magic = napi_class_count++;

    char buf[128];
    size_t n = (name_len == NAPI_AUTO_LENGTH) ? (name ? strlen(name) : 0) : name_len;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    if (name) memcpy(buf, name, n);
    buf[n] = 0;

    JSValue ctor = JS_NewCFunction2(env->ctx, (JSCFunction *)napi_ctor_bridge, buf, 0,
                                    JS_CFUNC_constructor_or_func_magic, magic);
    NAPI_TRY(env, ctor);
    JSValue proto = JS_NewObject(env->ctx);
    JS_SetConstructor(env->ctx, ctor, proto);

    napi_value nctor = napi_hold(env, ctor);
    napi_value nproto = napi_hold(env, JS_DupValue(env->ctx, proto));
    JS_FreeValue(env->ctx, proto);
    for (size_t i = 0; i < count; i++) {
        /* A static member goes on the constructor, everything else on the
           prototype -- the one place this differs from define_properties. */
        napi_value target = (props[i].attributes & napi_static) ? nctor : nproto;
        napi_status st = napi_define_properties(env, target, 1, &props[i]);
        if (st != napi_ok) return st;
    }
    *result = nctor;
    return NAPI_OK(env);
}

/* ------------------------------------------------------------- promises */

typedef struct { JSValue resolve, reject, promise; } NapiDeferred;

napi_status napi_create_promise(napi_env env, napi_deferred *deferred, napi_value *promise) {
    CHECK_ENV(env); CHECK_ARG(env, deferred); CHECK_ARG(env, promise);
    JSValue funcs[2];
    JSValue p = JS_NewPromiseCapability(env->ctx, funcs);
    NAPI_TRY(env, p);
    NapiDeferred *d = malloc(sizeof(*d));
    if (!d) { JS_FreeValue(env->ctx, p); return NAPI_FAIL(env, napi_generic_failure, "out of memory"); }
    d->resolve = funcs[0]; d->reject = funcs[1]; d->promise = JS_DupValue(env->ctx, p);
    *deferred = (napi_deferred)d;
    *promise = napi_hold(env, p);
    return NAPI_OK(env);
}
static napi_status napi_settle(napi_env env, napi_deferred deferred, napi_value v, bool ok) {
    CHECK_ENV(env); CHECK_ARG(env, deferred);
    NapiDeferred *d = (NapiDeferred *)deferred;
    JSValue arg = JS_DupValue(env->ctx, napi_val(v));
    JSValue r = JS_Call(env->ctx, ok ? d->resolve : d->reject, JS_UNDEFINED, 1, (JSValueConst *)&arg);
    JS_FreeValue(env->ctx, arg);
    JS_FreeValue(env->ctx, r);
    JS_FreeValue(env->ctx, d->resolve);
    JS_FreeValue(env->ctx, d->reject);
    JS_FreeValue(env->ctx, d->promise);
    free(d);
    return NAPI_OK(env);
}
napi_status napi_resolve_deferred(napi_env env, napi_deferred d, napi_value v) {
    return napi_settle(env, d, v, true);
}
napi_status napi_reject_deferred(napi_env env, napi_deferred d, napi_value v) {
    return napi_settle(env, d, v, false);
}
napi_status napi_is_promise(napi_env env, napi_value v, bool *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    JSValue then = JS_GetPropertyStr(env->ctx, napi_val(v), "then");
    *result = JS_IsFunction(env->ctx, then);
    JS_FreeValue(env->ctx, then);
    return NAPI_OK(env);
}

/* ----------------------------------------------------------- async work */

/* Backed by libuv's thread pool, which this runtime already drives. The
   execute callback runs off-thread and must not touch JS; the complete
   callback runs back on the loop thread, which is where JS lives. */
typedef struct {
    uv_work_t req;
    napi_env env;
    napi_async_execute_callback execute;
    napi_async_complete_callback complete;
    void *data;
    bool cancelled;
} NapiWork;

static void napi_work_run(uv_work_t *req) {
    NapiWork *w = (NapiWork *)req->data;
    if (w->execute) w->execute(w->env, w->data);
}
static void napi_work_done(uv_work_t *req, int status) {
    NapiWork *w = (NapiWork *)req->data;
    if (w->complete) {
        NapiScope scope;
        napi_push_scope(w->env, &scope);
        w->complete(w->env, status == UV_ECANCELED ? napi_cancelled : napi_ok, w->data);
        napi_pop_scope(w->env);
    }
    free(w);
}
napi_status napi_create_async_work(napi_env env, napi_value async_resource,
                                   napi_value async_resource_name,
                                   napi_async_execute_callback execute,
                                   napi_async_complete_callback complete,
                                   void *data, napi_async_work *result) {
    (void)async_resource; (void)async_resource_name;
    CHECK_ENV(env); CHECK_ARG(env, execute); CHECK_ARG(env, result);
    NapiWork *w = calloc(1, sizeof(*w));
    if (!w) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    w->env = env; w->execute = execute; w->complete = complete; w->data = data;
    w->req.data = w;
    *result = (napi_async_work)w;
    return NAPI_OK(env);
}
napi_status napi_queue_async_work(node_api_basic_env env, napi_async_work work) {
    CHECK_ENV(env); CHECK_ARG((napi_env)env, work);
    NapiWork *w = (NapiWork *)work;
    if (uv_queue_work(((napi_env)env)->loop, &w->req, napi_work_run, napi_work_done) != 0)
        return NAPI_FAIL((napi_env)env, napi_generic_failure, "cannot queue async work");
    return NAPI_OK((napi_env)env);
}
napi_status napi_delete_async_work(napi_env env, napi_async_work work) {
    CHECK_ENV(env);
    /* uv frees the request in its own completion callback; deleting a queued
       one here would free it out from under the loop. Only an unqueued work
       item is ours to release, and an addon that queued it never calls this
       before completion. */
    (void)work;
    return NAPI_OK(env);
}
napi_status napi_cancel_async_work(node_api_basic_env env, napi_async_work work) {
    CHECK_ENV(env); CHECK_ARG((napi_env)env, work);
    NapiWork *w = (NapiWork *)work;
    return uv_cancel((uv_req_t *)&w->req) == 0 ? NAPI_OK((napi_env)env)
         : NAPI_FAIL((napi_env)env, napi_generic_failure, "work already running");
}

/* Thread-safe functions: an addon's own threads calling into JS. QuickJS is
   single-threaded, so the call cannot happen where it is made. Each tsfn owns
   a queue and a uv_async_t; a worker thread appends and wakes the loop, and
   the loop thread -- the only one allowed to touch the context -- drains it.
   uv_async_send is the one libuv call that is safe from another thread, which
   is what makes this shape the obvious one. */
typedef struct NapiTsfnItem { struct NapiTsfnItem *next; void *data; } NapiTsfnItem;

typedef struct {
    napi_env env;
    uv_async_t async;
    uv_mutex_t lock;
    uv_cond_t room;                 /* blocking mode waits here when full */
    NapiTsfnItem *head, *tail;
    size_t queued, max_queue;
    size_t thread_count;
    bool aborted, closing;
    JSValue func;
    void *context;
    napi_threadsafe_function_call_js call_js;
    napi_finalize finalize_cb;
    void *finalize_data;
} NapiTsfn;

static void napi_tsfn_destroy(uv_handle_t *h) {
    NapiTsfn *t = h->data;
    if (t->finalize_cb) t->finalize_cb(t->env, t->finalize_data, t->context);
    JS_FreeValue(t->env->ctx, t->func);
    uv_mutex_destroy(&t->lock);
    uv_cond_destroy(&t->room);
    free(t);
}

static void napi_tsfn_drain(uv_async_t *async) {
    NapiTsfn *t = async->data;
    for (;;) {
        uv_mutex_lock(&t->lock);
        NapiTsfnItem *it = t->head;
        if (it) { t->head = it->next; if (!t->head) t->tail = NULL; t->queued--; }
        bool done = t->closing && !t->head;
        uv_mutex_unlock(&t->lock);
        if (it) uv_cond_signal(&t->room);
        if (!it) { if (done) uv_close((uv_handle_t *)&t->async, napi_tsfn_destroy); return; }

        NapiScope scope;
        napi_push_scope(t->env, &scope);
        if (t->call_js) {
            napi_value fn = JS_IsUndefined(t->func) ? NULL
                          : napi_hold(t->env, JS_DupValue(t->env->ctx, t->func));
            t->call_js(t->env, fn, t->context, it->data);
        } else if (!JS_IsUndefined(t->func)) {
            JSValue r = JS_Call(t->env->ctx, t->func, JS_UNDEFINED, 0, NULL);
            JS_FreeValue(t->env->ctx, r);
        }
        /* An exception here has no caller to propagate to -- the JS stack that
           would have caught it is on another thread and long gone. */
        if (!JS_IsUninitialized(t->env->pending)) {
            napi_fatal_exception(t->env, napi_hold(t->env, t->env->pending));
            t->env->pending = JS_UNINITIALIZED;
        }
        napi_pop_scope(t->env);
        free(it);
    }
}

napi_status napi_create_threadsafe_function(
    napi_env env, napi_value func, napi_value async_resource,
    napi_value async_resource_name, size_t max_queue_size, size_t initial_thread_count,
    void *thread_finalize_data, napi_finalize thread_finalize_cb, void *context,
    napi_threadsafe_function_call_js call_js_cb, napi_threadsafe_function *result) {
    (void)async_resource; (void)async_resource_name;
    CHECK_ENV(env); CHECK_ARG(env, result);
    if (initial_thread_count == 0)
        return NAPI_FAIL(env, napi_invalid_arg, "initial_thread_count must be at least 1");
    NapiTsfn *t = calloc(1, sizeof(*t));
    if (!t) return NAPI_FAIL(env, napi_generic_failure, "out of memory");
    t->env = env;
    t->func = func ? JS_DupValue(env->ctx, napi_val(func)) : JS_UNDEFINED;
    t->context = context;
    t->call_js = call_js_cb;
    t->finalize_cb = thread_finalize_cb;
    t->finalize_data = thread_finalize_data;
    t->max_queue = max_queue_size;
    t->thread_count = initial_thread_count;
    if (uv_mutex_init(&t->lock) != 0 || uv_cond_init(&t->room) != 0 ||
        uv_async_init(env->loop, &t->async, napi_tsfn_drain) != 0) {
        JS_FreeValue(env->ctx, t->func);
        free(t);
        return NAPI_FAIL(env, napi_generic_failure, "cannot create a thread-safe function");
    }
    t->async.data = t;
    /* A tsfn that has never been called should not hold the process open;
       napi_ref_threadsafe_function is how an addon asks for the opposite. */
    uv_unref((uv_handle_t *)&t->async);
    *result = (napi_threadsafe_function)t;
    return NAPI_OK(env);
}

napi_status napi_call_threadsafe_function(napi_threadsafe_function f, void *data,
                                          napi_threadsafe_function_call_mode mode) {
    NapiTsfn *t = (NapiTsfn *)f;
    if (!t) return napi_invalid_arg;
    uv_mutex_lock(&t->lock);
    if (t->aborted || t->closing) { uv_mutex_unlock(&t->lock); return napi_closing; }
    while (t->max_queue && t->queued >= t->max_queue) {
        if (mode == napi_tsfn_nonblocking) { uv_mutex_unlock(&t->lock); return napi_queue_full; }
        uv_cond_wait(&t->room, &t->lock);
        if (t->aborted || t->closing) { uv_mutex_unlock(&t->lock); return napi_closing; }
    }
    NapiTsfnItem *it = malloc(sizeof(*it));
    if (!it) { uv_mutex_unlock(&t->lock); return napi_generic_failure; }
    it->data = data; it->next = NULL;
    if (t->tail) t->tail->next = it; else t->head = it;
    t->tail = it;
    t->queued++;
    uv_mutex_unlock(&t->lock);
    uv_async_send(&t->async);
    return napi_ok;
}

napi_status napi_acquire_threadsafe_function(napi_threadsafe_function f) {
    NapiTsfn *t = (NapiTsfn *)f;
    if (!t) return napi_invalid_arg;
    uv_mutex_lock(&t->lock);
    napi_status st = (t->aborted || t->closing) ? napi_closing : napi_ok;
    if (st == napi_ok) t->thread_count++;
    uv_mutex_unlock(&t->lock);
    return st;
}

napi_status napi_release_threadsafe_function(napi_threadsafe_function f,
                                             napi_threadsafe_function_release_mode m) {
    NapiTsfn *t = (NapiTsfn *)f;
    if (!t) return napi_invalid_arg;
    uv_mutex_lock(&t->lock);
    if (m == napi_tsfn_abort) t->aborted = true;
    if (t->thread_count) t->thread_count--;
    bool finished = t->thread_count == 0;
    if (finished) t->closing = true;
    uv_mutex_unlock(&t->lock);
    uv_cond_broadcast(&t->room);
    /* The drain runs on the loop thread and is what closes the handle, so the
       last release only has to wake it. */
    if (finished) uv_async_send(&t->async);
    return napi_ok;
}

napi_status napi_ref_threadsafe_function(node_api_basic_env env, napi_threadsafe_function f) {
    (void)env;
    NapiTsfn *t = (NapiTsfn *)f;
    if (!t) return napi_invalid_arg;
    uv_ref((uv_handle_t *)&t->async);
    return napi_ok;
}
napi_status napi_unref_threadsafe_function(node_api_basic_env env, napi_threadsafe_function f) {
    (void)env;
    NapiTsfn *t = (NapiTsfn *)f;
    if (!t) return napi_invalid_arg;
    uv_unref((uv_handle_t *)&t->async);
    return napi_ok;
}
napi_status napi_get_threadsafe_function_context(napi_threadsafe_function f, void **c) {
    NapiTsfn *t = (NapiTsfn *)f;
    if (!t || !c) return napi_invalid_arg;
    *c = t->context;
    return napi_ok;
}

/* ---------------------------------------------------------- environment */

typedef struct NapiCleanup {
    struct NapiCleanup *next;
    void (*fn)(void *);
    void *arg;
} NapiCleanup;
static NapiCleanup *napi_cleanups;

napi_status napi_add_env_cleanup_hook(node_api_basic_env env, void (*fn)(void *), void *arg) {
    CHECK_ENV(env);
    NapiCleanup *c = malloc(sizeof(*c));
    if (!c) return napi_generic_failure;
    c->fn = fn; c->arg = arg; c->next = napi_cleanups; napi_cleanups = c;
    return napi_ok;
}
napi_status napi_remove_env_cleanup_hook(node_api_basic_env env, void (*fn)(void *), void *arg) {
    CHECK_ENV(env);
    for (NapiCleanup **p = &napi_cleanups; *p; p = &(*p)->next)
        if ((*p)->fn == fn && (*p)->arg == arg) { NapiCleanup *d = *p; *p = d->next; free(d); break; }
    return napi_ok;
}
napi_status napi_get_uv_event_loop(node_api_basic_env env, struct uv_loop_s **loop) {
    CHECK_ENV(env); CHECK_ARG((napi_env)env, loop);
    *loop = ((napi_env)env)->loop;
    return napi_ok;
}
napi_status napi_get_node_version(node_api_basic_env env, const napi_node_version **version) {
    CHECK_ENV(env); CHECK_ARG((napi_env)env, version);
    /* Addons gate features on this. Reporting a real LTS keeps them on paths
       that exist here rather than on experimental ones that do not. */
    static const napi_node_version v = { 20, 0, 0, "sxn" };
    *version = &v;
    return napi_ok;
}
napi_status napi_get_version(node_api_basic_env env, uint32_t *result) {
    CHECK_ENV(env); CHECK_ARG((napi_env)env, result);
    *result = 8;
    return napi_ok;
}
napi_status napi_adjust_external_memory(node_api_basic_env env, int64_t change, int64_t *adjusted) {
    CHECK_ENV(env);
    static int64_t total;
    total += change;
    if (adjusted) *adjusted = total;
    return napi_ok;
}
napi_status napi_run_script(napi_env env, napi_value script, napi_value *result) {
    CHECK_ENV(env); CHECK_ARG(env, result);
    const char *src = JS_ToCString(env->ctx, napi_val(script));
    if (!src) return napi_catch(env);
    JSValue out = JS_Eval(env->ctx, src, strlen(src), "<napi>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(env->ctx, src);
    NAPI_TRY(env, out);
    *result = napi_hold(env, out);
    return NAPI_OK(env);
}
napi_status napi_object_freeze(napi_env env, napi_value object) {
    CHECK_ENV(env);
    JSValue global = JS_GetGlobalObject(env->ctx);
    JSValue O = JS_GetPropertyStr(env->ctx, global, "Object");
    JS_FreeValue(env->ctx, global);
    JSValue arg = JS_DupValue(env->ctx, napi_val(object));
    JSValue r = JS_Invoke(env->ctx, O, JS_NewAtom(env->ctx, "freeze"), 1, (JSValueConst *)&arg);
    JS_FreeValue(env->ctx, arg);
    JS_FreeValue(env->ctx, O);
    JS_FreeValue(env->ctx, r);
    return NAPI_OK(env);
}

/* -------------------------------------------------------------- loading */

/* process.dlopen(module, filename): what require() calls for a .node file.
   The addon's undefined napi_* symbols are resolved from this executable,
   which is why the link exports its dynamic symbols. */
static JSValue sxn_process_dlopen(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "dlopen(module, filename)");
    const char *path = JS_ToCString(ctx, argv[1]);
    if (!path) return JS_EXCEPTION;

    dlerror();
    /* RTLD_NOW, not lazy: an addon that wants a napi_* function this runtime
       does not implement should fail to load with its name, rather than
       segfault the first time that path is taken. */
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *why = dlerror();
        JSValue e = JS_ThrowReferenceError(ctx, "cannot load addon '%s': %s",
                                           path, why ? why : "unknown error");
        JS_FreeCString(ctx, path);
        return e;
    }

    napi_addon_register_func init =
        (napi_addon_register_func)dlsym(handle, "napi_register_module_v1");
    if (!init) {
        JSValue e = JS_ThrowTypeError(ctx,
            "'%s' is not a Node-API addon: it exports no napi_register_module_v1. "
            "Addons built against the older V8 NODE_MODULE interface cannot be loaded.", path);
        JS_FreeCString(ctx, path);
        return e;
    }
    JS_FreeCString(ctx, path);

    napi_env env = sxn_env;
    NapiScope scope;
    napi_push_scope(env, &scope);
    JSValue exports = JS_GetPropertyStr(ctx, argv[0], "exports");
    if (JS_IsUndefined(exports) || JS_IsException(exports)) {
        JS_FreeValue(ctx, exports);
        exports = JS_NewObject(ctx);
    }
    napi_value nexports = napi_hold(env, JS_DupValue(ctx, exports));
    napi_value produced = init(env, nexports);
    JSValue out = produced ? JS_DupValue(ctx, napi_val(produced)) : JS_DupValue(ctx, exports);
    JS_FreeValue(ctx, exports);
    napi_pop_scope(env);

    if (!JS_IsUninitialized(env->pending)) {
        JSValue e = env->pending;
        env->pending = JS_UNINITIALIZED;
        JS_FreeValue(ctx, out);
        return JS_Throw(ctx, e);
    }
    JS_SetPropertyStr(ctx, argv[0], "exports", out);
    return JS_UNDEFINED;
}

/* Node drops an addon's references with the isolate; here the runtime is torn
   down while they still hold objects, and the debug build asserts on that. So
   release what the addon left behind before the context goes. */
void sxn_shutdown_napi(JSContext *ctx) {
    if (!sxn_env || sxn_env->ctx != ctx) return;
    for (NapiCleanup *c = napi_cleanups; c; ) {
        NapiCleanup *next = c->next;
        if (c->fn) c->fn(c->arg);
        free(c); c = next;
    }
    napi_cleanups = NULL;
    for (NapiRef *r = napi_refs; r; ) {
        NapiRef *next = r->next;
        JS_FreeValue(ctx, r->value);
        free(r); r = next;
    }
    napi_refs = NULL;
    while (sxn_env->scope) napi_pop_scope(sxn_env);
#ifndef NDEBUG
    for (NapiBlock *b = napi_dead_blocks; b; ) { NapiBlock *n = b->next; free(b); b = n; }
    napi_dead_blocks = NULL;
#endif
    if (!JS_IsUninitialized(sxn_env->pending)) JS_FreeValue(ctx, sxn_env->pending);
    free(sxn_env);
    sxn_env = NULL;
}

/* Installed by the node: layer, never by the runtime's own surface. */
void sxn_install_napi(JSContext *ctx, uv_loop_t *loop) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &napi_func_class_id);
    JS_NewClass(rt, napi_func_class_id, &napi_func_class);
    JS_NewClassID(rt, &napi_wrap_class_id);
    JS_NewClass(rt, napi_wrap_class_id, &napi_wrap_class);

    sxn_env = calloc(1, sizeof(*sxn_env));
    if (!sxn_env) return;
    sxn_env->ctx = ctx;
    sxn_env->pending = JS_UNINITIALIZED;
    sxn_env->loop = loop;

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__sxnDlopen",
                      JS_NewCFunction(ctx, sxn_process_dlopen, "dlopen", 2));
    JS_FreeValue(ctx, global);
}
