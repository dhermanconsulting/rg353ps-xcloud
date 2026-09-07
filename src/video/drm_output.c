#include "drm_output.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <stdint.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* Read a named property value off a DRM object. Returns 0 on success. */
static int get_prop(int fd, uint32_t obj, uint32_t type, const char *name,
		    uint64_t *value, uint32_t *prop_id)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
	int rc = -1;

	if (!props)
		return -1;
	for (uint32_t i = 0; i < props->count_props && rc; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (!p)
			continue;
		if (!strcmp(p->name, name)) {
			if (value)
				*value = props->prop_values[i];
			if (prop_id)
				*prop_id = p->prop_id;
			rc = 0;
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return rc;
}

static uint64_t prop_or(int fd, uint32_t plane, const char *name, uint64_t dflt)
{
	uint64_t v = dflt;

	get_prop(fd, plane, DRM_MODE_OBJECT_PLANE, name, &v, NULL);
	return v;
}

int drm_out_open(struct drm_out *o, const char *path)
{
	memset(o, 0, sizeof(*o));
	o->fd = open(path ? path : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (o->fd < 0) {
		fprintf(stderr, "drm: open %s: %s\n", path, strerror(errno));
		return -1;
	}

	/*
	 * EmulationStation drops master when it launches a port, so this
	 * succeeds there and fails over SSH while ES is up.
	 */
	if (drmSetMaster(o->fd) == 0) {
		o->have_master = 1;
	} else {
		fprintf(stderr, "drm: drmSetMaster failed (%s) - are we running "
				"as a port, or with EmulationStation stopped?\n",
			strerror(errno));
	}

	if (drmSetClientCap(o->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
		fprintf(stderr, "drm: UNIVERSAL_PLANES unavailable\n");
	o->atomic = drmSetClientCap(o->fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0;
	return 0;
}

/* Find the active CRTC by walking connector -> encoder -> crtc. */
static int find_crtc(struct drm_out *o, drmModeRes *res)
{
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(o->fd, res->connectors[i]);
		drmModeEncoder *e;

		if (!c)
			continue;
		if (c->connection != DRM_MODE_CONNECTED || !c->encoder_id) {
			drmModeFreeConnector(c);
			continue;
		}
		e = drmModeGetEncoder(o->fd, c->encoder_id);
		if (e && e->crtc_id) {
			drmModeCrtc *crtc = drmModeGetCrtc(o->fd, e->crtc_id);

			o->crtc_id = e->crtc_id;
			if (crtc) {
				o->crtc_w = crtc->mode.hdisplay;
				o->crtc_h = crtc->mode.vdisplay;
				drmModeFreeCrtc(crtc);
			}
			for (int k = 0; k < res->count_crtcs; k++)
				if (res->crtcs[k] == o->crtc_id)
					o->crtc_index = k;
		}
		if (e)
			drmModeFreeEncoder(e);
		drmModeFreeConnector(c);
		if (o->crtc_id)
			return 0;
	}
	return -1;
}

/*
 * Pick a free plane that advertises NV12 and can drive our CRTC.  Prefer a
 * real OVERLAY: it can never be crtc->primary, so losing its framebuffer can
 * never take the CRTC down with it.
 */
static int find_plane(struct drm_out *o)
{
	drmModePlaneRes *pres = drmModeGetPlaneResources(o->fd);
	uint32_t fallback = 0;

	if (!pres)
		return -1;

	for (uint32_t i = 0; i < pres->count_planes && !o->plane_id; i++) {
		drmModePlane *p = drmModeGetPlane(o->fd, pres->planes[i]);
		uint64_t type = 0;
		int nv12 = 0;

		if (!p)
			continue;
		for (uint32_t f = 0; f < p->count_formats; f++)
			if (p->formats[f] == DRM_FORMAT_NV12)
				nv12 = 1;

		if (nv12 && !p->crtc_id &&
		    (p->possible_crtcs & (1u << o->crtc_index))) {
			get_prop(o->fd, p->plane_id, DRM_MODE_OBJECT_PLANE,
				 "type", &type, NULL);
			if (type == DRM_PLANE_TYPE_OVERLAY)
				o->plane_id = p->plane_id;
			else if (!fallback)
				fallback = p->plane_id;
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(pres);

	if (!o->plane_id)
		o->plane_id = fallback;
	return o->plane_id ? 0 : -1;
}

/*
 * Take the CRTC over.
 *
 * zpos is a trap on this driver.  The property is mutable, the value is
 * accepted, and debugfs duly reports our window at zpos 7 above
 * EmulationStation's at zpos 6 - but every plane still reports
 * normalized-zpos=0, so VOP2's layer mixer never reorders anything and the
 * RGB Smart window keeps compositing on top.  Measured on device: our plane
 * at zpos 7 stayed completely invisible under a plane at zpos 6, with a valid
 * commit and a totally clean dmesg.
 *
 * So do not rely on z-order.  Switch off anything else scanning out on our
 * CRTC, and put it back exactly as it was on the way out.
 */
static void take_over_crtc(struct drm_out *o)
{
	drmModePlaneRes *pres = drmModeGetPlaneResources(o->fd);

	o->orig_zpos = prop_or(o->fd, o->plane_id, "zpos", 0);
	if (get_prop(o->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "zpos", NULL,
		     &o->zpos_prop_id) == 0)
		o->zpos_valid = 1;

	if (!pres)
		return;

	for (uint32_t i = 0; i < pres->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(o->fd, pres->planes[i]);
		struct saved_plane *s;

		if (!p)
			continue;
		if (p->plane_id == o->plane_id || !p->crtc_id ||
		    p->crtc_id != o->crtc_id || o->nsaved >= DRM_OUT_MAX_SAVED) {
			drmModeFreePlane(p);
			continue;
		}

		s = &o->saved[o->nsaved++];
		s->plane_id = p->plane_id;
		s->crtc_id = p->crtc_id;
		s->fb_id = p->fb_id;
		s->crtc_x = (uint32_t)prop_or(o->fd, p->plane_id, "CRTC_X", 0);
		s->crtc_y = (uint32_t)prop_or(o->fd, p->plane_id, "CRTC_Y", 0);
		s->crtc_w = (uint32_t)prop_or(o->fd, p->plane_id, "CRTC_W",
					      o->crtc_w);
		s->crtc_h = (uint32_t)prop_or(o->fd, p->plane_id, "CRTC_H",
					      o->crtc_h);
		s->src_x = (uint32_t)prop_or(o->fd, p->plane_id, "SRC_X", 0);
		s->src_y = (uint32_t)prop_or(o->fd, p->plane_id, "SRC_Y", 0);
		s->src_w = (uint32_t)prop_or(o->fd, p->plane_id, "SRC_W",
					     (uint64_t)o->crtc_w << 16);
		s->src_h = (uint32_t)prop_or(o->fd, p->plane_id, "SRC_H",
					     (uint64_t)o->crtc_h << 16);

		fprintf(stderr, "drm: disabling occluding plane %u (fb %u)\n",
			s->plane_id, s->fb_id);
		drmModeSetPlane(o->fd, s->plane_id, s->crtc_id, 0, 0,
				0, 0, 0, 0, 0, 0, 0, 0);
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(pres);
}

static void restore_planes(struct drm_out *o)
{
	for (int i = 0; i < o->nsaved; i++) {
		struct saved_plane *s = &o->saved[i];

		if (!s->fb_id)
			continue;
		if (drmModeSetPlane(o->fd, s->plane_id, s->crtc_id, s->fb_id, 0,
				    s->crtc_x, s->crtc_y, s->crtc_w, s->crtc_h,
				    s->src_x, s->src_y, s->src_w, s->src_h))
			fprintf(stderr, "drm: could not restore plane %u: %s "
					"(EmulationStation will redraw)\n",
				s->plane_id, strerror(errno));
	}
	o->nsaved = 0;
}

static void cache_plane_props(struct drm_out *o)
{
	struct { const char *name; uint32_t *slot; } map[] = {
		{ "FB_ID",   &o->props.fb_id },   { "CRTC_ID", &o->props.crtc_id },
		{ "SRC_X",   &o->props.src_x },   { "SRC_Y",   &o->props.src_y },
		{ "SRC_W",   &o->props.src_w },   { "SRC_H",   &o->props.src_h },
		{ "CRTC_X",  &o->props.crtc_x },  { "CRTC_Y",  &o->props.crtc_y },
		{ "CRTC_W",  &o->props.crtc_w },  { "CRTC_H",  &o->props.crtc_h },
	};

	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (get_prop(o->fd, o->plane_id, DRM_MODE_OBJECT_PLANE,
			     map[i].name, NULL, map[i].slot)) {
			fprintf(stderr, "drm: plane property %s missing, "
					"falling back to legacy SetPlane\n",
				map[i].name);
			o->atomic = 0;
			return;
		}
	}
	get_prop(o->fd, o->plane_id, DRM_MODE_OBJECT_PLANE, "zpos", NULL,
		 &o->props.zpos);
}

static int alloc_buffer(struct drm_out *o, struct drm_out_buf *b)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	/*
	 * NV12 at 8bpp is one linear allocation: src_h rows of luma followed
	 * by src_h/2 rows of interleaved chroma, hence height * 3 / 2.
	 */
	memset(&creq, 0, sizeof(creq));
	creq.width = o->src_w;
	creq.height = o->src_h * 3 / 2;
	creq.bpp = 8;
	if (drmIoctl(o->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) {
		fprintf(stderr, "drm: CREATE_DUMB: %s\n", strerror(errno));
		return -1;
	}
	b->handle = creq.handle;
	b->pitch = creq.pitch;   /* always use the pitch the kernel returned */
	b->size = creq.size;

	/* The framebuffer describes the VISIBLE picture, so height is src_h. */
	handles[0] = handles[1] = b->handle;
	pitches[0] = pitches[1] = b->pitch;
	offsets[0] = 0;
	offsets[1] = b->pitch * o->src_h;
	if (drmModeAddFB2(o->fd, o->src_w, o->src_h, DRM_FORMAT_NV12, handles,
			  pitches, offsets, &b->fb_id, 0)) {
		fprintf(stderr, "drm: AddFB2 NV12: %s\n", strerror(errno));
		return -1;
	}

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = b->handle;
	if (drmIoctl(o->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
		fprintf(stderr, "drm: MAP_DUMB: %s\n", strerror(errno));
		return -1;
	}
	b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, o->fd,
		      mreq.offset);
	if (b->map == MAP_FAILED) {
		b->map = NULL;
		fprintf(stderr, "drm: mmap: %s\n", strerror(errno));
		return -1;
	}
	b->luma = b->map;
	b->chroma = b->map + (size_t)b->pitch * o->src_h;

	/* Black, so a dropped frame never shows uninitialised memory. */
	memset(b->luma, 0x10, (size_t)b->pitch * o->src_h);
	memset(b->chroma, 0x80, (size_t)b->pitch * (o->src_h / 2));
	return 0;
}

/* Defined below, next to the page-flip handler it drains. */
static int wait_for_flip(struct drm_out *o, int timeout_ms);

/* Fit the source into the panel, preserving aspect. */
void drm_out_fit(const struct drm_out *o, int src_w, int src_h, int *w, int *h)
{
	int fit_w = o->crtc_w;
	int fit_h = (int)((long long)o->crtc_w * src_h / src_w);

	if (fit_h > o->crtc_h) {
		fit_h = o->crtc_h;
		fit_w = (int)((long long)o->crtc_h * src_w / src_h);
	}
	/* Even dimensions: VOP2 silently bumps odd ones when scaling. */
	*w = fit_w & ~1;
	*h = fit_h & ~1;
}

static void letterbox(struct drm_out *o, int src_w, int src_h)
{
	/* By default the plane reads the whole frame. */
	o->crop_x = o->crop_y = 0;
	o->crop_w = src_w;
	o->crop_h = src_h;

	/*
	 * Fill mode: crop the SOURCE to the panel's aspect ratio and give the
	 * plane the whole screen, rather than fitting the source inside the
	 * screen and blacking out the rest. 1280x720 on this 640x480 panel
	 * becomes a 960x720 centre crop scaled to 640x480 -- the picture
	 * reaches all four edges and a quarter of the width is off-screen.
	 *
	 * Only ever one axis: cropping both would zoom past the point.
	 * Everything stays even because VOP2 silently bumps odd values when
	 * it scales.
	 */
	if (o->zoom_fill && src_w > 0 && src_h > 0 && o->crtc_h > 0) {
		long long want_w =
			(long long)src_h * o->crtc_w / o->crtc_h;

		if (want_w <= src_w) {
			o->crop_w = (int)want_w & ~1;
			o->crop_x = ((src_w - o->crop_w) / 2) & ~1;
			/* Pan across what the crop left over. */
			o->crop_x += (src_w - o->crop_w) * o->pan / 200;
			if (o->crop_x < 0)
				o->crop_x = 0;
			if (o->crop_x > src_w - o->crop_w)
				o->crop_x = src_w - o->crop_w;
			o->crop_x &= ~1;
		} else {
			long long want_h =
				(long long)src_w * o->crtc_h / o->crtc_w;

			o->crop_h = (int)want_h & ~1;
			o->crop_y = ((src_h - o->crop_h) / 2) & ~1;
			o->crop_y += (src_h - o->crop_h) * o->pan / 200;
			if (o->crop_y < 0)
				o->crop_y = 0;
			if (o->crop_y > src_h - o->crop_h)
				o->crop_y = src_h - o->crop_h;
			o->crop_y &= ~1;
		}
		o->dst_x = o->dst_y = 0;
		o->dst_w = o->crtc_w;
		o->dst_h = o->crtc_h;
		return;
	}

	drm_out_fit(o, src_w, src_h, &o->dst_w, &o->dst_h);
	o->dst_x = (o->crtc_w - o->dst_w) / 2;
	o->dst_y = (o->crtc_h - o->dst_h) / 2;
}

/*
 * Change how the picture is fitted, without reallocating anything: only the
 * plane's source and destination rectangles move, so this can be called on a
 * running stream and takes effect on the next commit.
 */
void drm_out_set_zoom(struct drm_out *o, int fill, int pan)
{
	if (o->zoom_fill == !!fill && o->pan == pan)
		return;
	o->zoom_fill = !!fill;
	o->pan = pan;
	letterbox(o, o->src_w, o->src_h);
	fprintf(stderr, "drm: %s, source %d,%d %dx%d -> %dx%d at (%d,%d)\n",
		o->zoom_fill ? "fill" : "letterbox", o->crop_x, o->crop_y,
		o->crop_w, o->crop_h, o->dst_w, o->dst_h, o->dst_x, o->dst_y);
}

int drm_out_configure(struct drm_out *o, int src_w, int src_h)
{
	drmModeRes *res;

	o->src_w = src_w;
	o->src_h = src_h;

	res = drmModeGetResources(o->fd);
	if (!res) {
		fprintf(stderr, "drm: GetResources: %s\n", strerror(errno));
		return -1;
	}
	if (find_crtc(o, res)) {
		fprintf(stderr, "drm: no active CRTC\n");
		drmModeFreeResources(res);
		return -1;
	}
	drmModeFreeResources(res);

	if (find_plane(o)) {
		fprintf(stderr, "drm: no free NV12 plane on CRTC %u\n",
			o->crtc_id);
		return -1;
	}
	fprintf(stderr, "drm: crtc %u (%dx%d) plane %u\n", o->crtc_id,
		o->crtc_w, o->crtc_h, o->plane_id);

	letterbox(o, src_w, src_h);
	fprintf(stderr, "drm: %dx%d -> %dx%d at (%d,%d)\n", src_w, src_h,
		o->dst_w, o->dst_h, o->dst_x, o->dst_y);

	if (o->atomic)
		cache_plane_props(o);
	fprintf(stderr, "drm: present path = %s\n",
		o->atomic ? "atomic + page flip event" : "legacy SetPlane");

	take_over_crtc(o);

	for (int i = 0; i < DRM_OUT_BUFFERS; i++) {
		if (alloc_buffer(o, &o->bufs[i]))
			return -1;
		o->nbufs++;
	}
	o->back = 0;
	return 0;
}

/* Release a set of buffers. Split out so a resolution change can hold on to
 * the old pair while the new pair is put on screen (drm_out_set_source). */
static void free_buffer_array(struct drm_out *o, struct drm_out_buf *bufs,
			      int n)
{
	for (int i = 0; i < n; i++) {
		struct drm_out_buf *b = &bufs[i];
		struct drm_mode_destroy_dumb dreq;

		if (b->map)
			munmap(b->map, b->size);
		if (b->fb_id)
			drmModeRmFB(o->fd, b->fb_id);
		if (b->handle) {
			memset(&dreq, 0, sizeof(dreq));
			dreq.handle = b->handle;
			drmIoctl(o->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		}
		memset(b, 0, sizeof(*b));
	}
}

static void free_buffers(struct drm_out *o)
{
	free_buffer_array(o, o->bufs, o->nbufs);
	o->nbufs = 0;
}

/*
 * Change the source size without giving the CRTC back.
 *
 * The UI runs at the panel's native 640x480 so the plane does no scaling at
 * all, while video is 1280x720 letterboxed by VOP2. Switching between them
 * means new dumb buffers and a new destination rectangle, but the CRTC
 * takeover (and the occluding planes we switched off) must survive: dropping
 * and re-taking it mid-session would flash the EmulationStation framebuffer
 * back on screen. drm_out_configure cannot be called twice -- it appends to
 * bufs[] without freeing, which overruns the array on the second call.
 */
int drm_out_set_source(struct drm_out *o, int src_w, int src_h)
{
	struct drm_out_buf old[DRM_OUT_BUFFERS];
	int old_w = o->src_w, old_h = o->src_h;
	int nold;

	if (o->src_w == src_w && o->src_h == src_h && o->nbufs)
		return 0;

	/* A queued flip still references the current framebuffer. */
	wait_for_flip(o, 100);

	/*
	 * Keep the old buffers until a new one is on screen.
	 *
	 * Freeing the framebuffer the CRTC is scanning out leaves the plane
	 * with nothing to show, and the driver answers that with a
	 * synchronous plane disable: one black refresh plus a 20-40 ms stall,
	 * every time the resolution changes. That was tolerable while
	 * resolution changes were rare (a loading screen, a bandwidth step),
	 * but since the client began declaring 640x360 any title that honours
	 * it changes resolution on the way in -- so the stall moved onto the
	 * path we most want to be good (docs/RESOLUTION.md).
	 *
	 * So: hold the old pair, allocate the new pair, put a new buffer up,
	 * wait for that flip, and only then release the old. The plane goes
	 * straight from one framebuffer to another and is never left empty.
	 * Both pairs exist at once for one flip; at 720p that is a transient
	 * 2.8 MB on a device with 1 GB.
	 */
	nold = o->nbufs;
	memcpy(old, o->bufs, sizeof(old));
	memset(o->bufs, 0, sizeof(o->bufs));
	o->nbufs = 0;

	o->src_w = src_w;
	o->src_h = src_h;
	letterbox(o, src_w, src_h);
	fprintf(stderr, "drm: source now %dx%d -> %dx%d at (%d,%d)\n", src_w,
		src_h, o->dst_w, o->dst_h, o->dst_x, o->dst_y);

	for (int i = 0; i < DRM_OUT_BUFFERS; i++) {
		if (alloc_buffer(o, &o->bufs[i])) {
			/*
			 * No memory for the new size. The old pair is still
			 * allocated and still on screen, so put it back and
			 * keep displaying rather than tearing down a working
			 * plane on the way out.
			 */
			free_buffers(o);
			memcpy(o->bufs, old, sizeof(old));
			o->nbufs = nold;
			o->src_w = old_w;
			o->src_h = old_h;
			letterbox(o, old_w, old_h);
			fprintf(stderr, "drm: kept %dx%d, no buffers for "
					"%dx%d\n", old_w, old_h, src_w, src_h);
			return -1;
		}
		o->nbufs++;
	}
	o->back = 0;

	/*
	 * Show a new-size buffer before the old ones go. alloc_buffer clears
	 * to black, so this puts up exactly what the old code put up -- the
	 * difference is that the plane is handed a framebuffer rather than
	 * having one taken away. drm_out_commit leaves `back` on the buffer
	 * the caller should fill next, so it is not reset afterwards.
	 */
	if (nold) {
		if (drm_out_commit(o) == 0)
			wait_for_flip(o, 200);
		else
			fprintf(stderr, "drm: first commit at the new size "
					"failed; freeing the old buffers "
					"anyway\n");
	}
	free_buffer_array(o, old, nold);
	return 0;
}

struct drm_out_buf *drm_out_back_buffer(struct drm_out *o)
{
	return &o->bufs[o->back];
}

/* One output per process; the event carries our commit sequence instead. */
static struct drm_out *g_event_out;

static void page_flip_handler(int fd, unsigned seq, unsigned tv_sec,
			      unsigned tv_usec, void *data)
{
	struct drm_out *o = g_event_out;
	uint32_t commit = (uint32_t)(uintptr_t)data;

	(void)fd; (void)seq;
	if (!o)
		return;
	/* A flip we stopped waiting for (timeout) can still complete later.
	 * Its event must not be mistaken for the NEXT commit's, or the
	 * presenter would write a buffer that is still queued. */
	if (commit != o->commit_seq) {
		o->stale_events++;
		return;
	}
	/* The kernel stamps the event with the vblank IRQ time
	 * (CLOCK_MONOTONIC): the honest pacing clock, unaffected by how long
	 * we took to wake up and read it. */
	o->last_flip_us = (uint64_t)tv_sec * 1000000ull + tv_usec;
	o->flips++;
	o->flip_pending = 0;
}

/* Wait for the queued flip, bounded so a stuck driver cannot hang us. */
static int wait_for_flip(struct drm_out *o, int timeout_ms)
{
	drmEventContext ev;
	struct pollfd pfd;

	memset(&ev, 0, sizeof(ev));
	ev.version = 2;
	ev.page_flip_handler = page_flip_handler;

	pfd.fd = o->fd;
	pfd.events = POLLIN;
	while (o->flip_pending) {
		int rc = poll(&pfd, 1, timeout_ms);

		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (rc == 0) {
			fprintf(stderr, "drm: flip timed out after %d ms\n",
				timeout_ms);
			/* Give up on this flip but never forget it: bump the
			 * sequence so its event, if it ever comes, is stale. */
			o->flip_pending = 0;
			o->commit_seq++;
			return -1;
		}
		drmHandleEvent(o->fd, &ev);
	}
	return 0;
}

static int atomic_commit(struct drm_out *o, struct drm_out_buf *b, int first)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	int rc;

	if (!req)
		return -1;

	drmModeAtomicAddProperty(req, o->plane_id, o->props.crtc_id, o->crtc_id);
	drmModeAtomicAddProperty(req, o->plane_id, o->props.fb_id, b->fb_id);
	drmModeAtomicAddProperty(req, o->plane_id, o->props.src_x,
				 (uint64_t)o->crop_x << 16);
	drmModeAtomicAddProperty(req, o->plane_id, o->props.src_y,
				 (uint64_t)o->crop_y << 16);
	drmModeAtomicAddProperty(req, o->plane_id, o->props.src_w,
				 (uint64_t)o->crop_w << 16);
	drmModeAtomicAddProperty(req, o->plane_id, o->props.src_h,
				 (uint64_t)o->crop_h << 16);
	drmModeAtomicAddProperty(req, o->plane_id, o->props.crtc_x, o->dst_x);
	drmModeAtomicAddProperty(req, o->plane_id, o->props.crtc_y, o->dst_y);
	drmModeAtomicAddProperty(req, o->plane_id, o->props.crtc_w, o->dst_w);
	drmModeAtomicAddProperty(req, o->plane_id, o->props.crtc_h, o->dst_h);
	if (first && o->props.zpos)
		drmModeAtomicAddProperty(req, o->plane_id, o->props.zpos, 7);

	/*
	 * The mapping is write-combine; make sure the pixels have left the
	 * write buffer before scanout is pointed at them.
	 */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	o->flip_pending = 1;
	o->commit_seq++;
	g_event_out = o;
	rc = drmModeAtomicCommit(o->fd, req,
				 DRM_MODE_ATOMIC_NONBLOCK |
					 DRM_MODE_PAGE_FLIP_EVENT,
				 (void *)(uintptr_t)o->commit_seq);
	drmModeAtomicFree(req);
	if (rc) {
		o->flip_pending = 0;
		fprintf(stderr, "drm: atomic commit: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int atomic_present(struct drm_out *o, struct drm_out_buf *b, int first)
{
	if (atomic_commit(o, b, first))
		return -1;
	return wait_for_flip(o, 200);
}

static int legacy_present(struct drm_out *o, struct drm_out_buf *b)
{
	drmVBlank vbl;

	if (drmModeSetPlane(o->fd, o->plane_id, o->crtc_id, b->fb_id, 0,
			    o->dst_x, o->dst_y, o->dst_w, o->dst_h,
			    (uint32_t)o->crop_x << 16,
			    (uint32_t)o->crop_y << 16,
			    (uint32_t)o->crop_w << 16,
			    (uint32_t)o->crop_h << 16)) {
		fprintf(stderr, "drm: SetPlane: %s\n", strerror(errno));
		return -1;
	}

	/*
	 * The vblank request must name our CRTC. Without the high-crtc bits
	 * the kernel assumes pipe 0, which on this board is a disabled video
	 * port, and every wait returns EBUSY.
	 */
	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE;
	if (o->crtc_index == 1)
		vbl.request.type |= DRM_VBLANK_SECONDARY;
	else if (o->crtc_index > 1)
		vbl.request.type |=
			(o->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT) &
			DRM_VBLANK_HIGH_CRTC_MASK;
	vbl.request.sequence = 1;
	if (drmWaitVBlank(o->fd, &vbl))
		fprintf(stderr, "drm: WaitVBlank: %s\n", strerror(errno));
	return 0;
}

int drm_out_present(struct drm_out *o)
{
	struct drm_out_buf *b = &o->bufs[o->back];
	static int first = 1;

	if (o->atomic) {
		if (atomic_present(o, b, first) == 0) {
			first = 0;
			o->back = (o->back + 1) % o->nbufs;
			return 0;
		}
		fprintf(stderr, "drm: atomic present failed, using legacy\n");
		o->atomic = 0;
	}
	if (legacy_present(o, b))
		return -1;
	first = 0;
	o->back = (o->back + 1) % o->nbufs;
	return 0;
}

int drm_out_commit(struct drm_out *o)
{
	struct drm_out_buf *b = &o->bufs[o->back];
	static int first = 1;

	if (!o->atomic) {
		/* No event to wait for on the legacy path: it blocks inside
		 * SetPlane, so commit and wait are the same call. */
		return drm_out_present(o);
	}
	if (o->flip_pending && wait_for_flip(o, 200))
		return -1;
	if (atomic_commit(o, b, first))
		return -1;
	first = 0;
	o->back = (o->back + 1) % o->nbufs;
	return 0;
}

int drm_out_wait_flip(struct drm_out *o, int timeout_ms)
{
	return wait_for_flip(o, timeout_ms);
}

int drm_out_recommit(struct drm_out *o)
{
	/* The visible buffer is the one before `back`: commit advances
	 * `back` past the buffer it just queued. Before any commit at all
	 * there is nothing visible to repeat. */
	struct drm_out_buf *front;

	if (!o->atomic || !o->flips)
		return -1;
	if (o->flip_pending && wait_for_flip(o, 200))
		return -1;
	front = &o->bufs[(o->back + o->nbufs - 1) % o->nbufs];
	return atomic_commit(o, front, 0);
}

int drm_out_wait_vblank(struct drm_out *o)
{
	drmVBlank vbl;

	/*
	 * The vblank request must name our CRTC. Without the high-crtc bits
	 * the kernel assumes pipe 0, which on this board is a disabled video
	 * port, and every wait returns EBUSY.
	 */
	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE;
	if (o->crtc_index == 1)
		vbl.request.type |= DRM_VBLANK_SECONDARY;
	else if (o->crtc_index > 1)
		vbl.request.type |=
			(o->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT) &
			DRM_VBLANK_HIGH_CRTC_MASK;
	vbl.request.sequence = 1;
	if (drmWaitVBlank(o->fd, &vbl)) {
		fprintf(stderr, "drm: WaitVBlank: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

void drm_out_close(struct drm_out *o)
{
	if (o->fd < 0)
		return;

	/* Detach ours first, then give the screen back. */
	if (o->plane_id)
		drmModeSetPlane(o->fd, o->plane_id, o->crtc_id, 0, 0,
				0, 0, 0, 0, 0, 0, 0, 0);

	if (o->zpos_valid && o->zpos_prop_id)
		drmModeObjectSetProperty(o->fd, o->plane_id,
					 DRM_MODE_OBJECT_PLANE,
					 o->zpos_prop_id, o->orig_zpos);

	restore_planes(o);

	free_buffers(o);

	if (o->have_master)
		drmDropMaster(o->fd);
	close(o->fd);
	o->fd = -1;
}
