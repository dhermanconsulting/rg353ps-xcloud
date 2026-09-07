/*
 * I420 (yuv420p) -> NV12 into a DRM dumb buffer.
 *
 * The destination is write-combine memory: sequential writes are fast, reads
 * are catastrophically slow. Everything here writes forward and never reads
 * the destination, which is also why libswscale is not used for this step.
 */
#ifndef XCLOUD_NV12_H
#define XCLOUD_NV12_H

#include <stddef.h>
#include <stdint.h>

/* Copy the luma plane row by row, honouring differing strides. */
void nv12_copy_luma(uint8_t *dst, int dst_stride, const uint8_t *src,
		    int src_stride, int width, int height);

/* Interleave U and V into the chroma plane. width/height are LUMA dims. */
void nv12_interleave_chroma(uint8_t *dst, int dst_stride, const uint8_t *u,
			    int u_stride, const uint8_t *v, int v_stride,
			    int width, int height);

/*
 * ---- 2:1 downscale on the CPU -------------------------------------------
 *
 * Handing VOP2 a 1280x720 buffer with a 640x360 destination makes its Esmart
 * scaler do the halving, and it does it badly: the horizontal step is
 * 8198/4096 = 2.00146 source pixels with a 2-tap bilinear, so over most of
 * the line one tap has essentially all the weight and the other column is
 * discarded, and the parity that survives drifts across the picture.
 * Vertically 720 >= 2*360 engages the GT2 pre-scaler and the vertical filter
 * is bypassed entirely. Photographic content does not care; text is made of
 * 1-pixel stems and does.
 *
 * Doing the halving here instead gives a filter we choose, treats every
 * column alike, and hands the plane a source it does not have to scale at
 * all. It also writes a QUARTER as many bytes into the write-combine
 * mapping, which is where the old full-size copy spent its time.
 *
 * tools/scaletest.c renders the difference; docs/VIDEO-PACING.md has the
 * measurements.
 */
enum nv12_scale {
	NV12_SCALE_HW = 0,  /* no CPU scaling: hand VOP2 the full frame */
	NV12_SCALE_BOX,     /* 2x2 average: every source pixel counted once */
	NV12_SCALE_SHARP,   /* separable [-1 9 9 -1]/16, a box with acutance */
};

/*
 * Halve an I420 luma plane into the NV12 luma plane. dst_w/dst_h are the
 * DESTINATION dimensions; the source must be exactly twice them in both
 * axes. Writes each destination row once, forward, and never reads it.
 */
void nv12_scale2_luma(uint8_t *dst, int dst_stride, const uint8_t *src,
		      int src_stride, int dst_w, int dst_h,
		      enum nv12_scale mode);

/*
 * Halve the U and V planes and interleave them. dst_w/dst_h are the
 * destination LUMA dimensions, so this writes dst_w/2 x dst_h/2 UV pairs
 * from planes that are dst_w x dst_h. Always a box: chroma is already at
 * half resolution and sharpening it only rings the colour edges.
 */
void nv12_scale2_chroma(uint8_t *dst, int dst_stride, const uint8_t *u,
			int u_stride, const uint8_t *v, int v_stride,
			int dst_w, int dst_h);

#endif /* XCLOUD_NV12_H */
