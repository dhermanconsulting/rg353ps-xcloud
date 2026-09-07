/*
 * drmprobe - read-only DRM/KMS survey for the Anbernic RG353 (RK3566, VOP2).
 *
 * Answers the three M1 questions:
 *   1. Does EmulationStation actually release DRM master when it launches a
 *      port?  (Run this over SSH with ES up, then again from the Ports menu.)
 *   2. Which plane is an OVERLAY that advertises DRM_FORMAT_NV12 and can be
 *      attached to the active CRTC?
 *   3. Can we allocate NV12 dumb buffers and wrap them with drmModeAddFB2?
 *
 * It NEVER calls drmModeSetCrtc or drmModeSetPlane, so it cannot leave the
 * display wedged.  Everything it allocates is freed before it exits.
 *
 * Build: see docker/Dockerfile.arm64-bullseye
 *   aarch64-linux-gnu-gcc -O2 -o drmprobe drmprobe.c $(pkg-config --cflags --libs libdrm)
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>


/*
 * drmModeGetConnectorTypeName() only exists in libdrm 2.4.112 and later.
 * Bullseye ships 2.4.104, so carry the table locally.
 */
static const char *connector_type_name(uint32_t t)
{
	static const char *names[] = {
		"Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite",
		"SVIDEO", "LVDS", "Component", "DIN", "DP", "HDMI-A",
		"HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback",
		"SPI", "USB",
	};
	return t < sizeof(names) / sizeof(names[0]) ? names[t] : "?";
}

static const char *plane_type_name(uint64_t t)
{
	switch (t) {
	case DRM_PLANE_TYPE_PRIMARY: return "PRIMARY";
	case DRM_PLANE_TYPE_OVERLAY: return "OVERLAY";
	case DRM_PLANE_TYPE_CURSOR:  return "CURSOR";
	default:                     return "UNKNOWN";
	}
}

static void fourcc_str(uint32_t f, char out[5])
{
	out[0] = (char)(f & 0xff);
	out[1] = (char)((f >> 8) & 0xff);
	out[2] = (char)((f >> 16) & 0xff);
	out[3] = (char)((f >> 24) & 0xff);
	out[4] = '\0';
}

/* Read a named property off any DRM object.  Returns 0 on success. */
static int get_prop(int fd, uint32_t obj_id, uint32_t obj_type,
		    const char *name, uint64_t *value)
{
	drmModeObjectProperties *props =
		drmModeObjectGetProperties(fd, obj_id, obj_type);
	int rc = -1;

	if (!props)
		return -1;

	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (!p)
			continue;
		if (strcmp(p->name, name) == 0) {
			*value = props->prop_values[i];
			rc = 0;
		}
		drmModeFreeProperty(p);
		if (rc == 0)
			break;
	}
	drmModeFreeObjectProperties(props);
	return rc;
}

/*
 * Allocate a dumb buffer and optionally wrap it in a framebuffer.
 * Frees everything before returning.  Returns 0 on full success.
 */
static int try_dumb(int fd, uint32_t w, uint32_t alloc_h, uint32_t bpp,
		    uint32_t fourcc, uint32_t fb_h)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	uint32_t fb_id = 0;
	char cc[5];
	int rc = 0;

	memset(&creq, 0, sizeof(creq));
	creq.width = w;
	creq.height = alloc_h;
	creq.bpp = bpp;

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
		printf("      CREATE_DUMB %ux%u bpp=%u  -> FAILED (%s)\n",
		       w, alloc_h, bpp, strerror(errno));
		return -1;
	}
	printf("      CREATE_DUMB %ux%u bpp=%u  -> ok (handle=%u pitch=%u size=%llu)\n",
	       w, alloc_h, bpp, creq.handle, creq.pitch,
	       (unsigned long long)creq.size);

	if (fourcc) {
		fourcc_str(fourcc, cc);
		handles[0] = creq.handle;
		pitches[0] = creq.pitch;
		offsets[0] = 0;
		if (fb_h != alloc_h) {
			/*
			 * NV12 is two planes in one linear allocation: fb_h
			 * rows of luma, then fb_h/2 rows of interleaved
			 * chroma.  The FB must describe the visible height,
			 * and the chroma offset follows the LUMA plane - not
			 * the padded allocation height.
			 */
			handles[1] = creq.handle;
			pitches[1] = creq.pitch;
			offsets[1] = creq.pitch * fb_h;
		}
		if (drmModeAddFB2(fd, w, fb_h, fourcc, handles, pitches, offsets,
				  &fb_id, 0) < 0) {
			printf("      AddFB2 %s          -> FAILED (%s)\n",
			       cc, strerror(errno));
			rc = -1;
		} else {
			printf("      AddFB2 %s          -> ok (fb_id=%u)\n",
			       cc, fb_id);
			drmModeRmFB(fd, fb_id);
		}
	}

	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = creq.handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return rc;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/dri/card0";
	int fd, master_rc;
	drmModeRes *res;
	drmModePlaneRes *planes;
	uint32_t active_crtc = 0;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}
	printf("== device ==\n%s opened\n", path);

	/*
	 * The load-bearing question.  If EmulationStation still holds master
	 * this returns -EINVAL/-EACCES and a real client could not modeset.
	 */
	master_rc = drmSetMaster(fd);
	printf("drmSetMaster() = %d%s%s\n", master_rc,
	       master_rc ? " ERRNO=" : "",
	       master_rc ? strerror(errno) : "  (we are DRM master)");
	if (master_rc == 0)
		drmDropMaster(fd);

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
		printf("WARNING: UNIVERSAL_PLANES not available (%s)\n",
		       strerror(errno));
	printf("atomic cap: %s\n",
	       drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) ? "no" : "yes");

	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "drmModeGetResources: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("\n== connectors ==\n");
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c)
			continue;
		printf("connector %u type=%s-%u %s modes=%d encoder=%u\n",
		       c->connector_id,
		       connector_type_name(c->connector_type),
		       c->connector_type_id,
		       c->connection == DRM_MODE_CONNECTED ? "CONNECTED"
							   : "disconnected",
		       c->count_modes, c->encoder_id);
		for (int m = 0; m < c->count_modes; m++)
			printf("    mode[%d] %ux%u@%u %s\n", m,
			       c->modes[m].hdisplay, c->modes[m].vdisplay,
			       c->modes[m].vrefresh,
			       (c->modes[m].type & DRM_MODE_TYPE_PREFERRED)
				       ? "(preferred)" : "");
		/* Follow connector -> encoder -> crtc to find the live CRTC. */
		if (c->connection == DRM_MODE_CONNECTED && c->encoder_id) {
			drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id);
			if (e) {
				if (e->crtc_id) {
					active_crtc = e->crtc_id;
					printf("    -> encoder %u -> ACTIVE CRTC %u\n",
					       e->encoder_id, e->crtc_id);
				}
				drmModeFreeEncoder(e);
			}
		}
		drmModeFreeConnector(c);
	}

	printf("\n== crtcs ==\n");
	for (int i = 0; i < res->count_crtcs; i++) {
		drmModeCrtc *c = drmModeGetCrtc(fd, res->crtcs[i]);
		if (!c)
			continue;
		printf("crtc[%d] id=%u index_bit=0x%x mode_valid=%d %ux%u fb=%u\n",
		       i, c->crtc_id, 1u << i, c->mode_valid,
		       c->mode.hdisplay, c->mode.vdisplay, c->buffer_id);
		drmModeFreeCrtc(c);
	}
	printf("active CRTC (from connector walk) = %u\n", active_crtc);

	printf("\n== planes ==\n");
	planes = drmModeGetPlaneResources(fd);
	if (!planes) {
		fprintf(stderr, "drmModeGetPlaneResources: %s\n",
			strerror(errno));
		drmModeFreeResources(res);
		close(fd);
		return 1;
	}

	for (uint32_t i = 0; i < planes->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(fd, planes->planes[i]);
		uint64_t type = 0, zpos = 0;
		int nv12 = 0, free_plane, usable_here;
		char cc[5];

		if (!p)
			continue;

		get_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
		get_prop(fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "zpos", &zpos);

		for (uint32_t f = 0; f < p->count_formats; f++)
			if (p->formats[f] == DRM_FORMAT_NV12)
				nv12 = 1;

		free_plane = (p->crtc_id == 0);
		/*
		 * possible_crtcs is a bitmask over the CRTC *index* in
		 * res->crtcs, not the CRTC id - map the active id back.
		 */
		usable_here = 0;
		for (int c = 0; c < res->count_crtcs; c++)
			if (res->crtcs[c] == active_crtc &&
			    (p->possible_crtcs & (1u << c)))
				usable_here = 1;

		printf("plane %u  %-7s zpos=%llu crtc=%u fb=%u possible_crtcs=0x%x"
		       "  NV12=%s  %s  %s\n",
		       p->plane_id, plane_type_name(type),
		       (unsigned long long)zpos, p->crtc_id, p->fb_id,
		       p->possible_crtcs, nv12 ? "YES" : "no",
		       free_plane ? "free" : "IN USE",
		       usable_here ? "attachable-to-active-crtc" : "");

		printf("    formats:");
		for (uint32_t f = 0; f < p->count_formats; f++) {
			fourcc_str(p->formats[f], cc);
			printf(" %s", cc);
		}
		printf("\n");

		if (nv12 && free_plane && usable_here &&
		    type == DRM_PLANE_TYPE_OVERLAY)
			printf("    ^^ CANDIDATE for the video overlay\n");

		drmModeFreePlane(p);
	}

	printf("\n== buffer allocation tests ==\n");
	printf("  640x480 XRGB8888 (a UI-sized RGB buffer):\n");
	try_dumb(fd, 640, 480, 32, DRM_FORMAT_XRGB8888, 480);
	/*
	 * NV12 is 12bpp overall.  Dumb buffers are described in bpp for a
	 * single linear allocation, so ask for 8bpp at 1.5x the height and
	 * lay the chroma plane out by hand - this is what every KMS NV12
	 * user does.
	 */
	printf("  640x360 NV12 (target present size, 8bpp x 1.5 height):\n");
	try_dumb(fd, 640, 360 * 3 / 2, 8, DRM_FORMAT_NV12, 360);
	printf("  1280x720 NV12 (full stream size):\n");
	try_dumb(fd, 1280, 720 * 3 / 2, 8, DRM_FORMAT_NV12, 720);

	drmModeFreePlaneResources(planes);
	drmModeFreeResources(res);
	close(fd);
	printf("\ndone - no modeset was performed\n");
	return 0;
}
