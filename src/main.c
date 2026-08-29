#include "sxfe.h"
#include "quickjs.h"
#include "quickjs-libc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

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

static int execute_file(int argc, char **argv, const char *filename,
                        bool memory_report, bool leak_check) {
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
    if (leak_check) JS_SetDumpFlags(runtime, JS_ABORT_ON_LEAKS | JS_DUMP_MEM);
    if (getenv("SXN_DUMP_BYTECODE")) JS_SetDumpFlags(runtime, JS_GetDumpFlags(runtime) | JS_DUMP_BYTECODE_FINAL);
    js_std_init_handlers(runtime);
    JSContext *context = JS_NewContext(runtime);
    if (!context) { JS_FreeRuntime(runtime); return 2; }
    js_init_module_std(context, "qjs:std");
    js_init_module_os(context, "qjs:os");
    js_init_module_bjson(context, "qjs:bjson");
    js_std_add_helpers(context, argc - 1, argv + 1);
    if (sxn_install_network(context) != 0) {
        fputs("sxn: unable to initialize network runtime\n", stderr);
        goto failure;
    }
    if (sxn_install_node_compat(context, argv[0]) != 0) {
        fputs("sxn: unable to initialize node compatibility layer\n", stderr);
        goto failure;
    }
    JS_SetModuleLoaderFunc2(runtime, NULL, sxn_module_loader, js_module_check_attributes, NULL);
    JS_SetHostPromiseRejectionTracker(runtime, js_std_promise_rejection_tracker, NULL);

    source = sxn_load_file(context, &length, filename);
    if (!source) { js_std_dump_error(context); goto failure; }
    int flags = suffix(filename, ".cjs") ? JS_EVAL_TYPE_GLOBAL : JS_EVAL_TYPE_MODULE;
    JSValue value = JS_Eval(context, (const char *)source, length, filename,
                            flags | (flags == JS_EVAL_TYPE_MODULE ? JS_EVAL_FLAG_COMPILE_ONLY : 0));
    js_free(context, source);
    source = NULL;
    if (!JS_IsException(value) && flags == JS_EVAL_TYPE_MODULE) {
        if (js_module_set_import_meta(context, value, true, true) < 0) {
            JS_FreeValue(context, value); value = JS_EXCEPTION;
        } else value = JS_EvalFunction(context, value);
    }
    if (!JS_IsException(value)) value = js_std_await(context, value);
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
    js_std_free_handlers(runtime); JS_FreeContext(context); JS_FreeRuntime(runtime); return 0;
failure:
    if (source) js_free(context, source);
    sxn_free_node_compat(context);
    js_std_free_handlers(runtime); JS_FreeContext(context); JS_FreeRuntime(runtime); return 1;
}

static void usage(void) {
    puts("SXN 0.0.1\n"
         "Usage:\n"
         "  sxn <file.sx|file.js|file.mjs|file.cjs> [args...]\n"
         "  sxn run [script] -- [args...]\n"
         "  sxn <script> [-- args...]\n"
         "  sxn install [--trust package]\n"
         "  sxn add [--dev] package[@range]\n"
         "  sxn remove package\n"
         "  sxn init\n"
         "  sxn [--memory-report] [--leak-check] <file.sx|file.js|file.mjs|file.cjs> [args...]\n"
         "  sxn lsp --stdio\n"
         "  sxn --help | --version");
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) { usage(); return 0; }
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v")) { puts("sxn 0.0.1"); return 0; }
    if (!strcmp(argv[1], "lsp")) return sxn_lsp_main();
    if (!strcmp(argv[1], "run") || !strcmp(argv[1], "install") || !strcmp(argv[1], "add") ||
        !strcmp(argv[1], "remove") || !strcmp(argv[1], "init")) return sxn_package_command(argc, argv);
    bool memory_report = false, leak_check = false;
    int file_index = 1;
    while (file_index < argc && (!strcmp(argv[file_index], "--memory-report") || !strcmp(argv[file_index], "--leak-check"))) {
        if (!strcmp(argv[file_index], "--memory-report")) memory_report = true;
        else leak_check = true;
        ++file_index;
    }
    if (file_index >= argc) { usage(); return 2; }
    if (!suffix(argv[file_index], ".sx") && !suffix(argv[file_index], ".js") && !suffix(argv[file_index], ".mjs") && !suffix(argv[file_index], ".cjs")) {
        char **run_argv = calloc((size_t)argc + 1, sizeof(*run_argv));
        if (!run_argv) return 2;
        run_argv[0] = argv[0]; run_argv[1] = "run";
        for (int i = 1; i < argc; ++i) run_argv[i + 1] = argv[i];
        int status = sxn_package_command(argc + 1, run_argv);
        free(run_argv); return status;
    }
    return execute_file(argc, argv, argv[file_index], memory_report, leak_check);
}
