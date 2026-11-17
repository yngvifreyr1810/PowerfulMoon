#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif
#include "ptbr_install.h"

int main(int argc, char **argv) {
#ifdef _WIN32
    int count;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    char args[4][1600];
    char *utf8[4];
    if (!wide || count != 4) return 2;
    for (int i = 0; i < 4; i++) {
        if (!WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, args[i], sizeof args[i], NULL, NULL)) return 2;
        utf8[i] = args[i];
    }
    LocalFree(wide);
    argv = utf8;
#endif
    if (argc != 4) return 2;
    bool started = !strcmp(argv[1], "download") ? ptbr_install_start(argv[2]) : ptbr_install_local(argv[2], argv[3]);
    if (!strcmp(argv[1], "cancel")) ptbr_install_cancel();
    for (int i = 0; i < 4500 && ptbr_install_busy(); i++) {
#ifdef _WIN32
        Sleep(20);
#else
        usleep(20000);
#endif
    }
    if (ptbr_install_busy()) { ptbr_install_cancel(); return 3; }
    PtbrInstallStatus s;
    ptbr_install_status(&s);
    printf("{\"started\":%d,\"state\":%d,\"progress\":%.2f,\"error\":\"%s\"}\n", started, s.state, s.progress, s.error);
    return 0;
}
