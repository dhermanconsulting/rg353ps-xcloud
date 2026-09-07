/*
 * scaletest -- what the 2:1 downscale does to text, filter by filter.
 *
 * The client hands VOP2 a full 1280x720 NV12 buffer and a 640x360
 * destination rectangle, so every pixel on the panel comes out of the
 * Esmart window's scaler. This reproduces that scaler in software, next to
 * the filters we could use instead, and writes PNGs to look at.
 *
 * The hardware path, from drivers/gpu/drm/rockchip/rockchip_drm_vop2.c
 * (vop2_setup_scale) and the same arithmetic in the 4.19 BSP driver:
 *
 *   vertical    720 >= 2 * 360, so the GT2 pre-scaler runs and src_h
 *               becomes 360; the vertical scaler then sees 360 -> 360 and
 *               is bypassed entirely. GT2 is one hardware stage whose
 *               silicon behaviour -- decimate or average two lines -- is
 *               not stated by the driver, so both are emulated here.
 *   horizontal  a 2-tap bilinear with
 *                   fac = DIV_ROUND_UP((1280-1) << 12, 640-1) - 1 = 8198
 *               i.e. a step of 2.00146 source pixels, phase starting at 0.
 *               Over most of the line the two taps are weighted ~1.0/0.0,
 *               which is nearest-neighbour column dropping; only near the
 *               middle of the picture do they reach 50/50.
 *
 * That last point is the interesting one and the test image is built to
 * show it: the same text is drawn at the left and the right of the frame,
 * and isolated 1-pixel lines are drawn on even and on odd columns, so the
 * parity the scaler keeps is visible directly.
 *
 * Luma only. Text lives in luma, and the chroma planes take the same 2:1
 * ratio through the same scaler.
 *
 *   sh scripts/scaletest.sh          # builds and runs in the host image
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include "../src/ui/text.h"

#define SRC_W 1280
#define SRC_H 720
#define DST_W 640
#define DST_H 360

#define BG 16     /* video-range black, as the client's screens use */
#define FG 235    /* video-range white */

struct plane {
	uint8_t *y;
	int w, h, stride;
};

static struct plane plane_alloc(int w, int h, uint8_t fill)
{
	struct plane p;

	p.w = w;
	p.h = h;
	p.stride = w;
	p.y = malloc((size_t)w * h);
	if (!p.y) {
		fprintf(stderr, "scaletest: out of memory\n");
		exit(1);
	}
	memset(p.y, fill, (size_t)w * h);
	return p;
}

static void plane_free(struct plane *p)
{
	free(p->y);
	p->y = NULL;
}

static inline uint8_t at(const struct plane *p, int x, int y)
{
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= p->w)
		x = p->w - 1;
	if (y >= p->h)
		y = p->h - 1;
	return p->y[(size_t)y * p->stride + x];
}

/* ---- PNG, 8-bit greyscale, straight to zlib ----------------------------- */

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static int png_chunk(FILE *fp, const char *type, const uint8_t *data,
		     uint32_t len)
{
	uint8_t hdr[8], tail[4];
	uint32_t crc;

	be32(hdr, len);
	memcpy(hdr + 4, type, 4);
	if (fwrite(hdr, 1, 8, fp) != 8)
		return -1;
	if (len && fwrite(data, 1, len, fp) != len)
		return -1;
	crc = crc32(0, (const Bytef *)type, 4);
	if (len)
		crc = crc32(crc, data, len);
	be32(tail, crc);
	return fwrite(tail, 1, 4, fp) != 4 ? -1 : 0;
}

static int write_png(const char *path, const struct plane *p)
{
	static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
	size_t raw_len = (size_t)(p->w + 1) * p->h;
	uint8_t *raw = malloc(raw_len);
	uLongf zlen = compressBound((uLong)raw_len);
	uint8_t *z = malloc(zlen);
	uint8_t ihdr[13];
	FILE *fp;
	int rc = -1;

	if (!raw || !z)
		goto out;
	for (int y = 0; y < p->h; y++) {
		raw[(size_t)y * (p->w + 1)] = 0;   /* filter: none */
		memcpy(raw + (size_t)y * (p->w + 1) + 1,
		       p->y + (size_t)y * p->stride, p->w);
	}
	if (compress2(z, &zlen, raw, (uLong)raw_len, 6) != Z_OK)
		goto out;

	fp = fopen(path, "wb");
	if (!fp)
		goto out;
	be32(ihdr, (uint32_t)p->w);
	be32(ihdr + 4, (uint32_t)p->h);
	ihdr[8] = 8;    /* bit depth */
	ihdr[9] = 0;    /* colour type: greyscale */
	ihdr[10] = 0;
	ihdr[11] = 0;
	ihdr[12] = 0;
	if (fwrite(sig, 1, 8, fp) != 8 ||
	    png_chunk(fp, "IHDR", ihdr, 13) ||
	    png_chunk(fp, "IDAT", z, (uint32_t)zlen) ||
	    png_chunk(fp, "IEND", NULL, 0))
		fprintf(stderr, "scaletest: short write on %s\n", path);
	else
		rc = 0;
	fclose(fp);
out:
	free(raw);
	free(z);
	return rc;
}

/* ---- the test frame ------------------------------------------------------ */

/*
 * A 720p frame shaped like a game HUD: text at the sizes a 720p title
 * actually uses, drawn identically at the left and the right of the frame
 * so a scaler whose phase drifts across the line shows it, then isolated
 * 1-pixel lines split by parity, then the alternating patterns that sit
 * exactly on Nyquist.
 */
static const int kTextX[2] = { 30, 880 };

struct label {
	int size;
	int baseline;
	const char *s;
};

static const struct label kLines[] = {
	{ 28,  52, "LAP 3/5  1:23.4" },
	{ 24,  88, "AMMO 24/120" },
	{ 20, 120, "Press A to continue" },
	{ 16, 148, "Objective: finish first" },
	{ 14, 172, "resolution 1280x720 @ 60" },
};

static void build_source(struct plane *p)
{
	struct text_ctx *f;

	memset(p->y, BG, (size_t)p->stride * p->h);

	for (size_t i = 0; i < sizeof(kLines) / sizeof(kLines[0]); i++) {
		f = text_create(TEXT_FONT_DEFAULT, kLines[i].size);
		if (!f) {
			fprintf(stderr, "scaletest: no font at %d px\n",
				kLines[i].size);
			exit(1);
		}
		for (int c = 0; c < 2; c++)
			text_draw(f, p->y, p->stride, p->w, p->h, kTextX[c],
				  kLines[i].baseline, kLines[i].s, FG);
		text_destroy(f);
	}

	/*
	 * Isolated 1-pixel columns every 8, first on even x then on odd x.
	 * A scaler that keeps one column of each pair renders one of these
	 * bands at full contrast and erases the other, and which one it
	 * erases depends on where you are across the picture.
	 */
	for (int y = 200; y < 240; y++)
		for (int x = 40; x < 1240; x += 8)
			p->y[(size_t)y * p->stride + x] = FG;          /* even */
	for (int y = 250; y < 290; y++)
		for (int x = 41; x < 1240; x += 8)
			p->y[(size_t)y * p->stride + x] = FG;          /* odd  */

	/* The same split for rows, which is the GT2 stage's business. */
	for (int y = 300; y < 340; y += 2)
		memset(p->y + (size_t)y * p->stride + 40, FG, 1200);
	for (int y = 351; y < 390; y += 2)
		memset(p->y + (size_t)y * p->stride + 40, FG, 1200);

	/* Alternating 1-pixel patterns: exactly Nyquist, where the correct
	 * answer is flat grey and full contrast means aliasing. */
	for (int y = 400; y < 440; y++)
		for (int x = 40; x < 1240; x += 2)
			p->y[(size_t)y * p->stride + x] = FG;
	for (int y = 450; y < 490; y += 2)
		memset(p->y + (size_t)y * p->stride + 40, FG, 1200);

	/* Something continuous, standing in for game content: a smooth
	 * gradient has no thin high-contrast feature to lose. */
	for (int y = 500; y < 700; y++)
		for (int x = 40; x < 1240; x++)
			p->y[(size_t)y * p->stride + x] =
				(uint8_t)(BG + (x - 40) * (FG - BG) / 1200);
}

/* ---- the filters --------------------------------------------------------- */

/*
 * VOP2's Esmart scaler. gt2_average picks which of the two possible GT2
 * behaviours to model: averaging both source lines, or keeping the first.
 */
#define VOP2_HFAC 8198   /* DIV_ROUND_UP(1279 << 12, 639) - 1 */

static void filter_vop2(struct plane *dst, const struct plane *src,
			int gt2_average)
{
	struct plane mid = plane_alloc(src->w, dst->h, BG);

	for (int y = 0; y < mid.h; y++)
		for (int x = 0; x < mid.w; x++)
			mid.y[(size_t)y * mid.stride + x] =
				gt2_average
					? (uint8_t)((at(src, x, 2 * y) +
						     at(src, x, 2 * y + 1) + 1) / 2)
					: at(src, x, 2 * y);

	for (int y = 0; y < dst->h; y++) {
		for (int x = 0; x < dst->w; x++) {
			int acc = x * VOP2_HFAC;
			int sx = acc >> 12;
			int frac = acc & 4095;
			int a = at(&mid, sx, y);
			int b = at(&mid, sx + 1, y);

			dst->y[(size_t)y * dst->stride + x] =
				(uint8_t)((a * (4096 - frac) + b * frac + 2048) >> 12);
		}
	}
	plane_free(&mid);
}

/* The honest 2:1 answer: every source pixel counted once. */
static void filter_box(struct plane *dst, const struct plane *src)
{
	for (int y = 0; y < dst->h; y++)
		for (int x = 0; x < dst->w; x++)
			dst->y[(size_t)y * dst->stride + x] = (uint8_t)
				((at(src, 2 * x, 2 * y) +
				  at(src, 2 * x + 1, 2 * y) +
				  at(src, 2 * x, 2 * y + 1) +
				  at(src, 2 * x + 1, 2 * y + 1) + 2) / 4);
}

/*
 * A box with a little of the neighbouring pair subtracted: separable
 * [-1, 9, 9, -1] / 16, which is Catmull-Rom at the half-pixel phase. Sums
 * to 16, so flat areas are untouched; edges get back the acutance a plain
 * box costs them. This is the cheap "sharp" candidate.
 */
static void filter_sharp(struct plane *dst, const struct plane *src)
{
	struct plane mid = plane_alloc(dst->w, src->h, BG);

	for (int y = 0; y < mid.h; y++) {
		for (int x = 0; x < mid.w; x++) {
			int v = (-at(src, 2 * x - 1, y) + 9 * at(src, 2 * x, y) +
				 9 * at(src, 2 * x + 1, y) -
				 at(src, 2 * x + 2, y) + 8) >> 4;

			mid.y[(size_t)y * mid.stride + x] =
				(uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
	}
	for (int y = 0; y < dst->h; y++) {
		for (int x = 0; x < dst->w; x++) {
			int v = (-at(&mid, x, 2 * y - 1) + 9 * at(&mid, x, 2 * y) +
				 9 * at(&mid, x, 2 * y + 1) -
				 at(&mid, x, 2 * y + 2) + 8) >> 4;

			dst->y[(size_t)y * dst->stride + x] =
				(uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
	}
	plane_free(&mid);
}

/*
 * A box done in linear light. Averaging gamma-encoded samples darkens a
 * thin bright stroke: half coverage of 235 over 16 should read as a good
 * deal brighter than the (235+16)/2 a naive average gives. Costs two
 * lookups per sample.
 */
static double g_to_lin[256];
static uint8_t g_to_gamma[4096];

static void gamma_tables_init(void)
{
	for (int i = 0; i < 256; i++)
		g_to_lin[i] = pow(i / 255.0, 2.2);
	for (int i = 0; i < 4096; i++) {
		double v = pow(i / 4095.0, 1.0 / 2.2) * 255.0 + 0.5;

		g_to_gamma[i] = (uint8_t)(v > 255.0 ? 255.0 : v);
	}
}

static void filter_linear_box(struct plane *dst, const struct plane *src)
{
	for (int y = 0; y < dst->h; y++) {
		for (int x = 0; x < dst->w; x++) {
			double l = (g_to_lin[at(src, 2 * x, 2 * y)] +
				    g_to_lin[at(src, 2 * x + 1, 2 * y)] +
				    g_to_lin[at(src, 2 * x, 2 * y + 1)] +
				    g_to_lin[at(src, 2 * x + 1, 2 * y + 1)]) / 4.0;
			int idx = (int)(l * 4095.0 + 0.5);

			dst->y[(size_t)y * dst->stride + x] =
				g_to_gamma[idx < 0 ? 0 : idx > 4095 ? 4095 : idx];
		}
	}
}

/*
 * The reference every candidate is measured against: Lanczos-3, area
 * correct, in double precision. Far too slow for the device; it is here to
 * say what the picture should have looked like.
 */
static double lanczos(double x, double a)
{
	if (x < 0)
		x = -x;
	if (x < 1e-9)
		return 1.0;
	if (x >= a)
		return 0.0;
	return a * sin(M_PI * x) * sin(M_PI * x / a) / (M_PI * M_PI * x * x);
}

static void filter_reference(struct plane *dst, const struct plane *src)
{
	const double a = 3.0, scale = 2.0;   /* 2:1 in both axes */
	const int support = (int)(a * scale);
	struct plane mid = plane_alloc(dst->w, src->h, BG);

	for (int x = 0; x < dst->w; x++) {
		double centre = (x + 0.5) * scale - 0.5;
		int first = (int)ceil(centre - support);
		double w[16], sum = 0.0;
		int n = 0;

		for (int s = first; s <= centre + support && n < 16; s++, n++) {
			w[n] = lanczos((s - centre) / scale, a);
			sum += w[n];
		}
		for (int y = 0; y < src->h; y++) {
			double acc = 0.0;

			for (int i = 0; i < n; i++)
				acc += w[i] * at(src, first + i, y);
			acc /= sum;
			mid.y[(size_t)y * mid.stride + x] = (uint8_t)
				(acc < 0 ? 0 : acc > 255 ? 255 : acc + 0.5);
		}
	}
	for (int y = 0; y < dst->h; y++) {
		double centre = (y + 0.5) * scale - 0.5;
		int first = (int)ceil(centre - support);
		double w[16], sum = 0.0;
		int n = 0;

		for (int s = first; s <= centre + support && n < 16; s++, n++) {
			w[n] = lanczos((s - centre) / scale, a);
			sum += w[n];
		}
		for (int x = 0; x < dst->w; x++) {
			double acc = 0.0;

			for (int i = 0; i < n; i++)
				acc += w[i] * at(&mid, x, first + i);
			acc /= sum;
			dst->y[(size_t)y * dst->stride + x] = (uint8_t)
				(acc < 0 ? 0 : acc > 255 ? 255 : acc + 0.5);
		}
	}
	plane_free(&mid);
}

/* ---- comparison sheets --------------------------------------------------- */

static void blit_zoom(struct plane *dst, int dx, int dy, const struct plane *src,
		      int sx, int sy, int w, int h, int zoom)
{
	for (int y = 0; y < h * zoom; y++)
		for (int x = 0; x < w * zoom; x++) {
			int tx = dx + x, ty = dy + y;

			if (tx < 0 || ty < 0 || tx >= dst->w || ty >= dst->h)
				continue;
			dst->y[(size_t)ty * dst->stride + tx] =
				at(src, sx + x / zoom, sy + y / zoom);
		}
}

struct candidate {
	const char *name;
	const char *note;
	struct plane out;
};

/*
 * Mean absolute error against the Lanczos reference, over one rectangle.
 * Reported for the text block, which is the region the question is about;
 * a lower number is a more faithful rendering of what the frame contained.
 */
static double mae(const struct plane *a, const struct plane *b, int x0, int y0,
		  int w, int h)
{
	double acc = 0.0;

	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			acc += fabs((double)at(a, x, y) - at(b, x, y));
	return acc / ((double)w * h);
}

int main(void)
{
	const char *dir = "out/host/scaletest";
	struct plane src = plane_alloc(SRC_W, SRC_H, BG);
	struct candidate c[] = {
		{ "vop2-gt2-drop", "the panel today, if GT2 drops a line", {0} },
		{ "vop2-gt2-avg",  "the panel today, if GT2 averages two", {0} },
		{ "box",           "2x2 average on the CPU", {0} },
		{ "sharp",         "[-1 9 9 -1]/16 on the CPU", {0} },
		{ "linear-box",    "2x2 average in linear light", {0} },
		{ "reference",     "Lanczos-3, what it should look like", {0} },
	};
	const int n = (int)(sizeof(c) / sizeof(c[0]));
	struct text_ctx *lab;
	char path[512];

	gamma_tables_init();
	mkdir("out", 0755);
	mkdir("out/host", 0755);
	mkdir(dir, 0755);

	build_source(&src);
	snprintf(path, sizeof(path), "%s/source-720p.png", dir);
	write_png(path, &src);

	for (int i = 0; i < n; i++)
		c[i].out = plane_alloc(DST_W, DST_H, BG);
	filter_vop2(&c[0].out, &src, 0);
	filter_vop2(&c[1].out, &src, 1);
	filter_box(&c[2].out, &src);
	filter_sharp(&c[3].out, &src);
	filter_linear_box(&c[4].out, &src);
	filter_reference(&c[5].out, &src);

	for (int i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/%s.png", dir, c[i].name);
		write_png(path, &c[i].out);
	}

	/*
	 * Two sheets, one band per filter: the text as it lands on the panel
	 * at the left and the right of the frame, and the parity patterns.
	 */
	lab = text_create(TEXT_FONT_DEFAULT, 17);
	if (!lab) {
		fprintf(stderr, "scaletest: no label font\n");
		return 1;
	}

	{
		const int zoom = 3, cw = 190, ch = 78, gap = 10;
		const int bandw = cw * zoom * 2 + gap;
		const int bandh = ch * zoom + 40;
		struct plane sheet = plane_alloc(bandw, bandh * n, BG);

		for (int i = 0; i < n; i++) {
			int y0 = i * bandh;
			char line[160];

			snprintf(line, sizeof(line), "%s  -  %s", c[i].name,
				 c[i].note);
			text_draw(lab, sheet.y, sheet.stride, sheet.w, sheet.h,
				  4, y0 + 18, line, 200);
			blit_zoom(&sheet, 0, y0 + 26, &c[i].out, 12, 8, cw, ch,
				  zoom);
			blit_zoom(&sheet, cw * zoom + gap, y0 + 26, &c[i].out,
				  436, 8, cw, ch, zoom);
		}
		snprintf(path, sizeof(path), "%s/compare-text.png", dir);
		write_png(path, &sheet);
		plane_free(&sheet);
	}

	{
		/*
		 * The crop starts at dst y 98, i.e. src y 196, so each pattern
		 * band in build_source() lands at a known offset. Labelling
		 * them makes the sheet readable without the source next to it.
		 */
		static const struct { int rel_y; const char *s; } kRows[] = {
			{   6, "1px columns, even x" },
			{  81, "1px columns, odd x" },
			{ 156, "1px rows, even y" },
			{ 231, "1px rows, odd y" },
			{ 306, "alternating columns" },
			{ 381, "alternating rows" },
		};
		const int zoom = 3, cw = 200, ch = 150, labw = 200;
		const int bandh = ch * zoom + 40;
		struct plane sheet = plane_alloc(labw + cw * zoom, bandh * n, BG);

		for (int i = 0; i < n; i++) {
			int y0 = i * bandh;

			text_draw(lab, sheet.y, sheet.stride, sheet.w, sheet.h,
				  4, y0 + 20, c[i].name, 210);
			for (size_t r = 0; r < sizeof(kRows) / sizeof(kRows[0]); r++)
				text_draw(lab, sheet.y, sheet.stride, sheet.w,
					  sheet.h, 4,
					  y0 + 34 + kRows[r].rel_y + 34,
					  kRows[r].s, 150);
			blit_zoom(&sheet, labw, y0 + 34, &c[i].out, 20, 98, cw,
				  ch, zoom);
		}
		snprintf(path, sizeof(path), "%s/compare-patterns.png", dir);
		write_png(path, &sheet);
		plane_free(&sheet);
	}
	text_destroy(lab);

	/* Numbers, over the left and the right text blocks separately. */
	printf("mean absolute error vs Lanczos-3, over the text blocks\n");
	printf("  %-16s %10s %10s\n", "filter", "left", "right");
	for (int i = 0; i < n - 1; i++)
		printf("  %-16s %10.2f %10.2f\n", c[i].name,
		       mae(&c[i].out, &c[n - 1].out, 12, 8, 190, 78),
		       mae(&c[i].out, &c[n - 1].out, 436, 8, 190, 78));

	printf("\nPNGs in %s/\n", dir);
	for (int i = 0; i < n; i++)
		plane_free(&c[i].out);
	plane_free(&src);
	return 0;
}
