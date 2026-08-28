#include "sxfe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

