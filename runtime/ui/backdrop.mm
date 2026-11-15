// The setup's pixel-art background (backdrop.h). A virtual 320 x 180 pixel scene, scaled to cover the window and
// centred; every shape is made of whole virtual pixels, so it stays pixel art at any size. Original artwork for this
// project (no game assets).
#include "backdrop.h"

#include <math.h>

namespace {

const int W = 320, H = 180, GROUND = 148;  // the ground's top row

struct Px {
    ImDrawList *dl;
    float s, ox, oy;  // scale, origin
    void rect(float x, float y, float w, float h, ImU32 c) const {
        if (w <= 0 || h <= 0) return;
        dl->AddRectFilled(ImVec2(ox + floorf(x) * s, oy + floorf(y) * s), ImVec2(ox + floorf(x + w) * s, oy + floorf(y + h) * s), c);
    }
    // a pixelated disc: row by row, centre (cx, cy), radius r; only the rows above cy + cut
    void disc(float cx, float cy, float r, ImU32 c, float cut = 1e9f) const {
        for (int dy = -(int)r; dy <= (int)r && dy <= cut; dy++) {
            float hw = floorf(sqrtf(fmaxf(r * r - (float)(dy * dy), 0)) + 0.5f);
            rect(cx - hw, cy + dy, 2 * hw, 1, c);
        }
    }
};

ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

// palette
const ImU32 SKY_TOP = rgb(92, 148, 238), SKY_LOW = rgb(132, 186, 255), SKY_BAND = rgb(112, 168, 248);
const ImU32 CLOUD = rgb(252, 252, 255), CLOUD_SHADE = rgb(196, 220, 255), CLOUD_EDGE = rgb(150, 190, 245);
const ImU32 HILL_FAR = rgb(96, 170, 104), HILL_FAR_EDGE = rgb(64, 128, 76);
const ImU32 HILL = rgb(64, 176, 72), HILL_EDGE = rgb(24, 96, 40), HILL_SPOT = rgb(36, 128, 52);
const ImU32 PIPE = rgb(40, 168, 64), PIPE_LIGHT = rgb(140, 228, 120), PIPE_DARK = rgb(16, 96, 40), PIPE_EDGE = rgb(8, 48, 20);
const ImU32 BRICK = rgb(196, 92, 40), BRICK_LIGHT = rgb(236, 148, 92), BRICK_DARK = rgb(96, 40, 16);
const ImU32 GOLD = rgb(240, 176, 48), GOLD_LIGHT = rgb(255, 228, 140), GOLD_DARK = rgb(160, 96, 16), RIVET = rgb(120, 64, 8);
const ImU32 BUSH = rgb(72, 200, 64), BUSH_EDGE = rgb(24, 112, 32), BUSH_LIGHT = rgb(150, 236, 120);

void cloud(const Px &p, float x, float y, float k) {
    // three puffs on a flat bottom, a lighter underside, a soft outline
    float r1 = 7 * k, r2 = 10 * k, r3 = 7 * k;
    float base = y + 6 * k;
    p.disc(x, y + 1, r1 + 1, CLOUD_EDGE, base - y - 1);
    p.disc(x + 11 * k, y - 3 * k + 1, r2 + 1, CLOUD_EDGE, base - (y - 3 * k) - 1);
    p.disc(x + 22 * k, y + 1, r3 + 1, CLOUD_EDGE, base - y - 1);
    p.disc(x, y, r1, CLOUD, base - y);
    p.disc(x + 11 * k, y - 3 * k, r2, CLOUD, base - (y - 3 * k));
    p.disc(x + 22 * k, y, r3, CLOUD, base - y);
    p.rect(x - r1 + 1, base - 2, 22 * k + r1 + r3 - 2, 2, CLOUD_SHADE);
}

void hill(const Px &p, float cx, float r, ImU32 fill, ImU32 edge, bool spots) {
    p.disc(cx, GROUND, r + 1, edge, 0);
    p.disc(cx, GROUND, r, fill, 0);
    if (!spots) return;
    const float sp[][2] = {{-0.45f, -0.35f}, {0.2f, -0.55f}, {0.4f, -0.2f}, {-0.1f, -0.15f}};
    for (auto &q : sp) {
        float x = cx + q[0] * r, y = GROUND + q[1] * r;
        p.rect(x, y, 2, 4, HILL_SPOT);
        p.rect(x - 1, y + 1, 4, 2, HILL_SPOT);
    }
}

void bush(const Px &p, float x, int puffs) {
    for (int i = 0; i < puffs; i++) p.disc(x + i * 9, GROUND - 1, 7, BUSH_EDGE, 0);
    for (int i = 0; i < puffs; i++) p.disc(x + i * 9, GROUND - 1, 6, BUSH, 0);
    for (int i = 0; i < puffs; i++) p.rect(x + i * 9 - 3, GROUND - 6, 3, 2, BUSH_LIGHT);
}

void pipe(const Px &p, float x, float top) {
    // a body 26 wide from top + 12 to the ground, a lip 32 wide and 12 high; light stripe left, shade right
    float bw = 26, lw = 32, lh = 12, bx = x + (lw - bw) / 2;
    p.rect(bx - 1, top + lh, bw + 2, GROUND - top - lh, PIPE_EDGE);
    p.rect(bx, top + lh, bw, GROUND - top - lh, PIPE);
    p.rect(bx + 3, top + lh, 3, GROUND - top - lh, PIPE_LIGHT);
    p.rect(bx + 8, top + lh, 1, GROUND - top - lh, PIPE_LIGHT);
    p.rect(bx + bw - 7, top + lh, 5, GROUND - top - lh, PIPE_DARK);
    p.rect(x - 1, top - 1, lw + 2, lh + 2, PIPE_EDGE);
    p.rect(x, top, lw, lh, PIPE);
    p.rect(x + 3, top + 1, 3, lh - 2, PIPE_LIGHT);
    p.rect(x + 8, top + 1, 1, lh - 2, PIPE_LIGHT);
    p.rect(x + lw - 7, top + 1, 5, lh - 2, PIPE_DARK);
    p.rect(x, top, lw, 1, PIPE_LIGHT);
    // the opening's dark inside, seen from slightly above
    p.rect(x + 3, top - 1, lw - 6, 1, PIPE_EDGE);
}

void brick_tile(const Px &p, float x, float y) {
    p.rect(x, y, 16, 16, BRICK);
    p.rect(x, y, 16, 1, BRICK_LIGHT);
    for (int row = 0; row < 4; row++) {
        float ry = y + row * 4;
        p.rect(x, ry + 3, 16, 1, BRICK_DARK);  // mortar
        float off = (row & 1) ? 4 : 12;
        p.rect(x + off, ry, 1, 3, BRICK_DARK);
        p.rect(x + off + 1, ry, 1, 3, BRICK_LIGHT);
    }
}

void gold_block(const Px &p, float x, float y) {
    p.rect(x - 1, y - 1, 18, 18, GOLD_DARK);
    p.rect(x, y, 16, 16, GOLD);
    p.rect(x, y, 16, 2, GOLD_LIGHT);
    p.rect(x, y, 2, 16, GOLD_LIGHT);
    p.rect(x + 14, y + 2, 2, 14, GOLD_DARK);
    p.rect(x + 2, y + 14, 14, 2, GOLD_DARK);
    const float rv[][2] = {{3, 3}, {11, 3}, {3, 11}, {11, 11}};
    for (auto &q : rv) p.rect(x + q[0], y + q[1], 2, 2, RIVET);
    // a small star stamped in the middle
    p.rect(x + 7, y + 5, 2, 6, GOLD_DARK);
    p.rect(x + 5, y + 7, 6, 2, GOLD_DARK);
}

void coin(const Px &p, float x, float y, double t) {
    // turning: its width follows the phase
    float w = floorf(6 * fabsf((float)cos(t * 2.2)) + 0.5f);
    if (w < 2) w = 2;
    float cx = x + 3 - w / 2;
    p.rect(cx - 1, y, w + 2, 10, GOLD_DARK);
    p.rect(cx, y + 1, w, 8, GOLD);
    if (w > 3) p.rect(cx + 1, y + 2, 1, 6, GOLD_LIGHT);
}

}  // namespace

void backdrop_draw(ImDrawList *dl, ImVec2 p0, ImVec2 size, double t) {
    float s = fmaxf(size.x / W, size.y / H);
    Px p{dl, s, p0.x + (size.x - W * s) * 0.5f, p0.y + (size.y - H * s) * 0.5f};
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);

    // sky: three flat bands, pixel-art style
    p.rect(-W, -H, 3 * W, H + 60, SKY_TOP);
    p.rect(-W, 60, 3 * W, 40, SKY_BAND);
    p.rect(-W, 100, 3 * W, GROUND - 100, SKY_LOW);

    // clouds drifting right, wrapping around the scene
    const float cl[][3] = {{20, 26, 1.0f}, {120, 16, 1.2f}, {215, 34, 0.9f}, {290, 20, 1.1f}, {70, 52, 0.8f}};
    for (int i = 0; i < 5; i++) {
        float speed = 2.5f + i * 0.6f;
        float x = fmodf(cl[i][0] + (float)t * speed, W + 80) - 40;
        cloud(p, x, cl[i][1], cl[i][2]);
    }

    // far hills, then near hills
    hill(p, 40, 44, HILL_FAR, HILL_FAR_EDGE, false);
    hill(p, 250, 52, HILL_FAR, HILL_FAR_EDGE, false);
    hill(p, 0, 30, HILL, HILL_EDGE, true);
    hill(p, 150, 26, HILL, HILL_EDGE, true);
    hill(p, 318, 34, HILL, HILL_EDGE, true);

    // bushes
    bush(p, 54, 2);
    bush(p, 232, 2);

    // Everything with detail stays beside the installer's card: it covers about x 73..247 of the scene at every window
    // shape (the card is ~173 scene pixels wide), and 16:10 windows crop about 16 at each side.
    // floating blocks: a gold one on the left, brick - gold - brick on the right
    gold_block(p, 34, 70);
    for (int i = 0; i < 3; i++) {
        float x = 252 + i * 16, by = 62;
        if (i & 1) gold_block(p, x, by);
        else { p.rect(x - 1, by - 1, 18, 18, BRICK_DARK); brick_tile(p, x, by); }
    }

    // coins over the blocks, bobbing
    coin(p, 39, 52 + 2 * sinf((float)t * 2), t);
    for (int i = 0; i < 3; i++) coin(p, 257 + i * 16, 44 + 2 * sinf((float)t * 2 + i + 1), t + (i + 1) * 0.4);

    // pipes standing on the ground
    pipe(p, 22, 112);
    pipe(p, 266, 104);

    // the ground: two rows of brick tiles across (and beyond, for wide windows)
    for (int x = -W; x < 2 * W; x += 16) {
        brick_tile(p, (float)x, GROUND);
        brick_tile(p, (float)x, GROUND + 16);
    }
    p.rect(-W, GROUND + 32, 3 * W, 40, BRICK_DARK);

    dl->PopClipRect();
}
