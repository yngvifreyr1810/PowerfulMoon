// Power Moon Recomp's look (runtime/ui): one palette for the menu, its option rows, the Controls page, the on-screen
// keyboard - and the icon (tools/make_icon.py: PANEL, ACCENT). Charcoal panels, white text, one red accent.
#pragma once
#include "imgui.h"

#define THEME_RGB(r, g, b, a) ImVec4((r) / 255.0f, (g) / 255.0f, (b) / 255.0f, (a))
// the palette
static const ImVec4 kPanelV = THEME_RGB(18, 19, 22, 0.97f);         // the menu window (the icon's background, opaque)
static const ImVec4 kSurfaceV = THEME_RGB(30, 31, 36, 1);           // fields, buttons, rows
static const ImVec4 kSurfaceHoverV = THEME_RGB(42, 43, 49, 1);      // ... under the mouse / the cursor
static const ImVec4 kAccentV = THEME_RGB(229, 56, 59, 1);           // the red (the icon's crescent)
static const ImVec4 kAccentDeepV = THEME_RGB(92, 23, 25, 1);        // ... pressed / active fields
static const ImVec4 kAccentSoftV = THEME_RGB(255, 148, 148, 1);     // ... as text: a value that isn't the default
static const ImVec4 kTextV = THEME_RGB(242, 242, 245, 1);
static const ImVec4 kTextDimV = THEME_RGB(150, 152, 160, 1);        // secondary text, tabs not shown
static const ImVec4 kTextOffV = THEME_RGB(98, 100, 108, 1);         // greyed out
static const ImVec4 kLineV = THEME_RGB(255, 255, 255, 0.10f);       // separators
static const ImVec4 kWarnV = THEME_RGB(168, 110, 24, 1);            // a conflict (one key bound twice)

static inline ImU32 theme_u32(ImVec4 c, float alpha = -1) {
    if (alpha >= 0) c.w = alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}
#define kAccent theme_u32(kAccentV)
#define kAccentDim theme_u32(kAccentV, 0.43f)
#define kText theme_u32(kTextV)
#define kTextDim theme_u32(kTextDimV)
#define kTextOff theme_u32(kTextOffV)

// The icon's crescent (tools/make_icon.py) in a square of `size` at p: a disc centred in it, its diameter 2/3 of the
// square, minus the same disc moved half its radius right and up (painted in `bg`, the colour behind it).
static inline void theme_crescent(ImDrawList *dl, ImVec2 p, float size, ImU32 col, ImU32 bg) {
    float r = size / 3, cx = p.x + size / 2, cy = p.y + size / 2;
    dl->AddCircleFilled(ImVec2(cx, cy), r, col, 96);
    dl->AddCircleFilled(ImVec2(cx + r / 2, cy - r / 2), r, bg, 96);
}
