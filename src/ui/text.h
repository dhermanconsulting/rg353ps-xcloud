/*
 * Text rendering into an NV12 luma plane, via FreeType.
 *
 * The device already ships libfreetype 2.10 and the DejaVu family, and the
 * build container has the same soname, so we get real anti-aliased text with
 * no bundled font and no hand-rolled bitmap glyphs.
 *
 * Text is drawn into the luma plane only. Chroma stays neutral (0x80), which
 * renders as greyscale: white text on a black field.
 *
 * Strings are UTF-8. Game names carry TM signs, accents and the odd kana;
 * a code point the face has no glyph for is drawn as '?'. Rendered glyphs
 * are cached per context, so a repaint is blits only -- the library screen
 * has to stay under ~5 ms and FreeType rasterising every glyph on every
 * frame does not (see the timing note in src/app/library.cpp).
 */
#ifndef XCLOUD_TEXT_H
#define XCLOUD_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEXT_FONT_DEFAULT "/usr/share/fonts/dejavu/DejaVuSans.ttf"
#define TEXT_FONT_MONO    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf"

struct text_ctx;

struct text_ctx *text_create(const char *ttf_path, int pixel_size);
void text_destroy(struct text_ctx *t);

/* Height to advance between baselines. */
int text_line_height(struct text_ctx *t);

/* Baseline-to-top of the tallest glyph, for placing a line in a box. */
int text_ascent(struct text_ctx *t);

/* Width the string would occupy, in pixels. */
int text_measure(struct text_ctx *t, const char *s);

/*
 * Draw at (x, y), where y is the BASELINE. Clipped to the plane. `level` is
 * the luma value for full coverage (235 for white). Returns the end x.
 */
int text_draw(struct text_ctx *t, uint8_t *luma, int pitch, int w, int h,
	      int x, int y, const char *s, uint8_t level);

/* Convenience: centre a string horizontally. */
int text_draw_centred(struct text_ctx *t, uint8_t *luma, int pitch, int w,
		      int h, int y, const char *s, uint8_t level);

/*
 * Draw at most `max_w` pixels of the string, ending in an ellipsis if it had
 * to be cut. Returns the end x.
 */
int text_draw_fit(struct text_ctx *t, uint8_t *luma, int pitch, int w, int h,
		  int x, int y, const char *s, uint8_t level, int max_w);

/*
 * Word wrap: the number of bytes at the start of `s` that fit in `max_w`,
 * cut at the last space that fits, or mid-word if the first word alone does
 * not fit (never zero for a non-empty string). Callers draw that prefix,
 * skip the spaces after it and call again for the next line.
 */
size_t text_break(struct text_ctx *t, const char *s, int max_w);

/*
 * Decode one UTF-8 code point and advance *s past it. Malformed input yields
 * U+FFFD and advances one byte, so a loop always terminates. Shared with the
 * library's sort keys and letter buckets, which need the same decoding.
 */
uint32_t text_utf8_next(const char **s);

#ifdef __cplusplus
}
#endif

#endif /* XCLOUD_TEXT_H */
