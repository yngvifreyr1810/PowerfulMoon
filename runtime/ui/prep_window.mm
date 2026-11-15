// The shader preparation's window (runtime/nvn/vulkan/device.c prep_run): drawn before the game's Vulkan device exists,
// with SDL's renderer - but in the menu's look (theme.h) and font (Roboto Medium), through a Dear ImGui context of its
// own whose triangles go to SDL_RenderGeometry.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <vector>
#include "imgui.h"
#include "theme.h"

extern "C" {
#include "../paths.h"
#include "../rt.h"
}

static ImGuiContext *g_pctx;
static SDL_Texture *g_font_tex;
static SDL_Renderer *g_r;
static ImFont *g_body, *g_title;

static void prep_init(SDL_Renderer *r) {
    g_r = r;
    ImGuiContext *keep = ImGui::GetCurrentContext();
    g_pctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_pctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    char font[1024];
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    if (rt_resource("Roboto-Medium.ttf", "third_party/imgui/misc/fonts/Roboto-Medium.ttf", font, sizeof font)) {
        g_body = io.Fonts->AddFontFromFileTTF(font, 19, &cfg);
        g_title = io.Fonts->AddFontFromFileTTF(font, 30, &cfg);
    }
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    if (!g_title) g_title = g_body;
    unsigned char *px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    g_font_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (g_font_tex) {
        SDL_UpdateTexture(g_font_tex, NULL, px, w * 4);
        SDL_SetTextureBlendMode(g_font_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_font_tex, SDL_SCALEMODE_LINEAR);
    }
    io.Fonts->SetTexID((ImTextureID)(intptr_t)g_font_tex);
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_Text] = kTextV;
    s.WindowBorderSize = 0;
    ImGui::SetCurrentContext(keep);
}

// ImGui's triangles to SDL (colours as floats, indices as ints: SDL_RenderGeometry's)
static void prep_render(ImDrawData *dd) {
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList *l = dd->CmdLists[n];
        v.resize(l->VtxBuffer.Size);
        for (int i = 0; i < l->VtxBuffer.Size; i++) {
            const ImDrawVert &d = l->VtxBuffer[i];
            v[i].position = {d.pos.x, d.pos.y};
            v[i].tex_coord = {d.uv.x, d.uv.y};
            v[i].color = {(d.col & 0xFF) / 255.0f, (d.col >> 8 & 0xFF) / 255.0f, (d.col >> 16 & 0xFF) / 255.0f, (d.col >> 24) / 255.0f};
        }
        for (const ImDrawCmd &c : l->CmdBuffer) {
            if (c.UserCallback) continue;
            SDL_Rect clip = {(int)c.ClipRect.x, (int)c.ClipRect.y, (int)(c.ClipRect.z - c.ClipRect.x), (int)(c.ClipRect.w - c.ClipRect.y)};
            SDL_SetRenderClipRect(g_r, &clip);
            idx.resize(c.ElemCount);
            for (unsigned i = 0; i < c.ElemCount; i++) idx[i] = (int)(c.VtxOffset + l->IdxBuffer[c.IdxOffset + i]);
            SDL_RenderGeometry(g_r, (SDL_Texture *)(intptr_t)c.GetTexID(), v.data(), (int)v.size(), idx.data(), (int)idx.size());
        }
    }
    SDL_SetRenderClipRect(g_r, NULL);
}

extern "C" void prep_window_draw(SDL_Renderer *r, const char *phase, int done, int total, float sec) {
    if (!g_pctx) prep_init(r);
    ImGuiContext *keep = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_pctx);
    ImGuiIO &io = ImGui::GetIO();
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(r, &w, &h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = 1.0f / 20;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##prep", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float pad = 26;
    ImGui::SetCursorPos(ImVec2(pad, pad - 4));
    ImGui::PushFont(g_title);
    {   // the icon's pipe, then the title
        float lh = ImGui::GetFontSize(), sz = lh * 1.1f;
        ImVec2 lp = ImGui::GetCursorScreenPos();
        theme_crescent(dl, ImVec2(lp.x, lp.y + lh * 0.5f - sz * 0.55f), sz, kAccent, theme_u32(kPanelV, 1));
        ImGui::Dummy(ImVec2(sz, lh));
        ImGui::SameLine(0, lh * 0.35f);
        ImGui::TextUnformatted(total ? phase : "Preparing shaders");
    }
    ImGui::PopFont();
    ImGui::PushFont(g_body);
    ImGui::SetCursorPosX(pad);
    char line[96];
    int pct = total ? (int)(100.0 * done / total) : 0;
    snprintf(line, sizeof line, "%d%%     %d / %d     %.0f s", pct, done, total, sec);
    ImGui::TextUnformatted(line);
    ImGui::Dummy(ImVec2(0, 6));
    {   // the bar: the menu's row surface, filled white, a hairline round it
        ImVec2 p(pad, ImGui::GetCursorScreenPos().y);
        float bw = io.DisplaySize.x - 2 * pad, bh = 14, f = total ? (float)done / (float)total : 0;
        dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bh), theme_u32(kSurfaceV), 4);
        if (f > 0) dl->AddRectFilled(p, ImVec2(p.x + bw * f, p.y + bh), theme_u32(kTextV, 0.9f), 4);
        dl->AddRect(p, ImVec2(p.x + bw, p.y + bh), theme_u32(kLineV), 4);
        ImGui::Dummy(ImVec2(bw, bh));
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDimV);
    ImGui::SetCursorPosX(pad);
    ImGui::TextUnformatted("Once only: the game starts when this ends.");
    ImGui::SetCursorPosX(pad);
    ImGui::TextUnformatted("Closing this window skips it (stutters while playing).");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::End();
    ImGui::Render();
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);  // (the menu's black)
    SDL_RenderClear(r);
    prep_render(ImGui::GetDrawData());
    ImGui::SetCurrentContext(keep);
}

extern "C" void prep_window_close(void) {
    if (!g_pctx) return;
    ImGuiContext *keep = ImGui::GetCurrentContext();
    ImGui::DestroyContext(g_pctx);
    ImGui::SetCurrentContext(keep == g_pctx ? NULL : keep);
    if (g_font_tex) SDL_DestroyTexture(g_font_tex);
    g_pctx = NULL, g_font_tex = NULL, g_r = NULL, g_body = g_title = NULL;
}
