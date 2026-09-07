#!/usr/bin/env python3
"""
drmprobe.py - read-only DRM/KMS survey for the Anbernic RG353 (RK3566, VOP2).

Same job as tools/drmprobe.c but driven through raw ioctls, so it runs on the
device's stock Python 3.9 with no compiler and nothing installed.

Answers the M1 questions:
  1. Does EmulationStation release DRM master?  Run over SSH with ES up
     (expect SET_MASTER to fail) and again from the Ports menu (expect ok).
  2. Which plane is an OVERLAY advertising DRM_FORMAT_NV12 that can attach to
     the active CRTC?
  3. Can we allocate NV12 dumb buffers and wrap them with ADDFB2?

It never calls SETCRTC or SETPLANE, so it cannot wedge the display. Everything
it allocates is destroyed before exit.
"""

import ctypes
import fcntl
import os
import struct
import sys

# ---- ioctl encoding (asm-generic) ----------------------------------------
_IOC_NONE, _IOC_WRITE, _IOC_READ = 0, 1, 2
DRM_BASE = ord('d')


def _ioc(direction, nr, size):
    op = (direction << 30) | (size << 16) | (DRM_BASE << 8) | nr
    # fcntl.ioctl wants a C int; anything with bit 31 set must go in signed.
    return op - 0x100000000 if op >= 0x80000000 else op


def _io(nr):
    return _ioc(_IOC_NONE, nr, 0)


def _iow(nr, size):
    return _ioc(_IOC_WRITE, nr, size)


def _iowr(nr, size):
    return _ioc(_IOC_READ | _IOC_WRITE, nr, size)


# ---- struct formats (little-endian, explicit padding to match sizeof) -----
F_CARD_RES = '<4Q8I'                 # drm_mode_card_res              64
F_PLANE_RES = '<QII'                 # drm_mode_get_plane_res         16
F_GET_PLANE = '<6IQ'                 # drm_mode_get_plane             32
F_OBJ_PROPS = '<QQIII4x'             # drm_mode_obj_get_properties    32
F_GET_PROP = '<QQII32sII'            # drm_mode_get_property          64
F_CREATE_DUMB = '<6IQ'               # drm_mode_create_dumb           32
F_DESTROY_DUMB = '<I'                # drm_mode_destroy_dumb           4
F_FB_CMD2 = '<17I4x4Q'               # drm_mode_fb_cmd2              104
F_MODEINFO = '<I10H3I32s'            # drm_mode_modeinfo              68
F_CRTC = '<Q7I' + F_MODEINFO[1:]     # drm_mode_crtc                 104
F_CLIENT_CAP = '<QQ'                 # drm_set_client_cap             16

IOCTL_SET_MASTER = _io(0x1e)
IOCTL_DROP_MASTER = _io(0x1f)
IOCTL_SET_CLIENT_CAP = _iow(0x0d, struct.calcsize(F_CLIENT_CAP))
IOCTL_MODE_GETRESOURCES = _iowr(0xA0, struct.calcsize(F_CARD_RES))
IOCTL_MODE_GETCRTC = _iowr(0xA1, struct.calcsize(F_CRTC))
IOCTL_MODE_GETPROPERTY = _iowr(0xAA, struct.calcsize(F_GET_PROP))
IOCTL_MODE_RMFB = _iowr(0xAF, 4)
IOCTL_MODE_CREATE_DUMB = _iowr(0xB2, struct.calcsize(F_CREATE_DUMB))
IOCTL_MODE_DESTROY_DUMB = _iowr(0xB4, struct.calcsize(F_DESTROY_DUMB))
IOCTL_MODE_GETPLANERESOURCES = _iowr(0xB5, struct.calcsize(F_PLANE_RES))
IOCTL_MODE_GETPLANE = _iowr(0xB6, struct.calcsize(F_GET_PLANE))
IOCTL_MODE_ADDFB2 = _iowr(0xB8, struct.calcsize(F_FB_CMD2))

DRM_CLIENT_CAP_UNIVERSAL_PLANES = 2
DRM_CLIENT_CAP_ATOMIC = 3

PLANE_TYPE = {0: 'OVERLAY', 1: 'PRIMARY', 2: 'CURSOR'}


def fourcc(code):
    if code == 0:
        return '----'
    return ''.join(chr((code >> s) & 0xFF) for s in (0, 8, 16, 24))


def fourcc_of(s):
    return s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24)


DRM_FORMAT_NV12 = fourcc_of(b'NV12')
DRM_FORMAT_XRGB8888 = fourcc_of(b'XR24')


def ioctl(fd, op, payload):
    """Run an ioctl on a mutable buffer and return the buffer back."""
    buf = ctypes.create_string_buffer(payload, len(payload))
    fcntl.ioctl(fd, op, buf, True)
    return buf.raw


def u32_array(count):
    """Allocate a __u32[count] and return (buffer, address)."""
    if count == 0:
        return None, 0
    buf = (ctypes.c_uint32 * count)()
    return buf, ctypes.addressof(buf)


def u64_array(count):
    if count == 0:
        return None, 0
    buf = (ctypes.c_uint64 * count)()
    return buf, ctypes.addressof(buf)


# ---- property helpers -----------------------------------------------------
def object_properties(fd, obj_id, obj_type):
    """Return {name: value} for a DRM object."""
    blank = struct.pack(F_OBJ_PROPS, 0, 0, 0, obj_id, obj_type)
    try:
        res = struct.unpack(F_OBJ_PROPS, ioctl(fd, IOCTL_MODE_OBJ_GETPROPERTIES,
                                               blank))
    except OSError:
        return {}
    count = res[2]
    if count == 0:
        return {}

    props_buf, props_ptr = u32_array(count)
    vals_buf, vals_ptr = u64_array(count)
    filled = struct.pack(F_OBJ_PROPS, props_ptr, vals_ptr, count, obj_id,
                         obj_type)
    try:
        res = struct.unpack(F_OBJ_PROPS,
                            ioctl(fd, IOCTL_MODE_OBJ_GETPROPERTIES, filled))
    except OSError:
        return {}

    out = {}
    for i in range(min(count, res[2])):
        name = property_name(fd, props_buf[i])
        if name:
            out[name] = vals_buf[i]
    return out


_prop_name_cache = {}


def property_name(fd, prop_id):
    if prop_id in _prop_name_cache:
        return _prop_name_cache[prop_id]
    blank = struct.pack(F_GET_PROP, 0, 0, prop_id, 0, b'', 0, 0)
    try:
        res = struct.unpack(F_GET_PROP, ioctl(fd, IOCTL_MODE_GETPROPERTY,
                                              blank))
    except OSError:
        return None
    name = res[4].split(b'\x00', 1)[0].decode('ascii', 'replace')
    _prop_name_cache[prop_id] = name
    return name


# needs the calcsize of F_OBJ_PROPS, defined after the format strings
IOCTL_MODE_OBJ_GETPROPERTIES = _iowr(0xB9, struct.calcsize(F_OBJ_PROPS))


# ---- buffer tests ---------------------------------------------------------
def try_dumb(fd, w, h, bpp, want_fourcc=None, nv12=False):
    """Allocate a dumb buffer, optionally wrap it in a FB, then free both."""
    req = struct.pack(F_CREATE_DUMB, h, w, bpp, 0, 0, 0, 0)
    try:
        res = struct.unpack(F_CREATE_DUMB,
                            ioctl(fd, IOCTL_MODE_CREATE_DUMB, req))
    except OSError as e:
        print('      CREATE_DUMB %dx%d bpp=%d -> FAILED (%s)' % (w, h, bpp, e))
        return False
    handle, pitch, size = res[4], res[5], res[6]
    print('      CREATE_DUMB %dx%d bpp=%d -> ok (handle=%d pitch=%d size=%d)'
          % (w, h, bpp, handle, pitch, size))

    ok = True
    if want_fourcc:
        handles = [handle, handle if nv12 else 0, 0, 0]
        pitches = [pitch, pitch if nv12 else 0, 0, 0]
        # NV12 chroma plane sits after the luma plane.
        offsets = [0, pitch * (h * 2 // 3) if nv12 else 0, 0, 0]
        fb = struct.pack(F_FB_CMD2, 0, w, h if not nv12 else h * 2 // 3,
                         want_fourcc, 0,
                         *(handles + pitches + offsets), 0, 0, 0, 0)
        try:
            out = struct.unpack(F_FB_CMD2, ioctl(fd, IOCTL_MODE_ADDFB2, fb))
            fb_id = out[0]
            print('      ADDFB2 %s -> ok (fb_id=%d)' % (fourcc(want_fourcc),
                                                        fb_id))
            try:
                ioctl(fd, IOCTL_MODE_RMFB, struct.pack('<I', fb_id))
            except OSError:
                pass
        except OSError as e:
            print('      ADDFB2 %s -> FAILED (%s)' % (fourcc(want_fourcc), e))
            ok = False

    try:
        ioctl(fd, IOCTL_MODE_DESTROY_DUMB,
              struct.pack(F_DESTROY_DUMB, handle))
    except OSError:
        pass
    return ok


# ---- main -----------------------------------------------------------------
def main():
    path = sys.argv[1] if len(sys.argv) > 1 else '/dev/dri/card0'
    fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
    print('== device ==\n%s opened' % path)

    # The load-bearing question: is EmulationStation still holding master?
    try:
        fcntl.ioctl(fd, IOCTL_SET_MASTER, 0)
        print('SET_MASTER -> ok (we are DRM master)')
        try:
            fcntl.ioctl(fd, IOCTL_DROP_MASTER, 0)
        except OSError:
            pass
    except OSError as e:
        print('SET_MASTER -> FAILED (%s)  <-- something else holds master' % e)

    for name, cap in (('UNIVERSAL_PLANES', DRM_CLIENT_CAP_UNIVERSAL_PLANES),
                      ('ATOMIC', DRM_CLIENT_CAP_ATOMIC)):
        try:
            ioctl(fd, IOCTL_SET_CLIENT_CAP, struct.pack(F_CLIENT_CAP, cap, 1))
            print('client cap %-17s -> ok' % name)
        except OSError as e:
            print('client cap %-17s -> no (%s)' % (name, e))

    # Resources: two-pass, once for counts then once with buffers.
    res = struct.unpack(F_CARD_RES, ioctl(fd, IOCTL_MODE_GETRESOURCES,
                                          struct.pack(F_CARD_RES, *([0] * 12))))
    n_crtc, n_conn, n_enc = res[5], res[6], res[7]
    crtc_buf, crtc_ptr = u32_array(n_crtc)
    conn_buf, conn_ptr = u32_array(n_conn)
    enc_buf, enc_ptr = u32_array(n_enc)
    struct.unpack(F_CARD_RES, ioctl(
        fd, IOCTL_MODE_GETRESOURCES,
        struct.pack(F_CARD_RES, 0, crtc_ptr, conn_ptr, enc_ptr,
                    0, n_crtc, n_conn, n_enc, 0, 0, 0, 0)))
    crtcs = list(crtc_buf) if crtc_buf else []
    print('\n== crtcs == (%d crtcs, %d connectors, %d encoders)' %
          (n_crtc, n_conn, n_enc))

    active_crtc = 0
    active_index = -1
    for idx, cid in enumerate(crtcs):
        blank = struct.pack(F_CRTC, 0, 0, cid, 0, 0, 0, 0, 0,
                            0, *([0] * 10), 0, 0, 0, b'')
        try:
            c = struct.unpack(F_CRTC, ioctl(fd, IOCTL_MODE_GETCRTC, blank))
        except OSError as e:
            print('crtc[%d] id=%d -> GETCRTC failed (%s)' % (idx, cid, e))
            continue
        fb_id, mode_valid = c[3], c[7]
        hdisp, vdisp = c[9], c[14]
        name = c[-1].split(b'\x00', 1)[0].decode('ascii', 'replace')
        print('crtc[%d] id=%-3d index_bit=0x%x mode_valid=%d fb=%-3d %dx%d %s'
              % (idx, cid, 1 << idx, mode_valid, fb_id, hdisp, vdisp, name))
        if mode_valid and not active_crtc:
            active_crtc = cid
            active_index = idx
    print('active CRTC = %d' % active_crtc)

    # Planes
    pr = struct.unpack(F_PLANE_RES, ioctl(fd, IOCTL_MODE_GETPLANERESOURCES,
                                          struct.pack(F_PLANE_RES, 0, 0, 0)))
    n_planes = pr[1]
    pbuf, pptr = u32_array(n_planes)
    struct.unpack(F_PLANE_RES, ioctl(fd, IOCTL_MODE_GETPLANERESOURCES,
                                     struct.pack(F_PLANE_RES, pptr, n_planes,
                                                 0)))
    print('\n== planes == (%d)' % n_planes)

    candidates = []
    for pid in list(pbuf):
        blank = struct.pack(F_GET_PLANE, pid, 0, 0, 0, 0, 0, 0)
        p = struct.unpack(F_GET_PLANE, ioctl(fd, IOCTL_MODE_GETPLANE, blank))
        n_fmt = p[5]
        fbuf, fptr = u32_array(n_fmt)
        p = struct.unpack(F_GET_PLANE, ioctl(
            fd, IOCTL_MODE_GETPLANE,
            struct.pack(F_GET_PLANE, pid, 0, 0, 0, 0, n_fmt, fptr)))
        crtc_id, fb_id, possible = p[1], p[2], p[3]
        formats = [fourcc(f) for f in list(fbuf)] if fbuf else []
        props = object_properties(fd, pid, 0xeeeeeeee)  # DRM_MODE_OBJECT_PLANE
        ptype = PLANE_TYPE.get(props.get('type'), '?')
        zpos = props.get('zpos', '-')

        attachable = bool(active_index >= 0 and
                          (possible & (1 << active_index)))
        has_nv12 = 'NV12' in formats
        free = crtc_id == 0
        print('plane %-4d %-7s zpos=%-4s crtc=%-4d fb=%-4d possible=0x%x '
              'NV12=%-3s %s %s'
              % (pid, ptype, zpos, crtc_id, fb_id, possible,
                 'YES' if has_nv12 else 'no',
                 'free' if free else 'IN-USE',
                 'attachable' if attachable else ''))
        print('     formats: %s' % ' '.join(formats))
        if has_nv12 and free and attachable and ptype == 'OVERLAY':
            candidates.append(pid)
            print('     ^^ CANDIDATE for the video overlay')

    print('\n== buffer allocation tests ==')
    print('  640x480 XR24:')
    try_dumb(fd, 640, 480, 32, DRM_FORMAT_XRGB8888)
    print('  640x360 NV12 (present size, 8bpp x 1.5 height):')
    try_dumb(fd, 640, 360 * 3 // 2, 8, DRM_FORMAT_NV12, nv12=True)
    print('  1280x720 NV12 (full stream size):')
    try_dumb(fd, 1280, 720 * 3 // 2, 8, DRM_FORMAT_NV12, nv12=True)

    print('\ncandidate overlay planes: %s' % (candidates or 'NONE'))
    print('done - no modeset was performed')
    os.close(fd)


if __name__ == '__main__':
    main()
