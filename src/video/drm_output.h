/*
 * drm_output - present NV12 frames on a VOP2 overlay plane.
 *
 * Target: Anbernic RG353, RK3566, rockchip-drm VOP2, kernel 4.19 BSP.
 * The panel is 640x480 and the stream is 16:9, so we hand the plane a
 * full-size NV12 buffer and let VOP2 scale it into a centred letterbox.
 * No EGL, no Mali blob, no SDL.
 */
#ifndef XCLOUD_DRM_OUTPUT_H
#define XCLOUD_DRM_OUTPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_OUT_BUFFERS 2
#define DRM_OUT_MAX_SAVED 8

struct drm_out_buf {
	uint32_t handle;
	uint32_t fb_id;
	uint32_t pitch;
	uint64_t size;
	uint8_t *map;      /* write-combine: write only, never read */
	uint8_t *luma;     /* == map */
	uint8_t *chroma;   /* map + pitch * src_h */
};

/* Cached atomic property ids for our plane. */
struct plane_props {
	uint32_t fb_id, crtc_id;
	uint32_t src_x, src_y, src_w, src_h;
	uint32_t crtc_x, crtc_y, crtc_w, crtc_h;
	uint32_t zpos;
};

/*
 * A plane we switched off so it could not sit on top of us, plus everything
 * needed to put it back exactly as it was.
 */
struct saved_plane {
	uint32_t plane_id;
	uint32_t crtc_id;
	uint32_t fb_id;
	uint32_t crtc_x, crtc_y, crtc_w, crtc_h;
	uint32_t src_x, src_y, src_w, src_h;   /* 16.16 fixed point */
};

struct drm_out {
	int fd;
	int have_master;
	int atomic;

	uint32_t crtc_id;
	uint32_t plane_id;
	int crtc_index;        /* index in drmModeRes.crtcs, for vblank waits */
	int crtc_w, crtc_h;
	struct plane_props props;
	int flip_pending;
	uint64_t last_flip_us;  /* vblank IRQ time of the last flip event */
	uint64_t flips;         /* flip events received */
	uint32_t commit_seq;    /* user_data of the commit we are waiting on */
	uint32_t stale_events;  /* events for a commit we gave up waiting on */

	/* Source (decoded) size and the on-screen rectangle VOP2 scales into. */
	int src_w, src_h;
	int dst_x, dst_y, dst_w, dst_h;
	/*
	 * The part of the source the plane actually reads. Normally the whole
	 * frame; in fill mode it is a centre crop of the source's own aspect
	 * ratio, so 16:9 content fills a 4:3 panel with the sides cut off
	 * instead of being letterboxed. VOP2 already scales the plane, so
	 * cropping costs nothing: it is the same commit with a smaller source
	 * rectangle.
	 */
	int crop_x, crop_y, crop_w, crop_h;
	int zoom_fill;         /* 0 = letterbox (default), 1 = fill the panel */
	int pan;               /* -100..100 across the slack a crop leaves */

	struct drm_out_buf bufs[DRM_OUT_BUFFERS];
	int nbufs;
	int back;              /* index of the buffer the caller is filling */

	struct saved_plane saved[DRM_OUT_MAX_SAVED];
	int nsaved;

	int zpos_valid;
	uint64_t our_zpos;     /* zpos we set, and the value to restore */
	uint64_t orig_zpos;
	uint32_t zpos_prop_id;
};

/* Open the card and take DRM master. Returns 0 on success. */
int drm_out_open(struct drm_out *o, const char *path);

/*
 * Pick an NV12-capable plane on the active CRTC, allocate buffers for a
 * src_w x src_h source, compute the letterboxed destination rectangle, and
 * take the CRTC over by switching off any plane that would occlude us.
 */
int drm_out_configure(struct drm_out *o, int src_w, int src_h);

/* The buffer the caller should write the next frame into. */
struct drm_out_buf *drm_out_back_buffer(struct drm_out *o);

/*
 * The on-screen rectangle a src_w x src_h source would be letterboxed into,
 * without changing anything. The present path uses it to ask whether halving
 * the source on the CPU would land exactly on the panel, in which case it
 * can do the downscale itself and leave the plane nothing to scale.
 */
void drm_out_fit(const struct drm_out *o, int src_w, int src_h, int *w, int *h);

/*
 * Change the source size in place, keeping the CRTC we already took over.
 * Use this to switch between the 640x480 UI and 1280x720 video: calling
 * drm_out_configure a second time would append to bufs[] and overrun it, and
 * releasing the CRTC in between would flash EmulationStation back on screen.
 */
int drm_out_set_source(struct drm_out *o, int src_w, int src_h);

/*
 * How the picture is fitted to the panel. fill=0 letterboxes 16:9 into
 * 640x360 with bars; fill=1 crops the source to 4:3 and uses the whole
 * screen, losing a quarter of the width. pan is -100..100 across whatever
 * slack the crop leaves, 0 being centred.
 *
 * Costs nothing: VOP2 already scales the plane, so this only changes the
 * source and destination rectangles of the next commit. Safe on a running
 * stream -- no buffer is reallocated.
 */
void drm_out_set_zoom(struct drm_out *o, int fill, int pan);

/* Show the back buffer, then swap. Blocks until the flip completes. */
int drm_out_present(struct drm_out *o);

/*
 * The same in two halves, for a vsync-paced loop that wants to do other work
 * while the flip is in flight: commit the back buffer (non-blocking, swaps
 * immediately), do the shadow work, then wait for the flip event. Exactly
 * one commit may be outstanding: a second before the event would block
 * inside the driver's flush_work.
 */
int drm_out_commit(struct drm_out *o);
int drm_out_wait_flip(struct drm_out *o, int timeout_ms);

/*
 * Commit the buffer that is already on screen again, without swapping. This
 * is how a held (repeated) picture still produces exactly one flip event per
 * refresh, so the present loop is paced by the same wait whether or not it
 * had a new frame. Falls back to drm_out_wait_vblank semantics if it fails.
 */
int drm_out_recommit(struct drm_out *o);

/*
 * Wait for the next vblank without committing anything, for refreshes where
 * the picture is held. Returns 0 on success.
 */
int drm_out_wait_vblank(struct drm_out *o);

/* Restore everything we changed and close. Safe to call twice. */
void drm_out_close(struct drm_out *o);

#ifdef __cplusplus
}
#endif

#endif /* XCLOUD_DRM_OUTPUT_H */
