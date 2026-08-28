#include "sxfe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <limits.h>
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
    const char *manifest = "{\n  \"name\": \"sxfe-app\",\n  \"version\": \"0.1.0\",\n  \"type\": \"module\",\n  \"scripts\": {\n    \"start\": \"sxn index.sx\"\n  },\n  \"trustedDependencies\": []\n}\n";
    fwrite(manifest, 1, strlen(manifest), file); fclose(file);
    puts("Created package.json"); return 0;
}

int sxn_package_command(int argc, char **argv) {
    if (!strcmp(argv[1], "init")) return write_initial_manifest();
    if (expose_sxn_to_scripts(argv[0])) {
        fputs("sxn: unable to expose the current executable to package scripts\n", stderr);
        return 2;
    }
    if (!npm_available()) {
        fputs("sxn: this bootstrap package backend requires npm on PATH; the native registry backend is not yet enabled\n", stderr);
        return 2;
    }
    char command[8192] = SXN_NPM " ";
    const char *verb = argv[1];
    if (!strcmp(verb, "run")) strcat(command, "run ");
    else if (!strcmp(verb, "install")) strcat(command, "install --ignore-scripts ");
    else if (!strcmp(verb, "add")) strcat(command, "install --ignore-scripts ");
    else if (!strcmp(verb, "remove")) strcat(command, "uninstall --ignore-scripts ");
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--dev") && !strcmp(verb, "add")) strcat(command, "--save-dev ");
        else if (append_arg(command, sizeof(command), argv[i])) {
            fputs("sxn: unsafe or overlong package argument\n", stderr); return 2;
        }
    }
    int status = system(command);
    if (status == 0 && (!strcmp(verb, "install") || !strcmp(verb, "add") || !strcmp(verb, "remove"))) {
        FILE *lock = fopen("sxn.lock", "wb");
        if (lock) {
            const char *text = "{\n  \"lockfileVersion\": 1,\n  \"source\": \"package-lock.json\",\n  \"lifecycleScripts\": \"trusted-only\"\n}\n";
            fwrite(text, 1, strlen(text), lock); fclose(lock);
        }
    }
    return status == 0 ? 0 : 1;
}
