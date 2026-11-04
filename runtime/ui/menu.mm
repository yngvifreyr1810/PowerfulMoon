// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <vector>

#include "imgui.h"
#include "option_rows.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#include "theme.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
#endif

extern "C" {
#include "../hle.h"
#include "../mods.h"
#include "../settings.h"
#include "menu.h"
#include "setup.h"
void input_reset(void);
void controls_bindings_draw(float scale);  // controls_ui.mm
#include "../input_bind.h"
#include "../controller_motion.h"
#include "../paths.h"
#include "../update_launcher.h"
#include "../release_info.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_setup{false};  // the first-start setup page (setup.mm) instead of the menu
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_fullscreen_apply{false};
static std::atomic<bool> g_want_update{false};

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
void menu_setup_mode(bool on) { g_setup.store(on); }
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("ODYSSEY_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("ODYSSEY_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}

// Nintendo layout: the right face button (A) confirms, the bottom one (B) goes back.
static ImGuiKey key_from_pad(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceDown;   // activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceRight; // cancel / back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + A, B deletes, Start / + OK) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
// A notice the game thread waits on in menu_notice_run, like the name entry (runtime/hle/applets.c: a system applet
// this version has nothing to show for)
static std::atomic<int> g_note{0};  // 1 shown, 2 closed
static char g_note_title[128], g_note_text[800];
static bool capturing() { return g_open.load() || g_kbd.load() == 1 || g_note.load() == 1 || g_setup.load(); }
// (frame interpolation's presenter, runtime/nvn/vulkan/device.c, draws the overlay without the render lock) nothing open
// that takes input or runs the menu's actions, and the overlay drawn once (ImGui uploads its fonts on the first frame)
static std::atomic<bool> g_drawn;
extern "C" bool menu_overlay_lockfree(void) { return !capturing() && (!g_settings.show_fps || g_drawn.load()); }
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    static bool back_down, start_down;
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    if (g_note.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&  // Enter / Space / Esc: OK
        (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER ||
         e->key.scancode == SDL_SCANCODE_SPACE || e->key.scancode == SDL_SCANCODE_ESCAPE)) {
        g_note = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && !g_setup.load() && (e->key.scancode == SDL_SCANCODE_F1 ||
                                                  (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            g_want_fullscreen_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.scancode == SDL_SCANCODE_ESCAPE) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && !g_setup.load() && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = key_from_pad(e->gbutton.button);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

void menu_apply_window_requests(void *win) {
    static bool update_checked = false;
    if (!update_checked) {
        update_checked = true;
        if (g_settings.check_updates && !getenv("ODYSSEY_HIDDEN")) pm_open_updater(true);
    }
    if (g_want_update.exchange(false)) pm_open_updater(false);
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    if (first || g_want_fullscreen_apply.exchange(false)) {
        first = false;
        SDL_SetWindowFullscreen(w, g_settings.fullscreen);
    }
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours (another UI would have its own context)
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Roboto Medium); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The menu opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
void menu_tabs_focus(bool first_tab) { g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0; }
int g_tab_request_ext = -1;

// The look (theme.h): charcoal, white text, red as the one accent; square-ish corners; tabs are plain words with a red
// line under the selected one (tabs_body draws it).
static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 8; s.FrameRounding = 4; s.GrabRounding = 4; s.TabRounding = 0; s.ChildRounding = 6;
    s.PopupRounding = 6; s.ScrollbarRounding = 4;
    s.WindowPadding = ImVec2(32, 26); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 14; s.TabBarBorderSize = 0;
    ImVec4 *c = s.Colors, none(0, 0, 0, 0);
    c[ImGuiCol_WindowBg] = kPanelV;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = ImVec4(kPanelV.x, kPanelV.y, kPanelV.z, 0.99f);
    c[ImGuiCol_Border] = kLineV;
    c[ImGuiCol_Text] = kTextV;
    c[ImGuiCol_TextDisabled] = kTextDimV;
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = c[ImGuiCol_Header] = c[ImGuiCol_TableHeaderBg] = kSurfaceV;
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = kSurfaceHoverV;
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = kAccentDeepV;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabSelected] = c[ImGuiCol_TabSelectedOverline] = none;
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabDimmedSelectedOverline] = none;
    c[ImGuiCol_TabHovered] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrabActive] = c[ImGuiCol_NavCursor] = c[ImGuiCol_ScrollbarGrabActive] = kAccentV;
    c[ImGuiCol_SliderGrab] = ImVec4(kAccentV.x, kAccentV.y, kAccentV.z, 0.75f);
    c[ImGuiCol_Separator] = kLineV;
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.22f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    for (const char *p : paths) {
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg);
        if (g_font && !g_font_big) { ImFontConfig b = cfg; b.FontNo = 0; g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b); }
    }
    // the menu window itself: Roboto Medium (third_party/imgui/misc/fonts, Apache 2.0); the FPS counter, overlays and
    // on-screen keyboard keep the fonts above
    char menu_font[1024];
    if (const char *f = getenv("ODYSSEY_MENU_FONT")) strlcpy(menu_font, f, sizeof menu_font);  // (test) another face
    if (getenv("ODYSSEY_MENU_FONT") ||
        rt_resource("Roboto-Medium.ttf", "third_party/imgui/misc/fonts/Roboto-Medium.ttf", menu_font, sizeof menu_font)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(menu_font, 36, &cfg);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(menu_font, 58, &cfg);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        }
    }
}

// Section header: in the accent, capitals, as the option rows' sections (option_rows.mm opt_section).
static void header(const char *t) {
    char up[96];
    int i = 0;
    for (; t[i] && i < (int)sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)t[i]);
    up[i] = 0;
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(kAccentV, "%s", up);
    ImGui::Separator();
    ImGui::Spacing();
}

static void hint(const char *t) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", t);
    ImGui::PopTextWrapPos();
}

// A row with a label on the left and a choice of options on the right (dpad left/right cycles).
// reserve: room kept free right of the combo (a button on the same line)
static bool choice(const char *label, int *value, const char *const *names, int n, float reserve = 0) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    bool changed = false;
    ImGui::SetNextItemWidth(reserve > 0 ? -reserve : -1);
    if (ImGui::BeginCombo("##c", names[*value])) {
        for (int i = 0; i < n; i++) {
            if (ImGui::Selectable(names[i], i == *value)) { *value = i; changed = true; }
            if (i == *value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemFocused() && !ImGui::IsPopupOpen("##c")) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            *value = (*value + 1) % n; changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            *value = (*value + n - 1) % n; changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

static bool toggle(const char *label, bool *v) {
    const char *names[] = {"Off", "On"};
    int i = *v;
    bool ch = choice(label, &i, names, 2);
    *v = i;
    return ch;
}

extern "C" void renderer_get_scale(float *sx, float *sy);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    bool fs = g_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.")) {
        g_settings.fullscreen = fs;
        g_want_fullscreen_apply.store(true);
    }
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");
#ifndef __APPLE__  // (Vulkan: runtime/hle/odyssey_wide.c)
    {
        static const char *asp[] = {"16:9", "Fill window"};
        opt_choice("Aspect ratio", &g_settings.aspect, asp, 2, ASPECT_16_9,
                   "For windows wider than 16:9 (ultrawide monitors). 16:9: the Switch's picture, with bars at the sides. "
                   "Fill window: the game shows more of the world at the sides; the HUD keeps its shape in the middle.");
    }
#endif

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1x", "2x", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT;
        for (int i = 0; i < NRES; i++) if (res_mode[i] == g_settings.res_mode) ri = i;
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[400];
        int n = snprintf(d, sizeof d, "How sharp the 3D image is. Match display: your screen's own resolution. Dynamic: "
                                      "as sharp as your GPU keeps at full speed. 1x-4x: 1080p times that (2x: 4K; the "
                                      "Switch renders 1600 x 900). 75%% / 50%% / 25%%: a share of the display, for "
                                      "slower machines.\n\nRendering at %.0f x %.0f.", 1600.0 * sx, 900.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, " Dynamic: %.0f%% of the display right now.", output_scale_dynamic_factor() * 100);
        if (opt_choice("Internal resolution", &ri, res, NRES, RES_MATCH_AT, d, false)) g_settings.res_mode = res_mode[ri];
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        static const char *ups[] = {"Bilinear", "MetalFX"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
#else
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.";
#endif
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[512];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\nNot used at Match display: the game already renders at your display's resolution.", ud);
            ud = udm;
            opt_disable_next();
        }
        if (opt_choice("Upscaler", &ui, ups, 2, 1, ud)) g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR;
    }
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");
#ifndef __APPLE__  // (Vulkan: runtime/nvn/interp.c)
    opt_toggle("Frame interpolation", &g_settings.frame_interp, false,
               "Experimental. An extra image between each two of the game's frames: the game runs at 60, the picture moves "
               "at 120 - for displays at 120 Hz or more (nothing changes below 90 Hz). The extra images come from the game's own "
               "state halfway (positions, camera, animation), not from blending pictures. Costs about twice the GPU work and "
               "half a frame of delay.");
    static const char *interp_outputs[]={"Auto","120 fps (VRR)"};
    int output=g_settings.interpolation_output==120;
    if (opt_choice("Interpolation output",&output,interp_outputs,2,1,
        "Auto uses the current/desktop refresh rate. 120 fps (VRR) keeps interpolation enabled when a high-refresh "
        "display is temporarily reported at 60 Hz with VRR. Requires a mode of at least 120 Hz at the same resolution. "
        "Game logic stays at 60 Hz; GPU load and the interpolation safety brake can still reduce output."))
        g_settings.interpolation_output=output ? 120 : 0;
    static const char *vsync_modes[]={"Auto","On","Off"};
    opt_choice("VSync",&g_settings.vsync,vsync_modes,3,0,
        "Host presentation only. Auto: low-latency mailbox. On: FIFO. Off: immediate, which may tear outside the VRR range. "
        "Restart the game after changing this option. VRR remains controlled by Windows and the graphics driver.");
#endif
#ifndef __APPLE__  // (Vulkan: nvn/vulkan/textures.c "Banding")
    opt_toggle("Smooth gradients", &g_settings.deband, true,
               "Removes the visible color steps (banding) in skies, fog and dark areas: the game's image keeps more "
               "precision until it reaches the screen and gets a fine dither there. Off: as on the Switch.");
#endif
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Reverb", &g_settings.reverb, true,
               "The game's echo effect in caves and big rooms.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's videos (e.g. the control tips). Applies the next time a video starts.");
    opt_end();
}

// Mods (runtime/mods.h): the folders in mods/ with game files, each on or off (applies the next time the game starts).
static void tab_mods(float scale) {
    const char *how = "Put each mod in its own folder in here, with its romfs folder inside - as mods for the Switch "
                      "emulators come: mods/<Mod name>/romfs/...\n\nMods made for version 1.0.0 of the game work best. "
                      "If two mods change the same file, the one higher in the list wins.\n\nCode mods (exefs: IPS / "
                      "pchtxt patches, Super Mario Odyssey: Online) can't be used: the game code is recompiled from the "
                      "original game on your PC.";
    opt_begin("##mods", scale);
    int n = mods_count();
    opt_section(mods_changed() ? "Mods - restart the game to apply" : "Mods");
    if (!n) {
        opt_disable_next();
        opt_action("No mods found", "", how);
    }
    for (int i = 0; i < n; i++) {
        const ModInfo *m = mods_get(i);
        char d[1600];
        int k = snprintf(d, sizeof d, "mods/%s\n\n", m->id);
        if (!m->romfs) {
            snprintf(d + k, sizeof d - k, "A code mod: it changes the game code, which can't be used here - the game "
                                          "code is recompiled from the original game on your PC.");
        } else {
            k += snprintf(d + k, sizeof d - k, "%d file%s", m->files, m->files == 1 ? "" : "s");
            if (m->applied)
                k += snprintf(d + k, sizeof d - k, ": %d replace%s the game's, %d %s new.", m->replaced,
                              m->replaced == 1 ? "s" : "", m->added, m->added == 1 ? "is" : "are");
            else
                k += snprintf(d + k, sizeof d - k, ".");
            if (m->applied && m->shadowed)
                k += snprintf(d + k, sizeof d - k, " %d %s also in a mod higher in this list, which wins.", m->shadowed,
                              m->shadowed == 1 ? "is" : "are");
            if (m->applied && m->skipped)
                k += snprintf(d + k, sizeof d - k, " %d %s in %s the game doesn't have, left out.", m->skipped,
                              m->skipped == 1 ? "is" : "are", m->skipped == 1 ? "a folder" : "folders");
            if (m->code) k += snprintf(d + k, sizeof d - k, " Its code patches (exefs) can't be used here.");
            snprintf(d + k, sizeof d - k, "\n\nSwitching it on or off applies the next time the game starts.");
        }
        bool on = m->romfs && m->on;
        ImGui::PushID(i);  // (two mods may have the same name)
        if (!m->romfs) opt_disable_next();
        if (opt_toggle(m->name, &on, true, d)) mods_set_on(i, on);
        ImGui::PopID();
    }
    opt_section("Folder");
    char url[1600];
    if (opt_action("Open mods folder", "", how) && mods_dir_url(url, sizeof url)) SDL_OpenURL(url);
    opt_end();
}

// Quit: out to the desktop, after asking (progress since the last save is lost); B / Esc or Cancel backs out.
static void tab_quit(float scale, bool in_game) {
    static bool ask;
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = false;
    const char *d = in_game ? "Closes " RT_APP_NAME ".\n\nProgress since your last save is lost." : "Closes " RT_APP_NAME ".";
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (!ask) {
        if (opt_action("Quit to Desktop", "", d)) ask = true;
    } else {
        if (opt_action("Yes, quit to Desktop", "", d)) {
            settings_save();
            SDL_Event q = {};
            q.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&q);
            ask = false;
        }
        if (opt_action("Cancel", "", "Stays where you are.")) ask = false;
    }
    opt_end();
}

static void tab_about(void) {
#ifdef _WIN32
    ImGui::Text("PowerfulMoon %s", POWERFULMOON_VERSION);
    if (ImGui::Button("Check for updates")) g_want_update.store(true);
    if (ImGui::Checkbox("Check for approved updates on startup", &g_settings.check_updates)) settings_save();
    ImGui::Separator();
#endif
    header("About PowerfulMoon");
    ImGui::TextWrapped("Customized fork of Power Moon Recomp by zzzarathustra-dev. Maintained by Vinh An Project / yngvifreyr1810. MIT host; separate GPL controller bridge. Not affiliated with Nintendo.");
    if (ImGui::Button("Upstream project")) SDL_OpenURL("https://gitlab.com/zzzarathustra-dev/power-moon-recomp");
    ImGui::Spacing();
    header("System language");
    {
        // what the game is told the console's language is: its text follows this (runtime/settings.c)
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", settings_language_name());
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_names[i];
        if (choice("Language", &g_settings.language, names, N_LANGUAGES)) settings_save();
    }
    hint("The game's text language, as its own system setting. Applies the next time the game starts.");
    ImGui::Spacing();
    header("Controller");
    {
        static const int pct[] = {0, 5, 10, 15, 20, 25, 30};
        static const char *const names[] = {"Off", "5%", "10%", "15%", "20%", "25%", "30%"};
        int i = 0;
        for (int k = 0; k < 7; k++) if (pct[k] <= g_settings.stick_deadzone) i = k;
        if (choice("Stick deadzone", &i, names, 7)) g_settings.stick_deadzone = pct[i];
    }
    hint("How far a controller stick moves before it counts. Raise it if Mario or the camera drifts on their own.");
    ImGui::Spacing();
    header("Controls");
    if (ImGui::Checkbox("Motion controls",&g_settings.motion)) settings_save();
    ImGui::SameLine();ImGui::TextDisabled("(%s)",pm_motion_available()?"sensor available":"no sensor connected");
    if (ImGui::Checkbox("Controller vibration",&g_settings.rumble)) settings_save();
    if (ImGui::Checkbox("Direct ProCon2Bridge haptics (experimental)",&g_settings.hd_rumble)) settings_save();
    if (ImGui::SliderFloat("Vibration strength",&g_settings.rumble_strength,0,2,"%.2fx")) settings_save();
    if (ImGui::Button("Test vibration (2 seconds)")) pm_vibration_test();
    ImGui::Spacing();
    controls_bindings_draw(ImGui::GetFontSize() / 34);
    ImGui::Spacing();
    header("Menu");
    ImGui::TextUnformatted("Open / close this menu:  F1, Esc, the controller's Home / Xbox / PlayStation button, or Back + Start");
    ImGui::TextUnformatted("Switch tabs:  L / R  (Q / E)        Back:  B  (Esc)");
    ImGui::TextUnformatted("Fullscreen:  F11 / Alt+Enter");
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted("Enter your name");
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        if ((int)strlen(g_kbd_text) > g_kbd_max) g_kbd_text[g_kbd_max] = 0;
        ImGui::TextDisabled("Enter  confirm      Esc  cancel      (%d / %d)", (int)strlen(g_kbd_text), g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

static void notice_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 140));
    float w = fminf(io.DisplaySize.x * 0.9f, 1000 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##note", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_menu_font_big);
    ImGui::TextUnformatted(g_note_title);
    ImGui::PopFont();
    ImGui::PushFont(g_menu_font);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_note_text);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    static bool was_open;
    if (!was_open) ImGui::SetKeyboardFocusHere(), ImGui::SetNavCursorVisible(true);  // (the controller's cursor on OK)
    was_open = true;
    // OK: clicked, A on it, or any face button / + (only one choice: whichever the player's layout calls A)
    if (ImGui::Button("OK", ImVec2(200 * scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        g_note = 2;
    ImGui::PopFont();
    ImGui::End();
    if (g_note.load() != 1) was_open = false;
}

// Game thread (runtime/hle/applets.c): the notice over the last frame until the player closes it.
extern "C" void menu_notice_run(const char *title, const char *text) {
    extern void backend_represent(void);
    snprintf(g_note_title, sizeof g_note_title, "%s", title);
    snprintf(g_note_text, sizeof g_note_text, "%s", text);
    input_reset();
    g_note = 1;
    while (g_note.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot closes it)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    g_note = 0;
    input_reset();  // (the button that closed it doesn't reach the game)
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) ODYSSEY_KBD_AUTO=<text>: answer at once
    if (getenv("ODYSSEY_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("ODYSSEY_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// The tab bar and its pages, into the current window: the in-game menu (in_game
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: an extra
// first tab, game files and fonts, drawn by launcher.mm).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[11];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    // (ids: the pages below; 3-7 were Link's Awakening's and stay unused, so ODYSSEY_MENU_TAB numbers keep meaning)
    static const char *names[] = {"Video", "Audio", "Mods", NULL, NULL, NULL, NULL, NULL, "Controls", "Quit"};
    static const int order[] = {8, 0, 1, 2, 9};  // Controls, Video, Audio, Mods, Quit
    for (int i : order)
        if (i != 9 || !prelaunch) tabs[ntabs++] = {names[i], i, true};
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor on the tab bar goes along (left behind, A on it switched straight back; user 2026-10-11)
            if (focused_tab != -99) g_focus_tab = tabs[i].id, g_focus_frames = 0;
        }
    }
    // Down with the cursor on the tab bar: the page's first row (option rows; other pages keep ImGui's own pick)
    bool down_from_tab = focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible && !any_popup &&
                         (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    opt_focus_first(down_from_tab);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[64];
            snprintf(lbl, sizeof lbl, "  %s  ", t.name);
            ImGui::BeginDisabled(!t.on);
            // The tabs are their names (the shown one white, the others dim) with a red line under the shown one.
            // Focus (controller / keyboard): the tab lights up a little and gets a dim line, instead of ImGui's
            // outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, k == cur || lit ? kTextV : kTextDimV);
            int pushed = 2;
            if (lit) {
                for (ImGuiCol c : {ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected}) ImGui::PushStyleColor(c, ImVec4(1, 1, 1, 0.07f));
                pushed += 3;
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(pushed);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (open || lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 3 * scale > 2 ? 3 * scale : 2, m = 10 * scale;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + m, b.y - h), ImVec2(b.x - m, b.y), open ? kAccent : kAccentDim);
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                // Down from the tab bar, pages that aren't option rows (Controls): their first control
                // (option-row pages: opt_focus_first)
                if (down_from_tab && (t.id == -1 || t.id == 8)) {
                    opt_focus_first(false);
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 1: tab_audio(); break;
                case 2: tab_mods(scale); break;
                case 8: tab_about(); break;
                case 9: tab_quit(scale, in_game); break;
                }
                static const char *scroll_test = getenv("ODYSSEY_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first(false);  // (a page without option rows: nothing took it)
        ImGui::EndTabBar();
    }
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (another UI may have its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load(), setup = g_setup.load();
    bool kbd = g_kbd.load() == 1, note = g_note.load() == 1;
    if (!open && !kbd && !note && !setup && !g_settings.show_fps) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    float scale = (float)(th / 1080.0);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        style_scale = scale;
    }
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    if (setup) {
        setup_draw(scale, g_menu_font, g_menu_font_big);
        open = false;
    } else if (g_settings.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps", io.Framerate);
        ImGui::End();
    }

    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 130));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header: the crescent and the name, a close X in the corner (B / Esc too), a hairline under them
        ImGui::PushFont(g_font_big);
        {
            float lh = ImGui::GetFontSize(), sz = lh * 1.35f;
            ImVec2 lp = ImGui::GetCursorScreenPos();
            // (its disc spans the middle 2/3 of the square: the disc's left edge on the text's, centred on the line)
            theme_crescent(ImGui::GetWindowDrawList(), ImVec2(lp.x - sz / 6, lp.y + lh * 0.5f - sz * 0.5f), sz, kAccent,
                           theme_u32(kPanelV, 1));
            ImGui::Dummy(ImVec2(sz * 2 / 3, lh));
            ImGui::SameLine(0, lh * 0.4f);
            ImGui::TextUnformatted(RT_APP_NAME);
        }
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? kAccent : theme_u32(kTextV, 0.85f);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), theme_u32(kSurfaceHoverV), 4 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }
        ImGui::Separator();

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    if (note && !open) notice_draw(io, scale);  // (the menu opened over it: the notice waits behind it)
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
#endif
    g_drawn = true;
}
