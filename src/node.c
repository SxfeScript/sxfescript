#include <quickjs.h>
#include <quickjs-libc.h>
#include <uv.h>
#include "sxfe.h"
#include "sxn_node_compat.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

#define countof(x) (sizeof(x) / sizeof((x)[0]))

/* --- native primitives consumed only by node_compat.js ------------------
   Pure spec/behavior logic (Buffer, path, EventEmitter, the process object
   shape) lives in node_compat.js; this file supplies the handful of things
   that genuinely need C: real cwd/env access, exit, and safe signal
   delivery. Same split as bootstrap.js / network.c (Task 2). */

static JSValue js_sxn_cwd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return JS_ThrowInternalError(ctx, "getcwd failed: %s", strerror(errno));
    return JS_NewString(ctx, buf);
}

/* --- process.env: native exotic object, phase 1 of replacing node_compat.js
   with native C (see spec discussion) -- was a Proxy over __sxnEnvGet/Set/
   Delete/Keys in JS; a Proxy traps every access through two JS call frames,
   which is pure overhead for something that's just getenv/setenv/unsetenv/
   environ underneath. This exotic-property object gets there in one C call
   per access instead. */
static JSClassID sxn_env_class_id;

static int sxn_env_get_own_property(JSContext *ctx, JSPropertyDescriptor *desc,
                                     JSValueConst obj, JSAtom prop) {
    (void)obj;
    const char *name = JS_AtomToCString(ctx, prop);
    if (!name) return -1;
    const char *value = getenv(name);
    JS_FreeCString(ctx, name);
    if (!value) return false;
    if (desc) {
        desc->flags = JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE;
        desc->value = JS_NewString(ctx, value);
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return true;
}

static int sxn_env_get_own_property_names(JSContext *ctx, JSPropertyEnum **ptab,
                                           uint32_t *plen, JSValueConst obj) {
    (void)obj;
    uint32_t count = 0;
    for (char **e = environ; e && *e; e++) count++;
    JSPropertyEnum *tab = count ? js_malloc(ctx, sizeof(JSPropertyEnum) * count) : NULL;
    if (count && !tab) return -1;
    uint32_t idx = 0;
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        size_t len = eq ? (size_t)(eq - *e) : strlen(*e);
        tab[idx].is_enumerable = true;
        tab[idx].atom = JS_NewAtomLen(ctx, *e, len);
        idx++;
    }
    *ptab = tab;
    *plen = count;
    return 0;
}

static int sxn_env_delete_property(JSContext *ctx, JSValueConst obj, JSAtom prop) {
    (void)obj;
    const char *name = JS_AtomToCString(ctx, prop);
    if (!name) return -1;
    unsetenv(name);
    JS_FreeCString(ctx, name);
    return true;
}

static int sxn_env_define_own_property(JSContext *ctx, JSValueConst this_obj, JSAtom prop,
                                        JSValueConst val, JSValueConst getter, JSValueConst setter,
                                        int flags) {
    (void)this_obj; (void)getter; (void)setter; (void)flags;
    const char *name = JS_AtomToCString(ctx, prop);
    if (!name) return -1;
    const char *value = JS_ToCString(ctx, val);
    if (!value) { JS_FreeCString(ctx, name); return -1; }
    setenv(name, value, 1);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    return true;
}

static JSClassExoticMethods sxn_env_exotic = {
    .get_own_property = sxn_env_get_own_property,
    .get_own_property_names = sxn_env_get_own_property_names,
    .delete_property = sxn_env_delete_property,
    .define_own_property = sxn_env_define_own_property,
};

static JSClassDef sxn_env_class_def = {
    .class_name = "ProcessEnv",
    .exotic = &sxn_env_exotic,
};

/* Returns a fresh exotic env object; called once per context by
   sxn_install_node_compat. Registering the class is idempotent per
   runtime (JS_NewClassID no-ops if *pclass_id is already set), so this is
   safe even though sxn_install_node_compat could in principle run more
   than once against the same runtime. */
static JSValue sxn_new_env_object(JSContext *ctx) {
    JS_NewClassID(JS_GetRuntime(ctx), &sxn_env_class_id);
    JS_NewClass(JS_GetRuntime(ctx), sxn_env_class_id, &sxn_env_class_def);
    return JS_NewObjectClass(ctx, (int)sxn_env_class_id);
}

/* node:fs's sync readFileSync -- the only sync-full-file-read primitive
   missing from what network.c already exposes (Sxn.file(path).text() is
   async, Sxn.write/Sxn.file(path).exists() are sync but don't cover a raw
   byte read). Reuses quickjs-libc's js_load_file rather than reimplementing
   file I/O, matching the split's "thin C wrapper" rule. */
static JSValue js_sxn_read_file_sync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *path = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!path) return JS_ThrowTypeError(ctx, "expected a path");
    size_t length = 0;
    uint8_t *data = js_load_file(ctx, &length, path);
    if (!data) {
        JSValue err = JS_ThrowInternalError(ctx, "ENOENT: no such file or directory, open '%s'", path);
        JS_FreeCString(ctx, path);
        return err;
    }
    JS_FreeCString(ctx, path);
    JSValue out = JS_NewUint8ArrayCopy(ctx, data, length);
    js_free(ctx, data);
    return out;
}

/* node:fs's writeFileSync/existsSync -- phase 2 of replacing node_compat.js
   with native C. These were one-line JS wrappers (writeFileSync discarding
   Sxn.write's byte count to match Node's `undefined` return; existsSync
   forwarding to Sxn.file(path).exists()) with no logic of their own, so they
   become direct native exports instead of paying a JS call frame per call. */
static JSValue js_sxn_write_file_sync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *path = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!path) return JS_ThrowTypeError(ctx, "expected a path");
    size_t length = 0;
    const char *data = argc > 1 ? JS_ToCStringLen(ctx, &length, argv[1]) : NULL;
    if (!data) { JS_FreeCString(ctx, path); return JS_ThrowTypeError(ctx, "expected data"); }
    FILE *file = fopen(path, "wb");
    if (!file) {
        JSValue err = JS_ThrowInternalError(ctx, "cannot write '%s': %s", path, strerror(errno));
        JS_FreeCString(ctx, path); JS_FreeCString(ctx, data);
        return err;
    }
    size_t written = fwrite(data, 1, length, file);
    bool failed = fclose(file) != 0 || written != length;
    JS_FreeCString(ctx, path); JS_FreeCString(ctx, data);
    if (failed) return JS_ThrowInternalError(ctx, "file write failed");
    return JS_UNDEFINED;
}

static JSValue js_sxn_exists_sync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *path = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!path) return JS_ThrowTypeError(ctx, "expected a path");
    struct stat info;
    bool exists = stat(path, &info) == 0;
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, exists);
}

/* --- events: EventEmitter, phase 3 of replacing node_compat.js with native
   C. Listener storage stays the plain `this._events` object/arrays
   node_compat.js already used (Object.create(null) keyed by event type, so
   `once`'s self-removing wrapper -- which still lives in JS, see
   node_compat.js -- and any future introspection keep working unchanged.
   Only the hot dispatch paths (on/off/emit/listenerCount/listeners/
   removeAllListeners) move here, cutting a JS call frame off every emit(). */
/* "_events"/"length" are looked up on every on/off/emit/listenerCount call;
   JS_GetPropertyStr mints and frees a fresh JSAtom (hash + atom-table
   lookup) every time it's called with a C string, so these two keys are
   cached once as real JSAtoms at install time (sxn_install_node_compat)
   instead of re-hashed 500k times in a tight emit() loop. "error" is cached
   for the same reason: emit()'s unhandled-'error' check compares atoms
   instead of strcmp-ing a C string it no longer materializes.

   The event-type argument itself goes through JS_ValueToAtom rather than
   JS_ToCString + JS_GetPropertyStr: a string literal pushed by the bytecode
   (`ee.emit("x", ...)`) IS the interned atom's JSString, so JS_ValueToAtom
   hits the engine's identity fast path -- no UTF-8 conversion, no hashing,
   no atom-table probe -- where the old C-string round trip re-hashed the
   name on every single call. */
static JSAtom sxn_atom_events = JS_ATOM_NULL;
static JSAtom sxn_atom_length = JS_ATOM_NULL;
static JSAtom sxn_atom_error = JS_ATOM_NULL;

static JSValue sxn_ee_events(JSContext *ctx, JSValueConst this_val) {
    return JS_GetProperty(ctx, this_val, sxn_atom_events);
}

static uint32_t sxn_ee_length(JSContext *ctx, JSValueConst list) {
    if (JS_IsUndefined(list)) return 0;
    uint32_t len = 0;
    JSValue len_val = JS_GetProperty(ctx, list, sxn_atom_length);
    JS_ToUint32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);
    return len;
}

static JSValue js_ee_on(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "listener must be a function");
    JSAtom type = JS_ValueToAtom(ctx, argv[0]);
    if (type == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue events = sxn_ee_events(ctx, this_val);
    JSValue list = JS_GetProperty(ctx, events, type);
    if (JS_IsUndefined(list)) {
        list = JS_NewArray(ctx);
        JS_SetProperty(ctx, events, type, JS_DupValue(ctx, list));
    }
    JS_SetPropertyUint32(ctx, list, sxn_ee_length(ctx, list), JS_DupValue(ctx, argv[1]));
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, events);
    JS_FreeAtom(ctx, type);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_ee_off(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "expected an event type");
    JSAtom type = JS_ValueToAtom(ctx, argv[0]);
    if (type == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue events = sxn_ee_events(ctx, this_val);
    JSValue list = JS_GetProperty(ctx, events, type);
    uint32_t len = sxn_ee_length(ctx, list);
    if (!len) {
        JS_FreeValue(ctx, list); JS_FreeValue(ctx, events); JS_FreeAtom(ctx, type);
        return JS_DupValue(ctx, this_val);
    }
    JSValueConst listener = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue out = JS_NewArray(ctx);
    uint32_t out_len = 0;
    for (uint32_t i = 0; i < len; i++) {
        JSValue l = JS_GetPropertyUint32(ctx, list, i);
        JSValue original = JS_GetPropertyStr(ctx, l, "_original");
        bool matches = JS_IsStrictEqual(ctx, l, listener) || (!JS_IsUndefined(original) && JS_IsStrictEqual(ctx, original, listener));
        JS_FreeValue(ctx, original);
        if (matches) JS_FreeValue(ctx, l);
        else JS_SetPropertyUint32(ctx, out, out_len++, l);
    }
    JS_SetProperty(ctx, events, type, out);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, events);
    JS_FreeAtom(ctx, type);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_ee_remove_all_listeners(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1 || JS_IsUndefined(argv[0])) {
        /* Object.create(null), same null-proto shape the constructor uses --
           required so a listener type named e.g. "toString" can never
           shadow-lookup into Object.prototype during on()/off()/emit(). */
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue object_ctor = JS_GetPropertyStr(ctx, global, "Object");
        JSValue create_fn = JS_GetPropertyStr(ctx, object_ctor, "create");
        JSValueConst args[1] = { JS_NULL };
        JSValue events = JS_Call(ctx, create_fn, JS_UNDEFINED, 1, args);
        JS_FreeValue(ctx, create_fn); JS_FreeValue(ctx, object_ctor); JS_FreeValue(ctx, global);
        JS_SetPropertyStr(ctx, this_val, "_events", events);
        return JS_DupValue(ctx, this_val);
    }
    JSAtom type = JS_ValueToAtom(ctx, argv[0]);
    if (type == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue events = sxn_ee_events(ctx, this_val);
    JS_DeleteProperty(ctx, events, type, 0);
    JS_FreeValue(ctx, events);
    JS_FreeAtom(ctx, type);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_ee_emit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "emit requires a type");
    JSAtom type = JS_ValueToAtom(ctx, argv[0]);
    if (type == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue events = sxn_ee_events(ctx, this_val);
    JSValue list = JS_GetProperty(ctx, events, type);
    JS_FreeValue(ctx, events);
    /* Listener arrays are always plain fast arrays (built by JS_NewArray +
       integer appends), so JS_GetFastArray gives direct element access --
       no length property read, no per-index property lookup. The count is
       snapshotted before the loop (Node semantics: listeners added mid-emit
       don't fire this round; off() filters into a *new* array, so removals
       never touch this one). The element pointer, however, is re-fetched
       every iteration: a listener calling on() pushes onto this same live
       array and can realloc its storage. Property-based fallback covers a
       user who replaced _events[type] with something exotic. */
    JSValue *elems;
    uint32_t n;
    bool fast = JS_GetFastArray(ctx, list, &elems, &n);
    if (!fast) n = sxn_ee_length(ctx, list);
    if (!n) {
        JS_FreeValue(ctx, list);
        bool is_error = (type == sxn_atom_error);
        JS_FreeAtom(ctx, type);
        if (!is_error) return JS_NewBool(ctx, false);
        JSValue err;
        if (argc > 1) err = JS_DupValue(ctx, argv[1]);
        else {
            err = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "Unhandled error event"));
        }
        return JS_Throw(ctx, err);
    }
    JS_FreeAtom(ctx, type);
    for (uint32_t i = 0; i < n; i++) {
        JSValue l;
        if (fast) {
            uint32_t cur;
            if (!JS_GetFastArray(ctx, list, &elems, &cur) || i >= cur) break;
            l = JS_DupValue(ctx, elems[i]);
        } else {
            l = JS_GetPropertyUint32(ctx, list, i);
        }
        JSValue ret = JS_Call(ctx, l, this_val, argc - 1, argv + 1);
        JS_FreeValue(ctx, l);
        if (JS_IsException(ret)) { JS_FreeValue(ctx, list); return ret; }
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, list);
    return JS_NewBool(ctx, true);
}

static JSValue js_ee_listener_count(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "expected an event type");
    JSAtom type = JS_ValueToAtom(ctx, argv[0]);
    if (type == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue events = sxn_ee_events(ctx, this_val);
    JSValue list = JS_GetProperty(ctx, events, type);
    uint32_t len = sxn_ee_length(ctx, list);
    JS_FreeValue(ctx, list); JS_FreeValue(ctx, events); JS_FreeAtom(ctx, type);
    return JS_NewUint32(ctx, len);
}

static JSValue js_ee_listeners(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "expected an event type");
    JSAtom type = JS_ValueToAtom(ctx, argv[0]);
    if (type == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue events = sxn_ee_events(ctx, this_val);
    JSValue list = JS_GetProperty(ctx, events, type);
    uint32_t len = sxn_ee_length(ctx, list);
    JS_FreeValue(ctx, events); JS_FreeAtom(ctx, type);
    JSValue out = JS_NewArray(ctx);
    for (uint32_t i = 0; i < len; i++) {
        JSValue l = JS_GetPropertyUint32(ctx, list, i);
        JSValue original = JS_GetPropertyStr(ctx, l, "_original");
        JS_SetPropertyUint32(ctx, out, i, JS_IsUndefined(original) ? l : original);
        if (!JS_IsUndefined(original)) JS_FreeValue(ctx, l);
    }
    JS_FreeValue(ctx, list);
    return out;
}

/* --- path: posix, phase 4 of replacing node_compat.js with native C. Pure
   string manipulation (no I/O), ported 1:1 from node_compat.js's
   reduceSegments/makePathImpl algorithm for the "/" separator case -- same
   segment-reduction rules (drop "."/empty, resolve ".." against the last
   real segment, keep a leading ".." run for relative paths), just walking
   C strings instead of JS regex splits. win32 stays in JS (see
   node_compat.js): it's reached only when __sxnIsWindows, far off this
   runtime's common path, so it's left for a later pass. */
typedef struct { char **items; size_t len, cap; } SxnStrVec;

static void sxn_strvec_push(SxnStrVec *v, char *s) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = realloc(v->items, v->cap * sizeof(*v->items));
    }
    v->items[v->len++] = s;
}

static void sxn_strvec_free(SxnStrVec *v) {
    for (size_t i = 0; i < v->len; i++) free(v->items[i]);
    free(v->items);
}

static char *sxn_strndup(const char *s, size_t n) {
    char *out = malloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* Splits `rest` on '/' and reduces in one pass -- empty segments (from
   consecutive/leading/trailing slashes) and "." are simply never pushed, so
   no separate tokenizer is needed. */
static void sxn_posix_reduce(SxnStrVec *out, const char *rest, bool is_abs) {
    const char *p = rest;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 0 || (len == 1 && start[0] == '.')) {
            if (*p) p++;
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (out->len && strcmp(out->items[out->len - 1], "..") != 0) {
                free(out->items[--out->len]);
            } else if (!is_abs) {
                sxn_strvec_push(out, sxn_strndup("..", 2));
            }
        } else {
            sxn_strvec_push(out, sxn_strndup(start, len));
        }
        if (*p) p++;
    }
}

static char *sxn_posix_join_segments(SxnStrVec *segs, bool abs) {
    size_t total = abs ? 1 : 0;
    for (size_t i = 0; i < segs->len; i++) total += strlen(segs->items[i]) + (i ? 1 : 0);
    char *out = malloc(total + 1);
    char *w = out;
    if (abs) *w++ = '/';
    for (size_t i = 0; i < segs->len; i++) {
        if (i) *w++ = '/';
        size_t l = strlen(segs->items[i]);
        memcpy(w, segs->items[i], l);
        w += l;
    }
    *w = '\0';
    return out;
}

static char *sxn_posix_normalize(const char *path) {
    if (!*path) return strdup(".");
    bool abs = path[0] == '/';
    const char *rest = abs ? path + 1 : path;
    bool trailing_sep = *rest && rest[strlen(rest) - 1] == '/';
    SxnStrVec segs = {0};
    sxn_posix_reduce(&segs, rest, abs);
    char *out = sxn_posix_join_segments(&segs, abs);
    if (!*out) { free(out); out = strdup("."); }
    if (trailing_sep && segs.len && out[strlen(out) - 1] != '/') {
        size_t len = strlen(out);
        out = realloc(out, len + 2);
        out[len] = '/'; out[len + 1] = '\0';
    }
    sxn_strvec_free(&segs);
    return out;
}

static char *sxn_getcwd_alloc(void) {
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return strdup("/");
    return strdup(buf);
}

/* Shared by resolve() and relative(): resolve `segs[0..n)` against cwd,
   right-to-left, stopping at the first absolute segment -- same algorithm
   as node_compat.js's resolve(), just fed a plain string array instead of
   `arguments`. */
static char *sxn_posix_resolve_core(const char *const *segs, int n) {
    char *resolved = strdup("");
    bool resolved_absolute = false;
    for (int i = n - 1; i >= -1 && !resolved_absolute; i--) {
        const char *seg = i >= 0 ? segs[i] : NULL;
        char *cwd = NULL;
        if (!seg) { cwd = sxn_getcwd_alloc(); seg = cwd; }
        if (*seg) {
            size_t seg_len = strlen(seg), old_len = strlen(resolved);
            char *next = malloc(seg_len + 1 + old_len + 1);
            memcpy(next, seg, seg_len);
            next[seg_len] = '/';
            memcpy(next + seg_len + 1, resolved, old_len + 1);
            free(resolved);
            resolved = next;
            resolved_absolute = seg[0] == '/';
        }
        free(cwd);
    }
    char *out = sxn_posix_normalize(resolved);
    free(resolved);
    if (!resolved_absolute) {
        char *cwd = sxn_getcwd_alloc();
        size_t cwd_len = strlen(cwd), out_len = strlen(out);
        char *combined = malloc(cwd_len + 1 + out_len + 1);
        memcpy(combined, cwd, cwd_len);
        combined[cwd_len] = '/';
        memcpy(combined + cwd_len + 1, out, out_len + 1);
        free(cwd); free(out);
        out = sxn_posix_normalize(combined);
        free(combined);
    }
    size_t len = strlen(out);
    if (len > 1 && out[len - 1] == '/') out[len - 1] = '\0'; /* resolve() never keeps a trailing separator (root "/" excepted) */
    return out;
}

static JSValue sxn_cstr_list_call(JSContext *ctx, int argc, JSValueConst *argv,
                                   char *(*fn)(const char *const *, int)) {
    const char **segs = argc ? malloc(sizeof(*segs) * (size_t)argc) : NULL;
    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) continue;
        if (!JS_IsString(argv[i])) { free(segs); return JS_ThrowTypeError(ctx, "path segments must be strings"); }
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) { free(segs); return JS_EXCEPTION; }
        segs[n++] = s;
    }
    char *result = fn(segs, n);
    for (int i = 0; i < n; i++) JS_FreeCString(ctx, segs[i]);
    free(segs);
    JSValue out = JS_NewString(ctx, result);
    free(result);
    return out;
}

static char *sxn_posix_join_core(const char *const *segs, int n) {
    size_t total = 0;
    for (int i = 0; i < n; i++) if (*segs[i]) total += strlen(segs[i]) + 1;
    if (!total) return strdup(".");
    char *joined = malloc(total);
    char *w = joined;
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (!*segs[i]) continue;
        if (!first) *w++ = '/';
        first = false;
        size_t l = strlen(segs[i]);
        memcpy(w, segs[i], l);
        w += l;
    }
    *w = '\0';
    char *out = sxn_posix_normalize(joined);
    free(joined);
    return out;
}

static JSValue js_path_posix_join(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return sxn_cstr_list_call(ctx, argc, argv, sxn_posix_join_core);
}

static JSValue js_path_posix_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return sxn_cstr_list_call(ctx, argc, argv, sxn_posix_resolve_core);
}

static JSValue js_path_posix_normalize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : JS_ToCString(ctx, JS_UNDEFINED);
    if (!p) return JS_EXCEPTION;
    char *out = sxn_posix_normalize(p);
    JS_FreeCString(ctx, p);
    JSValue result = JS_NewString(ctx, out);
    free(out);
    return result;
}

static JSValue js_path_posix_is_absolute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : JS_ToCString(ctx, JS_UNDEFINED);
    if (!p) return JS_EXCEPTION;
    bool abs = p[0] == '/';
    JS_FreeCString(ctx, p);
    return JS_NewBool(ctx, abs);
}

static JSValue js_path_posix_dirname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : JS_ToCString(ctx, JS_UNDEFINED);
    if (!p) return JS_EXCEPTION;
    bool abs = p[0] == '/';
    const char *rest = abs ? p + 1 : p;
    size_t rlen = strlen(rest);
    long end = -1;
    bool matched_sep = true;
    for (long i = (long)rlen - 1; i >= 0; i--) {
        if (rest[i] == '/') {
            if (!matched_sep) { end = i; break; }
        } else matched_sep = false;
    }
    JSValue result;
    if (end == -1) result = JS_NewString(ctx, abs ? "/" : ".");
    else {
        char *out = malloc((size_t)end + 2);
        size_t off = 0;
        if (abs) out[off++] = '/';
        memcpy(out + off, rest, (size_t)end);
        out[off + (size_t)end] = '\0';
        result = JS_NewString(ctx, out);
        free(out);
    }
    JS_FreeCString(ctx, p);
    return result;
}

static char *sxn_posix_basename_core(const char *p, const char *suffix) {
    bool abs = p[0] == '/';
    const char *rest = abs ? p + 1 : p;
    size_t rlen = strlen(rest);
    while (rlen && rest[rlen - 1] == '/') rlen--; /* strip trailing separators */
    long idx = -1;
    for (long i = (long)rlen - 1; i >= 0; i--) if (rest[i] == '/') { idx = i; break; }
    const char *base_start = idx == -1 ? rest : rest + idx + 1;
    size_t base_len = idx == -1 ? rlen : rlen - (size_t)idx - 1;
    if (suffix && *suffix) {
        size_t suf_len = strlen(suffix);
        if (base_len > suf_len && !strncmp(base_start + base_len - suf_len, suffix, suf_len)) base_len -= suf_len;
    }
    return sxn_strndup(base_start, base_len);
}

static JSValue js_path_posix_basename(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : JS_ToCString(ctx, JS_UNDEFINED);
    if (!p) return JS_EXCEPTION;
    const char *suffix = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    char *base = sxn_posix_basename_core(p, suffix);
    JS_FreeCString(ctx, p);
    if (suffix) JS_FreeCString(ctx, suffix);
    JSValue result = JS_NewString(ctx, base);
    free(base);
    return result;
}

static JSValue js_path_posix_extname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : JS_ToCString(ctx, JS_UNDEFINED);
    if (!p) return JS_EXCEPTION;
    char *base = sxn_posix_basename_core(p, NULL);
    JS_FreeCString(ctx, p);
    long dot = -1;
    size_t blen = strlen(base);
    for (long i = (long)blen - 1; i >= 0; i--) if (base[i] == '.') { dot = i; break; }
    JSValue result = (dot <= 0) ? JS_NewString(ctx, "") : JS_NewStringLen(ctx, base + dot, blen - (size_t)dot);
    free(base);
    return result;
}

static JSValue js_path_posix_relative(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *from_in = argc > 0 ? JS_ToCString(ctx, argv[0]) : JS_ToCString(ctx, JS_UNDEFINED);
    const char *to_in = argc > 1 ? JS_ToCString(ctx, argv[1]) : JS_ToCString(ctx, JS_UNDEFINED);
    if (!from_in || !to_in) { JS_FreeCString(ctx, from_in); JS_FreeCString(ctx, to_in); return JS_EXCEPTION; }
    char *from = sxn_posix_resolve_core(&from_in, 1);
    char *to = sxn_posix_resolve_core(&to_in, 1);
    JS_FreeCString(ctx, from_in); JS_FreeCString(ctx, to_in);
    JSValue result;
    if (!strcmp(from, to)) result = JS_NewString(ctx, "");
    else {
        SxnStrVec fparts = {0}, tparts = {0};
        sxn_posix_reduce(&fparts, from + (from[0] == '/' ? 1 : 0), true);
        sxn_posix_reduce(&tparts, to + (to[0] == '/' ? 1 : 0), true);
        size_t common = 0;
        while (common < fparts.len && common < tparts.len && !strcmp(fparts.items[common], tparts.items[common])) common++;
        SxnStrVec out = {0};
        for (size_t i = common; i < fparts.len; i++) sxn_strvec_push(&out, strdup(".."));
        for (size_t i = common; i < tparts.len; i++) sxn_strvec_push(&out, strdup(tparts.items[i]));
        char *joined = sxn_posix_join_segments(&out, false);
        result = JS_NewString(ctx, joined);
        free(joined);
        sxn_strvec_free(&fparts); sxn_strvec_free(&tparts); sxn_strvec_free(&out);
    }
    free(from); free(to);
    return result;
}

/* --- buffer: Buffer static methods, phase 5 of replacing node_compat.js
   with native C. `class Buffer extends Uint8Array` stays a JS class
   declaration -- QuickJS's bytecode already special-cases extending a
   built-in TypedArray, and re-deriving that machinery in C (correct
   handling of the backing ArrayBuffer, byteOffset/byteLength, the
   species-constructor protocol) isn't proportionate to the payoff for a
   handful of static helpers. isBuffer/alloc/allocUnsafe, though, are pure
   dispatch with no encoding logic, so they're bound here as data closures
   over the Buffer constructor (captured once, after node_compat.js installs
   it) instead of a JS call frame per call. toString/from/slice/concat stay
   JS: they already bottom out in native primitives per call (__sxnUtf8*,
   Uint8Array#toHex/fromHex/toBase64/fromBase64), so there's no comparable
   win left to extract without the same ArrayBuffer-internals risk. */
static JSValue js_buffer_is_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic, JSValueConst *func_data) {
    (void)this_val; (void)magic;
    if (argc < 1) return JS_NewBool(ctx, false);
    int is = JS_IsInstanceOf(ctx, argv[0], func_data[0]);
    if (is < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, is);
}

static JSValue js_buffer_alloc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                int magic, JSValueConst *func_data) {
    (void)this_val; (void)magic;
    uint32_t size = 0;
    JS_ToUint32(ctx, &size, argc > 0 ? argv[0] : JS_UNDEFINED);
    JSValue size_val = JS_NewUint32(ctx, size);
    JSValueConst ctor_argv[1] = { size_val };
    JSValue buf = JS_CallConstructor(ctx, func_data[0], 1, ctor_argv);
    JS_FreeValue(ctx, size_val);
    if (JS_IsException(buf)) return buf;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        JSAtom fill_atom = JS_NewAtom(ctx, "fill");
        JSValueConst fill_argv[1] = { argv[1] };
        JSValue ret = JS_Invoke(ctx, buf, fill_atom, 1, fill_argv);
        JS_FreeAtom(ctx, fill_atom);
        if (JS_IsException(ret)) { JS_FreeValue(ctx, buf); return ret; }
        JS_FreeValue(ctx, ret);
    }
    return buf;
}

static JSValue js_buffer_alloc_unsafe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic, JSValueConst *func_data) {
    (void)this_val; (void)magic;
    uint32_t size = 0;
    JS_ToUint32(ctx, &size, argc > 0 ? argv[0] : JS_UNDEFINED);
    JSValue size_val = JS_NewUint32(ctx, size);
    JSValueConst ctor_argv[1] = { size_val };
    JSValue buf = JS_CallConstructor(ctx, func_data[0], 1, ctor_argv);
    JS_FreeValue(ctx, size_val);
    return buf;
}

/* Buffer.from fast path: intercepts only the (string, utf-8-or-absent)
   shape -- the hot one -- and builds the instance natively: one-pass UTF-8
   encode into a bare ArrayBuffer (JS_NewArrayBufferFromString), then a
   Uint8Array-class object against the cached Buffer.prototype
   (JS_NewUint8ArrayWithProto), skipping the JS `new Buffer(ab)` derived-
   constructor round trip per call. Every other argument shape (arrays,
   views, other/oddly-cased encodings) delegates to the original JS
   Buffer.from captured in func_data[1], which keeps its full behavior.
   func_data[0] is Buffer.prototype, captured at install; a user reassigning
   Buffer.prototype afterwards won't be seen by this fast path (accepted --
   Node's own internal fast paths behave the same way). */
static JSValue js_buffer_from_fast(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic, JSValueConst *func_data) {
    (void)magic;
    if (argc >= 1 && JS_IsString(argv[0])) {
        bool utf8 = false;
        if (argc < 2 || JS_IsUndefined(argv[1])) {
            utf8 = true;
        } else if (JS_IsString(argv[1])) {
            /* Exact-lowercase check only (matching node_compat.js's own fast
               path); "UTF-8" etc. falls through to the JS implementation's
               toLowerCase handling. JS_ToCString on an ASCII literal is
               zero-copy. */
            const char *enc = JS_ToCString(ctx, argv[1]);
            if (!enc) return JS_EXCEPTION;
            utf8 = !strcmp(enc, "utf-8") || !strcmp(enc, "utf8");
            JS_FreeCString(ctx, enc);
        }
        if (utf8) {
            JSValue ab = JS_NewArrayBufferFromString(ctx, argv[0]);
            return JS_NewUint8ArrayWithProto(ctx, func_data[0], ab);
        }
    }
    return JS_Call(ctx, func_data[1], this_val, argc, argv);
}

/* QuickJS interns the empty shape used for `new Foo()` in a runtime-wide
   hash table, but the table holds no reference: the shape's only owners are
   the objects wearing it. A loop that allocates and drops one object per
   iteration therefore destroys the shape with the last object and rebuilds
   it on the next one -- js_new_shape2 and js_free_shape0 both run every
   iteration, and each shape free also flushes the engine's property-lookup
   cache.

   Keeping one throwaway instance of each core value type alive for the life
   of the context pins those shapes in the table, so allocation loops reuse
   them instead. Measured at ~14% of the Buffer benchmark. These are the
   runtime's own types -- Buffer, Uint8Array and ArrayBuffer are created and
   discarded in hot loops by definition -- so their shapes are effectively
   runtime infrastructure, the same way an engine keeps its builtins' hidden
   classes permanently. The instances are parked on a non-enumerable global
   so they are released with the global object at teardown (keeping
   --leak-check clean) rather than leaked outright. */
static void sxn_pin_core_shapes(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue pins = JS_NewArray(ctx);
    uint32_t n = 0;

    JSValue buffer_ctor = JS_GetPropertyStr(ctx, global, "Buffer");
    if (JS_IsFunction(ctx, buffer_ctor)) {
        JSValue arg = JS_NewString(ctx, "");
        JSValueConst argv[1] = { arg };
        JSValue from = JS_GetPropertyStr(ctx, buffer_ctor, "from");
        JSValue buf = JS_Call(ctx, from, buffer_ctor, 1, argv);
        JS_FreeValue(ctx, from);
        JS_FreeValue(ctx, arg);
        if (JS_IsException(buf)) JS_FreeValue(ctx, JS_GetException(ctx));
        else JS_SetPropertyUint32(ctx, pins, n++, buf);
    }
    JS_FreeValue(ctx, buffer_ctor);

    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
    if (!JS_IsException(ab)) JS_SetPropertyUint32(ctx, pins, n++, ab);
    else JS_FreeValue(ctx, JS_GetException(ctx));

    JSValue u8 = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)"", 0);
    if (!JS_IsException(u8)) JS_SetPropertyUint32(ctx, pins, n++, u8);
    else JS_FreeValue(ctx, JS_GetException(ctx));

    JS_DefinePropertyValueStr(ctx, global, "__sxnPinnedShapes", pins, 0 /* non-enumerable, non-writable, non-configurable */);
    JS_FreeValue(ctx, global);
}

static void sxn_install_buffer_natives(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Buffer");
    JS_FreeValue(ctx, global);
    if (JS_IsUndefined(ctor)) return;
    JSValueConst data[1] = { ctor };
    JS_SetPropertyStr(ctx, ctor, "isBuffer", JS_NewCFunctionData(ctx, js_buffer_is_buffer, 1, 0, 1, data));
    JS_SetPropertyStr(ctx, ctor, "alloc", JS_NewCFunctionData(ctx, js_buffer_alloc, 2, 0, 1, data));
    JS_SetPropertyStr(ctx, ctor, "allocUnsafe", JS_NewCFunctionData(ctx, js_buffer_alloc_unsafe, 1, 0, 1, data));
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    JSValue orig_from = JS_GetPropertyStr(ctx, ctor, "from");
    if (!JS_IsUndefined(proto) && JS_IsFunction(ctx, orig_from)) {
        JSValueConst from_data[2] = { proto, orig_from };
        JS_SetPropertyStr(ctx, ctor, "from", JS_NewCFunctionData(ctx, js_buffer_from_fast, 2, 0, 2, from_data));
    }
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, orig_from);
    JS_FreeValue(ctx, ctor);
    sxn_pin_core_shapes(ctx);
}

static JSValue js_sxn_exit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t code = 0;
    if (argc > 0) JS_ToInt32(ctx, &code, argv[0]);
    exit(code);
    return JS_UNDEFINED; /* unreachable */
}

/* --- signal handling -----------------------------------------------------
   process.on('SIGINT'/'SIGTERM', fn) is wired to a libuv uv_signal_t, not a
   raw POSIX signal() / sigaction() handler. libuv owns the actual OS-level
   signal registration internally (on POSIX it installs its own sigaction
   whose handler only writes to an internal self-pipe -- see libuv's
   src/unix/signal.c) and only invokes sxn_signal_cb below from inside
   uv_run(), i.e. on the normal call stack driven by sxn_run_event_loop(),
   never from async-signal context. This file contains no signal()/
   sigaction() call of its own, so JS_Call is never reached from inside a
   raw signal handler. */
typedef struct SxnSignalWatch {
    uv_signal_t handle;
    JSContext *ctx;
    JSValue dispatcher;
    bool active;
} SxnSignalWatch;

static SxnSignalWatch sxn_sig_int, sxn_sig_term;

static void sxn_signal_cb(uv_signal_t *handle, int signum) {
    (void)signum;
    SxnSignalWatch *w = (SxnSignalWatch *)handle->data;
    JSValue ret = JS_Call(w->ctx, w->dispatcher, JS_UNDEFINED, 0, NULL);
    JS_FreeValue(w->ctx, ret); /* an exception here just gets dropped, same as AbortSignal listener errors in network.c */
}

static JSValue js_sxn_watch_signal(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!name || argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, name);
        return JS_ThrowTypeError(ctx, "watchSignal(name, fn) requires a signal name and a function");
    }
    int signum = 0;
    SxnSignalWatch *w = NULL;
    if (!strcmp(name, "SIGINT")) { signum = SIGINT; w = &sxn_sig_int; }
    else if (!strcmp(name, "SIGTERM")) { signum = SIGTERM; w = &sxn_sig_term; }
    JS_FreeCString(ctx, name);
    if (!w) return JS_ThrowTypeError(ctx, "unsupported signal (only SIGINT/SIGTERM)");
    if (w->active) JS_FreeValue(w->ctx, w->dispatcher);
    else {
        /* Same process-wide libuv loop as sxn_loop() in network.c (both are
           just accessors for uv_default_loop()); sxn_run_event_loop drives it. */
        uv_signal_init(uv_default_loop(), &w->handle);
        w->handle.data = w;
        w->active = true;
    }
    w->ctx = ctx;
    w->dispatcher = JS_DupValue(ctx, argv[1]);
    uv_signal_start(&w->handle, sxn_signal_cb, signum);
    return JS_UNDEFINED;
}

/* --- node:* module registration ------------------------------------------
   Same pattern as js_init_module_std/os/bjson in quickjs-libc.c: register
   the export names up front via JS_NewCModule/JS_AddModuleExport, then fill
   in the actual values (pulled from globals node_compat.js defines) from
   the init callback invoked at module instantiation time. */

static JSValue node_global_lookup(JSContext *ctx, const char *name) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue v = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);
    return v;
}

static int node_buffer_init(JSContext *ctx, JSModuleDef *m) {
    JSValue buffer = node_global_lookup(ctx, "Buffer");
    JS_SetModuleExport(ctx, m, "default", JS_DupValue(ctx, buffer));
    JS_SetModuleExport(ctx, m, "Buffer", buffer);
    return 0;
}

static JSModuleDef *sxn_init_module_node_buffer(JSContext *ctx, const char *name) {
    JSModuleDef *m = JS_NewCModule(ctx, name, node_buffer_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "Buffer");
    return m;
}

static int node_events_init(JSContext *ctx, JSModuleDef *m) {
    JSValue ee = node_global_lookup(ctx, "__sxnEventEmitter");
    JS_SetModuleExport(ctx, m, "default", JS_DupValue(ctx, ee));
    JS_SetModuleExport(ctx, m, "EventEmitter", ee);
    return 0;
}

static JSModuleDef *sxn_init_module_node_events(JSContext *ctx, const char *name) {
    JSModuleDef *m = JS_NewCModule(ctx, name, node_events_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "EventEmitter");
    return m;
}

static const char *node_path_export_names[] = {
    "posix", "win32", "sep", "delimiter", "join", "resolve",
    "normalize", "basename", "dirname", "extname", "isAbsolute", "relative",
};

static int node_path_init(JSContext *ctx, JSModuleDef *m) {
    JSValue path = node_global_lookup(ctx, "__sxnPath");
    JS_SetModuleExport(ctx, m, "default", JS_DupValue(ctx, path));
    for (size_t i = 0; i < countof(node_path_export_names); i++)
        JS_SetModuleExport(ctx, m, node_path_export_names[i], JS_GetPropertyStr(ctx, path, node_path_export_names[i]));
    JS_FreeValue(ctx, path);
    return 0;
}

static JSModuleDef *sxn_init_module_node_path(JSContext *ctx, const char *name) {
    JSModuleDef *m = JS_NewCModule(ctx, name, node_path_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "default");
    for (size_t i = 0; i < countof(node_path_export_names); i++)
        JS_AddModuleExport(ctx, m, node_path_export_names[i]);
    return m;
}

static const char *node_fs_export_names[] = { "readFileSync", "writeFileSync", "existsSync" };

static int node_fs_init(JSContext *ctx, JSModuleDef *m) {
    JSValue fs = node_global_lookup(ctx, "__sxnFs");
    JS_SetModuleExport(ctx, m, "default", JS_DupValue(ctx, fs));
    for (size_t i = 0; i < countof(node_fs_export_names); i++)
        JS_SetModuleExport(ctx, m, node_fs_export_names[i], JS_GetPropertyStr(ctx, fs, node_fs_export_names[i]));
    JS_FreeValue(ctx, fs);
    return 0;
}

static JSModuleDef *sxn_init_module_node_fs(JSContext *ctx, const char *name) {
    JSModuleDef *m = JS_NewCModule(ctx, name, node_fs_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "default");
    for (size_t i = 0; i < countof(node_fs_export_names); i++)
        JS_AddModuleExport(ctx, m, node_fs_export_names[i]);
    return m;
}

static const char *node_fs_promises_export_names[] = { "readFile", "writeFile" };

static int node_fs_promises_init(JSContext *ctx, JSModuleDef *m) {
    JSValue fsp = node_global_lookup(ctx, "__sxnFsPromises");
    JS_SetModuleExport(ctx, m, "default", JS_DupValue(ctx, fsp));
    for (size_t i = 0; i < countof(node_fs_promises_export_names); i++)
        JS_SetModuleExport(ctx, m, node_fs_promises_export_names[i], JS_GetPropertyStr(ctx, fsp, node_fs_promises_export_names[i]));
    JS_FreeValue(ctx, fsp);
    return 0;
}

static JSModuleDef *sxn_init_module_node_fs_promises(JSContext *ctx, const char *name) {
    JSModuleDef *m = JS_NewCModule(ctx, name, node_fs_promises_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "default");
    for (size_t i = 0; i < countof(node_fs_promises_export_names); i++)
        JS_AddModuleExport(ctx, m, node_fs_promises_export_names[i]);
    return m;
}

static int node_process_init(JSContext *ctx, JSModuleDef *m) {
    JSValue process = node_global_lookup(ctx, "process");
    JS_SetModuleExport(ctx, m, "default", process);
    return 0;
}

static JSModuleDef *sxn_init_module_node_process(JSContext *ctx, const char *name) {
    JSModuleDef *m = JS_NewCModule(ctx, name, node_process_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "default");
    return m;
}

void sxn_free_node_compat(JSContext *ctx) {
    /* The cached atoms below hold real references for the life of the
       context; without this they show up as leaks under --leak-check (only
       visible in builds where the runtime's leak dumps are compiled in).
       JS_FreeAtom is a no-op for the predefined ones. */
    JS_FreeAtom(ctx, sxn_atom_events);
    JS_FreeAtom(ctx, sxn_atom_length);
    JS_FreeAtom(ctx, sxn_atom_error);
    sxn_atom_events = JS_ATOM_NULL;
    sxn_atom_length = JS_ATOM_NULL;
    sxn_atom_error = JS_ATOM_NULL;
}

int sxn_install_node_compat(JSContext *ctx, const char *exec_path) {
    if (sxn_atom_events == JS_ATOM_NULL) sxn_atom_events = JS_NewAtom(ctx, "_events");
    if (sxn_atom_length == JS_ATOM_NULL) sxn_atom_length = JS_NewAtom(ctx, "length");
    if (sxn_atom_error == JS_ATOM_NULL) sxn_atom_error = JS_NewAtom(ctx, "error");
    JSValue global = JS_GetGlobalObject(ctx);
#ifdef _WIN32
    JS_SetPropertyStr(ctx, global, "__sxnIsWindows", JS_NewBool(ctx, true));
#else
    JS_SetPropertyStr(ctx, global, "__sxnIsWindows", JS_NewBool(ctx, false));
#endif
    JS_SetPropertyStr(ctx, global, "__sxnExecPath", JS_NewString(ctx, exec_path ? exec_path : "sxn"));
    JS_SetPropertyStr(ctx, global, "__sxnCwd", JS_NewCFunction(ctx, js_sxn_cwd, "__sxnCwd", 0));
    JS_SetPropertyStr(ctx, global, "__sxnEnvObject", sxn_new_env_object(ctx));
    JS_SetPropertyStr(ctx, global, "__sxnEeOn", JS_NewCFunction(ctx, js_ee_on, "on", 2));
    JS_SetPropertyStr(ctx, global, "__sxnEeOff", JS_NewCFunction(ctx, js_ee_off, "off", 2));
    JS_SetPropertyStr(ctx, global, "__sxnEeEmit", JS_NewCFunction(ctx, js_ee_emit, "emit", 1));
    JS_SetPropertyStr(ctx, global, "__sxnEeListenerCount", JS_NewCFunction(ctx, js_ee_listener_count, "listenerCount", 1));
    JS_SetPropertyStr(ctx, global, "__sxnEeListeners", JS_NewCFunction(ctx, js_ee_listeners, "listeners", 1));
    JS_SetPropertyStr(ctx, global, "__sxnEeRemoveAllListeners", JS_NewCFunction(ctx, js_ee_remove_all_listeners, "removeAllListeners", 1));
    JS_SetPropertyStr(ctx, global, "__sxnPosixJoin", JS_NewCFunction(ctx, js_path_posix_join, "join", 0));
    JS_SetPropertyStr(ctx, global, "__sxnPosixResolve", JS_NewCFunction(ctx, js_path_posix_resolve, "resolve", 0));
    JS_SetPropertyStr(ctx, global, "__sxnPosixNormalize", JS_NewCFunction(ctx, js_path_posix_normalize, "normalize", 1));
    JS_SetPropertyStr(ctx, global, "__sxnPosixIsAbsolute", JS_NewCFunction(ctx, js_path_posix_is_absolute, "isAbsolute", 1));
    JS_SetPropertyStr(ctx, global, "__sxnPosixDirname", JS_NewCFunction(ctx, js_path_posix_dirname, "dirname", 1));
    JS_SetPropertyStr(ctx, global, "__sxnPosixBasename", JS_NewCFunction(ctx, js_path_posix_basename, "basename", 2));
    JS_SetPropertyStr(ctx, global, "__sxnPosixExtname", JS_NewCFunction(ctx, js_path_posix_extname, "extname", 1));
    JS_SetPropertyStr(ctx, global, "__sxnPosixRelative", JS_NewCFunction(ctx, js_path_posix_relative, "relative", 2));
    JS_SetPropertyStr(ctx, global, "__sxnExit", JS_NewCFunction(ctx, js_sxn_exit, "__sxnExit", 1));
    JS_SetPropertyStr(ctx, global, "__sxnWatchSignal", JS_NewCFunction(ctx, js_sxn_watch_signal, "__sxnWatchSignal", 2));
    JS_SetPropertyStr(ctx, global, "__sxnReadFileSync", JS_NewCFunction(ctx, js_sxn_read_file_sync, "__sxnReadFileSync", 1));
    JS_SetPropertyStr(ctx, global, "__sxnWriteFileSync", JS_NewCFunction(ctx, js_sxn_write_file_sync, "__sxnWriteFileSync", 2));
    JS_SetPropertyStr(ctx, global, "__sxnExistsSync", JS_NewCFunction(ctx, js_sxn_exists_sync, "__sxnExistsSync", 1));
    JS_FreeValue(ctx, global);

    /* Evaluated as a module (compile-then-JS_EvalFunction, same pattern as
       main.c's .mjs path) rather than JS_EVAL_TYPE_GLOBAL: QuickJS's bytecode
       generator emits measurably faster function/property dispatch for module
       code than for global code, even with JS_EVAL_FLAG_STRICT forcing the
       same strict-mode semantics on the global-code path. The file has no
       import/export of its own -- it only assigns onto globalThis, which
       module vs. global evaluation doesn't affect -- so this is a pure
       bytecode-shape win with no behavior change. */
    JSValue result = JS_Eval(ctx, sxn_node_compat_js, strlen(sxn_node_compat_js), "<sxn:node_compat>",
                              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(result)) { JS_FreeValue(ctx, result); return -1; }
    if (js_module_set_import_meta(ctx, result, true, true) < 0) { JS_FreeValue(ctx, result); return -1; }
    result = JS_EvalFunction(ctx, result);
    if (JS_IsException(result)) { JS_FreeValue(ctx, result); return -1; }
    JS_FreeValue(ctx, result);
    sxn_install_buffer_natives(ctx);

    if (!sxn_init_module_node_buffer(ctx, "node:buffer")) return -1;
    if (!sxn_init_module_node_events(ctx, "node:events")) return -1;
    if (!sxn_init_module_node_path(ctx, "node:path")) return -1;
    if (!sxn_init_module_node_process(ctx, "node:process")) return -1;
    if (!sxn_init_module_node_fs(ctx, "node:fs")) return -1;
    if (!sxn_init_module_node_fs_promises(ctx, "node:fs/promises")) return -1;
    return 0;
}
