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
            snprintf(response, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":{\"capabilities\":{\"textDocumentSync\":1,\"hoverProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,\"documentSymbolProvider\":true,\"renameProvider\":true,\"documentFormattingProvider\":true,\"completionProvider\":{\"triggerCharacters\":[\".\",\"&\"]}}}}", id);
            respond(response);
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

