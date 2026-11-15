// The setup's background (runtime/ui/backdrop.mm): an original pixel-art scene drawn with rectangles - a sky with
// drifting clouds, rounded hills, green pipes, a brick ground, floating gold blocks and coins. Scales with the window
// (crisp at any size), no image files.
#pragma once
#include "imgui.h"

// The scene over the rectangle [p0, p0 + size), cropped to fill it; t: seconds (the animation).
void backdrop_draw(ImDrawList *dl, ImVec2 p0, ImVec2 size, double t);
