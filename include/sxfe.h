#ifndef SXFE_H
#define SXFE_H

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SXFE_VERSION "0.0.1"

/* The lexical rules the frontend (src/frontend.c) and the LSP's semantic
   highlighter (src/lsp.c) both scan by. They live here rather than in either
   file because the two must agree: a word the compiler treats as the `mut` or
   `safe` keyword is exactly the word the editor should color as one, and an
   occurrence inside a string or a comment is neither. */
static inline bool sxfe_ident_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '$'; }

/* True when `word` sits at s[i] as a whole word rather than inside a longer
   identifier, so "unsafe" does not match a search for "safe". */
static inline bool sxfe_word_at(const char *s, size_t n, size_t i, const char *word) {
    size_t w = strlen(word);
    return i + w <= n && !memcmp(s + i, word, w) &&
           (i == 0 || !sxfe_ident_char(s[i - 1])) && (i + w == n || !sxfe_ident_char(s[i + w]));
}

/* Index just past the string literal opening at s[i], which the caller has
   already checked is a quote. Handles ' " and ` alike, and backslash escapes. */
static inline size_t sxfe_skip_string(const char *s, size_t n, size_t i) {
    char quote = s[i++];
    while (i < n) {
        if (s[i] == '\\') { i += i + 1 < n ? 2 : 1; continue; }
        if (s[i++] == quote) break;
    }
    return i;
}

/* Index just past the comment at s[i], or i unchanged when none starts there. */
static inline size_t sxfe_skip_comment(const char *s, size_t n, size_t i) {
    if (i + 1 >= n || s[i] != '/') return i;
    if (s[i + 1] == '/') {
        i += 2; while (i < n && s[i] != '\n') ++i; return i;
    }
    if (s[i + 1] == '*') {
        i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i;
        return i + (i + 1 < n ? 2 : 0);
    }
    return i;
}

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
/* Releases the atoms sxn_install_node_compat cached; call once, before
   JS_FreeContext, or the runtime reports them as leaked. */
void sxn_free_node_compat(struct JSContext *context);

#ifdef __cplusplus
}
#endif
#endif
