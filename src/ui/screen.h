/*
 * Helpers for painting an NV12 buffer used as a flat UI surface.
 *
 * The buffer is a DRM dumb mapping: write-combine memory that is fast to
 * fill and painfully slow to read, so nothing here reads the destination.
 * Chroma is one interleaved Cb/Cr pair per 2x2 luma block, which is why the
 * colour calls round positions and sizes down to even numbers.
 */
#ifndef XCLOUD_SCREEN_H
#define XCLOUD_SCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fill luma with `luma_level` and reset chroma to neutral grey. */
void screen_clear(uint8_t *luma, uint8_t *chroma, int pitch, int w, int h,
		  uint8_t luma_level);

/* Filled rectangle in the luma plane, clipped. */
void screen_rect(uint8_t *luma, int pitch, int w, int h, int x, int y,
		 int rw, int rh, uint8_t level);

/* Filled rectangle in colour: luma AND chroma, clipped. */
void screen_rect_colour(uint8_t *luma, uint8_t *chroma, int pitch, int w,
			int h, int x, int y, int rw, int rh, uint8_t Y,
			uint8_t cb, uint8_t cr);

/* Rectangle outline `t` pixels thick, in luma only. */
/*
 * A filled disc. `chroma` may be NULL for plain luma; pass it with cb/cr to
 * tint one. For the button tester's controller, where rectangles alone do not
 * read as a gamepad.
 */
void screen_disc(uint8_t *luma, uint8_t *chroma, int pitch, int w, int h,
		 int cx, int cy, int r, uint8_t Y, uint8_t cb, uint8_t cr);

void screen_frame(uint8_t *luma, int pitch, int w, int h, int x, int y,
		  int rw, int rh, int t, uint8_t level);

/*
 * Copy an NV12 image (its own luma and chroma planes, `sw` x `sh`, both
 * even, `spitch` bytes per row in each plane) to (x, y), clipped. This is
 * how box art gets on screen with its colour: text and rectangles only
 * touch luma.
 */
void screen_blit_nv12(uint8_t *luma, uint8_t *chroma, int pitch, int w,
		      int h, int x, int y, const uint8_t *sluma,
		      const uint8_t *schroma, int spitch, int sw, int sh);

/* BT.601 limited-range Y/Cb/Cr for an sRGB colour, for the calls above. */
void screen_rgb_to_ycc(uint8_t r, uint8_t g, uint8_t b, uint8_t *Y,
		       uint8_t *cb, uint8_t *cr);

#ifdef __cplusplus
}
#endif

#endif /* XCLOUD_SCREEN_H */
