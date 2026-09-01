#include "sxfe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void respond(const char *json) {
    printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
    fflush(stdout);
}

static long json_id(const char *message) {
    const char *id = strstr(message, "\"id\"");
    if (!id || !(id = strchr(id, ':'))) return 0;
    return strtol(id + 1, NULL, 10);
}

/* --- JSON string values -------------------------------------------------
   Enough of a reader to pull "uri" and "text" out of a notification. The rest
   of this file matches on method names with strstr for the same reason: the
   server answers a handful of shapes, and a parser would be more machinery
   than the shapes are worth. */

static void utf8_push(char *out, size_t *length, unsigned long cp) {
    if (cp < 0x80) { out[(*length)++] = (char)cp; }
    else if (cp < 0x800) {
        out[(*length)++] = (char)(0xC0 | (cp >> 6));
        out[(*length)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*length)++] = (char)(0xE0 | (cp >> 12));
        out[(*length)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*length)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*length)++] = (char)(0xF0 | (cp >> 18));
        out[(*length)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*length)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*length)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static unsigned long hex4(const char *s) {
    unsigned long value = 0;
    for (int n = 0; n < 4; ++n) {
        char c = s[n];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') value |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= (unsigned long)(c - 'A' + 10);
        else return 0xFFFFFFFF;
    }
    return value;
}

/* The unescaped value of the first "key":"..." in message, or NULL. Caller frees.
   Both shapes this server reads put the key it wants first: didOpen sends uri
   before text, and a full-sync change sends only text. */
static char *json_string(const char *message, const char *key) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *at = strstr(message, pattern);
    if (!at) {
        snprintf(pattern, sizeof(pattern), "\"%s\" :", key);
        at = strstr(message, pattern);
        if (!at) return NULL;
    }
    at = strchr(at + strlen(pattern) - 1, '"');
    if (!at) return NULL;
    ++at;
    /* An escape never expands: \uXXXX is 6 bytes in and at most 4 bytes out. */
    char *out = malloc(strlen(at) + 1);
    if (!out) return NULL;
    size_t length = 0;
    for (; *at && *at != '"'; ++at) {
        if (*at != '\\') { out[length++] = *at; continue; }
        switch (*++at) {
            case 'n': out[length++] = '\n'; break;
            case 't': out[length++] = '\t'; break;
            case 'r': out[length++] = '\r'; break;
            case 'b': out[length++] = '\b'; break;
            case 'f': out[length++] = '\f'; break;
            case 'u': {
                unsigned long cp = hex4(at + 1);
                if (cp == 0xFFFFFFFF) { free(out); return NULL; }
                at += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && at[1] == '\\' && at[2] == 'u') {
                    unsigned long low = hex4(at + 3);
                    if (low >= 0xDC00 && low < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        at += 6;
                    }
                }
                utf8_push(out, &length, cp);
                break;
            }
            case 0: free(out); return NULL;
            default: out[length++] = *at; break;  /* \" \\ \/ */
        }
    }
    out[length] = 0;
    return out;
}

/* --- open documents -----------------------------------------------------
   textDocumentSync is Full, so every change carries the whole file and one
   stored copy per open URI is all the highlighter needs. */

typedef struct Document { char *uri; char *text; } Document;
static Document documents[64];
static size_t document_count;

static void document_store(char *uri, char *text) {
    if (!uri || !text) { free(uri); free(text); return; }
    for (size_t i = 0; i < document_count; ++i) {
        if (!strcmp(documents[i].uri, uri)) {
            free(documents[i].text);
            documents[i].text = text;
            free(uri);
            return;
        }
    }
    if (document_count == sizeof(documents) / sizeof(documents[0])) { free(uri); free(text); return; }
    documents[document_count].uri = uri;
    documents[document_count].text = text;
    ++document_count;
}

static void document_close(char *uri) {
    if (!uri) return;
    for (size_t i = 0; i < document_count; ++i) {
        if (!strcmp(documents[i].uri, uri)) {
            free(documents[i].uri);
            free(documents[i].text);
            documents[i] = documents[--document_count];
            break;
        }
    }
    free(uri);
}

static const char *document_text(const char *uri) {
    for (size_t i = 0; i < document_count; ++i)
        if (!strcmp(documents[i].uri, uri)) return documents[i].text;
    return NULL;
}

/* --- semantic tokens ----------------------------------------------------
   The editor's TextMate grammar (tooling/vscode/syntaxes/sxfe.tmLanguage.json)
   already colors the lexical keywords, and github.com borrows Rust's grammar
   for the same job (see .gitattributes). Neither can decide `safe`, which is a
   keyword only where it qualifies a `let` or `const` and an ordinary
   identifier everywhere else, and a grammar that guesses gets it wrong in
   whichever direction it guesses. That decision needs the compiler's own rule,
   so the server makes it here and sends the answer back as a token.

   The set is deliberately small: the ownership vocabulary, and nothing a
   grammar already handles on its own. */

enum { TOKEN_KEYWORD = 0, TOKEN_OPERATOR = 1 };
static const char *const OWNERSHIP_KEYWORDS[] = { "mut", "unsafe", "extern" };

typedef struct Tokens {
    unsigned *data; size_t count, capacity;
    unsigned line, column;  /* absolute position of the token last pushed */
} Tokens;

/* The wire format is five numbers per token, and every one but the first
   token's line is a delta from the token before it. */
static int tokens_push(Tokens *tokens, unsigned line, unsigned column, unsigned length, unsigned type) {
    if (tokens->count + 5 > tokens->capacity) {
        size_t capacity = tokens->capacity ? tokens->capacity * 2 : 64;
        unsigned *data = realloc(tokens->data, capacity * sizeof(*data));
        if (!data) return -1;
        tokens->data = data;
        tokens->capacity = capacity;
    }
    tokens->data[tokens->count++] = line - tokens->line;
    tokens->data[tokens->count++] = line == tokens->line ? column - tokens->column : column;
    tokens->data[tokens->count++] = length;
    tokens->data[tokens->count++] = type;
    tokens->data[tokens->count++] = 0;
    tokens->line = line;
    tokens->column = column;
    return 0;
}

/* Walks source up to `target`, keeping line and UTF-16 column in step. The
   protocol counts columns in UTF-16 code units, so an astral character costs
   two and everything else costs one. */
static void advance(const char *source, size_t *i, size_t target, unsigned *line, unsigned *column) {
    for (; *i < target; ++*i) {
        unsigned char c = (unsigned char)source[*i];
        if (c == '\n') { ++*line; *column = 0; continue; }
        if ((c & 0xC0) == 0x80) continue;  /* UTF-8 continuation byte */
        *column += c >= 0xF0 ? 2 : 1;
    }
}

static size_t next_word(const char *source, size_t length, size_t i) {
    while (i < length && (source[i] == ' ' || source[i] == '\t')) ++i;
    return i;
}

static int collect_tokens(const char *source, Tokens *tokens) {
    size_t length = strlen(source), i = 0;
    unsigned line = 0, column = 0;
    while (i < length) {
        char c = source[i];
        if (c == '\'' || c == '"' || c == '`') {
            advance(source, &i, sxfe_skip_string(source, length, i), &line, &column);
            continue;
        }
        size_t comment = sxfe_skip_comment(source, length, i);
        if (comment != i) { advance(source, &i, comment, &line, &column); continue; }

        /* `safe` qualifies a declaration or it is just a name. src/frontend.c
           accepts it only before `let` or `const`, so that is where it is a
           keyword. */
        if (sxfe_word_at(source, length, i, "safe")) {
            size_t after = next_word(source, length, i + 4);
            if (sxfe_word_at(source, length, after, "let") || sxfe_word_at(source, length, after, "const")) {
                if (tokens_push(tokens, line, column, 4, TOKEN_KEYWORD)) return -1;
                advance(source, &i, i + 4, &line, &column);
                continue;
            }
        }

        size_t matched = 0;
        for (size_t k = 0; k < sizeof(OWNERSHIP_KEYWORDS) / sizeof(OWNERSHIP_KEYWORDS[0]); ++k) {
            if (sxfe_word_at(source, length, i, OWNERSHIP_KEYWORDS[k])) {
                matched = strlen(OWNERSHIP_KEYWORDS[k]);
                break;
            }
        }
        if (matched) {
            if (tokens_push(tokens, line, column, (unsigned)matched, TOKEN_KEYWORD)) return -1;
            advance(source, &i, i + matched, &line, &column);
            continue;
        }

        /* The borrow sigil, but only the exclusive one: a bare `&` is still
           bitwise-and, and nothing here can tell the two apart. */
        if (c == '&' && sxfe_word_at(source, length, next_word(source, length, i + 1), "mut")) {
            if (tokens_push(tokens, line, column, 1, TOKEN_OPERATOR)) return -1;
            advance(source, &i, i + 1, &line, &column);
            continue;
        }

        /* Step over a whole identifier so its tail is never mistaken for a
           keyword, but only when one starts here: stepping from the character
           before would swallow the next word untested. */
        size_t end = i + 1;
        if (sxfe_ident_char(c)) while (end < length && sxfe_ident_char(source[end])) ++end;
        advance(source, &i, end, &line, &column);
    }
    return 0;
}

static void respond_semantic_tokens(const char *message, long id) {
    char *uri = json_string(message, "uri");
    const char *source = uri ? document_text(uri) : NULL;
    Tokens tokens = {0};
    int failed = source ? collect_tokens(source, &tokens) : 0;
    free(uri);

    size_t capacity = 64 + tokens.count * 12;
    char *response = malloc(capacity);
    if (!response || failed) {
        free(response);
        free(tokens.data);
        char error[128];
        snprintf(error, sizeof(error), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":null}", id);
        respond(error);
        return;
    }
    int written = snprintf(response, capacity, "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"data\":[", id);
    for (size_t n = 0; n < tokens.count; ++n)
        written += snprintf(response + written, capacity - (size_t)written, n ? ",%u" : "%u", tokens.data[n]);
    snprintf(response + written, capacity - (size_t)written, "]}}");
    respond(response);
    free(response);
    free(tokens.data);
}

int sxn_lsp_main(void) {
    char header[256];
    while (fgets(header, sizeof(header), stdin)) {
        size_t length = 0;
        if (sscanf(header, "Content-Length: %zu", &length) != 1) continue;
        while (fgets(header, sizeof(header), stdin) && strcmp(header, "\r\n") && strcmp(header, "\n")) {}
        char *message = malloc(length + 1);
        if (!message || fread(message, 1, length, stdin) != length) { free(message); return 1; }
        message[length] = 0;
        long id = json_id(message);
        if (strstr(message, "\"method\":\"initialize\"") || strstr(message, "\"method\": \"initialize\"")) {
            char response[1024];
            snprintf(response, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"capabilities\":{\"textDocumentSync\":1,\"hoverProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,\"documentSymbolProvider\":true,\"renameProvider\":true,\"documentFormattingProvider\":true,\"completionProvider\":{\"triggerCharacters\":[\".\",\"&\"]},\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":[\"keyword\",\"operator\"],\"tokenModifiers\":[]},\"full\":true}}}}", id);
            respond(response);
        /* Document notifications come before the method tests below, which
           match anywhere in the message: a file containing the word "exit"
           would otherwise shut the server down as it was opened. */
        } else if (strstr(message, "textDocument/didOpen") || strstr(message, "textDocument/didChange")) {
            document_store(json_string(message, "uri"), json_string(message, "text"));
        } else if (strstr(message, "textDocument/didClose")) {
            document_close(json_string(message, "uri"));
        } else if (strstr(message, "textDocument/semanticTokens")) {
            respond_semantic_tokens(message, id);
        } else if (strstr(message, "shutdown")) {
            char response[128]; snprintf(response, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":null}", id); respond(response);
        } else if (strstr(message, "textDocument/hover") || strstr(message, "textDocument/definition") ||
                   strstr(message, "textDocument/references") || strstr(message, "textDocument/rename") ||
                   strstr(message, "textDocument/formatting") || strstr(message, "textDocument/completion") ||
                   strstr(message, "textDocument/documentSymbol")) {
            char response[128]; snprintf(response, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":null}", id); respond(response);
        } else if (strstr(message, "exit")) { free(message); return 0; }
        free(message);
    }
    return 0;
}
