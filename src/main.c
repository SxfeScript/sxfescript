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

static void print_diagnostics(const char *filename, const SxfeCompileResult *result) {
    for (size_t i = 0; i < result->diagnostic_count; ++i) {
        const SxfeDiagnostic *d = &result->diagnostics[i];
        fprintf(stderr, "%s:%zu:%zu: %s %s\n", filename, d->line, d->column,
                sxfe_diagnostic_name(d->code), d->message);
    }
}

static uint8_t *sxn_load_file(JSContext *ctx, size_t *length, const char *filename) {
    uint8_t *source = js_load_file(ctx, length, filename);
    if (!source || !suffix(filename, ".sx")) return source;
    SxfeCompileResult compiled;
    int status = sxfe_compile((const char *)source, *length, &compiled);
    js_free(ctx, source);
    if (status != 0) {
        print_diagnostics(filename, &compiled);
        sxfe_compile_result_free(&compiled);
        JS_ThrowSyntaxError(ctx, "SxfeScript compilation failed for '%s'", filename);
        return NULL;
    }
    if (getenv("SXN_DUMP_TRANSFORM")) fprintf(stderr, "--- %s ---\n%.*s\n", filename, (int)compiled.length, compiled.javascript);
    uint8_t *result = js_malloc(ctx, compiled.length + 1);
    if (result) {
        memcpy(result, compiled.javascript, compiled.length + 1);
        *length = compiled.length;
    }
    sxfe_compile_result_free(&compiled);
    return result;
}

static JSModuleDef *sxn_module_loader(JSContext *ctx, const char *name, void *opaque,
                                      JSValueConst attributes) {
    if (!suffix(name, ".sx")) return js_module_loader(ctx, name, opaque, attributes);
    return js_module_load(ctx, name, opaque, attributes, sxn_load_file);
}

static int execute_file(int argc, char **argv) {
    const char *filename = argv[1];
    size_t length = 0;
    uint8_t *source = NULL;
    JSRuntime *runtime = JS_NewRuntime();
    if (!runtime) { fputs("sxn: unable to create QuickJS runtime\n", stderr); return 2; }
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
    js_std_free_handlers(runtime); JS_FreeContext(context); JS_FreeRuntime(runtime); return 0;
failure:
    if (source) js_free(context, source);
    js_std_free_handlers(runtime); JS_FreeContext(context); JS_FreeRuntime(runtime); return 1;
}

static void usage(void) {
    puts("SXN 0.1.0\n"
         "Usage:\n"
         "  sxn <file.sx|file.js|file.mjs|file.cjs> [args...]\n"
         "  sxn run [script] -- [args...]\n"
         "  sxn install [--trust package]\n"
         "  sxn add [--dev] package[@range]\n"
         "  sxn remove package\n"
         "  sxn init\n"
         "  sxn lsp --stdio\n"
         "  sxn --help | --version");
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) { usage(); return 0; }
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v")) { puts("sxn 0.1.0"); return 0; }
    if (!strcmp(argv[1], "lsp")) return sxn_lsp_main();
    if (!strcmp(argv[1], "run") || !strcmp(argv[1], "install") || !strcmp(argv[1], "add") ||
        !strcmp(argv[1], "remove") || !strcmp(argv[1], "init")) return sxn_package_command(argc, argv);
    if (!suffix(argv[1], ".sx") && !suffix(argv[1], ".js") && !suffix(argv[1], ".mjs") && !suffix(argv[1], ".cjs")) {
        fprintf(stderr, "sxn: unsupported entrypoint '%s'\n", argv[1]); return 2;
    }
    return execute_file(argc, argv);
}
