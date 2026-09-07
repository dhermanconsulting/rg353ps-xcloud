/*
 * sim_view - watch the host simulator in a browser, and drive it.
 *
 * The simulator runs the real client with a fake panel, so until now the
 * only way to see what it drew was a directory of PNGs after the fact.
 * That is fine for counting frames and useless for judging a picture,
 * which is exactly what the downscale filters need.
 *
 * With XCLOUD_SIM_VIEW=<port> set, drm_output_sim starts a small HTTP
 * server on that port. Point a browser at it and the page shows every
 * frame as it is presented, and sends the keyboard back as gamepad state.
 *
 * Frames go out as PNG parts of a multipart/x-mixed-replace response --
 * the old MJPEG trick with a lossless codec, because the whole point is to
 * look at what the scaler did to the text and JPEG would add artefacts of
 * its own on top. What the browser shows is exactly the buffer the plane
 * would have scanned out.
 *
 * Host simulator only: nothing here is built for the device.
 */
#ifndef XCLOUD_SIM_VIEW_H
#define XCLOUD_SIM_VIEW_H

#include <stddef.h>
#include <stdint.h>

/*
 * Start serving on `port`, 0 for off. Returns 0 if the view is running.
 * Safe to call once; drm_out_close() stops it.
 */
int sim_view_start(int port);
void sim_view_stop(void);

/*
 * Offer the frame that was just committed. Copies it and returns, so the
 * present loop never waits for a browser; a frame that arrives while the
 * last one is still being encoded simply replaces it. No-op when the view
 * is not running.
 */
void sim_view_publish(const uint8_t *luma, const uint8_t *chroma, int pitch,
		      int w, int h);

/*
 * NV12 -> PNG in memory, BT.601 limited range (what the decoder emits).
 * Returns a malloc'd buffer the caller frees, or NULL. Shared with the
 * screenshot writer so there is one place that knows the conversion.
 */
uint8_t *sim_png_encode(const uint8_t *luma, const uint8_t *chroma, int pitch,
			int w, int h, size_t *len_out);

#endif /* XCLOUD_SIM_VIEW_H */
