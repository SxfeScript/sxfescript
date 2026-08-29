#include "sxfe.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buffer { char *data; size_t length; size_t capacity; } Buffer;

static int reserve(Buffer *buffer, size_t extra) {
    if (buffer->length + extra + 1 <= buffer->capacity) return 0;
    size_t capacity = buffer->capacity ? buffer->capacity : 1024;
    while (capacity < buffer->length + extra + 1) capacity *= 2;
    char *data = realloc(buffer->data, capacity);
    if (!data) return -1;
    buffer->data = data;
    buffer->capacity = capacity;
    return 0;
}

static int append(Buffer *buffer, const char *data, size_t length) {
    if (reserve(buffer, length)) return -1;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = 0;
    return 0;
}

static bool ident_start(char c) { return isalpha((unsigned char)c) || c == '_' || c == '$'; }
static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '$'; }
static bool word_at(const char *s, size_t n, size_t i, const char *word) {
    size_t w = strlen(word);
    return i + w <= n && !memcmp(s + i, word, w) &&
           (i == 0 || !ident_char(s[i - 1])) && (i + w == n || !ident_char(s[i + w]));
}

static void location(const char *source, size_t offset, size_t *line, size_t *column) {
    *line = 1; *column = 1;
    for (size_t i = 0; i < offset; ++i) {
        if (source[i] == '\n') { ++*line; *column = 1; } else ++*column;
    }
}

static int diagnostic(SxfeCompileResult *out, const char *source, SxfeDiagnosticCode code,
                      size_t offset, const char *message) {
    size_t count = out->diagnostic_count + 1;
    SxfeDiagnostic *items = realloc(out->diagnostics, count * sizeof(*items));
    if (!items) return -1;
    out->diagnostics = items;
    SxfeDiagnostic *item = &items[count - 1];
    memset(item, 0, sizeof(*item));
    item->code = code;
    item->offset = offset;
    location(source, offset, &item->line, &item->column);
    snprintf(item->message, sizeof(item->message), "%s", message);
    out->diagnostic_count = count;
    return 0;
}

static size_t skip_string(const char *source, size_t length, size_t i) {
    char quote = source[i++];
    while (i < length) {
        if (source[i] == '\\') { i += i + 1 < length ? 2 : 1; continue; }
        if (source[i++] == quote) break;
    }
    return i;
}

static size_t skip_comment(const char *source, size_t length, size_t i) {
    if (i + 1 >= length || source[i] != '/') return i;
    if (source[i + 1] == '/') {
        i += 2; while (i < length && source[i] != '\n') ++i; return i;
    }
    if (source[i + 1] == '*') {
        i += 2; while (i + 1 < length && !(source[i] == '*' && source[i + 1] == '/')) ++i;
        return i + (i + 1 < length ? 2 : 0);
    }
    return i;
}

/* SX deliberately accepts only erasable TypeScript. Constructs that require emit are rejected. */
static void validate_unsupported(const char *source, size_t length, SxfeCompileResult *out) {
    const char *words[] = { "enum", "namespace", "decorator", "abstract" };
    bool reported[sizeof(words) / sizeof(words[0])] = {0};
    /* Skip string/comment bodies so an occurrence of these words inside them
       (e.g. a "// TODO: support enum" comment) doesn't misfire. */
    for (size_t i = 0; i < length; ) {
        if (source[i] == '\'' || source[i] == '"' || source[i] == '`') { i = skip_string(source, length, i); continue; }
        size_t comment = skip_comment(source, length, i);
        if (comment != i) { i = comment; continue; }
        for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); ++w) {
            if (!reported[w] && word_at(source, length, i, words[w])) {
                char message[160];
                snprintf(message, sizeof(message), "'%s' requires TypeScript code generation and is not supported in .sx", words[w]);
                diagnostic(out, source, SX1001_UNSUPPORTED_SYNTAX, i, message);
                reported[w] = true;
            }
        }
        ++i;
    }
}

static size_t skip_declaration(const char *source, size_t length, size_t i) {
    int braces = 0;
    bool saw_brace = false;
    while (i < length) {
        if (source[i] == '\'' || source[i] == '"' || source[i] == '`') { i = skip_string(source, length, i); continue; }
        size_t comment = skip_comment(source, length, i);
        if (comment != i) { i = comment; continue; }
        if (source[i] == '{') { ++braces; saw_brace = true; }
        else if (source[i] == '}') { if (--braces <= 0 && saw_brace) return i + 1; }
        else if (source[i] == ';' && !saw_brace) return i + 1;
        ++i;
    }
    return i;
}

static bool likely_type_at(const char *source, size_t length, size_t i) {
    if (i >= length) return false;
    if (source[i] == '&') return true;
    if (isupper((unsigned char)source[i])) return true;
    const char *types[] = { "i32", "f32", "f64", "bool", "void", "string", "number", "boolean", "unknown", "any", "never" };
    for (size_t n = 0; n < sizeof(types) / sizeof(types[0]); ++n) if (word_at(source, length, i, types[n])) return true;
    return false;
}

static size_t skip_type(const char *source, size_t length, size_t i) {
    int angle = 0, square = 0, paren = 0, brace = 0;
    while (i < length) {
        char c = source[i];
        if (c == '<') ++angle; else if (c == '>' && angle) --angle;
        else if (c == '[') ++square; else if (c == ']' && square) --square;
        else if (c == '(') ++paren; else if (c == ')' && paren) --paren;
        else if (c == '{') ++brace; else if (c == '}' && brace) --brace;
        if (!angle && !square && !paren && !brace && (c == ',' || c == ')' || c == '=' || c == ';' || c == '{')) break;
        if (!angle && !square && !paren && !brace && c == '\n') break;
        ++i;
    }
    return i;
}

int sxfe_compile(const char *source, size_t length, SxfeCompileResult *out) {
    if (!source || !out) return -1;
    memset(out, 0, sizeof(*out));
    validate_unsupported(source, length, out);
    Buffer result = {0};
    int paren_depth = 0;
    bool declaration_context = false;
    for (size_t i = 0; i < length;) {
        if (source[i] == '\'' || source[i] == '"' || source[i] == '`') {
            size_t end = skip_string(source, length, i);
            if (append(&result, source + i, end - i)) goto oom;
            i = end; continue;
        }
        size_t comment = skip_comment(source, length, i);
        if (comment != i) { if (append(&result, source + i, comment - i)) goto oom; i = comment; continue; }
        if (word_at(source, length, i, "interface") || word_at(source, length, i, "type")) {
            size_t end = skip_declaration(source, length, i);
            for (size_t p = i; p < end; ++p) if (source[p] == '\n' && append(&result, "\n", 1)) goto oom;
            i = end; continue;
        }
        /* Primitive FFI declarations are lowered to a host call until the
           native SX parser grows first-class FFI bytecodes. */
        if (word_at(source, length, i, "extern") || word_at(source, length, i, "using")) {
            size_t end = i;
            while (end < length && source[end] != ';') ++end;
            if (end < length) ++end;
            size_t p = i + (word_at(source, length, i, "extern") ? 6 : 5);
            while (p < end && isspace((unsigned char)source[p])) ++p;
            size_t ns = p; while (p < end && ident_char(source[p])) ++p;
            size_t name_len = p - ns;
            const char *from = strstr(source + p, "from");
            const char *quote = from ? strchr(from, '"') : NULL;
            const char *close = quote ? strchr(quote + 1, '"') : NULL;
            const char *open = strchr(source + p, '(');
            const char *colon = open ? strchr(open, ':') : NULL;
            if (name_len && from && quote && close && open && colon) {
                char line[1024];
                int n = snprintf(line, sizeof(line),
                    "const %.*s = (...args) => Sxn.ffi(\"%.*s\", \"%.*s\", \"%.*s\", args);",
                    (int)name_len, source + ns, (int)(close - quote - 1), quote + 1,
                    (int)name_len, source + ns, (int)(colon - open - 1), open + 1);
                if (n < 0 || (size_t)n >= sizeof(line) || append(&result, line, (size_t)n)) goto oom;
                i = end;
                continue;
            }
            diagnostic(out, source, SX1002_INVALID_TYPE, i,
                       "invalid FFI declaration; expected extern name(types): return from \"library\"");
            i = end;
            continue;
        }
        /* `safe` is a contextual declaration qualifier.  It is intentionally
           erased by the compatibility frontend; the native SX parser will
           attach the type contract to the resulting binding. */
        if (word_at(source, length, i, "safe")) {
            size_t j = i + 4;
            while (j < length && isspace((unsigned char)source[j])) ++j;
            if (word_at(source, length, j, "let") || word_at(source, length, j, "const")) {
                i = j;
                if (append(&result, "", 0)) goto oom;
                continue;
            }
            if (word_at(source, length, j, "var")) {
                diagnostic(out, source, SX1002_INVALID_TYPE, i,
                           "safe may qualify only let or const declarations");
            }
        }
        if (word_at(source, length, i, "let") || word_at(source, length, i, "const") || word_at(source, length, i, "var")) declaration_context = true;
        if (word_at(source, length, i, "let")) {
            if (append(&result, "let", 3)) goto oom; i += 3;
            size_t ws = i; while (i < length && isspace((unsigned char)source[i])) ++i;
            if (word_at(source, length, i, "mut")) { i += 3; if (append(&result, " ", 1)) goto oom; }
            else if (append(&result, source + ws, i - ws)) goto oom;
            continue;
        }
        if (word_at(source, length, i, "unsafe")) { i += 6; continue; }
        if (source[i] == '&') {
            if (i + 1 < length && source[i + 1] == '&') {
                if (append(&result, "&&", 2)) goto oom;
                i += 2;
                continue;
            }
            ++i; while (i < length && isspace((unsigned char)source[i])) ++i;
            if (word_at(source, length, i, "mut")) { i += 3; while (i < length && isspace((unsigned char)source[i])) ++i; }
            continue;
        }
        if (source[i] == '(') { ++paren_depth; if (append(&result, source + i++, 1)) goto oom; continue; }
        if (source[i] == ')') { if (paren_depth) --paren_depth; if (append(&result, source + i++, 1)) goto oom; continue; }
        if (source[i] == ':') {
            size_t j = i + 1; while (j < length && isspace((unsigned char)source[j])) ++j;
            size_t previous = i;
            while (previous > 0 && isspace((unsigned char)source[previous - 1])) --previous;
            bool likely_type = likely_type_at(source, length, j) &&
                               (paren_depth > 0 || declaration_context ||
                                (previous > 0 && source[previous - 1] == ')'));
            if (likely_type) { i = skip_type(source, length, j); continue; }
        }
        if (source[i] == '=' || source[i] == ';') declaration_context = false;
        if (append(&result, source + i, 1)) goto oom;
        ++i;
    }
    out->javascript = result.data;
    out->length = result.length;
    return out->diagnostic_count ? 1 : 0;
oom:
    free(result.data); sxfe_compile_result_free(out); return -1;
}

void sxfe_compile_result_free(SxfeCompileResult *result) {
    if (!result) return;
    free(result->javascript); free(result->diagnostics); memset(result, 0, sizeof(*result));
}

const char *sxfe_diagnostic_name(SxfeDiagnosticCode code) {
    switch (code) {
        case SX0000_OK: return "SX0000";
        case SX1001_UNSUPPORTED_SYNTAX: return "SX1001";
        case SX1002_INVALID_TYPE: return "SX1002";
        case SX2001_VALUE_MOVED: return "SX2001";
        case SX2002_BORROW_CONFLICT: return "SX2002";
        case SX2003_IMMUTABLE_BORROW: return "SX2003";
        case SX2004_ESCAPING_BORROW: return "SX2004";
        case SX3001_LAYOUT_ERROR: return "SX3001";
        case SX4001_UNSAFE_BOUNDARY: return "SX4001";
    }
    return "SX9999";
}
