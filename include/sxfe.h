#ifndef SXFE_H
#define SXFE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SXFE_VERSION "0.0.2"

/* Length of the absolute-path root at p: 0 if relative, 1 for POSIX's
   leading '/', or 3 for a Windows drive letter (`C:/` or `C:\`). Used by
   the CLI/module-resolution code in main.c, which has to recognize an
   absolute path handed in from argv or an import specifier on every
   platform - unlike node:path's posix.* implementation (src/node.c),
   which stays leading-'/'-only on purpose: that is real Node behavior,
   not a gap. */
static inline size_t sxn_path_root_len(const char *p) {
    if (p[0] == '/') return 1;
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':' && (p[2] == '/' || p[2] == '\\'))
        return 3;
#endif
    return 0;
}
static inline bool sxn_path_is_absolute(const char *p) { return sxn_path_root_len(p) != 0; }

typedef enum SxfeDiagnosticCode {
    SX0000_OK = 0,
    SX1001_UNSUPPORTED_SYNTAX,
    SX1002_INVALID_TYPE,
    SX2001_VALUE_MOVED,
    SX2002_BORROW_CONFLICT,
    SX2003_IMMUTABLE_BORROW,
    SX2004_ESCAPING_BORROW,
    SX3001_LAYOUT_ERROR,
    SX4001_UNSAFE_BOUNDARY
} SxfeDiagnosticCode;

typedef struct SxfeDiagnostic {
    SxfeDiagnosticCode code;
    size_t offset;
    size_t line;
    size_t column;
    char message[192];
} SxfeDiagnostic;

typedef struct SxfeCompileResult {
    char *javascript;
    size_t length;
    SxfeDiagnostic *diagnostics;
    size_t diagnostic_count;
} SxfeCompileResult;

int sxfe_compile(const char *source, size_t length, SxfeCompileResult *out);
void sxfe_compile_result_free(SxfeCompileResult *result);
const char *sxfe_diagnostic_name(SxfeDiagnosticCode code);

typedef enum SxfePrimitive {
    SXFE_I32,
    SXFE_F32,
    SXFE_F64,
    SXFE_BOOL
} SxfePrimitive;

typedef struct SxfeLayoutField {
    const char *name;
    SxfePrimitive primitive;
    size_t offset;
} SxfeLayoutField;

typedef struct SxfeLayout {
    const char *name;
    SxfeLayoutField *fields;
    size_t field_count;
    size_t size;
    size_t alignment;
} SxfeLayout;

int sxfe_layout_finalize(SxfeLayout *layout);

typedef struct SxfeArena {
    uint8_t *data;
    size_t capacity;
    size_t stack_pointer;
} SxfeArena;

int sxfe_arena_init(SxfeArena *arena, size_t initial_capacity);
void sxfe_arena_destroy(SxfeArena *arena);
void *sxfe_arena_alloc(SxfeArena *arena, size_t size, size_t alignment, size_t *mark);
void sxfe_arena_release(SxfeArena *arena, size_t mark);
int sxfe_arena_move(void *destination, void *source, size_t size);

int sxn_package_command(int argc, char **argv);
int sxn_lsp_main(void);

#ifdef __cplusplus
}
#endif
#endif
