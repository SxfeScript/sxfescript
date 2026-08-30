/* MinGW's runtime ships no dlfcn.h. This covers the three calls ffi.c and
   napi.c actually make (dlopen, dlsym, dlerror) over LoadLibrary/
   GetProcAddress/FormatMessage; dlclose is never called by either, so it's
   not provided. Flags are POSIX-only concepts with no Windows equivalent,
   so they're accepted and ignored. */
#ifndef SXN_DLFCN_WIN32_H
#define SXN_DLFCN_WIN32_H

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

#define RTLD_LAZY 0
#define RTLD_NOW 0
#define RTLD_LOCAL 0

static void *dlopen(const char *path, int flags) {
    (void)flags;
    return path ? (void *)LoadLibraryA(path) : (void *)GetModuleHandleA(NULL);
}

static void *dlsym(void *handle, const char *sym) {
    void *fn = (void *)GetProcAddress((HMODULE)handle, sym);
    if (fn || handle != (void *)GetModuleHandleA(NULL)) return fn;
    /* dlopen(NULL) on POSIX reaches every symbol already loaded into the
       process, not just the main executable's own exports - the main
       module's exports are all GetProcAddress(GetModuleHandle(NULL), ...)
       covers, so a plain lookup above misses libc functions like strlen
       that sxn.exe only imports rather than exports. Walk every loaded
       module (msvcrt.dll among them) to match the POSIX behavior. */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            fn = (void *)GetProcAddress(me.hModule, sym);
        } while (!fn && Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return fn;
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
