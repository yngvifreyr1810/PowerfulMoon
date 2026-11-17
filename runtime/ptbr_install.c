#define _GNU_SOURCE
#include "ptbr_install.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#endif

#include "download.h"
#include "localization.h"
#include "nxcrypto.h"
#include "paths.h"
#include "platform.h"
#include "utf8file.h"

#define PACKAGE_SIZE 773795u
#define PACKAGE_SHA "c282c43894796cb629f8668b2aec3fee50ddad88fcb7a92a5fc63f570c4409d3"
#define PACKAGE_URL "https://drive.usercontent.google.com/download?id=1EBq0PjkNvrnDSqRN7Ku_PyWPBBYu-_HG&export=download"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static PtbrInstallStatus status;
static bool busy;
static atomic_bool cancelled;
static char request_root[1024], request_archive[1200];

static void state(int value, float progress, const char *error) {
    pthread_mutex_lock(&lock);
    status.state = value;
    status.progress = progress;
    snprintf(status.error, sizeof status.error, "%s", error ? error : "");
    pthread_mutex_unlock(&lock);
}

void ptbr_install_status(PtbrInstallStatus *out) {
    pthread_mutex_lock(&lock);
    *out = status;
    pthread_mutex_unlock(&lock);
}

bool ptbr_install_busy(void) {
    pthread_mutex_lock(&lock);
    bool value = busy;
    pthread_mutex_unlock(&lock);
    return value;
}

void ptbr_install_cancel(void) { atomic_store(&cancelled, true); }

static bool progress(uint64_t got, uint64_t total, void *ctx) {
    (void)ctx;
    state(PTBR_INSTALL_DOWNLOAD, total ? (float)((double)got / total) * 0.65f : 0, NULL);
    return !atomic_load(&cancelled);
}

// Caminhos novos ficam abaixo de mods/; links e destinos existentes nunca sao substituidos.
static bool normal_dir(const char *path) {
#ifdef _WIN32
    wchar_t w[1600];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, 1600)) return false;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat s;
    return !lstat(path, &s) && S_ISDIR(s.st_mode);
#endif
}

static bool exists(const char *path) {
#ifdef _WIN32
    wchar_t w[1600];
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, 1600) &&
           GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat s;
    return !lstat(path, &s);
#endif
}

static bool make_dir(const char *path) {
#ifdef _WIN32
    wchar_t w[1600];
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, 1600) && CreateDirectoryW(w, NULL);
#else
    return !mkdir(path, 0700);
#endif
}

static bool publish(const char *from, const char *to) {
#ifdef _WIN32
    wchar_t a[1600], b[1600];
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, from, -1, a, 1600) &&
           MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, to, -1, b, 1600) && MoveFileW(a, b);
#elif defined(__linux__)
    return !syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, 1 /* RENAME_NOREPLACE */);
#else
    (void)from; (void)to;
    return false;
#endif
}

static bool archive_matches(const char *path) {
    FILE *f = rt_fopen_utf8(path, "rb");
    if (!f) return false;
    uint8_t *data = malloc(PACKAGE_SIZE);
    bool ok = data && fread(data, 1, PACKAGE_SIZE, f) == PACKAGE_SIZE && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (ok) {
        uint8_t hash[32];
        char hex[65];
        nx_sha256(data, PACKAGE_SIZE, hash);
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
        ok = !strcmp(hex, PACKAGE_SHA);
    }
    free(data);
    return ok;
}

static bool copy_archive(const char *from, const char *to) {
    FILE *in = rt_fopen_utf8(from, "rb"), *out = in ? rt_fopen_utf8(to, "wb") : NULL;
    bool ok = in && out;
    uint64_t total = 0;
    char data[16384];
    size_t n;
    while (ok && (n = fread(data, 1, sizeof data, in))) {
        total += n;
        ok = total <= PACKAGE_SIZE && !atomic_load(&cancelled) && fwrite(data, 1, n, out) == n;
    }
    if (in) { ok = ok && !ferror(in); fclose(in); }
    if (out && fclose(out)) ok = false;
    return ok;
}

static bool unpack(const char *archive, const char *romfs, const char *log_path) {
    char tar[1200] = "";
#ifdef _WIN32
    UINT n = GetSystemDirectoryA(tar, sizeof tar - 10);
    if (!n || n >= sizeof tar - 10) return false;
    strcat(tar, "\\tar.exe");
#elif defined(__linux__)
    if (!rt_resource("bsdtar", NULL, tar, sizeof tar)) {
        if (!access("/usr/bin/bsdtar", X_OK)) snprintf(tar, sizeof tar, "/usr/bin/bsdtar");
        else return false;
    }
#else
    return false;
#endif
    // O hash do RAR completo foi conferido antes de executar o extrator. Nenhum caminho vem do usuario.
    const char *args[] = {tar, "-xf", archive, "-C", romfs, "--strip-components", "5", NULL};
    PlatProc p = plat_spawn(args, log_path, false);
    if (!p) return false;
    for (int elapsed = 0; elapsed < 600; elapsed++) {
        int code = plat_proc_wait(p, false);
        if (code >= 0) return code == 0;
        if (atomic_load(&cancelled)) { plat_proc_kill(p); return false; }
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50000);
#endif
    }
    plat_proc_kill(p);
    return false;
}

static void *worker(void *unused) {
    (void)unused;
    char mods[1200], stage[1300] = "", dest[1300], archive[1400], content[1400], romfs[1500], log_path[1400];
    char error[256] = "Couldn't prepare a private installation folder. Check the game-data folder permissions.";
    bool ok = false, own_stage = false;
    snprintf(mods, sizeof mods, "%s/mods", request_root);
    snprintf(dest, sizeof dest, "%s/JumpManClub Brasil", mods);
    if (!normal_dir(request_root)) goto done;
    if (!exists(mods) && !make_dir(mods)) goto done;
    if (!normal_dir(mods)) goto done;
    if (exists(dest)) {
        snprintf(error, sizeof error, "The JumpManClub Brasil folder already exists. Enable or check that mod; no files were overwritten.");
        goto done;
    }
    for (int i = 0; i < 8 && !own_stage; i++) {
        snprintf(stage, sizeof stage, "%s/.ptbr-install-%08x", mods, arc4random());
        own_stage = make_dir(stage);
    }
    if (!own_stage) goto done;
    snprintf(archive, sizeof archive, "%s/package.rar", stage);
    snprintf(content, sizeof content, "%s/content", stage);
    snprintf(romfs, sizeof romfs, "%s/romfs", content);
    snprintf(log_path, sizeof log_path, "%s/extract.log", stage);
    if (atomic_load(&cancelled)) goto done;
    if (request_archive[0]) {
        if (!copy_archive(request_archive, archive)) { snprintf(error, sizeof error, "Couldn't read the complete official package."); goto done; }
    } else if (!download_https_file(PACKAGE_URL, archive, PACKAGE_SIZE, progress, NULL, error, sizeof error)) goto done;
    if (atomic_load(&cancelled)) goto done;
    state(PTBR_INSTALL_VERIFY, 0.7f, NULL);
    if (!archive_matches(archive)) {
        snprintf(error, sizeof error, "The package checksum doesn't match the official v1.1 download. Nothing was installed; visit the translation page.");
        goto done;
    }
    if (!make_dir(content) || !make_dir(romfs)) goto done;
    state(PTBR_INSTALL_EXTRACT, 0.8f, NULL);
    if (!unpack(archive, romfs, log_path)) {
        snprintf(error, sizeof error, "Couldn't extract the translation. Windows needs its system tar; Linux needs the bundled bsdtar (or libarchive-tools).");
        goto done;
    }
    for (int i = 0; i < PTBR_FILES; i++) {
        char file[1700];
        snprintf(file, sizeof file, "%s/%s", romfs, localization_ptbr_path(i));
        if (!localization_ptbr_matches(i, file)) {
            snprintf(error, sizeof error, "The extracted translation is incomplete or modified. Nothing was installed.");
            goto done;
        }
    }
    if (atomic_load(&cancelled)) goto done;
    state(PTBR_INSTALL_PUBLISH, 0.95f, NULL);
    if (!publish(content, dest)) {
        snprintf(error, sizeof error, "Couldn't install the mod, or its destination appeared during installation. Existing files were preserved.");
        goto done;
    }
    ok = true;
done:
    if (own_stage) plat_remove_tree(stage);
    state(ok ? PTBR_INSTALL_READY : atomic_load(&cancelled) ? PTBR_INSTALL_CANCELLED : PTBR_INSTALL_FAILED,
          ok ? 1 : 0, ok || atomic_load(&cancelled) ? NULL : error);
    pthread_mutex_lock(&lock);
    busy = false;
    pthread_mutex_unlock(&lock);
    return NULL;
}

static bool start(const char *root, const char *archive) {
    pthread_mutex_lock(&lock);
    if (busy) { pthread_mutex_unlock(&lock); return false; }
    if (!root || strlen(root) >= sizeof request_root || (archive && strlen(archive) >= sizeof request_archive)) {
        status.state = PTBR_INSTALL_FAILED;
        snprintf(status.error, sizeof status.error, "The game-data path is too long.");
        pthread_mutex_unlock(&lock);
        return false;
    }
    snprintf(request_root, sizeof request_root, "%s", root);
    snprintf(request_archive, sizeof request_archive, "%s", archive ? archive : "");
    atomic_store(&cancelled, false);
    status = (PtbrInstallStatus){.state = PTBR_INSTALL_DOWNLOAD};
    busy = true;
    pthread_mutex_unlock(&lock);
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL)) {
        state(PTBR_INSTALL_FAILED, 0, "Couldn't start the installer. Try again.");
        pthread_mutex_lock(&lock);
        busy = false;
        pthread_mutex_unlock(&lock);
        return false;
    }
    pthread_detach(thread);
    return true;
}

bool ptbr_install_start(const char *data_root) { return start(data_root, NULL); }
bool ptbr_install_local(const char *data_root, const char *archive) { return start(data_root, archive); }
