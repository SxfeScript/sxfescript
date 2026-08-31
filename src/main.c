#include "sxfe.h"
#include "quickjs.h"
#include "quickjs-libc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif

#ifdef _WIN32
/* _WIN32_WINNT is set by CMakeLists (needed before quickjs/cutils.h's own
   <windows.h> include, which locks the API level in on first inclusion). */
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#define strcasecmp _stricmp
#endif

/* Defined in network.c; declared here rather than in sxfe.h because that
   header deliberately avoids including quickjs.h and JSValue is a value
   type, not something that can be forward-declared. */
JSValue sxn_await_with_loop(JSContext *ctx, JSValue obj);

static bool suffix(const char *value, const char *ending) {
    size_t a = strlen(value), b = strlen(ending);
    return a >= b && !strcasecmp(value + a - b, ending);
}

static uint8_t *sxn_load_file(JSContext *ctx, size_t *length, const char *filename) {
    /* QuickJS's native parser understands SX syntax (safe/mut/interface,
       type annotations, &/&mut reference sigils) directly, so .sx sources
       load exactly like any other extension. The text-level compatibility
       transformer (sxfe_compile) is no longer wired into execution; it
       stays available as a standalone, independently-tested unit (see the
       sxfe-frontend ctest) but has no remaining call site here. */
    return js_load_file(ctx, length, filename);
}

static bool has_prefix(const char *value, const char *prefix) {
    return !strncmp(value, prefix, strlen(prefix));
}

/* QuickJS's default normalizer is internal, and installing our own replaces
   it, so the relative case has to be reproduced here: resolve "./" and "../"
   against the importing module's directory, and pass anything else through. */
static char *sxn_normalize_relative(JSContext *ctx, const char *base_name,
                                    const char *name) {
    if (name[0] != '.') return js_strdup(ctx, name);

    const char *r = strrchr(base_name, '/');
    size_t dlen = r ? (size_t)(r - base_name) : 0;
    size_t cap = dlen + strlen(name) + 2;
    char *out = js_malloc(ctx, cap);
    if (!out) return NULL;
    memcpy(out, base_name, dlen);
    out[dlen] = 0;

    r = name;
    for (;;) {
        if (r[0] == '.' && r[1] == '/') {
            r += 2;
        } else if (r[0] == '.' && r[1] == '.' && r[2] == '/') {
            char *p = strrchr(out, '/');
            if (!p) {
                if (out[0] == 0) break;   /* cannot climb above the root */
                p = out;
            }
            *p = 0;
            r += 3;
        } else {
            break;
        }
    }
    if (out[0] != 0) strncat(out, "/", cap - strlen(out) - 1);
    strncat(out, r, cap - strlen(out) - 1);
    return out;
}

/* ---- bare specifier resolution -------------------------------------------
   `import x from "pkg"` previously failed even with node_modules/pkg present,
   which meant nothing installed by `sxn install` could be imported. This walks
   up from the importing module the way Node does, and reads the package's own
   entry point rather than assuming index.js. */

/* Collapse "." and ".." segments in place. Without this a chain of relative
   requires accumulates "./" per hop, so the same file gets a different cache
   key each time and a cycle recurses until the stack gives out. */
static void sxn_norm_path(char *p) {
    char *out = p, *seg = p;
    size_t root_len = sxn_path_root_len(p);
    if (root_len) { out += root_len; seg += root_len; }
    char *w = out;
    while (*seg) {
        char *end = strchr(seg, '/');
        size_t n = end ? (size_t)(end - seg) : strlen(seg);
        if (n == 1 && seg[0] == '.') {
            /* drop */
        } else if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            if (w > out) {                       /* pop one written segment */
                char *back = w - 1;
                if (back > out && *back == '/') back--;
                while (back > out && *back != '/') back--;
                w = (back == out && *back != '/') ? out : back + (*back == '/' ? 1 : 0);
                if (w > out) w--;                /* drop the separator too */
            } else {
                if (w != out) *w++ = '/';
                memcpy(w, "..", 2); w += 2;
            }
        } else if (n > 0) {
            if (w != out) *w++ = '/';
            memmove(w, seg, n); w += n;
        }
        if (!end) break;
        seg = end + 1;
    }
    *w = 0;
    if (root_len == 1 && p[1] == 0 && w == out) { p[1] = 0; }
}

static bool sxn_is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

/* A file, or the same name with an extension appended, in this runtime's
   documented resolution order. Returns a fresh string or NULL. */
static char *sxn_resolve_file(JSContext *ctx, const char *base) {
    static const char *ext[] = { "", ".sx", ".mjs", ".js", ".cjs", ".json", ".node", ".ts" };
    char buf[PATH_MAX];
    for (size_t i = 0; i < sizeof(ext)/sizeof(ext[0]); i++) {
        if (snprintf(buf, sizeof(buf), "%s%s", base, ext[i]) >= (int)sizeof(buf)) continue;
        sxn_norm_path(buf);
        if (sxn_is_file(buf)) return js_strdup(ctx, buf);
    }
    /* a directory: index.<ext> */
    for (size_t i = 1; i < sizeof(ext)/sizeof(ext[0]); i++) {
        if (snprintf(buf, sizeof(buf), "%s/index%s", base, ext[i]) >= (int)sizeof(buf)) continue;
        sxn_norm_path(buf);
        if (sxn_is_file(buf)) return js_strdup(ctx, buf);
    }
    return NULL;
}

/* Walk an "exports" value down to a path string. Conditions nest -- tinybench
   ships {"import": {"types": ..., "import": "./dist/index.js"}} -- so this
   recurses, preferring the ESM conditions and ignoring "types", which is a
   declaration file rather than something to execute. */
static JSValue sxn_pick_condition(JSContext *ctx, JSValueConst node, int depth) {
    static const char *cond[] = { "import", "module", "default", "require", "node" };
    if (JS_IsString(node)) return JS_DupValue(ctx, node);
    if (!JS_IsObject(node) || depth > 4) return JS_UNDEFINED;
    for (size_t i = 0; i < sizeof(cond)/sizeof(cond[0]); i++) {
        JSValue v = JS_GetPropertyStr(ctx, node, cond[i]);
        JSValue got = sxn_pick_condition(ctx, v, depth + 1);
        JS_FreeValue(ctx, v);
        if (!JS_IsUndefined(got)) return got;
    }
    return JS_UNDEFINED;
}

/* Pull a usable entry path out of a package.json. Prefers "exports" for the
   "." subpath (string form, or an object keyed by condition, checking import
   then default then require), then "module", then "main". */
/* Node's rule for what a file is: .mjs/.mts are modules, .cjs is CommonJS,
   and everything else -- .js and the extensionless CLIs every npm package
   ships -- is decided by the nearest package.json's "type", defaulting to
   CommonJS. Treating every .js as a module is why `sxn ./node_modules/.bin/x`
   died on `exports is not defined`. The extensions this runtime owns, .sx and
   .ts, stay modules regardless: type stripping here is this project's own
   feature rather than Node's, and it has always been ESM. */
static bool sxn_entry_is_commonjs(JSContext *ctx, const char *filename) {
    if (suffix(filename, ".cjs")) return true;
    if (suffix(filename, ".mjs") || suffix(filename, ".mts") ||
        suffix(filename, ".sx") || suffix(filename, ".ts"))
        return false;

    char dir[PATH_MAX];
    if (sxn_path_is_absolute(filename)) snprintf(dir, sizeof(dir), "%s", filename);
    else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return true;
        snprintf(dir, sizeof(dir), "%s/%s", cwd, filename);
    }
    for (;;) {
        char *sl = strrchr(dir, '/');
        if (!sl || sl == dir) return true;          /* reached the root: CommonJS */
        *sl = 0;
        char manifest[PATH_MAX];
        if (snprintf(manifest, sizeof(manifest), "%s/package.json", dir) >= (int)sizeof(manifest))
            return true;
        size_t len = 0;
        uint8_t *buf = js_load_file(ctx, &len, manifest);
        if (!buf) continue;                          /* keep walking up */
        JSValue json = JS_ParseJSON(ctx, (const char *)buf, len, manifest);
        js_free(ctx, buf);
        if (JS_IsException(json)) { JS_FreeValue(ctx, json); return true; }
        JSValue type = JS_GetPropertyStr(ctx, json, "type");
        const char *t = JS_ToCString(ctx, type);
        bool cjs = !t || strcmp(t, "module") != 0;
        if (t) JS_FreeCString(ctx, t);
        JS_FreeValue(ctx, type);
        JS_FreeValue(ctx, json);
        return cjs;                                  /* nearest manifest decides */
    }
}

/* -------------------------------------------------------------------------
 * .sxbc: precompiled bytecode for a single entry file.
 *
 * A file this runtime is handed is normally source text, parsed fresh on
 * every launch. .sxbc skips that: `sxn compile` writes the parsed form once,
 * `sxn app.sxbc` (or `sxn --compile-cache app.js`, which writes and then
 * reads its own cache) loads it directly. The format is 5 bytes of this
 * runtime's own header -- so a version mismatch or a foreign file gets a
 * clear error instead of a QuickJS-internal one -- followed by whatever
 * JS_WriteObject produced.
 *
 * The one thing this format cannot skip is module resolution: a module
 * compiled ahead of time still has to have its `import`s resolved against
 * the loader when it's read back, because that resolution didn't happen at
 * parse time the way it does for JS_Eval'd source. JS_ResolveModule is that
 * step; the module case below is JS_ReadObject, JS_ResolveModule,
 * js_module_set_import_meta, JS_EvalFunction, in that order -- the same
 * sequence quickjs-libc's own js_std_eval_binary uses for a compiled module,
 * just with our own dispatch on top of it. */
#define SXN_SXBC_MAGIC "SXB1"
#define SXN_SXBC_KIND_MODULE 0
#define SXN_SXBC_KIND_CJS 1

/* The CommonJS wrapper text, exactly what sxn_require_fn wraps a source file
   in below -- kept here too so a compiled .cjs function can be called with
   the same five arguments require() gives it. */
static const char *sxn_cjs_wrap_pre =
    "(function (exports, require, module, __filename, __dirname) {";
static const char *sxn_cjs_wrap_post = "\n});";

/* Defined below (needs the require() machinery); used by sxn_run_sxbc for a compiled CommonJS entry. */
static JSValue sxn_make_require(JSContext *ctx, const char *dir);

static int sxn_write_sxbc(const char *path, uint8_t kind, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(SXN_SXBC_MAGIC, 1, 4, f) == 4 &&
             fwrite(&kind, 1, 1, f) == 1 &&
             (len == 0 || fwrite(buf, 1, len, f) == len);
    if (fclose(f) != 0) ok = 0;
    if (!ok) unlink(path);
    return ok ? 0 : -1;
}

/* Derives `out.sxbc` from a source filename: replaces a recognized source
   extension, or appends `.sxbc` to anything else (an extensionless CLI, or
   a name that's already unfamiliar) rather than guessing wrong. */
static void sxn_sxbc_default_path(char *out, size_t out_size, const char *src) {
    static const char *known[] = { ".sx", ".ts", ".mjs", ".mts", ".cjs", ".js" };
    size_t len = strlen(src);
    for (size_t i = 0; i < sizeof(known)/sizeof(known[0]); i++) {
        size_t k = strlen(known[i]);
        if (len >= k && !strcasecmp(src + len - k, known[i])) {
            snprintf(out, out_size, "%.*s.sxbc", (int)(len - k), src);
            return;
        }
    }
    snprintf(out, out_size, "%s.sxbc", src);
}

/* Compiles `filename` (module or CommonJS, exactly as execute_file would run
   it) to bytecode and writes it to `out_path`. Returns 0 on success; on
   failure, prints the parse error and returns -1. `context` must not yet
   have run anything the compiled code depends on -- compiling is a pure
   parse, so this is safe to call before or after normal setup. */
static int sxn_compile_file(JSContext *ctx, const char *filename, const char *out_path,
                            bool strip) {
    size_t len = 0;
    uint8_t *src = js_load_file(ctx, &len, filename);
    if (!src) {
        fprintf(stderr, "sxn: cannot open '%s': %s\n", filename, strerror(errno));
        return -1;
    }
    /* JS_WRITE_OBJ_STRIP_SOURCE/STRIP_DEBUG drop the line-number and local-
       variable tables, but not the compiled function's own script name --
       that's load-bearing (import.meta.url, module identity) and stays
       embedded regardless. So --strip's actual privacy job is done here: the
       name QuickJS embeds is whatever this call passes as `filename`, and
       for a stripped build that's the bare basename, not the path the
       compiling machine happens to keep the source at. */
    const char *eval_name = filename;
    if (strip) {
        const char *b = strrchr(filename, '/');
        eval_name = b ? b + 1 : filename;
    }
    bool cjs = sxn_entry_is_commonjs(ctx, filename);
    JSValue compiled;
    if (cjs) {
        if (len >= 2 && src[0] == '#' && src[1] == '!') {
            size_t i = 0;
            while (i < len && src[i] != '\n') src[i++] = ' ';
        }
        size_t total = strlen(sxn_cjs_wrap_pre) + len + strlen(sxn_cjs_wrap_post) + 1;
        char *code = js_malloc(ctx, total);
        if (!code) { js_free(ctx, src); return -1; }
        snprintf(code, total, "%s%.*s%s", sxn_cjs_wrap_pre, (int)len, (const char *)src,
                 sxn_cjs_wrap_post);
        js_free(ctx, src);
        compiled = JS_Eval(ctx, code, strlen(code), eval_name,
                           JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        js_free(ctx, code);
    } else {
        compiled = JS_Eval(ctx, (const char *)src, len, eval_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        js_free(ctx, src);
    }
    if (JS_IsException(compiled)) {
        js_std_dump_error(ctx);
        JS_FreeValue(ctx, compiled);
        return -1;
    }
    size_t bc_len = 0;
    /* Line/variable tables gone too, on top of the bare filename above: a
       stripped error reports a bytecode offset, not a source line. */
    int write_flags = JS_WRITE_OBJ_BYTECODE |
        (strip ? (JS_WRITE_OBJ_STRIP_SOURCE | JS_WRITE_OBJ_STRIP_DEBUG) : 0);
    uint8_t *bc = JS_WriteObject(ctx, &bc_len, compiled, write_flags);
    JS_FreeValue(ctx, compiled);
    if (!bc) {
        fprintf(stderr, "sxn: cannot serialize '%s' to bytecode\n", filename);
        return -1;
    }
    int rc = sxn_write_sxbc(out_path, cjs ? SXN_SXBC_KIND_CJS : SXN_SXBC_KIND_MODULE, bc, bc_len);
    js_free(ctx, bc);
    if (rc != 0) {
        fprintf(stderr, "sxn: cannot write '%s': %s\n", out_path, strerror(errno));
        return -1;
    }
    return 0;
}

/* Loads and evaluates a .sxbc file as the entry point. Returns the value
   execute_file's normal post-processing (top-level await, error dump) should
   receive -- a promise for a module, or the require()-equivalent
   module.exports for a compiled CommonJS function -- or JS_EXCEPTION with
   the error already reported. */
static JSValue sxn_run_sxbc(JSContext *ctx, const char *filename) {
    size_t len = 0;
    uint8_t *buf = js_load_file(ctx, &len, filename);
    if (!buf) {
        fprintf(stderr, "sxn: cannot open '%s': %s\n", filename, strerror(errno));
        return JS_EXCEPTION;
    }
    if (len < 5 || memcmp(buf, SXN_SXBC_MAGIC, 4) != 0) {
        js_free(ctx, buf);
        fprintf(stderr, "sxn: '%s' is not a recognized .sxbc file "
                        "(wrong format, or compiled by a different version -- recompile it)\n",
                filename);
        return JS_EXCEPTION;
    }
    uint8_t kind = buf[4];
    JSValue obj = JS_ReadObject(ctx, buf + 5, len - 5, JS_READ_OBJ_BYTECODE);
    js_free(ctx, buf);
    if (JS_IsException(obj)) { js_std_dump_error(ctx); return JS_EXCEPTION; }

    if (kind == SXN_SXBC_KIND_MODULE) {
        /* use_realpath=false, matching quickjs-libc's own js_std_eval_binary
           for a compiled module: the embedded name may not exist on disk at
           this exact path any more -- --strip made sure of that on purpose,
           and even an unstripped .sxbc is meant to run somewhere other than
           the machine that compiled it. realpath() would either fail loudly
           (this crashed with "realpath failure" before the fix) or silently
           resolve to the wrong file if something else now sits at that path. */
        if (JS_ResolveModule(ctx, obj) < 0 ||
            js_module_set_import_meta(ctx, obj, false, false) < 0) {
            js_std_dump_error(ctx);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        return JS_EvalFunction(ctx, obj);
    }

    if (kind != SXN_SXBC_KIND_CJS) {
        JS_FreeValue(ctx, obj);
        fprintf(stderr, "sxn: '%s' has an unrecognized .sxbc kind byte -- recompile it\n", filename);
        return JS_EXCEPTION;
    }

    /* `obj` right now is the *compiled top-level script* "(function(...) {
       ... });", not the function itself -- getting the function out means
       running that script, the same way JS_Eval without COMPILE_ONLY would
       have. Its completion value (an expression statement's value) is the
       function object, which is what compiling the .cjs source, rather than
       calling JS_Eval on it directly, deferred to here. */
    obj = JS_EvalFunction(ctx, obj);
    if (JS_IsException(obj)) { js_std_dump_error(ctx); return JS_EXCEPTION; }

    char abs[PATH_MAX], dir[PATH_MAX];
    if (sxn_path_is_absolute(filename)) snprintf(abs, sizeof(abs), "%s", filename);
    else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
        snprintf(abs, sizeof(abs), "%s/%s", cwd, filename);
    }
    snprintf(dir, sizeof(dir), "%s", abs);
    char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else snprintf(dir, sizeof(dir), ".");

    JSValue module_obj = JS_NewObject(ctx);
    JSValue exports = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, module_obj, "exports", JS_DupValue(ctx, exports));
    JSValue req = sxn_make_require(ctx, dir);
    JSValueConst args[5] = { exports, req, module_obj, JS_NewString(ctx, abs), JS_NewString(ctx, dir) };
    JSValue res = JS_Call(ctx, obj, exports, 5, args);
    JS_FreeValue(ctx, (JSValue)args[3]);
    JS_FreeValue(ctx, (JSValue)args[4]);
    JS_FreeValue(ctx, req);
    JS_FreeValue(ctx, obj);
    if (JS_IsException(res)) {
        js_std_dump_error(ctx);
        JS_FreeValue(ctx, module_obj); JS_FreeValue(ctx, exports);
        return res;
    }
    JS_FreeValue(ctx, res);
    JSValue final = JS_GetPropertyStr(ctx, module_obj, "exports");
    JS_FreeValue(ctx, module_obj); JS_FreeValue(ctx, exports);
    return final;
}

/* Checks whether `cache_path` is a valid, up-to-date .sxbc for `source_path`
   -- exists, has this runtime's magic header, and is not older than the
   source. A source rewritten after its cache was written must not be served
   stale bytecode; mtime is the same freshness test `make` uses and is cheap
   enough to pay on every launch. */
static bool sxn_sxbc_cache_is_fresh(const char *cache_path, const char *source_path) {
    struct stat cache_st, src_st;
    if (stat(cache_path, &cache_st) != 0 || stat(source_path, &src_st) != 0) return false;
    if (cache_st.st_mtime < src_st.st_mtime) return false;
    FILE *f = fopen(cache_path, "rb");
    if (!f) return false;
    char magic[4];
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, SXN_SXBC_MAGIC, 4) == 0;
    fclose(f);
    return ok;
}

static char *sxn_pkg_entry(JSContext *ctx, const char *pkg_dir) {
    char manifest[PATH_MAX], cand[PATH_MAX];
    if (snprintf(manifest, sizeof(manifest), "%s/package.json", pkg_dir) >= (int)sizeof(manifest))
        return NULL;
    size_t len = 0;
    uint8_t *buf = js_load_file(ctx, &len, manifest);
    if (!buf) return NULL;
    JSValue json = JS_ParseJSON(ctx, (const char *)buf, len, manifest);
    js_free(ctx, buf);
    if (JS_IsException(json)) { JS_FreeValue(ctx, json); return NULL; }

    const char *rel = NULL;
    JSValue hold = JS_UNDEFINED;
    JSValue exports = JS_GetPropertyStr(ctx, json, "exports");
    if (JS_IsString(exports) || JS_IsObject(exports)) {
        JSValue dot = JS_GetPropertyStr(ctx, exports, ".");
        JSValue pick = JS_IsUndefined(dot) ? JS_DupValue(ctx, exports) : JS_DupValue(ctx, dot);
        JS_FreeValue(ctx, dot);
        hold = sxn_pick_condition(ctx, pick, 0);
        JS_FreeValue(ctx, pick);
    }
    JS_FreeValue(ctx, exports);
    if (JS_IsUndefined(hold)) {
        JSValue mod = JS_GetPropertyStr(ctx, json, "module");
        if (JS_IsString(mod)) hold = JS_DupValue(ctx, mod);
        JS_FreeValue(ctx, mod);
    }
    if (JS_IsUndefined(hold)) {
        JSValue main = JS_GetPropertyStr(ctx, json, "main");
        if (JS_IsString(main)) hold = JS_DupValue(ctx, main);
        JS_FreeValue(ctx, main);
    }
    char *out = NULL;
    if (JS_IsString(hold)) {
        rel = JS_ToCString(ctx, hold);
        if (rel) {
            const char *r = rel;
            while (r[0] == '.' && r[1] == '/') r += 2;
            if (snprintf(cand, sizeof(cand), "%s/%s", pkg_dir, r) < (int)sizeof(cand))
                out = sxn_resolve_file(ctx, cand);
            JS_FreeCString(ctx, rel);
        }
    }
    JS_FreeValue(ctx, hold);
    JS_FreeValue(ctx, json);
    if (!out) out = sxn_resolve_file(ctx, pkg_dir);   /* fall back to index.* */
    return out;
}

static char *sxn_module_normalize(JSContext *ctx, const char *base_name,
                                  const char *name, void *opaque);

/* ---- CommonJS ------------------------------------------------------------
   Most of npm is still CJS, and `require` was simply undefined. Each module
   gets its own require bound to its own directory, so relative specifiers
   resolve against the file doing the requiring rather than the process cwd,
   and results are cached by resolved path so a diamond dependency is
   evaluated once and shares one exports object. */

static JSValue sxn_cjs_cache(JSContext *ctx) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue cache = JS_GetPropertyStr(ctx, g, "__sxnCjsCache");
    if (!JS_IsObject(cache)) {
        JS_FreeValue(ctx, cache);
        cache = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, g, "__sxnCjsCache", JS_DupValue(ctx, cache), 0);
    }
    JS_FreeValue(ctx, g);
    return cache;
}

static JSValue sxn_make_require(JSContext *ctx, const char *dir);

/* Resolve a specifier the way require() does: relative and absolute paths by
   extension, bare names through node_modules. Returns a fresh string. */
static char *sxn_require_resolve(JSContext *ctx, const char *dir, const char *name) {
    char buf[PATH_MAX];
    if (name[0] == '.' || sxn_path_is_absolute(name)) {
        bool name_abs = sxn_path_is_absolute(name);
        if (snprintf(buf, sizeof(buf), name_abs ? "%s%s" : "%s/%s",
                     name_abs ? "" : dir, name) >= (int)sizeof(buf))
            return NULL;
        return sxn_resolve_file(ctx, buf);
    }
    /* Bare: reuse the same walk the ESM normalizer does. */
    char base[PATH_MAX];
    if (snprintf(base, sizeof(base), "%s/x", dir) >= (int)sizeof(base)) return NULL;
    char *hit = sxn_module_normalize(ctx, base, name, NULL);
    if (hit && sxn_is_file(hit)) return hit;
    js_free(ctx, hit);
    return NULL;
}

static JSValue sxn_require_fn(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv,
                              int magic, JSValueConst *func_data) {
    (void)this_val; (void)magic;
    if (argc < 1) return JS_ThrowTypeError(ctx, "require needs a specifier");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;

    /* Builtins are reachable by bare name as well as with the node: prefix,
       and require() is synchronous so it cannot go through import(). The JS
       side owns the mapping; ask it first, and only fall through to
       node_modules when it says the name is not a builtin. */
    {
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue isb = JS_GetPropertyStr(ctx, g, "__sxnIsBuiltin");
        bool builtin = false;
        if (JS_IsFunction(ctx, isb)) {
            JSValueConst a[1] = { argv[0] };
            JSValue r = JS_Call(ctx, isb, JS_UNDEFINED, 1, a);
            builtin = JS_ToBool(ctx, r);
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, isb);
        JS_FreeValue(ctx, g);
        if (builtin) {
            JSValue g2 = JS_GetGlobalObject(ctx);
            JSValue reg = JS_GetPropertyStr(ctx, g2, "__sxnBuiltinRequire");
            JS_FreeValue(ctx, g2);
            JSValueConst a[1] = { argv[0] };
            JSValue r = JS_Call(ctx, reg, JS_UNDEFINED, 1, a);
            JS_FreeValue(ctx, reg);
            JS_FreeCString(ctx, name);
            return r;
        }
    }
    if (has_prefix(name, "node:") || has_prefix(name, "qjs:")) {
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue reg = JS_GetPropertyStr(ctx, g, "__sxnBuiltinRequire");
        JS_FreeValue(ctx, g);
        if (JS_IsFunction(ctx, reg)) {
            JSValueConst a[1] = { argv[0] };
            JSValue r = JS_Call(ctx, reg, JS_UNDEFINED, 1, a);
            JS_FreeValue(ctx, reg);
            JS_FreeCString(ctx, name);
            return r;
        }
        JS_FreeValue(ctx, reg);
        JSValue e = JS_ThrowReferenceError(ctx, "require('%s') is not available; use import", name);
        JS_FreeCString(ctx, name);
        return e;
    }

    const char *dir = JS_ToCString(ctx, func_data[0]);
    char *path = dir ? sxn_require_resolve(ctx, dir, name) : NULL;
    if (dir) JS_FreeCString(ctx, dir);
    if (!path) {
        /* Node throws a plain Error with code MODULE_NOT_FOUND here, and
           code is what callers branch on. */
        JSValue err = JS_NewError(ctx);
        char msg[PATH_MAX + 32];
        snprintf(msg, sizeof(msg), "Cannot find module '%s'", name);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg));
        JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, "MODULE_NOT_FOUND"));
        JS_FreeCString(ctx, name);
        return JS_Throw(ctx, err);
    }
    JS_FreeCString(ctx, name);

    JSValue cache = sxn_cjs_cache(ctx);
    JSValue hit = JS_GetPropertyStr(ctx, cache, path);
    if (JS_IsObject(hit)) {          /* already loaded: same exports object */
        JSValue ex = JS_GetPropertyStr(ctx, hit, "exports");
        JS_FreeValue(ctx, hit); JS_FreeValue(ctx, cache); js_free(ctx, path);
        return ex;
    }
    JS_FreeValue(ctx, hit);

    /* A .node file is a compiled addon, not source: hand it to
       process.dlopen, which is where the Node-API loader lives. It is only
       present when the node: layer installed one. */
    if (suffix(path, ".node")) {
        JSValue module_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, module_obj, "exports", JS_NewObject(ctx));
        JS_SetPropertyStr(ctx, module_obj, "id", JS_NewString(ctx, path));
        JS_SetPropertyStr(ctx, module_obj, "filename", JS_NewString(ctx, path));
        JS_SetPropertyStr(ctx, cache, path, JS_DupValue(ctx, module_obj));

        JSValue global = JS_GetGlobalObject(ctx);
        JSValue process = JS_GetPropertyStr(ctx, global, "process");
        JSValue dlopen = JS_GetPropertyStr(ctx, process, "dlopen");
        JS_FreeValue(ctx, global);
        JSValue result;
        if (!JS_IsFunction(ctx, dlopen)) {
            result = JS_ThrowTypeError(ctx,
                "cannot load '%s': native addons need the node compatibility layer", path);
        } else {
            JSValue file = JS_NewString(ctx, path);
            JSValueConst args[2] = { module_obj, file };
            result = JS_Call(ctx, dlopen, process, 2, args);
            JS_FreeValue(ctx, file);
        }
        JS_FreeValue(ctx, dlopen);
        JS_FreeValue(ctx, process);
        if (JS_IsException(result)) {
            JSAtom a = JS_NewAtom(ctx, path);
            JS_DeleteProperty(ctx, cache, a, 0);
            JS_FreeAtom(ctx, a);
            JS_FreeValue(ctx, module_obj); JS_FreeValue(ctx, cache); js_free(ctx, path);
            return result;
        }
        JS_FreeValue(ctx, result);
        JSValue exports = JS_GetPropertyStr(ctx, module_obj, "exports");
        JS_FreeValue(ctx, module_obj); JS_FreeValue(ctx, cache); js_free(ctx, path);
        return exports;
    }

    size_t len = 0;
    uint8_t *src = js_load_file(ctx, &len, path);
    if (!src) {
        JS_FreeValue(ctx, cache);
        JSValue e = JS_ThrowReferenceError(ctx, "cannot read module '%s'", path);
        js_free(ctx, path);
        return e;
    }

    /* JSON is a module too, and needs no wrapper. */
    if (suffix(path, ".json")) {
        JSValue v = JS_ParseJSON(ctx, (const char *)src, len, path);
        js_free(ctx, src);
        if (!JS_IsException(v)) {
            JSValue rec = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, rec, "exports", JS_DupValue(ctx, v));
            JS_SetPropertyStr(ctx, cache, path, rec);
        }
        JS_FreeValue(ctx, cache); js_free(ctx, path);
        return v;
    }

    /* module and exports go in the cache before evaluating, so a cycle sees
       the partially-filled exports rather than recursing forever. */
    JSValue module_obj = JS_NewObject(ctx);
    JSValue exports = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, module_obj, "exports", JS_DupValue(ctx, exports));
    JS_SetPropertyStr(ctx, module_obj, "id", JS_NewString(ctx, path));
    JS_SetPropertyStr(ctx, cache, path, JS_DupValue(ctx, module_obj));

    char mdir[PATH_MAX];
    snprintf(mdir, sizeof(mdir), "%s", path);
    char *sl = strrchr(mdir, '/'); if (sl) *sl = 0; else snprintf(mdir, sizeof(mdir), ".");

    /* A leading #! line is not JavaScript. Node strips it from every file it
       loads, which is what makes the extensionless CLIs in node_modules/.bin
       runnable at all. Blank it rather than remove it, so reported line
       numbers still match the file. */
    if (len >= 2 && src[0] == '#' && src[1] == '!') {
        size_t i = 0;
        while (i < len && src[i] != '\n') src[i++] = ' ';
    }

    /* Wrap exactly the way Node does, so the file's own `this` is exports. */
    static const char *pre = "(function (exports, require, module, __filename, __dirname) {";
    static const char *post = "\n});";
    size_t total = strlen(pre) + len + strlen(post) + 1;
    char *code = js_malloc(ctx, total);
    if (!code) { js_free(ctx, src); JS_FreeValue(ctx, cache); js_free(ctx, path);
                 JS_FreeValue(ctx, module_obj); JS_FreeValue(ctx, exports); return JS_EXCEPTION; }
    snprintf(code, total, "%s%.*s%s", pre, (int)len, (const char *)src, post);
    js_free(ctx, src);

    JSValue fn = JS_Eval(ctx, code, strlen(code), path, JS_EVAL_TYPE_GLOBAL);
    js_free(ctx, code);
    if (JS_IsException(fn)) {
        JS_FreeValue(ctx, cache); js_free(ctx, path);
        JS_FreeValue(ctx, module_obj); JS_FreeValue(ctx, exports);
        return fn;
    }
    JSValue req = sxn_make_require(ctx, mdir);
    JSValueConst args[5] = { exports, req, module_obj,
                             JS_NewString(ctx, path), JS_NewString(ctx, mdir) };
    JSValue res = JS_Call(ctx, fn, exports, 5, args);
    JS_FreeValue(ctx, (JSValue)args[3]);
    JS_FreeValue(ctx, (JSValue)args[4]);
    JS_FreeValue(ctx, req);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(res)) {
        JS_DeleteProperty(ctx, cache, JS_NewAtom(ctx, path), 0);
        JS_FreeValue(ctx, cache); js_free(ctx, path);
        JS_FreeValue(ctx, module_obj); JS_FreeValue(ctx, exports);
        return res;
    }
    JS_FreeValue(ctx, res);
    /* module.exports may have been replaced wholesale. */
    JSValue final = JS_GetPropertyStr(ctx, module_obj, "exports");
    JS_FreeValue(ctx, module_obj); JS_FreeValue(ctx, exports);
    JS_FreeValue(ctx, cache); js_free(ctx, path);
    return final;
}

/* require.resolve(spec): the path require() would load, without loading it.
   Packages use it to find a sibling's files -- Next reads styled-jsx's
   package.json this way before it will start. */
static JSValue sxn_require_resolve_fn(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv,
                                      int magic, JSValueConst *func_data) {
    (void)this_val; (void)magic;
    if (argc < 1) return JS_ThrowTypeError(ctx, "require.resolve needs a specifier");
    const char *dir = JS_ToCString(ctx, func_data[0]);
    const char *name = JS_ToCString(ctx, argv[0]);
    JSValue out = JS_EXCEPTION;
    if (dir && name) {
        char *path = sxn_require_resolve(ctx, dir, name);
        if (path) { out = JS_NewString(ctx, path); js_free(ctx, path); }
        else out = JS_ThrowReferenceError(ctx, "cannot resolve module '%s'", name);
    }
    if (dir) JS_FreeCString(ctx, dir);
    if (name) JS_FreeCString(ctx, name);
    return out;
}

/* module.createRequire(path): a require rooted at that path's directory,
   rather than the entry file's. ESM code uses it to reach CommonJS, and a
   require anchored at the wrong directory resolves the wrong siblings. */
static JSValue sxn_make_require_fn(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    const char *from = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    char dir[PATH_MAX];
    if (!from) return JS_ThrowTypeError(ctx, "createRequire needs a path or file: URL");
    const char *p = has_prefix(from, "file://") ? from + 7 : from;
    snprintf(dir, sizeof(dir), "%s", p);
    JS_FreeCString(ctx, from);
    /* Node accepts the file itself or its directory; both anchor at the
       directory, and a trailing slash means it was already one. */
    if (sxn_is_file(dir)) {
        char *sl = strrchr(dir, '/');
        if (sl && sl != dir) *sl = 0;
    }
    return sxn_make_require(ctx, dir);
}

static JSValue sxn_make_require(JSContext *ctx, const char *dir) {
    JSValue d = JS_NewString(ctx, dir);
    JSValueConst data[1] = { d };
    JSValue fn = JS_NewCFunctionData(ctx, sxn_require_fn, 1, 0, 1, data);
    JS_SetPropertyStr(ctx, fn, "resolve",
                      JS_NewCFunctionData(ctx, sxn_require_resolve_fn, 1, 0, 1, data));
    JS_FreeValue(ctx, d);
    return fn;
}

static char *sxn_module_normalize(JSContext *ctx, const char *base_name,
                                  const char *name, void *opaque) {
    (void)opaque;
    /* Builtins and relative/absolute paths keep the stock behaviour. */
    if (has_prefix(name, "node:") || has_prefix(name, "qjs:") ||
        name[0] == '.' || sxn_path_is_absolute(name))
        return sxn_normalize_relative(ctx, base_name, name);

    /* A bare specifier: "pkg", "@scope/pkg", or either with a subpath. Walk up
       from the importing module's directory looking for node_modules. */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", base_name);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else snprintf(dir, sizeof(dir), ".");

    for (;;) {
        char pkg[PATH_MAX];
        if (snprintf(pkg, sizeof(pkg), "%s/node_modules/%s", dir, name) < (int)sizeof(pkg)) {
            /* A subpath ("pkg/lib/x") resolves as a file inside the package;
               a bare package name resolves through its manifest. */
            /* The package name is one segment, or two when scoped, so a
               subpath is whatever follows that. "@scope/pkg" is a bare name;
               "@scope/pkg/x" and "pkg/x" carry subpaths. */
            const char *after = name;
            if (name[0] == '@') {
                const char *sep = strchr(name + 1, '/');
                after = sep ? sep + 1 : name + strlen(name);
            }
            bool has_subpath = strchr(after, '/') != NULL;
            char *hit = has_subpath ? sxn_resolve_file(ctx, pkg) : sxn_pkg_entry(ctx, pkg);
            if (!hit && !has_subpath) hit = sxn_resolve_file(ctx, pkg);
            if (hit) return hit;
        }
        char *up = strrchr(dir, '/');
        if (!up) break;
        *up = 0;
        if (dir[0] == 0) break;
    }
    /* Unresolved: hand back the name so the loader reports it the usual way. */
    return sxn_normalize_relative(ctx, base_name, name);
}

static JSModuleDef *sxn_module_loader(JSContext *ctx, const char *name, void *opaque,
                                      JSValueConst attributes) {
    /* node:buffer/path/events/process/fs/fs-promises are pre-registered by
       sxn_install_node_compat via JS_NewCModule (same mechanism as
       qjs:std/qjs:os/qjs:bjson below), so `import ... from "node:xxx"`
       resolves to them without ever reaching this loader. Only an
       unregistered node: specifier gets here -- report it clearly instead
       of falling through to file-based resolution, which would otherwise
       try (and fail confusingly) to open a file literally named "node:xxx". */
    if (has_prefix(name, "node:")) {
        JS_ThrowReferenceError(ctx, "unsupported node: module '%s'", name);
        return NULL;
    }
    if (!suffix(name, ".sx")) return js_module_loader(ctx, name, opaque, attributes);
    return js_module_load(ctx, name, opaque, attributes, sxn_load_file);
}


/* Bytes of JS stack to allow, derived from the thread's actual stack limit.
   Reserves 2MB for native frames beyond the interpreter's own checks, and
   never returns less than QuickJS's 1MB default. */
static size_t sxn_js_stack_budget(void) {
    const size_t reserve = 2u * 1024 * 1024;
    const size_t fallback = 1u * 1024 * 1024;
#ifdef _WIN32
    /* getrlimit's equivalent: the current thread's actual reserved stack
       region, not the PE header's default (which GetCurrentThreadStackLimits
       reports correctly even when the linker or a caller changed it). */
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    size_t real = (size_t)(high - low);
    if (real > reserve + fallback) return real - reserve;
#else
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 &&
        rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > reserve + fallback)
        return (size_t)rl.rlim_cur - reserve;
#endif
    return fallback;
}

/* Defined in src/napi.c; a no-op when no addon was ever loaded. */
void sxn_shutdown_napi(JSContext *ctx);

static int execute_file(int argc, char **argv, const char *filename,
                        int arg_offset, bool memory_report, bool leak_check) {
    size_t length = 0;
    uint8_t *source = NULL;
    JSRuntime *runtime = JS_NewRuntime();
    if (!runtime) { fputs("sxn: unable to create QuickJS runtime\n", stderr); return 2; }
    /* QuickJS's default 256KB threshold is tuned for a bare interpreter;
       bootstrap.js + node_compat.js's one-time class/closure setup alone
       gets within reach of it, which used to trigger a spurious cyclic GC
       during startup (before any user code ran) on every invocation. Give
       it real headroom rather than let that budget keep shrinking as more
       builtin JS surface (node:* compat, future tasks) is added. */
    JS_SetGCThreshold(runtime, 8 * 1024 * 1024);
    /* QuickJS defaults its JS stack budget to 1MB regardless of how much
       stack the OS actually gave the thread, which capped recursion at ~948
       frames here against Node's ~8900 -- deep enough to break ordinary
       recursive code (tree walks, recursive-descent parsers, nested JSON).
       Size it from the real limit instead, keeping a 2MB reserve so the
       clean RangeError still fires well before the actual stack runs out:
       native builtins can descend several C frames between the interpreter's
       overflow checks, and overshooting that is a crash rather than an
       exception. */
    JS_SetMaxStackSize(runtime, sxn_js_stack_budget());
    if (leak_check) JS_SetDumpFlags(runtime, JS_ABORT_ON_LEAKS | JS_DUMP_MEM);
    if (getenv("SXN_DUMP_BYTECODE")) JS_SetDumpFlags(runtime, JS_GetDumpFlags(runtime) | JS_DUMP_BYTECODE_FINAL);
    if (getenv("SXN_DUMP_LEAKS")) JS_SetDumpFlags(runtime, JS_GetDumpFlags(runtime) | JS_DUMP_LEAKS);
    js_std_init_handlers(runtime);
    JSContext *context = JS_NewContext(runtime);
    if (!context) { JS_FreeRuntime(runtime); return 2; }
    js_init_module_std(context, "qjs:std");
    js_init_module_os(context, "qjs:os");
    js_init_module_bjson(context, "qjs:bjson");
    /* process.argv is [execPath, scriptPath, ...userArgs]; arg_offset is
       how many leading argv entries (argv[0] plus any --flag this runtime
       consumed for itself, e.g. --compile-cache) aren't part of that, so a
       flag before the script name doesn't leak into the script's own argv
       the way it did when this was hardcoded to skip exactly one. */
    js_std_add_helpers(context, argc - arg_offset, argv + arg_offset);
    if (sxn_install_network(context) != 0) {
        fputs("sxn: unable to initialize network runtime\n", stderr);
        goto failure;
    }
    if (sxn_install_node_compat(context, argv[0]) != 0) {
        fputs("sxn: unable to initialize node compatibility layer\n", stderr);
        goto failure;
    }
    JS_SetModuleLoaderFunc2(runtime, sxn_module_normalize, sxn_module_loader, js_module_check_attributes, NULL);
    JS_SetHostPromiseRejectionTracker(runtime, js_std_promise_rejection_tracker, NULL);

    bool is_sxbc = suffix(filename, ".sxbc");
    if (!is_sxbc) {
        source = sxn_load_file(context, &length, filename);
        if (!source) {
            /* js_load_file reports failure without setting an exception, so
               dumping one printed "[uninitialized]" instead of naming the file. */
            fprintf(stderr, "sxn: cannot open '%s': %s\n", filename, strerror(errno));
            goto failure;
        }
    }
    /* A CommonJS entry point needs require/module/exports/__dirname in scope,
       and every module needs a require bound to its own directory. Install a
       global one anchored at the entry file's directory so ESM can use it too,
       the way Node's createRequire is used. A .sxbc entry needs the same
       global, since bytecode compiled from a module can call require() too. */
    {
        char entry_dir[PATH_MAX];
        snprintf(entry_dir, sizeof(entry_dir), "%s", filename);
        char *sl = strrchr(entry_dir, '/');
        if (sl) *sl = 0; else snprintf(entry_dir, sizeof(entry_dir), ".");
        JSValue g = JS_GetGlobalObject(context);
        JS_SetPropertyStr(context, g, "require", sxn_make_require(context, entry_dir));
        JS_SetPropertyStr(context, g, "__sxnMakeRequire",
                          JS_NewCFunction(context, sxn_make_require_fn, "createRequire", 1));
        JS_FreeValue(context, g);
    }
    JSValue value;
    if (is_sxbc) {
        /* Precompiled bytecode: no parse at all, just load and run it.
           sxn_run_sxbc reports its own errors (a bad file, or a real
           exception from loading/resolving/evaluating it) on the way out,
           so a failure here skips the shared dump-error below rather than
           printing it twice. */
        value = sxn_run_sxbc(context, filename);
        if (JS_IsException(value)) { JS_FreeValue(context, value); goto failure; }
    } else {
        int flags = sxn_entry_is_commonjs(context, filename) ? JS_EVAL_TYPE_GLOBAL
                                                             : JS_EVAL_TYPE_MODULE;
        if (flags == JS_EVAL_TYPE_GLOBAL) {
            /* Run the entry through require() rather than as bare global code, so
               it gets the same wrapper every other CommonJS module gets and has
               module, exports, __filename and __dirname in scope. */
            js_free(context, source);
            source = NULL;
            char abs[PATH_MAX];
            if (sxn_path_is_absolute(filename)) snprintf(abs, sizeof(abs), "%s", filename);
            else {
                char cwd[PATH_MAX];
                if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
                snprintf(abs, sizeof(abs), "%s/%s", cwd, filename);
            }
            JSValue g = JS_GetGlobalObject(context);
            JSValue req = JS_GetPropertyStr(context, g, "require");
            JSValue spec = JS_NewString(context, abs);
            JSValueConst a[1] = { spec };
            value = JS_Call(context, req, JS_UNDEFINED, 1, a);
            JS_FreeValue(context, spec);
            JS_FreeValue(context, req);
            JS_FreeValue(context, g);
        } else
        value = JS_Eval(context, (const char *)source, length, filename,
                                flags | JS_EVAL_FLAG_COMPILE_ONLY);
        if (source) { js_free(context, source); source = NULL; }
        if (!JS_IsException(value) && flags == JS_EVAL_TYPE_MODULE) {
            if (js_module_set_import_meta(context, value, true, true) < 0) {
                JS_FreeValue(context, value); value = JS_EXCEPTION;
            } else value = JS_EvalFunction(context, value);
        }
    }
    /* A module with top-level await evaluates to a pending promise. Awaiting
       it has to drive the uv loop too, or any await on a timer, a fetch or a
       server never resumes. */
    if (!JS_IsException(value)) value = sxn_await_with_loop(context, value);
    if (JS_IsException(value)) { js_std_dump_error(context); JS_FreeValue(context, value); goto failure; }
    JS_FreeValue(context, value);
    if (js_std_loop(context)) { js_std_dump_error(context); goto failure; }
    /* Drains any server sockets / async file reads registered by network.c;
       a no-op that returns immediately for scripts that never called
       Sxn.serve or Sxn.file(...).text(). */
    if (sxn_run_event_loop(context)) { js_std_dump_error(context); goto failure; }
    if (memory_report) {
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(runtime, &usage);
        JS_DumpMemoryUsage(stderr, &usage, runtime);
    }
    sxn_free_node_compat(context);
    sxn_shutdown_napi(context);
    js_std_free_handlers(runtime); JS_FreeContext(context); JS_FreeRuntime(runtime); return 0;
failure:
    if (source) js_free(context, source);
    sxn_free_node_compat(context);
    sxn_shutdown_napi(context);
    js_std_free_handlers(runtime); JS_FreeContext(context); JS_FreeRuntime(runtime); return 1;
}

/* `sxn compile <file> [-o out.sxbc]`: ahead-of-time compile for
   distribution -- ship the bytecode, not the source. Needs only a bare
   context: compiling is a pure parse, with no network or node: layer to
   install and nothing yet to execute. */
static int sxn_compile_command(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    bool strip = false;
    for (int i = 2; i < argc; i++) {
        if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "--strip")) {
            strip = true;
        } else if (!in) {
            in = argv[i];
        } else {
            fprintf(stderr, "sxn: compile takes one input file (got '%s' after '%s')\n", argv[i], in);
            return 2;
        }
    }
    if (!in) {
        fputs("sxn: usage: sxn compile <file> [-o out.sxbc]\n", stderr);
        return 2;
    }
    char default_out[PATH_MAX];
    if (!out) { sxn_sxbc_default_path(default_out, sizeof(default_out), in); out = default_out; }

    JSRuntime *runtime = JS_NewRuntime();
    if (!runtime) { fputs("sxn: unable to create QuickJS runtime\n", stderr); return 2; }
    js_std_init_handlers(runtime);
    JSContext *context = JS_NewContext(runtime);
    if (!context) { JS_FreeRuntime(runtime); return 2; }
    /* A module's imports are resolved while it is compiled, so compiling
       needs the same loader running a file does. Without it, `sxn compile`
       failed on any file with a relative import -- which is every file in a
       program of more than one. */
    JS_SetModuleLoaderFunc2(runtime, sxn_module_normalize, sxn_module_loader,
                            js_module_check_attributes, NULL);
    int rc = sxn_compile_file(context, in, out, strip);
    js_std_free_handlers(runtime);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    if (rc == 0) fprintf(stderr, "sxn: compiled '%s' -> '%s'\n", in, out);
    return rc == 0 ? 0 : 1;
}

static void usage(void) {
    puts("SXN 0.0.1\n"
         "Usage:\n"
         "  sxn <file.sx|file.ts|file.js|file.mjs|file.cjs> [args...]\n"
         "  sxn run [script] -- [args...]\n"
         "  sxn <script> [-- args...]\n"
         "  sxn install [--trust package]\n"
         "  sxn add [--dev] package[@range]\n"
         "  sxn remove package\n"
         "  sxn init\n"
         "  sxn [--memory-report] [--leak-check] [--compile-cache] <file.sx|file.ts|file.js|file.mjs|file.cjs> [args...]\n"
         "  sxn compile <file> [-o out.sxbc] [--strip]   -- compile to bytecode for distribution\n"
         "  sxn <file.sxbc> [args...]          -- run precompiled bytecode directly\n"
         "  sxn lsp --stdio\n"
         "  sxn --help | --version");
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* Windows' CRT opens stdio in text mode by default, which rewrites every
       '\n' a write() makes into "\r\n" - every other platform this runs on
       writes bare LF, and scripts/tooling reading sxn's output shouldn't
       have to special-case Windows to match it. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) { usage(); return 0; }
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v")) { puts("sxn 0.0.1"); return 0; }
    if (!strcmp(argv[1], "lsp")) return sxn_lsp_main();
    if (!strcmp(argv[1], "compile")) return sxn_compile_command(argc, argv);
    if (!strcmp(argv[1], "run") || !strcmp(argv[1], "install") || !strcmp(argv[1], "add") ||
        !strcmp(argv[1], "remove") || !strcmp(argv[1], "init")) return sxn_package_command(argc, argv);
    bool memory_report = false, leak_check = false, compile_cache = false;
    int file_index = 1;
    while (file_index < argc && (!strcmp(argv[file_index], "--memory-report") ||
                                 !strcmp(argv[file_index], "--leak-check") ||
                                 !strcmp(argv[file_index], "--compile-cache"))) {
        if (!strcmp(argv[file_index], "--memory-report")) memory_report = true;
        else if (!strcmp(argv[file_index], "--leak-check")) leak_check = true;
        else compile_cache = true;
        ++file_index;
    }
    if (file_index >= argc) { usage(); return 2; }
    /* An argument that names a real file is a file to run, whatever it is
       called. Node and Bun both run `./node_modules/.bin/whatever`, and every
       CLI shipped by an npm package is extensionless, so requiring a known
       suffix here sent them all to `npm run` and reported a missing script.
       A name that is not a file is still a package script. */
    if (!sxn_is_file(argv[file_index]) &&
        !suffix(argv[file_index], ".sx") && !suffix(argv[file_index], ".js") && !suffix(argv[file_index], ".mjs") && !suffix(argv[file_index], ".cjs") && !suffix(argv[file_index], ".ts") && !suffix(argv[file_index], ".mts") && !suffix(argv[file_index], ".sxbc")) {
        char **run_argv = calloc((size_t)argc + 1, sizeof(*run_argv));
        if (!run_argv) return 2;
        run_argv[0] = argv[0]; run_argv[1] = "run";
        for (int i = 1; i < argc; ++i) run_argv[i + 1] = argv[i];
        int status = sxn_package_command(argc + 1, run_argv);
        free(run_argv); return status;
    }
    const char *entry = argv[file_index];
    char cache_path[PATH_MAX];
    if (compile_cache && !suffix(entry, ".sxbc")) {
        /* "Convert to bytecode, then run that" -- but only actually compile
           when the cache is missing or older than the source; a script run
           twice in a row should parse once, not on every launch. */
        sxn_sxbc_default_path(cache_path, sizeof(cache_path), entry);
        if (!sxn_sxbc_cache_is_fresh(cache_path, entry)) {
            JSRuntime *rt = JS_NewRuntime();
            if (!rt) { fputs("sxn: unable to create QuickJS runtime\n", stderr); return 2; }
            js_std_init_handlers(rt);
            JSContext *ctx = JS_NewContext(rt);
            if (!ctx) { JS_FreeRuntime(rt); return 2; }
            /* A cache built for this runtime's own next launch keeps
               source info: it's read back on the same machine, so there's
               nothing to strip for and stack traces should stay useful. */
            int rc = sxn_compile_file(ctx, entry, cache_path, false);
            js_std_free_handlers(rt);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            if (rc != 0) return 1;
        }
        entry = cache_path;
    }
    return execute_file(argc, argv, entry, file_index, memory_report, leak_check);
}
