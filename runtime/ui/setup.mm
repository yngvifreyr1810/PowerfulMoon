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

#ifdef __APPLE__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif
#include "menu.h"
#include "setup.h"

extern "C" {
#include "../gamebuild.h"
#include "../gamelib.h"
#include "../paths.h"
#include "../platform.h"
#include "../romfs_build.h"
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
        "Power Moon Recomp needs the extracted game files (exefs and romfs), not a game package (NSP, XCI).",
        "Power Moon Recomp runs version 1.0.0 of Super Mario Odyssey only, without updates.",
        "exefs has no sdk. Extract the game files again and choose that folder.",
        "",
        "Writing the game data failed. Make sure the drive has room and try again.",
        "Download Power Moon Recomp again and keep everything in its folder together (the zig and odysseygen folders).",
        "Make sure there's about 2 GB free and try again. If it happens again, report it with PowerMoonRecomp.log.",
#ifdef __APPLE__
        "macOS didn't let Power Moon Recomp read it. Move the game files into a folder in your home folder (not the "
        "Desktop, Documents, Downloads or iCloud Drive) and choose that, or allow access: System Settings - Privacy & "
        "Security - Files and Folders (or Full Disk Access) for Power Moon Recomp, or Terminal if you started it there."
#else
        "The system didn't let Power Moon Recomp read it. Copy the game files to a folder you own and choose that."
#endif
        , ""};
    char h[600];
    if (k == FAIL_SPACE)
        snprintf(h, sizeof h, "Setup needs about %.0f GB free on this drive, next to Power Moon Recomp. Free some space or "
                 "move the Power Moon Recomp folder to a drive with more room.", ceil(need_gb));
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

static bool accent_button(const char *label, float scale) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccentV);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(fminf(kAccentV.x * 1.12f, 1), kAccentV.y * 1.12f, kAccentV.z * 1.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentDeepV);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, kTextV);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(30 * scale, 14 * scale));
    bool r = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return r;
}

void setup_draw(float scale, void *font_v, void *font_big_v) {
    ImFont *font = (ImFont *)font_v, *big = (ImFont *)font_big_v;
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
    ImGui::TextUnformatted("Super Mario Odyssey, recompiled to run natively on this PC.");
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
            ImGui::TextUnformatted("Power Moon Recomp includes no game files, and nothing is uploaded anywhere. Setup "
                                   "copies what the game needs next to Power Moon Recomp (about 6 GB) and recompiles the "
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
