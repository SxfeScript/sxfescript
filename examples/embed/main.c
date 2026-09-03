/* The smallest thing that runs a script on the runtime half alone.
 *
 * This is what an embedder does: make a QuickJS runtime and context the way
 * it already does, call sxn_install_runtime to get the WinterTC surface on
 * it -- TextEncoder, URL, URLPattern, Headers/Request/Response, the Streams,
 * structuredClone, crypto, the timers, fetch -- and then drive the loop.
 *
 * It exists to be built with SXN_ENABLE_NODE=OFF and
 * SXN_LOOP_BACKEND=builtin, where there is no node: layer, no require, and
 * no libuv, and to run the WinterTC fixtures there. A test suite that only
 * ever ran through `sxn` could not tell whether the split was real.
 *
 * A host with its own loop would not call sxn_run_event_loop at all: it
 * would install its own SxnLoopOps and call sxn_runtime_tick once a frame.
 * That path has no test here because it needs a host; the interface is the
 * same one this file leaves at its default. */

#include <quickjs.h>
#include <quickjs-libc.h>

#include "sxfe.h"
#include "sxn_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) { fputs("usage: sxn-embed <file.mjs>\n", stderr); return 2; }

    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fputs("sxn-embed: no runtime\n", stderr); return 2; }
    /* QuickJS budgets 1MB of JS stack regardless of what the thread actually
       has, which caps recursion around 950 frames -- shallow enough to break
       ordinary recursive code. `sxn` sizes this from RLIMIT_STACK with a
       reserve (see sxn_js_stack_budget in src/main.c); an embedder should do
       something similar. A flat 4MB is enough to make the point here. */
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return 2; }

    /* console.log and the argv helpers. An embedder with its own console
       installs it here instead, before sxn_install_runtime, so the runtime's
       console.info/debug alias theirs rather than the other way round. */
    js_std_init_handlers(rt);
    js_std_add_helpers(ctx, argc - 1, argv + 1);

    if (sxn_install_runtime(ctx) != 0) {
        fputs("sxn-embed: cannot install the runtime surface\n", stderr);
        js_std_free_handlers(rt); JS_FreeContext(ctx); JS_FreeRuntime(rt);
        return 2;
    }

    size_t len = 0;
    char *src = read_file(argv[1], &len);
    if (!src) { fprintf(stderr, "sxn-embed: cannot open '%s'\n", argv[1]); goto fail; }

    int status = 0;
    JSValue v = JS_Eval(ctx, src, len, argv[1],
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(src);
    if (!JS_IsException(v)) {
        if (js_module_set_import_meta(ctx, v, true, true) < 0) {
            JS_FreeValue(ctx, v);
            v = JS_EXCEPTION;
        } else {
            v = JS_EvalFunction(ctx, v);
        }
    }
    /* Top-level await evaluates to a pending promise; settling it needs the
       loop, not just the job queue. */
    if (!JS_IsException(v)) v = sxn_await_with_loop(ctx, v);
    if (JS_IsException(v)) { js_std_dump_error(ctx); status = 1; }
    JS_FreeValue(ctx, v);

    if (!status) {
        while (sxn_runtime_tick(ctx, 1, 0)) {}
        if (JS_HasException(ctx)) { js_std_dump_error(ctx); status = 1; }
    }

    js_std_free_handlers(rt); JS_FreeContext(ctx); JS_FreeRuntime(rt);
    return status;

fail:
    js_std_free_handlers(rt); JS_FreeContext(ctx); JS_FreeRuntime(rt);
    return 1;
}
