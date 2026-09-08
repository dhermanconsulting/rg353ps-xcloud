#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

/*
 * One rendered glyph. The bitmap is our own copy: FreeType's slot buffer is
 * overwritten by the next FT_Load_*, so caching means owning the pixels.
 * A 21 px glyph is ~15x18 bytes; a context's whole cache of ASCII plus the
 * odd accented letter is well under 100 KB.
 */
struct glyph {
	uint32_t cp;        /* code point this entry is for; 0 = empty slot */
	int16_t left, top;  /* FreeType's bitmap_left / bitmap_top */
	int16_t advance;    /* pen advance, pixels */
	uint16_t w, rows;
	uint8_t *bitmap;    /* rows * w coverage bytes */
};

/* Non-ASCII code points live in a small open-addressed table. */
#define TEXT_OTHER_SLOTS 256

struct text_ctx {
	FT_Library lib;
	FT_Face face;
	int px;
	struct glyph ascii[128];
	struct glyph other[TEXT_OTHER_SLOTS];
	int other_used;
};

/*
 * Second chance for a font that is not where TEXT_FONT_* says. The device
 * keeps DejaVu at /usr/share/fonts/dejavu/, the Debian package the host
 * simulator uses puts it at /usr/share/fonts/truetype/dejavu/. Only reached
 * after the caller's path failed to open, so on the device, where it opens
 * first time, this never runs. Returns 1 with t->face open.
 */
static int open_face_fallback(struct text_ctx *t, const char *ttf_path)
{
	static const char *const dirs[] = {
		"/usr/share/fonts/dejavu/",
		"/usr/share/fonts/truetype/dejavu/",
	};
	const char *base = strrchr(ttf_path, '/');
	char path[512];

	base = base ? base + 1 : ttf_path;
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(path, sizeof(path), "%s%s", dirs[i], base);
		if (!strcmp(path, ttf_path))
			continue;
		if (FT_New_Face(t->lib, path, 0, &t->face) == 0) {
			fprintf(stderr, "text: %s not found, using %s\n",
				ttf_path, path);
			return 1;
		}
	}
	return 0;
}

struct text_ctx *text_create(const char *ttf_path, int pixel_size)
{
	struct text_ctx *t = calloc(1, sizeof(*t));

	if (!t)
		return NULL;
	t->px = pixel_size;

	if (FT_Init_FreeType(&t->lib)) {
		fprintf(stderr, "text: FT_Init_FreeType failed\n");
		free(t);
		return NULL;
	}
	if (FT_New_Face(t->lib, ttf_path, 0, &t->face) &&
	    !open_face_fallback(t, ttf_path)) {
		fprintf(stderr, "text: cannot open font %s\n", ttf_path);
		FT_Done_FreeType(t->lib);
		free(t);
		return NULL;
	}
	if (FT_Set_Pixel_Sizes(t->face, 0, pixel_size)) {
		fprintf(stderr, "text: cannot set size %d\n", pixel_size);
		text_destroy(t);
		return NULL;
	}
	return t;
}

void text_destroy(struct text_ctx *t)
{
	if (!t)
		return;
	for (int i = 0; i < 128; i++)
		free(t->ascii[i].bitmap);
	for (int i = 0; i < TEXT_OTHER_SLOTS; i++)
		free(t->other[i].bitmap);
	if (t->face)
		FT_Done_Face(t->face);
	if (t->lib)
		FT_Done_FreeType(t->lib);
	free(t);
}

int text_line_height(struct text_ctx *t)
{
	if (!t)
		return 0;
	/* 26.6 fixed point in the scaled metrics. */
	return (int)(t->face->size->metrics.height >> 6);
}

int text_ascent(struct text_ctx *t)
{
	if (!t)
		return 0;
	return (int)(t->face->size->metrics.ascender >> 6);
}

uint32_t text_utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p++;
	int more;

	if (c < 0x80) {
		more = 0;
	} else if ((c & 0xE0) == 0xC0) {
		c &= 0x1F;
		more = 1;
	} else if ((c & 0xF0) == 0xE0) {
		c &= 0x0F;
		more = 2;
	} else if ((c & 0xF8) == 0xF0) {
		c &= 0x07;
		more = 3;
	} else {
		*s = (const char *)p;
		return 0xFFFD;
	}
	for (int i = 0; i < more; i++, p++) {
		if ((*p & 0xC0) != 0x80) {
			/* Truncated sequence: resync on this byte. */
			*s = (const char *)p;
			return 0xFFFD;
		}
		c = (c << 6) | (*p & 0x3F);
	}
	*s = (const char *)p;
	return c;
}

/* Render one code point into `g`, copying the bitmap. Returns 0 on failure. */
static int render_glyph(struct text_ctx *t, uint32_t cp, struct glyph *g)
{
	FT_GlyphSlot slot;
	FT_UInt index = FT_Get_Char_Index(t->face, cp);

	/* The face lacks it (kana, symbols outside DejaVu's coverage, or the
	 * U+FFFD our decoder yields for bad bytes): show a '?' rather than
	 * nothing, so a name is at least visibly incomplete. */
	if (!index)
		index = FT_Get_Char_Index(t->face, '?');
	if (FT_Load_Glyph(t->face, index, FT_LOAD_RENDER))
		return 0;
	slot = t->face->glyph;
	g->cp = cp;
	g->left = (int16_t)slot->bitmap_left;
	g->top = (int16_t)slot->bitmap_top;
	g->advance = (int16_t)(slot->advance.x >> 6);
	g->w = (uint16_t)slot->bitmap.width;
	g->rows = (uint16_t)slot->bitmap.rows;
	g->bitmap = NULL;
	if (g->w && g->rows) {
		g->bitmap = malloc((size_t)g->w * g->rows);
		if (!g->bitmap)
			return 0;
		for (unsigned r = 0; r < g->rows; r++)
			memcpy(g->bitmap + (size_t)r * g->w,
			       slot->bitmap.buffer + (size_t)r * slot->bitmap.pitch,
			       g->w);
	}
	return 1;
}

/*
 * The cached glyph for a code point, rendering it on first use. NULL only
 * if FreeType fails, in which case the caller skips the character.
 */
static const struct glyph *lookup(struct text_ctx *t, uint32_t cp)
{
	struct glyph *g;

	if (cp < 128) {
		g = &t->ascii[cp];
		if (g->cp == cp && cp)
			return g;
		return render_glyph(t, cp, g) ? g : NULL;
	}
	/* Open addressing with linear probing; the table is never more than
	 * three-quarters full, so probes stay short. */
	for (unsigned i = 0; i < TEXT_OTHER_SLOTS; i++) {
		g = &t->other[(cp * 2654435761u + i) % TEXT_OTHER_SLOTS];
		if (g->cp == cp)
			return g;
		if (!g->cp) {
			if (t->other_used >= TEXT_OTHER_SLOTS * 3 / 4)
				break;
			if (!render_glyph(t, cp, g))
				return NULL;
			t->other_used++;
			return g;
		}
	}
	/* Table full: a static scratch entry, valid until the next lookup.
	 * Only reached with more than ~190 distinct non-ASCII characters on
	 * screen over a session, which a library of game names does not do. */
	{
		static struct glyph scratch;

		free(scratch.bitmap);
		scratch.bitmap = NULL;
		return render_glyph(t, cp, &scratch) ? &scratch : NULL;
	}
}

int text_measure(struct text_ctx *t, const char *s)
{
	int x = 0;

	if (!t || !s)
		return 0;
	while (*s) {
		const struct glyph *g = lookup(t, text_utf8_next(&s));

		if (g)
			x += g->advance;
	}
	return x;
}

/*
 * Blit one cached glyph into the luma plane.
 *
 * The destination is a DRM dumb mapping, which is write-combine: reads are
 * extremely slow. So this does NOT alpha-blend against what is already there.
 * It writes the scaled coverage only where coverage is non-zero, so callers
 * should draw text onto a known flat background, which is what screen_clear
 * and screen_rect give us. `lut` maps coverage to luma for the caller's
 * level, built once per string rather than divided per pixel.
 */
/*
 * cx0/cx1 are the horizontal bounds to paint within, already clamped to the
 * plane by the caller. Normally they ARE the plane; text_draw_window narrows
 * them so a string can be slid through a window without spilling out of it.
 */
static void blit_glyph(const struct glyph *g, uint8_t *luma, int pitch,
		       int h, int x0, int y0, int cx0, int cx1,
		       const uint8_t *lut)
{
	for (unsigned row = 0; row < g->rows; row++) {
		int y = y0 + (int)row;
		const uint8_t *src;
		uint8_t *dst;

		if (y < 0 || y >= h)
			continue;
		src = g->bitmap + (size_t)row * g->w;
		dst = luma + (size_t)y * pitch;
		for (unsigned col = 0; col < g->w; col++) {
			int x = x0 + (int)col;
			unsigned cov = src[col];

			if (x < cx0 || x >= cx1 || !cov)
				continue;
			dst[x] = lut[cov];
		}
	}
}

static void build_lut(uint8_t *lut, uint8_t level)
{
	/* Coverage scaled between video black (16) and the requested level. */
	for (int cov = 0; cov < 256; cov++)
		lut[cov] = (uint8_t)(16 + (level - 16) * cov / 255);
}

/* Draw the first `len` bytes of s (all of it if len is SIZE_MAX). */
static int draw_n_clip(struct text_ctx *t, uint8_t *luma, int pitch, int w,
		       int h, int x, int y, const char *s, size_t len,
		       uint8_t level, int cx0, int cx1)
{
	const char *end = len == (size_t)-1 ? NULL : s + len;
	uint8_t lut[256];

	if (cx0 < 0)
		cx0 = 0;
	if (cx1 > w)
		cx1 = w;
	build_lut(lut, level);
	while (*s && (!end || s < end)) {
		const struct glyph *g = lookup(t, text_utf8_next(&s));

		if (!g)
			continue;
		if (g->bitmap)
			blit_glyph(g, luma, pitch, h, x + g->left,
				   y - g->top, cx0, cx1, lut);
		x += g->advance;
	}
	return x;
}

static int draw_n(struct text_ctx *t, uint8_t *luma, int pitch, int w, int h,
		  int x, int y, const char *s, size_t len, uint8_t level)
{
	return draw_n_clip(t, luma, pitch, w, h, x, y, s, len, level, 0, w);
}

int text_draw(struct text_ctx *t, uint8_t *luma, int pitch, int w, int h,
	      int x, int y, const char *s, uint8_t level)
{
	if (!t || !s || !luma)
		return x;
	return draw_n(t, luma, pitch, w, h, x, y, s, (size_t)-1, level);
}

int text_draw_centred(struct text_ctx *t, uint8_t *luma, int pitch, int w,
		      int h, int y, const char *s, uint8_t level)
{
	int width = text_measure(t, s);

	return text_draw(t, luma, pitch, w, h, (w - width) / 2, y, s, level);
}

int text_draw_fit(struct text_ctx *t, uint8_t *luma, int pitch, int w, int h,
		  int x, int y, const char *s, uint8_t level, int max_w)
{
	static const char ellipsis[] = "\xE2\x80\xA6";   /* U+2026 */
	const struct glyph *dots;
	const char *p;
	int used = 0, limit;
	size_t keep = 0;

	if (!t || !s || !luma)
		return x;
	if (text_measure(t, s) <= max_w)
		return text_draw(t, luma, pitch, w, h, x, y, s, level);

	dots = lookup(t, 0x2026);
	limit = max_w - (dots ? dots->advance : 0);
	/* The longest prefix that leaves room for the ellipsis. */
	for (p = s; *p;) {
		const char *next = p;
		const struct glyph *g = lookup(t, text_utf8_next(&next));
		int adv = g ? g->advance : 0;

		if (used + adv > limit)
			break;
		used += adv;
		p = next;
		keep = (size_t)(p - s);
	}
	/* Do not leave a dangling space before the dots. */
	while (keep && s[keep - 1] == ' ')
		keep--;
	x = draw_n(t, luma, pitch, w, h, x, y, s, keep, level);
	return text_draw(t, luma, pitch, w, h, x, y, ellipsis, level);
}

int text_draw_window(struct text_ctx *t, uint8_t *luma, int pitch, int w,
		     int h, int x, int y, const char *s, uint8_t level,
		     int win_x, int win_w)
{
	if (!t || !s || !luma)
		return x;
	return draw_n_clip(t, luma, pitch, w, h, x, y, s, (size_t)-1, level,
			   win_x, win_x + win_w);
}

size_t text_break(struct text_ctx *t, const char *s, int max_w)
{
	const char *p = s;
	int used = 0;
	size_t last_space = 0;   /* byte offset of the last fitting space */
	size_t fit = 0;          /* bytes that fit so far */

	if (!t || !s || !*s)
		return 0;
	while (*p) {
		const char *next = p;
		uint32_t cp = text_utf8_next(&next);
		const struct glyph *g = lookup(t, cp);
		int adv = g ? g->advance : 0;

		if (used + adv > max_w)
			break;
		if (cp == ' ')
			last_space = (size_t)(p - s);
		used += adv;
		p = next;
		fit = (size_t)(p - s);
	}
	if (!*p)
		return fit;                     /* it all fits */
	if (last_space)
		return last_space;              /* cut at the space */
	if (!fit) {
		/* Not even one character fits: take one anyway so the caller
		 * always makes progress. */
		const char *one = s;
		text_utf8_next(&one);
		return (size_t)(one - s);
	}
	return fit;                             /* mid-word */
}
