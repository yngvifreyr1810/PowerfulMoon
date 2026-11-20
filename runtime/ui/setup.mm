// The installer (setup.h): shown at the first start, drawn by the menu (menu.mm calls setup_draw in setup mode, with
// its ImGui context, fonts and input) over the pixel-art backdrop (backdrop.mm). The player picks:
//   - the game: their own Super Mario Odyssey 1.0.0, as an NSP or XCI file (read in place with their prod.keys:
//     runtime/nxpkg.c) or an extracted folder (exefs + romfs);
//   - prod.keys (for an NSP / XCI), dumped from their console - read on this PC only, never copied anywhere;
//   - the install folder (default: this program's folder; another one gets a copy of the program first).
// A thread then does the work: the code modules into the data root's extracted/exefs, the RomFS image
// (extracted/romfs.bin, the only copy of the game data, plus the videos as files), the game code recompiled on this
// PC (runtime/gamebuild.c), and every shader compiled ahead (nvn/vulkan/device.c vk_prepare_install, with the
// release's pipeline seed): later starts go straight to the game. An app update with new game code rebuilds only that.
#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

#include "imgui.h"
#include "imgui_internal.h"  // (SetFocusID, NavCursorVisible: the start page's cursor)
#include "theme.h"
#include "i18n.h"
#include "backdrop.h"

#ifdef __APPLE__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif
#include "menu.h"
#include "setup.h"
#include "steam_shortcut.h"

#ifndef __APPLE__
#endif
extern "C" {
#include "../diagnostics.h"
#include "../gamebuild.h"
#include "../gamelib.h"
#include "../nxpkg.h"
#include "../paths.h"
#include "../platform.h"
#include "../romfs_build.h"
#include "../rt.h"
#include "../settings.h"
#include "../version.h"
#ifdef __APPLE__
void *ui_metal_layer(void);
void *ui_metal_device(void);
#else
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
int vk_prepare_install(void (*progress)(int phase, int done, int total), volatile int *cancel);
void vk_shader_cache_reset(void);
#endif
void ui_window_init(void);
SDL_Window *ui_window(void);
}

// ------------------------------------------------------------------ files
static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
static bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static std::string ext_of(const std::string &p) {
    size_t d = p.find_last_of('.'), s = p.find_last_of("/\\");
    return d == std::string::npos || (s != std::string::npos && d < s) ? "" : lower(p.substr(d));
}
static std::string dir_of(const std::string &p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? "." : p.substr(0, s);
}

static bool data_there(const std::string &root) {  // the RomFS image, or (a source checkout) the folder main.c makes it from
    return file_exists(root + "/romfs.bin") || is_dir(root + "/romfs/StageData");
}

bool setup_ready(const char *root) {
    std::string r = root;
    return loader_peek_game((r + "/exefs/main").c_str()) == GAME_ODYSSEY && data_there(r) && gamelib_present(root);
}

// exefs: the folder with main and main.npdm; romfs: the folder with StageData and SystemData (the game's data root).
// Found in the picked folder or a few levels below it (dumping tools nest them differently).
static int g_scan_errno;  // the first folder that couldn't be read while looking
static void find_game(const std::string &dir, int depth, std::string &exefs, std::string &romfs, std::string &package) {
    if (depth > 4 || (!exefs.empty() && !romfs.empty())) return;
    if (exefs.empty() && file_exists(dir + "/main") && file_exists(dir + "/main.npdm")) exefs = dir;
    if (romfs.empty() && is_dir(dir + "/StageData") && is_dir(dir + "/SystemData")) romfs = dir;
    DIR *d = opendir(dir.c_str());
    if (!d) {
        if (!g_scan_errno) g_scan_errno = errno ? errno : EACCES;
        LOG("[setup] can't read %s: %s\n", dir.c_str(), strerror(errno));
        return;
    }
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n[0] == '.') continue;
        std::string ext = ext_of(n);
        if (package.empty() && (ext == ".xci" || ext == ".nsp")) package = dir + "/" + n;
        if (n != "StageData" && n != "ObjectData" && is_dir(dir + "/" + n)) sub.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(sub.begin(), sub.end());
    for (auto &s : sub) find_game(s, depth + 1, exefs, romfs, package);
}

static double free_gb(const char *dir) {  // < 0: unknown
#ifdef _WIN32
    wchar_t w[1100];
    ULARGE_INTEGER avail;
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, 1100);
    return GetDiskFreeSpaceExW(w, &avail, NULL, NULL) ? (double)avail.QuadPart / 1e9 : -1;
#else
    struct statvfs s;
    return statvfs(dir, &s) == 0 ? (double)s.f_bavail * (double)s.f_frsize / 1e9 : -1;
#endif
}

static std::string norm(std::string s) {  // (comparing folders: one separator, one case on Windows)
    for (auto &c : s) {
        if (c == '\\') c = '/';
#ifdef _WIN32
        c = (char)tolower((unsigned char)c);
#endif
    }
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// prod.keys near a game file: next to it, in a folder beside it (dumps often keep the keys in their own folder), or
// in the user's .switch folder (where the dumping tools put them).
static std::string find_keys(const std::string &package) {
    std::string d = dir_of(package);
    if (file_exists(d + "/prod.keys")) return d + "/prod.keys";
    if (DIR *dd = opendir(d.c_str())) {
        std::string found;
        while (struct dirent *e = readdir(dd)) {
            std::string n = e->d_name;
            if (n[0] == '.' || !is_dir(d + "/" + n)) continue;
            if (file_exists(d + "/" + n + "/prod.keys")) { found = d + "/" + n + "/prod.keys"; break; }
        }
        closedir(dd);
        if (!found.empty()) return found;
    }
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    if (home && file_exists(std::string(home) + "/.switch/prod.keys")) return std::string(home) + "/.switch/prod.keys";
    return "";
}

// ------------------------------------------------------------------ the choices (main thread; dialogs answer on any)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_pick_game;      // an NSP / XCI file or a folder
static std::string g_keys;      // prod.keys
static std::string g_update;    // the update's NSP ("" = none: the base game as it is)
static std::string g_dest;      // the install folder ("" = here)
static std::string g_here;      // this program's data root
static std::string g_notice;    // a problem with a choice, shown under it
static int g_notice_row = -1;

enum { ROW_GAME, ROW_UPDATE, ROW_KEYS, ROW_DEST };
static bool game_is_package(const std::string &g) { std::string e = ext_of(g); return e == ".nsp" || e == ".xci"; }

static void choose_game(const std::string &p) {
    pthread_mutex_lock(&g_lock);
    g_pick_game = p;
    g_notice.clear(), g_notice_row = -1;
    std::string e = ext_of(p);
    if (e == ".nsz" || e == ".xcz") {
        g_notice = "Compressed NSZ / XCZ files can't be read: decompress it to an NSP / XCI first.", g_notice_row = ROW_GAME;
        g_pick_game.clear();
    } else if (game_is_package(p) && g_keys.empty()) {
        g_keys = find_keys(p);
    }
    pthread_mutex_unlock(&g_lock);
    LOG("[setup] game: %s\n", p.c_str());
}

static void SDLCALL picked_game(void *, const char *const *files, int) {
    if (files && files[0]) choose_game(files[0]);
}
static void SDLCALL picked_keys(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_keys = files[0];
    if (g_notice_row == ROW_KEYS) g_notice.clear(), g_notice_row = -1;
    pthread_mutex_unlock(&g_lock);
}
static void SDLCALL picked_update(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_update = files[0];
    if (g_notice_row == ROW_UPDATE) g_notice.clear(), g_notice_row = -1;
    if (g_keys.empty()) g_keys = find_keys(g_update);
    pthread_mutex_unlock(&g_lock);
}
// The update row (1.4.1 runs: runtime/loader.c KNOWN_GAMES)
static bool update_supported(void) { return true; }
static void SDLCALL picked_dest(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_dest = files[0];
    if (g_notice_row == ROW_DEST) g_notice.clear(), g_notice_row = -1;
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ the work (its own thread)
enum { ST_IDLE, ST_BUSY, ST_FAILED, ST_DONE };
static std::atomic<int> g_state{ST_IDLE};
static std::string g_root;                  // where the game files go (the data root's extracted/)
static std::string g_step, g_fail_head, g_fail_hint;
static std::atomic<float> g_frac{0};
static uint64_t g_t0;                       // when the work started (time left)
static bool g_code_only;                    // the files are there: only the game code (a new version of the app)
static bool g_prepared_here;                // a shader preparation finished in this process (restart_after_prep)
static volatile int g_cancel;
static std::string g_job_game, g_job_keys, g_job_update;  // the choices the work runs with

static void set_step(const char *s) {
    pthread_mutex_lock(&g_lock);
    g_step = s;
    pthread_mutex_unlock(&g_lock);
    LOG("[setup] %s\n", s);
}

static void fail(const char *head, const std::string &hint) {
    pthread_mutex_lock(&g_lock);
    g_fail_head = head;
    g_fail_hint = hint;
    pthread_mutex_unlock(&g_lock);
    LOG("[setup] failed: %s - %s\n", head, hint.c_str());
    g_state = ST_FAILED;
}

// the bar: game files 0 - 0.25, game code 0.25 - 0.55, shaders 0.55 - 1
static void data_progress(uint64_t done, uint64_t total) {
    if (total) g_frac = 0.25f * (float)((double)done / (double)total);
}
static void code_progress(double f, const char *step) {
    g_frac = (g_code_only ? 0 : 0.25f) + (g_code_only ? 1.0f : 0.30f) * (float)f;
    static const char *last;
    if (step != last) last = step, set_step(step);
}
static void shader_progress(int phase, int done, int total) {
    float f = total ? (float)done / (float)total : 0;
    g_frac = phase == 2 ? 0.85f + 0.15f * f : 0.55f + 0.30f * f;
    char s[96];
    snprintf(s, sizeof s, phase == 2 ? "Compiling pipelines  %d / %d" : "Compiling shaders  %d / %d", done, total);
    pthread_mutex_lock(&g_lock);
    g_step = s;
    pthread_mutex_unlock(&g_lock);
}

static bool copy_file_to(const std::string &src, const std::string &dst) {
    FILE *in = fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE *out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    std::vector<char> buf(1 << 20);
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf.data(), 1, buf.size(), in)) > 0) ok = fwrite(buf.data(), 1, n, out) == n;
    fclose(in);
    return fclose(out) == 0 && ok;
}

// The game files from an NSP / XCI: code modules, the RomFS image, the videos out of it as files (the movie player
// opens them as files: hle/movie.c). With an update (upd: its NSP): the update's code modules, and the update's RomFS
// (its BKTR patch over the base game's: nxpkg_extract_romfs_patched).
static bool files_from_package(const std::string &root, const std::string &pkg, const std::string &keys, const std::string &upd) {
    set_step("Opening your game file...");
    char why[512] = "";
    NxPkg *p = nxpkg_open(pkg.c_str(), keys.c_str(), why, sizeof why), *u = NULL;
    if (!p) { fail("That game file can't be used", why); return false; }
    if (!upd.empty()) {
        set_step("Opening the update...");
        u = nxpkg_open_update(upd.c_str(), keys.c_str(), why, sizeof why);
        if (!u) { nxpkg_close(p); fail("That update can't be used", why); return false; }
    }
    double need = (double)nxpkg_romfs_size(u ? u : p) / 1e9 + 6, have = free_gb(rt_data_root());
    if (have >= 0 && have < need) {
        nxpkg_close(p), nxpkg_close(u);
        char h[300];
        snprintf(h, sizeof h, "Installing needs about %.0f GB free on this drive (the game data, the game code and the "
                              "compiled shaders). Free some space or choose another install folder.", ceil(need));
        fail("Not enough free space", h);
        return false;
    }
    rt_remove_game_files(root.c_str());  // (an earlier or interrupted setup's; removes the folder too)
    mkdir(root.c_str(), 0755);
    set_step("Reading the game code...");
    if (!nxpkg_extract_exefs(u ? u : p, (root + "/exefs").c_str(), why, sizeof why)) {
        nxpkg_close(p), nxpkg_close(u);
        fail(u ? "Reading the update failed" : "Reading the game file failed", why);
        return false;
    }
    if (loader_peek_game((root + "/exefs/main").c_str()) != GAME_ODYSSEY) {
        nxpkg_close(p), nxpkg_close(u);
        fail("Wrong version of the game", u ? "That update isn't version 1.4.1 of Super Mario Odyssey."
                                            : "Super Odyssey PC Recomp runs version 1.0.0 of Super Mario Odyssey (the base game), "
                                              "optionally with the 1.4.1 update.");
        return false;
    }
    set_step(u ? "Writing the game data with the update (checking every block)..." : "Writing the game data (checking every block)...");
    bool ok = u ? nxpkg_extract_romfs_patched(p, u, (root + "/romfs.bin").c_str(), data_progress, why, sizeof why)
                : nxpkg_extract_romfs(p, (root + "/romfs.bin").c_str(), data_progress, why, sizeof why);
    nxpkg_close(p), nxpkg_close(u);
    if (!ok) { fail("Reading the game file failed", why); return false; }
    set_step("Copying the videos...");
    mkdir((root + "/romfs").c_str(), 0755);
    if (!romfs_image_extract_dir((root + "/romfs.bin").c_str(), "MovieData", (root + "/romfs/MovieData").c_str(), why, sizeof why)) {
        fail("Setup didn't finish", why);
        return false;
    }
    g_frac = 0.25f;
    return true;
}

// The game files from an extracted folder (exefs + romfs), as before.
static bool files_from_folder(const std::string &root, const std::string &src) {
    set_step("Looking for the game files...");
    std::string exefs, romfs, package;
    g_scan_errno = 0;
    find_game(src, 0, exefs, romfs, package);
    LOG("[setup] exefs: %s, romfs: %s\n", exefs.empty() ? "(none)" : exefs.c_str(), romfs.empty() ? "(none)" : romfs.c_str());
    if (exefs.empty() || romfs.empty()) {
        if (!package.empty()) fail("That folder holds a game file", "Choose the NSP or XCI file itself (NSP / XCI file), with your prod.keys.");
        else if (g_scan_errno == EPERM || g_scan_errno == EACCES) fail("No access to that folder", "Copy the game files to a folder you own and choose that.");
        else fail("No game files found there", "Choose your NSP or XCI file, or the folder with the extracted game (exefs and romfs).");
        return false;
    }
    if (loader_peek_game((exefs + "/main").c_str()) != GAME_ODYSSEY) {
        fail("Wrong version of the game", "Super Odyssey PC Recomp runs version 1.0.0 of Super Mario Odyssey, or 1.4.1 "
                                          "(the extracted folder with the update already applied).");
        return false;
    }
    if (!file_exists(exefs + "/sdk")) { fail("Game files incomplete", "exefs has no sdk. Extract the game files again."); return false; }
    std::string mine = norm(std::string(rt_data_root()) + "/" + root);
    if (!norm(exefs).compare(0, mine.size(), mine) || !norm(romfs).compare(0, mine.size(), mine)) {
        fail("No game files found there", "Choose the folder with your own extracted game, not this program's copy.");
        return false;
    }
    uint64_t rbytes = romfs_dir_bytes(romfs.c_str());
    double need = (double)rbytes / 1e9 + 6, have = free_gb(rt_data_root());
    if (rbytes < 4500000000ull) {
        char h[300];
        snprintf(h, sizeof h, "Its romfs holds %.1f GB, the base game's holds 5.5 GB: this is probably an update's romfs. "
                              "Choose the base game.", rbytes / 1e9);
        fail("Game files incomplete", h);
        return false;
    }
    if (have >= 0 && have < need) {
        char h[300];
        snprintf(h, sizeof h, "Installing needs about %.0f GB free on this drive. Free some space or choose another "
                              "install folder.", ceil(need));
        fail("Not enough free space", h);
        return false;
    }
    rt_remove_game_files(root.c_str());
    mkdir(root.c_str(), 0755);
    set_step("Copying the game code...");
    if (!romfs_copy_tree(exefs.c_str(), (root + "/exefs").c_str(), NULL)) { fail("Setup didn't finish", "Copying the game code failed."); return false; }
    mkdir((root + "/romfs").c_str(), 0755);
    if (is_dir(romfs + "/MovieData") && !romfs_copy_tree((romfs + "/MovieData").c_str(), (root + "/romfs/MovieData").c_str(), NULL)) {
        fail("Setup didn't finish", "Copying the videos failed.");
        return false;
    }
    set_step("Writing the game data...");
    char why[256] = "";
    if (!romfs_build(romfs.c_str(), (root + "/romfs.bin").c_str(), data_progress, why, sizeof why)) {
        fail("Setup didn't finish", std::string("Writing the game data failed: ") + why);
        return false;
    }
    g_frac = 0.25f;
    return true;
}

static void *work(void *) {
    std::string root = g_root;
    g_frac = 0;
    g_t0 = plat_now_ns();
    g_cancel = 0;
    if (!gamebuild_available()) {
        fail("The game code builder is missing", "Download Super Odyssey PC Recomp again and keep everything in its folder "
                                                 "together (the zig and odysseygen folders).");
        return NULL;
    }
    if (!g_code_only) {
        bool ok = game_is_package(g_job_game) ? files_from_package(root, g_job_game, g_job_keys, g_job_update)
                                              : files_from_folder(root, g_job_game);
        if (!ok) return NULL;
    }
    set_step("Recompiling the game code...");
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    GameBuildOpts o = {code_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1};
    char why[256] = "";
    if (!gamebuild_run(root.c_str(), &o, why, sizeof why)) {
        LOG("[setup] game code: %s\n", why);
        fail("The game code didn't build", "Make sure there's about 2 GB free and try again. If it happens again, report it "
                                           "with SuperOdysseyRecomp.log.");
        return NULL;
    }
    if (!setup_ready(root.c_str())) { fail("Game files incomplete", "Choose your game again."); return NULL; }
#ifndef __APPLE__
    if (!g_code_only) {
        // (the release's pipeline seed is unpacked by vk_prepare_install: nvn/vulkan/device.c prep_seed)
        set_step("Compiling shaders...");
        g_frac = 0.55f;
        int code = vk_prepare_install(shader_progress, &g_cancel);
        if (code == -1) return NULL;  // (the window was closed)
        g_prepared_here = code == 0;
        // not fatal: the game prepares what's missing at its next start
        if (code != 0) LOG("[setup] shader preparation ended with %d; the game finishes it at its start\n", code);
    }
#endif
    LOG("[setup] done in %.0fs\n", (double)(plat_now_ns() - g_t0) / 1e9);
    g_frac = 1;
    g_state = ST_DONE;
    return NULL;
}

// A shader preparation finished in this process (the setup's last step or the start page's): the game starts in a new
// one, without the start page. The driver reads its own shader cache when the GPU is opened - here before the
// preparation filled it - so ~24,800 programs found only there (exec.c pc_worker) were compiled again while playing:
// ~1,800-2,000 libraries in the owner's new-user tests (2026-11-19), against 5 over 3 ms from a new process.
// Windows: the new process is waited for (plat_exec), so a launcher waiting on this one still works.
// ODYSSEY_PREP_NO_RESTART=1: played on here, the prepared pipeline cache file taken in.
#ifndef __APPLE__
extern "C" void vk_prepare_take_cache(void);
#endif
static void restart_after_prep(void) {
#ifndef __APPLE__
    if (!g_prepared_here) return;
    g_prepared_here = false;
    if (!getenv("ODYSSEY_PREP_NO_RESTART")) {
        LOG("[setup] shaders prepared: the game starts again in a new process, straight into the game\n");
        if (SDL_Window *w = ui_window()) SDL_HideWindow(w);
#ifdef _WIN32
        _putenv_s("ODYSSEY_NO_CONFIG", "1");
        _putenv_s("ODYSSEY_CONFIG", ""), _putenv_s("ODYSSEY_PREP_PAGE_TEST", "");  // (tests' switches: the page again)
#else
        setenv("ODYSSEY_NO_CONFIG", "1", 1);
        unsetenv("ODYSSEY_CONFIG"), unsetenv("ODYSSEY_PREP_PAGE_TEST");
#endif
        plat_exec(NULL);
        LOG("[setup] the new process didn't start: playing on in this one\n");
        if (SDL_Window *w = ui_window()) SDL_ShowWindow(w);
    }
    vk_prepare_take_cache();
#endif
}

static void start_work(void) {
    g_state = ST_BUSY;
    pthread_t th;
    if (pthread_create(&th, NULL, work, NULL) == 0) pthread_detach(th);
    else fail("Setup didn't start", "Try again.");
}

// ------------------------------------------------------------------ the page (render thread, menu.mm)
enum { DLG_NONE, DLG_GAME_FILE, DLG_GAME_FOLDER, DLG_KEYS, DLG_DEST, DLG_UPDATE };
static std::atomic<int> g_want_dialog{DLG_NONE};  // (dialogs open on the main thread)
static std::atomic<bool> g_want_install{false}, g_want_play{false};

// The primary button (0.4.1): filled red, white text; size.x > 0: that width. The controller / keyboard cursor on it:
// a white outline (red on red wouldn't show).
static bool accent_button(const char *label, float scale, bool enabled = true, ImVec2 size = ImVec2(0, 0)) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentV);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHotV);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentDeepV);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, kTextV);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextV);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(34 * scale, 14 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    bool r = ImGui::Button(tr(label), size);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ImGui::EndDisabled();
    return r && enabled;
}
// The secondary button: a charcoal surface, lighter under the mouse; the cursor on it: a red outline
static bool plain_button(const char *label, float scale, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, kSurfaceV);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSurfaceHoverV);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentDeepV);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, kAccentV);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(34 * scale, 14 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10 * scale);
    bool r = ImGui::Button(tr(label), size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return r;
}
static bool small_button(const char *label, float scale) {
    ImGui::PushStyleColor(ImGuiCol_Button, kSurfaceHoverV);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16 * scale, 7 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5 * scale);
    bool r = ImGui::Button(tr(label));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    return r;
}

// a path that fits in w: its end, with "..." in front (Windows: its own separators)
static std::string fit(std::string s, float w) {
#ifdef _WIN32
    std::replace(s.begin(), s.end(), '/', '\\');
#endif
    if (ImGui::CalcTextSize(s.c_str()).x <= w) return s;
    for (size_t cut = 1; cut < s.size(); cut++) {
        std::string t = "..." + s.substr(cut);
        if (ImGui::CalcTextSize(t.c_str()).x <= w) return t;
    }
    return "...";
}

// one choice: its label and the chosen value (or a hint) on the left, the buttons on the right
// ODYSSEY_SETUP_DOCS=1 (the README's picture, docs/installer.png): the page as the documentation describes it - the game
// as its extracted folder, then the install folder; nothing else changes
static bool setup_docs_view(void) {
    static int on = -1;
    if (on < 0) on = getenv("ODYSSEY_SETUP_DOCS") != NULL;
    return on;
}
static void row(const char *label, const std::string &value, const char *empty, float colw, float scale, int which,
                const std::string &notice, int notice_row, bool off = false) {
    // Two columns: the label and the value (or a hint) on the left, wrapped before the buttons; the buttons on the
    // right, their right edge on the column's, centred on the left column's height. A thin line between rows.
    float pad = 2 * 16 * scale, gap = ImGui::GetStyle().ItemSpacing.x, colgap = 24 * scale;
    float btnw = which == ROW_GAME && setup_docs_view() ? ImGui::CalcTextSize("Folder").x + pad
               : which == ROW_GAME ? ImGui::CalcTextSize("NSP / XCI file").x + ImGui::CalcTextSize("Folder").x + 2 * pad + gap
                                   : ImGui::CalcTextSize(which == ROW_KEYS || which == ROW_UPDATE ? "Choose..." : "Change...").x + pad;
    float btnh = ImGui::GetFontSize() + 2 * 7 * scale;
    float leftw = colw - btnw - colgap;
    ImGui::Dummy(ImVec2(0, 10 * scale));
    float top = ImGui::GetCursorPosY();
    float x0 = ImGui::GetCursorPosX();
    ImGui::PushTextWrapPos(x0 + leftw);
    ImGui::PushStyleColor(ImGuiCol_Text, off ? kTextOffV : kTextV);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, value.empty() ? kTextOffV : kTextDimV);
    ImGui::SetWindowFontScale(0.78f);
    if (value.empty()) ImGui::TextWrapped("%s", empty);
    else ImGui::TextUnformatted(fit(value, leftw).c_str());
    ImGui::SetWindowFontScale(1);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    float bottom = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y;
    if (!off) {
        float by = top + fmaxf(0, (bottom - top - btnh) * 0.5f);
        ImGui::SetCursorPos(ImVec2(x0 + colw - btnw, by));
        ImGui::PushID(which);
        if (which == ROW_GAME) {
            if (!setup_docs_view()) {
                if (small_button("NSP / XCI file", scale)) g_want_dialog = DLG_GAME_FILE;
                ImGui::SameLine();
            }
            if (small_button("Folder", scale)) g_want_dialog = DLG_GAME_FOLDER;
        } else if (small_button(which == ROW_KEYS || which == ROW_UPDATE ? "Choose..." : "Change...", scale)) {
            g_want_dialog = which == ROW_KEYS ? DLG_KEYS : which == ROW_UPDATE ? DLG_UPDATE : DLG_DEST;
        }
        ImGui::PopID();
        bottom = fmaxf(bottom, by + btnh);
    }
    ImGui::SetCursorPos(ImVec2(x0, bottom + 10 * scale));
    ImVec2 lp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(lp, ImVec2(lp.x + colw, lp.y), IM_COL32(255, 255, 255, 22), 1.0f * scale);
    ImGui::Dummy(ImVec2(0, 1 * scale));
    if (notice_row == which && !notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.82f, 0.45f, 1));
        ImGui::TextWrapped("%s", notice.c_str());
        ImGui::PopStyleColor();
    }
}


// ------------------------------------------------------------------ the configuration page (before the game boots)
// Every start shows the settings first (the menu's tabs on the card), then Play or Exit - the settings that only take
// effect at the next start (memory use, warm-up...) can be set before the game is there. "Show at start" turns it off;
// the menu in play (F1, the controller's Select + Start) keeps every setting. Test runs and --play go straight in.
extern "C" bool input_motion_only_event(const SDL_Event *e);  // runtime/hle/input.c
static std::atomic<bool> g_config{false};
static std::atomic<int> g_config_result{0};  // 1: Play, 2: Exit
bool setup_config_shown(void) { return g_config.load(); }
void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool, void (*prelaunch)(float));

// ---- the shader preparation on this page (no window of its own): asked for when Play is pressed and a full
// preparation is due (a fresh install, Compile shaders in the menu, a new translation), then run here with a progress
// bar. Later: the game starts, and the next start asks again.
#ifndef __APPLE__
extern "C" bool vk_prepare_due(void);
extern "C" void vk_prepare_skip_this_start(void);
enum { PREP_NONE, PREP_ASK, PREP_RUN, PREP_DONE };
static std::atomic<int> g_prep{PREP_NONE}, g_prep_phase{0}, g_prep_done{0}, g_prep_total{0}, g_prep_code{0};
static volatile int g_prep_cancel;
static bool g_prep_stay;  // "Compile, play later" (#101): back to the start page when it is done, not into the game
// (#101, second comment) the start page's own "Compile shaders" button: shown while a full preparation is due, so a
// player sees it before pressing Play and can get the game ready without starting it
static bool g_prep_due_home;
static uint64_t g_prep_t0, g_prep_phase_t0;
static int g_prep_last_phase = -1;
static void prep_page_progress(int phase, int done, int total) {
    g_prep_phase = phase, g_prep_done = done, g_prep_total = total;
}
static void *prep_page_work(void *) {
    int code = vk_prepare_install(prep_page_progress, &g_prep_cancel);
    g_prep_code = code;
    if (code == 0) g_prepared_here = true;
    if (code != 0) vk_prepare_skip_this_start();  // (cancelled or failed: what is done stays; the game starts)
    g_prep = PREP_DONE;
    return NULL;
}
static void prep_start(void) {
    g_prep_cancel = 0;
    g_prep_t0 = g_prep_phase_t0 = SDL_GetTicksNS();
    g_prep_last_phase = -1;
    g_prep_phase = 0, g_prep_done = 0, g_prep_total = 0;
    g_prep = PREP_RUN;
    pthread_t t;
    if (pthread_create(&t, NULL, prep_page_work, NULL) == 0) pthread_detach(t);
    else { vk_prepare_skip_this_start(); g_prep = PREP_DONE; }
}
static void prep_card(float scale, ImFont *big, ImVec2 c0, float cardw, float cardh, float pad) {
    float w = fminf(cardw - 2 * pad, 760 * scale);
    ImVec2 p0(c0.x + (cardw - w) * 0.5f, c0.y + cardh * 0.24f);
    ImGui::SetCursorScreenPos(p0);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(p0.x + w);
    ImGui::PushFont(big);
    ImGui::TextUnformatted(tr(g_prep.load() == PREP_ASK ? "Compile the shaders now?" : "Compiling the shaders"));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 8 * scale));
    if (g_prep.load() == PREP_ASK) {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
        ImGui::TextWrapped("%s", tr("The game's shaders are turned into ones your GPU runs, once, before playing. Without it they "
                           "are made the first time each one is needed, which shows as short stutters for a while. "
                           "It takes a few minutes (about 6 on a Steam Deck) and is only needed again after an update "
                           "that changes them, or when you ask for it in the menu."));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 18 * scale));
        if (accent_button("Compile now", scale)) { g_prep_stay = false; prep_start(); }
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        ImGui::SameLine(0, 16 * scale);
        if (accent_button("Compile, play later", scale)) { g_prep_stay = true; prep_start(); }
        ImGui::SameLine(0, 16 * scale);
        if (accent_button("Later", scale)) { g_prep_stay = false; vk_prepare_skip_this_start(); g_prep = PREP_DONE; }
    } else {
        int ph = g_prep_phase.load(), d = g_prep_done.load(), n = g_prep_total.load();
        uint64_t now = SDL_GetTicksNS();
        if (ph != g_prep_last_phase) g_prep_last_phase = ph, g_prep_phase_t0 = now;
        static const char *const steps[] = {"Step 1 of 3: translating the game's shaders", "Step 2 of 3: compiling them for your GPU",
                                            "Step 3 of 3: compiling the recorded pipelines"};
        ImGui::TextUnformatted(tr(steps[ph >= 0 && ph <= 2 ? ph : 1]));
        float f = n > 0 ? (float)d / (float)n : 0;
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccentV);
        char ov[64];
        snprintf(ov, sizeof ov, n > 0 ? "%d / %d" : tr("finding the game's shaders..."), d, n);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * scale, 8 * scale));
        ImGui::ProgressBar(f, ImVec2(w, 0), ov);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        double el = (now - g_prep_t0) / 1e9, pel = (now - g_prep_phase_t0) / 1e9;
        char t[128];
        if (f > 0.02f && pel > 3) {
            double left = pel / f - pel;
            snprintf(t, sizeof t, "%d:%02d so far, about %d:%02d left in this step", (int)el / 60, (int)el % 60, (int)left / 60, (int)left % 60);
        } else snprintf(t, sizeof t, "%d:%02d so far", (int)el / 60, (int)el % 60);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
        ImGui::TextUnformatted(t);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 18 * scale));
        if (accent_button(g_prep_stay ? "Stop" : "Stop and play", scale)) g_prep_cancel = 1;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
        ImGui::TextWrapped("%s", tr(g_prep_stay ? "What is compiled so far is kept; the rest is made while you play. "
                                                  "When it is done you are back on the start page."
                                                : "What is compiled so far is kept; the rest is made while you play."));
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
}
#endif

// The start page (0.4.1, owner 2026-11-17: the start option "awkward to use and to start the game"; "modern, more
// controller and mouse and keyboard friendly"). Home first: the name, a large red Play with the cursor on it, Settings
// and Exit under it, whether this page shows at start, and the settings that matter most at a glance. Settings: the
// menu's own sidebar and pages, with Play and Back at the bottom. Start (or Enter on Home) plays from either view.
bool menu_sidebar_focused(void);
void menu_focus_sidebar(void);
void menu_hint_bar(float scale, int mode);
float menu_hint_bar_height(float scale);
// The last session's crash (2026-11-19), at the next start: its reason, and the report's text copied or its file shown
// for a bug report (on Linux / the Steam Deck the crash's own box can't have buttons, so this is where they are)
static char g_crash_path[1200], g_crash_reason[256];
static bool g_crash_notice;
static int g_crash_action;  // 1 copied (see diagnostics_copy_status), 2 shown, 3 not shown
static void crash_notice(float scale, float w) {
    if (!g_crash_notice) return;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float in = 20 * scale;
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + in, p0.y + in * 0.8f));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(p0.x + w - in);
    ImGui::TextColored(kAccentV, "%s", tr("THE GAME CRASHED LAST TIME"));
    ImGui::TextWrapped("%s", g_crash_reason);
    ImGui::Dummy(ImVec2(0, 4 * scale));
    if (small_button("Copy report text", scale)) g_crash_action = diagnostics_copy_report(g_crash_path) ? 1 : -1;
    ImGui::SameLine();
    if (small_button("Show report file", scale)) g_crash_action = diagnostics_show_report(g_crash_path) ? 2 : 3;
    ImGui::SameLine();
    bool dismiss = small_button("Dismiss", scale);
    const char *a = strrchr(g_crash_path, '/'), *b = strrchr(g_crash_path, '\\');
    const char *name = a > b ? a + 1 : b ? b + 1 : g_crash_path;
    char status[1400];
    int copy = diagnostics_copy_status();
    if (g_crash_action == 1 && copy > 0) snprintf(status, sizeof status, "%s", tr("Copied: paste it into the bug report (Ctrl+V)."));
    else if (g_crash_action == 1 && copy == 0) snprintf(status, sizeof status, "%s", tr("Copying..."));
    else if (g_crash_action == 1 || g_crash_action == -1)
        snprintf(status, sizeof status, "%s %s", tr("Could not copy. Attach the file instead:"), g_crash_path);
    else if (g_crash_action == 2) snprintf(status, sizeof status, "%s %s", tr("Its folder is open: attach the file"), name);
    else if (g_crash_action == 3) snprintf(status, sizeof status, "%s %s", tr("The report file:"), g_crash_path);
    else snprintf(status, sizeof status, "%s", tr("To report it: copy the report's text and paste it into the bug report, or "
                                                 "attach the file."));
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
    ImGui::TextWrapped("%s", status);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    float y1 = ImGui::GetItemRectMax().y + in * 0.8f;
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, y1), theme_u32(kSidebarV), 14 * scale);
    dl->AddRect(p0, ImVec2(p0.x + w, y1), theme_u32(kAccentV, 0.6f), 14 * scale, 0, fmaxf(1, 1.5f * scale));
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, y1 + 14 * scale));
    ImGui::Dummy(ImVec2(w, 0));
    if (dismiss) g_crash_notice = false;
}

static bool g_cfg_settings;      // the Settings view (else Home)
static int g_cfg_focus = 1;      // put the cursor on Play (1) / Settings (2) next frame; 0: leave it
static void home_view(float scale, ImFont *big, ImVec2 c0, float cardw, float cardh, float pad) {
    float hint_h = menu_hint_bar_height(scale);
    float colw = fminf(560 * scale, cardw * 0.42f), bw = colw;
    float bh = ImGui::GetFontSize() + 2 * 14 * scale;
    // the left column: the buttons, centred in the card's height
    float stack = bh * 1.35f + 2 * (bh + 14 * scale) + 70 * scale;
#ifndef __APPLE__
    if (g_prep_due_home) stack += bh + 14 * scale + ImGui::GetFontSize() * 2.6f;
#endif
    float y0 = c0.y + fmaxf(pad * 3.2f, (cardh - stack) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(c0.x + pad * 1.6f, y0));
    ImGui::BeginGroup();
    ImGui::PushFont(big);
    if (accent_button("Play", scale, true, ImVec2(bw, bh * 1.35f))) g_config_result = 1;
    ImGui::PopFont();
    if (g_cfg_focus == 1) { ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow()); ImGui::SetNavCursorVisible(true); g_cfg_focus = 0; }
    ImGui::Dummy(ImVec2(0, 6 * scale));
#ifndef __APPLE__
    if (g_prep_due_home) {  // (compiles, then this page again: Play starts at once)
        if (plain_button("Compile shaders now", scale, ImVec2(bw, bh))) { g_prep_stay = true; prep_start(); }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
        ImGui::TextColored(kTextDimV, "%s", tr("The shaders need compiling (a new install or an update that changed them). "
                                               "Do it now and play later, or press Play and choose then."));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 6 * scale));
    }
#endif
    if (plain_button("Settings", scale, ImVec2(bw, bh))) { g_cfg_settings = true; menu_focus_sidebar(); }
    if (g_cfg_focus == 2) { ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow()); ImGui::SetNavCursorVisible(true); g_cfg_focus = 0; }
    if (plain_button("Exit", scale, ImVec2(bw, bh))) g_config_result = 2;
    ImGui::Dummy(ImVec2(0, 10 * scale));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kTextV);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kSurfaceV);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, kAccentV);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
    if (ImGui::Checkbox(tr("Show this screen at every start"), &g_settings.config_at_start)) settings_save();
    ImGui::PopStyleColor(4);
    ImGui::EndGroup();

    // the right column: the settings that matter most, at a glance (Settings changes them)
    float rx = c0.x + pad * 1.6f + colw + pad * 2.2f, rw = c0.x + cardw - pad * 1.6f - rx;
    if (rw > 260 * scale) {
        ImGui::SetCursorScreenPos(ImVec2(rx, g_crash_notice ? fminf(y0, c0.y + pad * 3.2f) : y0));
        ImGui::BeginGroup();
        crash_notice(scale, rw);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 b0 = ImGui::GetCursorScreenPos();
        float lh = ImGui::GetFontSize() * 1.75f;
        static const char *const modes[] = {"Windowed", "Borderless", "Exclusive fullscreen"};
        static const char *const aas[] = {"Off", "SMAA", "FXAA"};
        static const char *const warm[] = {"Auto", "Normal", "Long"};
        char fps[32], up[64];
        snprintf(fps, sizeof fps, "%d FPS", settings_game_hz());
        static const char *const tech[TEMPORAL_BACKEND_COUNT] = {"NVIDIA DLSS", "AMD FSR 3.1", "AMD FSR 4", "Intel XeSS", "AMD FSR 2"};
        int tb = g_settings.temporal_backend;
        snprintf(up, sizeof up, "%s", g_settings.temporal_off || tb < 0 || tb >= TEMPORAL_BACKEND_COUNT ? tr("Off") : tech[tb]);
        struct { const char *k; const char *v; } rows[] = {
            {"Display mode", tr(modes[g_settings.window_mode >= 0 && g_settings.window_mode < 3 ? g_settings.window_mode : 1])},
            {"Frame rate", fps},
            {"Upscale method", up},
            {"Anti-aliasing", tr(aas[g_settings.antialias >= 0 && g_settings.antialias < 3 ? g_settings.antialias : 0])},
            {"Warm-up at start", tr(warm[g_settings.warmup >= 0 && g_settings.warmup < 3 ? g_settings.warmup : 0])},
        };
        int n = (int)(sizeof rows / sizeof *rows);
        float boxh = 30 * scale + ImGui::GetFontSize() * 1.6f + n * lh + lh * 0.6f + 20 * scale;
        dl->AddRectFilled(b0, ImVec2(b0.x + rw, b0.y + boxh), theme_u32(kSidebarV), 14 * scale);
        dl->AddRect(b0, ImVec2(b0.x + rw, b0.y + boxh), theme_u32(kLineV), 14 * scale, 0, fmaxf(1, scale));
        ImGui::SetCursorScreenPos(ImVec2(b0.x + 24 * scale, b0.y + 20 * scale));
        ImGui::TextColored(kTextDimV, "%s", tr("YOUR SETTINGS"));
        ImGui::Dummy(ImVec2(0, 4 * scale));
        for (int i = 0; i < n; i++) {
            ImGui::SetCursorScreenPos(ImVec2(b0.x + 24 * scale, ImGui::GetCursorScreenPos().y));
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddText(p, kTextDim, tr(rows[i].k));
            ImVec2 vs = ImGui::CalcTextSize(rows[i].v);
            dl->AddText(ImVec2(b0.x + rw - 24 * scale - vs.x, p.y), kText, rows[i].v);
            ImGui::Dummy(ImVec2(rw - 48 * scale, lh));
        }
        ImGui::SetCursorScreenPos(ImVec2(b0.x, b0.y + boxh + 14 * scale));
        if (plat_is_steam_deck()) {  // (the owner's finding, 2026-11-15: at a low power limit the GPU clock decides the frame rate)
            ImGui::PushTextWrapPos(b0.x + rw);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
            ImGui::TextWrapped("%s", tr("Steam Deck tip: press the ... button, open Performance, turn on Manual GPU Clock Control and set "
                               "it to 1000 MHz. With a low TDP limit (7-10 W) this keeps the GPU fast enough for a steady 60 FPS."));
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
    }
    ImGui::SetCursorScreenPos(ImVec2(c0.x + pad, c0.y + cardh - hint_h - pad * 0.6f));
    ImGui::BeginGroup();
    ImGui::PushItemWidth(cardw - 2 * pad);
    menu_hint_bar(scale, 2);
    ImGui::PopItemWidth();
    ImGui::EndGroup();
}

static void config_page(float scale, ImFont *font, ImFont *big) {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##config", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(font);
    backdrop_draw(ImGui::GetBackgroundDrawList(), ImVec2(0, 0), io.DisplaySize, ImGui::GetTime());
    float margin = 22 * scale, pad = 28 * scale;
    float cardw = fminf(io.DisplaySize.x - 2 * margin, fmaxf(1240 * scale, io.DisplaySize.x * 0.84f)), cardh = io.DisplaySize.y - 2 * margin;
    ImVec2 c0((io.DisplaySize.x - cardw) * 0.5f, margin);
    ImDrawList *bg = ImGui::GetWindowDrawList();
    bg->AddRectFilled(c0, ImVec2(c0.x + cardw, c0.y + cardh), theme_u32(kPanelV), 18 * scale);
    bg->AddRect(c0, ImVec2(c0.x + cardw, c0.y + cardh), theme_u32(kLineV), 18 * scale, 0, fmaxf(1, scale));
    // the header: the pipe, the name and the version; in Settings, "Settings" after it
    ImGui::SetCursorPos(ImVec2(c0.x + pad, c0.y + pad * 0.8f));
    ImGui::BeginGroup();
    ImGui::PushFont(big);
    {
        float lh = ImGui::GetFontSize(), sz = lh * 1.15f;
        ImVec2 lp = ImGui::GetCursorScreenPos();
        theme_logo(ImGui::GetWindowDrawList(), ImVec2(lp.x, lp.y + lh * 0.5f - sz * 0.55f), sz, IM_COL32(59, 196, 90, 255), 0);
        ImGui::Dummy(ImVec2(sz, lh));
        ImGui::SameLine(0, lh * 0.4f);
        ImGui::TextUnformatted(g_cfg_settings ? tr("Settings") : "Super Odyssey PC Recomp");
    }
    ImGui::PopFont();
    ImGui::SameLine(0, 14 * scale);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8 * scale);
    ImGui::TextColored(kTextDimV, "%s %s", tr("Version"), APP_VERSION);
    ImGui::EndGroup();
#ifndef __APPLE__
    if (g_prep.load() == PREP_ASK || g_prep.load() == PREP_RUN) {  // (the shader preparation: this card only)
        prep_card(scale, big, c0, cardw, cardh, pad);
        ImGui::PopFont();
        ImGui::End();
        return;
    }
#endif
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (!g_cfg_settings) {
        home_view(scale, big, c0, cardw, cardh, pad);
    } else {
        float footer = ImGui::GetFontSize() + 2 * 14 * scale + menu_hint_bar_height(scale) + 30 * scale;
        float top = ImGui::GetCursorPosY() + 10 * scale;
        ImGui::SetCursorPos(ImVec2(c0.x + pad, top));
        ImGui::BeginChild("##cfgtabs", ImVec2(cardw - 2 * pad, c0.y + cardh - top - footer), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NavFlattened);
        menu_tabs_draw(scale, font, big, false, NULL);
        ImGui::EndChild();
        // the footer: Play and Back, then the hint bar
        ImGui::SetCursorPos(ImVec2(c0.x + pad, c0.y + cardh - footer + 8 * scale));
        if (accent_button("Play", scale)) g_config_result = 1;
        ImGui::SameLine(0, 14 * scale);
        if (plain_button("Back", scale)) { g_cfg_settings = false; g_cfg_focus = 2; }
        ImGui::SetCursorPosX(c0.x + pad);
        ImGui::BeginGroup();
        menu_hint_bar(scale, 1);
        ImGui::EndGroup();
        // B / Esc: in a page, back to the sidebar; on the sidebar, back to Home
        if (!any_popup && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            if (!menu_sidebar_focused() && ImGui::GetCurrentContext()->NavCursorVisible) menu_focus_sidebar();
            else { g_cfg_settings = false; g_cfg_focus = 2; }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) g_config_result = 1;
    ImGui::PopFont();
    ImGui::End();
}

// (main thread) the window's frame with the page drawn; shot: a test's picture of it
static void page_frame(SDL_Window *w, const char *shot) {
#ifdef __APPLE__
    @autoreleasepool {
        (void)shot;
        CAMetalLayer *layer = (__bridge CAMetalLayer *)ui_metal_layer();
        id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
        static id<MTLCommandQueue> queue;
        if (!queue) queue = [dev newCommandQueue];
        id<CAMetalDrawable> d = layer ? [layer nextDrawable] : nil;
        if (d) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            menu_render(dev, cb, d.texture);
            [cb presentDrawable:d];
            [cb commit];
        } else {
            SDL_Delay(16);
        }
    }
#else
    static const float bgc[4] = {0, 0, 0, 1};
    if (vk_frame_begin(bgc)) {
        int pw, ph;
        SDL_GetWindowSizeInPixels(w, &pw, &ph);
        menu_render(vk_frame_cmd(), (uint32_t)pw, (uint32_t)ph);
        vk_frame_end(shot);
    } else {
        SDL_Delay(16);
    }
#endif
}

bool config_run(void) {
    // (test runs and --play: straight into the game)
    const char *skip_vars[] = {"ODYSSEY_NO_CONFIG", "ODYSSEY_INPUT", "ODYSSEY_HOST_INPUT", "ODYSSEY_WARP", "ODYSSEY_HIDDEN",
                               "ODYSSEY_EXIT_AFTER_SHOT", "ODYSSEY_SETUP_EXIT_WHEN_DONE", "ODYSSEY_BIND_TEST"};
    for (const char *v : skip_vars)
        if (const char *e = getenv(v); e && *e && !(v == skip_vars[0] && !strcmp(e, "0")) && !getenv("ODYSSEY_PREP_PAGE_TEST"))
            return true;
    // (the settings file read first: its "Show this screen at every start" was checked before ui_window_init read the
    // file, so the default - shown - always won and unticking it did nothing, on every platform: issue #74, owner's
    // Steam Deck 2026-11-16)
    settings_load();
    const char *force = getenv("ODYSSEY_CONFIG");  // (1: shown even when turned off; 0: never)
#ifndef __APPLE__
    bool prep_due = vk_prepare_due();  // (shown for that even when the page is turned off: no window of its own)
#else
    bool prep_due = false;
#endif
    LOG("[config] show at start %d, shaders to prepare %d%s\n", g_settings.config_at_start, prep_due,
        force ? " (ODYSSEY_CONFIG)" : "");
    {   // the last session crashed (its notice not shown yet): this page shows once for it, even when turned off
        bool seen = true;
        g_crash_notice = diagnostics_last_crash_info(g_crash_path, sizeof g_crash_path, g_crash_reason, sizeof g_crash_reason,
                                                     &seen) && !seen;
        if (g_crash_notice) {
            LOG("[config] the last session crashed: %s (%s)\n", g_crash_reason, g_crash_path);
            diagnostics_last_crash_seen();
        }
    }
    if (force ? atoi(force) == 0 : !g_settings.config_at_start && !prep_due && !g_crash_notice) return true;
    ui_window_init();
#ifndef __APPLE__
    if (prep_due && !g_settings.config_at_start) g_prep = PREP_ASK;  // (straight to the question)
#ifndef __APPLE__
    g_prep_due_home = prep_due;
#endif
#endif
    SDL_Window *w = ui_window();
    LOG("[config] the configuration page before the game\n");
    g_config_result = 0;
    g_config = true;
    menu_setup_mode(true);
    static const char *shot = getenv("ODYSSEY_CONFIG_SHOT");  // (test) <frame>: the page as build/config.png, then Play
    if (getenv("ODYSSEY_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("ODYSSEY_MENU_TAB")); }  // (tests: that tab)
    if (const char *v = getenv("ODYSSEY_CONFIG_VIEW"); v && !strcmp(v, "settings")) g_cfg_settings = true, g_cfg_focus = 0;  // (tests)
    long frame = 0;
    int result = 0;
    {   // controllers: SDL sends their buttons only for opened ones - the game's input (input.c) opens them later, so the
        // page answered only mouse and keyboard (owner 2026-11-16). input.c finds them open and takes them over.
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        for (int i = 0; ids && i < n; i++) SDL_OpenGamepad(ids[i]);
        SDL_free(ids);
    }
    {   // the controller's cursor on the first tab, as the menu in game (Down: its first setting; Start plays)
        extern void menu_tabs_focus(bool first_tab);
        menu_tabs_focus(false);  // (the tab the page opens on)
    }
    while (!result) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) result = 2;
            else {
                if (e.type == SDL_EVENT_GAMEPAD_ADDED && !SDL_GetGamepadFromID(e.gdevice.which)) SDL_OpenGamepad(e.gdevice.which);
                if (!input_motion_only_event(&e)) menu_handle_event(&e);  // (the Deck's own controller under Steam Input: gyro only)
            }
        }
        menu_apply_window_requests(w);
        {   // the display's refresh for Frame rate: Match display (the game's renderer keeps it current later)
            SDL_DisplayID d = SDL_GetDisplayForWindow(w);
            const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
            if (m && m->refresh_rate > 0) g_display_hz = (int)(m->refresh_rate + 0.5f);
        }
        {   // (60 frames a second: the page is static, and a mailbox swapchain would otherwise draw it flat out)
            static uint64_t next;
            uint64_t now = SDL_GetTicksNS();
            if (next > now && next - now < 100000000ull) SDL_DelayNS(next - now);  // (a sleep, no spinning: a page, not the game)
            next = (next > now && next - now < 100000000ull ? next : now) + 16666667ull;
        }
        frame++;
        {   // ODYSSEY_CONFIG_PAD_TEST=1 (tests): a virtual controller appears at frame 30, moves down twice (90, 110: the
            // menu's cursor, build/config_pad.png at 150) and presses Start at 180 (Play)
            static int pad_test = -1;
            if (pad_test < 0) pad_test = getenv("ODYSSEY_CONFIG_PAD_TEST") != NULL;
            static SDL_Joystick *vj;
            if (pad_test && frame == 30) {
                SDL_VirtualJoystickDesc d;
                SDL_INIT_INTERFACE(&d);
                d.type = SDL_JOYSTICK_TYPE_GAMEPAD, d.naxes = 6, d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT, d.name = "test pad";
                vj = SDL_OpenJoystick(SDL_AttachVirtualJoystick(&d));
                LOG("[config] test controller %s\n", vj ? "attached" : SDL_GetError());
            }
            int b = frame == 90 || frame == 110 ? SDL_GAMEPAD_BUTTON_DPAD_DOWN : frame == 180 ? SDL_GAMEPAD_BUTTON_START : -1;
            if (vj && b >= 0) SDL_SetJoystickVirtualButton(vj, b, true);
            if (vj && (frame == 95 || frame == 115 || frame == 185)) {
                for (int k = 0; k < SDL_GAMEPAD_BUTTON_COUNT; k++) SDL_SetJoystickVirtualButton(vj, k, false);
            }
            if (pad_test && frame == 150) { page_frame(w, "build/config_pad.png"); continue; }
        }
        bool s = shot && frame == atol(shot);
#ifndef __APPLE__
        // ODYSSEY_PREP_PAGE_TEST=1 (tests): the question at frame 60 (build/prep_ask.png), Compile now at 90, the progress
        // at 900 (build/prep_run.png), then stopped. =play: Compile now at 90, left to finish, then the game (a new
        // player's path: tools' stutter census)
        static const char *pt = getenv("ODYSSEY_PREP_PAGE_TEST");
        const char *pshot = NULL;
        if (pt && !strcmp(pt, "play")) {
            if (frame == 30 && g_prep.load() == PREP_NONE) g_prep = PREP_ASK;
            if (frame == 90 && g_prep.load() == PREP_ASK) { g_prep_stay = false; prep_start(); }
        } else if (pt) {
            if (frame == 30 && g_prep.load() == PREP_NONE) g_prep = PREP_ASK;
            if (frame == 60) pshot = "build/prep_ask.png";
            if (frame == 90 && g_prep.load() == PREP_ASK) { g_prep_stay = !strcmp(pt, "stay"); prep_start(); }
            if (!strcmp(pt, "stay") && frame > 901 && g_prep.load() == PREP_NONE) { pshot = "build/prep_back.png"; result = 2; }
            if (frame == 900) pshot = "build/prep_run.png";
            if (frame == 901) g_prep_cancel = 1;
        }
        if (pshot) { page_frame(w, pshot); continue; }
#endif
        page_frame(w, s ? "build/config.png" : NULL);
        if (s) result = 1;
        if (!result) result = g_config_result.exchange(0);
#ifndef __APPLE__
        // Play with a preparation due: the question first (the shot test: straight on)
        if (result == 1 && !s && g_prep.load() == PREP_NONE && vk_prepare_due()) { g_prep = PREP_ASK; result = 0; }
        static bool quit_after;  // (closed while compiling: it stops first, then the program exits)
        if (g_prep.load() == PREP_RUN && result == 2) { g_prep_cancel = 1; quit_after = true; result = 0; }
        if (g_prep.load() == PREP_DONE && !result) {
            if (g_prep_stay && !quit_after) {  // (compiled, not played: the start page again, Play starts the game)
                g_prep_stay = false, g_prep = PREP_NONE, g_cfg_settings = false, g_cfg_focus = 1;
                g_prep_due_home = g_prep_code.load() != 0;  // (stopped: still due, the button stays)
                LOG("[config] shader compilation ended (code %d); back on the start page (play later)\n", g_prep_code.load());
            } else result = quit_after ? 2 : 1;
        }
#endif
    }
#ifndef __APPLE__
    while (g_prep.load() == PREP_RUN) SDL_Delay(20);  // (stopped: its process ends)
#endif
    g_config = false;
    menu_setup_mode(false);
    settings_save();
    LOG("[config] %s\n", result == 1 ? "play" : "exit");
    if (result == 1) restart_after_prep();
    return result == 1;
}

void setup_draw(float scale, void *font_v, void *font_big_v) {
    ImFont *font = (ImFont *)font_v, *big = (ImFont *)font_big_v;
    if (g_config.load()) { config_page(scale, font, big); return; }
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##setup", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(font);
    backdrop_draw(ImGui::GetBackgroundDrawList(), ImVec2(0, 0), io.DisplaySize, ImGui::GetTime());

    // the card
    float cardw = fminf(io.DisplaySize.x - 2 * 28 * scale, 1040 * scale), pad = 40 * scale;
    float colw = cardw - 2 * pad;
    // as tall as its content was last frame (at this scale), at most the window minus a margin: the rest scrolls
    static float content_h;  // (unscaled)
    float cardh = fminf(io.DisplaySize.y - 2 * 28 * scale, content_h > 0 ? content_h * scale + pad * 1.5f : 820 * scale);
    ImVec2 c0((io.DisplaySize.x - cardw) * 0.5f, (io.DisplaySize.y - cardh) * 0.42f);
    ImDrawList *bg = ImGui::GetWindowDrawList();
    bg->AddRectFilled(ImVec2(c0.x + 6 * scale, c0.y + 8 * scale), ImVec2(c0.x + cardw + 6 * scale, c0.y + cardh + 8 * scale),
                      IM_COL32(0, 0, 0, 90), 14 * scale);
    bg->AddRectFilled(c0, ImVec2(c0.x + cardw, c0.y + cardh), theme_u32(kPanelV), 14 * scale);
    bg->AddRect(c0, ImVec2(c0.x + cardw, c0.y + cardh), IM_COL32(255, 255, 255, 28), 14 * scale, 0, 2 * scale);
    ImGui::SetCursorPos(ImVec2(c0.x + pad, c0.y + pad * 0.8f));
    ImGui::BeginChild("##col", ImVec2(colw, cardh - pad * 1.4f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    float textw = colw - (ImGui::GetScrollMaxY() > 0 ? ImGui::GetStyle().ScrollbarSize : 0);
    ImGui::PushTextWrapPos(textw);

    ImGui::PushFont(big);
    {   // the icon's pipe, then the name
        float lh = ImGui::GetFontSize(), sz = lh * 1.2f;
        ImVec2 lp = ImGui::GetCursorScreenPos();
        theme_logo(ImGui::GetWindowDrawList(), ImVec2(lp.x, lp.y + lh * 0.5f - sz * 0.55f), sz, IM_COL32(59, 196, 90, 255), 0);  // (the icon's pipe green)
        ImGui::Dummy(ImVec2(sz, lh));
        ImGui::SameLine(0, lh * 0.35f);
        ImGui::TextUnformatted("Super Odyssey PC Recomp");
    }
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
    ImGui::SetWindowFontScale(0.86f);
    ImGui::TextUnformatted("Super Mario Odyssey, recompiled to run natively on PC - from your own copy of the game.");
    ImGui::SetWindowFontScale(1);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 10 * scale));

    int st = g_state.load();
    std::string step, head, hint, game, keys, dest, notice, update;
    int notice_row;
    pthread_mutex_lock(&g_lock);
    step = g_step, head = g_fail_head, hint = g_fail_hint;
    game = g_pick_game, keys = g_keys, update = g_update, dest = g_dest.empty() ? g_here : g_dest, notice = g_notice, notice_row = g_notice_row;
    pthread_mutex_unlock(&g_lock);

    if (st == ST_BUSY) {
        ImGui::TextUnformatted(g_code_only ? "Getting the game code ready for this version." : "Installing.");
        ImGui::Dummy(ImVec2(0, 12 * scale));
        float f = g_frac.load();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = 14 * scale;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + colw, p.y + h), theme_u32(kSurfaceV), 4 * scale);
        dl->AddRectFilled(p, ImVec2(p.x + colw * fminf(fmaxf(f, 0), 1), p.y + h), theme_u32(kTextV, 0.85f), 4 * scale);
        ImGui::Dummy(ImVec2(colw, h));
        ImGui::Dummy(ImVec2(0, 8 * scale));
        char line[240];
        double secs = (double)(plat_now_ns() - g_t0) / 1e9;
        int left = f > 0.04f ? (int)ceil(secs * (1 - f) / f / 60) : 0;
        if (left > 0) snprintf(line, sizeof line, "%s   %.0f%%   about %d min left", step.c_str(), f * 100, left);
        else snprintf(line, sizeof line, "%s   %.0f%%", step.c_str(), f * 100);
        ImGui::TextUnformatted(line);
        ImGui::Dummy(ImVec2(0, 18 * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
        ImGui::TextUnformatted(g_code_only ? "This takes a few minutes, once."
                                           : "This happens once: the game data is checked and written, the game code is "
                                             "recompiled for this PC, and every shader is compiled ahead so the game "
                                             "doesn't stutter later. You can keep using your PC meanwhile.");
        ImGui::PopStyleColor();
    } else if (st == ST_DONE) {
        ImGui::TextUnformatted("All set. The game is installed and its shaders are compiled.");
        ImGui::Dummy(ImVec2(0, 24 * scale));
        if (accent_button("Play", scale)) g_want_play = true;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        steam_shortcut_draw(scale);
    } else {
        if (st == ST_FAILED) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.6f, 0.55f, 1));
            ImGui::TextUnformatted(head.c_str());
            ImGui::PopStyleColor();
            ImGui::TextUnformatted(hint.c_str());
            ImGui::Dummy(ImVec2(0, 6 * scale));
        }
        bool pkg = game_is_package(game);
        if (setup_docs_view()) {
            row("1. Your game: Super Mario Odyssey", "", "The extracted exefs and romfs folders of your own copy of the game",
                textw, scale, ROW_GAME, notice, notice_row);
            row("2. Install the game to", dest, "", textw, scale, ROW_DEST, notice, notice_row);
            ImGui::Dummy(ImVec2(0, 16 * scale));
            accent_button("Install", scale, true);
            ImGui::Dummy(ImVec2(0, 10 * scale));
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
            ImGui::SetWindowFontScale(0.7f);
            ImGui::TextUnformatted("Nothing of the game is included, and nothing is uploaded anywhere: your game files are only "
                                   "read on this PC. The install takes about the size of the game plus a few GB (the recompiled "
                                   "code and the compiled shaders).");
            ImGui::SetWindowFontScale(1);
            ImGui::PopStyleColor();
        } else {
        row("1. Your game: Super Mario Odyssey", game, "An NSP or XCI dumped from your console, or the extracted folder",
            textw, scale, ROW_GAME, notice, notice_row);
        if (update_supported())
            row("2. Update 1.4.1 (optional)", update, pkg ? "The 1.4.1 update dumped from your console, as an NSP"
                                                            : "For an NSP or XCI game file", textw, scale,
                ROW_UPDATE, notice, notice_row, !pkg);
        else
            row("2. Update 1.4.1", "", "Not supported yet - planned for a later version", textw, scale, ROW_UPDATE, notice, notice_row, true);
        if (pkg || !keys.empty())
            row("3. prod.keys", keys, "The prod.keys dumped from your own console", textw, scale, ROW_KEYS, notice, notice_row);
        else
            row("3. prod.keys", "", "Only needed for an NSP or XCI file", textw, scale, ROW_KEYS, notice, notice_row, true);
        row("4. Install the game to", dest, "", textw, scale, ROW_DEST, notice, notice_row);
        ImGui::Dummy(ImVec2(0, 16 * scale));
        bool ready = !game.empty() && (!pkg || !keys.empty());
        if (accent_button(st == ST_FAILED ? "Try again" : "Install", scale, ready)) g_want_install = true;
        ImGui::Dummy(ImVec2(0, 10 * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
        ImGui::SetWindowFontScale(0.7f);
        ImGui::TextUnformatted("Nothing of the game is included, and nothing is uploaded anywhere: your game file and keys are "
                               "only read on this PC. The install takes about the size of the game plus a few GB "
                               "(the recompiled code and the compiled shaders).");
        ImGui::SetWindowFontScale(1);
        ImGui::PopStyleColor();
        }
    }
    ImGui::PopTextWrapPos();
    content_h = ImGui::GetCursorPosY() / scale;
    ImGui::EndChild();

    // footer
    {
        const char *t = "A fan project, not affiliated with or endorsed by Nintendo.";
        float s = 0.72f;
        ImGui::SetWindowFontScale(s);
        ImVec2 ts = ImGui::CalcTextSize(t);
        ImVec2 tp((io.DisplaySize.x - ts.x) * 0.5f, io.DisplaySize.y - ts.y - 14 * scale);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(tp.x - 10 * scale, tp.y - 4 * scale),
                                                  ImVec2(tp.x + ts.x + 10 * scale, tp.y + ts.y + 4 * scale),
                                                  IM_COL32(0, 0, 0, 120), 6 * scale);
        ImGui::SetCursorPos(tp);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.85f));
        ImGui::TextUnformatted(t);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1);
    }
    ImGui::PopFont();
    ImGui::End();
}

// ------------------------------------------------------------------ the loop (main thread)
bool setup_run(const char *root) {
    g_root = root;
    g_here = rt_data_root();
    ui_window_init();
    SDL_Window *w = ui_window();
    std::string r = root;
    LOG("[setup] data root %s, game files %s\n", rt_data_root(), root);
    if (loader_peek_game((r + "/exefs/main").c_str()) == GAME_ODYSSEY && data_there(r)) {
        g_code_only = true;  // a new version of the app: the files are there, its game code isn't
        start_work();
    }
    menu_setup_mode(true);
    // (tests) the choices as given
    if (const char *p = getenv("ODYSSEY_SETUP_PICK"); p && *p) choose_game(p);
    if (const char *k = getenv("ODYSSEY_SETUP_KEYS"); k && *k) g_keys = k;
    if (const char *d = getenv("ODYSSEY_SETUP_DEST"); d && *d) g_dest = d;
    if (const char *u = getenv("ODYSSEY_SETUP_UPDATE"); u && *u) g_update = u;
    if (const char *s = getenv("ODYSSEY_SETUP_START"); s && atoi(s) && !g_pick_game.empty() && g_state.load() == ST_IDLE)
        g_want_install = true;
    static const char *shot = getenv("ODYSSEY_SETUP_SHOT");  // (test) <frame>: the page as build/setup.png
    long frame = 0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {  // (a build running: its helpers end with this process)
                LOG("window closed during setup\n");
                g_cancel = 1;
                menu_setup_mode(false);
                return false;
            }
            menu_handle_event(&e);
        }
        menu_apply_window_requests(w);
        bool busy = g_state.load() == ST_BUSY;
        switch (busy ? (int)DLG_NONE : g_want_dialog.exchange(DLG_NONE)) {
        case DLG_GAME_FILE: {
            static const SDL_DialogFileFilter f[] = {{"Switch game file (NSP, XCI)", "nsp;xci"}};
            SDL_ShowOpenFileDialog(picked_game, NULL, w, f, 1, NULL, false);
            break;
        }
        case DLG_GAME_FOLDER: SDL_ShowOpenFolderDialog(picked_game, NULL, w, NULL, false); break;
        case DLG_KEYS: {
            static const SDL_DialogFileFilter f[] = {{"prod.keys", "keys"}};
            SDL_ShowOpenFileDialog(picked_keys, NULL, w, f, 1, NULL, false);
            break;
        }
        case DLG_DEST: SDL_ShowOpenFolderDialog(picked_dest, NULL, w, g_here.c_str(), false); break;
        case DLG_UPDATE: {
            static const SDL_DialogFileFilter f[] = {{"Update (NSP)", "nsp"}};
            SDL_ShowOpenFileDialog(picked_update, NULL, w, f, 1, NULL, false);
            break;
        }
        default: break;
        }
        if (g_want_install.exchange(false) && !busy) {
            pthread_mutex_lock(&g_lock);
            std::string game = g_pick_game, keys = g_keys, dest = g_dest, update = update_supported() ? g_update : "";
            // another folder for the game: the data root moves there (paths.c rt_set_data_root, remembered next to the
            // exe); the program itself stays where it is
            bool ok = true;
            if (!dest.empty() && norm(dest) != norm(g_here)) {
                ok = rt_set_data_root(dest.c_str());
                if (ok) {
#ifndef __APPLE__
                    vk_shader_cache_reset();
#endif
                    g_here = rt_data_root();
                    g_dest.clear();
                }
                else g_notice = "That folder can't be used (Program Files needs admin rights). Choose a folder you own, "
                                "e.g. in Documents or on another drive.", g_notice_row = ROW_DEST;
            }
            pthread_mutex_unlock(&g_lock);
            if (ok) {
                g_job_game = game, g_job_keys = keys, g_job_update = update, g_code_only = false;
                start_work();
            }
        }
        if (g_state.load() == ST_DONE && getenv("ODYSSEY_SETUP_EXIT_WHEN_DONE")) {  // (test) end here, the game not started
            LOG("[setup] done; exiting (ODYSSEY_SETUP_EXIT_WHEN_DONE)\n");
            menu_setup_mode(false);
            return false;
        }
        if (g_state.load() == ST_DONE && (g_code_only || g_want_play.exchange(false))) {
            if (!g_code_only && !getenv("ODYSSEY_WINDOW")) {  // a new install plays in fullscreen (the menu can change it)
                g_settings.window_mode = BORDERLESS;
                settings_save();
                menu_request_window_apply();
                menu_apply_window_requests(w);
            }
            if (!g_code_only) restart_after_prep();
            break;
        }
#ifdef __APPLE__
        @autoreleasepool {  // the window's Metal layer: the page drawn over the whole drawable, then presented
            CAMetalLayer *layer = (__bridge CAMetalLayer *)ui_metal_layer();
            id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
            static id<MTLCommandQueue> queue;
            if (!queue) queue = [dev newCommandQueue];
            id<CAMetalDrawable> d = layer ? [layer nextDrawable] : nil;
            if (d) {
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                menu_render(dev, cb, d.texture);
                [cb presentDrawable:d];
                [cb commit];
            } else {
                SDL_Delay(16);
            }
        }
#else
        static const float bgc[4] = {92 / 255.0f, 148 / 255.0f, 238 / 255.0f, 1};
        if (vk_frame_begin(bgc)) {
            int pw, ph;
            SDL_GetWindowSizeInPixels(w, &pw, &ph);
            menu_render(vk_frame_cmd(), (uint32_t)pw, (uint32_t)ph);
            frame++;
            bool s = shot && frame == atol(shot);
            vk_frame_end(s ? "build/setup.png" : NULL);
        } else {
            SDL_Delay(16);
        }
#endif
        // 60 pictures a second, also while it works (owner 2026-11-18: at the ~15 a second it had while installing, the
        // page and its progress bar looked stuck). The cost is small now that it is paced: on a Steam Deck the page at
        // 60 took 9.5% of one core, a fifth of that in the precise delay's spinning - a plain sleep here. (Unpaced, on
        // a mailbox swapchain, it was drawn thousands of times a second and took most of a core from the compile.)
        (void)busy;
        {
            static uint64_t next;
            uint64_t now = SDL_GetTicksNS();
            if (next > now && next - now < 100000000ull) SDL_DelayNS(next - now);
            next = (next > now && next - now < 100000000ull ? next : now) + 16666667ull;
        }
    }
    menu_setup_mode(false);
    return true;
}
