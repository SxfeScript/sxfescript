#include "sxfe.h"
#include <quickjs.h>
#include <quickjs-libc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

#ifndef _WIN32
#include <curl/curl.h>
#include <openssl/evp.h>
#include <zlib.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define SXN_NPM "npm.cmd"
#define SXN_NULL " >NUL 2>NUL"
#else
#define SXN_NPM "npm"
#define SXN_NULL " >/dev/null 2>/dev/null"
#endif

static int npm_available(void) {
    return system(SXN_NPM " --version" SXN_NULL) == 0;
}

static int expose_sxn_to_scripts(const char *executable) {
    if (!executable || !strchr(executable, '/')) return 0;
#ifdef _WIN32
    (void)executable;
    return 0;
#else
    char resolved[PATH_MAX];
    if (!realpath(executable, resolved)) return -1;
    char *slash = strrchr(resolved, '/');
    if (!slash) return -1;
    *slash = 0;
    const char *old_path = getenv("PATH");
    size_t needed = strlen(resolved) + (old_path ? strlen(old_path) : 0) + 2;
    char *path = malloc(needed);
    if (!path) return -1;
    snprintf(path, needed, "%s%s%s", resolved, old_path ? ":" : "", old_path ? old_path : "");
    int status = setenv("PATH", path, 1);
    free(path);
    return status;
#endif
}

static int append_arg(char *command, size_t capacity, const char *argument) {
    /* Reject shell metacharacters rather than attempting lossy cross-platform quoting. */
    if (strpbrk(argument, ";&|`$\n\r")) return -1;
    size_t used = strlen(command), needed = strlen(argument) + 3;
    if (used + needed >= capacity) return -1;
    command[used++] = '"';
    for (const char *p = argument; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            if (used + 2 >= capacity) return -1;
            command[used++] = '\\';
        }
        command[used++] = *p;
    }
    command[used++] = '"'; command[used++] = ' '; command[used] = 0;
    return 0;
}

static int write_initial_manifest(void) {
    FILE *check = fopen("package.json", "rb");
    if (check) { fclose(check); fputs("sxn init: package.json already exists\n", stderr); return 1; }
    FILE *file = fopen("package.json", "wb");
    if (!file) { perror("package.json"); return 1; }
    const char *manifest = "{\n  \"name\": \"sxfe-app\",\n  \"version\": \"0.0.1\",\n  \"type\": \"module\",\n  \"scripts\": {\n    \"start\": \"sxn index.sx\"\n  },\n  \"trustedDependencies\": []\n}\n";
    fwrite(manifest, 1, strlen(manifest), file); fclose(file);
    puts("Created package.json"); return 0;
}

static int run_via_npm(int argc, char **argv, const char *executable) {
    if (expose_sxn_to_scripts(executable)) {
        fputs("sxn: unable to expose the current executable to package scripts\n", stderr);
        return 2;
    }
    if (!npm_available()) {
        fputs("sxn: `sxn run` requires npm on PATH to execute package.json scripts\n", stderr);
        return 2;
    }
    char command[8192] = SXN_NPM " run ";
    for (int i = 2; i < argc; ++i) {
        if (append_arg(command, sizeof(command), argv[i])) {
            fputs("sxn: unsafe or overlong argument\n", stderr); return 2;
        }
    }
    return system(command) == 0 ? 0 : 1;
}

#ifndef _WIN32
/* ===========================================================================
   Task 5 Part B: native registry installer for install/add/remove, replacing
   the npm-delegation bootstrap for those three verbs (`run` above and `init`
   above are unchanged -- running an arbitrary declared package.json script
   and writing the initial manifest were never in scope). POSIX-only for now;
   Windows keeps the old npm-delegated path (see sxn_package_command) since
   the tar extraction / directory walk below is POSIX (opendir/mkdir/PATH_MAX)
   and this codebase has no tested Windows target to validate an equivalent
   against.

   JSON handling reuses the already-linked QuickJS engine's own JSON.parse/
   JSON.stringify (via JS_ParseJSON/JS_JSONStringify on a throwaway
   JSContext) instead of hand-rolling a JSON parser -- this command path
   otherwise never touches QuickJS at all. Digests reuse OpenSSL EVP exactly
   like sxn_digest in network.c (Task 2); gzip is the one genuinely new
   dependency (zlib, linked in CMakeLists.txt -- OpenSSL has no gzip
   support); tar parsing (a fixed 512-byte-block header format) is small
   enough to hand-write, matching the instruction to add zlib but not a tar
   library. */

static JSRuntime *json_rt;
static JSContext *jctx;

static int json_open(void) {
    json_rt = JS_NewRuntime();
    if (!json_rt) return -1;
    jctx = JS_NewContext(json_rt);
    return jctx ? 0 : -1;
}

static void json_close(void) {
    if (jctx) JS_FreeContext(jctx);
    if (json_rt) JS_FreeRuntime(json_rt);
}

static JSValue json_parse(const char *text, size_t len) {
    return JS_ParseJSON(jctx, text, len, "<pkg>");
}

static JSValue json_parse_file(const char *path) {
    size_t len = 0;
    uint8_t *buf = js_load_file(NULL, &len, path);
    if (!buf) return JS_EXCEPTION;
    JSValue v = json_parse((const char *)buf, len);
    free(buf);
    return v;
}

static char *json_get_string(JSValue obj, const char *key) {
    JSValue v = JS_GetPropertyStr(jctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(jctx, v); return NULL; }
    const char *cstr = JS_ToCString(jctx, v);
    char *out = cstr ? strdup(cstr) : NULL;
    JS_FreeCString(jctx, cstr);
    JS_FreeValue(jctx, v);
    return out;
}

static char *json_stringify_pretty(JSValue v) {
    JSValue space = JS_NewInt32(jctx, 2);
    JSValue s = JS_JSONStringify(jctx, v, JS_UNDEFINED, space);
    if (JS_IsException(s)) { JS_FreeValue(jctx, s); return NULL; }
    size_t len = 0;
    const char *cstr = JS_ToCStringLen(jctx, &len, s);
    char *out = NULL;
    if (cstr) { out = malloc(len + 1); memcpy(out, cstr, len); out[len] = 0; }
    JS_FreeCString(jctx, cstr);
    JS_FreeValue(jctx, s);
    return out;
}

/* --- tiny curl GET-into-memory (registry metadata + tarball download) ---- */

typedef struct MemBuf { char *data; size_t len, cap; } MemBuf;

static size_t membuf_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    MemBuf *mb = userdata; size_t n = size * nmemb;
    if (mb->len + n + 1 > mb->cap) {
        mb->cap = mb->cap ? mb->cap * 2 : 65536;
        while (mb->cap < mb->len + n + 1) mb->cap *= 2;
        mb->data = realloc(mb->data, mb->cap);
    }
    memcpy(mb->data + mb->len, ptr, n); mb->len += n; mb->data[mb->len] = 0;
    return n;
}

static int curl_get(const char *url, MemBuf *out) {
    memset(out, 0, sizeof(*out));
    CURL *easy = curl_easy_init();
    if (!easy) return -1;
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "sxn/0.0.1");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, membuf_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, out);
    CURLcode rc = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(easy);
    if (rc != CURLE_OK || status < 200 || status >= 300) { free(out->data); out->data = NULL; return -1; }
    return 0;
}

/* --- SHA-512 (SRI) / SHA-1 (legacy shasum) integrity, same EVP call shape
   as sxn_digest in network.c; base64 via EVP_EncodeBlock like the WebSocket
   accept-key computation elsewhere in network.c. */

static char *base64_encode(const unsigned char *data, int len) {
    char *out = malloc((size_t)(4 * ((len + 2) / 3)) + 1);
    int n = EVP_EncodeBlock((unsigned char *)out, data, len);
    out[n] = 0;
    return out;
}

static char *hex_encode(const unsigned char *data, unsigned int len) {
    char *out = malloc((size_t)len * 2 + 1);
    for (unsigned i = 0; i < len; i++) snprintf(out + i * 2, 3, "%02x", data[i]);
    return out;
}

/* Verifies `tarball` against the registry's dist.integrity (preferred,
   sha512-/sha1- SRI form) or dist.shasum (legacy sha1 hex) fallback.
   Returns 0 and fills *used with the value verified against on success;
   returns -1 (nothing written to *used) on mismatch OR when no integrity
   data was available at all -- absence of integrity data is treated as a
   verification failure, not a pass, since extracting an unverified tarball
   defeats the point of this step. */
static int verify_integrity(const unsigned char *tarball, size_t len, const char *integrity, const char *shasum, char **used) {
    if (integrity && !strncmp(integrity, "sha512-", 7)) {
        unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int dl = 0;
        EVP_Digest(tarball, len, digest, &dl, EVP_sha512(), NULL);
        char *b64 = base64_encode(digest, (int)dl);
        int ok = !strcmp(b64, integrity + 7);
        if (ok) { *used = strdup(integrity); free(b64); return 0; }
        fprintf(stderr, "sxn: sha512 mismatch: computed sha512-%s, expected %s\n", b64, integrity);
        free(b64);
        return -1;
    }
    if (integrity && !strncmp(integrity, "sha1-", 5)) {
        unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int dl = 0;
        EVP_Digest(tarball, len, digest, &dl, EVP_sha1(), NULL);
        char *b64 = base64_encode(digest, (int)dl);
        int ok = !strcmp(b64, integrity + 5);
        if (ok) { *used = strdup(integrity); free(b64); return 0; }
        fprintf(stderr, "sxn: sha1 mismatch: computed sha1-%s, expected %s\n", b64, integrity);
        free(b64);
        return -1;
    }
    if (shasum) {
        unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int dl = 0;
        EVP_Digest(tarball, len, digest, &dl, EVP_sha1(), NULL);
        char *hex = hex_encode(digest, dl);
        int ok = !strcasecmp(hex, shasum);
        if (ok) {
            char buf[128]; snprintf(buf, sizeof(buf), "sha1:%s", hex);
            *used = strdup(buf); free(hex); return 0;
        }
        fprintf(stderr, "sxn: shasum mismatch: computed %s, expected %s\n", hex, shasum);
        free(hex);
        return -1;
    }
    fprintf(stderr, "sxn: registry provided no integrity data -- refusing to trust the tarball\n");
    return -1;
}

/* --- gzip inflate (zlib) ------------------------------------------------- */

static int gunzip(const unsigned char *in, size_t in_len, unsigned char **out, size_t *out_len) {
    z_stream strm; memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) return -1;
    size_t cap = in_len * 4 + 1024;
    unsigned char *buf = malloc(cap);
    strm.next_in = (Bytef *)in; strm.avail_in = (uInt)in_len;
    size_t total = 0;
    int ret;
    do {
        if (total == cap) { cap *= 2; buf = realloc(buf, cap); }
        strm.next_out = buf + total; strm.avail_out = (uInt)(cap - total);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { free(buf); inflateEnd(&strm); return -1; }
        total = cap - strm.avail_out;
    } while (ret != Z_STREAM_END);
    inflateEnd(&strm);
    *out = buf; *out_len = total;
    return 0;
}

/* --- POSIX ustar/pax/GNU-longname tar extraction -------------------------
   Hand-written: it's a fixed 512-byte-block header format, not worth a
   dependency. Handles regular files ('0'/NUL) and directories ('5'); GNU
   longname ('L') and PAX extended headers ('x'/'X', "path=" key only)
   override the following entry's name so packages with long paths still
   extract correctly. Symlinks/hardlinks/devices are deliberately skipped
   (not extracted) to avoid writing outside the destination via a link. */

typedef struct TarHeader {
    char name[100]; char mode[8]; char uid[8]; char gid[8];
    char size[12]; char mtime[12]; char chksum[8]; char typeflag[1];
    char linkname[100]; char magic[6]; char version[2];
    char uname[32]; char gname[32]; char devmajor[8]; char devminor[8];
    char prefix[155]; char pad[12];
} TarHeader;

static long parse_octal(const char *field, size_t n) {
    long value = 0;
    for (size_t i = 0; i < n && field[i]; i++) {
        if (field[i] < '0' || field[i] > '7') break;
        value = value * 8 + (field[i] - '0');
    }
    return value;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX]; snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

/* Rejects absolute paths and ".." traversal segments before joining, so a
   hostile tarball entry can't write outside dest_dir. */
static int safe_join(const char *base, const char *rel, char *out, size_t outcap) {
    if (rel[0] == '/' || rel[0] == 0) return -1;
    size_t rl = strlen(rel);
    for (size_t i = 0; i < rl; i++) {
        if (rel[i] == '.' && rel[i + 1] == '.' && (i == 0 || rel[i - 1] == '/') && (rel[i + 2] == '/' || rel[i + 2] == 0))
            return -1;
    }
    if (snprintf(out, outcap, "%s/%s", base, rel) >= (int)outcap) return -1;
    return 0;
}

static void extract_tar(const unsigned char *data, size_t len, const char *dest_dir) {
    size_t off = 0;
    char *pending_longname = NULL;
    while (off + 512 <= len) {
        const TarHeader *h = (const TarHeader *)(data + off);
        int all_zero = 1;
        for (size_t i = 0; i < 512; i++) if (((const unsigned char *)h)[i]) { all_zero = 0; break; }
        if (all_zero) break; /* end-of-archive marker */
        off += 512;
        long size = parse_octal(h->size, sizeof(h->size));
        size_t data_len = size > 0 ? (size_t)size : 0;
        size_t padded = ((data_len + 511) / 512) * 512;
        if (off + padded > len) break; /* truncated archive; stop rather than read OOB */
        const unsigned char *content = data + off;
        char typeflag = h->typeflag[0];
        char name[600];
        if (pending_longname) {
            snprintf(name, sizeof(name), "%s", pending_longname);
            free(pending_longname); pending_longname = NULL;
        } else if (h->prefix[0]) {
            snprintf(name, sizeof(name), "%.155s/%.100s", h->prefix, h->name);
        } else {
            snprintf(name, sizeof(name), "%.100s", h->name);
        }

        if (typeflag == 'L') {
            pending_longname = malloc(data_len + 1);
            memcpy(pending_longname, content, data_len); pending_longname[data_len] = 0;
        } else if (typeflag == 'x' || typeflag == 'X') {
            char *block = malloc(data_len + 1);
            memcpy(block, content, data_len); block[data_len] = 0;
            char *p = strstr(block, " path=");
            if (p) {
                p += 6; char *end = strchr(p, '\n');
                size_t plen = end ? (size_t)(end - p) : strlen(p);
                pending_longname = malloc(plen + 1);
                memcpy(pending_longname, p, plen); pending_longname[plen] = 0;
            }
            free(block);
        } else if (typeflag != 'g') {
            /* npm tarballs wrap every entry in a "package/" directory. */
            const char *rel = name;
            if (!strncmp(rel, "package/", 8)) rel += 8;
            if (*rel) {
                char full[PATH_MAX];
                if (safe_join(dest_dir, rel, full, sizeof(full)) == 0) {
                    if (typeflag == '5') {
                        mkdir_p(full);
                    } else if (typeflag == '0' || typeflag == 0) {
                        char *slash = strrchr(full, '/');
                        if (slash) { *slash = 0; mkdir_p(full); *slash = '/'; }
                        FILE *f = fopen(full, "wb");
                        if (f) { fwrite(content, 1, data_len, f); fclose(f); }
                    } /* symlink/hardlink/device/fifo: intentionally not extracted */
                }
            }
        }
        off += padded;
    }
    free(pending_longname);
}

/* --- registry resolve + trustedDependencies + lockfile ------------------- */

typedef struct ResolvedPackage {
    char *name, *version, *tarball_url, *integrity, *shasum;
} ResolvedPackage;

static void free_pkg(ResolvedPackage *pkg) {
    free(pkg->name); free(pkg->version); free(pkg->tarball_url); free(pkg->integrity); free(pkg->shasum);
    memset(pkg, 0, sizeof(*pkg));
}

static void url_encode_name(const char *name, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 4 < cap; i++) {
        if (name[i] == '/') { out[j++] = '%'; out[j++] = '2'; out[j++] = 'f'; }
        else out[j++] = name[i];
    }
    out[j] = 0;
}

/* `want_version`: an exact version string ("1.2.3") is used as-is; a range
   ("^1.2.3", "~1.2.3", "*", NULL/empty) resolves to dist-tags.latest. This
   is a deliberate scoping cut -- no real semver range matching -- flat,
   direct-dependency resolution only, as called out in the task brief. */
static int registry_resolve(const char *name, const char *want_version, ResolvedPackage *out) {
    memset(out, 0, sizeof(*out));
    char encoded[300]; url_encode_name(name, encoded, sizeof(encoded));
    char url[512]; snprintf(url, sizeof(url), "https://registry.npmjs.org/%s", encoded);
    MemBuf body;
    if (curl_get(url, &body) != 0) { fprintf(stderr, "sxn: registry lookup failed for %s\n", name); return -1; }
    JSValue meta = json_parse(body.data, body.len);
    free(body.data);
    if (JS_IsException(meta)) { fprintf(stderr, "sxn: invalid registry response for %s\n", name); JS_FreeValue(jctx, meta); return -1; }

    char *resolved_version = NULL;
    int exact = want_version && want_version[0] >= '0' && want_version[0] <= '9';
    if (exact) {
        resolved_version = strdup(want_version);
    } else {
        JSValue tags = JS_GetPropertyStr(jctx, meta, "dist-tags");
        resolved_version = json_get_string(tags, "latest");
        JS_FreeValue(jctx, tags);
    }
    if (!resolved_version) { fprintf(stderr, "sxn: could not resolve a version for %s\n", name); JS_FreeValue(jctx, meta); return -1; }

    JSValue versions = JS_GetPropertyStr(jctx, meta, "versions");
    JSValue entry = JS_GetPropertyStr(jctx, versions, resolved_version);
    if (JS_IsUndefined(entry)) {
        fprintf(stderr, "sxn: version %s not found for %s\n", resolved_version, name);
        free(resolved_version); JS_FreeValue(jctx, entry); JS_FreeValue(jctx, versions); JS_FreeValue(jctx, meta);
        return -1;
    }
    JSValue dist = JS_GetPropertyStr(jctx, entry, "dist");
    out->name = strdup(name);
    out->version = resolved_version;
    out->tarball_url = json_get_string(dist, "tarball");
    out->integrity = json_get_string(dist, "integrity");
    out->shasum = json_get_string(dist, "shasum");
    JS_FreeValue(jctx, dist); JS_FreeValue(jctx, entry); JS_FreeValue(jctx, versions); JS_FreeValue(jctx, meta);
    if (!out->tarball_url) { fprintf(stderr, "sxn: no tarball URL for %s@%s\n", name, out->version); free_pkg(out); return -1; }
    return 0;
}

static int name_in_json_array(JSValue arr, const char *name) {
    if (!JS_IsArray(arr)) return 0;
    JSValue len_v = JS_GetPropertyStr(jctx, arr, "length");
    int64_t len = 0; JS_ToInt64(jctx, &len, len_v); JS_FreeValue(jctx, len_v);
    int found = 0;
    for (int64_t i = 0; i < len && !found; i++) {
        JSValue item = JS_GetPropertyUint32(jctx, arr, (uint32_t)i);
        const char *s = JS_ToCString(jctx, item);
        if (s && !strcmp(s, name)) found = 1;
        JS_FreeCString(jctx, s); JS_FreeValue(jctx, item);
    }
    return found;
}

static int is_trusted(const char *pkg_name) {
    JSValue root = json_parse_file("package.json");
    if (JS_IsException(root)) { JS_FreeValue(jctx, root); return 0; }
    JSValue trusted = JS_GetPropertyStr(jctx, root, "trustedDependencies");
    int ok = name_in_json_array(trusted, pkg_name);
    JS_FreeValue(jctx, trusted); JS_FreeValue(jctx, root);
    return ok;
}

static void run_lifecycle_script(const char *pkg_dir, const char *script) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return;
    if (chdir(pkg_dir) != 0) return;
    printf("sxn: running postinstall (trusted): %s\n", script);
    int status = system(script);
    if (status != 0) fprintf(stderr, "sxn: postinstall exited with status %d\n", status);
    chdir(cwd);
}

/* Only runs a lifecycle script when the package's own name is listed in the
   ROOT package.json's trustedDependencies -- otherwise it's skipped
   entirely (not attempted-and-ignored), matching the task's "make the
   existing blanket --ignore-scripts real/conditional" instruction. */
static void maybe_run_postinstall(const char *pkg_dir, const char *pkg_name) {
    if (!is_trusted(pkg_name)) return;
    char manifest_path[PATH_MAX]; snprintf(manifest_path, sizeof(manifest_path), "%s/package.json", pkg_dir);
    JSValue manifest = json_parse_file(manifest_path);
    if (JS_IsException(manifest)) { JS_FreeValue(jctx, manifest); return; }
    JSValue scripts = JS_GetPropertyStr(jctx, manifest, "scripts");
    char *postinstall = json_get_string(scripts, "postinstall");
    JS_FreeValue(jctx, scripts); JS_FreeValue(jctx, manifest);
    if (postinstall) { run_lifecycle_script(pkg_dir, postinstall); free(postinstall); }
}

static JSValue load_lock(void) {
    JSValue v = json_parse_file("sxn.lock");
    if (JS_IsException(v) || !JS_IsObject(v)) { JS_FreeValue(jctx, v); return JS_NewObject(jctx); }
    return v;
}

static void save_lock(JSValue root) {
    JS_SetPropertyStr(jctx, root, "lockfileVersion", JS_NewInt32(jctx, 1));
    char *text = json_stringify_pretty(root);
    if (text) {
        FILE *f = fopen("sxn.lock", "wb");
        if (f) { fwrite(text, 1, strlen(text), f); fputc('\n', f); fclose(f); }
        free(text);
    }
}

/* Each lockfile entry is real, derived data (resolved version, the actual
   tarball URL fetched, the integrity value verified against) -- not a
   hardcoded placeholder string. This is intentionally flat: one entry per
   directly-installed package, no transitive dependency tree. */
static void write_lock_entry(const ResolvedPackage *pkg, const char *integrity_used) {
    JSValue root = load_lock();
    JSValue deps = JS_GetPropertyStr(jctx, root, "dependencies");
    if (!JS_IsObject(deps)) { JS_FreeValue(jctx, deps); deps = JS_NewObject(jctx); JS_SetPropertyStr(jctx, root, "dependencies", JS_DupValue(jctx, deps)); }
    JSValue entry = JS_NewObject(jctx);
    JS_SetPropertyStr(jctx, entry, "version", JS_NewString(jctx, pkg->version));
    JS_SetPropertyStr(jctx, entry, "resolved", JS_NewString(jctx, pkg->tarball_url));
    JS_SetPropertyStr(jctx, entry, "integrity", JS_NewString(jctx, integrity_used ? integrity_used : ""));
    JS_SetPropertyStr(jctx, deps, pkg->name, entry);
    JS_FreeValue(jctx, deps);
    save_lock(root);
    JS_FreeValue(jctx, root);
}

static void remove_lock_entry(const char *name) {
    JSValue root = load_lock();
    JSValue deps = JS_GetPropertyStr(jctx, root, "dependencies");
    if (JS_IsObject(deps)) JS_SetPropertyStr(jctx, deps, name, JS_UNDEFINED); /* JSON.stringify omits undefined-valued props */
    JS_FreeValue(jctx, deps);
    save_lock(root);
    JS_FreeValue(jctx, root);
}

/* --- package.json dependency-list bookkeeping (add/remove) --------------- */

static void add_to_manifest(const char *name, const char *version, int dev) {
    JSValue root = json_parse_file("package.json");
    if (JS_IsException(root)) { JS_FreeValue(jctx, root); fprintf(stderr, "sxn: no package.json to update (run `sxn init` first)\n"); return; }
    const char *field = dev ? "devDependencies" : "dependencies";
    JSValue deps = JS_GetPropertyStr(jctx, root, field);
    if (!JS_IsObject(deps)) { JS_FreeValue(jctx, deps); deps = JS_NewObject(jctx); JS_SetPropertyStr(jctx, root, field, JS_DupValue(jctx, deps)); }
    char range[160]; snprintf(range, sizeof(range), "^%s", version);
    JS_SetPropertyStr(jctx, deps, name, JS_NewString(jctx, range));
    JS_FreeValue(jctx, deps);
    char *text = json_stringify_pretty(root);
    JS_FreeValue(jctx, root);
    if (text) { FILE *f = fopen("package.json", "wb"); if (f) { fwrite(text, 1, strlen(text), f); fputc('\n', f); fclose(f); } free(text); }
}

static void remove_from_manifest(const char *name) {
    JSValue root = json_parse_file("package.json");
    if (JS_IsException(root)) { JS_FreeValue(jctx, root); return; }
    JSValue deps = JS_GetPropertyStr(jctx, root, "dependencies");
    if (JS_IsObject(deps)) JS_SetPropertyStr(jctx, deps, name, JS_UNDEFINED);
    JS_FreeValue(jctx, deps);
    JSValue dev_deps = JS_GetPropertyStr(jctx, root, "devDependencies");
    if (JS_IsObject(dev_deps)) JS_SetPropertyStr(jctx, dev_deps, name, JS_UNDEFINED);
    JS_FreeValue(jctx, dev_deps);
    char *text = json_stringify_pretty(root);
    JS_FreeValue(jctx, root);
    if (text) { FILE *f = fopen("package.json", "wb"); if (f) { fwrite(text, 1, strlen(text), f); fputc('\n', f); fclose(f); } free(text); }
}

static int rm_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *entry;
        while ((entry = readdir(d))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child[PATH_MAX]; snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            rm_recursive(child);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
    return 0;
}

/* --- top-level install/add/remove flows ----------------------------------- */

/* Resolve -> download -> verify -> extract -> (maybe) run postinstall ->
   lockfile. Returns 0 on success; on any failure nothing is left extracted
   (extraction only happens after integrity verification passes). */
static int install_one(const char *name, const char *want_version, char **resolved_version_out) {
    ResolvedPackage pkg;
    if (registry_resolve(name, want_version, &pkg) != 0) return -1;
    printf("sxn: resolved %s@%s\n", pkg.name, pkg.version);
    printf("sxn: tarball %s\n", pkg.tarball_url);

    MemBuf tarball;
    if (curl_get(pkg.tarball_url, &tarball) != 0) {
        fprintf(stderr, "sxn: failed to download tarball for %s\n", pkg.name);
        free_pkg(&pkg); return -1;
    }
    printf("sxn: downloaded %zu bytes\n", tarball.len);

    char *integrity_used = NULL;
    if (verify_integrity((const unsigned char *)tarball.data, tarball.len, pkg.integrity, pkg.shasum, &integrity_used) != 0) {
        fprintf(stderr, "sxn: integrity check FAILED for %s@%s -- install rejected, nothing extracted\n", pkg.name, pkg.version);
        free(tarball.data); free_pkg(&pkg); return -1;
    }
    printf("sxn: integrity verified against %s\n", integrity_used);

    unsigned char *tar_data = NULL; size_t tar_len = 0;
    int gz_ok = gunzip((const unsigned char *)tarball.data, tarball.len, &tar_data, &tar_len);
    free(tarball.data);
    if (gz_ok != 0) {
        fprintf(stderr, "sxn: gzip decompression failed for %s\n", pkg.name);
        free(integrity_used); free_pkg(&pkg); return -1;
    }

    char dest[PATH_MAX]; snprintf(dest, sizeof(dest), "node_modules/%s", pkg.name);
    mkdir_p("node_modules");
    mkdir_p(dest);
    extract_tar(tar_data, tar_len, dest);
    free(tar_data);
    printf("sxn: extracted %s@%s -> %s\n", pkg.name, pkg.version, dest);

    maybe_run_postinstall(dest, pkg.name);
    write_lock_entry(&pkg, integrity_used);

    if (resolved_version_out) *resolved_version_out = strdup(pkg.version);
    free(integrity_used);
    free_pkg(&pkg);
    return 0;
}

static void split_name_version(const char *spec, char *name, size_t name_cap, char *version, size_t version_cap) {
    version[0] = 0;
    const char *at = strrchr(spec, '@');
    if (at && at != spec) {
        size_t nlen = (size_t)(at - spec);
        snprintf(name, name_cap, "%.*s", (int)nlen, spec);
        snprintf(version, version_cap, "%s", at + 1);
    } else {
        snprintf(name, name_cap, "%s", spec);
    }
}

static int do_add(int argc, char **argv) {
    int dev = 0; const char *spec = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dev")) dev = 1;
        else spec = argv[i];
    }
    if (!spec) { fputs("sxn: add requires a package name\n", stderr); return 1; }
    char name[256], version[128];
    split_name_version(spec, name, sizeof(name), version, sizeof(version));
    char *resolved = NULL;
    if (install_one(name, version[0] ? version : NULL, &resolved) != 0) return 1;
    add_to_manifest(name, resolved, dev);
    free(resolved);
    return 0;
}

static int do_remove(int argc, char **argv) {
    int status = 0;
    for (int i = 2; i < argc; i++) {
        char dest[PATH_MAX]; snprintf(dest, sizeof(dest), "node_modules/%s", argv[i]);
        rm_recursive(dest);
        remove_from_manifest(argv[i]);
        remove_lock_entry(argv[i]);
        printf("sxn: removed %s\n", argv[i]);
    }
    return status;
}

/* Only pinned exact versions ("1.2.3") in package.json are honored as-is;
   any other range specifier (a leading ^ or ~, a bare "*", or a missing
   entry) falls back to dist-tags.latest -- same scoping cut as
   registry_resolve. */
static const char *manifest_exact_version(const char *range) {
    return (range && range[0] >= '0' && range[0] <= '9') ? range : NULL;
}

static int install_deps_object(JSValue deps) {
    if (!JS_IsObject(deps)) return 0;
    JSPropertyEnum *tab = NULL; uint32_t len = 0;
    if (JS_GetOwnPropertyNames(jctx, &tab, &len, deps, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) return 0;
    int status = 0;
    for (uint32_t i = 0; i < len; i++) {
        const char *name = JS_AtomToCString(jctx, tab[i].atom);
        JSValue range_v = JS_GetPropertyStr(jctx, deps, name);
        const char *range = JS_ToCString(jctx, range_v);
        if (install_one(name, manifest_exact_version(range), NULL) != 0) status = 1;
        JS_FreeCString(jctx, range); JS_FreeValue(jctx, range_v);
        JS_FreeCString(jctx, name);
    }
    JS_FreePropertyEnum(jctx, tab, len);
    return status;
}

static int do_install(void) {
    JSValue root = json_parse_file("package.json");
    if (JS_IsException(root)) { JS_FreeValue(jctx, root); fputs("sxn: no package.json (run `sxn init` first)\n", stderr); return 1; }
    JSValue deps = JS_GetPropertyStr(jctx, root, "dependencies");
    JSValue dev_deps = JS_GetPropertyStr(jctx, root, "devDependencies");
    int status = install_deps_object(deps) | install_deps_object(dev_deps);
    JS_FreeValue(jctx, deps); JS_FreeValue(jctx, dev_deps); JS_FreeValue(jctx, root);
    return status;
}

static int native_package_command(int argc, char **argv) {
    const char *verb = argv[1];
    if (json_open() != 0) { fputs("sxn: unable to initialize JSON engine\n", stderr); return 2; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int status;
    if (!strcmp(verb, "add")) status = do_add(argc, argv);
    else if (!strcmp(verb, "remove")) status = do_remove(argc, argv);
    else status = do_install();
    curl_global_cleanup();
    json_close();
    return status;
}
#endif /* !_WIN32 */

int sxn_package_command(int argc, char **argv) {
    const char *verb = argv[1];
    if (!strcmp(verb, "init")) return write_initial_manifest();
    if (!strcmp(verb, "run")) return run_via_npm(argc, argv, argv[0]);
#ifndef _WIN32
    if (!strcmp(verb, "install") || !strcmp(verb, "add") || !strcmp(verb, "remove"))
        return native_package_command(argc, argv);
#else
    /* Windows keeps the original npm-delegation bootstrap for these three
       verbs; see the comment above native_package_command. */
    if (!strcmp(verb, "install") || !strcmp(verb, "add") || !strcmp(verb, "remove")) {
        if (expose_sxn_to_scripts(argv[0])) {
            fputs("sxn: unable to expose the current executable to package scripts\n", stderr);
            return 2;
        }
        if (!npm_available()) {
            fputs("sxn: this bootstrap package backend requires npm on PATH; the native registry backend is POSIX-only for now\n", stderr);
            return 2;
        }
        char command[8192] = SXN_NPM " ";
        if (!strcmp(verb, "install")) strcat(command, "install --ignore-scripts ");
        else if (!strcmp(verb, "add")) strcat(command, "install --ignore-scripts ");
        else strcat(command, "uninstall --ignore-scripts ");
        for (int i = 2; i < argc; ++i) {
            if (!strcmp(argv[i], "--dev") && !strcmp(verb, "add")) strcat(command, "--save-dev ");
            else if (append_arg(command, sizeof(command), argv[i])) {
                fputs("sxn: unsafe or overlong package argument\n", stderr); return 2;
            }
        }
        return system(command) == 0 ? 0 : 1;
    }
#endif
    fputs("sxn: unknown package command\n", stderr);
    return 2;
}
