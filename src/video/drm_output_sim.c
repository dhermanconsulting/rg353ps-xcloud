/*
 * drm_output_sim - the HOST SIMULATOR stand-in for drm_output.c.
 *
 * NEVER BUILT FOR THE DEVICE. scripts/build-host.sh links this file in place
 * of drm_output.c so the same client (src/app/main.cpp and everything under
 * it) runs on an x86-64 Linux box with no display: the "plane" is a pair of
 * malloc'd NV12 buffers and the "panel" is a clock. The device build
 * (scripts/build-app.sh) never sees this file.
 *
 * What it reproduces, and why:
 *
 *   - The vblank cadence. The real panel runs at 60.3 Hz -- 16.58 ms between
 *     the kernel's flip timestamps (docs/VIDEO-PACING.md) -- slightly faster
 *     than the 60 fps source, so one held frame every ~3 s is the physics.
 *     Simulated vblanks sit on a fixed grid of that period, and a commit
 *     latches at the first vblank AFTER it, just as a commit that misses a
 *     vblank on the hardware lands on the next one. So `late=` in the pace|
 *     line means the same thing here as there: the presenter took longer
 *     than a refresh between commits. XCLOUD_SIM_HZ overrides the rate.
 *   - last_flip_us is the vblank time, not the time we woke up, exactly as
 *     the page-flip event's timestamp is on the device. flips, commit_seq,
 *     stale_events and flip_pending are kept so the callers' accounting and
 *     log lines read the same.
 *   - The two-buffer discipline: commit swaps `back`, recommit repeats the
 *     front buffer without swapping, exactly one commit may be outstanding,
 *     and a flip we time out on is forgotten by bumping commit_seq.
 *
 * What it does not: there is no driver worker, so a commit costs nothing
 * and the commit= column reads 0; and nothing scales, so dst_* are computed
 * (same letterbox arithmetic) but only logged.
 *
 * Screenshots: with XCLOUD_SIM_SHOTS=<dir>, every XCLOUD_SIM_SHOT_EVERY-th
 * (default 60) presented frame, and EVERY frame presented at the UI's
 * 640x480, is written as <dir>/frame-NNNNNN.png, NNNNNN being the presented
 * frame count. The NV12 -> RGB conversion (BT.601 limited range, what the
 * decoder emits) and the PNG encode (zlib directly, no libpng) run on their
 * own thread from a copy of the buffer taken at commit time: a 720p PNG is
 * tens of milliseconds, which on the present thread would miss refreshes
 * and corrupt the very numbers this simulator exists to produce.
 */
#define _GNU_SOURCE
#include "drm_output.h"
#include "sim_view.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The panel the plane scales into, as on the device. */
#define SIM_PANEL_W 640
#define SIM_PANEL_H 480
/* Measured from the kernel's flip timestamps on the RG353: 16.58 ms. */
#define SIM_DEFAULT_HZ 60.3
/*
 * Every frame at the UI's exact size is a screen worth a picture. Width
 * alone is not enough: with -scale box|sharp the client halves 1280x720 to
 * 640x360 itself, which shares the UI's width and would otherwise have every
 * video frame written out as a PNG.
 */
#define SIM_UI_W 640
#define SIM_UI_H 480
#define SIM_SHOT_QUEUE 4      /* copies in flight before shots are dropped */

/* ---- simulated vblank clock --------------------------------------------- */

static uint64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until_ns(uint64_t when)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(when / 1000000000ull);
	ts.tv_nsec = (long)(when % 1000000000ull);
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
		;
}

/* One shot waiting to be encoded: a private copy of a committed buffer. */
struct shot {
	uint8_t *nv12;
	int w, h, pitch;
	uint64_t frame;
};

/* One output per process, like the real driver's g_event_out. */
static struct sim {
	int open;
	uint64_t period_ns;    /* vblank period */
	uint64_t epoch_ns;     /* vblank 0: the grid every flip lands on */
	uint64_t commit_ns;    /* when the outstanding commit was made */
	int pending_buf;       /* buffer index that commit shows */
	uint64_t presented;    /* new frames put on screen (not recommits) */

	/* Screenshot writer. */
	char *shot_dir;
	unsigned shot_every;
	pthread_t writer;
	int writer_running;
	pthread_mutex_t lock;
	pthread_cond_t cv;
	struct shot queue[SIM_SHOT_QUEUE];
	int qhead, qlen;
	int quit;
	uint64_t shots_written, shots_dropped;
} g;

/* ---- PNG ---------------------------------------------------------------- */

/*
 * The encoder itself lives in sim_view.c, which needs the same NV12 -> PNG
 * for the browser stream. Same picture, written to a file instead of a
 * socket.
 */
static int write_png(const struct shot *s, const char *path)
{
	size_t len;
	uint8_t *png = sim_png_encode(s->nv12,
				      s->nv12 + (size_t)s->pitch * s->h,
				      s->pitch, s->w, s->h, &len);
	FILE *fp;
	int rc;

	if (!png)
		return -1;
	fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "drm-sim: %s: %s\n", path, strerror(errno));
		free(png);
		return -1;
	}
	rc = fwrite(png, 1, len, fp) == len ? 0 : -1;
	if (rc)
		fprintf(stderr, "drm-sim: short write on %s\n", path);
	fclose(fp);
	free(png);
	return rc;
}


static void *writer_main(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "xc-shots");
	for (;;) {
		struct shot s;
		char path[4096];

		pthread_mutex_lock(&g.lock);
		while (!g.qlen && !g.quit)
			pthread_cond_wait(&g.cv, &g.lock);
		if (!g.qlen && g.quit) {
			pthread_mutex_unlock(&g.lock);
			return NULL;
		}
		s = g.queue[g.qhead];
		g.qhead = (g.qhead + 1) % SIM_SHOT_QUEUE;
		g.qlen--;
		pthread_mutex_unlock(&g.lock);

		snprintf(path, sizeof(path), "%s/frame-%06llu.png", g.shot_dir,
			 (unsigned long long)s.frame);
		if (write_png(&s, path) == 0)
			g.shots_written++;
		free(s.nv12);
	}
}

/* Present thread: copy the committed buffer and hand it to the writer. */
static void queue_shot(struct drm_out *o, int idx, uint64_t frame)
{
	struct drm_out_buf *b = &o->bufs[idx];
	struct shot s;

	s.w = o->src_w;
	s.h = o->src_h;
	s.pitch = (int)b->pitch;
	s.frame = frame;
	s.nv12 = malloc(b->size);
	if (!s.nv12)
		return;
	memcpy(s.nv12, b->map, b->size);

	pthread_mutex_lock(&g.lock);
	if (g.qlen >= SIM_SHOT_QUEUE) {
		/* The encoder is behind; dropping a picture is better than
		 * growing memory or stalling the presenter. */
		g.shots_dropped++;
		pthread_mutex_unlock(&g.lock);
		free(s.nv12);
		return;
	}
	g.queue[(g.qhead + g.qlen) % SIM_SHOT_QUEUE] = s;
	g.qlen++;
	pthread_cond_signal(&g.cv);
	pthread_mutex_unlock(&g.lock);
}

static void writer_start(void)
{
	pthread_mutex_init(&g.lock, NULL);
	pthread_cond_init(&g.cv, NULL);
	if (mkdir(g.shot_dir, 0755) && errno != EEXIST)
		fprintf(stderr, "drm-sim: mkdir %s: %s\n", g.shot_dir,
			strerror(errno));
	if (pthread_create(&g.writer, NULL, writer_main, NULL) == 0)
		g.writer_running = 1;
	else
		fprintf(stderr, "drm-sim: no screenshot thread: %s\n",
			strerror(errno));
}

/* Let the queue drain (the last UI screen is usually the one wanted). */
static void writer_stop(void)
{
	if (!g.writer_running)
		return;
	pthread_mutex_lock(&g.lock);
	g.quit = 1;
	pthread_cond_broadcast(&g.cv);
	pthread_mutex_unlock(&g.lock);
	pthread_join(g.writer, NULL);
	g.writer_running = 0;
}

/* ---- the drm_out API ---------------------------------------------------- */

int drm_out_open(struct drm_out *o, const char *path)
{
	const char *env;
	double hz = SIM_DEFAULT_HZ;

	memset(o, 0, sizeof(*o));
	memset(&g, 0, sizeof(g));
	o->fd = -1;          /* no device; nothing outside this file reads it */
	o->have_master = 1;
	o->atomic = 1;

	env = getenv("XCLOUD_SIM_HZ");
	if (env && atof(env) > 0)
		hz = atof(env);
	g.period_ns = (uint64_t)(1e9 / hz + 0.5);
	g.epoch_ns = mono_ns();
	g.shot_every = 60;
	env = getenv("XCLOUD_SIM_SHOT_EVERY");
	if (env)
		g.shot_every = (unsigned)atoi(env);
	env = getenv("XCLOUD_SIM_SHOTS");
	if (env && *env)
		g.shot_dir = strdup(env);
	/* XCLOUD_SIM_VIEW=<port>: the same frames, live, in a browser. */
	env = getenv("XCLOUD_SIM_VIEW");
	if (env && atoi(env) > 0)
		sim_view_start(atoi(env));
	g.open = 1;

	fprintf(stderr, "drm-sim: SIMULATED display (%s ignored): panel %dx%d "
			"at %.2f Hz (%.3f ms)\n",
		path ? path : "(null)", SIM_PANEL_W, SIM_PANEL_H, hz,
		g.period_ns / 1e6);
	if (g.shot_dir) {
		fprintf(stderr, "drm-sim: screenshots -> %s/frame-NNNNNN.png, "
				"every %u video frames and every UI frame\n",
			g.shot_dir, g.shot_every);
		writer_start();
	}
	return 0;
}

/* Fit the source into the panel, preserving aspect: same as drm_output.c. */
void drm_out_fit(const struct drm_out *o, int src_w, int src_h, int *w, int *h)
{
	int fit_w = o->crtc_w;
	int fit_h = (int)((long long)o->crtc_w * src_h / src_w);

	if (fit_h > o->crtc_h) {
		fit_h = o->crtc_h;
		fit_w = (int)((long long)o->crtc_h * src_w / src_h);
	}
	*w = fit_w & ~1;
	*h = fit_h & ~1;
}

static void letterbox(struct drm_out *o, int src_w, int src_h)
{
	drm_out_fit(o, src_w, src_h, &o->dst_w, &o->dst_h);
	o->dst_x = (o->crtc_w - o->dst_w) / 2;
	o->dst_y = (o->crtc_h - o->dst_h) / 2;
}

static int alloc_buffer(struct drm_out *o, struct drm_out_buf *b, int idx)
{
	/* A 64-byte stride, as the kernel would hand back, so a caller that
	 * confused pitch with width would be caught here too. */
	b->pitch = ((uint32_t)o->src_w + 63u) & ~63u;
	b->size = (uint64_t)b->pitch * (uint64_t)o->src_h * 3 / 2;
	b->map = malloc(b->size);
	if (!b->map) {
		fprintf(stderr, "drm-sim: out of memory for a %dx%d buffer\n",
			o->src_w, o->src_h);
		return -1;
	}
	b->handle = (uint32_t)idx + 1;   /* non-zero, as real ones are */
	b->fb_id = (uint32_t)idx + 1;
	b->luma = b->map;
	b->chroma = b->map + (size_t)b->pitch * o->src_h;
	/* Black, so a dropped frame never shows uninitialised memory. */
	memset(b->luma, 0x10, (size_t)b->pitch * o->src_h);
	memset(b->chroma, 0x80, (size_t)b->pitch * (o->src_h / 2));
	return 0;
}

static void free_buffers(struct drm_out *o)
{
	for (int i = 0; i < o->nbufs; i++) {
		free(o->bufs[i].map);
		memset(&o->bufs[i], 0, sizeof(o->bufs[i]));
	}
	o->nbufs = 0;
}

static int wait_for_flip(struct drm_out *o, int timeout_ms);

int drm_out_configure(struct drm_out *o, int src_w, int src_h)
{
	o->crtc_id = 1;
	o->plane_id = 1;
	o->crtc_index = 0;
	o->crtc_w = SIM_PANEL_W;
	o->crtc_h = SIM_PANEL_H;
	o->src_w = src_w;
	o->src_h = src_h;
	fprintf(stderr, "drm: crtc %u (%dx%d) plane %u [simulated]\n",
		o->crtc_id, o->crtc_w, o->crtc_h, o->plane_id);

	letterbox(o, src_w, src_h);
	fprintf(stderr, "drm: %dx%d -> %dx%d at (%d,%d)\n", src_w, src_h,
		o->dst_w, o->dst_h, o->dst_x, o->dst_y);
	fprintf(stderr, "drm: present path = simulated vblank\n");

	for (int i = 0; i < DRM_OUT_BUFFERS; i++) {
		if (alloc_buffer(o, &o->bufs[i], i))
			return -1;
		o->nbufs++;
	}
	o->back = 0;
	return 0;
}

void drm_out_set_zoom(struct drm_out *o, int fill, int pan)
{
	/* The simulated panel has no plane to crop with, and the PNGs it
	 * writes are of the buffer rather than of scanout, so this only
	 * records the choice: it is here so the client links and so a
	 * simulator run logs what it would have asked the plane for. */
	if (o->zoom_fill == !!fill && o->pan == pan)
		return;
	o->zoom_fill = !!fill;
	o->pan = pan;
	fprintf(stderr, "drm-sim: %s (pan %d), not reproduced here\n",
		fill ? "fill" : "letterbox", pan);
}

int drm_out_set_source(struct drm_out *o, int src_w, int src_h)
{
	if (o->src_w == src_w && o->src_h == src_h && o->nbufs)
		return 0;

	/* A queued flip still references the old buffer. */
	wait_for_flip(o, 100);
	free_buffers(o);

	o->src_w = src_w;
	o->src_h = src_h;
	letterbox(o, src_w, src_h);
	fprintf(stderr, "drm: source now %dx%d -> %dx%d at (%d,%d)\n", src_w,
		src_h, o->dst_w, o->dst_h, o->dst_x, o->dst_y);

	for (int i = 0; i < DRM_OUT_BUFFERS; i++) {
		if (alloc_buffer(o, &o->bufs[i], i))
			return -1;
		o->nbufs++;
	}
	o->back = 0;
	return 0;
}

struct drm_out_buf *drm_out_back_buffer(struct drm_out *o)
{
	return &o->bufs[o->back];
}

/*
 * Queue a "commit": it will latch at the first vblank after now. `fresh`
 * is a new picture (commit/present) as opposed to a repeat (recommit); only
 * fresh ones count as presented and are candidates for a screenshot.
 */
static void sim_commit(struct drm_out *o, int idx, int fresh)
{
	o->flip_pending = 1;
	o->commit_seq++;
	g.commit_ns = mono_ns();
	g.pending_buf = idx;
	if (!fresh)
		return;
	g.presented++;
	if (g.writer_running &&
	    ((o->src_w == SIM_UI_W && o->src_h == SIM_UI_H) ||
	     (g.shot_every && g.presented % g.shot_every == 0)))
		queue_shot(o, idx, g.presented);
	/* Every frame goes to the browser view, which copies and returns; it
	 * drops frames on its own side rather than holding this thread up. */
	{
		struct drm_out_buf *b = &o->bufs[idx];

		sim_view_publish(b->luma, b->chroma, (int)b->pitch, o->src_w,
				 o->src_h);
	}
}

/*
 * The flip "event": sleep until the vblank the outstanding commit latches
 * at, then stamp it. The grid runs from open, so a presenter that is late
 * for one vblank lands on the next and shows a doubled interval, exactly as
 * the kernel's timestamps would.
 */
static int wait_for_flip(struct drm_out *o, int timeout_ms)
{
	uint64_t now, target, deadline;

	if (!o->flip_pending)
		return 0;
	target = g.epoch_ns +
		 ((g.commit_ns - g.epoch_ns) / g.period_ns + 1) * g.period_ns;
	now = mono_ns();
	deadline = now + (uint64_t)timeout_ms * 1000000ull;
	if (target > deadline) {
		/* Cannot happen at a sane rate (the target is at most one
		 * period away), but keep the real semantics: give up, and
		 * make sure this commit's event is never mistaken for the
		 * next one's. */
		sleep_until_ns(deadline);
		fprintf(stderr, "drm: flip timed out after %d ms\n", timeout_ms);
		o->flip_pending = 0;
		o->commit_seq++;
		return -1;
	}
	if (target > now)
		sleep_until_ns(target);
	o->last_flip_us = target / 1000;
	o->flips++;
	o->flip_pending = 0;
	return 0;
}

int drm_out_commit(struct drm_out *o)
{
	if (o->flip_pending && wait_for_flip(o, 200))
		return -1;
	sim_commit(o, o->back, 1);
	o->back = (o->back + 1) % o->nbufs;
	return 0;
}

int drm_out_wait_flip(struct drm_out *o, int timeout_ms)
{
	return wait_for_flip(o, timeout_ms);
}

int drm_out_present(struct drm_out *o)
{
	if (drm_out_commit(o))
		return -1;
	return wait_for_flip(o, 200);
}

int drm_out_recommit(struct drm_out *o)
{
	/* The visible buffer is the one before `back`, and before any flip
	 * at all there is nothing to repeat -- same rule as the real one. */
	if (!o->flips)
		return -1;
	if (o->flip_pending && wait_for_flip(o, 200))
		return -1;
	sim_commit(o, (o->back + o->nbufs - 1) % o->nbufs, 0);
	return 0;
}

int drm_out_wait_vblank(struct drm_out *o)
{
	uint64_t now = mono_ns();

	(void)o;
	/* Like drmWaitVBlank, this paces but does not stamp last_flip_us:
	 * only a flip event does. */
	sleep_until_ns(g.epoch_ns +
		       ((now - g.epoch_ns) / g.period_ns + 1) * g.period_ns);
	return 0;
}

void drm_out_close(struct drm_out *o)
{
	if (!g.open)
		return;
	wait_for_flip(o, 100);
	writer_stop();
	sim_view_stop();
	free_buffers(o);
	fprintf(stderr, "drm-sim: %llu flips, %llu frames presented",
		(unsigned long long)o->flips, (unsigned long long)g.presented);
	if (g.shot_dir)
		fprintf(stderr, ", %llu screenshots written, %llu dropped",
			(unsigned long long)g.shots_written,
			(unsigned long long)g.shots_dropped);
	fprintf(stderr, "\n");
	free(g.shot_dir);
	g.shot_dir = NULL;
	g.open = 0;
	o->fd = -1;
}
