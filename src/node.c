#include <quickjs.h>
#include <quickjs-libc.h>
#include <uv.h>
#include "sxfe.h"
#include "sxn_node_compat.h"
#include <zlib.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
/* MinGW's CRT has no setenv/unsetenv (BSD/glibc extensions), and declares
   the environment block as _environ rather than environ. _putenv_s with an
   empty value removes the variable, matching unsetenv - documented CRT
   behavior, not a guess. */
#define environ _environ
static int setenv(const char *name, const char *value, int overwrite) {
    (void)overwrite;
    return _putenv_s(name, value);
}
static int unsetenv(const char *name) {
    return _putenv_s(name, "");
}
#else
#include <unistd.h>
extern char **environ;
#endif

#define countof(x) (sizeof(x) / sizeof((x)[0]))

/* --- native primitives consumed only by node_compat.js ------------------
   Pure spec/behavior logic (Buffer, path, EventEmitter, the process object
   shape) lives in node_compat.js; this file supplies the handful of things
   that genuinely need C: real cwd/env access, exit, and safe signal
   delivery. Same split as bootstrap.js / network.c (Task 2). */

/* process.cwd() is asked constantly -- every relative path a package
   resolves goes through it -- and getcwd() is a system call that walks the
   directory back to the root: 7 microseconds here. Node caches it, and so
   does this, with process.chdir() below as the only thing that can change
   it. */
static char sxn_cwd_cache[4096];

static JSValue js_sxn_cwd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!sxn_cwd_cache[0] && !getcwd(sxn_cwd_cache, sizeof(sxn_cwd_cache)))
        return JS_ThrowInternalError(ctx, "getcwd failed: %s", strerror(errno));
    return JS_NewString(ctx, sxn_cwd_cache);
}

static JSValue js_sxn_chdir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *dir = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!dir) return JS_ThrowTypeError(ctx, "chdir(directory) requires a directory");
    int rc = chdir(dir);
    if (rc != 0) {
        JSValue error = JS_ThrowInternalError(ctx, "chdir %s: %s", dir, strerror(errno));
        JSValue exception = JS_GetException(ctx);
        JS_SetPropertyStr(ctx, exception, "code", JS_NewString(ctx, "ENOENT"));
        JS_Throw(ctx, exception);
        JS_FreeCString(ctx, dir);
        return error;
    }
    JS_FreeCString(ctx, dir);
    sxn_cwd_cache[0] = '\0';        /* the cache is what chdir invalidates */
    return JS_UNDEFINED;
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
    /* Bytes are written as they are. Everything went through
       JS_ToCStringLen before, which turns a Buffer into its decimal digits
       and any byte that is not valid UTF-8 into the replacement character. */
    size_t length = 0;
    const char *data = NULL;
    const char *owned = NULL;
    if (argc > 1) {
        uint8_t *bytes = JS_GetUint8Array(ctx, &length, argv[1]);
        if (bytes) data = (const char *)bytes;
        else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            owned = JS_ToCStringLen(ctx, &length, argv[1]);
            data = owned;
        }
    }
    if (!data) { JS_FreeCString(ctx, path); return JS_ThrowTypeError(ctx, "expected data"); }
    FILE *file = fopen(path, "wb");
    if (!file) {
        JSValue err = JS_ThrowInternalError(ctx, "cannot write '%s': %s", path, strerror(errno));
        JS_FreeCString(ctx, path); JS_FreeCString(ctx, owned);
        return err;
    }
    size_t written = fwrite(data, 1, length, file);
    bool failed = fclose(file) != 0 || written != length;
    JS_FreeCString(ctx, path); JS_FreeCString(ctx, owned);
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
/* All normal EventEmitter instances share a simple own `_events` data slot.
   Cache that slot by shape so emit's hot lookup avoids a property hash probe,
   while direct replacement/deletion/accessor redefinition still falls back
   through QuickJS's full [[Get]] path. */
static JSOwnDataPropertyCache sxn_ee_events_cache;

static JSValue sxn_ee_events(JSContext *ctx, JSValueConst this_val) {
    JSValue events = JS_GetOwnDataPropertyCached(ctx, this_val, sxn_atom_events,
                                                 &sxn_ee_events_cache);
    /* Create the store on demand. Node does the same, and the mixin pattern
       depends on it: Express copies EventEmitter.prototype onto a bare
       function, so the constructor never runs and there is no _events until
       the first on()/emit(). */
    if (JS_IsUndefined(events) || JS_IsNull(events)) {
        JS_FreeValue(ctx, events);
        events = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(events)) return events;
        if (JS_SetProperty(ctx, this_val, sxn_atom_events, JS_DupValue(ctx, events)) < 0) {
            JS_FreeValue(ctx, events);
            return JS_EXCEPTION;
        }
    }
    return events;
}

/* emit resolves the list before re-entering JS, while `this_val` still roots
   the emitter and therefore its own `_events` value. Borrow the fast own-data
   slot to avoid a refcount pair; unusual receivers/accessors take the normal
   owned-property path. */
static JSValue sxn_ee_events_for_emit(JSContext *ctx, JSValueConst this_val,
                                      bool *borrowed) {
    JSValueConst events;
    if (JS_TryGetOwnDataPropertyCached(this_val, sxn_atom_events,
                                       &sxn_ee_events_cache, &events)) {
        *borrowed = true;
        return events;
    }
    *borrowed = false;
    return JS_GetProperty(ctx, this_val, sxn_atom_events);
}

/* emit() resolution memo. Repeated `ee.emit("x", ...)` re-derives the same
   answer every call: the same event-name string converts to the same atom,
   and the same `_events` object yields the same listener array. Cache that
   last resolution.

   Validity is established without ever trusting a bare address:
   - The `_events` object is looked up fresh on every emit (it is the
     emitter's own property, one probe) and its pointer is *compared* to the
     cached one. So replacing `ee._events` wholesale, or emitting on a
     different emitter, misses -- there is no assumption that one emitter
     owns the entry.
   - The cache holds strong references to everything it keys on, so a cached
     object cannot be freed and have its address reused by something else
     while the entry still refers to it. This is the part that makes pointer
     identity sound rather than a bet.
   - `sxn_ee_gen` is bumped by every listener mutation (on/off/once via on,
     removeAllListeners), so adding or removing a listener invalidates the
     memo even though the array object may be unchanged.
   A miss simply falls through to the full lookup. */
static JSValue sxn_ee_memo_events = JS_UNDEFINED; /* strong ref */
static JSValue sxn_ee_memo_typev = JS_UNDEFINED;  /* strong ref: the name string */
static JSValue sxn_ee_memo_list = JS_UNDEFINED;   /* strong ref */
static JSAtom sxn_ee_memo_atom = JS_ATOM_NULL;    /* strong ref */
static uint32_t sxn_ee_memo_gen;
static uint32_t sxn_ee_gen = 1;
static bool sxn_ee_memo_singleton;

static void sxn_ee_memo_clear(JSContext *ctx) {
    JS_FreeValue(ctx, sxn_ee_memo_events);
    JS_FreeValue(ctx, sxn_ee_memo_typev);
    JS_FreeValue(ctx, sxn_ee_memo_list);
    JS_FreeAtom(ctx, sxn_ee_memo_atom);
    sxn_ee_memo_events = JS_UNDEFINED;
    sxn_ee_memo_typev = JS_UNDEFINED;
    sxn_ee_memo_list = JS_UNDEFINED;
    sxn_ee_memo_atom = JS_ATOM_NULL;
    sxn_ee_memo_singleton = false;
}

static void sxn_ee_memo_store(JSContext *ctx, JSValueConst events, JSValueConst typev,
                              JSAtom atom, JSValueConst list) {
    sxn_ee_memo_clear(ctx);
    sxn_ee_memo_events = JS_DupValue(ctx, events);
    sxn_ee_memo_typev = JS_DupValue(ctx, typev);
    sxn_ee_memo_list = JS_DupValue(ctx, list);
    sxn_ee_memo_atom = JS_DupAtom(ctx, atom);
    sxn_ee_memo_gen = sxn_ee_gen;
    sxn_ee_memo_singleton = JS_IsFunction(ctx, list);
}

static uint32_t sxn_ee_length(JSContext *ctx, JSValueConst list) {
    if (JS_IsUndefined(list)) return 0;
    /* Match Node's compact representation: one listener is stored as the
       function itself and promoted to an array only when a second listener is
       added.  This also keeps the hot one-listener emit path out of array
       property access entirely. */
    if (JS_IsFunction(ctx, list)) return 1;
    uint32_t len = 0;
    JSValue len_val = JS_GetProperty(ctx, list, sxn_atom_length);
    JS_ToUint32(ctx, &len, len_val);
    JS_FreeValue(ctx, len_val);
    return len;
}

static JSValue js_ee_on(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    sxn_ee_gen++; /* listener set changes: invalidate the emit memo */
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "listener must be a function");
    JSAtom type = JS_ValueToAtom(ctx, argv[0]);
    if (type == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue events = sxn_ee_events(ctx, this_val);
    JSValue list = JS_GetProperty(ctx, events, type);
    if (JS_IsUndefined(list)) {
        /* Node stores the common one-listener case directly as a function;
           promote to a fast array only when another listener arrives. */
        JS_SetProperty(ctx, events, type, JS_DupValue(ctx, argv[1]));
    } else if (JS_IsFunction(ctx, list)) {
        JSValue promoted = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, promoted, 0, list); /* consumes list */
        JS_SetPropertyUint32(ctx, promoted, 1, JS_DupValue(ctx, argv[1]));
        JS_SetProperty(ctx, events, type, promoted);
        list = JS_UNDEFINED; /* promoted now owns the old function */
    } else {
        JS_SetPropertyUint32(ctx, list, sxn_ee_length(ctx, list), JS_DupValue(ctx, argv[1]));
    }
    JS_FreeValue(ctx, list);
    /* Arm the call-site emit fusion for the sole-listener case, the only shape
       it handles. Registration is where the layout is known; the call site
       re-validates it by shape and slot on every call, and reads sxn_ee_gen,
       which every later mutation bumps, so this never needs explicit
       clearing. */
    if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_STRING) {
        JSValue cur = JS_GetProperty(ctx, events, type);
        if (JS_IsFunction(ctx, cur)) {
            JSValue emit_fn = JS_GetPropertyStr(ctx, this_val, "emit");
            JS_EnableEmitFusion(ctx, emit_fn, this_val, argv[0], cur,
                                sxn_atom_events, type, &sxn_ee_gen);
            JS_FreeValue(ctx, emit_fn);
        }
        JS_FreeValue(ctx, cur);
    }
    JS_FreeValue(ctx, events);
    JS_FreeAtom(ctx, type);
    return JS_DupValue(ctx, this_val);
}

/* How many emits are on the stack. Process-wide for the same reason the
   zlib cache above is, and it only ever chooses between two correct paths
   in off(), so a stale value would cost speed rather than correctness. */
static int sxn_ee_emit_depth;

static JSValue js_ee_off(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    sxn_ee_gen++; /* listener set changes: invalidate the emit memo */
    if (argc < 1) return JS_ThrowTypeError(ctx, "expected an event type");
    JSAtom type = JS_ValueToAtom(ctx, argv[0]);
    if (type == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue events = sxn_ee_events(ctx, this_val);
    JSValue list = JS_GetProperty(ctx, events, type);
    if (JS_IsFunction(ctx, list)) {
        JSValueConst listener = argc > 1 ? argv[1] : JS_UNDEFINED;
        /* once() stores a wrapper as the singleton; match Node's off() by
           checking its _original listener as well as the wrapper identity. */
        JSValue original = JS_GetPropertyStr(ctx, list, "_original");
        bool matches = JS_IsStrictEqual(ctx, list, listener) ||
                       (!JS_IsUndefined(original) && JS_IsStrictEqual(ctx, original, listener));
        JS_FreeValue(ctx, original);
        if (matches)
            JS_DeleteProperty(ctx, events, type, 0);
        JS_FreeValue(ctx, list); JS_FreeValue(ctx, events); JS_FreeAtom(ctx, type);
        return JS_DupValue(ctx, this_val);
    }
    uint32_t len = sxn_ee_length(ctx, list);
    if (!len) {
        JS_FreeValue(ctx, list); JS_FreeValue(ctx, events); JS_FreeAtom(ctx, type);
        return JS_DupValue(ctx, this_val);
    }
    JSValueConst listener = argc > 1 ? argv[1] : JS_UNDEFINED;
    /* Node removes the first match only, so the list is scanned until one is
       found and then closed up in place. This used to build a second array
       and copy every listener into it, which allocated on every removal and
       walked the whole list even when the match was the first entry --
       expensive on an emitter many streams have piped into. */
    if (sxn_ee_emit_depth > 0) {
        /* An emit is walking this list: hand it a copy to keep walking. */
        JSValue kept = JS_NewArray(ctx);
        uint32_t kept_len = 0;
        bool dropped = false;
        for (uint32_t i = 0; i < len; i++) {
            JSValue l = JS_GetPropertyUint32(ctx, list, i);
            bool matches = !dropped && JS_IsStrictEqual(ctx, l, listener);
            if (!matches && !dropped) {
                JSValue original = JS_GetPropertyStr(ctx, l, "_original");
                matches = !JS_IsUndefined(original) && JS_IsStrictEqual(ctx, original, listener);
                JS_FreeValue(ctx, original);
            }
            if (matches) { dropped = true; JS_FreeValue(ctx, l); }
            else JS_SetPropertyUint32(ctx, kept, kept_len++, l);
        }
        if (kept_len == 0) JS_DeleteProperty(ctx, events, type, 0);
        else if (kept_len == 1) {
            JSValue single = JS_GetPropertyUint32(ctx, kept, 0);
            JS_SetProperty(ctx, events, type, single);
        } else {
            JS_SetProperty(ctx, events, type, JS_DupValue(ctx, kept));
        }
        JS_FreeValue(ctx, kept);
        JS_FreeValue(ctx, list); JS_FreeValue(ctx, events); JS_FreeAtom(ctx, type);
        return JS_DupValue(ctx, this_val);
    }
    uint32_t found = len;
    for (uint32_t i = 0; i < len; i++) {
        JSValue l = JS_GetPropertyUint32(ctx, list, i);
        bool matches = JS_IsStrictEqual(ctx, l, listener);
        if (!matches) {
            /* once() stores a wrapper, and off(original) has to find it. */
            JSValue original = JS_GetPropertyStr(ctx, l, "_original");
            matches = !JS_IsUndefined(original) && JS_IsStrictEqual(ctx, original, listener);
            JS_FreeValue(ctx, original);
        }
        JS_FreeValue(ctx, l);
        if (matches) { found = i; break; }
    }
    if (found == len) {
        JS_FreeValue(ctx, list); JS_FreeValue(ctx, events); JS_FreeAtom(ctx, type);
        return JS_DupValue(ctx, this_val);
    }
    for (uint32_t i = found + 1; i < len; i++)
        JS_SetPropertyUint32(ctx, list, i - 1, JS_GetPropertyUint32(ctx, list, i));
    uint32_t out_len = len - 1;
    JS_SetPropertyStr(ctx, list, "length", JS_NewUint32(ctx, out_len));
    JSValue out = JS_DupValue(ctx, list);
    if (out_len == 0) {
        JS_DeleteProperty(ctx, events, type, 0);
        JS_FreeValue(ctx, out);
    } else if (out_len == 1) {
        /* Keep the compact singleton representation after removing all but
           one listener, just as Node and tseep do on their hot path. */
        JSValue single = JS_GetPropertyUint32(ctx, out, 0);
        JS_SetProperty(ctx, events, type, single);
        JS_FreeValue(ctx, out);
    } else {
        JS_SetProperty(ctx, events, type, out);
    }
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, events);
    JS_FreeAtom(ctx, type);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_ee_remove_all_listeners(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    sxn_ee_gen++; /* listener set changes: invalidate the emit memo */
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
    bool events_borrowed;
    JSValue events = sxn_ee_events_for_emit(ctx, this_val, &events_borrowed);
    JSAtom type;
    JSValue list;
    bool type_owned = true, list_owned = true;
    bool singleton = false;
    /* Memo hit: same _events object, same event-name string object, no
       listener mutation since. Skips the atom conversion and the listener
       array lookup; see sxn_ee_memo_store. Only heap strings are keyed by
       identity -- anything else (a symbol, a computed string) takes the
       full path. */
    if (sxn_ee_memo_gen == sxn_ee_gen &&
        JS_VALUE_GET_TAG(argv[0]) == JS_TAG_STRING &&
        JS_VALUE_GET_PTR(argv[0]) == JS_VALUE_GET_PTR(sxn_ee_memo_typev) &&
        JS_VALUE_GET_TAG(events) == JS_TAG_OBJECT &&
        JS_VALUE_GET_PTR(events) == JS_VALUE_GET_PTR(sxn_ee_memo_events)) {
        /* The memo owns the atom for this entire native call. We only compare
           it before any callback (the unhandled-error case), so borrow it
           rather than taking another atom refcount pair on every hit. A
           singleton function is likewise not touched after its callback;
           arrays must remain rooted while callbacks can mutate the emitter. */
        type = sxn_ee_memo_atom;
        type_owned = false;
        singleton = sxn_ee_memo_singleton;
        if (singleton) {
            list = sxn_ee_memo_list;
            list_owned = false;
        } else {
            list = JS_DupValue(ctx, sxn_ee_memo_list);
            list_owned = true;
        }
    } else {
        type = JS_ValueToAtom(ctx, argv[0]);
        if (type == JS_ATOM_NULL) {
            if (!events_borrowed) JS_FreeValue(ctx, events);
            return JS_EXCEPTION;
        }
        list = JS_GetProperty(ctx, events, type);
        if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_STRING && JS_VALUE_GET_TAG(events) == JS_TAG_OBJECT)
            sxn_ee_memo_store(ctx, events, argv[0], type, list);
    }
    if (!events_borrowed) JS_FreeValue(ctx, events);
    if (singleton || JS_IsFunction(ctx, list)) {
        /* Single-listener representation borrowed from Node/tseep: no array
           lookup, element duplication, or per-iteration re-fetch is needed.
           Check this before probing the fast-array representation altogether. */
        int fast = argc == 2 ? JS_TryFastCapturedAddCall(ctx, list, argv[1]) : 0;
        JSValue ret = fast ? JS_UNDEFINED : JS_Call(ctx, list, this_val, argc - 1, argv + 1);
        if (list_owned) JS_FreeValue(ctx, list);
        if (fast < 0) return JS_EXCEPTION;
        if (JS_IsException(ret)) return ret;
        JS_FreeValue(ctx, ret);
        if (type_owned) JS_FreeAtom(ctx, type);
        return JS_TRUE;
    }
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
        if (list_owned) JS_FreeValue(ctx, list);
        bool is_error = (type == sxn_atom_error);
        if (type_owned) JS_FreeAtom(ctx, type);
        if (!is_error) return JS_NewBool(ctx, false);
        JSValue err;
        if (argc > 1) err = JS_DupValue(ctx, argv[1]);
        else {
            err = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "Unhandled error event"));
        }
        return JS_Throw(ctx, err);
    }
    if (type_owned) JS_FreeAtom(ctx, type);
    /* A listener removed while this loop is running must not disturb it --
       Node emits to the set of listeners that existed when emit started. off()
       reads this depth and copies the list instead of closing it up in place
       when it is not zero. */
    sxn_ee_emit_depth++;
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
        if (JS_IsException(ret)) {
            sxn_ee_emit_depth--;
            if (list_owned) JS_FreeValue(ctx, list);
            return ret;
        }
        JS_FreeValue(ctx, ret);
    }
    sxn_ee_emit_depth--;
    if (list_owned) JS_FreeValue(ctx, list);
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
    if (JS_IsFunction(ctx, list)) {
        /* listeners() exposes the original callback for once() wrappers. */
        JSValue original = JS_GetPropertyStr(ctx, list, "_original");
        if (JS_IsUndefined(original)) JS_SetPropertyUint32(ctx, out, 0, list);
        else { JS_FreeValue(ctx, list); JS_SetPropertyUint32(ctx, out, 0, original); }
        return out;
    }
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

/* Node's own rule, which a simpler "last dot wins" gets wrong for a name
   that is all dots: extname("..") is "", not ".". */
static char *sxn_posix_extname_core(const char *p) {
    size_t len = strlen(p);
    long start_dot = -1, start_part = 0, end = -1;
    bool matched_slash = true;
    int pre_dot = 0;
    for (long i = (long)len - 1; i >= 0; i--) {
        char c = p[i];
        if (c == '/') {
            if (!matched_slash) { start_part = i + 1; break; }
            continue;
        }
        if (end == -1) { matched_slash = false; end = i + 1; }
        if (c == '.') {
            if (start_dot == -1) start_dot = i;
            else if (pre_dot != 1) pre_dot = 1;
        } else if (start_dot != -1) {
            pre_dot = -1;
        }
    }
    if (start_dot == -1 || end == -1 || pre_dot == 0
        || (pre_dot == 1 && start_dot == end - 1 && start_dot == start_part + 1))
        return sxn_strndup("", 0);
    return sxn_strndup(p + start_dot, (size_t)(end - start_dot));
}

static JSValue js_path_posix_extname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : JS_ToCString(ctx, JS_UNDEFINED);
    if (!p) return JS_EXCEPTION;
    char *ext = sxn_posix_extname_core(p);
    JS_FreeCString(ctx, p);
    JSValue result = JS_NewString(ctx, ext);
    free(ext);
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
/* arcsx: node:zlib's one-shot compress and decompress. windowBits selects the
   container: 15 is zlib, 15+16 is gzip, and negative is raw with no header,
   which is exactly how the three pairs of Node functions differ. Streaming
   Gzip/Gunzip objects are built on these in node_compat.js, one call per
   chunk boundary being unnecessary because the whole payload is in memory. */
/* A stream that failed mid-run is not reusable: end it and let the next
   call build a fresh one. */
static void sxn_zlib_drop(bool *live, z_stream *zs, bool compress) {
    if (compress) deflateEnd(zs); else inflateEnd(zs);
    *live = false;
}

static JSValue sxn_zlib_run(JSContext *ctx, JSValueConst input,
                            int window_bits, int level, bool compress) {
    size_t in_len = 0;
    uint8_t *in = JS_GetUint8Array(ctx, &in_len, input);
    if (!in) {
        JSValue ab = JS_GetTypedArrayBuffer(ctx, input, NULL, NULL, NULL);
        if (JS_IsException(ab)) return JS_ThrowTypeError(ctx, "zlib expects bytes");
        JS_FreeValue(ctx, ab);
        return JS_ThrowTypeError(ctx, "zlib expects bytes");
    }

    /* deflateInit2 allocates the window and the hash tables -- a quarter of
       a megabyte at these settings -- and deflateEnd gives them straight
       back, so a run of small compressions spent most of its time in malloc.
       zlib's own answer is deflateReset, which keeps the state and the
       settings, so one stream per (direction, window, level) is kept here
       and reset instead. See spec/NODE.md for the measurement. */
    static struct {
        z_stream zs;
        int window_bits, level;
        bool live;
    } cache[2];
    int slot = compress ? 1 : 0;
    z_stream *cached = &cache[slot].zs;
    int rc;
    if (cache[slot].live && cache[slot].window_bits == window_bits &&
        (!compress || cache[slot].level == level)) {
        rc = compress ? deflateReset(cached) : inflateReset(cached);
    } else {
        if (cache[slot].live) {
            compress ? deflateEnd(cached) : inflateEnd(cached);
            cache[slot].live = false;
        }
        memset(cached, 0, sizeof(*cached));
        rc = compress
            ? deflateInit2(cached, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY)
            : inflateInit2(cached, window_bits);
        if (rc == Z_OK) {
            cache[slot].live = true;
            cache[slot].window_bits = window_bits;
            cache[slot].level = level;
        }
    }
    if (rc != Z_OK) return JS_ThrowInternalError(ctx, "zlib init failed: %d", rc);

    size_t cap = in_len < 1024 ? 1024 : in_len * 2;
    uint8_t *out = js_malloc(ctx, cap);
    if (!out) { sxn_zlib_drop(&cache[slot].live, cached, compress); return JS_EXCEPTION; }

    cached->next_in = in;
    cached->avail_in = (uInt)in_len;
    size_t produced = 0;
    for (;;) {
        if (produced == cap) {
            size_t ncap = cap * 2;
            uint8_t *grown = js_realloc(ctx, out, ncap);
            if (!grown) { js_free(ctx, out); sxn_zlib_drop(&cache[slot].live, cached, compress); return JS_EXCEPTION; }
            out = grown; cap = ncap;
        }
        cached->next_out = out + produced;
        cached->avail_out = (uInt)(cap - produced);
        rc = compress ? deflate(cached, Z_FINISH) : inflate(cached, Z_FINISH);
        produced = cap - cached->avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK || rc == Z_BUF_ERROR) {
            if (cached->avail_out == 0) continue;           /* needs more room */
            if (!compress && rc == Z_BUF_ERROR) break; /* truncated input */
            continue;
        }
        js_free(ctx, out);
        sxn_zlib_drop(&cache[slot].live, cached, compress);
        return JS_ThrowInternalError(ctx, "zlib %s failed: %d",
                                     compress ? "deflate" : "inflate", rc);
    }
    /* The stream stays; the next call resets it rather than rebuilding it. */
    JSValue result = JS_NewUint8ArrayCopy(ctx, out, produced);
    js_free(ctx, out);
    return result;
}

/* Streaming zlib, for CompressionStream/DecompressionStream and anything else
   that has to hand bytes over a chunk at a time. The one-shot path above
   keeps a reset stream for the whole call; this one keeps a z_stream per
   object for as long as the object lives, which is what a stream needs. */
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

static JSValue js_zlib_deflate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t bits = 15, level = Z_DEFAULT_COMPRESSION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "zlib needs input");
    if (argc > 1) JS_ToInt32(ctx, &bits, argv[1]);
    if (argc > 2 && !JS_IsUndefined(argv[2])) JS_ToInt32(ctx, &level, argv[2]);
    return sxn_zlib_run(ctx, argv[0], bits, level, true);
}

static JSValue js_zlib_inflate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t bits = 15;
    if (argc < 1) return JS_ThrowTypeError(ctx, "zlib needs input");
    if (argc > 1) JS_ToInt32(ctx, &bits, argv[1]);
    return sxn_zlib_run(ctx, argv[0], bits, 0, false);
}

static JSValue js_sxn_platform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
#if defined(__APPLE__)
    return JS_NewString(ctx, "darwin");
#elif defined(_WIN32)
    return JS_NewString(ctx, "win32");
#elif defined(__linux__)
    return JS_NewString(ctx, "linux");
#elif defined(__FreeBSD__)
    return JS_NewString(ctx, "freebsd");
#else
    return JS_NewString(ctx, "unknown");
#endif
}

static JSValue js_sxn_arch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
#if defined(__aarch64__) || defined(_M_ARM64)
    return JS_NewString(ctx, "arm64");
#elif defined(__x86_64__) || defined(_M_X64)
    return JS_NewString(ctx, "x64");
#elif defined(__i386__) || defined(_M_IX86)
    return JS_NewString(ctx, "ia32");
#else
    return JS_NewString(ctx, "unknown");
#endif
}

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

/* Encoding names, interned once; compared by identity on the Buffer
   fast paths below instead of via JS_ToCString + strcmp. */
static JSAtom sxn_atom_utf8, sxn_atom_utf8_dash, sxn_atom_hex;
static JSAtom sxn_atom_base64, sxn_atom_base64url, sxn_atom_toBase64;
static JSAtom sxn_atom_latin1, sxn_atom_binary, sxn_atom_ascii;
static JSAtom sxn_atom_ucs2, sxn_atom_ucs2_dash, sxn_atom_utf16le, sxn_atom_utf16le_dash;

/* Buffer.byteLength(value[, encoding]): how many bytes the value would
   occupy, without producing them. The utf-8 case is the whole point -- it is
   the idiomatic way to ask this question, and encoding the string just to read
   .length off the result is what it replaces. Binary-data arguments report
   their own size from their internal slot; only base64 has to inspect the
   input, and it is measured rather than decoded. */
static bool sxn_enc_is(const char *s, const char *lower) {
    /* Node matches encoding names case-insensitively. */
    size_t i;
    for (i = 0; lower[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return s[i] == '\0';
}

static JSValue js_buffer_byte_length(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                     int magic, JSValueConst *func_data) {
    (void)this_val; (void)magic; (void)func_data;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Buffer.byteLength requires a value");

    /* Binary data reports its own size, whatever the encoding. The brand is
       checked against the internal slot: an ordinary object that happens to
       carry a byteLength property is not binary data, and its getter must not
       run. */
    if (JS_VALUE_GET_TAG(argv[0]) != JS_TAG_STRING) {
        int64_t n = JS_BinaryDataByteLength(ctx, argv[0]);
        if (n >= 0) return JS_NewInt64(ctx, n);
        return JS_ThrowTypeError(ctx,
            "Buffer.byteLength expects a string, Buffer, TypedArray, DataView or ArrayBuffer");
    }

    int64_t slen = 0;
    if (JS_GetLength(ctx, argv[0], &slen) < 0) return JS_EXCEPTION;

    /* utf-8 is the default and by far the common case, so it is settled by
       atom identity before any string comparison happens. */
    JSAtom enc = JS_ATOM_NULL;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        enc = JS_ValueToAtom(ctx, argv[1]);
        if (enc == JS_ATOM_NULL) return JS_EXCEPTION;
    }
    if (enc == JS_ATOM_NULL || enc == sxn_atom_utf8 || enc == sxn_atom_utf8_dash) {
        JS_FreeAtom(ctx, enc);
        return JS_NewInt64(ctx, JS_Utf8ByteLength(ctx, argv[0]));
    }

    const char *name = JS_AtomToCString(ctx, enc);
    JS_FreeAtom(ctx, enc);
    if (!name) return JS_EXCEPTION;
    int64_t out;
    if (sxn_enc_is(name, "latin1") || sxn_enc_is(name, "binary") ||
        sxn_enc_is(name, "ascii")) {
        out = slen;
    } else if (sxn_enc_is(name, "ucs2") || sxn_enc_is(name, "ucs-2") ||
               sxn_enc_is(name, "utf16le") || sxn_enc_is(name, "utf-16le")) {
        out = slen * 2;
    } else if (sxn_enc_is(name, "hex")) {
        out = slen >> 1;
    } else if (sxn_enc_is(name, "base64") || sxn_enc_is(name, "base64url")) {
        /* Bytes the input would decode to, without decoding it. */
        const char *p = JS_ToCString(ctx, argv[0]);
        int64_t n = slen;
        if (!p) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        while (n > 0 && (p[n - 1] == '=' || p[n - 1] == '\n' || p[n - 1] == '\r'))
            n--;
        JS_FreeCString(ctx, p);
        out = n * 3 / 4;
    } else {
        out = JS_Utf8ByteLength(ctx, argv[0]);  /* Node: unknown means utf-8 */
    }
    JS_FreeCString(ctx, name);
    return JS_NewInt64(ctx, out);
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
        } else if (JS_VALUE_GET_TAG(argv[1]) == JS_TAG_STRING) {
            /* Exact-lowercase check only (matching node_compat.js's own fast
               path); "UTF-8" etc. falls through to the JS implementation's
               toLowerCase handling. Compared by atom identity rather than
               JS_ToCString + strcmp, so a literal encoding argument costs two
               pointer compares. */
            /* A literal encoding argument arrives as the atom's own string
               object, so identity settles it without interning; anything else
               still converts. */
            utf8 = JS_IsAtomString(ctx, argv[1], sxn_atom_utf8_dash) ||
                   JS_IsAtomString(ctx, argv[1], sxn_atom_utf8);
            if (!utf8) {
                JSAtom a = JS_ValueToAtom(ctx, argv[1]);
                if (a == JS_ATOM_NULL) return JS_EXCEPTION;
                utf8 = (a == sxn_atom_utf8_dash || a == sxn_atom_utf8);
                JS_FreeAtom(ctx, a);
            }
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
/* Buffer.prototype.toString fast path. The JS version dispatched on the
   encoding with a chain of `===` comparisons -- for toString("hex") that is
   three failed string compares before the match, each a full
   js_strict_eq2/js_strict_eq_slow round trip, which a profile put at ~7% of
   the Buffer benchmark. Resolving the encoding to an atom once and switching
   on identity replaces all of them with pointer compares.

   Only the encodings with a native implementation are handled here; latin1,
   ascii, mixed case and anything unknown fall through to the original JS
   method in func_data[1], which keeps its normalization and its TypeError. */
static JSValue js_buffer_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic, JSValueConst *func_data) {
    (void)magic;
    JSAtom enc;
    /* Same shortcut as Buffer.from: the literal "hex" or "utf-8" at the call
       site is the atom's string, so the common encodings never reach the
       atom table. */
    if (argc >= 1) {
        if (JS_IsAtomString(ctx, argv[0], sxn_atom_hex))
            return JS_Uint8ArrayToHex(ctx, this_val);
        if (JS_IsAtomString(ctx, argv[0], sxn_atom_utf8_dash) ||
            JS_IsAtomString(ctx, argv[0], sxn_atom_utf8))
            return JS_Call(ctx, func_data[0], JS_UNDEFINED, 1, &this_val);
    }
    if (argc < 1 || JS_IsUndefined(argv[0])) {
        enc = JS_DupAtom(ctx, sxn_atom_utf8_dash);
    } else if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_STRING) {
        enc = JS_ValueToAtom(ctx, argv[0]);
        if (enc == JS_ATOM_NULL) return JS_EXCEPTION;
    } else {
        return JS_Call(ctx, func_data[1], this_val, argc, argv);
    }
    JSValue ret;
    if (enc == sxn_atom_utf8_dash || enc == sxn_atom_utf8) {
        ret = JS_Call(ctx, func_data[0], JS_UNDEFINED, 1, &this_val); /* __sxnUtf8DecodeText */
    } else if (enc == sxn_atom_hex) {
        /* We have already performed Buffer's encoding dispatch. Calling the
           native primitive directly avoids a second property lookup and JS
           call frame for Uint8Array.prototype.toHex(). */
        ret = JS_Uint8ArrayToHex(ctx, this_val);
    } else if (enc == sxn_atom_base64) {
        ret = JS_Invoke(ctx, this_val, sxn_atom_toBase64, 0, NULL);
    } else {
        JS_FreeAtom(ctx, enc);
        return JS_Call(ctx, func_data[1], this_val, argc, argv);
    }
    JS_FreeAtom(ctx, enc);
    return ret;
}

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
    JS_SetPropertyStr(ctx, ctor, "byteLength", JS_NewCFunctionData(ctx, js_buffer_byte_length, 2, 0, 1, data));
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    JSValue orig_from = JS_GetPropertyStr(ctx, ctor, "from");
    if (!JS_IsUndefined(proto) && JS_IsFunction(ctx, orig_from)) {
        JSValueConst from_data[2] = { proto, orig_from };
        JS_SetPropertyStr(ctx, ctor, "from", JS_NewCFunctionData(ctx, js_buffer_from_fast, 2, 0, 2, from_data));
        /* Enable the `Buffer.from(s, "utf-8").length` fusion against the
           function just installed, so the call site's guard compares against
           this exact object. Anything that replaces Buffer.from, or moves
           `length` on the prototype chain, fails the guard and takes the
           ordinary path. */
        JSValue installed_from = JS_GetPropertyStr(ctx, ctor, "from");
        JS_EnableBufferLengthFusion(ctx, installed_from, proto);
        JS_FreeValue(ctx, installed_from);
        /* The same fusion for `encoder.encode(s).length`: TextEncoder's encode
           is bound straight to its C primitive, so the guard is that function's
           identity plus the typed-array length guard captured just above. */
        JSValue g2 = JS_GetGlobalObject(ctx);
        JSValue enc_fn = JS_GetPropertyStr(ctx, g2, "__sxnUtf8Encode");
        JS_EnableEncodeLengthFusion(ctx, enc_fn);
        JS_FreeValue(ctx, enc_fn);
        JS_FreeValue(ctx, g2);
    }
    /* Encoding-name atoms, used by both the Buffer.from fast path and the
       toString dispatch below, so they are interned before either is
       installed. */
    if (sxn_atom_utf8 == JS_ATOM_NULL) {
        sxn_atom_utf8 = JS_NewAtom(ctx, "utf8");
        sxn_atom_utf8_dash = JS_NewAtom(ctx, "utf-8");
        sxn_atom_hex = JS_NewAtom(ctx, "hex");
        sxn_atom_base64 = JS_NewAtom(ctx, "base64");
        sxn_atom_base64url = JS_NewAtom(ctx, "base64url");
        sxn_atom_toBase64 = JS_NewAtom(ctx, "toBase64");
        sxn_atom_latin1 = JS_NewAtom(ctx, "latin1");
        sxn_atom_binary = JS_NewAtom(ctx, "binary");
        sxn_atom_ascii = JS_NewAtom(ctx, "ascii");
        sxn_atom_ucs2 = JS_NewAtom(ctx, "ucs2");
        sxn_atom_ucs2_dash = JS_NewAtom(ctx, "ucs-2");
        sxn_atom_utf16le = JS_NewAtom(ctx, "utf16le");
        sxn_atom_utf16le_dash = JS_NewAtom(ctx, "utf-16le");
    }
    if (!JS_IsUndefined(proto)) {
        JSValue global2 = JS_GetGlobalObject(ctx);
        JSValue decode_text = JS_GetPropertyStr(ctx, global2, "__sxnUtf8DecodeText");
        JSValue orig_to_string = JS_GetPropertyStr(ctx, proto, "toString");
        if (JS_IsFunction(ctx, decode_text) && JS_IsFunction(ctx, orig_to_string)) {
            JSValueConst ts_data[2] = { decode_text, orig_to_string };
            JS_SetPropertyStr(ctx, proto, "toString",
                              JS_NewCFunctionData(ctx, js_buffer_to_string, 1, 0, 2, ts_data));
        }
        JS_FreeValue(ctx, decode_text);
        JS_FreeValue(ctx, orig_to_string);
        JS_FreeValue(ctx, global2);
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
    JS_SetModuleExport(ctx, m, "EventEmitter", JS_DupValue(ctx, ee));
    /* events.once / events.on are module-level helpers in Node, and are
       carried on the constructor here. */
    JS_SetModuleExport(ctx, m, "once", JS_GetPropertyStr(ctx, ee, "once"));
    JS_SetModuleExport(ctx, m, "on", JS_GetPropertyStr(ctx, ee, "on"));
    JS_FreeValue(ctx, ee);
    return 0;
}

static JSModuleDef *sxn_init_module_node_events(JSContext *ctx, const char *name) {
    JSModuleDef *m = JS_NewCModule(ctx, name, node_events_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "default");
    JS_AddModuleExport(ctx, m, "EventEmitter");
    JS_AddModuleExport(ctx, m, "once");
    JS_AddModuleExport(ctx, m, "on");
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

/* arcsx: the remaining builtins packages reach for. Each is a plain object
   built in node_compat.js and exposed on a global; the module wrapper just
   re-exports its own keys, so adding a function there needs no C change. */
static const char *node_util_names[] = {
    "inspect", "format", "promisify", "callbackify", "inherits", "deprecate",
    "isDeepStrictEqual", "types", "TextEncoder", "TextDecoder",
};
static const char *node_os_names[] = {
    "EOL", "platform", "arch", "type", "release", "version", "machine",
    "hostname", "tmpdir", "homedir", "endianness", "cpus",
    "availableParallelism", "networkInterfaces", "totalmem", "freemem",
    "loadavg", "uptime", "userInfo", "devNull", "constants",
};

/* ---------------- node:querystring, in C ----------------
   Pure string work with no state of its own, which is what makes it worth
   moving out of node_compat.js: every byte of a query string went through
   split(), a regexp for "+", and decodeURIComponent per part. */

static int sxn_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decoding with "+" for space, into a fresh buffer. A stray "%" is
   kept as it stands, which is what Node's lenient fallback does rather than
   throwing the way decodeURIComponent would. */
static char *sxn_qs_decode(const char *src, size_t len, size_t *out_len) {
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '+') { out[o++] = ' '; continue; }
        if (c == '%' && i + 2 < len) {
            int hi = sxn_hex_value((unsigned char)src[i + 1]);
            int lo = sxn_hex_value((unsigned char)src[i + 2]);
            if (hi >= 0 && lo >= 0) { out[o++] = (char)((hi << 4) | lo); i += 2; continue; }
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

/* The characters querystring.escape leaves alone, which are the same ones
   encodeURIComponent leaves alone. */
static bool sxn_qs_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*'
        || c == '\'' || c == '(' || c == ')';
}

/* A tiny growable string, so encoding does not build JS values per piece. */
typedef struct DynStr { char *data; size_t len, cap; } DynStr;

static void dynstr_need(DynStr *s, size_t extra) {
    if (s->len + extra + 1 <= s->cap) return;
    size_t cap = s->cap ? s->cap * 2 : 128;
    while (cap < s->len + extra + 1) cap *= 2;
    s->data = realloc(s->data, cap);
    s->cap = cap;
}

static void dynstr_add(DynStr *s, const char *src, size_t len) {
    dynstr_need(s, len);
    memcpy(s->data + s->len, src, len);
    s->len += len;
    s->data[s->len] = 0;
}

static void sxn_qs_encode_into(DynStr *out, const char *src, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (sxn_qs_unreserved(c)) {
            dynstr_add(out, (const char *)&c, 1);
        } else {
            char esc[3] = { '%', hex[c >> 4], hex[c & 15] };
            dynstr_add(out, esc, 3);
        }
    }
}

static JSValue js_qs_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    JSValue out = JS_NewObjectProto(ctx, JS_NULL);   /* Node hands back a bare object */
    if (argc < 1 || !JS_IsString(argv[0])) return out;
    size_t len = 0;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) { JS_FreeValue(ctx, out); return JS_EXCEPTION; }

    const char *sep = "&"; size_t sep_len = 1;
    const char *eq = "="; size_t eq_len = 1;
    const char *sep_owned = NULL, *eq_owned = NULL;
    if (argc > 1 && JS_IsString(argv[1])) { sep_owned = JS_ToCStringLen(ctx, &sep_len, argv[1]); if (sep_owned && sep_len) sep = sep_owned; else sep_len = 1; }
    if (argc > 2 && JS_IsString(argv[2])) { eq_owned = JS_ToCStringLen(ctx, &eq_len, argv[2]); if (eq_owned && eq_len) eq = eq_owned; else eq_len = 1; }
    /* Node stops at 1000 keys unless told otherwise, so a query string
       cannot be used to make an object with a million properties. */
    int64_t max_keys = 1000;
    if (argc > 3 && JS_IsObject(argv[3])) {
        JSValue limit = JS_GetPropertyStr(ctx, argv[3], "maxKeys");
        if (!JS_IsUndefined(limit)) JS_ToInt64(ctx, &max_keys, limit);
        JS_FreeValue(ctx, limit);
    }

    int64_t seen = 0;
    size_t i = 0;
    while (i <= len) {
        const char *part = str + i;
        const char *found = sep_len == 1 ? memchr(part, sep[0], len - i) : strstr(part, sep);
        size_t part_len = found ? (size_t)(found - part) : len - i;
        i += part_len + sep_len;
        if (part_len == 0) { if (!found) break; continue; }
        if (max_keys > 0 && seen >= max_keys) break;

        const char *split = eq_len == 1 ? memchr(part, eq[0], part_len) : NULL;
        if (!split && eq_len > 1) {
            for (size_t j = 0; j + eq_len <= part_len; j++)
                if (!memcmp(part + j, eq, eq_len)) { split = part + j; break; }
        }
        size_t key_len = split ? (size_t)(split - part) : part_len;
        size_t value_len = split ? part_len - key_len - eq_len : 0;
        size_t dk = 0, dv = 0;
        char *key = sxn_qs_decode(part, key_len, &dk);
        char *value = split ? sxn_qs_decode(split + eq_len, value_len, &dv) : sxn_qs_decode("", 0, &dv);
        if (!key || !value) { free(key); free(value); break; }
        seen++;

        JSAtom atom = JS_NewAtomLen(ctx, key, dk);
        JSValue existing = JS_GetProperty(ctx, out, atom);
        JSValue fresh = JS_NewStringLen(ctx, value, dv);
        if (JS_IsUndefined(existing)) {
            JS_SetProperty(ctx, out, atom, fresh);
        } else if (JS_IsArray(existing)) {
            uint32_t length = 0;
            JSValue size = JS_GetPropertyStr(ctx, existing, "length");
            JS_ToUint32(ctx, &length, size);
            JS_FreeValue(ctx, size);
            JS_SetPropertyUint32(ctx, existing, length, fresh);
            JS_SetProperty(ctx, out, atom, existing);
            existing = JS_UNDEFINED;
        } else {
            JSValue list = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, list, 0, existing);
            JS_SetPropertyUint32(ctx, list, 1, fresh);
            JS_SetProperty(ctx, out, atom, list);
            existing = JS_UNDEFINED;
        }
        JS_FreeValue(ctx, existing);
        JS_FreeAtom(ctx, atom);
        free(key);
        free(value);
        if (!found) break;
    }
    JS_FreeCString(ctx, str);
    if (sep_owned) JS_FreeCString(ctx, sep_owned);
    if (eq_owned) JS_FreeCString(ctx, eq_owned);
    return out;
}

/* One value of an object being stringified: a string, a number, a boolean or
   anything else, which Node writes as empty. */
static void sxn_qs_add_value(JSContext *ctx, DynStr *out, JSValueConst value) {
    int tag = JS_VALUE_GET_NORM_TAG(value);
    if (tag == JS_TAG_STRING || tag == JS_TAG_INT || tag == JS_TAG_FLOAT64 || tag == JS_TAG_BOOL) {
        size_t len = 0;
        const char *text = JS_ToCStringLen(ctx, &len, value);
        if (text) { sxn_qs_encode_into(out, text, len); JS_FreeCString(ctx, text); }
    }
}

static JSValue js_qs_stringify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_NewString(ctx, "");
    const char *sep = "&", *eq = "=";
    const char *sep_owned = NULL, *eq_owned = NULL;
    size_t sep_len = 1, eq_len = 1;
    if (argc > 1 && JS_IsString(argv[1])) { sep_owned = JS_ToCStringLen(ctx, &sep_len, argv[1]); if (sep_owned) sep = sep_owned; }
    if (argc > 2 && JS_IsString(argv[2])) { eq_owned = JS_ToCStringLen(ctx, &eq_len, argv[2]); if (eq_owned) eq = eq_owned; }

    JSPropertyEnum *props = NULL;
    uint32_t count = 0;
    DynStr out = {0};
    if (!JS_GetOwnPropertyNames(ctx, &props, &count, argv[0], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
        for (uint32_t i = 0; i < count; i++) {
            JSValue value = JS_GetProperty(ctx, argv[0], props[i].atom);
            size_t key_len = 0;
            JSValue key_value = JS_AtomToString(ctx, props[i].atom);
            const char *key = JS_ToCStringLen(ctx, &key_len, key_value);
            if (JS_IsArray(value)) {
                uint32_t length = 0;
                JSValue size = JS_GetPropertyStr(ctx, value, "length");
                JS_ToUint32(ctx, &length, size);
                JS_FreeValue(ctx, size);
                for (uint32_t j = 0; j < length; j++) {
                    if (out.len) dynstr_add(&out, sep, sep_len);
                    if (key) sxn_qs_encode_into(&out, key, key_len);
                    dynstr_add(&out, eq, eq_len);
                    JSValue one = JS_GetPropertyUint32(ctx, value, j);
                    sxn_qs_add_value(ctx, &out, one);
                    JS_FreeValue(ctx, one);
                }
            } else {
                if (out.len) dynstr_add(&out, sep, sep_len);
                if (key) sxn_qs_encode_into(&out, key, key_len);
                dynstr_add(&out, eq, eq_len);
                sxn_qs_add_value(ctx, &out, value);
            }
            if (key) JS_FreeCString(ctx, key);
            JS_FreeValue(ctx, key_value);
            JS_FreeValue(ctx, value);
        }
        JS_FreePropertyEnum(ctx, props, count);
    }
    if (sep_owned) JS_FreeCString(ctx, sep_owned);
    if (eq_owned) JS_FreeCString(ctx, eq_owned);
    JSValue result = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    free(out.data);
    return result;
}

static JSValue js_qs_escape(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "undefined");
    size_t len = 0;
    const char *text = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!text) return JS_EXCEPTION;
    DynStr out = {0};
    sxn_qs_encode_into(&out, text, len);
    JS_FreeCString(ctx, text);
    JSValue result = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    free(out.data);
    return result;
}

static JSValue js_qs_unescape(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "undefined");
    size_t len = 0;
    const char *text = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!text) return JS_EXCEPTION;
    size_t out_len = 0;
    char *decoded = sxn_qs_decode(text, len, &out_len);
    JS_FreeCString(ctx, text);
    if (!decoded) return JS_ThrowOutOfMemory(ctx);
    JSValue result = JS_NewStringLen(ctx, decoded, out_len);
    free(decoded);
    return result;
}


/* ---------------- node:path's win32 half, in C ----------------
   The last of path that was still JavaScript, and the last place in
   node_compat.js that reached for a regexp to walk a string. Same algorithm
   as the JS it replaces; separators are '\\' and '/', a root is a drive, a
   UNC share or a bare separator, and comparison is case-insensitive. */

static bool sxn_win_sep(char c) { return c == '\\' || c == '/'; }

typedef struct SxnWinRoot {
    size_t length;      /* how much of the path the root takes */
    char prefix[512];   /* what a normalized path starts with */
    char root_path[512];/* the root on its own, with a separator */
} SxnWinRoot;

static void sxn_win_root(const char *p, SxnWinRoot *root) {
    size_t len = strlen(p);
    root->length = 0;
    root->prefix[0] = 0;
    root->root_path[0] = 0;
    /* \\server\share */
    if (len >= 2 && sxn_win_sep(p[0]) && sxn_win_sep(p[1])) {
        size_t i = 2;
        while (i < len && sxn_win_sep(p[i])) i++;
        size_t server = i;
        while (i < len && !sxn_win_sep(p[i])) i++;
        if (i > server && i < len) {
            size_t after_server = i;
            while (i < len && sxn_win_sep(p[i])) i++;
            size_t share = i;
            while (i < len && !sxn_win_sep(p[i])) i++;
            if (i > share) {
                root->length = i;
                snprintf(root->prefix, sizeof(root->prefix), "\\\\%.*s\\%.*s\\",
                         (int)(after_server - server), p + server,
                         (int)(i - share), p + share);
                snprintf(root->root_path, sizeof(root->root_path), "%.*s", (int)i, p);
                return;
            }
        }
    }
    /* C:\ or C: */
    if (len >= 2 && ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':') {
        bool with_sep = len >= 3 && sxn_win_sep(p[2]);
        root->length = with_sep ? 3 : 2;
        snprintf(root->prefix, sizeof(root->prefix), "%c:%s", p[0], with_sep ? "\\" : "");
        snprintf(root->root_path, sizeof(root->root_path), "%c:\\", p[0]);
        return;
    }
    if (len >= 1 && sxn_win_sep(p[0])) {
        root->length = 1;
        snprintf(root->prefix, sizeof(root->prefix), "\\");
        snprintf(root->root_path, sizeof(root->root_path), "\\");
    }
}

static bool sxn_win_is_absolute(const char *p) {
    size_t len = strlen(p);
    if (len >= 2 && sxn_win_sep(p[0]) && sxn_win_sep(p[1])) return true;
    if (len >= 3 && ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z'))
        && p[1] == ':' && sxn_win_sep(p[2])) return true;
    if (len >= 1 && sxn_win_sep(p[0])) return true;
    return false;
}

static void sxn_win_reduce(SxnStrVec *out, const char *rest, bool is_abs) {
    const char *p = rest;
    while (*p) {
        const char *start = p;
        while (*p && !sxn_win_sep(*p)) p++;
        size_t len = (size_t)(p - start);
        if (len == 0 || (len == 1 && start[0] == '.')) {
            if (*p) p++;
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (out->len && strcmp(out->items[out->len - 1], "..") != 0) free(out->items[--out->len]);
            else if (!is_abs) sxn_strvec_push(out, sxn_strndup("..", 2));
        } else {
            sxn_strvec_push(out, sxn_strndup(start, len));
        }
        if (*p) p++;
    }
}

static char *sxn_win_normalize(const char *path) {
    if (!*path) return strdup(".");
    bool abs = sxn_win_is_absolute(path);
    SxnWinRoot root;
    sxn_win_root(path, &root);
    const char *rest = path + root.length;
    size_t rest_len = strlen(rest);
    bool trailing_sep = rest_len > 0 && sxn_win_sep(rest[rest_len - 1]);
    SxnStrVec segs = {0};
    sxn_win_reduce(&segs, rest, abs);

    size_t total = strlen(root.prefix) + 1;
    for (size_t i = 0; i < segs.len; i++) total += strlen(segs.items[i]) + 1;
    char *out = malloc(total + 2);
    strcpy(out, root.prefix);
    for (size_t i = 0; i < segs.len; i++) {
        if (i) strcat(out, "\\");
        strcat(out, segs.items[i]);
    }
    if (!*out) { free(out); out = strdup("."); }
    else if (trailing_sep && segs.len && out[strlen(out) - 1] != '\\') strcat(out, "\\");
    sxn_strvec_free(&segs);
    return out;
}

static char *sxn_win_join_core(const char *const *segs, int n) {
    size_t total = 1;
    for (int i = 0; i < n; i++) if (segs[i]) total += strlen(segs[i]) + 1;
    char *joined = malloc(total + 1);
    joined[0] = 0;
    bool any = false;
    for (int i = 0; i < n; i++) {
        if (!segs[i] || !*segs[i]) continue;
        if (any) strcat(joined, "\\");
        strcat(joined, segs[i]);
        any = true;
    }
    if (!any) { free(joined); return strdup("."); }
    char *out = sxn_win_normalize(joined);
    free(joined);
    return out;
}

static char *sxn_win_resolve_core(const char *const *segs, int n) {
    char *resolved = strdup("");
    bool absolute = false;
    for (int i = n - 1; i >= -1 && !absolute; i--) {
        char *seg = i >= 0 ? (segs[i] ? strdup(segs[i]) : strdup("")) : sxn_getcwd_alloc();
        if (!*seg) { free(seg); continue; }
        size_t len = strlen(seg) + 1 + strlen(resolved) + 1;
        char *next = malloc(len);
        snprintf(next, len, "%s\\%s", seg, resolved);
        free(resolved);
        resolved = next;
        absolute = sxn_win_is_absolute(seg);
        free(seg);
    }
    char *out;
    if (absolute) {
        out = sxn_win_normalize(resolved);
    } else {
        char *cwd = sxn_getcwd_alloc();
        size_t len = strlen(cwd) + 1 + strlen(resolved) + 1;
        char *combined = malloc(len);
        snprintf(combined, len, "%s\\%s", cwd, resolved);
        out = sxn_win_normalize(combined);
        free(combined);
        free(cwd);
    }
    free(resolved);
    SxnWinRoot root;
    sxn_win_root(out, &root);
    size_t out_len = strlen(out);
    if (out_len > root.length && sxn_win_sep(out[out_len - 1])) out[out_len - 1] = 0;
    return out;
}

/* basename, dirname and extname follow Node's own loops rather than a
   root-prefix model: on Windows the interesting cases -- a UNC share, a
   drive-relative path, a name that is all dots -- are exactly where a
   simpler model and Node disagree. */

/* Where the path proper starts: past a drive letter, if there is one. */
static size_t sxn_win_root_start(const char *p, size_t len) {
    if (len >= 2 && ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':')
        return 2;
    return 0;
}

static char *sxn_win_basename_core(const char *p, const char *suffix) {
    size_t len = strlen(p);
    size_t start = sxn_win_root_start(p, len);
    long end = -1;
    bool matched_slash = true;
    size_t begin = start;
    for (long i = (long)len - 1; i >= (long)start; i--) {
        if (sxn_win_sep(p[i])) {
            if (!matched_slash) { begin = (size_t)i + 1; break; }
        } else if (end == -1) {
            matched_slash = false;
            end = i + 1;
        }
    }
    if (end == -1) return sxn_strndup("", 0);
    size_t base_len = (size_t)end - begin;
    if (suffix) {
        size_t slen = strlen(suffix);
        if (base_len > slen && !memcmp(p + begin + base_len - slen, suffix, slen)) base_len -= slen;
    }
    return sxn_strndup(p + begin, base_len);
}

static char *sxn_win_dirname_core(const char *p) {
    size_t len = strlen(p);
    if (len == 0) return strdup(".");
    size_t root_end = 0;
    size_t offset = 0;
    if (len > 1 && sxn_win_sep(p[0])) {
        root_end = offset = 1;
        if (sxn_win_sep(p[1])) {
            /* \\server\share: the root runs to the end of the share name. */
            size_t j = 2, last = j;
            while (j < len && !sxn_win_sep(p[j])) j++;
            if (j < len && j != last) {
                last = j;
                while (j < len && sxn_win_sep(p[j])) j++;
                if (j < len && j != last) {
                    last = j;
                    while (j < len && !sxn_win_sep(p[j])) j++;
                    if (j == len) return sxn_strndup(p, len);
                    if (j != last) root_end = offset = j + 1;
                }
            }
        }
    } else if (sxn_win_root_start(p, len) == 2) {
        root_end = len > 2 && sxn_win_sep(p[2]) ? 3 : 2;
        offset = root_end;
    }
    long end = -1;
    bool matched_slash = true;
    for (long i = (long)len - 1; i >= (long)offset; i--) {
        if (sxn_win_sep(p[i])) {
            if (!matched_slash) { end = i; break; }
        } else {
            matched_slash = false;
        }
    }
    if (end == -1) {
        if (root_end == 0) return strdup(".");
        return sxn_strndup(p, root_end);
    }
    if (end == 0) return sxn_strndup(p, 1);
    return sxn_strndup(p, (size_t)end);
}

static char *sxn_win_extname_core(const char *p) {
    size_t len = strlen(p);
    size_t start = sxn_win_root_start(p, len);
    long start_dot = -1, start_part = (long)start, end = -1;
    bool matched_slash = true;
    /* 0 = nothing seen yet, 1 = only dots so far, -1 = a real character. */
    int pre_dot = 0;
    for (long i = (long)len - 1; i >= (long)start; i--) {
        char c = p[i];
        if (sxn_win_sep(c)) {
            if (!matched_slash) { start_part = i + 1; break; }
            continue;
        }
        if (end == -1) { matched_slash = false; end = i + 1; }
        if (c == '.') {
            if (start_dot == -1) start_dot = i;
            else if (pre_dot != 1) pre_dot = 1;
        } else if (start_dot != -1) {
            pre_dot = -1;
        }
    }
    if (start_dot == -1 || end == -1 || pre_dot == 0
        || (pre_dot == 1 && start_dot == end - 1 && start_dot == start_part + 1))
        return sxn_strndup("", 0);
    return sxn_strndup(p + start_dot, (size_t)(end - start_dot));
}

static JSValue js_path_win_normalize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!p) return JS_EXCEPTION;
    char *out = sxn_win_normalize(p);
    JS_FreeCString(ctx, p);
    JSValue result = JS_NewString(ctx, out);
    free(out);
    return result;
}

static JSValue js_path_win_is_absolute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!p) return JS_EXCEPTION;
    bool abs = sxn_win_is_absolute(p);
    JS_FreeCString(ctx, p);
    return JS_NewBool(ctx, abs);
}

static JSValue js_path_win_join(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return sxn_cstr_list_call(ctx, argc, argv, sxn_win_join_core);
}

static JSValue js_path_win_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    return sxn_cstr_list_call(ctx, argc, argv, sxn_win_resolve_core);
}

static JSValue js_path_win_dirname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!p) return JS_EXCEPTION;
    char *out = sxn_win_dirname_core(p);
    JS_FreeCString(ctx, p);
    JSValue result = JS_NewString(ctx, out);
    free(out);
    return result;
}

static JSValue js_path_win_basename(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!p) return JS_EXCEPTION;
    const char *suffix = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    char *base = sxn_win_basename_core(p, suffix);
    JS_FreeCString(ctx, p);
    if (suffix) JS_FreeCString(ctx, suffix);
    JSValue result = JS_NewString(ctx, base);
    free(base);
    return result;
}

static JSValue js_path_win_extname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *p = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!p) return JS_EXCEPTION;
    char *ext = sxn_win_extname_core(p);
    JS_FreeCString(ctx, p);
    JSValue result = JS_NewString(ctx, ext);
    free(ext);
    return result;
}

/* Case-insensitive, because Windows paths are. */
static int sxn_win_casecmp(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static JSValue js_path_win_relative(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *from_in = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *to_in = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    if (!from_in || !to_in) {
        if (from_in) JS_FreeCString(ctx, from_in);
        if (to_in) JS_FreeCString(ctx, to_in);
        return JS_EXCEPTION;
    }
    const char *one[1];
    one[0] = from_in; char *from = sxn_win_resolve_core(one, 1);
    one[0] = to_in;   char *to = sxn_win_resolve_core(one, 1);
    JS_FreeCString(ctx, from_in);
    JS_FreeCString(ctx, to_in);
    if (!strcmp(from, to)) { free(from); free(to); return JS_NewString(ctx, ""); }

    SxnStrVec fs_ = {0}, ts = {0};
    sxn_win_reduce(&fs_, from, false);
    sxn_win_reduce(&ts, to, false);
    size_t common = 0;
    while (common < fs_.len && common < ts.len && !sxn_win_casecmp(fs_.items[common], ts.items[common])) common++;

    DynStr out = {0};
    for (size_t i = common; i < fs_.len; i++) {
        if (out.len) dynstr_add(&out, "\\", 1);
        dynstr_add(&out, "..", 2);
    }
    for (size_t i = common; i < ts.len; i++) {
        if (out.len) dynstr_add(&out, "\\", 1);
        dynstr_add(&out, ts.items[i], strlen(ts.items[i]));
    }
    sxn_strvec_free(&fs_);
    sxn_strvec_free(&ts);
    free(from);
    free(to);
    JSValue result = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    free(out.data);
    return result;
}


/* net.isIP, from the system's own address parser. Express reaches for this
   on every request that carries X-Forwarded-For, and it was two regexps and
   a split per call. */
static JSValue js_net_is_ip(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NewInt32(ctx, 0);
    const char *text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    int family = 0;
    struct { uint8_t bytes[16]; } addr;
    if (uv_inet_pton(AF_INET, text, &addr) == 0) {
        family = 4;
    } else {
        /* A zone index (fe80::1%eth0) names an interface rather than part of
           the address; Node accepts one and so does this, but it has to name
           something -- and the address itself is never parsed with the "%"
           still in it, which some platforms would accept. */
        const char *percent = strchr(text, '%');
        if (!percent) {
            if (uv_inet_pton(AF_INET6, text, &addr) == 0) family = 6;
        } else if (percent != text && percent[1] != 0) {
            size_t len = (size_t)(percent - text);
            char *bare = malloc(len + 1);
            if (bare) {
                memcpy(bare, text, len);
                bare[len] = 0;
                if (uv_inet_pton(AF_INET6, bare, &addr) == 0) family = 6;
                free(bare);
            }
        }
    }
    JS_FreeCString(ctx, text);
    return JS_NewInt32(ctx, family);
}


/* ---------------- Buffer's numeric accessors, in C ----------------
   Node has about forty of these -- readUInt32BE, writeInt16LE and the rest.
   They are pure byte-to-number conversion, they are what binary protocol
   code spends its time in, and none of them existed here. One function
   covers them all: the magic argument carries the width, the sign, the
   endianness and whether it is a float. */

#define SXN_NUM_WIDTH(m)  ((m) & 0x0f)
#define SXN_NUM_SIGNED    0x10
#define SXN_NUM_BIG_END   0x20
#define SXN_NUM_FLOAT     0x40
#define SXN_NUM_BIG_INT   0x80

static bool sxn_num_range(JSContext *ctx, size_t buf_len, int64_t offset, int width) {
    if (offset < 0 || (uint64_t)offset + (uint64_t)width > (uint64_t)buf_len) {
        JS_ThrowRangeError(ctx, "the value of \"offset\" is out of range");
        return false;
    }
    return true;
}

static uint64_t sxn_read_raw(const uint8_t *p, int width, bool big_endian) {
    uint64_t v = 0;
    if (big_endian) for (int i = 0; i < width; i++) v = (v << 8) | p[i];
    else for (int i = width - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void sxn_write_raw(uint8_t *p, uint64_t v, int width, bool big_endian) {
    if (big_endian) for (int i = width - 1; i >= 0; i--) { p[i] = (uint8_t)(v & 0xff); v >>= 8; }
    else for (int i = 0; i < width; i++) { p[i] = (uint8_t)(v & 0xff); v >>= 8; }
}

static JSValue js_buffer_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    size_t len = 0;
    uint8_t *bytes = JS_GetUint8Array(ctx, &len, this_val);
    if (!bytes) return JS_ThrowTypeError(ctx, "not a Buffer");
    int width = SXN_NUM_WIDTH(magic);
    int64_t offset = 0;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && JS_ToInt64(ctx, &offset, argv[0])) return JS_EXCEPTION;
    if (!sxn_num_range(ctx, len, offset, width)) return JS_EXCEPTION;
    const uint8_t *p = bytes + offset;
    bool big = (magic & SXN_NUM_BIG_END) != 0;
    uint64_t raw = sxn_read_raw(p, width, big);
    if (magic & SXN_NUM_FLOAT) {
        if (width == 4) { float f; uint32_t bits = (uint32_t)raw; memcpy(&f, &bits, 4); return JS_NewFloat64(ctx, (double)f); }
        double d; memcpy(&d, &raw, 8); return JS_NewFloat64(ctx, d);
    }
    if (magic & SXN_NUM_BIG_INT) {
        if (magic & SXN_NUM_SIGNED) return JS_NewBigInt64(ctx, (int64_t)raw);
        return JS_NewBigUint64(ctx, raw);
    }
    if (magic & SXN_NUM_SIGNED) {
        int shift = 64 - width * 8;
        return JS_NewInt64(ctx, ((int64_t)(raw << shift)) >> shift);
    }
    return JS_NewInt64(ctx, (int64_t)raw);
}

static JSValue js_buffer_write_num(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    size_t len = 0;
    uint8_t *bytes = JS_GetUint8Array(ctx, &len, this_val);
    if (!bytes) return JS_ThrowTypeError(ctx, "not a Buffer");
    int width = SXN_NUM_WIDTH(magic);
    int64_t offset = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &offset, argv[1])) return JS_EXCEPTION;
    if (!sxn_num_range(ctx, len, offset, width)) return JS_EXCEPTION;
    uint8_t *p = bytes + offset;
    bool big = (magic & SXN_NUM_BIG_END) != 0;
    if (magic & SXN_NUM_FLOAT) {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
        if (width == 4) { float f = (float)d; uint32_t bits; memcpy(&bits, &f, 4); sxn_write_raw(p, bits, 4, big); }
        else { uint64_t bits; memcpy(&bits, &d, 8); sxn_write_raw(p, bits, 8, big); }
    } else if (magic & SXN_NUM_BIG_INT) {
        int64_t v = 0;
        if (JS_ToBigInt64(ctx, &v, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
        sxn_write_raw(p, (uint64_t)v, width, big);
    } else {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, argc > 0 ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
        sxn_write_raw(p, (uint64_t)(int64_t)d, width, big);
    }
    return JS_NewInt64(ctx, offset + width);
}

/* Buffer#copy(target, targetStart, sourceStart, sourceEnd) -> bytes copied,
   with the overlapping case handled the way Node's is. */
static JSValue js_buffer_copy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t src_len = 0, dst_len = 0;
    uint8_t *src = JS_GetUint8Array(ctx, &src_len, this_val);
    uint8_t *dst = argc > 0 ? JS_GetUint8Array(ctx, &dst_len, argv[0]) : NULL;
    if (!src || !dst) return JS_ThrowTypeError(ctx, "copy expects a Buffer target");
    int64_t target_start = 0, source_start = 0, source_end = (int64_t)src_len;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToInt64(ctx, &target_start, argv[1])) return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && JS_ToInt64(ctx, &source_start, argv[2])) return JS_EXCEPTION;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && JS_ToInt64(ctx, &source_end, argv[3])) return JS_EXCEPTION;
    if (target_start < 0 || source_start < 0 || source_end < source_start)
        return JS_ThrowRangeError(ctx, "index out of range");
    if (source_end > (int64_t)src_len) source_end = (int64_t)src_len;
    int64_t n = source_end - source_start;
    if (n > (int64_t)dst_len - target_start) n = (int64_t)dst_len - target_start;
    if (n <= 0) return JS_NewInt32(ctx, 0);
    memmove(dst + target_start, src + source_start, (size_t)n);
    return JS_NewInt64(ctx, n);
}


/* The variable-width accessors: readUIntBE(offset, byteLength) and its
   siblings take the width as an argument, 1 to 6 bytes, which is why they
   cannot share the fixed-width table above. */
static JSValue js_buffer_read_var(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    size_t len = 0;
    uint8_t *bytes = JS_GetUint8Array(ctx, &len, this_val);
    if (!bytes) return JS_ThrowTypeError(ctx, "not a Buffer");
    int64_t offset = 0, width = 0;
    if (argc > 0 && JS_ToInt64(ctx, &offset, argv[0])) return JS_EXCEPTION;
    if (argc > 1 && JS_ToInt64(ctx, &width, argv[1])) return JS_EXCEPTION;
    if (width < 1 || width > 6) return JS_ThrowRangeError(ctx, "byteLength must be between 1 and 6");
    if (!sxn_num_range(ctx, len, offset, (int)width)) return JS_EXCEPTION;
    uint64_t raw = sxn_read_raw(bytes + offset, (int)width, (magic & SXN_NUM_BIG_END) != 0);
    if (magic & SXN_NUM_SIGNED) {
        int shift = 64 - (int)width * 8;
        return JS_NewInt64(ctx, ((int64_t)(raw << shift)) >> shift);
    }
    return JS_NewInt64(ctx, (int64_t)raw);
}

static JSValue js_buffer_write_var(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    size_t len = 0;
    uint8_t *bytes = JS_GetUint8Array(ctx, &len, this_val);
    if (!bytes) return JS_ThrowTypeError(ctx, "not a Buffer");
    double value = 0;
    int64_t offset = 0, width = 0;
    if (argc > 0 && JS_ToFloat64(ctx, &value, argv[0])) return JS_EXCEPTION;
    if (argc > 1 && JS_ToInt64(ctx, &offset, argv[1])) return JS_EXCEPTION;
    if (argc > 2 && JS_ToInt64(ctx, &width, argv[2])) return JS_EXCEPTION;
    if (width < 1 || width > 6) return JS_ThrowRangeError(ctx, "byteLength must be between 1 and 6");
    if (!sxn_num_range(ctx, len, offset, (int)width)) return JS_EXCEPTION;
    sxn_write_raw(bytes + offset, (uint64_t)(int64_t)value, (int)width, (magic & SXN_NUM_BIG_END) != 0);
    return JS_NewInt64(ctx, offset + width);
}

/* swap16/swap32/swap64: reverse each group of bytes in place. */
static JSValue js_buffer_swap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)argc; (void)argv;
    size_t len = 0;
    uint8_t *bytes = JS_GetUint8Array(ctx, &len, this_val);
    if (!bytes) return JS_ThrowTypeError(ctx, "not a Buffer");
    size_t width = (size_t)magic;
    if (len % width) return JS_ThrowRangeError(ctx, "buffer size must be a multiple of %d", (int)width);
    for (size_t i = 0; i + width <= len; i += width)
        for (size_t a = 0, b = width - 1; a < b; a++, b--) {
            uint8_t t = bytes[i + a];
            bytes[i + a] = bytes[i + b];
            bytes[i + b] = t;
        }
    return JS_DupValue(ctx, this_val);
}


/* Buffer#write(string, offset, length, encoding): UTF-8 into the bytes that
   are already there, which is the shape a protocol writer wants and which
   this runtime did not have at all. */
static JSValue js_buffer_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t buf_len = 0;
    uint8_t *bytes = JS_GetUint8Array(ctx, &buf_len, this_val);
    if (!bytes) return JS_ThrowTypeError(ctx, "not a Buffer");
    if (argc < 1) return JS_NewInt32(ctx, 0);

    int64_t offset = 0, max = -1;
    const char *encoding = NULL;
    /* write(string), write(string, encoding), write(string, offset[, length][, encoding]) */
    int at = 1;
    if (at < argc && JS_IsString(argv[at])) {
        encoding = JS_ToCString(ctx, argv[at]);
        at++;
    } else {
        if (at < argc && !JS_IsUndefined(argv[at])) { if (JS_ToInt64(ctx, &offset, argv[at])) return JS_EXCEPTION; }
        at++;
        if (at < argc && !JS_IsUndefined(argv[at]) && !JS_IsString(argv[at])) { if (JS_ToInt64(ctx, &max, argv[at])) return JS_EXCEPTION; at++; }
        if (at < argc && JS_IsString(argv[at])) { encoding = JS_ToCString(ctx, argv[at]); at++; }
    }
    bool utf8 = !encoding || !strcmp(encoding, "utf8") || !strcmp(encoding, "utf-8");
    if (encoding) JS_FreeCString(ctx, encoding);
    if (!utf8) return JS_ThrowTypeError(ctx, "Buffer#write supports utf-8 only");
    if (offset < 0 || (uint64_t)offset > (uint64_t)buf_len)
        return JS_ThrowRangeError(ctx, "the value of \"offset\" is out of range");

    size_t text_len = 0;
    const char *text = JS_ToCStringLen(ctx, &text_len, argv[0]);
    if (!text) return JS_EXCEPTION;
    size_t room = buf_len - (size_t)offset;
    if (max >= 0 && (size_t)max < room) room = (size_t)max;
    size_t n = text_len < room ? text_len : room;
    /* Never leave half a character behind: back off to a boundary. */
    while (n > 0 && n < text_len && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
    memcpy(bytes + offset, text, n);
    JS_FreeCString(ctx, text);
    return JS_NewInt64(ctx, (int64_t)n);
}


/* Node's lenient hex and base64 readers, which every real payload takes:
   Uint8Array.fromHex and fromBase64 are strict and throw on the first
   character they do not like, and base64 as it actually travels -- PEM, MIME
   -- has newlines in it.

   Both are defined over UTF-16 code units: Node reads the string one unit at
   a time and masks it to a byte, which is why an emoji ends a base64 string.
   A C function is handed UTF-8 and cannot see that, so these return NULL for
   anything non-ASCII and the caller keeps the JavaScript loop for it. Every
   valid hex or base64 string is ASCII. */
static void sxn_free_plain_buffer(JSRuntime *rt, void *opaque, void *ptr) {
    (void)rt; (void)opaque;
    free(ptr);
}

static uint8_t *sxn_string_code_units(JSContext *ctx, JSValueConst val, size_t *out_count, bool wide);

static JSValue js_hex_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    size_t len = 0;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    uint8_t *out = malloc(len / 2 + 1);
    if (!out) { JS_FreeCString(ctx, str); return JS_ThrowOutOfMemory(ctx); }
    size_t n = 0;
    bool ascii = true;
    for (size_t i = 0; i + 1 < len; i += 2) {
        unsigned char a = (unsigned char)str[i], b = (unsigned char)str[i + 1];
        if ((a | b) & 0x80) { ascii = false; break; }
        int hi = sxn_hex_value(a), lo = sxn_hex_value(b);
        if (hi < 0 || lo < 0) break;          /* Node stops here rather than throwing */
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    JS_FreeCString(ctx, str);
    if (ascii) {
        /* The buffer goes to JavaScript as it stands rather than being
           copied into a fresh one. */
        return JS_NewUint8Array(ctx, out, n, sxn_free_plain_buffer, NULL, false);
    }
    /* Anything non-ASCII means Node's byte-at-a-time reading of the string
       is visible, so read the code units and mask each to a byte. */
    free(out);
    size_t count = 0;
    uint8_t *units = sxn_string_code_units(ctx, argv[0], &count, false);
    if (!units) return JS_EXCEPTION;
    uint8_t *bytes = malloc(count / 2 + 1);
    if (!bytes) { js_free(ctx, units); return JS_ThrowOutOfMemory(ctx); }
    n = 0;
    for (size_t i = 0; i + 1 < count; i += 2) {
        int hi = sxn_hex_value(units[i]), lo = sxn_hex_value(units[i + 1]);
        if (hi < 0 || lo < 0) break;
        bytes[n++] = (uint8_t)((hi << 4) | lo);
    }
    js_free(ctx, units);
    return JS_NewUint8Array(ctx, bytes, n, sxn_free_plain_buffer, NULL, false);
}

static JSValue js_base64_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    size_t len = 0;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    /* Both alphabets at once, which is what Node's reader accepts. */
    static int8_t table[256];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 256; i++) table[i] = -1;
        const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        for (int i = 0; a[i]; i++) table[(unsigned char)a[i]] = (int8_t)i;
        table[(unsigned char)'+'] = 62; table[(unsigned char)'/'] = 63;
        table[(unsigned char)'-'] = 62; table[(unsigned char)'_'] = 63;
        built = true;
    }
    uint8_t *out = malloc(len * 3 / 4 + 4);
    if (!out) { JS_FreeCString(ctx, str); return JS_ThrowOutOfMemory(ctx); }
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    bool ascii = true;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c & 0x80) { ascii = false; break; }
        if (c == '=') break;                  /* padding ends the data */
        int v = table[c];
        if (v < 0) continue;                  /* anything else is skipped */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[n++] = (uint8_t)((acc >> bits) & 0xff); }
    }
    JS_FreeCString(ctx, str);
    if (ascii) return JS_NewUint8Array(ctx, out, n, sxn_free_plain_buffer, NULL, false);
    /* Non-ASCII: Node reads the string a byte at a time, so a code unit
       above 0xff is truncated rather than skipped -- which is why an emoji
       ends a base64 string, its surrogate's low byte being '='. */
    free(out);
    size_t count = 0;
    uint8_t *units = sxn_string_code_units(ctx, argv[0], &count, false);
    if (!units) return JS_EXCEPTION;
    uint8_t *bytes = malloc(count * 3 / 4 + 4);
    if (!bytes) { js_free(ctx, units); return JS_ThrowOutOfMemory(ctx); }
    n = 0; acc = 0; bits = 0;
    for (size_t i = 0; i < count; i++) {
        uint8_t c = units[i];
        if (c == '=') break;
        int v = table[c];
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; bytes[n++] = (uint8_t)((acc >> bits) & 0xff); }
    }
    js_free(ctx, units);
    return JS_NewUint8Array(ctx, bytes, n, sxn_free_plain_buffer, NULL, false);
}


/* util.format, in C. The scan and the substitution are string work; only
   the two cases that need util.inspect -- %s of something that is not a
   string, and %o/%O -- call back into JavaScript, and the inspect function
   is handed in for that. %j is JSON, which the engine already has. */
static void sxn_format_append_value(JSContext *ctx, DynStr *out, JSValueConst value) {
    size_t len = 0;
    const char *text = JS_ToCStringLen(ctx, &len, value);
    if (text) { dynstr_add(out, text, len); JS_FreeCString(ctx, text); }
}

/* Calls the JavaScript inspect that was passed as the first argument. */
static void sxn_format_inspect(JSContext *ctx, DynStr *out, JSValueConst inspect, JSValueConst value, int depth) {
    JSValue args[2];
    args[0] = JS_DupValue(ctx, value);
    if (depth >= 0) {
        args[1] = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, args[1], "depth", JS_NewInt32(ctx, depth));
    } else {
        args[1] = JS_UNDEFINED;
    }
    JSValue text = JS_Call(ctx, inspect, JS_UNDEFINED, depth >= 0 ? 2 : 1, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    if (!JS_IsException(text)) sxn_format_append_value(ctx, out, text);
    JS_FreeValue(ctx, text);
}

static JSValue js_util_format(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");
    JSValueConst inspect = argv[0];
    int first = 1;
    DynStr out = {0};
    /* Without a string to substitute into, everything is inspected. */
    if (argc <= first || !JS_IsString(argv[first])) {
        for (int i = first; i < argc; i++) {
            if (i > first) dynstr_add(&out, " ", 1);
            sxn_format_inspect(ctx, &out, inspect, argv[i], -1);
        }
        JSValue result = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
        free(out.data);
        return result;
    }
    /* One argument: Node hands the string back untouched, "%%" included. */
    if (argc == first + 1) return JS_DupValue(ctx, argv[first]);

    size_t len = 0;
    const char *fmt = JS_ToCStringLen(ctx, &len, argv[first]);
    if (!fmt) return JS_EXCEPTION;
    int next = first + 1;
    size_t i = 0;
    while (i < len) {
        if (fmt[i] != '%' || i + 1 >= len) { dynstr_add(&out, fmt + i, 1); i++; continue; }
        char kind = fmt[i + 1];
        if (kind == '%') { dynstr_add(&out, "%", 1); i += 2; continue; }
        if (!strchr("sdifjoOc", kind) || next >= argc) { dynstr_add(&out, fmt + i, 1); i++; continue; }
        JSValueConst value = argv[next++];
        i += 2;
        switch (kind) {
        case 's':
            if (JS_IsString(value)) sxn_format_append_value(ctx, &out, value);
            else sxn_format_inspect(ctx, &out, inspect, value, -1);
            break;
        case 'd': case 'f': {
            if (JS_IsBigInt(value)) { sxn_format_append_value(ctx, &out, value); dynstr_add(&out, "n", 1); break; }
            double d = 0;
            if (JS_ToFloat64(ctx, &d, value)) { JS_FreeValue(ctx, JS_GetException(ctx)); d = NAN; }
            JSValue number = JS_NewFloat64(ctx, d);
            sxn_format_append_value(ctx, &out, number);
            JS_FreeValue(ctx, number);
            break;
        }
        case 'i': {
            if (JS_IsBigInt(value)) { sxn_format_append_value(ctx, &out, value); dynstr_add(&out, "n", 1); break; }
            double d = 0;
            if (JS_ToFloat64(ctx, &d, value)) { JS_FreeValue(ctx, JS_GetException(ctx)); d = NAN; }
            JSValue number = JS_NewFloat64(ctx, isnan(d) ? NAN : trunc(d));
            sxn_format_append_value(ctx, &out, number);
            JS_FreeValue(ctx, number);
            break;
        }
        case 'j': {
            JSValue json = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsException(json)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                dynstr_add(&out, "[Circular]", 10);
            } else if (JS_IsUndefined(json)) {
                dynstr_add(&out, "undefined", 9);
            } else {
                sxn_format_append_value(ctx, &out, json);
            }
            JS_FreeValue(ctx, json);
            break;
        }
        case 'o': case 'O':
            sxn_format_inspect(ctx, &out, inspect, value, 4);
            break;
        case 'c':
            break;   /* a CSS directive, which has nothing to say here */
        }
    }
    JS_FreeCString(ctx, fmt);
    /* Whatever is left over follows, separated by spaces. */
    for (; next < argc; next++) {
        dynstr_add(&out, " ", 1);
        if (JS_IsString(argv[next])) sxn_format_append_value(ctx, &out, argv[next]);
        else sxn_format_inspect(ctx, &out, inspect, argv[next], -1);
    }
    JSValue result = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    free(out.data);
    return result;
}



/* Buffer#toString for the encodings that are a straight widening of bytes
   into code units: latin1, Node's 7-bit "ascii", and utf16le. In JavaScript
   each of these was a String.fromCharCode appended in a loop, which builds a
   new string per byte; here the code units are filled in once and handed to
   the engine as a whole string. */
static JSValue js_buffer_decode_units(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val;
    size_t len = 0;
    uint8_t *bytes = argc > 0 ? JS_GetUint8Array(ctx, &len, argv[0]) : NULL;
    if (!bytes) return JS_EXCEPTION;
    size_t count = magic == 2 ? len / 2 : len;
    if (count == 0) return JS_NewStringLen(ctx, "", 0);
    uint16_t *units = js_malloc(ctx, count * sizeof(uint16_t));
    if (!units) return JS_EXCEPTION;
    if (magic == 2)
        for (size_t i = 0; i < count; i++) units[i] = (uint16_t)(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
    else {
        uint8_t mask = magic == 1 ? 0x7f : 0xff;   /* "ascii" drops the high bit */
        for (size_t i = 0; i < count; i++) units[i] = bytes[i] & mask;
    }
    JSValue str = JS_NewStringUTF16(ctx, units, count);
    js_free(ctx, units);
    return str;
}


/* A string's code units, each masked down to a byte -- what Node means by
   latin1, and what its hex and base64 readers see in a string with anything
   non-ASCII in it. The string is read as CESU-8, which encodes each
   surrogate on its own, so every code unit survives the trip. */
static uint8_t *sxn_string_code_units(JSContext *ctx, JSValueConst val, size_t *out_count, bool wide) {
    size_t len = 0;
    const char *str = JS_ToCStringLen2(ctx, &len, val, true);
    if (!str) return NULL;
    uint8_t *out = js_malloc(ctx, wide ? (len + 1) * 2 : len + 1);
    if (!out) { JS_FreeCString(ctx, str); return NULL; }
    size_t n = 0;
    for (size_t i = 0; i < len; ) {
        uint8_t c = (uint8_t)str[i];
        uint32_t unit;
        if (c < 0x80) { unit = c; i += 1; }
        else if ((c & 0xe0) == 0xc0 && i + 1 < len) { unit = ((c & 0x1fu) << 6) | ((uint8_t)str[i + 1] & 0x3fu); i += 2; }
        else if ((c & 0xf0) == 0xe0 && i + 2 < len) {
            unit = ((c & 0x0fu) << 12) | (((uint8_t)str[i + 1] & 0x3fu) << 6) | ((uint8_t)str[i + 2] & 0x3fu);
            i += 3;
        } else { unit = c; i += 1; }
        if (wide) { out[n++] = unit & 0xff; out[n++] = (unit >> 8) & 0xff; }
        else out[n++] = unit & 0xff;
    }
    JS_FreeCString(ctx, str);
    *out_count = n;
    return out;
}

/* The other direction for Buffer: a string into latin1 bytes (Node keeps the
   low byte of each code unit) or into utf16le. */
static JSValue js_buffer_encode_units(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val;
    if (argc < 1) return JS_EXCEPTION;
    size_t n = 0;
    uint8_t *out = sxn_string_code_units(ctx, argv[0], &n, magic != 0);
    if (!out) return JS_EXCEPTION;
    JSValue bytes = JS_NewUint8ArrayCopy(ctx, out, n);
    js_free(ctx, out);
    return bytes;
}

/* require() of a builtin. This was a 25-entry object literal in JavaScript,
   rebuilt on every single require() call before the name was even looked at;
   here the table is static and the answer is one property read. */
typedef struct { const char *name, *global, *sub; } SxnBuiltinEntry;
static const SxnBuiltinEntry sxn_builtin_table[] = {
    { "events", "__sxnEventEmitter", NULL },
    { "path", "__sxnPath", NULL },
    { "process", "process", NULL },
    { "fs", "__sxnFs", NULL },
    { "fs/promises", "__sxnFsPromises", NULL },
    { "util", "__sxnUtil", NULL },
    { "os", "__sxnOs", NULL },
    { "url", "__sxnUrl", NULL },
    { "querystring", "__sxnQuerystring", NULL },
    { "assert", "__sxnAssert", NULL },
    { "assert/strict", "__sxnAssert", NULL },
    { "stream", "__sxnStream", NULL },
    { "stream/promises", "__sxnStream", "promises" },
    { "http", "__sxnHttp", NULL },
    { "tty", "__sxnTty", NULL },
    { "string_decoder", "__sxnStringDecoder", NULL },
    { "timers", "__sxnTimers", NULL },
    { "timers/promises", "__sxnTimers", "promises" },
    { "perf_hooks", "__sxnPerfHooks", NULL },
    { "module", "__sxnModule", NULL },
    { "zlib", "__sxnZlib", NULL },
    { "crypto", "__sxnCrypto", NULL },
    { "net", "__sxnNet", NULL },
    { "child_process", "__sxnChildProcess", NULL },
    { "dns", "__sxnDns", NULL },
    { "dns/promises", "__sxnDnsPromises", NULL },
    { "https", "__sxnHttps", NULL },
    { "tls", "__sxnTls", NULL },
    { "http2", "__sxnHttp2", NULL },
    { "stream/web", "__sxnStreamWeb", NULL },
    { "vm", "__sxnVm", NULL },
    { "v8", "__sxnV8", NULL },
    { "worker_threads", "__sxnWorkerThreads", NULL },
    { "cluster", "__sxnCluster", NULL },
    { "readline", "__sxnReadline", NULL },
    { "readline/promises", "__sxnReadlinePromises", NULL },
    { "async_hooks", "__sxnAsyncHooks", NULL },
    { "inspector", "__sxnInspector", NULL },
    { "dgram", "__sxnDgram", NULL },
    { "console", "__sxnConsole", NULL },
    { "constants", "__sxnConstants", NULL },
    { "punycode", "__sxnPunycode", NULL },
    { "diagnostics_channel", "__sxnDiagnosticsChannel", NULL },
    { NULL, NULL, NULL },
};

/* module.builtinModules, read off the same table require() uses -- the list
   used to be written out by hand in JavaScript and had fallen behind it. */
static JSValue js_builtin_names(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue list = JS_NewArray(ctx);
    uint32_t n = 0;
    JS_SetPropertyUint32(ctx, list, n++, JS_NewString(ctx, "buffer"));
    for (const SxnBuiltinEntry *e = sxn_builtin_table; e->name; e++)
        JS_SetPropertyUint32(ctx, list, n++, JS_NewString(ctx, e->name));
    return list;
}

/* Returns JS_UNINITIALIZED for a name that is not a builtin, so the caller
   decides between throwing and answering false. */
static JSValue sxn_builtin_lookup(JSContext *ctx, JSValueConst spec) {
    const char *name = JS_ToCString(ctx, spec);
    if (!name) return JS_EXCEPTION;
    const char *bare = strncmp(name, "node:", 5) == 0 ? name + 5 : name;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue result = JS_UNINITIALIZED;
    if (!strcmp(bare, "buffer")) {
        /* node:buffer is the constructor under both names, built once. */
        result = JS_GetPropertyStr(ctx, global, "__sxnBufferModule");
        if (JS_IsUndefined(result)) {
            JSValue buffer = JS_GetPropertyStr(ctx, global, "Buffer");
            result = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, result, "Buffer", JS_DupValue(ctx, buffer));
            JS_SetPropertyStr(ctx, result, "default", buffer);
            JS_SetPropertyStr(ctx, global, "__sxnBufferModule", JS_DupValue(ctx, result));
        }
    } else {
        for (const SxnBuiltinEntry *e = sxn_builtin_table; e->name; e++) {
            if (strcmp(bare, e->name)) continue;
            result = JS_GetPropertyStr(ctx, global, e->global);
            if (e->sub && !JS_IsUndefined(result) && !JS_IsNull(result)) {
                JSValue outer = result;
                result = JS_GetPropertyStr(ctx, outer, e->sub);
                JS_FreeValue(ctx, outer);
            }
            if (JS_IsUndefined(result)) result = JS_NULL;   /* known, not loaded */
            break;
        }
    }
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, name);
    return result;
}

static JSValue js_builtin_require(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "require expects a specifier");
    JSValue mod = sxn_builtin_lookup(ctx, argv[0]);
    if (JS_IsException(mod)) return mod;
    if (JS_IsUninitialized(mod)) {
        const char *name = JS_ToCString(ctx, argv[0]);
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, name ? name : "?"));
        JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, "MODULE_NOT_FOUND"));
        if (name) {
            char msg[256];
            snprintf(msg, sizeof msg, "Cannot find module '%s'", name);
            JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg));
            JS_FreeCString(ctx, name);
        }
        return JS_Throw(ctx, err);
    }
    return mod;
}

static JSValue js_is_builtin(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewBool(ctx, false);
    JSValue mod = sxn_builtin_lookup(ctx, argv[0]);
    if (JS_IsException(mod)) return mod;
    bool known = !JS_IsUninitialized(mod);
    JS_FreeValue(ctx, mod);
    return JS_NewBool(ctx, known);
}


/* node:http lowercases every request header name and then flattens the
   result into rawHeaders, once per request. Both walks happen here in one
   pass over the property names. */
static JSValue js_http_headers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    JSValue headers = JS_NewObject(ctx);
    JSValue raw = JS_NewArray(ctx);
    if (JS_IsException(headers) || JS_IsException(raw)) goto fail;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSPropertyEnum *keys = NULL;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &keys, &count, argv[0], JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY))
            goto fail;
        uint32_t written = 0;
        for (uint32_t i = 0; i < count; i++) {
            JSValue value = JS_GetProperty(ctx, argv[0], keys[i].atom);
            const char *name = JS_AtomToCString(ctx, keys[i].atom);
            if (JS_IsException(value) || !name) {
                JS_FreeValue(ctx, value);
                if (name) JS_FreeCString(ctx, name);
                JS_FreePropertyEnum(ctx, keys, count);
                goto fail;
            }
            size_t len = strlen(name);
            char stack[64];
            char *lower = len < sizeof stack ? stack : js_malloc(ctx, len + 1);
            if (!lower) {
                JS_FreeValue(ctx, value);
                JS_FreeCString(ctx, name);
                JS_FreePropertyEnum(ctx, keys, count);
                goto fail;
            }
            for (size_t j = 0; j < len; j++) {
                char c = name[j];
                lower[j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            lower[len] = '\0';
            JS_SetPropertyStr(ctx, headers, lower, JS_DupValue(ctx, value));
            JS_SetPropertyUint32(ctx, raw, written++, JS_NewStringLen(ctx, lower, len));
            JS_SetPropertyUint32(ctx, raw, written++, value);
            if (lower != stack) js_free(ctx, lower);
            JS_FreeCString(ctx, name);
        }
        JS_FreePropertyEnum(ctx, keys, count);
    }
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "headers", headers);
    JS_SetPropertyStr(ctx, out, "rawHeaders", raw);
    return out;
 fail:
    JS_FreeValue(ctx, headers);
    JS_FreeValue(ctx, raw);
    return JS_EXCEPTION;
}


/* res.end() turns the chunks written to it into one body. The common cases
   -- nothing written, one string, all bytes -- are answered here; a mix of
   strings and bytes is handed back undefined for the JavaScript to join,
   because concatenating strings is the engine's own job. */
static JSValue js_join_chunks(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsArray(argv[0])) return JS_UNDEFINED;
    int64_t count = 0;
    if (JS_GetLength(ctx, argv[0], &count)) return JS_EXCEPTION;
    if (count == 0) return JS_NewStringLen(ctx, "", 0);
    if (count == 1) {
        JSValue only = JS_GetPropertyUint32(ctx, argv[0], 0);
        if (JS_IsString(only)) return only;
        JS_FreeValue(ctx, only);
    }
    /* All bytes: total the lengths, then copy each one in. */
    size_t total = 0;
    for (int64_t i = 0; i < count; i++) {
        JSValue chunk = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        size_t len = 0;
        uint8_t *bytes = JS_GetUint8Array(ctx, &len, chunk);
        JS_FreeValue(ctx, chunk);
        if (!bytes) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_UNDEFINED; }
        total += len;
    }
    uint8_t *out = js_malloc(ctx, total ? total : 1);
    if (!out) return JS_EXCEPTION;
    size_t at = 0;
    for (int64_t i = 0; i < count; i++) {
        JSValue chunk = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        size_t len = 0;
        uint8_t *bytes = JS_GetUint8Array(ctx, &len, chunk);
        if (bytes && len) memcpy(out + at, bytes, len);
        at += len;
        JS_FreeValue(ctx, chunk);
    }
    JSValue body = JS_NewUint8ArrayCopy(ctx, out, total);
    js_free(ctx, out);
    return body;
}


/* EventEmitter#once. The JavaScript version allocated a closure that had to
   name itself in order to remove itself; here the wrapper is a C function
   carrying the emitter, the event name and the listener, plus one small
   holder that is given the wrapper once it exists. */
static JSValue sxn_once_fire(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)magic;
    JSValue wrapper = JS_GetPropertyStr(ctx, data[3], "w");
    JSValue off = JS_GetPropertyStr(ctx, data[0], "off");
    JSValueConst off_args[2] = { data[1], wrapper };
    JS_FreeValue(ctx, JS_Call(ctx, off, data[0], 2, off_args));
    JS_FreeValue(ctx, off);
    JS_FreeValue(ctx, wrapper);
    return JS_Call(ctx, data[2], this_val, argc, argv);
}

static JSValue js_ee_once(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "once expects an event name and a listener");
    JSValue holder = JS_NewObjectProto(ctx, JS_NULL);
    if (JS_IsException(holder)) return holder;
    JSValue data[4] = { JS_DupValue(ctx, this_val), JS_DupValue(ctx, argv[0]),
                        JS_DupValue(ctx, argv[1]), holder };
    JSValue wrapper = JS_NewCFunctionData(ctx, sxn_once_fire, 0, 0, 4, data);
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, data[i]);
    if (JS_IsException(wrapper)) return wrapper;
    JS_SetPropertyStr(ctx, holder, "w", JS_DupValue(ctx, wrapper));
    /* Node exposes the original listener here, and removeListener(original)
       finds the wrapper through it. */
    JS_SetPropertyStr(ctx, wrapper, "_original", JS_DupValue(ctx, argv[1]));
    JSValue on = JS_GetPropertyStr(ctx, this_val, "on");
    JSValueConst on_args[2] = { argv[0], wrapper };
    JSValue result = JS_Call(ctx, on, this_val, 2, on_args);
    JS_FreeValue(ctx, on);
    JS_FreeValue(ctx, wrapper);
    return result;
}


/* Every node:http request carries a socket, and building it in JavaScript
   meant a fresh EventEmitter plus eleven properties -- three of them
   functions -- per request. The shape is the same every time, so the
   prototype is built once here and each request gets an object pointing at
   it. */
static JSValue sxn_socket_self(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)ctx; (void)argc; (void)argv;
    return JS_DupValue(ctx, this_val);
}

static JSValue sxn_socket_destroy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "destroyed", JS_TRUE);
    JS_SetPropertyStr(ctx, this_val, "readable", JS_FALSE);
    JS_SetPropertyStr(ctx, this_val, "writable", JS_FALSE);
    JSValue emit = JS_GetPropertyStr(ctx, this_val, "emit");
    JSValue name = JS_NewString(ctx, "close");
    JSValueConst args[1] = { name };
    JS_FreeValue(ctx, JS_Call(ctx, emit, this_val, 1, args));
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, emit);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_http_socket(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue proto = JS_GetPropertyStr(ctx, global, "__sxnSocketProto");
    if (JS_IsUndefined(proto)) {
        JSValue ee = JS_GetPropertyStr(ctx, global, "__sxnEventEmitter");
        JSValue ee_proto = JS_GetPropertyStr(ctx, ee, "prototype");
        JS_FreeValue(ctx, ee);
        proto = JS_NewObjectProto(ctx, ee_proto);
        JS_FreeValue(ctx, ee_proto);
        JS_SetPropertyStr(ctx, proto, "remoteAddress", JS_NewString(ctx, "127.0.0.1"));
        JS_SetPropertyStr(ctx, proto, "remotePort", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, proto, "localAddress", JS_NewString(ctx, "127.0.0.1"));
        JS_SetPropertyStr(ctx, proto, "encrypted", JS_FALSE);
        JS_SetPropertyStr(ctx, proto, "readable", JS_TRUE);
        JS_SetPropertyStr(ctx, proto, "writable", JS_TRUE);
        JS_SetPropertyStr(ctx, proto, "destroyed", JS_FALSE);
        JS_SetPropertyStr(ctx, proto, "setTimeout", JS_NewCFunction(ctx, sxn_socket_self, "setTimeout", 0));
        JS_SetPropertyStr(ctx, proto, "setNoDelay", JS_NewCFunction(ctx, sxn_socket_self, "setNoDelay", 0));
        JS_SetPropertyStr(ctx, proto, "setKeepAlive", JS_NewCFunction(ctx, sxn_socket_self, "setKeepAlive", 0));
        JS_SetPropertyStr(ctx, proto, "destroy", JS_NewCFunction(ctx, sxn_socket_destroy, "destroy", 0));
        JS_SetPropertyStr(ctx, proto, "end", JS_NewCFunction(ctx, sxn_socket_destroy, "end", 0));
        JS_SetPropertyStr(ctx, global, "__sxnSocketProto", JS_DupValue(ctx, proto));
    }
    JSValue socket = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, global);
    if (JS_IsException(socket)) return socket;
    JS_SetPropertyStr(ctx, socket, "_events", JS_NewObjectProto(ctx, JS_NULL));
    return socket;
}



/* A request's body is pushed on the first read, not before -- body-parser
   attaches its 'data' listener after the handler returns. That deferral was
   a closure built per request over `sent` and `body`; here it is one shared
   function reading the two fields off the request itself. */
static JSValue js_http_read_body(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JSValue sent = JS_GetPropertyStr(ctx, this_val, "_bodySent");
    bool already = JS_ToBool(ctx, sent);
    JS_FreeValue(ctx, sent);
    if (already) return JS_UNDEFINED;
    JS_SetPropertyStr(ctx, this_val, "_bodySent", JS_TRUE);
    JSValue body = JS_GetPropertyStr(ctx, this_val, "_rawBody");
    JSValue push = JS_GetPropertyStr(ctx, this_val, "push");
    bool empty = JS_IsUndefined(body) || JS_IsNull(body);
    if (!empty && JS_IsString(body)) {
        const char *text = JS_ToCString(ctx, body);
        empty = text && text[0] == '\0';
        if (text) JS_FreeCString(ctx, text);
    }
    if (!empty) {
        JSValueConst args[1] = { body };
        JS_FreeValue(ctx, JS_Call(ctx, push, this_val, 1, args));
    }
    JS_FreeValue(ctx, body);
    JSValueConst end[1] = { JS_NULL };
    JS_FreeValue(ctx, JS_Call(ctx, push, this_val, 1, end));
    JS_FreeValue(ctx, push);
    return JS_UNDEFINED;
}

/* The same for 'end' marking the request complete: one shared listener
   instead of an arrow function per request. */
static JSValue js_http_complete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JS_SetPropertyStr(ctx, this_val, "complete", JS_TRUE);
    return JS_UNDEFINED;
}


/* Every one of res.setHeader/getHeader/hasHeader/removeHeader lowercases the
   name it is given first. Scanning a short name in C beats
   String(name).toLowerCase(): 0.17 microseconds a call became 0.058. */
static JSValue js_header_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    if (argc < 1) return JS_DupValue(ctx, this_val);
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    size_t len = strlen(name);
    char stack[64];
    char *lower = len < sizeof stack ? stack : js_malloc(ctx, len + 1);
    if (!lower) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[len] = '\0';
    JSValue headers = JS_GetPropertyStr(ctx, this_val, "_headers");
    JSValue result;
    switch (magic) {
    case 0:   /* set */
        JS_SetPropertyStr(ctx, headers, lower, argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED);
        result = JS_DupValue(ctx, this_val);
        break;
    case 1:   /* get */
        result = JS_GetPropertyStr(ctx, headers, lower);
        break;
    case 2: { /* has: an own property, so a name like "constructor" is not one */
        JSAtom atom = JS_NewAtomLen(ctx, lower, len);
        int has = JS_GetOwnProperty(ctx, NULL, headers, atom);
        JS_FreeAtom(ctx, atom);
        if (has < 0) { JS_FreeValue(ctx, headers); if (lower != stack) js_free(ctx, lower); JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        result = JS_NewBool(ctx, has > 0);
        break;
    }
    default:  /* remove */
        {
            JSAtom atom = JS_NewAtomLen(ctx, lower, len);
            JS_DeleteProperty(ctx, headers, atom, 0);
            JS_FreeAtom(ctx, atom);
            result = JS_UNDEFINED;
        }
        break;
    }
    JS_FreeValue(ctx, headers);
    if (lower != stack) js_free(ctx, lower);
    JS_FreeCString(ctx, name);
    return result;
}



/* Buffer.concat, and the same walk that node:crypto and node:zlib each had
   a copy of: total the lengths, then copy each part in. Three JavaScript
   loops became one memcpy per part. */
static JSValue js_concat_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsArray(argv[0])) return JS_ThrowTypeError(ctx, "concat expects a list");
    int64_t count = 0;
    if (JS_GetLength(ctx, argv[0], &count)) return JS_EXCEPTION;
    size_t total = 0;
    for (int64_t i = 0; i < count; i++) {
        JSValue part = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        size_t len = 0;
        uint8_t *bytes = JS_GetUint8Array(ctx, &len, part);
        JS_FreeValue(ctx, part);
        if (!bytes) return JS_EXCEPTION;
        total += len;
    }
    /* Node's second argument is the length to produce: short parts leave
       zeroes behind them, long ones are cut off. An empty list is empty
       whatever length was asked for, which is Node's own answer. */
    if (count > 0 && argc > 1 && !JS_IsUndefined(argv[1])) {
        uint32_t wanted = 0;
        if (JS_ToUint32(ctx, &wanted, argv[1])) return JS_EXCEPTION;
        total = wanted;
    }
    uint8_t *out = js_mallocz(ctx, total ? total : 1);
    if (!out) return JS_EXCEPTION;
    size_t at = 0;
    for (int64_t i = 0; i < count && at < total; i++) {
        JSValue part = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        size_t len = 0;
        uint8_t *bytes = JS_GetUint8Array(ctx, &len, part);
        if (bytes) {
            if (len > total - at) len = total - at;
            memcpy(out + at, bytes, len);
            at += len;
        }
        JS_FreeValue(ctx, part);
    }
    JSValue result = JS_NewUint8ArrayCopy(ctx, out, total);
    js_free(ctx, out);
    return result;
}


/* The callback a Writable hands its _write. It was a JavaScript closure per
   chunk; here it is a C function carrying the stream and the caller's
   callback, if there was one. An error still reaches the stream's 'error'
   listeners whether or not anybody passed a callback. */
static JSValue sxn_write_done(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)this_val; (void)magic;
    JSValueConst err = argc > 0 ? argv[0] : JS_UNDEFINED;
    bool failed = !JS_IsUndefined(err) && !JS_IsNull(err);
    if (failed) {
        JSValue emit = JS_GetPropertyStr(ctx, data[0], "emit");
        JSValue name = JS_NewString(ctx, "error");
        JSValueConst args[2] = { name, err };
        JS_FreeValue(ctx, JS_Call(ctx, emit, data[0], 2, args));
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, emit);
    }
    if (JS_IsFunction(ctx, data[1])) {
        JSValueConst args[1] = { failed ? err : JS_NULL };
        JS_FreeValue(ctx, JS_Call(ctx, data[1], JS_UNDEFINED, 1, args));
    }
    return JS_UNDEFINED;
}

static JSValue js_write_callback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    JSValue data[2] = { JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED),
                        JS_DupValue(ctx, argc > 1 ? argv[1] : JS_UNDEFINED) };
    JSValue fn = JS_NewCFunctionData(ctx, sxn_write_done, 1, 0, 2, data);
    JS_FreeValue(ctx, data[0]);
    JS_FreeValue(ctx, data[1]);
    return fn;
}


/* url.fileURLToPath: strip the scheme and an empty "localhost" host, then
   percent-decode what is left. This was a startsWith, a regexp and
   decodeURIComponent per call. */
static int sxn_hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static JSValue js_file_url_to_path(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "must be a file: URL");
    size_t len = 0;
    const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;
    if (len < 7 || memcmp(str, "file://", 7) != 0) {
        JS_FreeCString(ctx, str);
        return JS_ThrowTypeError(ctx, "must be a file: URL");
    }
    const char *body = str + 7;
    size_t body_len = len - 7;
    if (body_len >= 9 && memcmp(body, "localhost", 9) == 0) { body += 9; body_len -= 9; }
    char *out = js_malloc(ctx, body_len + 2);
    if (!out) { JS_FreeCString(ctx, str); return JS_EXCEPTION; }
    size_t n = 0;
    for (size_t i = 0; i < body_len; i++) {
        if (body[i] == '%' && i + 2 < body_len) {
            int hi = sxn_hex_digit((unsigned char)body[i + 1]);
            int lo = sxn_hex_digit((unsigned char)body[i + 2]);
            if (hi >= 0 && lo >= 0) { out[n++] = (char)((hi << 4) | lo); i += 2; continue; }
        }
        out[n++] = body[i];
    }
    if (n == 0) out[n++] = '/';
    JSValue path = JS_NewStringLen(ctx, out, n);
    js_free(ctx, out);
    JS_FreeCString(ctx, str);
    return path;
}


/* The other direction: a path into the text of a file: URL. Node percent-
   encodes what a URL cannot carry literally and leaves the rest alone; this
   was encodeURI plus a regexp for '?' and '#' per call. */
static JSValue js_path_to_file_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "pathToFileURL expects a path");
    size_t len = 0;
    const char *path = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!path) return JS_EXCEPTION;
    char *out = js_malloc(ctx, len * 3 + 8);
    if (!out) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
    static const char *hex = "0123456789ABCDEF";
    size_t n = 0;
    memcpy(out, "file://", 7);
    n = 7;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        /* encodeURI's unreserved set, less '?' and '#', which Node escapes
           in a path because a URL would read them as query and fragment,
           and less the brackets, which a URL keeps for IPv6 hosts. */
        bool literal = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || strchr("-_.!~*'();/:@&=+$,", (char)c) != NULL;
        if (literal) out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 0xf]; }
    }
    JSValue text = JS_NewStringLen(ctx, out, n);
    js_free(ctx, out);
    JS_FreeCString(ctx, path);
    return text;
}


/* StringDecoder for utf-8: hand back everything up to the last complete
   character and keep the incomplete tail for the next chunk. This went
   through a TextDecoder with { stream: true }, which is the same idea at
   four times the cost. The tail is at most three bytes, so it lives on the
   decoder as a small array. */
static JSValue js_decode_chunk(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "decodeChunk expects a decoder and bytes");
    size_t len = 0;
    uint8_t *bytes = JS_GetUint8Array(ctx, &len, argv[1]);
    if (!bytes) return JS_EXCEPTION;
    /* Whatever was left over last time comes first. */
    uint8_t tail[3];
    size_t tail_len = 0;
    JSValue held = JS_GetPropertyStr(ctx, argv[0], "_tail");
    if (JS_IsObject(held)) {
        size_t held_len = 0;
        uint8_t *held_bytes = JS_GetUint8Array(ctx, &held_len, held);
        if (held_bytes && held_len <= sizeof tail) { memcpy(tail, held_bytes, held_len); tail_len = held_len; }
        else JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, held);

    size_t total = tail_len + len;
    uint8_t *all = js_malloc(ctx, total ? total : 1);
    if (!all) return JS_EXCEPTION;
    if (tail_len) memcpy(all, tail, tail_len);
    if (len) memcpy(all + tail_len, bytes, len);

    /* Walk back over at most three bytes looking for the start of a
       sequence that has not all arrived yet. */
    size_t complete = total;
    for (size_t back = 1; back <= 3 && back <= total; back++) {
        uint8_t c = all[total - back];
        if ((c & 0xc0) == 0x80) continue;            /* a continuation byte */
        size_t needed = (c & 0x80) == 0 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
        if (needed > back) complete = total - back;  /* short: hold it back */
        break;
    }
    JSValue text = JS_NewStringLen(ctx, (const char *)all, complete);
    if (complete < total) {
        JS_SetPropertyStr(ctx, argv[0], "_tail", JS_NewUint8ArrayCopy(ctx, all + complete, total - complete));
    } else {
        JS_SetPropertyStr(ctx, argv[0], "_tail", JS_NULL);
    }
    js_free(ctx, all);
    return text;
}


/* Measured against the JavaScript it would replace: res.getHeaders copies
   the header object, res.getHeaderNames lists its keys. */
static JSValue js_header_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)argc; (void)argv;
    JSValue headers = JS_GetPropertyStr(ctx, this_val, "_headers");
    JSPropertyEnum *keys = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &keys, &count, headers, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
        JS_FreeValue(ctx, headers);
        return JS_EXCEPTION;
    }
    JSValue out = magic ? JS_NewArray(ctx) : JS_NewObject(ctx);
    for (uint32_t i = 0; i < count; i++) {
        if (magic) JS_SetPropertyUint32(ctx, out, i, JS_AtomToString(ctx, keys[i].atom));
        else JS_SetProperty(ctx, out, keys[i].atom, JS_GetProperty(ctx, headers, keys[i].atom));
    }
    JS_FreePropertyEnum(ctx, keys, count);
    JS_FreeValue(ctx, headers);
    return out;
}


#define SXN_TICK_ARRAY 4   /* magic is stored unsigned, so this is not -1 */

/* process.nextTick: the JavaScript version copied `arguments` into an array
   and built a closure over it for every tick. The C one carries the
   function and up to three arguments directly. */
static JSValue sxn_tick_run(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)this_val; (void)argc; (void)argv;
    if (magic == SXN_TICK_ARRAY) {
        /* More than three arguments: they were kept as an array. */
        int64_t count = 0;
        if (JS_GetLength(ctx, data[1], &count)) return JS_EXCEPTION;
        JSValue *args = js_malloc(ctx, sizeof(JSValue) * (count ? (size_t)count : 1));
        if (!args) return JS_EXCEPTION;
        for (int64_t i = 0; i < count; i++) args[i] = JS_GetPropertyUint32(ctx, data[1], (uint32_t)i);
        JSValue out = JS_Call(ctx, data[0], JS_UNDEFINED, (int)count, (JSValueConst *)args);
        for (int64_t i = 0; i < count; i++) JS_FreeValue(ctx, args[i]);
        js_free(ctx, args);
        return out;
    }
    JSValueConst args[3] = { data[1], data[2], data[3] };
    return JS_Call(ctx, data[0], JS_UNDEFINED, magic, args);
}

static JSValue js_next_tick(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "nextTick expects a function");
    int extra = argc - 1;
    JSValue data[4] = { JS_DupValue(ctx, argv[0]), JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED };
    int magic = extra;
    if (extra > 3) {
        /* Rare enough to keep in an array rather than widening the closure. */
        JSValue list = JS_NewArray(ctx);
        for (int i = 0; i < extra; i++)
            JS_SetPropertyUint32(ctx, list, (uint32_t)i, JS_DupValue(ctx, argv[i + 1]));
        data[1] = list;
        magic = SXN_TICK_ARRAY;
    } else {
        for (int i = 0; i < extra; i++) data[i + 1] = JS_DupValue(ctx, argv[i + 1]);
    }
    JSValue job = JS_NewCFunctionData(ctx, sxn_tick_run, 0, magic, 4, data);
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, data[i]);
    if (JS_IsException(job)) return job;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue enqueue = JS_GetPropertyStr(ctx, global, "queueMicrotask");
    JSValueConst call_args[1] = { job };
    JSValue result = JS_Call(ctx, enqueue, global, 1, call_args);
    JS_FreeValue(ctx, enqueue);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, job);
    if (JS_IsException(result)) return result;
    JS_FreeValue(ctx, result);
    return JS_UNDEFINED;
}


/* util.inspect. The JavaScript version built an options object per level, a
   mapped array per container and a joined string per level; this walks the
   value once into one buffer. What it prints is unchanged, down to the
   quoting of keys that are not identifiers. */
typedef struct SxnSeen { JSValueConst value; struct SxnSeen *prev; } SxnSeen;

static void sxn_inspect_value(JSContext *ctx, DynStr *out, JSValueConst v, int depth, int max, SxnSeen *seen);

static void sxn_inspect_str(JSContext *ctx, DynStr *out, JSValueConst v) {
    size_t len = 0;
    const char *text = JS_ToCStringLen(ctx, &len, v);
    if (!text) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    dynstr_add(out, text, len);
    JS_FreeCString(ctx, text);
}

/* A string prints as JSON does inside a container, which is also how Node
   quotes it -- except that Node uses single quotes. */
static void sxn_inspect_quoted(JSContext *ctx, DynStr *out, JSValueConst v) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue stringify = JS_GetPropertyStr(ctx, json, "stringify");
    JSValueConst args[1] = { v };
    JSValue text = JS_Call(ctx, stringify, json, 1, args);
    JS_FreeValue(ctx, stringify);
    JS_FreeValue(ctx, json);
    JS_FreeValue(ctx, global);
    if (!JS_IsException(text)) sxn_inspect_str(ctx, out, text);
    else JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, text);
}

static bool sxn_is_identifier(const char *name, size_t len) {
    if (len == 0) return false;
    char c = name[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$')) return false;
    for (size_t i = 1; i < len; i++) {
        c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '$'))
            return false;
    }
    return true;
}

static void sxn_inspect_call(JSContext *ctx, DynStr *out, JSValueConst v, const char *method) {
    JSValue fn = JS_GetPropertyStr(ctx, v, method);
    JSValue text = JS_Call(ctx, fn, v, 0, NULL);
    JS_FreeValue(ctx, fn);
    if (!JS_IsException(text)) sxn_inspect_str(ctx, out, text);
    else JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, text);
}

static void sxn_inspect_entries(JSContext *ctx, DynStr *out, JSValueConst v, bool is_map,
                                int depth, int max, SxnSeen *seen) {
    JSValue fn = JS_GetPropertyStr(ctx, v, is_map ? "entries" : "values");
    JSValue iterator = JS_Call(ctx, fn, v, 0, NULL);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(iterator)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    JSValue next = JS_GetPropertyStr(ctx, iterator, "next");
    bool first = true;
    for (;;) {
        JSValue step = JS_Call(ctx, next, iterator, 0, NULL);
        if (JS_IsException(step)) { JS_FreeValue(ctx, JS_GetException(ctx)); break; }
        JSValue done = JS_GetPropertyStr(ctx, step, "done");
        bool finished = JS_ToBool(ctx, done);
        JS_FreeValue(ctx, done);
        if (finished) { JS_FreeValue(ctx, step); break; }
        JSValue entry = JS_GetPropertyStr(ctx, step, "value");
        JS_FreeValue(ctx, step);
        if (!first) dynstr_add(out, ", ", 2);
        first = false;
        if (is_map) {
            JSValue key = JS_GetPropertyUint32(ctx, entry, 0);
            JSValue val = JS_GetPropertyUint32(ctx, entry, 1);
            sxn_inspect_value(ctx, out, key, depth + 1, max, seen);
            dynstr_add(out, " => ", 4);
            sxn_inspect_value(ctx, out, val, depth + 1, max, seen);
            JS_FreeValue(ctx, key);
            JS_FreeValue(ctx, val);
        } else {
            sxn_inspect_value(ctx, out, entry, depth + 1, max, seen);
        }
        JS_FreeValue(ctx, entry);
    }
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, iterator);
}

static void sxn_inspect_value(JSContext *ctx, DynStr *out, JSValueConst v, int depth, int max, SxnSeen *seen) {
    if (JS_IsNull(v)) { dynstr_add(out, "null", 4); return; }
    if (JS_IsUndefined(v)) { dynstr_add(out, "undefined", 9); return; }
    if (JS_IsString(v)) {
        if (depth == 0) sxn_inspect_str(ctx, out, v);
        else sxn_inspect_quoted(ctx, out, v);
        return;
    }
    if (JS_IsBool(v) || JS_IsNumber(v)) { sxn_inspect_str(ctx, out, v); return; }
    if (JS_IsBigInt(v)) { sxn_inspect_str(ctx, out, v); dynstr_add(out, "n", 1); return; }
    if (JS_IsSymbol(v)) {
        /* A symbol refuses to become a string implicitly, so call its own
           toString the way the JavaScript version did. */
        JSValue fn = JS_GetPropertyStr(ctx, v, "toString");
        JSValue text = JS_Call(ctx, fn, v, 0, NULL);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(text)) { JS_FreeValue(ctx, JS_GetException(ctx)); dynstr_add(out, "Symbol()", 8); }
        else sxn_inspect_str(ctx, out, text);
        JS_FreeValue(ctx, text);
        return;
    }
    if (JS_IsFunction(ctx, v)) {
        JSValue name = JS_GetPropertyStr(ctx, v, "name");
        const char *text = JS_IsString(name) ? JS_ToCString(ctx, name) : NULL;
        dynstr_add(out, "[Function: ", 11);
        if (text && text[0]) dynstr_add(out, text, strlen(text));
        else dynstr_add(out, "anonymous", 9);
        dynstr_add(out, "]", 1);
        if (text) JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, name);
        return;
    }
    if (!JS_IsObject(v)) { sxn_inspect_str(ctx, out, v); return; }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor_error = JS_GetPropertyStr(ctx, global, "Error");
    bool is_error = JS_IsInstanceOf(ctx, v, ctor_error) > 0;
    JS_FreeValue(ctx, ctor_error);
    JS_FreeValue(ctx, global);
    if (is_error) {
        /* This engine's stack is the frames alone, with no "Error: message"
           line at the top of it, so that line is written here -- which is
           what Node prints and what the JavaScript version left out. */
        JSValue name = JS_GetPropertyStr(ctx, v, "name");
        JSValue message = JS_GetPropertyStr(ctx, v, "message");
        sxn_inspect_str(ctx, out, name);
        size_t message_len = 0;
        const char *message_text = JS_ToCStringLen(ctx, &message_len, message);
        if (message_text && message_len) {
            dynstr_add(out, ": ", 2);
            dynstr_add(out, message_text, message_len);
        }
        if (message_text) JS_FreeCString(ctx, message_text);
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, message);
        JSValue stack = JS_GetPropertyStr(ctx, v, "stack");
        size_t stack_len = 0;
        const char *stack_text = JS_IsString(stack) ? JS_ToCStringLen(ctx, &stack_len, stack) : NULL;
        if (stack_text && stack_len) {
            if (stack_text[0] != '\n') dynstr_add(out, "\n", 1);
            dynstr_add(out, stack_text, stack_len);
            while (out->len && out->data[out->len - 1] == '\n') out->len--;
        }
        if (stack_text) JS_FreeCString(ctx, stack_text);
        JS_FreeValue(ctx, stack);
        return;
    }

    static JSClassID date_id, regexp_id, map_id, set_id;
    if (!date_id) {
        static const char *probe_src = "[new Date(), /x/, new Map(), new Set()]";
        JSValue probe = JS_Eval(ctx, probe_src, strlen(probe_src), "<inspect>", JS_EVAL_TYPE_GLOBAL);
        JSValue item;
        item = JS_GetPropertyUint32(ctx, probe, 0); date_id = JS_GetClassID(item); JS_FreeValue(ctx, item);
        item = JS_GetPropertyUint32(ctx, probe, 1); regexp_id = JS_GetClassID(item); JS_FreeValue(ctx, item);
        item = JS_GetPropertyUint32(ctx, probe, 2); map_id = JS_GetClassID(item); JS_FreeValue(ctx, item);
        item = JS_GetPropertyUint32(ctx, probe, 3); set_id = JS_GetClassID(item); JS_FreeValue(ctx, item);
        JS_FreeValue(ctx, probe);
    }
    JSClassID cls = JS_GetClassID(v);
    if (cls == date_id) {
        /* toISOString throws on an invalid date, which is what the
           JavaScript version did to whoever printed one. Node prints
           "Invalid Date" instead, and so does this. */
        JSValue fn = JS_GetPropertyStr(ctx, v, "toISOString");
        JSValue text = JS_Call(ctx, fn, v, 0, NULL);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(text)) { JS_FreeValue(ctx, JS_GetException(ctx)); dynstr_add(out, "Invalid Date", 12); }
        else sxn_inspect_str(ctx, out, text);
        JS_FreeValue(ctx, text);
        return;
    }
    if (cls == regexp_id) { sxn_inspect_call(ctx, out, v, "toString"); return; }

    for (SxnSeen *p = seen; p; p = p->prev)
        if (JS_IsStrictEqual(ctx, p->value, v)) { dynstr_add(out, "[Circular *1]", 13); return; }
    bool is_array = JS_IsArray(v);
    if (depth > max) {
        if (is_array) dynstr_add(out, "[Array]", 7);
        else dynstr_add(out, "[Object]", 8);
        return;
    }
    SxnSeen here = { v, seen };

    if (is_array) {
        int64_t count = 0;
        JS_GetLength(ctx, v, &count);
        if (count == 0) { dynstr_add(out, "[]", 2); return; }
        dynstr_add(out, "[ ", 2);
        for (int64_t i = 0; i < count; i++) {
            if (i) dynstr_add(out, ", ", 2);
            JSValue item = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            sxn_inspect_value(ctx, out, item, depth + 1, max, &here);
            JS_FreeValue(ctx, item);
        }
        dynstr_add(out, " ]", 2);
        return;
    }
    if (cls == map_id || cls == set_id) {
        bool is_map = cls == map_id;
        JSValue size = JS_GetPropertyStr(ctx, v, "size");
        int32_t count = 0;
        JS_ToInt32(ctx, &count, size);
        JS_FreeValue(ctx, size);
        char head[32];
        int head_len = snprintf(head, sizeof head, "%s(%d) {", is_map ? "Map" : "Set", count);
        dynstr_add(out, head, (size_t)head_len);
        if (count) {
            dynstr_add(out, " ", 1);
            sxn_inspect_entries(ctx, out, v, is_map, depth, max, &here);
            dynstr_add(out, " ", 1);
        }
        dynstr_add(out, "}", 1);
        return;
    }
    size_t ta_offset = 0, ta_len = 0, ta_element = 0;
    JSValue ta_buffer = JS_GetTypedArrayBuffer(ctx, v, &ta_offset, &ta_len, &ta_element);
    if (!JS_IsException(ta_buffer)) {
        JS_FreeValue(ctx, ta_buffer);
        /* A typed array prints its own class name and its elements. */
        JSValue ctor = JS_GetPropertyStr(ctx, v, "constructor");
        JSValue name = JS_GetPropertyStr(ctx, ctor, "name");
        JS_FreeValue(ctx, ctor);
        int64_t count = 0;
        JS_GetLength(ctx, v, &count);
        sxn_inspect_str(ctx, out, name);
        JS_FreeValue(ctx, name);
        char head[32];
        int head_len = snprintf(head, sizeof head, "(%lld) [ ", (long long)count);
        dynstr_add(out, head, (size_t)head_len);
        for (int64_t i = 0; i < count; i++) {
            if (i) dynstr_add(out, ", ", 2);
            JSValue item = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
            sxn_inspect_str(ctx, out, item);
            JS_FreeValue(ctx, item);
        }
        dynstr_add(out, " ]", 2);
        return;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));   /* not a typed array, then */

    JSPropertyEnum *keys = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &keys, &count, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        dynstr_add(out, "{}", 2);
        return;
    }
    if (count == 0) { JS_FreePropertyEnum(ctx, keys, count); dynstr_add(out, "{}", 2); return; }
    dynstr_add(out, "{ ", 2);
    for (uint32_t i = 0; i < count; i++) {
        if (i) dynstr_add(out, ", ", 2);
        JSValue key = JS_AtomToString(ctx, keys[i].atom);
        size_t name_len = 0;
        const char *name = JS_ToCStringLen(ctx, &name_len, key);
        if (name && sxn_is_identifier(name, name_len)) dynstr_add(out, name, name_len);
        else sxn_inspect_quoted(ctx, out, key);
        if (name) JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, key);
        dynstr_add(out, ": ", 2);
        JSValue item = JS_GetProperty(ctx, v, keys[i].atom);
        sxn_inspect_value(ctx, out, item, depth + 1, max, &here);
        JS_FreeValue(ctx, item);
    }
    JS_FreePropertyEnum(ctx, keys, count);
    dynstr_add(out, " }", 2);
}

static JSValue js_inspect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    int max = 2;
    bool quote_top = false;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue depth = JS_GetPropertyStr(ctx, argv[1], "depth");
        if (!JS_IsUndefined(depth)) { int32_t d = 2; JS_ToInt32(ctx, &d, depth); max = d; }
        JS_FreeValue(ctx, depth);
        JSValue quoted = JS_GetPropertyStr(ctx, argv[1], "quoteStrings");
        quote_top = JS_ToBool(ctx, quoted);
        JS_FreeValue(ctx, quoted);
    }
    DynStr out = { 0 };
    sxn_inspect_value(ctx, &out, argc > 0 ? argv[0] : JS_UNDEFINED, quote_top ? 1 : 0, max, NULL);
    JSValue text = JS_NewStringLen(ctx, out.data ? out.data : "", out.len);
    free(out.data);
    return text;
}


/* util.promisify. The JavaScript version spread the arguments, built a
   Promise with an executor closure and then a callback closure inside it,
   all per call. Here the promise is made directly and the callback is a C
   function carrying its two resolving functions. */
static JSValue sxn_promisify_callback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)this_val; (void)magic;
    JSValueConst err = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsUndefined(err) && !JS_IsNull(err)) {
        JSValueConst args[1] = { err };
        JS_FreeValue(ctx, JS_Call(ctx, data[1], JS_UNDEFINED, 1, args));
        return JS_UNDEFINED;
    }
    /* Node resolves with the callback's first value and drops the rest,
       which the JavaScript version here did not: it handed back an array.
       Node's own answer is the one to keep. */
    JSValue value = argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
    JSValueConst args[1] = { value };
    JS_FreeValue(ctx, JS_Call(ctx, data[0], JS_UNDEFINED, 1, args));
    JS_FreeValue(ctx, value);
    return JS_UNDEFINED;
}

static JSValue sxn_promisified(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)magic;
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return promise;
    JSValue callback = JS_NewCFunctionData(ctx, sxn_promisify_callback, 2, 0, 2, resolving);
    if (JS_IsException(callback)) {
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        JS_FreeValue(ctx, promise);
        return callback;
    }

    JSValue *args = js_malloc(ctx, sizeof(JSValue) * (size_t)(argc + 1));
    if (!args) { JS_FreeValue(ctx, callback); JS_FreeValue(ctx, promise); return JS_EXCEPTION; }
    for (int i = 0; i < argc; i++) args[i] = JS_DupValue(ctx, argv[i]);
    args[argc] = callback;
    JSValue result = JS_Call(ctx, data[0], this_val, argc + 1, (JSValueConst *)args);
    for (int i = 0; i <= argc; i++) JS_FreeValue(ctx, args[i]);
    js_free(ctx, args);
    if (JS_IsException(result)) {
        /* Node calls the function inside the promise, so a synchronous
           throw comes back as a rejection rather than out of the call. */
        JSValue error = JS_GetException(ctx);
        JSValueConst reject_args[1] = { error };
        JS_FreeValue(ctx, JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, reject_args));
        JS_FreeValue(ctx, error);
    } else {
        JS_FreeValue(ctx, result);
    }
    JS_FreeValue(ctx, resolving[0]);
    JS_FreeValue(ctx, resolving[1]);
    return promise;
}

static JSValue js_promisify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "promisify expects a function");
    JSValue data[1] = { JS_DupValue(ctx, argv[0]) };
    JSValue wrapped = JS_NewCFunctionData(ctx, sxn_promisified, 0, 0, 1, data);
    JS_FreeValue(ctx, data[0]);
    if (JS_IsException(wrapped)) return wrapped;
    /* Node keeps the original's name on the wrapper. */
    JSValue name = JS_GetPropertyStr(ctx, argv[0], "name");
    JSAtom name_atom = JS_NewAtom(ctx, "name");
    JS_DefinePropertyValue(ctx, wrapped, name_atom, name, JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, name_atom);
    return wrapped;
}


/* Readable#pipe. Four closures per pipe in JavaScript, one for each of the
   events involved; here they are four C functions sharing the same two
   values -- the source and the destination -- and the fourth carries the
   end option too. */
static JSValue sxn_pipe_handler(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)this_val;
    JSValueConst source = data[0], dest = data[1];
    switch (magic) {
    case 0: {   /* data: write, and pause the source if it says to */
        JSValue write = JS_GetPropertyStr(ctx, dest, "write");
        JSValueConst args[1] = { argc > 0 ? argv[0] : JS_UNDEFINED };
        JSValue ok = JS_Call(ctx, write, dest, 1, args);
        JS_FreeValue(ctx, write);
        if (JS_IsException(ok)) return ok;
        bool full = JS_IsBool(ok) && !JS_ToBool(ctx, ok);
        JS_FreeValue(ctx, ok);
        if (full) {
            JSValue pause = JS_GetPropertyStr(ctx, source, "pause");
            JS_FreeValue(ctx, JS_Call(ctx, pause, source, 0, NULL));
            JS_FreeValue(ctx, pause);
        }
        return JS_UNDEFINED;
    }
    case 1: {   /* drain: the destination wants more */
        JSValue resume = JS_GetPropertyStr(ctx, source, "resume");
        JS_FreeValue(ctx, JS_Call(ctx, resume, source, 0, NULL));
        JS_FreeValue(ctx, resume);
        return JS_UNDEFINED;
    }
    case 2: {   /* end: unless the caller asked for the destination to stay */
        if (JS_ToBool(ctx, data[2])) {
            JSValue end = JS_GetPropertyStr(ctx, dest, "end");
            JS_FreeValue(ctx, JS_Call(ctx, end, dest, 0, NULL));
            JS_FreeValue(ctx, end);
        }
        return JS_UNDEFINED;
    }
    default: {  /* error: forward it */
        JSValue emit = JS_GetPropertyStr(ctx, dest, "emit");
        JSValue name = JS_NewString(ctx, "error");
        JSValueConst args[2] = { name, argc > 0 ? argv[0] : JS_UNDEFINED };
        JS_FreeValue(ctx, JS_Call(ctx, emit, dest, 2, args));
        JS_FreeValue(ctx, name);
        JS_FreeValue(ctx, emit);
        return JS_UNDEFINED;
    }
    }
}

static void sxn_pipe_listen(JSContext *ctx, JSValueConst target, const char *event, JSValueConst fn) {
    JSValue on = JS_GetPropertyStr(ctx, target, "on");
    JSValue name = JS_NewString(ctx, event);
    JSValueConst args[2] = { name, fn };
    JS_FreeValue(ctx, JS_Call(ctx, on, target, 2, args));
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, on);
}

static JSValue js_stream_pipe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "pipe expects a destination");
    JSValueConst dest = argv[0];
    bool end_dest = true;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue end_opt = JS_GetPropertyStr(ctx, argv[1], "end");
        if (!JS_IsUndefined(end_opt)) end_dest = JS_ToBool(ctx, end_opt);
        JS_FreeValue(ctx, end_opt);
    }
    JSValue data[3] = { JS_DupValue(ctx, this_val), JS_DupValue(ctx, dest), JS_NewBool(ctx, end_dest) };
    JSValue on_data = JS_NewCFunctionData(ctx, sxn_pipe_handler, 1, 0, 3, data);
    JSValue on_drain = JS_NewCFunctionData(ctx, sxn_pipe_handler, 0, 1, 3, data);
    JSValue on_end = JS_NewCFunctionData(ctx, sxn_pipe_handler, 0, 2, 3, data);
    JSValue on_error = JS_NewCFunctionData(ctx, sxn_pipe_handler, 1, 3, 3, data);
    for (int i = 0; i < 3; i++) JS_FreeValue(ctx, data[i]);

    sxn_pipe_listen(ctx, this_val, "data", on_data);
    sxn_pipe_listen(ctx, dest, "drain", on_drain);
    sxn_pipe_listen(ctx, this_val, "end", on_end);
    sxn_pipe_listen(ctx, this_val, "error", on_error);

    /* unpipe needs to find these again, so the record is the same shape the
       JavaScript kept. */
    JSValue record = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, record, "dest", JS_DupValue(ctx, dest));
    JS_SetPropertyStr(ctx, record, "onData", on_data);
    JS_SetPropertyStr(ctx, record, "onDrain", on_drain);
    JS_SetPropertyStr(ctx, record, "onEnd", on_end);
    JS_SetPropertyStr(ctx, record, "onError", on_error);
    JSValue pipes = JS_GetPropertyStr(ctx, this_val, "_pipes");
    if (!JS_IsObject(pipes)) {
        JS_FreeValue(ctx, pipes);
        pipes = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "_pipes", JS_DupValue(ctx, pipes));
    }
    int64_t count = 0;
    JS_GetLength(ctx, pipes, &count);
    JS_SetPropertyUint32(ctx, pipes, (uint32_t)count, record);
    JS_FreeValue(ctx, pipes);

    JSValue resume = JS_GetPropertyStr(ctx, this_val, "resume");
    JS_FreeValue(ctx, JS_Call(ctx, resume, this_val, 0, NULL));
    JS_FreeValue(ctx, resume);
    return JS_DupValue(ctx, dest);
}


/* Buffer.from of a view. Going through `new Buffer(...)` meant running a
   Uint8Array subclass constructor per call; the copy is made here and given
   Buffer's prototype -- a fresh object of ours, so nothing the caller holds
   is changed. The array-like path below is kept for a Float64Array and
   friends, whose elements Node truncates to bytes one by one, but a plain
   Array is left to the constructor: reading its elements from C measured
   three times slower than the engine filling the array itself. */
static JSValue js_buffer_from_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Buffer.from: unsupported argument");
    size_t len = 0;
    uint8_t *bytes = JS_GetUint8Array(ctx, &len, argv[0]);
    JSValue out;
    if (bytes) {
        out = JS_NewUint8ArrayCopy(ctx, bytes, len);
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        int64_t count = 0;
        if (JS_GetLength(ctx, argv[0], &count)) return JS_EXCEPTION;
        uint8_t *raw = js_malloc(ctx, count ? (size_t)count : 1);
        if (!raw) return JS_EXCEPTION;
        for (int64_t i = 0; i < count; i++) {
            JSValue item = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
            int32_t value = 0;
            /* Node truncates to a byte, and anything not a number is zero. */
            if (JS_ToInt32(ctx, &value, item)) { JS_FreeValue(ctx, JS_GetException(ctx)); value = 0; }
            JS_FreeValue(ctx, item);
            raw[i] = (uint8_t)value;
        }
        out = JS_NewUint8ArrayCopy(ctx, raw, (size_t)count);
        js_free(ctx, raw);
    }
    if (JS_IsException(out)) return out;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue buffer_class = JS_GetPropertyStr(ctx, global, "Buffer");
    JSValue proto = JS_GetPropertyStr(ctx, buffer_class, "prototype");
    JS_FreeValue(ctx, buffer_class);
    JS_FreeValue(ctx, global);
    JS_SetPrototype(ctx, out, proto);
    JS_FreeValue(ctx, proto);
    return out;
}


/* util.inherits: two property operations, done from C instead of through
   Object.defineProperty and Object.setPrototypeOf. */
static JSValue js_inherits(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsFunction(ctx, argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "inherits expects two constructors");
    JSAtom super_atom = JS_NewAtom(ctx, "super_");
    JS_DefinePropertyValue(ctx, argv[0], super_atom, JS_DupValue(ctx, argv[1]),
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, super_atom);
    JSValue proto = JS_GetPropertyStr(ctx, argv[0], "prototype");
    JSValue super_proto = JS_GetPropertyStr(ctx, argv[1], "prototype");
    JS_SetPrototype(ctx, proto, super_proto);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, super_proto);
    return JS_UNDEFINED;
}


/* util.callbackify: the JavaScript built two closures per call for the two
   halves of the promise. Here they are C functions carrying the callback. */
static JSValue sxn_callbackify_settle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)this_val;
    JSValueConst value = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue args[2];
    if (magic) {   /* rejected */
        args[0] = JS_DupValue(ctx, value);
        if (JS_IsNull(args[0]) || JS_IsUndefined(args[0])) {
            JS_FreeValue(ctx, args[0]);
            /* Node wraps a falsy rejection in an Error and keeps the
               original on `reason`, which is worth having. */
            args[0] = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, args[0], "message",
                              JS_NewString(ctx, "Promise was rejected with falsy value"));
            JS_SetPropertyStr(ctx, args[0], "reason", JS_DupValue(ctx, value));
        }
        args[1] = JS_UNDEFINED;
    } else {
        args[0] = JS_NULL;
        args[1] = JS_DupValue(ctx, value);
    }
    JS_FreeValue(ctx, JS_Call(ctx, data[0], JS_UNDEFINED, 2, (JSValueConst *)args));
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    return JS_UNDEFINED;
}

static JSValue sxn_callbackified(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)magic;
    if (argc < 1 || !JS_IsFunction(ctx, argv[argc - 1]))
        return JS_ThrowTypeError(ctx, "the last argument must be a callback");
    JSValueConst callback = argv[argc - 1];
    JSValue result = JS_Call(ctx, data[0], this_val, argc - 1, argv);
    if (JS_IsException(result)) return result;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue promise_class = JS_GetPropertyStr(ctx, global, "Promise");
    JS_FreeValue(ctx, global);
    JSValue resolve = JS_GetPropertyStr(ctx, promise_class, "resolve");
    JSValueConst resolve_args[1] = { result };
    JSValue promise = JS_Call(ctx, resolve, promise_class, 1, resolve_args);
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, promise_class);
    JS_FreeValue(ctx, result);
    if (JS_IsException(promise)) return promise;

    JSValue handler_data[1] = { JS_DupValue(ctx, callback) };
    JSValue on_value = JS_NewCFunctionData(ctx, sxn_callbackify_settle, 1, 0, 1, handler_data);
    JSValue on_error = JS_NewCFunctionData(ctx, sxn_callbackify_settle, 1, 1, 1, handler_data);
    JS_FreeValue(ctx, handler_data[0]);
    JSValue then = JS_GetPropertyStr(ctx, promise, "then");
    JSValueConst then_args[2] = { on_value, on_error };
    JS_FreeValue(ctx, JS_Call(ctx, then, promise, 2, then_args));
    JS_FreeValue(ctx, then);
    JS_FreeValue(ctx, on_value);
    JS_FreeValue(ctx, on_error);
    JS_FreeValue(ctx, promise);
    return JS_UNDEFINED;
}

static JSValue js_callbackify(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "callbackify expects a function");
    JSValue data[1] = { JS_DupValue(ctx, argv[0]) };
    JSValue wrapped = JS_NewCFunctionData(ctx, sxn_callbackified, 0, 0, 1, data);
    JS_FreeValue(ctx, data[0]);
    return wrapped;
}

/* ---------------- assert's deep comparison, in C ----------------
   The whole of it is calls back into the engine -- reading properties,
   comparing values, walking a Map -- so this is not faster than the
   JavaScript it replaces. It is here because the compatibility layer is
   being moved into C and this is the last piece of it that carries real
   logic rather than glue. The measurement is in spec/NODE.md.

   Cycles are handled the way the specification's SameValue-based walk is:
   a pair already being compared higher up the stack is taken as equal,
   which terminates and matches what Node does for two identical cycles. */
typedef struct SxnDeepPair { JSValueConst a, b; struct SxnDeepPair *prev; } SxnDeepPair;

static int sxn_deep_equal(JSContext *ctx, JSValueConst a, JSValueConst b, bool strict, SxnDeepPair *seen, int depth);

static bool sxn_same_value(JSContext *ctx, JSValueConst a, JSValueConst b) {
    /* Object.is: NaN equals itself, +0 and -0 do not. */
    return JS_IsSameValue(ctx, a, b);
}

static int sxn_deep_keys_equal(JSContext *ctx, JSValueConst a, JSValueConst b, bool strict, SxnDeepPair *seen, int depth) {
    JSPropertyEnum *ka = NULL, *kb = NULL;
    uint32_t na = 0, nb = 0;
    int result = -1;
    /* The strict comparison counts own enumerable symbol keys; the loose one
       does not look at them at all. */
    int flags = JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY | (strict ? JS_GPN_SYMBOL_MASK : 0);
    if (JS_GetOwnPropertyNames(ctx, &ka, &na, a, flags)) return -1;
    if (JS_GetOwnPropertyNames(ctx, &kb, &nb, b, flags)) {
        JS_FreePropertyEnum(ctx, ka, na);
        return -1;
    }
    if (na != nb) { result = 0; goto done; }
    for (uint32_t i = 0; i < na; i++) {
        int has = JS_HasProperty(ctx, b, ka[i].atom);
        if (has < 0) { result = -1; goto done; }
        if (!has) { result = 0; goto done; }
        JSValue va = JS_GetProperty(ctx, a, ka[i].atom);
        if (JS_IsException(va)) { result = -1; goto done; }
        JSValue vb = JS_GetProperty(ctx, b, ka[i].atom);
        if (JS_IsException(vb)) { JS_FreeValue(ctx, va); result = -1; goto done; }
        int same = sxn_deep_equal(ctx, va, vb, strict, seen, depth + 1);
        JS_FreeValue(ctx, va);
        JS_FreeValue(ctx, vb);
        if (same <= 0) { result = same; goto done; }
    }
    result = 1;
 done:
    JS_FreePropertyEnum(ctx, ka, na);
    JS_FreePropertyEnum(ctx, kb, nb);
    return result;
}

/* Every entry of a Map, matched against the other Map's entry for the same
   key. Set membership is the same walk with the value ignored. */
static int sxn_deep_map_equal(JSContext *ctx, JSValueConst a, JSValueConst b, bool is_map, bool strict, SxnDeepPair *seen, int depth) {
    int result = -1;
    JSValue iterator = JS_UNDEFINED, next = JS_UNDEFINED;
    JSValue size_a = JS_GetPropertyStr(ctx, a, "size");
    JSValue size_b = JS_GetPropertyStr(ctx, b, "size");
    int32_t na = 0, nb = 0;
    JS_ToInt32(ctx, &na, size_a);
    JS_ToInt32(ctx, &nb, size_b);
    JS_FreeValue(ctx, size_a);
    JS_FreeValue(ctx, size_b);
    if (na != nb) return 0;

    JSValue entries_fn = JS_GetPropertyStr(ctx, a, is_map ? "entries" : "values");
    iterator = JS_Call(ctx, entries_fn, a, 0, NULL);
    JS_FreeValue(ctx, entries_fn);
    if (JS_IsException(iterator)) goto done;
    next = JS_GetPropertyStr(ctx, iterator, "next");
    if (JS_IsException(next)) goto done;
    for (;;) {
        JSValue step = JS_Call(ctx, next, iterator, 0, NULL);
        if (JS_IsException(step)) goto done;
        JSValue done_flag = JS_GetPropertyStr(ctx, step, "done");
        bool finished = JS_ToBool(ctx, done_flag);
        JS_FreeValue(ctx, done_flag);
        if (finished) { JS_FreeValue(ctx, step); break; }
        JSValue entry = JS_GetPropertyStr(ctx, step, "value");
        JS_FreeValue(ctx, step);
        JSValue key = is_map ? JS_GetPropertyUint32(ctx, entry, 0) : JS_DupValue(ctx, entry);
        JSValue has_fn = JS_GetPropertyStr(ctx, b, "has");
        JSValueConst args[1] = { key };
        JSValue has = JS_Call(ctx, has_fn, b, 1, args);
        JS_FreeValue(ctx, has_fn);
        bool present = JS_ToBool(ctx, has);
        JS_FreeValue(ctx, has);
        if (!present) {
            JS_FreeValue(ctx, key); JS_FreeValue(ctx, entry);
            result = 0; goto done;
        }
        if (is_map) {
            JSValue want = JS_GetPropertyUint32(ctx, entry, 1);
            JSValue get_fn = JS_GetPropertyStr(ctx, b, "get");
            JSValueConst get_args[1] = { key };
            JSValue got = JS_Call(ctx, get_fn, b, 1, get_args);
            JS_FreeValue(ctx, get_fn);
            int same = sxn_deep_equal(ctx, want, got, strict, seen, depth + 1);
            JS_FreeValue(ctx, want);
            JS_FreeValue(ctx, got);
            if (same <= 0) {
                JS_FreeValue(ctx, key); JS_FreeValue(ctx, entry);
                result = same; goto done;
            }
        }
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, entry);
    }
    result = 1;
 done:
    JS_FreeValue(ctx, iterator);
    JS_FreeValue(ctx, next);
    return result;
}

static int sxn_deep_equal(JSContext *ctx, JSValueConst a, JSValueConst b, bool strict, SxnDeepPair *seen, int depth) {
    if (depth > 512) return JS_ThrowRangeError(ctx, "deep comparison too deep"), -1;
    if (strict ? sxn_same_value(ctx, a, b) : JS_IsStrictEqual(ctx, a, b)) return 1;
    if (!JS_IsObject(a) || !JS_IsObject(b)) {
        /* An object never equals a primitive, either way round: the loose
           comparison is loose about 1 and "1", not about boxes. */
        if (JS_IsObject(a) || JS_IsObject(b)) return 0;
        if (strict) return sxn_same_value(ctx, a, b) ? 1 : 0;
        /* NaN is its own match here, as it is in Node. */
        if (JS_IsSameValueZero(ctx, a, b)) return 1;
        int eq = JS_IsEqual(ctx, a, b);
        return eq < 0 ? -1 : (eq > 0 ? 1 : 0);
    }
    /* Two different functions are never equal, whatever they carry. */
    if (JS_IsFunction(ctx, a) || JS_IsFunction(ctx, b)) return 0;

    for (SxnDeepPair *p = seen; p; p = p->prev)
        if (JS_IsStrictEqual(ctx, p->a, a) && JS_IsStrictEqual(ctx, p->b, b))
            return 1;   /* already being compared: a cycle */
    SxnDeepPair pair = { a, b, seen };

    if (strict) {
        /* Only the strict comparison cares which class an object came from. */
        JSValue proto_a = JS_GetPrototype(ctx, a);
        JSValue proto_b = JS_GetPrototype(ctx, b);
        bool same_proto = JS_IsStrictEqual(ctx, proto_a, proto_b);
        JS_FreeValue(ctx, proto_a);
        JS_FreeValue(ctx, proto_b);
        if (!same_proto) return 0;
    }

    /* Dates, regexps, maps and sets compare by content rather than by their
       properties. The class id is what instanceof would find and what a
       subclass keeps, so it is read directly instead of by name. */
    static const char *probe_src = "[new Date(), /x/, new Map(), new Set()]";
    static JSClassID date_id, regexp_id, map_id, set_id;
    if (!date_id) {
        JSValue probe = JS_Eval(ctx, probe_src, strlen(probe_src), "<deep>", JS_EVAL_TYPE_GLOBAL);
        JSValue v;
        v = JS_GetPropertyUint32(ctx, probe, 0); date_id = JS_GetClassID(v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyUint32(ctx, probe, 1); regexp_id = JS_GetClassID(v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyUint32(ctx, probe, 2); map_id = JS_GetClassID(v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyUint32(ctx, probe, 3); set_id = JS_GetClassID(v); JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, probe);
    }
    JSClassID cls = JS_GetClassID(a);
    /* Even the loose comparison keeps arrays, typed arrays and dates apart
       from plain objects, which is what the class says. */
    if (cls != JS_GetClassID(b)) return 0;
    int result = -2;
    if (cls == date_id) {
        JSValue fa = JS_GetPropertyStr(ctx, a, "getTime");
        JSValue va = JS_Call(ctx, fa, a, 0, NULL);
        JSValue vb = JS_Call(ctx, fa, b, 0, NULL);
        JS_FreeValue(ctx, fa);
        double da = 0, db = 0;
        JS_ToFloat64(ctx, &da, va);
        JS_ToFloat64(ctx, &db, vb);
        JS_FreeValue(ctx, va);
        JS_FreeValue(ctx, vb);
        result = (da == db || (isnan(da) && isnan(db))) ? 1 : 0;
    } else if (cls == regexp_id) {
        JSValue sa = JS_ToString(ctx, a), sb = JS_ToString(ctx, b);
        result = JS_IsStrictEqual(ctx, sa, sb) ? 1 : 0;
        JS_FreeValue(ctx, sa);
        JS_FreeValue(ctx, sb);
    } else if (cls == map_id) {
        result = sxn_deep_map_equal(ctx, a, b, true, strict, &pair, depth);
    } else if (cls == set_id) {
        result = sxn_deep_map_equal(ctx, a, b, false, strict, &pair, depth);
    }
    if (result != -2) return result;

    /* A typed array compares as bytes. */
    size_t bytes_a = 0, bytes_b = 0;
    uint8_t *raw_a = JS_GetUint8Array(ctx, &bytes_a, a);
    if (!raw_a) JS_FreeValue(ctx, JS_GetException(ctx));
    uint8_t *raw_b = raw_a ? JS_GetUint8Array(ctx, &bytes_b, b) : NULL;
    if (raw_a && !raw_b) JS_FreeValue(ctx, JS_GetException(ctx));
    if (raw_a && raw_b)
        return (bytes_a == bytes_b && (bytes_a == 0 || memcmp(raw_a, raw_b, bytes_a) == 0)) ? 1 : 0;

    return sxn_deep_keys_equal(ctx, a, b, strict, &pair, depth);
}

static JSValue js_deep_equal(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    (void)this_val;
    if (argc < 2) return JS_NewBool(ctx, false);
    int result = sxn_deep_equal(ctx, argv[0], argv[1], magic != 0, NULL, 0);
    if (result < 0) return JS_EXCEPTION;
    return JS_NewBool(ctx, result == 1);
}

static const char *node_querystring_names[] = { "parse", "stringify", "escape", "unescape", "decode", "encode" };
static const char *node_url_names[] = {
    "URL", "URLSearchParams", "fileURLToPath", "pathToFileURL", "format", "parse",
};
static const char *node_net_names[] = {
    "isIP", "isIPv4", "isIPv6", "Socket", "Server",
    "createConnection", "connect", "createServer",
};
static const char *node_crypto_names[] = {
    "createHash", "createHmac", "randomBytes", "randomUUID", "randomFillSync",
    "getRandomValues", "randomInt", "timingSafeEqual", "getHashes",
    "constants", "webcrypto", "subtle", "Hash", "Hmac",
};
static const char *node_zlib_names[] = {
    "constants", "gzip", "gunzip", "deflate", "inflate", "deflateRaw",
    "inflateRaw", "unzip",
    "gzipSync", "gunzipSync", "deflateSync", "inflateSync", "deflateRawSync",
    "inflateRawSync", "unzipSync",
    "createGzip", "createGunzip", "createDeflate", "createInflate",
    "createDeflateRaw", "createInflateRaw", "createUnzip",
};
static const char *node_tty_names[] = { "isatty", "ReadStream", "WriteStream" };
static const char *node_string_decoder_names[] = { "StringDecoder" };
static const char *node_timers_names[] = {
    "setTimeout", "clearTimeout", "setInterval", "clearInterval",
    "setImmediate", "clearImmediate", "promises",
};
static const char *node_perf_hooks_names[] = { "performance", "PerformanceObserver" };
static const char *node_module_names[] = { "createRequire", "builtinModules", "isBuiltin" };
static const char *node_http_names[] = {
    "createServer", "request", "get", "Server", "IncomingMessage",
    "ServerResponse", "STATUS_CODES", "METHODS",
};
static const char *node_stream_names[] = {
    "Readable", "Writable", "Duplex", "Transform", "PassThrough",
    "Stream", "pipeline", "finished", "promises",
};
static const char *node_assert_names[] = {
    "ok", "equal", "notEqual", "strictEqual", "notStrictEqual", "deepEqual",
    "deepStrictEqual", "notDeepStrictEqual", "fail", "throws", "doesNotThrow",
    "match", "strict", "AssertionError",
};

/* One shared init: pull the object off its global and re-export its keys. */
typedef struct { const char *global; const char **names; size_t count; } NodeSimpleModule;

static int node_simple_init(JSContext *ctx, JSModuleDef *m,
                            const char *global, const char **names, size_t count) {
    JSValue obj = node_global_lookup(ctx, global);
    JS_SetModuleExport(ctx, m, "default", JS_DupValue(ctx, obj));
    for (size_t i = 0; i < count; i++)
        JS_SetModuleExport(ctx, m, names[i], JS_GetPropertyStr(ctx, obj, names[i]));
    JS_FreeValue(ctx, obj);
    return 0;
}

#define NODE_SIMPLE_MODULE(tag, globalname, namearr)                          \
    static int node_##tag##_init(JSContext *ctx, JSModuleDef *m) {            \
        return node_simple_init(ctx, m, globalname, namearr, countof(namearr)); \
    }                                                                         \
    static JSModuleDef *sxn_init_module_node_##tag(JSContext *ctx, const char *name) { \
        JSModuleDef *m = JS_NewCModule(ctx, name, node_##tag##_init);         \
        if (!m) return NULL;                                                  \
        JS_AddModuleExport(ctx, m, "default");                                \
        for (size_t i = 0; i < countof(namearr); i++)                         \
            JS_AddModuleExport(ctx, m, namearr[i]);                           \
        return m;                                                             \
    }

NODE_SIMPLE_MODULE(util, "__sxnUtil", node_util_names)
NODE_SIMPLE_MODULE(os, "__sxnOs", node_os_names)
NODE_SIMPLE_MODULE(querystring, "__sxnQuerystring", node_querystring_names)
NODE_SIMPLE_MODULE(url, "__sxnUrl", node_url_names)
NODE_SIMPLE_MODULE(assert, "__sxnAssert", node_assert_names)
NODE_SIMPLE_MODULE(stream, "__sxnStream", node_stream_names)
NODE_SIMPLE_MODULE(http, "__sxnHttp", node_http_names)
NODE_SIMPLE_MODULE(net, "__sxnNet", node_net_names)
NODE_SIMPLE_MODULE(crypto, "__sxnCrypto", node_crypto_names)
NODE_SIMPLE_MODULE(zlib, "__sxnZlib", node_zlib_names)
NODE_SIMPLE_MODULE(tty, "__sxnTty", node_tty_names)
NODE_SIMPLE_MODULE(string_decoder, "__sxnStringDecoder", node_string_decoder_names)
NODE_SIMPLE_MODULE(timers, "__sxnTimers", node_timers_names)
NODE_SIMPLE_MODULE(perf_hooks, "__sxnPerfHooks", node_perf_hooks_names)
NODE_SIMPLE_MODULE(module, "__sxnModule", node_module_names)
static const char *node_timers_promises_names[] = { "setTimeout", "setImmediate" };
NODE_SIMPLE_MODULE(timers_promises, "__sxnTimersPromises", node_timers_promises_names)
static const char *node_stream_promises_names[] = { "pipeline", "finished" };
NODE_SIMPLE_MODULE(stream_promises, "__sxnStreamPromises", node_stream_promises_names)

static const char *node_child_process_names[] = {
    "spawn", "spawnSync", "exec", "execSync", "execFile", "execFileSync",
    "fork", "ChildProcess",
};
static const char *node_dns_names[] = {
    "lookup", "resolve", "resolve4", "resolve6", "resolveMx", "resolveTxt",
    "resolveSrv", "resolveNs", "resolveCname", "reverse", "getServers",
    "setServers", "promises", "Resolver",
};
static const char *node_dns_promises_names[] = { "lookup", "resolve", "resolve4", "resolve6", "getServers" };
static const char *node_https_names[] = { "request", "get", "Agent", "globalAgent", "createServer", "Server" };
static const char *node_tls_names[] = {
    "connect", "createServer", "TLSSocket", "Server", "createSecureContext",
    "rootCertificates", "DEFAULT_MIN_VERSION", "DEFAULT_MAX_VERSION",
};
static const char *node_http2_names[] = {
    "constants", "connect", "createServer", "createSecureServer", "getDefaultSettings",
};
static const char *node_stream_web_names[] = {
    "ReadableStream", "ReadableStreamDefaultReader", "ReadableStreamBYOBReader",
    "ReadableStreamDefaultController", "ReadableByteStreamController",
    "ReadableStreamBYOBRequest", "WritableStream", "WritableStreamDefaultWriter",
    "WritableStreamDefaultController", "TransformStream",
    "TransformStreamDefaultController", "ByteLengthQueuingStrategy",
    "CountQueuingStrategy", "TextEncoderStream", "TextDecoderStream",
    "CompressionStream", "DecompressionStream",
};
static const char *node_vm_names[] = {
    "runInThisContext", "runInNewContext", "runInContext", "createContext",
    "isContext", "compileFunction", "Script",
};
static const char *node_v8_names[] = {
    "getHeapStatistics", "getHeapSpaceStatistics", "setFlagsFromString",
    "serialize", "deserialize", "cachedDataVersionTag",
};
static const char *node_worker_threads_names[] = {
    "isMainThread", "threadId", "parentPort", "workerData", "resourceLimits",
    "SHARE_ENV", "Worker", "MessageChannel", "MessagePort", "BroadcastChannel",
    "markAsUntransferable", "moveMessagePortToContext", "receiveMessageOnPort",
    "setEnvironmentData", "getEnvironmentData",
};
static const char *node_cluster_names[] = {
    "isPrimary", "isMaster", "isWorker", "worker", "workers", "settings",
    "schedulingPolicy", "setupPrimary", "setupMaster", "fork", "disconnect",
};
static const char *node_readline_names[] = {
    "Interface", "createInterface", "clearLine", "clearScreenDown", "cursorTo",
    "moveCursor", "emitKeypressEvents", "promises",
};
static const char *node_readline_promises_names[] = { "Interface", "createInterface" };
static const char *node_async_hooks_names[] = {
    "AsyncLocalStorage", "AsyncResource", "executionAsyncId", "triggerAsyncId",
    "executionAsyncResource", "createHook",
};
static const char *node_inspector_names[] = { "url", "open", "close", "waitForDebugger", "console", "Session", "promises" };

NODE_SIMPLE_MODULE(child_process, "__sxnChildProcess", node_child_process_names)
NODE_SIMPLE_MODULE(dns, "__sxnDns", node_dns_names)
NODE_SIMPLE_MODULE(dns_promises, "__sxnDnsPromises", node_dns_promises_names)
NODE_SIMPLE_MODULE(https, "__sxnHttps", node_https_names)
NODE_SIMPLE_MODULE(tls, "__sxnTls", node_tls_names)
NODE_SIMPLE_MODULE(http2, "__sxnHttp2", node_http2_names)
NODE_SIMPLE_MODULE(stream_web, "__sxnStreamWeb", node_stream_web_names)
NODE_SIMPLE_MODULE(vm, "__sxnVm", node_vm_names)
NODE_SIMPLE_MODULE(v8, "__sxnV8", node_v8_names)
NODE_SIMPLE_MODULE(worker_threads, "__sxnWorkerThreads", node_worker_threads_names)
NODE_SIMPLE_MODULE(cluster, "__sxnCluster", node_cluster_names)
NODE_SIMPLE_MODULE(readline, "__sxnReadline", node_readline_names)
NODE_SIMPLE_MODULE(readline_promises, "__sxnReadlinePromises", node_readline_promises_names)
NODE_SIMPLE_MODULE(async_hooks, "__sxnAsyncHooks", node_async_hooks_names)
NODE_SIMPLE_MODULE(inspector, "__sxnInspector", node_inspector_names)

static const char *node_dgram_names[] = { "Socket", "createSocket" };
static const char *node_punycode_names[] = { "encode", "decode", "toASCII", "toUnicode", "ucs2", "version" };
static const char *node_diagnostics_channel_names[] = {
    "Channel", "channel", "hasSubscribers", "subscribe", "unsubscribe", "tracingChannel",
};
NODE_SIMPLE_MODULE(dgram, "__sxnDgram", node_dgram_names)
NODE_SIMPLE_MODULE(punycode, "__sxnPunycode", node_punycode_names)
NODE_SIMPLE_MODULE(diagnostics_channel, "__sxnDiagnosticsChannel", node_diagnostics_channel_names)



static const char *node_fs_export_names[] = {
    "readFileSync", "writeFileSync", "existsSync", "statSync", "lstatSync",
    "createReadStream", "Stats", "constants",
};

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

static const char *node_fs_promises_export_names[] = { "readFile", "writeFile", "stat", "lstat" };

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
    /* Release the call-site fusion caches here rather than leaving them to
       JS_FreeContext, which returns early when anything still holds a context
       reference and would leave the cached shapes outstanding. */
    JS_DisableEmitFusion(ctx);
    JS_DisableBufferLengthFusion(ctx);
    /* The cached atoms below hold real references for the life of the
       context; without this they show up as leaks under --leak-check (only
       visible in builds where the runtime's leak dumps are compiled in).
       JS_FreeAtom is a no-op for the predefined ones. */
    sxn_ee_memo_clear(ctx);
    JS_FreeAtom(ctx, sxn_atom_events);
    JS_FreeAtom(ctx, sxn_atom_length);
    JS_FreeAtom(ctx, sxn_atom_error);
    JS_FreeAtom(ctx, sxn_atom_utf8); JS_FreeAtom(ctx, sxn_atom_utf8_dash);
    JS_FreeAtom(ctx, sxn_atom_hex); JS_FreeAtom(ctx, sxn_atom_base64);
    JS_FreeAtom(ctx, sxn_atom_base64url); JS_FreeAtom(ctx, sxn_atom_toBase64);
    JS_FreeAtom(ctx, sxn_atom_latin1); JS_FreeAtom(ctx, sxn_atom_binary);
    JS_FreeAtom(ctx, sxn_atom_ascii); JS_FreeAtom(ctx, sxn_atom_ucs2);
    JS_FreeAtom(ctx, sxn_atom_ucs2_dash); JS_FreeAtom(ctx, sxn_atom_utf16le);
    JS_FreeAtom(ctx, sxn_atom_utf16le_dash);
    sxn_atom_latin1 = sxn_atom_binary = sxn_atom_ascii = JS_ATOM_NULL;
    sxn_atom_ucs2 = sxn_atom_ucs2_dash = JS_ATOM_NULL;
    sxn_atom_utf16le = sxn_atom_utf16le_dash = JS_ATOM_NULL;
    sxn_atom_utf8 = sxn_atom_utf8_dash = sxn_atom_hex = JS_ATOM_NULL;
    sxn_atom_base64 = sxn_atom_base64url = JS_ATOM_NULL;
    sxn_atom_toBase64 = JS_ATOM_NULL;
    sxn_atom_events = JS_ATOM_NULL;
    sxn_atom_length = JS_ATOM_NULL;
    sxn_atom_error = JS_ATOM_NULL;
}

/* Node-API lives in src/napi.c and is installed from here, not from the
   runtime's own surface: loading npm's compiled addons is Node emulation.
   Sxn.ffi is the other half of the pair and sits on the runtime side. */
void sxn_install_napi(JSContext *ctx, uv_loop_t *loop);

/* Every node: module this runtime has, and the function that registers it.
   Registration is not free -- a JSModuleDef plus an atom per export name --
   and a program that imports two of them used to pay for all forty-four at
   startup. They are registered when the loader asks for one instead. */
typedef struct { const char *name; JSModuleDef *(*init)(JSContext *, const char *); } SxnNodeModule;
static const SxnNodeModule sxn_node_modules[] = {
    { "node:buffer", sxn_init_module_node_buffer },
    { "node:events", sxn_init_module_node_events },
    { "node:path", sxn_init_module_node_path },
    { "node:process", sxn_init_module_node_process },
    { "node:fs", sxn_init_module_node_fs },
    { "node:fs/promises", sxn_init_module_node_fs_promises },
    { "node:util", sxn_init_module_node_util },
    { "node:os", sxn_init_module_node_os },
    { "node:querystring", sxn_init_module_node_querystring },
    { "node:url", sxn_init_module_node_url },
    { "node:assert", sxn_init_module_node_assert },
    { "node:assert/strict", sxn_init_module_node_assert },
    { "node:stream", sxn_init_module_node_stream },
    { "node:http", sxn_init_module_node_http },
    { "node:net", sxn_init_module_node_net },
    { "node:crypto", sxn_init_module_node_crypto },
    { "node:zlib", sxn_init_module_node_zlib },
    { "node:tty", sxn_init_module_node_tty },
    { "node:string_decoder", sxn_init_module_node_string_decoder },
    { "node:timers", sxn_init_module_node_timers },
    { "node:timers/promises", sxn_init_module_node_timers_promises },
    { "node:stream/promises", sxn_init_module_node_stream_promises },
    { "node:perf_hooks", sxn_init_module_node_perf_hooks },
    { "node:module", sxn_init_module_node_module },
    { "node:child_process", sxn_init_module_node_child_process },
    { "node:dns", sxn_init_module_node_dns },
    { "node:dns/promises", sxn_init_module_node_dns_promises },
    { "node:https", sxn_init_module_node_https },
    { "node:tls", sxn_init_module_node_tls },
    { "node:http2", sxn_init_module_node_http2 },
    { "node:stream/web", sxn_init_module_node_stream_web },
    { "node:vm", sxn_init_module_node_vm },
    { "node:v8", sxn_init_module_node_v8 },
    { "node:worker_threads", sxn_init_module_node_worker_threads },
    { "node:cluster", sxn_init_module_node_cluster },
    { "node:readline", sxn_init_module_node_readline },
    { "node:readline/promises", sxn_init_module_node_readline_promises },
    { "node:async_hooks", sxn_init_module_node_async_hooks },
    { "node:inspector", sxn_init_module_node_inspector },
    { "node:dgram", sxn_init_module_node_dgram },
    { "node:punycode", sxn_init_module_node_punycode },
    { "node:diagnostics_channel", sxn_init_module_node_diagnostics_channel },
    { NULL, NULL },
};

JSModuleDef *sxn_node_module_load(JSContext *ctx, const char *name) {
    for (const SxnNodeModule *m = sxn_node_modules; m->name; m++)
        if (!strcmp(m->name, name)) return m->init(ctx, name);
    return NULL;
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
    JS_SetPropertyStr(ctx, global, "__sxnChdir", JS_NewCFunction(ctx, js_sxn_chdir, "__sxnChdir", 1));
    /* Node names the OS and CPU; packages branch on them. Derived from the
       compiler's own target macros rather than a runtime uname call. */
    JS_SetPropertyStr(ctx, global, "__sxnZlibDeflate", JS_NewCFunction(ctx, js_zlib_deflate, "__sxnZlibDeflate", 3));
    JS_SetPropertyStr(ctx, global, "__sxnZlibInflate", JS_NewCFunction(ctx, js_zlib_inflate, "__sxnZlibInflate", 2));
    JS_SetPropertyStr(ctx, global, "__sxnPlatform", JS_NewCFunction(ctx, js_sxn_platform, "__sxnPlatform", 0));
    JS_SetPropertyStr(ctx, global, "__sxnArch", JS_NewCFunction(ctx, js_sxn_arch, "__sxnArch", 0));
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
    JS_SetPropertyStr(ctx, global, "__sxnWinNormalize", JS_NewCFunction(ctx, js_path_win_normalize, "normalize", 1));
    JS_SetPropertyStr(ctx, global, "__sxnWinIsAbsolute", JS_NewCFunction(ctx, js_path_win_is_absolute, "isAbsolute", 1));
    JS_SetPropertyStr(ctx, global, "__sxnWinJoin", JS_NewCFunction(ctx, js_path_win_join, "join", 2));
    JS_SetPropertyStr(ctx, global, "__sxnWinResolve", JS_NewCFunction(ctx, js_path_win_resolve, "resolve", 2));
    JS_SetPropertyStr(ctx, global, "__sxnWinDirname", JS_NewCFunction(ctx, js_path_win_dirname, "dirname", 1));
    JS_SetPropertyStr(ctx, global, "__sxnWinBasename", JS_NewCFunction(ctx, js_path_win_basename, "basename", 2));
    JS_SetPropertyStr(ctx, global, "__sxnWinExtname", JS_NewCFunction(ctx, js_path_win_extname, "extname", 1));
    JS_SetPropertyStr(ctx, global, "__sxnWinRelative", JS_NewCFunction(ctx, js_path_win_relative, "relative", 2));
    {
        /* name, magic */
        static const struct { const char *name; int magic; } reads[] = {
            { "readUInt8", 1 }, { "readInt8", 1 | SXN_NUM_SIGNED },
            { "readUInt16LE", 2 }, { "readUInt16BE", 2 | SXN_NUM_BIG_END },
            { "readInt16LE", 2 | SXN_NUM_SIGNED }, { "readInt16BE", 2 | SXN_NUM_SIGNED | SXN_NUM_BIG_END },
            { "readUInt32LE", 4 }, { "readUInt32BE", 4 | SXN_NUM_BIG_END },
            { "readInt32LE", 4 | SXN_NUM_SIGNED }, { "readInt32BE", 4 | SXN_NUM_SIGNED | SXN_NUM_BIG_END },
            { "readFloatLE", 4 | SXN_NUM_FLOAT }, { "readFloatBE", 4 | SXN_NUM_FLOAT | SXN_NUM_BIG_END },
            { "readDoubleLE", 8 | SXN_NUM_FLOAT }, { "readDoubleBE", 8 | SXN_NUM_FLOAT | SXN_NUM_BIG_END },
            { "readBigUInt64LE", 8 | SXN_NUM_BIG_INT }, { "readBigUInt64BE", 8 | SXN_NUM_BIG_INT | SXN_NUM_BIG_END },
            { "readBigInt64LE", 8 | SXN_NUM_BIG_INT | SXN_NUM_SIGNED },
            { "readBigInt64BE", 8 | SXN_NUM_BIG_INT | SXN_NUM_SIGNED | SXN_NUM_BIG_END },
        };
        static const struct { const char *name; int magic; } writes[] = {
            { "writeUInt8", 1 }, { "writeInt8", 1 | SXN_NUM_SIGNED },
            { "writeUInt16LE", 2 }, { "writeUInt16BE", 2 | SXN_NUM_BIG_END },
            { "writeInt16LE", 2 | SXN_NUM_SIGNED }, { "writeInt16BE", 2 | SXN_NUM_SIGNED | SXN_NUM_BIG_END },
            { "writeUInt32LE", 4 }, { "writeUInt32BE", 4 | SXN_NUM_BIG_END },
            { "writeInt32LE", 4 | SXN_NUM_SIGNED }, { "writeInt32BE", 4 | SXN_NUM_SIGNED | SXN_NUM_BIG_END },
            { "writeFloatLE", 4 | SXN_NUM_FLOAT }, { "writeFloatBE", 4 | SXN_NUM_FLOAT | SXN_NUM_BIG_END },
            { "writeDoubleLE", 8 | SXN_NUM_FLOAT }, { "writeDoubleBE", 8 | SXN_NUM_FLOAT | SXN_NUM_BIG_END },
            { "writeBigUInt64LE", 8 | SXN_NUM_BIG_INT }, { "writeBigUInt64BE", 8 | SXN_NUM_BIG_INT | SXN_NUM_BIG_END },
            { "writeBigInt64LE", 8 | SXN_NUM_BIG_INT | SXN_NUM_SIGNED },
            { "writeBigInt64BE", 8 | SXN_NUM_BIG_INT | SXN_NUM_SIGNED | SXN_NUM_BIG_END },
        };
        JSValue accessors = JS_NewObject(ctx);
        for (size_t i = 0; i < countof(reads); i++)
            JS_SetPropertyStr(ctx, accessors, reads[i].name,
                              JS_NewCFunctionMagic(ctx, js_buffer_read, reads[i].name, 1, JS_CFUNC_generic_magic, reads[i].magic));
        for (size_t i = 0; i < countof(writes); i++)
            JS_SetPropertyStr(ctx, accessors, writes[i].name,
                              JS_NewCFunctionMagic(ctx, js_buffer_write_num, writes[i].name, 2, JS_CFUNC_generic_magic, writes[i].magic));
        JS_SetPropertyStr(ctx, accessors, "copy", JS_NewCFunction(ctx, js_buffer_copy, "copy", 4));
        JS_SetPropertyStr(ctx, accessors, "readUIntLE", JS_NewCFunctionMagic(ctx, js_buffer_read_var, "readUIntLE", 2, JS_CFUNC_generic_magic, 0));
        JS_SetPropertyStr(ctx, accessors, "readUIntBE", JS_NewCFunctionMagic(ctx, js_buffer_read_var, "readUIntBE", 2, JS_CFUNC_generic_magic, SXN_NUM_BIG_END));
        JS_SetPropertyStr(ctx, accessors, "readIntLE", JS_NewCFunctionMagic(ctx, js_buffer_read_var, "readIntLE", 2, JS_CFUNC_generic_magic, SXN_NUM_SIGNED));
        JS_SetPropertyStr(ctx, accessors, "readIntBE", JS_NewCFunctionMagic(ctx, js_buffer_read_var, "readIntBE", 2, JS_CFUNC_generic_magic, SXN_NUM_SIGNED | SXN_NUM_BIG_END));
        JS_SetPropertyStr(ctx, accessors, "writeUIntLE", JS_NewCFunctionMagic(ctx, js_buffer_write_var, "writeUIntLE", 3, JS_CFUNC_generic_magic, 0));
        JS_SetPropertyStr(ctx, accessors, "writeUIntBE", JS_NewCFunctionMagic(ctx, js_buffer_write_var, "writeUIntBE", 3, JS_CFUNC_generic_magic, SXN_NUM_BIG_END));
        JS_SetPropertyStr(ctx, accessors, "writeIntLE", JS_NewCFunctionMagic(ctx, js_buffer_write_var, "writeIntLE", 3, JS_CFUNC_generic_magic, SXN_NUM_SIGNED));
        JS_SetPropertyStr(ctx, accessors, "writeIntBE", JS_NewCFunctionMagic(ctx, js_buffer_write_var, "writeIntBE", 3, JS_CFUNC_generic_magic, SXN_NUM_SIGNED | SXN_NUM_BIG_END));
        JS_SetPropertyStr(ctx, accessors, "write", JS_NewCFunction(ctx, js_buffer_write, "write", 4));
        JS_SetPropertyStr(ctx, accessors, "swap16", JS_NewCFunctionMagic(ctx, js_buffer_swap, "swap16", 0, JS_CFUNC_generic_magic, 2));
        JS_SetPropertyStr(ctx, accessors, "swap32", JS_NewCFunctionMagic(ctx, js_buffer_swap, "swap32", 0, JS_CFUNC_generic_magic, 4));
        JS_SetPropertyStr(ctx, accessors, "swap64", JS_NewCFunctionMagic(ctx, js_buffer_swap, "swap64", 0, JS_CFUNC_generic_magic, 8));
        JS_SetPropertyStr(ctx, global, "__sxnBufferAccessors", accessors);
    }
    JS_SetPropertyStr(ctx, global, "__sxnCallbackify", JS_NewCFunction(ctx, js_callbackify, "callbackify", 1));
    JS_SetPropertyStr(ctx, global, "__sxnInherits", JS_NewCFunction(ctx, js_inherits, "inherits", 2));
    JS_SetPropertyStr(ctx, global, "__sxnBufferFromBytes", JS_NewCFunction(ctx, js_buffer_from_bytes, "__sxnBufferFromBytes", 1));
    JS_SetPropertyStr(ctx, global, "__sxnPipe", JS_NewCFunction(ctx, js_stream_pipe, "pipe", 2));
    JS_SetPropertyStr(ctx, global, "__sxnPromisify", JS_NewCFunction(ctx, js_promisify, "promisify", 1));
    JS_SetPropertyStr(ctx, global, "__sxnInspect", JS_NewCFunction(ctx, js_inspect, "inspect", 2));
    JS_SetPropertyStr(ctx, global, "__sxnNextTick", JS_NewCFunction(ctx, js_next_tick, "nextTick", 1));
    JS_SetPropertyStr(ctx, global, "__sxnGetHeaders", JS_NewCFunctionMagic(ctx, js_header_list, "getHeaders", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "__sxnGetHeaderNames", JS_NewCFunctionMagic(ctx, js_header_list, "getHeaderNames", 0, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "__sxnDecodeChunk", JS_NewCFunction(ctx, js_decode_chunk, "__sxnDecodeChunk", 2));
    JS_SetPropertyStr(ctx, global, "__sxnPathToFileUrl", JS_NewCFunction(ctx, js_path_to_file_url, "pathToFileURL", 1));
    JS_SetPropertyStr(ctx, global, "__sxnFileUrlToPath", JS_NewCFunction(ctx, js_file_url_to_path, "fileURLToPath", 1));
    JS_SetPropertyStr(ctx, global, "__sxnWriteCallback", JS_NewCFunction(ctx, js_write_callback, "__sxnWriteCallback", 2));
    JS_SetPropertyStr(ctx, global, "__sxnConcatBytes", JS_NewCFunction(ctx, js_concat_bytes, "__sxnConcatBytes", 2));
    JS_SetPropertyStr(ctx, global, "__sxnSetHeader", JS_NewCFunctionMagic(ctx, js_header_op, "setHeader", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "__sxnGetHeader", JS_NewCFunctionMagic(ctx, js_header_op, "getHeader", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "__sxnHasHeader", JS_NewCFunctionMagic(ctx, js_header_op, "hasHeader", 1, JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx, global, "__sxnRemoveHeader", JS_NewCFunctionMagic(ctx, js_header_op, "removeHeader", 1, JS_CFUNC_generic_magic, 3));
    JS_SetPropertyStr(ctx, global, "__sxnHttpReadBody", JS_NewCFunction(ctx, js_http_read_body, "_read", 0));
    JS_SetPropertyStr(ctx, global, "__sxnHttpComplete", JS_NewCFunction(ctx, js_http_complete, "onEnd", 0));
    JS_SetPropertyStr(ctx, global, "__sxnHttpSocket", JS_NewCFunction(ctx, js_http_socket, "__sxnHttpSocket", 0));
    JS_SetPropertyStr(ctx, global, "__sxnEeOnce", JS_NewCFunction(ctx, js_ee_once, "once", 2));
    JS_SetPropertyStr(ctx, global, "__sxnJoinChunks", JS_NewCFunction(ctx, js_join_chunks, "__sxnJoinChunks", 1));
    JS_SetPropertyStr(ctx, global, "__sxnHttpHeaders", JS_NewCFunction(ctx, js_http_headers, "__sxnHttpHeaders", 1));
    JS_SetPropertyStr(ctx, global, "__sxnBuiltinRequire", JS_NewCFunction(ctx, js_builtin_require, "__sxnBuiltinRequire", 1));
    JS_SetPropertyStr(ctx, global, "__sxnZlibStreamNew", JS_NewCFunction(ctx, js_zlib_stream_new, "__sxnZlibStreamNew", 3));
    JS_SetPropertyStr(ctx, global, "__sxnZlibStreamPush", JS_NewCFunction(ctx, js_zlib_stream_push, "__sxnZlibStreamPush", 3));
    JS_SetPropertyStr(ctx, global, "__sxnBuiltinNames", JS_NewCFunction(ctx, js_builtin_names, "__sxnBuiltinNames", 0));
    JS_SetPropertyStr(ctx, global, "__sxnIsBuiltin", JS_NewCFunction(ctx, js_is_builtin, "__sxnIsBuiltin", 1));
    JS_SetPropertyStr(ctx, global, "__sxnLatin1Bytes", JS_NewCFunctionMagic(ctx, js_buffer_encode_units, "__sxnLatin1Bytes", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "__sxnUtf16leBytes", JS_NewCFunctionMagic(ctx, js_buffer_encode_units, "__sxnUtf16leBytes", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "__sxnLatin1String", JS_NewCFunctionMagic(ctx, js_buffer_decode_units, "__sxnLatin1String", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "__sxnAsciiString", JS_NewCFunctionMagic(ctx, js_buffer_decode_units, "__sxnAsciiString", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "__sxnUtf16leString", JS_NewCFunctionMagic(ctx, js_buffer_decode_units, "__sxnUtf16leString", 1, JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx, global, "__sxnDeepEqual", JS_NewCFunctionMagic(ctx, js_deep_equal, "__sxnDeepEqual", 2, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "__sxnLooseDeepEqual", JS_NewCFunctionMagic(ctx, js_deep_equal, "__sxnLooseDeepEqual", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "__sxnFormat", JS_NewCFunction(ctx, js_util_format, "__sxnFormat", 3));
    JS_SetPropertyStr(ctx, global, "__sxnHexBytes", JS_NewCFunction(ctx, js_hex_bytes, "__sxnHexBytes", 1));
    JS_SetPropertyStr(ctx, global, "__sxnBase64Bytes", JS_NewCFunction(ctx, js_base64_bytes, "__sxnBase64Bytes", 1));
    JS_SetPropertyStr(ctx, global, "__sxnIsIP", JS_NewCFunction(ctx, js_net_is_ip, "isIP", 1));
    JS_SetPropertyStr(ctx, global, "__sxnQsParse", JS_NewCFunction(ctx, js_qs_parse, "parse", 4));
    JS_SetPropertyStr(ctx, global, "__sxnQsStringify", JS_NewCFunction(ctx, js_qs_stringify, "stringify", 3));
    JS_SetPropertyStr(ctx, global, "__sxnQsEscape", JS_NewCFunction(ctx, js_qs_escape, "escape", 1));
    JS_SetPropertyStr(ctx, global, "__sxnQsUnescape", JS_NewCFunction(ctx, js_qs_unescape, "unescape", 1));
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
    sxn_install_napi(ctx, uv_default_loop());

    JSValue result = JS_ReadObject(ctx, sxn_node_compat_bc, sxn_node_compat_bc_size,
                                   JS_READ_OBJ_BYTECODE);
    if (JS_IsException(result)) { JS_FreeValue(ctx, result); return -1; }
    if (js_module_set_import_meta(ctx, result, true, true) < 0) { JS_FreeValue(ctx, result); return -1; }
    result = JS_EvalFunction(ctx, result);
    if (JS_IsException(result)) { JS_FreeValue(ctx, result); return -1; }
    JS_FreeValue(ctx, result);
    sxn_install_buffer_natives(ctx);

    return 0;
}
