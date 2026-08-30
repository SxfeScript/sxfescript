/* MinGW's runtime ships no dlfcn.h. This covers the three calls ffi.c and
   napi.c actually make (dlopen, dlsym, dlerror) over LoadLibrary/
   GetProcAddress/FormatMessage; dlclose is never called by either, so it's
   not provided. Flags are POSIX-only concepts with no Windows equivalent,
   so they're accepted and ignored. */
#ifndef SXN_DLFCN_WIN32_H
#define SXN_DLFCN_WIN32_H

#include <windows.h>
#include <stdio.h>

#define RTLD_LAZY 0
#define RTLD_NOW 0
#define RTLD_LOCAL 0

static void *dlopen(const char *path, int flags) {
    (void)flags;
    return path ? (void *)LoadLibraryA(path) : (void *)GetModuleHandleA(NULL);
}

static void *dlsym(void *handle, const char *sym) {
    return (void *)GetProcAddress((HMODULE)handle, sym);
}

static const char *dlerror(void) {
    static char buf[256];
    DWORD err = GetLastError();
    if (!err) return NULL;
    SetLastError(0);
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                              NULL, err, 0, buf, sizeof(buf), NULL);
    if (n) { while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0'; }
    else snprintf(buf, sizeof(buf), "error %lu", (unsigned long)err);
    return buf;
}

#endif
