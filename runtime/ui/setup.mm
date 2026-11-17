// First start: the player's own game files (setup.h). The page is drawn by the menu (menu.mm calls setup_draw in setup
// mode, its ImGui context, fonts and input); a thread does the work: exefs copied into the data root's extracted/, the
// RomFS image built straight from the picked romfs (runtime/romfs_build.c: about 5 GB, the only copy of it), the game
// code recompiled on this machine (runtime/gamebuild.c: a few minutes). An app update with new game code rebuilds
// only that, at once.
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
#include "theme.h"
#include "i18n.h"

#ifdef __APPLE__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif
#include "menu.h"
#include "setup.h"
#include "steam_shortcut.h"
#include "backdrop.h"

extern "C" {
#include "../gamebuild.h"
#include "../gamelib.h"
#include "../paths.h"
#include "../platform.h"
#include "../romfs_build.h"
#include "../settings.h"
#include "../rt.h"
#ifdef __APPLE__
void *ui_metal_layer(void);
void *ui_metal_device(void);
#else
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
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

static bool data_there(const std::string &root) {  // the RomFS image, or (a source checkout) the folder main.c makes it from
    return file_exists(root + "/romfs.bin") || is_dir(root + "/romfs/StageData");
}

bool setup_ready(const char *root) {
    std::string r = root;
    return loader_peek_game((r + "/exefs/main").c_str()) == GAME_ODYSSEY && data_there(r) && gamelib_present(root);
}

// exefs: the folder with main and main.npdm; romfs: the folder with StageData and SystemData (the game's data root).
// Found in the picked folder or a few levels below it (dumping tools nest them differently).
static int g_scan_errno;  // the first folder that couldn't be read while looking (EPERM / EACCES: macOS privacy)
static void find_game(const std::string &dir, int depth, std::string &exefs, std::string &romfs, bool &packaged) {
    if (depth > 4 || (!exefs.empty() && !romfs.empty())) return;
    if (exefs.empty() && file_exists(dir + "/main") && file_exists(dir + "/main.npdm")) exefs = dir;
    if (romfs.empty() && is_dir(dir + "/StageData") && is_dir(dir + "/SystemData")) romfs = dir;
    DIR *d = opendir(dir.c_str());
    if (!d) {
        if (!g_scan_errno) g_scan_errno = errno ? errno : EACCES;
        LOG("[setup] can't read %s: %s\n", dir.c_str(), strerror(errno));
        return;
    }
    if (depth == 0) {  // (what the chosen folder holds: the log says why nothing was found)
        std::string names;
        int n = 0;
        while (struct dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            if (n++ < 12) names += std::string(n > 1 ? ", " : "") + e->d_name;
        }
        LOG("[setup] chosen folder %s holds %d entries: %s%s\n", dir.c_str(), n, names.c_str(), n > 12 ? ", ..." : "");
        rewinddir(d);
    }
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n[0] == '.') continue;
        std::string ext = n.size() > 4 ? n.substr(n.size() - 4) : "";
        for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
        if (ext == ".xci" || ext == ".nsp" || ext == ".nca" || ext == ".nsz" || ext == ".xcz") packaged = true;
        if (n != "StageData" && n != "ObjectData" && is_dir(dir + "/" + n)) sub.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(sub.begin(), sub.end());
    for (auto &s : sub) find_game(s, depth + 1, exefs, romfs, packaged);
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

static std::string lower_slashes(std::string s) {  // (comparing folders: one separator, one case on Windows)
    for (auto &c : s) {
        if (c == '\\') c = '/';
#ifdef _WIN32
        c = (char)tolower((unsigned char)c);
#endif
    }
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// ------------------------------------------------------------------ the work (its own thread)
enum { ST_IDLE, ST_BUSY, ST_FAILED, ST_DONE };
static std::atomic<int> g_state{ST_IDLE};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_root;                  // where the game files go (the data root's extracted/)
static std::string g_pick;                  // the folder the player chose ("" = only the game code to build)
static std::atomic<bool> g_picked{false};   // (the folder dialog answers on any thread)
static std::string g_step, g_fail_head, g_fail_hint;
static std::atomic<float> g_frac{0};
static uint64_t g_t0;                       // when the work started (time left)
static bool g_code_only;                    // the files are there: only the game code (a new version of the app)

static void set_step(const char *s) {
    pthread_mutex_lock(&g_lock);
    g_step = s;
    pthread_mutex_unlock(&g_lock);
    LOG("[setup] %s\n", s);
}

enum { FAIL_NOT_FOUND, FAIL_PACKAGED, FAIL_VERSION, FAIL_INCOMPLETE, FAIL_SPACE, FAIL_COPY, FAIL_BUILDER, FAIL_BUILD,
       FAIL_ACCESS, FAIL_ROMFS };
static void fail(int k, double need_gb = 0) {
    static const char *const head[] = {"No game files found in that folder", "Those files can't be used",
                                       "Wrong version of the game", "Game files incomplete", "Not enough free space",
                                       "Setup didn't finish", "The game code builder is missing",
                                       "The game code didn't build", "No access to that folder", "Game files incomplete"};
    static const char *const hint[] = {
        "Choose the folder that holds the extracted game: exefs and romfs, or their contents.",
        "PowerfulMoon needs the extracted game files (exefs and romfs), not a game package (NSP, XCI).",
        "PowerfulMoon runs version 1.0.0 of Super Mario Odyssey only, without updates.",
        "exefs has no sdk. Extract the game files again and choose that folder.",
        "",
        "Writing the game data failed. Make sure the drive has room and try again.",
        "Download PowerfulMoon again and keep everything in its folder together (the zig and odysseygen folders).",
        "Make sure there's about 2 GB free and try again. If it happens again, report it with PowerfulMoon.log.",
#ifdef __APPLE__
        "macOS didn't let PowerfulMoon read it. Move the game files into a folder in your home folder (not the "
        "Desktop, Documents, Downloads or iCloud Drive) and choose that, or allow access: System Settings - Privacy & "
        "Security - Files and Folders (or Full Disk Access) for PowerfulMoon, or Terminal if you started it there."
#else
        "The system didn't let PowerfulMoon read it. Copy the game files to a folder you own and choose that."
#endif
        , ""};
    char h[600];
    if (k == FAIL_SPACE)
        snprintf(h, sizeof h, "Setup needs about %.0f GB free on this drive, next to PowerfulMoon. Free some space or "
                 "move the PowerfulMoon folder to a drive with more room.", ceil(need_gb));
    else if (k == FAIL_ROMFS)  // (need_gb: the size found)
        snprintf(h, sizeof h, "Its romfs holds %.1f GB, Super Mario Odyssey 1.0.0's holds 5.2 GB. This is probably the "
                 "romfs of an update (only the files it changes): extract the base game, version 1.0.0, without "
                 "updates, and choose that folder.", need_gb);
    else
        snprintf(h, sizeof h, "%s", hint[k]);
    pthread_mutex_lock(&g_lock);
    g_fail_head = head[k];
    g_fail_hint = h;
    pthread_mutex_unlock(&g_lock);
    LOG("[setup] failed: %s\n", head[k]);
    g_state = ST_FAILED;
}

static void data_progress(uint64_t done, uint64_t total) {  // the RomFS image: the first fifth of the bar
    if (total) g_frac = 0.2f * (float)((double)done / (double)total);
}
static void code_progress(double f, const char *step) {  // the game code: the rest
    g_frac = (g_code_only ? 0 : 0.2f) + (g_code_only ? 1.0f : 0.8f) * (float)f;
    static const char *last;
    if (step != last) last = step, set_step(step);
}

static void *work(void *) {
    std::string root = g_root, src;
    pthread_mutex_lock(&g_lock);
    src = g_pick;
    pthread_mutex_unlock(&g_lock);
    g_frac = 0;
    g_t0 = plat_now_ns();
    if (!gamebuild_available()) { fail(FAIL_BUILDER); return NULL; }
    if (!g_code_only) {
        set_step("Looking for the game files...");
        std::string exefs, romfs;
        bool packaged = false;
        g_scan_errno = 0;
        find_game(src, 0, exefs, romfs, packaged);
        LOG("[setup] exefs: %s, romfs: %s%s\n", exefs.empty() ? "(none)" : exefs.c_str(), romfs.empty() ? "(none)" : romfs.c_str(),
            packaged ? " (a game package is there)" : "");
        if (exefs.empty() || romfs.empty()) {
            fail(packaged ? FAIL_PACKAGED : g_scan_errno == EPERM || g_scan_errno == EACCES ? FAIL_ACCESS : FAIL_NOT_FOUND);
            return NULL;
        }
        if (loader_peek_game((exefs + "/main").c_str()) != GAME_ODYSSEY) { fail(FAIL_VERSION); return NULL; }
        if (!file_exists(exefs + "/sdk")) { fail(FAIL_INCOMPLETE); return NULL; }
        // (the picked folder can't be our own copy: setting up again deletes that first)
        std::string mine = lower_slashes(std::string(rt_data_root()) + "/" + root);
        std::string pe = lower_slashes(exefs), pr = lower_slashes(romfs);
        if (!pe.compare(0, mine.size(), mine) || !pr.compare(0, mine.size(), mine)) { fail(FAIL_NOT_FOUND); return NULL; }
        uint64_t rbytes = romfs_dir_bytes(romfs.c_str());
        double need = (double)rbytes / 1e9 + 1.5, have = free_gb(rt_data_root());  // (+ the game code while it builds)
        LOG("[setup] exefs %s, romfs %s (%.1f GB); %.1f GB free here\n", exefs.c_str(), romfs.c_str(), rbytes / 1e9, have);
        // (1.0.0's is 5.2 GB; a player chose an update's, 1.2 GB, and the game stopped at boot: services/fs.c)
        if (rbytes < 4500000000ull) { fail(FAIL_ROMFS, rbytes / 1e9); return NULL; }
        if (have >= 0 && have < need) { fail(FAIL_SPACE, need); return NULL; }
        rt_remove_game_files(root.c_str());  // (an earlier or interrupted setup's; removes the folder too)
        mkdir(root.c_str(), 0755);
        set_step("Copying the game files...");
        if (!romfs_copy_tree(exefs.c_str(), (root + "/exefs").c_str(), NULL)) { fail(FAIL_COPY); return NULL; }
        // the videos (227 MB): the movie player opens them as files (hle/movie.c), not from the RomFS image - without
        // them the first tutorial clip (after Cappy) crashed the game's decoder thread
        mkdir((root + "/romfs").c_str(), 0755);
        if (is_dir(romfs + "/MovieData") &&
            !romfs_copy_tree((romfs + "/MovieData").c_str(), (root + "/romfs/MovieData").c_str(), NULL)) {
            fail(FAIL_COPY);
            return NULL;
        }
        set_step("Building the game data...");
        char why[256] = "";
        if (!romfs_build(romfs.c_str(), (root + "/romfs.bin").c_str(), data_progress, why, sizeof why)) {
            LOG("[setup] RomFS image: %s\n", why);
            fail(FAIL_COPY);
            return NULL;
        }
        g_frac = 0.2f;
    }
    set_step("Recompiling the game code...");
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    GameBuildOpts o = {code_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1};
    char why[256] = "";
    if (!gamebuild_run(root.c_str(), &o, why, sizeof why)) {
        LOG("[setup] game code: %s\n", why);
        fail(FAIL_BUILD);
        return NULL;
    }
    if (!setup_ready(root.c_str())) { fail(FAIL_INCOMPLETE); return NULL; }
    LOG("[setup] done in %.0fs\n", (double)(plat_now_ns() - g_t0) / 1e9);
    g_frac = 1;
    g_state = ST_DONE;
    return NULL;
}

static void start_work(void) {
    g_state = ST_BUSY;
    pthread_t th;
    if (pthread_create(&th, NULL, work, NULL) == 0) pthread_detach(th);
    else fail(FAIL_COPY);
}

static void SDLCALL picked(void *, const char *const *files, int) {
    if (!files || !files[0]) return;  // (cancelled, or the dialog failed)
    pthread_mutex_lock(&g_lock);
    g_pick = files[0];
    pthread_mutex_unlock(&g_lock);
    g_picked = true;
}

// ------------------------------------------------------------------ the page (render thread, menu.mm)
static std::atomic<bool> g_want_dialog{false};  // (the dialog opens on the main thread)

static bool accent_button(const char *label, float scale, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentV);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(fminf(kAccentV.x * 1.12f, 1), kAccentV.y * 1.12f, kAccentV.z * 1.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentDeepV);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, kTextV);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(30 * scale, 14 * scale));
    bool r = ImGui::Button(tr(label));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    return r && enabled;
}

// ------------------------------------------------------------------ the configuration page (before the game boots)
// Every start shows the settings first (the menu's tabs on the card), then Play or Exit - the settings that only take
// effect at the next start (memory use, warm-up...) can be set before the game is there. "Show at start" turns it off;
// the menu in play (F1, the controller's Select + Start) keeps every setting. Test runs and --play go straight in.
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
extern "C" int vk_prepare_install(void (*progress)(int phase, int done, int total), volatile int *cancel);
enum { PREP_NONE, PREP_ASK, PREP_RUN, PREP_DONE };
static std::atomic<int> g_prep{PREP_NONE}, g_prep_phase{0}, g_prep_done{0}, g_prep_total{0}, g_prep_code{0};
static volatile int g_prep_cancel;
static uint64_t g_prep_t0, g_prep_phase_t0;
static int g_prep_last_phase = -1;
static void prep_page_progress(int phase, int done, int total) {
    g_prep_phase = phase, g_prep_done = done, g_prep_total = total;
}
static void *prep_page_work(void *) {
    int code = vk_prepare_install(prep_page_progress, &g_prep_cancel);
    g_prep_code = code;
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
        if (accent_button("Compile now", scale)) prep_start();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        ImGui::SameLine(0, 16 * scale);
        if (accent_button("Later", scale)) { vk_prepare_skip_this_start(); g_prep = PREP_DONE; }
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
        if (accent_button("Stop and play", scale)) g_prep_cancel = 1;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
        ImGui::TextWrapped("%s", tr("What is compiled so far is kept; the rest is made while you play."));
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
}
#endif

static void config_page(float scale, ImFont *font, ImFont *big) {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##config", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(font);
    backdrop_draw(ImGui::GetBackgroundDrawList(), ImVec2(0, 0), io.DisplaySize, ImGui::GetTime());
    float margin = 22 * scale, pad = 28 * scale;
    float cardw = fminf(io.DisplaySize.x - 2 * margin, fmaxf(1240 * scale, io.DisplaySize.x * 0.82f)), cardh = io.DisplaySize.y - 2 * margin;
    ImVec2 c0((io.DisplaySize.x - cardw) * 0.5f, margin);
    ImDrawList *bg = ImGui::GetWindowDrawList();
    bg->AddRectFilled(c0, ImVec2(c0.x + cardw, c0.y + cardh), theme_u32(kPanelV, 0.86f), 14 * scale);  // (over a bright backdrop)
    bg->AddRect(c0, ImVec2(c0.x + cardw, c0.y + cardh), IM_COL32(255, 255, 255, 28), 14 * scale, 0, 2 * scale);
    float colw = cardw - 2 * pad;
    ImGui::SetCursorPos(ImVec2(c0.x + pad, c0.y + pad * 0.7f));
    ImGui::BeginGroup();
    ImGui::PushFont(big);
    {
        float lh = ImGui::GetFontSize(), sz = lh * 1.1f;
        ImVec2 lp = ImGui::GetCursorScreenPos();
        theme_crescent(ImGui::GetWindowDrawList(), ImVec2(lp.x, lp.y + lh * 0.5f - sz * 0.55f), sz, kAccent, theme_u32(kPanelV, 1));
        ImGui::Dummy(ImVec2(sz, lh));
        ImGui::SameLine(0, lh * 0.35f);
        ImGui::TextUnformatted(tr("Configuration"));
    }
    ImGui::PopFont();
    ImGui::EndGroup();
    if (plat_is_steam_deck()) {  // (the owner's finding, 2026-11-15: at a low power limit the GPU clock decides the frame rate)
        ImGui::SetCursorPosX(c0.x + pad);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
        ImGui::PushTextWrapPos(c0.x + cardw - pad);
        ImGui::TextWrapped("%s", tr("Steam Deck tip: press the ... button, open Performance, turn on Manual GPU Clock Control and set "
                           "it to 1000 MHz. With a low TDP limit (7-10 W) this keeps the GPU fast enough for a steady 60 FPS."));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
#ifndef __APPLE__
    if (g_prep.load() == PREP_ASK || g_prep.load() == PREP_RUN) {  // (the shader preparation: this card only)
        prep_card(scale, big, c0, cardw, cardh, pad);
        ImGui::PopFont();
        ImGui::End();
        return;
    }
#endif
    float footer = 84 * scale;
    float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(c0.x + pad * 0.4f, top));
    ImGui::BeginChild("##cfgtabs", ImVec2(cardw - pad * 0.8f, c0.y + cardh - top - footer - pad * 0.4f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    menu_tabs_draw(scale, font, big, false, NULL);
    ImGui::EndChild();

    // the footer: Play, Exit, and whether this page shows at start
    ImGui::SetCursorPos(ImVec2(c0.x + pad, c0.y + cardh - footer + 6 * scale));
    ImVec2 lp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(lp.x, lp.y - 8 * scale), ImVec2(lp.x + colw, lp.y - 8 * scale),
                                        IM_COL32(255, 255, 255, 22), 1.0f * scale);
    if (accent_button("Play", scale)) g_config_result = 1;  // (the controller's cursor starts on the first tab: config_run)
    ImGui::SameLine(0, 16 * scale);
    if (accent_button("Exit", scale)) g_config_result = 2;
    ImGui::SameLine(0, 32 * scale);
    {
        float bh = ImGui::GetFrameHeight() + 28 * scale - ImGui::GetFrameHeight();  // (the buttons' height: their padding)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (bh - ImGui::GetTextLineHeight()) * 0.5f + 7 * scale);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, kTextV);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, kSurfaceV);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, kAccentV);
        if (ImGui::Checkbox(tr("Show this screen at every start"), &g_settings.config_at_start)) settings_save();
        ImGui::PopStyleColor(3);
    }
    {   // (the controller's hint, at the right)
        const char *hint = tr("Start: Play");
        ImGui::SetWindowFontScale(0.8f);
        ImVec2 ts = ImGui::CalcTextSize(hint);
        ImGui::SameLine(c0.x + cardw - pad - ts.x);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1);
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
    const char *force = getenv("ODYSSEY_CONFIG");  // (1: shown even when turned off; 0: never)
#ifndef __APPLE__
    bool prep_due = vk_prepare_due();  // (shown for that even when the page is turned off: no window of its own)
#else
    bool prep_due = false;
#endif
    if (force ? atoi(force) == 0 : !g_settings.config_at_start && !prep_due) return true;
    ui_window_init();
#ifndef __APPLE__
    if (prep_due && !g_settings.config_at_start) g_prep = PREP_ASK;  // (straight to the question)
#endif
    SDL_Window *w = ui_window();
    LOG("[config] the configuration page before the game\n");
    g_config_result = 0;
    g_config = true;
    menu_setup_mode(true);
    static const char *shot = getenv("ODYSSEY_CONFIG_SHOT");  // (test) <frame>: the page as build/config.png, then Play
    if (getenv("ODYSSEY_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("ODYSSEY_MENU_TAB")); }  // (tests: that tab)
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
                menu_handle_event(&e);
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
            if (next > now && next - now < 100000000ull) SDL_DelayPrecise(next - now);
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
        // at 900 (build/prep_run.png), then stopped
        static const char *pt = getenv("ODYSSEY_PREP_PAGE_TEST");
        const char *pshot = NULL;
        if (pt) {
            if (frame == 30 && g_prep.load() == PREP_NONE) g_prep = PREP_ASK;
            if (frame == 60) pshot = "build/prep_ask.png";
            if (frame == 90 && g_prep.load() == PREP_ASK) prep_start();
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
        if (g_prep.load() == PREP_DONE && !result) result = quit_after ? 2 : 1;
#endif
    }
#ifndef __APPLE__
    while (g_prep.load() == PREP_RUN) SDL_Delay(20);  // (stopped: its process ends)
#endif
    g_config = false;
    menu_setup_mode(false);
    settings_save();
    LOG("[config] %s\n", result == 1 ? "play" : "exit");
    return result == 1;
}

void setup_draw(float scale, void *font_v, void *font_big_v) {
    ImFont *font = (ImFont *)font_v, *big = (ImFont *)font_big_v;
    if (g_config.load()) { config_page(scale, font, big); return; }
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(kPanelV.x, kPanelV.y, kPanelV.z, 1));
    ImGui::Begin("##setup", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(font);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float colw = fminf(io.DisplaySize.x - 2 * 56 * scale, 980 * scale);
    float top = fmaxf(io.DisplaySize.y * 0.14f, 40 * scale);
    ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - colw) * 0.5f, top));
    ImGui::BeginChild("##col", ImVec2(colw, io.DisplaySize.y - top - 30 * scale), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImGui::PushTextWrapPos(colw);

    // the crescent and the name (as the menu's header), what this is
    ImGui::PushFont(big);
    {
        float lh = ImGui::GetFontSize(), sz = lh * 1.5f;
        ImVec2 lp = ImGui::GetCursorScreenPos();
        theme_crescent(dl, ImVec2(lp.x - sz / 6, lp.y + lh * 0.5f - sz * 0.5f), sz, kAccent, theme_u32(kPanelV, 1));
        ImGui::Dummy(ImVec2(sz * 2 / 3, lh));
        ImGui::SameLine(0, lh * 0.42f);
        ImGui::TextUnformatted(RT_APP_NAME);
    }
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
    ImGui::TextUnformatted("A customized fork of Power Moon Recomp by zzzarathustra-dev.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 26 * scale));

    int st = g_state.load();
    std::string step, head, hint;
    pthread_mutex_lock(&g_lock);
    step = g_step, head = g_fail_head, hint = g_fail_hint;
    pthread_mutex_unlock(&g_lock);
    if (st == ST_BUSY || st == ST_DONE) {
        ImGui::TextUnformatted(g_code_only ? "Getting the game code ready for this version." : "Setting up your game files.");
        ImGui::Dummy(ImVec2(0, 10 * scale));
        float f = g_frac.load();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = 8 * scale;
        dl->AddRectFilled(p, ImVec2(p.x + colw, p.y + h), theme_u32(kSurfaceV));
        dl->AddRectFilled(p, ImVec2(p.x + colw * fminf(fmaxf(f, 0), 1), p.y + h), kAccent);
        ImGui::Dummy(ImVec2(colw, h));
        ImGui::Dummy(ImVec2(0, 6 * scale));
        char line[200];
        double secs = (double)(plat_now_ns() - g_t0) / 1e9;
        int left = f > 0.04f ? (int)ceil(secs * (1 - f) / f / 60) : 0;
        if (left > 0) snprintf(line, sizeof line, "%s   %.0f%%   about %d min left", step.c_str(), f * 100, left);
        else snprintf(line, sizeof line, "%s   %.0f%%", step.c_str(), f * 100);
        ImGui::TextUnformatted(line);
        ImGui::Dummy(ImVec2(0, 18 * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
        ImGui::TextUnformatted("This takes a few minutes, once. The game starts when it's done; you can keep using your PC "
                               "meanwhile.");
        ImGui::PopStyleColor();
    } else {
        if (st == ST_FAILED) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAccentSoftV);
            ImGui::TextUnformatted(head.c_str());
            ImGui::PopStyleColor();
            ImGui::TextUnformatted(hint.c_str());
        } else {
            ImGui::TextUnformatted("To play, choose the folder with your own game files: Super Mario Odyssey version "
                                   "1.0.0 (no updates), extracted (the exefs and romfs folders).");
            ImGui::Dummy(ImVec2(0, 6 * scale));
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
            ImGui::TextUnformatted("PowerfulMoon includes no game files, and nothing is uploaded anywhere. Setup "
                                   "copies what the game needs next to PowerfulMoon (about 6 GB) and recompiles the "
                                   "game code on this PC.");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 24 * scale));
        if (accent_button(st == ST_FAILED ? "Choose another folder" : "Choose game folder", scale)) g_want_dialog = true;
        ImGui::SetItemDefaultFocus();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    // footer
    {
        const char *t = "A fan project, not affiliated with or endorsed by Nintendo.";
        ImVec2 ts = ImGui::CalcTextSize(t);
        float s = 0.72f;
        ImGui::SetWindowFontScale(s);
        ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - ts.x * s) * 0.5f, io.DisplaySize.y - ts.y * s - 22 * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, kTextOffV);
        ImGui::TextUnformatted(t);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1);
    }
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------ the loop (main thread)
bool setup_run(const char *root) {
    g_root = root;
    ui_window_init();
    SDL_Window *w = ui_window();
    std::string r = root;
    LOG("[setup] data root %s, game files %s\n", rt_data_root(), root);
    if (loader_peek_game((r + "/exefs/main").c_str()) == GAME_ODYSSEY && data_there(r)) {
        g_code_only = true;  // a new version of the app: the files are there, its game code isn't
        start_work();
    }
    menu_setup_mode(true);
    if (const char *p = getenv("ODYSSEY_SETUP_PICK")) picked(NULL, &p, 0);  // (test) as if that folder was chosen
    static const char *shot = getenv("ODYSSEY_SETUP_SHOT");  // (test) <frame>: the page as build/setup.png
    long frame = 0;
    while (g_state.load() != ST_DONE) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {  // (a build running: its helpers end with this process)
                LOG("window closed during setup\n");
                menu_setup_mode(false);
                return false;
            }
            menu_handle_event(&e);
        }
        menu_apply_window_requests(w);
        if (g_want_dialog.exchange(false) && g_state.load() != ST_BUSY) SDL_ShowOpenFolderDialog(picked, NULL, w, NULL, false);
        if (g_picked.exchange(false) && g_state.load() != ST_BUSY) { g_code_only = false; start_work(); }
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
        static const float bg[4] = {18 / 255.0f, 19 / 255.0f, 22 / 255.0f, 1};
        if (vk_frame_begin(bg)) {
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
    }
    menu_setup_mode(false);
    return true;
}
