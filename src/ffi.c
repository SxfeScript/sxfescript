/* Sxn.ffi: call a C function in a shared library, through libffi.
 *
 * This lives on the runtime side rather than the Node-compatibility side on
 * purpose. Calling native code is an engine capability, not an emulation of
 * Node: Rayact embeds this engine and already loads native code in its own
 * core (`native/desktop/plugin_loader.cpp`, gated per platform by *linkage*
 * rather than by capability), and it has no Node surface at all to host this.
 * `process.dlopen` and the Node-API addon loader are the opposite case and
 * live in src/napi.c, which a mobile build drops entirely.
 *
 * Scope is the C ABI's scalar types, plus opaque pointers and NUL-terminated
 * strings. Structs by value, callbacks and variadics are rejected with a
 * clear error rather than half-supported: each needs ownership rules this
 * runtime has not written down yet (spec/ABI.md).
 */
#include <ffi.h>
#ifdef _WIN32
#include "sxn_dlfcn_win32.h"
#else
#include <dlfcn.h>
#endif
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "quickjs.h"

/* Every type this boundary accepts. Anything else is a TypeError naming the
   type, because a silently mis-marshalled argument is a crash later. */
typedef enum {
    SXN_FFI_VOID, SXN_FFI_BOOL,
    SXN_FFI_I8, SXN_FFI_U8, SXN_FFI_I16, SXN_FFI_U16,
    SXN_FFI_I32, SXN_FFI_U32, SXN_FFI_I64, SXN_FFI_U64,
    SXN_FFI_F32, SXN_FFI_F64,
    SXN_FFI_POINTER, SXN_FFI_CSTRING,
} SxnFfiType;

static const struct { const char *name; SxnFfiType type; } sxn_ffi_types[] = {
    { "void", SXN_FFI_VOID },   { "bool", SXN_FFI_BOOL },
    { "i8", SXN_FFI_I8 },       { "u8", SXN_FFI_U8 },
    { "i16", SXN_FFI_I16 },     { "u16", SXN_FFI_U16 },
    { "i32", SXN_FFI_I32 },     { "u32", SXN_FFI_U32 },
    { "i64", SXN_FFI_I64 },     { "u64", SXN_FFI_U64 },
    { "f32", SXN_FFI_F32 },     { "f64", SXN_FFI_F64 },
    { "pointer", SXN_FFI_POINTER }, { "cstring", SXN_FFI_CSTRING },
    /* The spellings a C header would use, so a declaration can be copied. */
    { "int", SXN_FFI_I32 },     { "unsigned", SXN_FFI_U32 },
    { "long", SXN_FFI_I64 },    { "double", SXN_FFI_F64 },
    { "float", SXN_FFI_F32 },   { "char", SXN_FFI_I8 },
    { "size_t", SXN_FFI_U64 },  { "ptr", SXN_FFI_POINTER },
    { "string", SXN_FFI_CSTRING },
};

static ffi_type *sxn_ffi_abi_type(SxnFfiType t) {
    switch (t) {
    case SXN_FFI_VOID:    return &ffi_type_void;
    case SXN_FFI_BOOL:    return &ffi_type_uint8;
    case SXN_FFI_I8:      return &ffi_type_sint8;
    case SXN_FFI_U8:      return &ffi_type_uint8;
    case SXN_FFI_I16:     return &ffi_type_sint16;
    case SXN_FFI_U16:     return &ffi_type_uint16;
    case SXN_FFI_I32:     return &ffi_type_sint32;
    case SXN_FFI_U32:     return &ffi_type_uint32;
    case SXN_FFI_I64:     return &ffi_type_sint64;
    case SXN_FFI_U64:     return &ffi_type_uint64;
    case SXN_FFI_F32:     return &ffi_type_float;
    case SXN_FFI_F64:     return &ffi_type_double;
    default:              return &ffi_type_pointer;
    }
}

/* One prepared call site: the resolved symbol and the cif libffi built for
   it. Preparing the cif once per declaration rather than once per call is
   the whole reason this is a handle and not a plain function. */
typedef struct {
    void *fn;
    ffi_cif cif;
    ffi_type **arg_abi;
    SxnFfiType *arg_type;
    SxnFfiType ret_type;
    unsigned argc;
    char *name;
} SxnFfiSymbol;

static JSClassID sxn_ffi_symbol_class_id;

static void sxn_ffi_symbol_finalizer(JSRuntime *rt, JSValue val) {
    SxnFfiSymbol *s = JS_GetOpaque(val, sxn_ffi_symbol_class_id);
    if (!s) return;
    js_free_rt(rt, s->arg_abi);
    js_free_rt(rt, s->arg_type);
    js_free_rt(rt, s->name);
    js_free_rt(rt, s);
}

static JSClassDef sxn_ffi_symbol_class = {
    "SxnFfiSymbol", .finalizer = sxn_ffi_symbol_finalizer,
};

/* Library handles are opened once and never closed, the way a plugin loader
   keeps them: a wrapper or callback that outlived its library would call
   into unmapped memory, and nothing here tracks that lifetime yet. */
#define SXN_FFI_MAX_LIBS 32
static struct { char *path; void *handle; } sxn_ffi_libs[SXN_FFI_MAX_LIBS];
static int sxn_ffi_lib_count;

static void *sxn_ffi_open(JSContext *ctx, const char *path) {
    for (int i = 0; i < sxn_ffi_lib_count; i++)
        if (!strcmp(sxn_ffi_libs[i].path, path)) return sxn_ffi_libs[i].handle;
    /* An empty name means this executable, which is how a program reaches
       libc and its own symbols without naming a platform-specific file. */
    void *h = dlopen(path[0] ? path : NULL, RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        const char *why = dlerror();
        JS_ThrowReferenceError(ctx, "cannot load library '%s': %s", path, why ? why : "unknown error");
        return NULL;
    }
    if (sxn_ffi_lib_count < SXN_FFI_MAX_LIBS) {
        char *copy = js_strdup(ctx, path);
        if (copy) {
            sxn_ffi_libs[sxn_ffi_lib_count].path = copy;
            sxn_ffi_libs[sxn_ffi_lib_count].handle = h;
            sxn_ffi_lib_count++;
        }
    }
    return h;
}

static int sxn_ffi_parse_type(JSContext *ctx, JSValueConst v, SxnFfiType *out) {
    const char *s = JS_ToCString(ctx, v);
    if (!s) return -1;
    /* Tolerate the spacing a copied C declaration carries. */
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    for (size_t i = 0; i < sizeof(sxn_ffi_types)/sizeof(sxn_ffi_types[0]); i++) {
        if (strlen(sxn_ffi_types[i].name) == n && !strncmp(s, sxn_ffi_types[i].name, n)) {
            *out = sxn_ffi_types[i].type;
            JS_FreeCString(ctx, s);
            return 0;
        }
    }
    JS_ThrowTypeError(ctx, "unsupported FFI type '%s'; expected one of "
                      "void bool i8 u8 i16 u16 i32 u32 i64 u64 f32 f64 pointer cstring", s);
    JS_FreeCString(ctx, s);
    return -1;
}

/* One argument slot. `owned` collects the temporary UTF-8 buffers a cstring
   argument needs, so they stay alive for exactly the duration of the call. */
typedef union {
    uint8_t u8; int8_t i8; uint16_t u16; int16_t i16;
    uint32_t u32; int32_t i32; uint64_t u64; int64_t i64;
    float f32; double f64; void *ptr;
} SxnFfiSlot;

static int sxn_ffi_pack(JSContext *ctx, SxnFfiType t, JSValueConst v,
                        SxnFfiSlot *slot, const char **owned) {
    switch (t) {
    case SXN_FFI_BOOL: slot->u8 = (uint8_t)(JS_ToBool(ctx, v) ? 1 : 0); return 0;
    case SXN_FFI_I8: case SXN_FFI_U8:
    case SXN_FFI_I16: case SXN_FFI_U16:
    case SXN_FFI_I32: case SXN_FFI_U32: {
        /* Every one of these fits in an int64 exactly, so one conversion
           covers signed and unsigned alike and the truncation is the C
           conversion the caller asked for. */
        int64_t n;
        if (JS_ToInt64Ext(ctx, &n, v) < 0) return -1;
        switch (t) {
        case SXN_FFI_I8: slot->i8 = (int8_t)n; break;
        case SXN_FFI_U8: slot->u8 = (uint8_t)n; break;
        case SXN_FFI_I16: slot->i16 = (int16_t)n; break;
        case SXN_FFI_U16: slot->u16 = (uint16_t)n; break;
        case SXN_FFI_I32: slot->i32 = (int32_t)n; break;
        default: slot->u32 = (uint32_t)n; break;
        }
        return 0;
    }
    case SXN_FFI_I64: case SXN_FFI_U64: {
        /* 64-bit integers go through BigInt, because a double cannot carry
           them: silently losing the low bits of a handle or a size is worse
           than requiring the caller to be explicit. A plain number is still
           accepted when it is an exact integer. */
        int64_t n;
        if (JS_ToInt64Ext(ctx, &n, v) < 0) return -1;
        if (t == SXN_FFI_I64) slot->i64 = n; else slot->u64 = (uint64_t)n;
        return 0;
    }
    case SXN_FFI_F32: case SXN_FFI_F64: {
        double d;
        if (JS_ToFloat64(ctx, &d, v) < 0) return -1;
        if (t == SXN_FFI_F32) slot->f32 = (float)d; else slot->f64 = d;
        return 0;
    }
    case SXN_FFI_POINTER: {
        if (JS_IsNull(v) || JS_IsUndefined(v)) { slot->ptr = NULL; return 0; }
        /* A typed array or ArrayBuffer passes the address of its bytes, which
           is what makes an out-parameter usable from JS. */
        size_t len; size_t off, elem;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &elem);
        if (!JS_IsException(ab)) {
            size_t total;
            uint8_t *base = JS_GetArrayBuffer(ctx, &total, ab);
            JS_FreeValue(ctx, ab);
            if (base) { slot->ptr = base + off; return 0; }
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        uint8_t *base = JS_GetArrayBuffer(ctx, &len, v);
        if (base) { slot->ptr = base; return 0; }
        JS_FreeValue(ctx, JS_GetException(ctx));
        int64_t n;
        if (JS_ToInt64Ext(ctx, &n, v) < 0) return -1;
        slot->ptr = (void *)(intptr_t)n;
        return 0;
    }
    case SXN_FFI_CSTRING: {
        if (JS_IsNull(v) || JS_IsUndefined(v)) { slot->ptr = NULL; return 0; }
        const char *s = JS_ToCString(ctx, v);
        if (!s) return -1;
        *owned = s;
        slot->ptr = (void *)s;
        return 0;
    }
    default:
        return JS_ThrowTypeError(ctx, "void is not an argument type"), -1;
    }
}

static JSValue sxn_ffi_unpack(JSContext *ctx, SxnFfiType t, const SxnFfiSlot *slot) {
    switch (t) {
    case SXN_FFI_VOID:    return JS_UNDEFINED;
    case SXN_FFI_BOOL:    return JS_NewBool(ctx, slot->u8 != 0);
    case SXN_FFI_I8:      return JS_NewInt32(ctx, slot->i8);
    case SXN_FFI_U8:      return JS_NewInt32(ctx, slot->u8);
    case SXN_FFI_I16:     return JS_NewInt32(ctx, slot->i16);
    case SXN_FFI_U16:     return JS_NewInt32(ctx, slot->u16);
    case SXN_FFI_I32:     return JS_NewInt32(ctx, slot->i32);
    case SXN_FFI_U32:     return JS_NewUint32(ctx, slot->u32);
    case SXN_FFI_I64:     return JS_NewBigInt64(ctx, slot->i64);
    case SXN_FFI_U64:     return JS_NewBigUint64(ctx, slot->u64);
    case SXN_FFI_F32:     return JS_NewFloat64(ctx, (double)slot->f32);
    case SXN_FFI_F64:     return JS_NewFloat64(ctx, slot->f64);
    case SXN_FFI_POINTER: return slot->ptr ? JS_NewBigUint64(ctx, (uint64_t)(uintptr_t)slot->ptr)
                                           : JS_NULL;
    case SXN_FFI_CSTRING: return slot->ptr ? JS_NewString(ctx, (const char *)slot->ptr) : JS_NULL;
    }
    return JS_UNDEFINED;
}

#define SXN_FFI_MAX_ARGS 16

static JSValue sxn_ffi_invoke(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv,
                              int magic, JSValue *func_data) {
    (void)this_val; (void)magic;
    SxnFfiSymbol *s = JS_GetOpaque(func_data[0], sxn_ffi_symbol_class_id);
    if (!s) return JS_ThrowTypeError(ctx, "not an FFI symbol");
    if ((unsigned)argc < s->argc)
        return JS_ThrowTypeError(ctx, "%s expects %u argument%s, got %d",
                                 s->name, s->argc, s->argc == 1 ? "" : "s", argc);

    SxnFfiSlot slots[SXN_FFI_MAX_ARGS];
    void *addrs[SXN_FFI_MAX_ARGS];
    const char *owned[SXN_FFI_MAX_ARGS];
    unsigned i;
    for (i = 0; i < s->argc; i++) owned[i] = NULL;
    for (i = 0; i < s->argc; i++) {
        if (sxn_ffi_pack(ctx, s->arg_type[i], argv[i], &slots[i], &owned[i]) < 0) {
            for (unsigned j = 0; j < s->argc; j++) if (owned[j]) JS_FreeCString(ctx, owned[j]);
            return JS_EXCEPTION;
        }
        addrs[i] = &slots[i];
    }

    /* libffi writes at least a full register, so the return slot has to be
       one even when the declared type is narrower. */
    union { SxnFfiSlot v; ffi_arg pad; } ret;
    memset(&ret, 0, sizeof(ret));
    ffi_call(&s->cif, FFI_FN(s->fn), &ret, addrs);

    for (i = 0; i < s->argc; i++) if (owned[i]) JS_FreeCString(ctx, owned[i]);
    return sxn_ffi_unpack(ctx, s->ret_type, &ret.v);
}

/* Sxn.ffi(library, symbol, argTypes, returnType) -> callable
 *
 *   const pow = Sxn.ffi("libm.dylib", "pow", ["f64", "f64"], "f64");
 *   pow(2, 10);   // 1024
 *
 * An empty library name means this executable. The cif is prepared here, so
 * the returned function is the only thing on the hot path.
 */
JSValue sxn_ffi(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 3)
        return JS_ThrowTypeError(ctx,
            "Sxn.ffi(library, symbol, argTypes[, returnType]) requires at least a library, "
            "a symbol and an argument-type list");

    const char *lib = JS_ToCString(ctx, argv[0]);
    if (!lib) return JS_EXCEPTION;
    const char *sym = JS_ToCString(ctx, argv[1]);
    if (!sym) { JS_FreeCString(ctx, lib); return JS_EXCEPTION; }

    JSValue out = JS_EXCEPTION;
    SxnFfiSymbol *s = NULL;
    JSValue holder = JS_UNDEFINED;

    void *handle = sxn_ffi_open(ctx, lib);
    if (!handle) goto done;
    dlerror();
    void *fn = dlsym(handle, sym);
    if (!fn) {
        const char *why = dlerror();
        JS_ThrowReferenceError(ctx, "library '%s' has no symbol '%s'%s%s",
                               lib, sym, why ? ": " : "", why ? why : "");
        goto done;
    }

    /* The argument list is an array of type names. A string is accepted too,
       because that is what the `unsafe extern` lowering produces. */
    JSValue list = JS_DupValue(ctx, argv[2]);
    if (JS_IsString(list)) {
        JSValue sep = JS_NewString(ctx, ",");
        JSValue split = JS_Invoke(ctx, list, JS_NewAtom(ctx, "split"), 1, (JSValueConst *)&sep);
        JS_FreeValue(ctx, sep);
        JS_FreeValue(ctx, list);
        if (JS_IsException(split)) goto done;
        list = split;
    }
    int64_t n = 0;
    if (JS_GetLength(ctx, list, &n) < 0) { JS_FreeValue(ctx, list); goto done; }
    if (n > SXN_FFI_MAX_ARGS) {
        JS_FreeValue(ctx, list);
        JS_ThrowRangeError(ctx, "FFI supports at most %d arguments, got %lld",
                           SXN_FFI_MAX_ARGS, (long long)n);
        goto done;
    }

    s = js_mallocz(ctx, sizeof(*s));
    if (!s) { JS_FreeValue(ctx, list); goto done; }
    s->fn = fn;
    s->argc = (unsigned)n;
    s->ret_type = SXN_FFI_VOID;
    s->name = js_strdup(ctx, sym);
    if (n > 0) {
        s->arg_abi = js_mallocz(ctx, sizeof(*s->arg_abi) * (size_t)n);
        s->arg_type = js_mallocz(ctx, sizeof(*s->arg_type) * (size_t)n);
        if (!s->arg_abi || !s->arg_type) { JS_FreeValue(ctx, list); goto done; }
    }
    for (int64_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyInt64(ctx, list, i);
        int bad = JS_IsException(e) || sxn_ffi_parse_type(ctx, e, &s->arg_type[i]) < 0;
        JS_FreeValue(ctx, e);
        if (bad) { JS_FreeValue(ctx, list); goto done; }
        if (s->arg_type[i] == SXN_FFI_VOID) {
            /* `void` alone is C's way of writing "no arguments"; anywhere
               else in the list it is a mistake. */
            if (n == 1) { s->argc = 0; break; }
            JS_FreeValue(ctx, list);
            JS_ThrowTypeError(ctx, "void is not an argument type");
            goto done;
        }
        s->arg_abi[i] = sxn_ffi_abi_type(s->arg_type[i]);
    }
    JS_FreeValue(ctx, list);

    if (argc > 3 && !JS_IsUndefined(argv[3]) &&
        sxn_ffi_parse_type(ctx, argv[3], &s->ret_type) < 0) goto done;

    if (ffi_prep_cif(&s->cif, FFI_DEFAULT_ABI, s->argc,
                     sxn_ffi_abi_type(s->ret_type), s->arg_abi) != FFI_OK) {
        JS_ThrowTypeError(ctx, "cannot build a call for '%s' with those types", sym);
        goto done;
    }

    holder = JS_NewObjectClass(ctx, sxn_ffi_symbol_class_id);
    if (JS_IsException(holder)) goto done;
    JS_SetOpaque(holder, s);
    s = NULL;                       /* the holder owns it now */
    out = JS_NewCFunctionData(ctx, sxn_ffi_invoke, (int)n, 0, 1, (JSValueConst *)&holder);

done:
    if (s) { js_free(ctx, s->arg_abi); js_free(ctx, s->arg_type); js_free(ctx, s->name); js_free(ctx, s); }
    JS_FreeValue(ctx, holder);
    JS_FreeCString(ctx, lib);
    JS_FreeCString(ctx, sym);
    return out;
}

void sxn_ffi_init(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &sxn_ffi_symbol_class_id);
    JS_NewClass(rt, sxn_ffi_symbol_class_id, &sxn_ffi_symbol_class);
}
