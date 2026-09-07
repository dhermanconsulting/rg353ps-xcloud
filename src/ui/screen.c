#include "screen.h"

#include <string.h>

void screen_clear(uint8_t *luma, uint8_t *chroma, int pitch, int w, int h,
		  uint8_t luma_level)
{
	(void)w;
	memset(luma, luma_level, (size_t)pitch * h);
	if (chroma)
		memset(chroma, 0x80, (size_t)pitch * (h / 2));
}

/* Clip a rectangle to the plane. Returns 0 if nothing is left. */
static int clip(int w, int h, int *x, int *y, int *rw, int *rh)
{
	if (*x < 0) { *rw += *x; *x = 0; }
	if (*y < 0) { *rh += *y; *y = 0; }
	if (*x + *rw > w) *rw = w - *x;
	if (*y + *rh > h) *rh = h - *y;
	return *rw > 0 && *rh > 0;
}

void screen_rect(uint8_t *luma, int pitch, int w, int h, int x, int y,
		 int rw, int rh, uint8_t level)
{
	if (!clip(w, h, &x, &y, &rw, &rh))
		return;
	for (int row = 0; row < rh; row++)
		memset(luma + (size_t)(y + row) * pitch + x, level, rw);
}

void screen_rect_colour(uint8_t *luma, uint8_t *chroma, int pitch, int w,
			int h, int x, int y, int rw, int rh, uint8_t Y,
			uint8_t cb, uint8_t cr)
{
	/* Whole chroma blocks only: a half-covered 2x2 would tint the
	 * neighbour pixel. Round the origin down and the size up to cover. */
	x &= ~1;
	y &= ~1;
	rw = (rw + 1) & ~1;
	rh = (rh + 1) & ~1;
	if (!clip(w, h, &x, &y, &rw, &rh))
		return;
	for (int row = 0; row < rh; row++)
		memset(luma + (size_t)(y + row) * pitch + x, Y, rw);
	if (!chroma)
		return;
	for (int row = 0; row < rh / 2; row++) {
		uint8_t *dst = chroma + (size_t)(y / 2 + row) * pitch + x;

		for (int i = 0; i < rw; i += 2) {
			dst[i] = cb;
			dst[i + 1] = cr;
		}
	}
}

void screen_frame(uint8_t *luma, int pitch, int w, int h, int x, int y,
		  int rw, int rh, int t, uint8_t level)
{
	screen_rect(luma, pitch, w, h, x, y, rw, t, level);
	screen_rect(luma, pitch, w, h, x, y + rh - t, rw, t, level);
	screen_rect(luma, pitch, w, h, x, y + t, t, rh - 2 * t, level);
	screen_rect(luma, pitch, w, h, x + rw - t, y + t, t, rh - 2 * t, level);
}

void screen_blit_nv12(uint8_t *luma, uint8_t *chroma, int pitch, int w,
		      int h, int x, int y, const uint8_t *sluma,
		      const uint8_t *schroma, int spitch, int sw, int sh)
{
	int sx = 0, sy = 0;   /* first source column/row after clipping */

	x &= ~1;
	y &= ~1;
	if (x < 0) { sx = -x; sw += x; x = 0; }
	if (y < 0) { sy = -y; sh += y; y = 0; }
	if (x + sw > w) sw = w - x;
	if (y + sh > h) sh = h - y;
	sw &= ~1;
	sh &= ~1;
	if (sw <= 0 || sh <= 0)
		return;
	for (int row = 0; row < sh; row++)
		memcpy(luma + (size_t)(y + row) * pitch + x,
		       sluma + (size_t)(sy + row) * spitch + sx, (size_t)sw);
	if (!chroma || !schroma)
		return;
	for (int row = 0; row < sh / 2; row++)
		memcpy(chroma + (size_t)(y / 2 + row) * pitch + x,
		       schroma + (size_t)(sy / 2 + row) * spitch + sx,
		       (size_t)sw);
}

void screen_rgb_to_ycc(uint8_t r, uint8_t g, uint8_t b, uint8_t *Y,
		       uint8_t *cb, uint8_t *cr)
{
	/* BT.601, 8.8 fixed point, limited range: what the panel path and
	 * the simulator's PNG writer both assume for this buffer. */
	int y = (66 * r + 129 * g + 25 * b + 128) >> 8;
	int u = (-38 * r - 74 * g + 112 * b + 128) >> 8;
	int v = (112 * r - 94 * g - 18 * b + 128) >> 8;

	*Y = (uint8_t)(y + 16);
	*cb = (uint8_t)(u + 128);
	*cr = (uint8_t)(v + 128);
}
