// Canvas.cpp - see Canvas.h.
#include <ui/Canvas.h>

#include <math.h>
#include <string.h>

namespace ui_tft {

void Canvas::fill(Rect r, uint16_t colour) {
  const int x0 = r.x > win_.x ? r.x : win_.x, y0 = r.y > win_.y ? r.y : win_.y;
  const int x1 = r.x + r.w < win_.x + win_.w ? r.x + r.w : win_.x + win_.w;
  const int y1 = r.y + r.h < win_.y + win_.h ? r.y + r.h : win_.y + win_.h;
  for (int y = y0; y < y1; y++) {
    uint16_t* row = px_ + (y - win_.y) * win_.w - win_.x;
    for (int x = x0; x < x1; x++) row[x] = colour;
  }
}

void Canvas::roundRect(Rect r, int radius, uint16_t colour) {
  if (radius * 2 > r.h) radius = r.h / 2;
  if (radius * 2 > r.w) radius = r.w / 2;
  if (radius <= 0) return fill(r, colour);
  if (!r.overlaps(win_)) return;
  fill({r.x, (int16_t)(r.y + radius), r.w, (int16_t)(r.h - 2 * radius)}, colour);
  fill({(int16_t)(r.x + radius), r.y, (int16_t)(r.w - 2 * radius), (int16_t)radius}, colour);
  fill({(int16_t)(r.x + radius), (int16_t)(r.y + r.h - radius), (int16_t)(r.w - 2 * radius), (int16_t)radius}, colour);
  // The four corners: coverage from the distance to the corner's centre.
  for (int j = 0; j < radius; j++) {
    const int yTop = r.y + j, yBot = r.y + r.h - 1 - j;
    if ((yTop < win_.y || yTop >= win_.y + win_.h) && (yBot < win_.y || yBot >= win_.y + win_.h)) continue;
    for (int i = 0; i < radius; i++) {
      const float dx = (float)(radius - i) - 0.5f, dy = (float)(radius - j) - 0.5f;
      const float cover = (float)radius - sqrtf(dx * dx + dy * dy) + 0.5f;
      const int q = cover >= 1.0f ? 4 : (cover <= 0.0f ? 0 : (int)(cover * 4.0f + 0.5f));
      if (!q) continue;
      const int xl = r.x + i, xr = r.x + r.w - 1 - i;
      blend(xl, yTop, colour, q);
      blend(xr, yTop, colour, q);
      blend(xl, yBot, colour, q);
      blend(xr, yBot, colour, q);
    }
  }
}

int Canvas::text(const Font& f, int x, int y, const char* s, uint16_t colour) {
  const bool rows = y < win_.y + win_.h && y + f.height > win_.y;
  for (; *s; s++) {
    const Glyph* g = f.glyph(*s);
    if (!g) continue;
    const int gx = x + g->dx, gy = y + g->dy;
    if (rows && g->w && gx < win_.x + win_.w && gx + g->w > win_.x) {
      const uint8_t* bits = f.bits + g->offset;
      for (int j = 0; j < g->h; j++) {
        const int py = gy + j;
        if (py < win_.y || py >= win_.y + win_.h) continue;
        int n = j * g->w;
        for (int i = 0; i < g->w; i++, n++) {
          const int a = (bits[n >> 2] >> ((n & 3) * 2)) & 3;
          if (a) blend(gx + i, py, colour, a == 3 ? 4 : a + (a >> 1));  // 1, 2, 3 -> 1, 3, 4 quarters
        }
      }
    }
    x += g->advance;
  }
  return x;
}

int Canvas::textWidth(const Font& f, const char* s) {
  int w = 0;
  for (; *s; s++) {
    const Glyph* g = f.glyph(*s);
    if (g) w += g->advance;
  }
  return w;
}

bool Canvas::fitText(const Font& f, const char* s, int maxW, char* out, int n) {
  strncpy(out, s, n - 1);
  out[n - 1] = 0;
  if (textWidth(f, out) <= maxW) return false;
  int len = (int)strlen(out);
  while (len > 0) {
    len--;
    while (len > 0 && out[len - 1] == ' ') len--;  // no space before the dots
    if (len + 3 > n) continue;
    out[len] = '.';
    out[len + 1] = '.';
    out[len + 2] = 0;
    if (textWidth(f, out) <= maxW) return true;
  }
  out[0] = 0;
  return true;
}

void Canvas::textIn(Rect r, const Font& f, const char* s, uint16_t colour, Align align) {
  if (!r.overlaps(win_)) return;
  char buf[64];
  fitText(f, s, r.w, buf, sizeof(buf));
  const int w = textWidth(f, buf);
  const int x = align == kLeft ? r.x : (align == kRight ? r.x + r.w - w : r.x + (r.w - w) / 2);
  // Centred on the capital letters' height, which is what the eye takes as the middle.
  const int cap = f.ascent * 73 / 100;
  text(f, x, r.y + (r.h - cap) / 2 - (f.ascent - cap), buf, colour);
}

}  // namespace ui_tft
