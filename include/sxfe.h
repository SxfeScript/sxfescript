#ifndef SXFE_H
#define SXFE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SXFE_VERSION "0.0.1"

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

struct JSContext;
int sxn_install_network(struct JSContext *context);
/* Drains pending libuv I/O (server sockets, async file reads) registered by
   sxn_install_network, interleaved with the QuickJS job queue. No-op/returns
   immediately if nothing registered any handles (e.g. a script that never
   calls Sxn.serve or the async file API). */
int sxn_run_event_loop(struct JSContext *context);

/* Installs the `node:buffer`/`node:path`/`node:events`/`node:process`
   compatibility modules (src/node.c + src/node_compat.js), following the
   same native-primitives-plus-JS-bootstrap split as sxn_install_network.
   exec_path becomes process.argv[0]. */
int sxn_install_node_compat(struct JSContext *context, const char *exec_path);

#ifdef __cplusplus
}
#endif
#endif
