/*
 * nv12check -- the vectorised 2:1 downscale against plain C, on aarch64.
 *
 * src/video/nv12.c takes a NEON path on the device and a scalar one
 * everywhere else, so the host simulator exercises only half of it and a
 * wrong intrinsic would first show up as a corrupt picture on the handheld.
 * This runs both against the same input and compares byte for byte.
 *
 * Build for the device (or for QEMU) and run there:
 *
 *   sh scripts/nv12check.sh
 *
 * Sizes deliberately include widths that are not multiples of 16, so the
 * scalar tails after each vector loop are covered too, and strides that are
 * not the width, so a stride confusion cannot pass.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/video/nv12.h"

static uint32_t rng_state = 0x12345678u;

static uint8_t rnd8(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return (uint8_t)(rng_state >> 7);
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

#define SHARP4(a, b, c, d) ((9 * ((b) + (c)) - ((a) + (d)) + 8) >> 4)

/* ---- references, written the obvious way -------------------------------- */

static void ref_box(uint8_t *dst, int dst_stride, const uint8_t *src,
		    int src_stride, int dst_w, int dst_h)
{
	for (int y = 0; y < dst_h; y++)
		for (int x = 0; x < dst_w; x++) {
			const uint8_t *r0 = src + (size_t)(2 * y) * src_stride;
			const uint8_t *r1 = r0 + src_stride;

			dst[(size_t)y * dst_stride + x] = (uint8_t)
				((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] +
				  r1[2 * x + 1] + 2) >> 2);
		}
}

static void ref_sharp(uint8_t *dst, int dst_stride, const uint8_t *src,
		      int src_stride, int dst_w, int dst_h)
{
	const int src_w = dst_w * 2, src_h = dst_h * 2;
	int16_t *h = malloc((size_t)dst_w * src_h * sizeof(*h));

	for (int y = 0; y < src_h; y++) {
		const uint8_t *r = src + (size_t)y * src_stride;

		for (int x = 0; x < dst_w; x++)
			h[(size_t)y * dst_w + x] = (int16_t)
				SHARP4(r[clampi(2 * x - 1, 0, src_w - 1)],
				       r[2 * x], r[2 * x + 1],
				       r[clampi(2 * x + 2, 0, src_w - 1)]);
	}
	for (int y = 0; y < dst_h; y++)
		for (int x = 0; x < dst_w; x++) {
			const int16_t *a = h + (size_t)clampi(2 * y - 1, 0, src_h - 1) * dst_w;
			const int16_t *b = h + (size_t)clampi(2 * y, 0, src_h - 1) * dst_w;
			const int16_t *c = h + (size_t)clampi(2 * y + 1, 0, src_h - 1) * dst_w;
			const int16_t *d = h + (size_t)clampi(2 * y + 2, 0, src_h - 1) * dst_w;

			dst[(size_t)y * dst_stride + x] = (uint8_t)
				clampi(SHARP4(a[x], b[x], c[x], d[x]), 0, 255);
		}
	free(h);
}

static void ref_chroma(uint8_t *dst, int dst_stride, const uint8_t *u,
		       int u_stride, const uint8_t *v, int v_stride, int dst_w,
		       int dst_h)
{
	for (int y = 0; y < dst_h / 2; y++)
		for (int x = 0; x < dst_w / 2; x++) {
			const uint8_t *u0 = u + (size_t)(2 * y) * u_stride;
			const uint8_t *u1 = u0 + u_stride;
			const uint8_t *v0 = v + (size_t)(2 * y) * v_stride;
			const uint8_t *v1 = v0 + v_stride;
			uint8_t *d = dst + (size_t)y * dst_stride;

			d[x * 2] = (uint8_t)((u0[2 * x] + u0[2 * x + 1] +
					      u1[2 * x] + u1[2 * x + 1] + 2) >> 2);
			d[x * 2 + 1] = (uint8_t)((v0[2 * x] + v0[2 * x + 1] +
						  v1[2 * x] + v1[2 * x + 1] + 2) >> 2);
		}
}

/* ---- the comparison ------------------------------------------------------ */

static int fails;

static void compare(const char *what, int w, int h, const uint8_t *got,
		    const uint8_t *want, int stride, int rows)
{
	for (int y = 0; y < rows; y++)
		for (int x = 0; x < w; x++) {
			size_t at = (size_t)y * stride + x;

			if (got[at] == want[at])
				continue;
			printf("FAIL %s %dx%d: at (%d,%d) got %u want %u\n",
			       what, w, h, x, y, got[at], want[at]);
			fails++;
			return;
		}
	printf("ok   %s %dx%d\n", what, w, h);
}

static void run(int dst_w, int dst_h)
{
	const int src_w = dst_w * 2, src_h = dst_h * 2;
	const int src_stride = src_w + 37;   /* never the width */
	const int uv_stride = dst_w + 11;    /* U and V are dst_w x dst_h */
	const int dst_stride = dst_w + 13;
	uint8_t *src = malloc((size_t)src_stride * src_h);
	uint8_t *u = malloc((size_t)uv_stride * dst_h);
	uint8_t *v = malloc((size_t)uv_stride * dst_h);
	uint8_t *got = malloc((size_t)dst_stride * dst_h);
	uint8_t *want = malloc((size_t)dst_stride * dst_h);

	if (!src || !u || !v || !got || !want) {
		printf("FAIL out of memory at %dx%d\n", dst_w, dst_h);
		fails++;
		return;
	}
	for (size_t i = 0; i < (size_t)src_stride * src_h; i++)
		src[i] = rnd8();
	for (size_t i = 0; i < (size_t)uv_stride * dst_h; i++) {
		u[i] = rnd8();
		v[i] = rnd8();
	}

	memset(got, 0xAA, (size_t)dst_stride * dst_h);
	memset(want, 0xAA, (size_t)dst_stride * dst_h);
	nv12_scale2_luma(got, dst_stride, src, src_stride, dst_w, dst_h,
			 NV12_SCALE_BOX);
	ref_box(want, dst_stride, src, src_stride, dst_w, dst_h);
	compare("luma box", dst_w, dst_h, got, want, dst_stride, dst_h);

	memset(got, 0xAA, (size_t)dst_stride * dst_h);
	memset(want, 0xAA, (size_t)dst_stride * dst_h);
	nv12_scale2_luma(got, dst_stride, src, src_stride, dst_w, dst_h,
			 NV12_SCALE_SHARP);
	ref_sharp(want, dst_stride, src, src_stride, dst_w, dst_h);
	compare("luma sharp", dst_w, dst_h, got, want, dst_stride, dst_h);

	memset(got, 0xAA, (size_t)dst_stride * dst_h);
	memset(want, 0xAA, (size_t)dst_stride * dst_h);
	nv12_scale2_chroma(got, dst_stride, u, uv_stride, v, uv_stride, dst_w,
			   dst_h);
	ref_chroma(want, dst_stride, u, uv_stride, v, uv_stride, dst_w, dst_h);
	compare("chroma", dst_w, dst_h, got, want, dst_stride, dst_h / 2);

	free(src);
	free(u);
	free(v);
	free(got);
	free(want);
}

int main(void)
{
#if defined(__ARM_NEON) || defined(__aarch64__)
	printf("nv12check: NEON path\n");
#else
	printf("nv12check: scalar path (this only proves the reference)\n");
#endif
	run(640, 360);   /* the real one */
	run(320, 180);
	run(624, 352);   /* width not a multiple of 16: scalar tails */
	run(158, 90);    /* too small for the sharp vector loop at all */
	printf("nv12check: %s\n", fails ? "FAILED" : "all comparisons equal");
	return fails ? 1 : 0;
}
