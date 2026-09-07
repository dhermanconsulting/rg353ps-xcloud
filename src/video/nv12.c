#include "nv12.h"

#include <string.h>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define HAVE_NEON 1
#endif

void nv12_copy_luma(uint8_t *dst, int dst_stride, const uint8_t *src,
		    int src_stride, int width, int height)
{
	if (dst_stride == src_stride && dst_stride == width) {
		memcpy(dst, src, (size_t)width * height);
		return;
	}
	for (int y = 0; y < height; y++)
		memcpy(dst + (size_t)y * dst_stride,
		       src + (size_t)y * src_stride, width);
}

void nv12_interleave_chroma(uint8_t *dst, int dst_stride, const uint8_t *u,
			    int u_stride, const uint8_t *v, int v_stride,
			    int width, int height)
{
	const int cw = width / 2;   /* chroma samples per row */
	const int ch = height / 2;  /* chroma rows */

	for (int y = 0; y < ch; y++) {
		uint8_t *d = dst + (size_t)y * dst_stride;
		const uint8_t *su = u + (size_t)y * u_stride;
		const uint8_t *sv = v + (size_t)y * v_stride;
		int x = 0;

#ifdef HAVE_NEON
		/*
		 * vst2q_u8 writes 16 U and 16 V as 32 interleaved bytes in one
		 * store - exactly the NV12 layout, and a pure forward write.
		 */
		for (; x + 16 <= cw; x += 16) {
			uint8x16x2_t uv;
			uv.val[0] = vld1q_u8(su + x);
			uv.val[1] = vld1q_u8(sv + x);
			vst2q_u8(d + x * 2, uv);
		}
#endif
		for (; x < cw; x++) {
			d[x * 2] = su[x];
			d[x * 2 + 1] = sv[x];
		}
	}
}

/* ---- 2:1 downscale ------------------------------------------------------- */

static inline int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* 9*(b + c) - (a + d), rounded down 4 bits. Sums to 16, so a flat area comes
 * through untouched and only edges are altered. */
#define SHARP4(a, b, c, d) ((9 * ((b) + (c)) - ((a) + (d)) + 8) >> 4)

static void scale2_box_luma(uint8_t *dst, int dst_stride, const uint8_t *src,
			    int src_stride, int dst_w, int dst_h)
{
	for (int y = 0; y < dst_h; y++) {
		const uint8_t *r0 = src + (size_t)(2 * y) * src_stride;
		const uint8_t *r1 = r0 + src_stride;
		uint8_t *d = dst + (size_t)y * dst_stride;
		int x = 0;

#ifdef HAVE_NEON
		/*
		 * vld2q_u8 de-interleaves the pair of source columns that
		 * make each output column, so the 2x2 sum is four widening
		 * adds and one rounding narrow -- 16 output pixels per pass.
		 */
		for (; x + 16 <= dst_w; x += 16) {
			uint8x16x2_t a = vld2q_u8(r0 + 2 * x);
			uint8x16x2_t b = vld2q_u8(r1 + 2 * x);
			uint16x8_t lo = vaddl_u8(vget_low_u8(a.val[0]),
						 vget_low_u8(a.val[1]));
			uint16x8_t hi = vaddl_u8(vget_high_u8(a.val[0]),
						 vget_high_u8(a.val[1]));

			lo = vaddw_u8(lo, vget_low_u8(b.val[0]));
			lo = vaddw_u8(lo, vget_low_u8(b.val[1]));
			hi = vaddw_u8(hi, vget_high_u8(b.val[0]));
			hi = vaddw_u8(hi, vget_high_u8(b.val[1]));
			vst1q_u8(d + x, vcombine_u8(vrshrn_n_u16(lo, 2),
						    vrshrn_n_u16(hi, 2)));
		}
#endif
		for (; x < dst_w; x++)
			d[x] = (uint8_t)((r0[2 * x] + r0[2 * x + 1] +
					  r1[2 * x] + r1[2 * x + 1] + 2) >> 2);
	}
}

/*
 * One source row through the horizontal 4-tap, into int16. Values land in
 * [-32, 287]: the overshoot either side of 8-bit range is the acutance, and
 * it is kept until the vertical pass so it is only clamped once.
 */
static void sharp_hrow(int16_t *out, const uint8_t *src, int dst_w)
{
	const int src_w = dst_w * 2;
	int x = 0;

	/* Output 0 reaches back past the start of the row. */
	out[0] = (int16_t)SHARP4(src[0], src[0], src[1], src[2]);
	x = 1;

#ifdef HAVE_NEON
	/*
	 * Three de-interleaving loads give all four taps directly: at offset
	 * 0 the even lane is src[2x] and the odd lane src[2x+1], at offset -1
	 * the even lane is src[2x-1], and at offset +2 it is src[2x+2]. The
	 * bound keeps the +2 load's last byte inside the row.
	 */
	for (; x + 17 <= dst_w; x += 16) {
		uint8x16x2_t m = vld2q_u8(src + 2 * x);
		uint8x16x2_t p = vld2q_u8(src + 2 * x - 1);
		uint8x16x2_t n = vld2q_u8(src + 2 * x + 2);
		int16x8_t bc, ad;

		bc = vreinterpretq_s16_u16(vaddl_u8(vget_low_u8(m.val[0]),
						    vget_low_u8(m.val[1])));
		ad = vreinterpretq_s16_u16(vaddl_u8(vget_low_u8(p.val[0]),
						    vget_low_u8(n.val[0])));
		vst1q_s16(out + x,
			  vrshrq_n_s16(vsubq_s16(vmulq_n_s16(bc, 9), ad), 4));

		bc = vreinterpretq_s16_u16(vaddl_u8(vget_high_u8(m.val[0]),
						    vget_high_u8(m.val[1])));
		ad = vreinterpretq_s16_u16(vaddl_u8(vget_high_u8(p.val[0]),
						    vget_high_u8(n.val[0])));
		vst1q_s16(out + x + 8,
			  vrshrq_n_s16(vsubq_s16(vmulq_n_s16(bc, 9), ad), 4));
	}
#endif
	for (; x < dst_w; x++)
		out[x] = (int16_t)SHARP4(src[clampi(2 * x - 1, 0, src_w - 1)],
					 src[2 * x], src[2 * x + 1],
					 src[clampi(2 * x + 2, 0, src_w - 1)]);
}

static void scale2_sharp_luma(uint8_t *dst, int dst_stride, const uint8_t *src,
			      int src_stride, int dst_w, int dst_h)
{
	/*
	 * Output row y needs source rows 2y-1, 2y, 2y+1 and 2y+2, so four
	 * horizontally-filtered rows are kept and rotated two at a time and
	 * every source row is filtered exactly once. The destination is the
	 * panel-fitted size, so 1024 is far above anything that can arrive;
	 * a wider one falls back rather than overrunning.
	 */
	int16_t rows[4 * 1024];
	const int src_h = dst_h * 2;
	int16_t *r[4];

	if (dst_w > 1024) {
		scale2_box_luma(dst, dst_stride, src, src_stride, dst_w, dst_h);
		return;
	}
	for (int i = 0; i < 4; i++)
		r[i] = rows + (size_t)i * dst_w;

	sharp_hrow(r[0], src, dst_w);   /* row -1, clamped to row 0 */
	sharp_hrow(r[1], src, dst_w);
	sharp_hrow(r[2], src + (size_t)clampi(1, 0, src_h - 1) * src_stride,
		   dst_w);
	sharp_hrow(r[3], src + (size_t)clampi(2, 0, src_h - 1) * src_stride,
		   dst_w);

	for (int y = 0; y < dst_h; y++) {
		uint8_t *d = dst + (size_t)y * dst_stride;
		int x = 0;

#ifdef HAVE_NEON
		for (; x + 8 <= dst_w; x += 8) {
			int16x8_t a = vld1q_s16(r[0] + x);
			int16x8_t b = vld1q_s16(r[1] + x);
			int16x8_t c = vld1q_s16(r[2] + x);
			int16x8_t e = vld1q_s16(r[3] + x);
			int16x8_t s = vsubq_s16(vmulq_n_s16(vaddq_s16(b, c), 9),
						vaddq_s16(a, e));

			vst1_u8(d + x, vqmovun_s16(vrshrq_n_s16(s, 4)));
		}
#endif
		for (; x < dst_w; x++)
			d[x] = (uint8_t)clampi(SHARP4(r[0][x], r[1][x], r[2][x],
						      r[3][x]), 0, 255);

		if (y + 1 < dst_h) {
			int16_t *t0 = r[0], *t1 = r[1];

			r[0] = r[2];
			r[1] = r[3];
			r[2] = t0;
			r[3] = t1;
			sharp_hrow(r[2], src + (size_t)clampi(2 * y + 3, 0,
							      src_h - 1) *
						       src_stride, dst_w);
			sharp_hrow(r[3], src + (size_t)clampi(2 * y + 4, 0,
							      src_h - 1) *
						       src_stride, dst_w);
		}
	}
}

void nv12_scale2_luma(uint8_t *dst, int dst_stride, const uint8_t *src,
		      int src_stride, int dst_w, int dst_h,
		      enum nv12_scale mode)
{
	if (mode == NV12_SCALE_SHARP)
		scale2_sharp_luma(dst, dst_stride, src, src_stride, dst_w,
				  dst_h);
	else
		scale2_box_luma(dst, dst_stride, src, src_stride, dst_w, dst_h);
}

void nv12_scale2_chroma(uint8_t *dst, int dst_stride, const uint8_t *u,
			int u_stride, const uint8_t *v, int v_stride,
			int dst_w, int dst_h)
{
	const int cw = dst_w / 2;   /* UV pairs per destination row */
	const int ch = dst_h / 2;

	for (int y = 0; y < ch; y++) {
		const uint8_t *u0 = u + (size_t)(2 * y) * u_stride;
		const uint8_t *u1 = u0 + u_stride;
		const uint8_t *v0 = v + (size_t)(2 * y) * v_stride;
		const uint8_t *v1 = v0 + v_stride;
		uint8_t *d = dst + (size_t)y * dst_stride;
		int x = 0;

#ifdef HAVE_NEON
		for (; x + 16 <= cw; x += 16) {
			uint8x16x2_t au = vld2q_u8(u0 + 2 * x);
			uint8x16x2_t bu = vld2q_u8(u1 + 2 * x);
			uint8x16x2_t av = vld2q_u8(v0 + 2 * x);
			uint8x16x2_t bv = vld2q_u8(v1 + 2 * x);
			uint8x16x2_t out;
			uint16x8_t lo, hi;

			lo = vaddl_u8(vget_low_u8(au.val[0]),
				      vget_low_u8(au.val[1]));
			lo = vaddw_u8(lo, vget_low_u8(bu.val[0]));
			lo = vaddw_u8(lo, vget_low_u8(bu.val[1]));
			hi = vaddl_u8(vget_high_u8(au.val[0]),
				      vget_high_u8(au.val[1]));
			hi = vaddw_u8(hi, vget_high_u8(bu.val[0]));
			hi = vaddw_u8(hi, vget_high_u8(bu.val[1]));
			out.val[0] = vcombine_u8(vrshrn_n_u16(lo, 2),
						 vrshrn_n_u16(hi, 2));

			lo = vaddl_u8(vget_low_u8(av.val[0]),
				      vget_low_u8(av.val[1]));
			lo = vaddw_u8(lo, vget_low_u8(bv.val[0]));
			lo = vaddw_u8(lo, vget_low_u8(bv.val[1]));
			hi = vaddl_u8(vget_high_u8(av.val[0]),
				      vget_high_u8(av.val[1]));
			hi = vaddw_u8(hi, vget_high_u8(bv.val[0]));
			hi = vaddw_u8(hi, vget_high_u8(bv.val[1]));
			out.val[1] = vcombine_u8(vrshrn_n_u16(lo, 2),
						 vrshrn_n_u16(hi, 2));

			vst2q_u8(d + x * 2, out);
		}
#endif
		for (; x < cw; x++) {
			d[x * 2] = (uint8_t)((u0[2 * x] + u0[2 * x + 1] +
					      u1[2 * x] + u1[2 * x + 1] + 2) >> 2);
			d[x * 2 + 1] = (uint8_t)((v0[2 * x] + v0[2 * x + 1] +
						  v1[2 * x] + v1[2 * x + 1] + 2) >> 2);
		}
	}
}
