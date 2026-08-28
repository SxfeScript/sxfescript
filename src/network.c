#include <quickjs.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include "sxfe.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define countof(x) (sizeof(x) / sizeof((x)[0]))

static int send_all(int fd, const char *data, size_t length) {
    while (length) {
        ssize_t sent = send(fd, data, length, 0);
        if (sent <= 0) return -1;
        data += sent; length -= (size_t)sent;
    }
    return 0;
}

static const char *reason(int status) {
    switch (status) { case 200: return "OK"; case 201: return "Created"; case 204: return "No Content";
        case 400: return "Bad Request"; case 404: return "Not Found"; case 500: return "Internal Server Error";
        default: return "Response"; }
}

static char *header_value(char *request, const char *name) {
    size_t length = strlen(name);
    for (char *line = strstr(request, "\r\n"); line && line[2] && line[2] != '\r';) {
        line += 2;
        if (!strncasecmp(line, name, length) && line[length] == ':') {
            char *value = line + length + 1; while (*value == ' ' || *value == '\t') ++value;
            char *end = strstr(value, "\r\n"); if (end) *end = 0; return value;
        }
        line = strstr(line, "\r\n");
    }
    return NULL;
}

static int websocket_handshake(int fd, const char *key) {
    char joined[256], encoded[64], response[512];
    unsigned char digest[SHA_DIGEST_LENGTH];
    snprintf(joined, sizeof(joined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    SHA1((unsigned char *)joined, strlen(joined), digest);
    EVP_EncodeBlock((unsigned char *)encoded, digest, SHA_DIGEST_LENGTH);
    int n = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", encoded);
    return send_all(fd, response, (size_t)n);
}

static int websocket_text(int fd, const char *text) {
    size_t length = strlen(text); unsigned char header[10]; size_t h = 0;
    header[h++] = 0x81;
    if (length < 126) header[h++] = (unsigned char)length;
    else if (length <= 65535) { header[h++] = 126; header[h++] = (length >> 8) & 255; header[h++] = length & 255; }
    else return -1;
    return send(fd, header, h, 0) == (ssize_t)h && send_all(fd, text, length) == 0 ? 0 : -1;
}

static JSValue js_serve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t port = 3000;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "serve(options, handler) requires a handler");
    JSValue port_value = JS_GetPropertyStr(ctx, argv[0], "port"); JS_ToInt32(ctx, &port, port_value); JS_FreeValue(ctx, port_value);
    int server = socket(AF_INET, SOCK_STREAM, 0), enabled = 1;
    if (server < 0) return JS_ThrowInternalError(ctx, "socket: %s", strerror(errno));
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons((uint16_t)port);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) || listen(server, 64)) {
        close(server); return JS_ThrowInternalError(ctx, "listen on %d: %s", port, strerror(errno));
    }
    for (;;) {
        int client = accept(server, NULL, NULL); if (client < 0) { if (errno == EINTR) continue; break; }
        char buffer[65536]; ssize_t got = recv(client, buffer, sizeof(buffer) - 1, 0);
        if (got <= 0) { close(client); continue; } buffer[got] = 0;
        char method[16] = {0}, url[4096] = {0}; sscanf(buffer, "%15s %4095s", method, url);
        char *body = strstr(buffer, "\r\n\r\n"); body = body ? body + 4 : buffer + got;
        char *ws_key = header_value(buffer, "Sec-WebSocket-Key");
        char *upgrade = header_value(buffer, "Upgrade");
        JSValue request = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, request, "method", JS_NewString(ctx, method));
        JS_SetPropertyStr(ctx, request, "url", JS_NewString(ctx, url));
        JS_SetPropertyStr(ctx, request, "body", JS_NewString(ctx, body));
        JSValue headers = JS_NewObject(ctx);
        if (upgrade) JS_SetPropertyStr(ctx, headers, "upgrade", JS_NewString(ctx, upgrade));
        JS_SetPropertyStr(ctx, request, "headers", headers);
        JSValue result = JS_Call(ctx, argv[1], JS_UNDEFINED, 1, &request); JS_FreeValue(ctx, request);
        if (JS_IsException(result)) { JS_FreeValue(ctx, result); send_all(client, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n", 57); close(client); continue; }
        JSValue mode_value = JS_GetPropertyStr(ctx, result, "mode"); const char *mode = JS_ToCString(ctx, mode_value);
        if (mode && !strcmp(mode, "websocket") && upgrade && ws_key) {
            websocket_handshake(client, ws_key);
            JSValue messages = JS_GetPropertyStr(ctx, result, "wsMessages"); uint32_t length = 0; JSValue size = JS_GetPropertyStr(ctx, messages, "length"); JS_ToUint32(ctx, &length, size); JS_FreeValue(ctx, size);
            for (uint32_t i = 0; i < length; ++i) { JSValue item = JS_GetPropertyUint32(ctx, messages, i); const char *text = JS_ToCString(ctx, item); if (text) websocket_text(client, text); JS_FreeCString(ctx, text); JS_FreeValue(ctx, item); }
            JS_FreeValue(ctx, messages);
        } else if (mode && !strcmp(mode, "sse")) {
            const char *sse_head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n"; send_all(client, sse_head, strlen(sse_head));
            JSValue events = JS_GetPropertyStr(ctx, result, "events"); uint32_t length = 0; JSValue size = JS_GetPropertyStr(ctx, events, "length"); JS_ToUint32(ctx, &length, size); JS_FreeValue(ctx, size);
            for (uint32_t i = 0; i < length; ++i) { JSValue event = JS_GetPropertyUint32(ctx, events, i); JSValue data = JS_GetPropertyStr(ctx, event, "data"); JSValue type = JS_GetPropertyStr(ctx, event, "event"); JSValue id = JS_GetPropertyStr(ctx, event, "id"); const char *text = JS_ToCString(ctx, data), *event_name = JS_ToCString(ctx, type), *event_id = JS_ToCString(ctx, id); char line[4096]; int n = snprintf(line, sizeof(line), "%s%s%s%sdata: %s\n\n", event_name ? "event: " : "", event_name ? event_name : "", event_name ? "\n" : "", event_id ? "id: " : "", text ? text : ""); if (event_id) { char full[4096]; n = snprintf(full, sizeof(full), "event: %s\nid: %s\ndata: %s\n\n", event_name ? event_name : "message", event_id, text ? text : ""); send_all(client, full, (size_t)n); } else send_all(client, line, (size_t)n); JS_FreeCString(ctx, text); JS_FreeCString(ctx, event_name); JS_FreeCString(ctx, event_id); JS_FreeValue(ctx, data); JS_FreeValue(ctx, type); JS_FreeValue(ctx, id); JS_FreeValue(ctx, event); }
            JS_FreeValue(ctx, events);
        } else {
            int32_t status = 200; JSValue status_value = JS_GetPropertyStr(ctx, result, "statusCode"); JS_ToInt32(ctx, &status, status_value); JS_FreeValue(ctx, status_value);
            JSValue body_value = JS_GetPropertyStr(ctx, result, "body"); const char *body = JS_ToCString(ctx, body_value); size_t length = body ? strlen(body) : 0;
            char head[512]; int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n", status, reason(status), length);
            send_all(client, head, (size_t)n); if (body) send_all(client, body, length); JS_FreeCString(ctx, body); JS_FreeValue(ctx, body_value);
        }
        JS_FreeCString(ctx, mode); JS_FreeValue(ctx, mode_value); JS_FreeValue(ctx, result); close(client);
    }
    close(server); return JS_UNDEFINED;
}

typedef struct FetchBuffer { char *data; size_t length; } FetchBuffer;
static size_t fetch_write(char *data, size_t size, size_t count, void *opaque) {
    FetchBuffer *buffer = opaque; size_t amount = size * count;
    char *next = realloc(buffer->data, buffer->length + amount + 1); if (!next) return 0;
    buffer->data = next; memcpy(next + buffer->length, data, amount); buffer->length += amount; next[buffer->length] = 0; return amount;
}
static JSValue fetch_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_GetPropertyStr(ctx, this_val, "_body");
}
static JSValue fetch_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue body = JS_GetPropertyStr(ctx, this_val, "_body"); size_t length = 0; const char *text = JS_ToCStringLen(ctx, &length, body);
    JSValue result = text ? JS_ParseJSON(ctx, text, length, "<fetch>") : JS_EXCEPTION; JS_FreeCString(ctx, text); JS_FreeValue(ctx, body); return result;
}
static JSValue js_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url = argc ? JS_ToCString(ctx, argv[0]) : NULL; if (!url) return JS_EXCEPTION;
    const char *method = NULL, *request_body = NULL;
    JSValue method_value = JS_UNDEFINED, body_value = JS_UNDEFINED;
    if (argc > 1 && JS_IsObject(argv[1])) {
        method_value = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(method_value)) method = JS_ToCString(ctx, method_value);
        body_value = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(body_value)) request_body = JS_ToCString(ctx, body_value);
    }
    CURL *curl = curl_easy_init(); FetchBuffer buffer = {0}; long status = 0;
    if (!curl) { JS_FreeCString(ctx, url); return JS_ThrowInternalError(ctx, "curl initialization failed"); }
    curl_easy_setopt(curl, CURLOPT_URL, url); curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (method) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (request_body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write); curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "sxn/0.1"); CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status); curl_easy_cleanup(curl); JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, method); JS_FreeCString(ctx, request_body); JS_FreeValue(ctx, method_value); JS_FreeValue(ctx, body_value);
    if (code != CURLE_OK) { free(buffer.data); return JS_ThrowInternalError(ctx, "fetch failed: %s", curl_easy_strerror(code)); }
    JSValue result = JS_NewObject(ctx); JS_SetPropertyStr(ctx, result, "status", JS_NewInt32(ctx, (int)status));
    JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    JS_SetPropertyStr(ctx, result, "_body", JS_NewStringLen(ctx, buffer.data ? buffer.data : "", buffer.length));
    JS_SetPropertyStr(ctx, result, "text", JS_NewCFunction(ctx, fetch_text, "text", 0));
    JS_SetPropertyStr(ctx, result, "json", JS_NewCFunction(ctx, fetch_json, "json", 0)); free(buffer.data); return result;
}

static JSValue sxn_file_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue path_value = JS_GetPropertyStr(ctx, this_val, "path");
    const char *path = JS_ToCString(ctx, path_value);
    if (!path) { JS_FreeValue(ctx, path_value); return JS_EXCEPTION; }
    FILE *file = fopen(path, "rb");
    if (!file) { JSValue error = JS_ThrowInternalError(ctx, "cannot read '%s': %s", path, strerror(errno)); JS_FreeCString(ctx, path); JS_FreeValue(ctx, path_value); return error; }
    fseek(file, 0, SEEK_END); long length = ftell(file); rewind(file);
    char *data = malloc((size_t)length + 1);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        fclose(file); free(data); JS_FreeCString(ctx, path); JS_FreeValue(ctx, path_value);
        return JS_ThrowInternalError(ctx, "cannot read file");
    }
    fclose(file); data[length] = 0;
    JSValue result = JS_NewStringLen(ctx, data, (size_t)length);
    free(data); JS_FreeCString(ctx, path); JS_FreeValue(ctx, path_value); return result;
}

static JSValue sxn_file_exists(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue path_value = JS_GetPropertyStr(ctx, this_val, "path");
    const char *path = JS_ToCString(ctx, path_value); struct stat info;
    int exists = path && stat(path, &info) == 0;
    JS_FreeCString(ctx, path); JS_FreeValue(ctx, path_value); return JS_NewBool(ctx, exists);
}

static JSValue sxn_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *path = argc ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!path) return JS_ThrowTypeError(ctx, "Sxn.file(path) requires a path");
    struct stat info; int exists = stat(path, &info) == 0;
    JSValue file = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, file, "path", JS_NewString(ctx, path));
    JS_SetPropertyStr(ctx, file, "size", JS_NewInt64(ctx, exists ? (int64_t)info.st_size : 0));
    JS_SetPropertyStr(ctx, file, "type", JS_NewString(ctx, "application/octet-stream"));
    JS_SetPropertyStr(ctx, file, "text", JS_NewCFunction(ctx, sxn_file_text, "text", 0));
    JS_SetPropertyStr(ctx, file, "exists", JS_NewCFunction(ctx, sxn_file_exists, "exists", 0));
    JS_FreeCString(ctx, path); return file;
}

static JSValue sxn_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "Sxn.write(destination, data) requires two arguments");
    const char *path = JS_ToCString(ctx, argv[0]); size_t length = 0;
    const char *data = JS_ToCStringLen(ctx, &length, argv[1]);
    if (!path || !data) { JS_FreeCString(ctx, path); JS_FreeCString(ctx, data); return JS_EXCEPTION; }
    FILE *file = fopen(path, "wb");
    if (!file) { JSValue error = JS_ThrowInternalError(ctx, "cannot write '%s': %s", path, strerror(errno)); JS_FreeCString(ctx, path); JS_FreeCString(ctx, data); return error; }
    size_t written = fwrite(data, 1, length, file); int failed = fclose(file) != 0 || written != length;
    JS_FreeCString(ctx, path); JS_FreeCString(ctx, data);
    if (failed) return JS_ThrowInternalError(ctx, "file write failed");
    return JS_NewInt64(ctx, (int64_t)written);
}

int sxn_install_network(JSContext *ctx) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return -1;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue runtime = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, runtime, "version", JS_NewString(ctx, "0.1.0"));
    JS_SetPropertyStr(ctx, runtime, "serve", JS_NewCFunction(ctx, js_serve, "serve", 2));
    JS_SetPropertyStr(ctx, runtime, "file", JS_NewCFunction(ctx, sxn_file, "file", 1));
    JS_SetPropertyStr(ctx, runtime, "write", JS_NewCFunction(ctx, sxn_write, "write", 2));
    JS_SetPropertyStr(ctx, runtime, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    JS_SetPropertyStr(ctx, global, "Sxn", runtime);
    JS_SetPropertyStr(ctx, global, "__sxnServe", JS_NewCFunction(ctx, js_serve, "__sxnServe", 2));
    JS_SetPropertyStr(ctx, global, "fetch", JS_NewCFunction(ctx, js_fetch, "fetch", 2));
    JS_FreeValue(ctx, global); return 0;
}
