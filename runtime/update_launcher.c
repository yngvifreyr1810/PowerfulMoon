// MIT. Network/download work belongs to the separate updater, never the render thread.
#include "update_launcher.h"
#include "release_info.h"
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
void pm_open_updater(bool automatic) {
    static HANDLE process;
    if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) return;
    if (process) { CloseHandle(process); process = NULL; }
    wchar_t dir[32768], exe[32768], command[32768];
    DWORD n = GetModuleFileNameW(NULL, dir, 32768);
    if (!n || n >= 32768) return;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    if (swprintf(exe, 32768, L"%ls\\PowerfulMoon-Update.exe", dir) < 0) return;
    wchar_t version[80];
    if (!MultiByteToWideChar(CP_UTF8, 0, POWERFULMOON_VERSION, -1, version, 80)) return;
    if (swprintf(command, 32768, L"\"%ls\" --target \"%ls\" --current %ls %ls", exe, dir, version, automatic ? L"--automatic" : L"") < 0) return;
    STARTUPINFOW startup = {.cb = sizeof startup};
    PROCESS_INFORMATION child = {0};
    if (CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &startup, &child)) {
        CloseHandle(child.hThread);
        process = child.hProcess;
    } else if (!automatic) {
        MessageBoxW(NULL, L"The updater could not be opened. Reinstall the complete PowerfulMoon package or visit its GitHub Releases page.", L"PowerfulMoon Update", MB_OK | MB_ICONINFORMATION);
    }
}
#else
void pm_open_updater(bool automatic) { (void)automatic; }
#endif
