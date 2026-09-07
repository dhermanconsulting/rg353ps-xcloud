/*
 * playfile - M2 milestone harness.
 *
 * Decode an H.264 file in software and show it fullscreen on the RG353's
 * VOP2 overlay plane, letterboxed, with the display controller doing the
 * downscale. Proves the whole video path before any WebRTC exists.
 *
 *   ./playfile clip.mp4 [max_frames]
 *
 * Always run it under `timeout` while developing, so a wedged modeset
 * clears itself:
 *   timeout 30 ./playfile clip.mp4
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/media/decoder.h"
#include "../src/video/drm_output.h"
#include "../src/video/nv12.h"

static struct drm_out g_out;
static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	struct decoder dec;
	long max_frames = 0;
	long frames = 0;
	double t_start, t_decode = 0, t_convert = 0, t_present = 0;
	int rc = 1;

	if (argc < 2) {
		fprintf(stderr, "usage: playfile <file> [max_frames]\n");
		return 2;
	}
	if (argc > 2)
		max_frames = strtol(argv[2], NULL, 10);

	/* Restore the display even on Ctrl-C or SIGTERM from `timeout`. */
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);

	if (decoder_open_file(&dec, argv[1]))
		return 1;

	if (drm_out_open(&g_out, "/dev/dri/card0"))
		goto out_decoder;
	if (drm_out_configure(&g_out, dec.ctx->width, dec.ctx->height))
		goto out_drm;

	/*
	 * Show a solid colour first. This separates "can we own the screen"
	 * from "is the decode path right": if this appears and the video does
	 * not, the bug is in decode or pixel format, not in DRM.
	 */
	{
		struct drm_out_buf *b = drm_out_back_buffer(&g_out);
		int held = 0;

		memset(b->luma, 0x51, (size_t)b->pitch * g_out.src_h);
		memset(b->chroma, 0x5b, (size_t)b->pitch * (g_out.src_h / 2));
		fprintf(stderr, "playfile: showing solid colour for ~2s\n");
		while (held++ < 120 && !g_stop) {
			g_out.back = 0;          /* keep showing this buffer */
			if (drm_out_present(&g_out))
				goto out_drm;
		}
		g_out.back = 0;
	}

	t_start = now_ms();
	while (!g_stop) {
		struct drm_out_buf *b;
		double t0, t1, t2, t3;
		int got;

		t0 = now_ms();
		got = decoder_next_frame(&dec);
		if (got < 0)
			goto out_drm;
		if (got == 0)
			break;
		t1 = now_ms();

		if (dec.frame->format != AV_PIX_FMT_YUV420P) {
			fprintf(stderr, "playfile: unexpected pixel format %d\n",
				dec.frame->format);
			goto out_drm;
		}

		b = drm_out_back_buffer(&g_out);
		nv12_copy_luma(b->luma, b->pitch, dec.frame->data[0],
			       dec.frame->linesize[0], g_out.src_w, g_out.src_h);
		nv12_interleave_chroma(b->chroma, b->pitch, dec.frame->data[1],
				       dec.frame->linesize[1], dec.frame->data[2],
				       dec.frame->linesize[2], g_out.src_w,
				       g_out.src_h);
		t2 = now_ms();

		if (drm_out_present(&g_out))
			goto out_drm;
		t3 = now_ms();

		t_decode += t1 - t0;
		t_convert += t2 - t1;
		t_present += t3 - t2;
		frames++;
		if (max_frames && frames >= max_frames)
			break;
	}

	{
		double elapsed = now_ms() - t_start;

		fprintf(stderr,
			"\nplayfile: %ld frames in %.1f ms = %.1f fps\n"
			"  decode  %7.2f ms/frame\n"
			"  convert %7.2f ms/frame\n"
			"  present %7.2f ms/frame (includes waiting for vblank)\n",
			frames, elapsed, frames ? frames * 1000.0 / elapsed : 0.0,
			frames ? t_decode / frames : 0.0,
			frames ? t_convert / frames : 0.0,
			frames ? t_present / frames : 0.0);
	}
	rc = 0;

out_drm:
	drm_out_close(&g_out);
out_decoder:
	decoder_close(&dec);
	return rc;
}
