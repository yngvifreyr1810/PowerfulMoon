// Linux's opt-in shortcut controls, shared by setup completion and Updates.
#include "steam_shortcut.h"
#ifdef __linux__
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "imgui.h"
extern "C" {
#include "../paths.h"
#include "../platform.h"
}

static std::atomic<bool> busy;
static std::mutex result_lock;
static std::string result;

static void shortcut_request(bool undo, const std::string &image, const std::string &root,
                             const std::string &receipt, const std::string &helper) {
    busy = true;
    try {
        std::thread([undo, image, root, receipt, helper]() {
            std::string log = root + "/.steam-shortcut.log";
            const char *argv_add[] = {helper.c_str(), "--steam-shortcut", "--appimage", image.c_str(),
                "--data-root", root.c_str(), "--receipt", receipt.c_str(), NULL};
            const char *argv_undo[] = {helper.c_str(), "--steam-shortcut", "--undo", "--receipt", receipt.c_str(), NULL};
            PlatProc p = plat_spawn(undo ? argv_undo : argv_add, log.c_str(), false);
            int status = p ? plat_proc_wait(p, true) : -1;
            char message[1200] = {0};
            if (p) {
                FILE *f = fopen(log.c_str(), "rb");
                if (f) { size_t n = fread(message, 1, sizeof message - 1, f); message[n] = 0; fclose(f); }
            }
            if (!message[0]) snprintf(message, sizeof message, "%s", status == 0 ? "Steam shortcut updated." :
                                     "Could not run the Steam shortcut helper.");
            { std::lock_guard<std::mutex> guard(result_lock); result = message; }
            busy = false;
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> guard(result_lock);
        result = "Could not start the Steam shortcut helper.";
        busy = false;
    }
}

void steam_shortcut_draw(float scale) {
    const char *appimage = getenv("APPIMAGE");
    if (!appimage || !*appimage) return;  // an AppImage shortcut must launch the outer file, not its temporary mount
    char helper[4096];
    if (!rt_resource("odysseygen/odysseygen", NULL, helper, sizeof helper)) return;
    std::string root = rt_data_root(), receipt = root + "/.steam-shortcut-undo.json";
    ImGui::PushID("steam-shortcut");
    ImGui::Spacing();
    ImGui::SeparatorText("Steam");
    ImGui::TextWrapped("Close Steam before adding the shortcut. Start it again afterwards.");
    bool working = busy.load();
    ImGui::BeginDisabled(working);
    if (ImGui::Button("Add to Steam", ImVec2(250 * scale, 0)))
        shortcut_request(false, appimage, root, receipt, helper);
    struct stat st;
    if (stat(receipt.c_str(), &st) == 0) {
        if (ImGui::Button("Undo Steam shortcut change", ImVec2(330 * scale, 0)))
            shortcut_request(true, appimage, root, receipt, helper);
    }
    ImGui::EndDisabled();
    if (working) ImGui::TextUnformatted("Updating Steam shortcut...");
    std::string message;
    { std::lock_guard<std::mutex> guard(result_lock); message = result; }
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    ImGui::PopID();
}
#else
void steam_shortcut_draw(float scale) { (void)scale; }
#endif
