/*
 * uitest - prove the on-screen UI layer before wiring it to real auth.
 *
 * Renders a mock device-code screen at the panel's native 640x480 (no
 * scaling), holds it, then a mock catalog list, then exits and restores the
 * display. This is the M3 UI surface exactly as the real screens will use it.
 *
 *   timeout 30 ./uitest [seconds_per_screen]
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ui/screen.h"
#include "../src/ui/text.h"
#include "../src/video/drm_output.h"

static struct drm_out g_out;
static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* Hold the current back buffer on screen for roughly `seconds`. */
static int hold(struct drm_out *o, double seconds)
{
	int frames = (int)(seconds * 60);

	while (frames-- > 0 && !g_stop) {
		o->back = 0;              /* keep presenting the same buffer */
		if (drm_out_present(o))
			return -1;
	}
	o->back = 0;
	return g_stop ? -1 : 0;
}

int main(int argc, char **argv)
{
	struct text_ctx *big, *mid, *small, *mono;
	struct drm_out_buf *b;
	double secs = argc > 1 ? atof(argv[1]) : 6.0;
	int rc = 1, W = 640, H = 480;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);

	if (drm_out_open(&g_out, "/dev/dri/card0"))
		return 1;
	/* Native panel size: fills the screen, no scaling. */
	if (drm_out_configure(&g_out, W, H))
		goto out;

	big = text_create(TEXT_FONT_DEFAULT, 34);
	mid = text_create(TEXT_FONT_DEFAULT, 22);
	small = text_create(TEXT_FONT_DEFAULT, 17);
	mono = text_create(TEXT_FONT_MONO, 40);
	if (!big || !mid || !small || !mono) {
		fprintf(stderr, "uitest: font load failed\n");
		goto out;
	}

	/* ---- screen 1: device code ---- */
	b = drm_out_back_buffer(&g_out);
	screen_clear(b->luma, b->chroma, b->pitch, W, H, 16);
	screen_rect(b->luma, b->pitch, W, H, 0, 0, W, 56, 40);
	text_draw_centred(big, b->luma, b->pitch, W, H, 40, "Xbox Cloud Gaming", 235);
	text_draw_centred(mid, b->luma, b->pitch, W, H, 130,
			  "On a phone or PC, open", 200);
	text_draw_centred(mid, b->luma, b->pitch, W, H, 165,
			  "microsoft.com/link", 235);
	text_draw_centred(small, b->luma, b->pitch, W, H, 225,
			  "and enter this code", 180);
	screen_rect(b->luma, b->pitch, W, H, 120, 250, 400, 70, 45);
	text_draw_centred(mono, b->luma, b->pitch, W, H, 302, "F7K2-9QLM", 235);
	text_draw_centred(small, b->luma, b->pitch, W, H, 400,
			  "Waiting for sign-in...", 150);
	text_draw_centred(small, b->luma, b->pitch, W, H, 445,
			  "B to cancel", 120);
	fprintf(stderr, "uitest: device code screen\n");
	if (hold(&g_out, secs))
		goto out;

	/* ---- screen 2: catalog list ---- */
	{
		static const char *games[] = {
			"Forza Horizon 5", "Halo Infinite", "Sea of Thieves",
			"Grounded", "Hi-Fi RUSH", "Pentiment",
			"Age of Empires II", "Among Us",
		};
		const int n = (int)(sizeof(games) / sizeof(games[0]));
		const int selected = 2;
		int lh = text_line_height(mid) + 6;
		int y0 = 110;

		b = drm_out_back_buffer(&g_out);
		screen_clear(b->luma, b->chroma, b->pitch, W, H, 16);
		screen_rect(b->luma, b->pitch, W, H, 0, 0, W, 56, 40);
		text_draw(big, b->luma, b->pitch, W, H, 20, 40, "Your library", 235);
		text_draw(small, b->luma, b->pitch, W, H, 430, 38, "585 playable", 170);

		for (int i = 0; i < n; i++) {
			int y = y0 + i * lh;

			if (i == selected) {
				screen_rect(b->luma, b->pitch, W, H, 12,
					    y - lh + 8, W - 24, lh, 60);
				text_draw(mid, b->luma, b->pitch, W, H, 24, y,
					  games[i], 235);
			} else {
				text_draw(mid, b->luma, b->pitch, W, H, 24, y,
					  games[i], 175);
			}
		}
		text_draw_centred(small, b->luma, b->pitch, W, H, 455,
				  "D-pad to move    A to play    B to sign out", 130);
		fprintf(stderr, "uitest: catalog screen\n");
		if (hold(&g_out, secs))
			goto out;
	}

	rc = 0;
out:
	drm_out_close(&g_out);
	return rc;
}
