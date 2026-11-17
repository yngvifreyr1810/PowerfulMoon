#pragma once
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

static inline FILE *rt_fopen_utf8(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wp[1600], wm[8];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wp, 1600) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, wm, 8)) return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}
