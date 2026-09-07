/*
 * sim_view - the browser view of the host simulator. See sim_view.h.
 *
 * One listener thread accepts connections and hands each to a short-lived
 * thread: "/" is the page, "/input" is a gamepad update, "/stream" is the
 * multipart PNG stream and lives as long as the tab is open. Only one
 * stream client at a time -- this is a debug view, not a service.
 *
 * The present loop only ever memcpy's the frame under a mutex and signals;
 * conversion and PNG encoding happen on the streaming thread, so a slow
 * browser can drop frames but can never slow the thing being measured.
 */
#define _GNU_SOURCE
#include "sim_view.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "../input/evdev_pad.h"

#define SIM_VIEW_MAX_FPS 30

static struct {
	pthread_mutex_t lock;
	pthread_cond_t cv;
	uint8_t *nv12;      /* luma then chroma, both at `pitch` */
	size_t cap;
	int w, h, pitch;
	uint64_t seq;
	int running;
	int quit;
	int listen_fd;
	int streaming;      /* a stream client is connected */
	pthread_t thread;
} g = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0, 0, 0,
	0, 0, 0, 0, -1, 0, 0 };

/* ---- PNG, straight to zlib ---------------------------------------------- */

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint8_t clamp8(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/*
 * NV12 -> PNG scanlines (filter byte 0, then RGB triples). BT.601 limited
 * range, the matrix libavcodec's yuv420p output implies for this stream, in
 * 8.8 fixed point.
 */
static void nv12_to_rows(const uint8_t *luma, const uint8_t *chroma, int pitch,
			 int w, int h, uint8_t *rows)
{
	const size_t row_bytes = 1 + (size_t)w * 3;

	for (int y = 0; y < h; y++) {
		const uint8_t *ly = luma + (size_t)y * pitch;
		const uint8_t *cy = chroma + (size_t)(y / 2) * pitch;
		uint8_t *out = rows + (size_t)y * row_bytes;

		*out++ = 0;  /* filter: none */
		for (int x = 0; x < w; x++) {
			int c = 298 * (ly[x] - 16);
			int d = cy[(x & ~1)] - 128;
			int e = cy[(x & ~1) + 1] - 128;

			*out++ = clamp8((c + 409 * e + 128) >> 8);
			*out++ = clamp8((c - 100 * d - 208 * e + 128) >> 8);
			*out++ = clamp8((c + 516 * d + 128) >> 8);
		}
	}
}

static int append(uint8_t **buf, size_t *len, size_t *cap, const void *data,
		  size_t n)
{
	if (*len + n > *cap) {
		size_t want = (*len + n) * 2;
		uint8_t *grown = realloc(*buf, want);

		if (!grown)
			return -1;
		*buf = grown;
		*cap = want;
	}
	memcpy(*buf + *len, data, n);
	*len += n;
	return 0;
}

static int chunk(uint8_t **buf, size_t *len, size_t *cap, const char *type,
		 const uint8_t *data, uint32_t n)
{
	uint8_t hdr[8], tail[4];
	uint32_t crc;

	put_be32(hdr, n);
	memcpy(hdr + 4, type, 4);
	crc = crc32(0, (const Bytef *)type, 4);
	if (n)
		crc = crc32(crc, data, n);
	put_be32(tail, crc);
	return append(buf, len, cap, hdr, 8) ||
	       (n && append(buf, len, cap, data, n)) ||
	       append(buf, len, cap, tail, 4);
}

uint8_t *sim_png_encode(const uint8_t *luma, const uint8_t *chroma, int pitch,
			int w, int h, size_t *len_out)
{
	static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
	const size_t raw_len = (1 + (size_t)w * 3) * (size_t)h;
	uint8_t *raw = malloc(raw_len);
	uLongf zlen = compressBound((uLong)raw_len);
	uint8_t *z = malloc(zlen);
	uint8_t *out = NULL;
	size_t len = 0, cap = 0;
	uint8_t ihdr[13];

	if (!raw || !z)
		goto fail;
	nv12_to_rows(luma, chroma, pitch, w, h, raw);
	/* Level 1: these are for looking at, not archiving, and the frames
	 * keep coming. Level 6 is three times slower for a few per cent. */
	if (compress2(z, &zlen, raw, (uLong)raw_len, 1) != Z_OK)
		goto fail;

	put_be32(ihdr, (uint32_t)w);
	put_be32(ihdr + 4, (uint32_t)h);
	ihdr[8] = 8;   /* bit depth */
	ihdr[9] = 2;   /* colour type: truecolour */
	ihdr[10] = 0;
	ihdr[11] = 0;
	ihdr[12] = 0;
	if (append(&out, &len, &cap, sig, 8) ||
	    chunk(&out, &len, &cap, "IHDR", ihdr, 13) ||
	    chunk(&out, &len, &cap, "IDAT", z, (uint32_t)zlen) ||
	    chunk(&out, &len, &cap, "IEND", NULL, 0))
		goto fail;

	free(raw);
	free(z);
	*len_out = len;
	return out;
fail:
	free(raw);
	free(z);
	free(out);
	*len_out = 0;
	return NULL;
}

/* ---- the page ------------------------------------------------------------ */

/*
 * Keyboard -> pad. The bit numbers in the script below are enum pad_button
 * values, which a string cannot assert for itself -- so the enum is pinned
 * here instead, and reordering it fails the build rather than silently
 * remapping someone's controls.
 */
_Static_assert(PAD_A == 0 && PAD_B == 1 && PAD_X == 2 && PAD_Y == 3 &&
		       PAD_L1 == 4 && PAD_R1 == 5 && PAD_L2 == 6 &&
		       PAD_R2 == 7 && PAD_SELECT == 8 && PAD_START == 9 &&
		       PAD_UP == 13 && PAD_DOWN == 14 && PAD_LEFT == 15 &&
		       PAD_RIGHT == 16,
	       "the keyboard table in kPage encodes these bit numbers");

static const char kPage[] =
"<!doctype html><meta charset=utf-8><title>xcloud sim</title>"
"<style>"
"html{background:#111;color:#ccc;font:14px system-ui,sans-serif}"
"body{margin:0;display:flex;flex-direction:column;align-items:center;gap:10px;padding:12px}"
"img{background:#000;image-rendering:pixelated;max-width:100%;border:1px solid #333}"
"table{border-collapse:collapse;font-size:12px;color:#999}"
"td{padding:1px 10px 1px 0}kbd{background:#222;border:1px solid #444;border-radius:3px;padding:0 4px;color:#ddd}"
"#s{font-size:12px;color:#666}"
"</style>"
"<img id=v src=/stream>"
"<div id=s>click the picture, then use the keyboard</div>"
"<table>"
"<tr><td><kbd>&larr;&uarr;&darr;&rarr;</kbd> d-pad<td><kbd>W A S D</kbd> left stick<td><kbd>I J K L</kbd> right stick"
"<tr><td><kbd>Z</kbd> A &nbsp; <kbd>X</kbd> B &nbsp; <kbd>C</kbd> X &nbsp; <kbd>V</kbd> Y"
"<td><kbd>Q</kbd> LB &nbsp; <kbd>E</kbd> RB &nbsp; <kbd>1</kbd> LT &nbsp; <kbd>3</kbd> RT"
"<td><kbd>Enter</kbd> Start &nbsp; <kbd>Backspace</kbd> Select"
"</table>"
"<script>"
"const B={KeyZ:0,KeyX:1,KeyC:2,KeyV:3,KeyQ:4,KeyE:5,Digit1:6,Digit3:7,"
"Backspace:8,Enter:9,ArrowUp:13,ArrowDown:14,ArrowLeft:15,ArrowRight:16};"
"const A={KeyW:['ly',-1],KeyS:['ly',1],KeyA:['lx',-1],KeyD:['lx',1],"
"KeyI:['ry',-1],KeyK:['ry',1],KeyJ:['rx',-1],KeyL:['rx',1]};"
"let held=new Set(),sent='';"
"function push(){"
" let m=0,ax={lx:0,ly:0,rx:0,ry:0};"
" for(const k of held){if(k in B)m|=1<<B[k];if(k in A)ax[A[k][0]]+=A[k][1];}"
" const q='b='+m+'&lx='+(ax.lx*32000)+'&ly='+(ax.ly*32000)+"
"'&rx='+(ax.rx*32000)+'&ry='+(ax.ry*32000);"
" if(q===sent)return;sent=q;fetch('/input?'+q,{keepalive:true});}"
"addEventListener('keydown',e=>{if(e.code in B||e.code in A){e.preventDefault();"
"held.add(e.code);push();}});"
"addEventListener('keyup',e=>{if(held.delete(e.code)){e.preventDefault();push();}});"
"addEventListener('blur',()=>{held.clear();push();});"
"const v=document.getElementById('v'),st=document.getElementById('s');"
"function retry(){st.textContent='waiting for the simulator...';"
"setTimeout(()=>{v.src='/stream?'+Date.now();},800);}"
"v.onerror=retry;"
"addEventListener('load',()=>{st.textContent='click the picture, then use the keyboard';});"
"</script>";

/* ---- HTTP ---------------------------------------------------------------- */

static int send_all(int fd, const void *data, size_t n)
{
	const uint8_t *p = data;

	while (n) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);

		if (w <= 0)
			return -1;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static long query_int(const char *query, const char *key, long fallback)
{
	const size_t klen = strlen(key);
	const char *at = query;

	while (at && *at) {
		if (!strncmp(at, key, klen) && at[klen] == '=')
			return strtol(at + klen + 1, NULL, 10);
		at = strchr(at, '&');
		if (at)
			at++;
	}
	return fallback;
}

static int16_t clamp16(long v)
{
	return (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v);
}

static void serve_stream(int fd)
{
	static const char head[] =
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: multipart/x-mixed-replace; boundary=f\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n\r\n";
	uint64_t seen = 0;
	uint8_t *frame = NULL;
	size_t cap = 0;
	double next_at = 0;

	if (send_all(fd, head, sizeof(head) - 1))
		return;

	while (!g.quit) {
		int w, h, pitch;
		size_t need, png_len;
		uint8_t *png;
		char part[128];
		struct timespec ts;
		double now;

		pthread_mutex_lock(&g.lock);
		while (!g.quit && g.seq == seen) {
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 1;
			pthread_cond_timedwait(&g.cv, &g.lock, &ts);
		}
		if (g.quit || !g.nv12) {
			pthread_mutex_unlock(&g.lock);
			break;
		}
		seen = g.seq;
		w = g.w;
		h = g.h;
		pitch = g.pitch;
		need = (size_t)pitch * h * 3 / 2;
		if (cap < need) {
			uint8_t *grown = realloc(frame, need);

			if (!grown) {
				pthread_mutex_unlock(&g.lock);
				break;
			}
			frame = grown;
			cap = need;
		}
		memcpy(frame, g.nv12, need);
		pthread_mutex_unlock(&g.lock);

		/* Cap the rate: a 60 fps stream would otherwise spend the
		 * machine on zlib for frames nobody can see. */
		clock_gettime(CLOCK_REALTIME, &ts);
		now = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
		if (now < next_at) {
			struct timespec s = { 0, (long)((next_at - now) * 1e6) };

			nanosleep(&s, NULL);
			continue;
		}
		next_at = now + 1000.0 / SIM_VIEW_MAX_FPS;

		png = sim_png_encode(frame, frame + (size_t)pitch * h, pitch, w,
				     h, &png_len);
		if (!png)
			continue;
		snprintf(part, sizeof(part),
			 "--f\r\nContent-Type: image/png\r\nContent-Length: %zu\r\n\r\n",
			 png_len);
		if (send_all(fd, part, strlen(part)) ||
		    send_all(fd, png, png_len) || send_all(fd, "\r\n", 2)) {
			free(png);
			break;
		}
		free(png);
	}
	free(frame);
}

static void *client_thread(void *arg)
{
	const int fd = (int)(intptr_t)arg;
	char req[2048];
	ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
	char *path, *end;

	if (n <= 0)
		goto out;
	req[n] = 0;
	if (strncmp(req, "GET ", 4))
		goto out;
	path = req + 4;
	end = strchr(path, ' ');
	if (!end)
		goto out;
	*end = 0;

	if (!strcmp(path, "/") || !strncmp(path, "/?", 2)) {
		char head[160];

		snprintf(head, sizeof(head),
			 "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
			 "Content-Length: %zu\r\nConnection: close\r\n\r\n",
			 sizeof(kPage) - 1);
		if (!send_all(fd, head, strlen(head)))
			send_all(fd, kPage, sizeof(kPage) - 1);
	} else if (!strncmp(path, "/input", 6)) {
		const char *q = strchr(path, '?');
		static const char ok[] =
			"HTTP/1.0 204 No Content\r\nConnection: close\r\n\r\n";

		if (q)
			pad_virtual_set((uint32_t)query_int(q + 1, "b", 0),
					clamp16(query_int(q + 1, "lx", 0)),
					clamp16(query_int(q + 1, "ly", 0)),
					clamp16(query_int(q + 1, "rx", 0)),
					clamp16(query_int(q + 1, "ry", 0)));
		send_all(fd, ok, sizeof(ok) - 1);
	} else if (!strncmp(path, "/stream", 7)) {   /* may carry ?cachebust */
		int busy;

		pthread_mutex_lock(&g.lock);
		busy = g.streaming;
		g.streaming = 1;
		pthread_mutex_unlock(&g.lock);
		if (busy) {
			static const char no[] =
				"HTTP/1.0 503 Service Unavailable\r\n"
				"Connection: close\r\n\r\none view at a time\n";

			send_all(fd, no, sizeof(no) - 1);
		} else {
			serve_stream(fd);
			pthread_mutex_lock(&g.lock);
			g.streaming = 0;
			pthread_mutex_unlock(&g.lock);
		}
	} else {
		static const char nf[] =
			"HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n";

		send_all(fd, nf, sizeof(nf) - 1);
	}
out:
	close(fd);
	return NULL;
}

static void *listen_thread(void *arg)
{
	(void)arg;
	while (!g.quit) {
		int fd = accept(g.listen_fd, NULL, NULL);
		pthread_t t;
		int one = 1;

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		if (pthread_create(&t, NULL, client_thread,
				   (void *)(intptr_t)fd)) {
			close(fd);
			continue;
		}
		pthread_detach(t);
	}
	return NULL;
}

/* ---- lifecycle ----------------------------------------------------------- */

int sim_view_start(int port)
{
	struct sockaddr_in addr;
	int one = 1;

	if (g.running || port <= 0)
		return -1;
	/* A browser that closes a tab mid-write would otherwise kill us. */
	signal(SIGPIPE, SIG_IGN);

	g.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (g.listen_fd < 0) {
		fprintf(stderr, "sim-view: socket: %s\n", strerror(errno));
		return -1;
	}
	setsockopt(g.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);
	if (bind(g.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(g.listen_fd, 4)) {
		fprintf(stderr, "sim-view: port %d: %s\n", port,
			strerror(errno));
		close(g.listen_fd);
		g.listen_fd = -1;
		return -1;
	}
	g.running = 1;
	if (pthread_create(&g.thread, NULL, listen_thread, NULL)) {
		close(g.listen_fd);
		g.listen_fd = -1;
		g.running = 0;
		return -1;
	}
	fprintf(stderr, "sim-view: http://localhost:%d/ (keyboard drives the "
			"pad; PNG frames, capped at %d fps)\n",
		port, SIM_VIEW_MAX_FPS);
	return 0;
}

void sim_view_stop(void)
{
	if (!g.running)
		return;
	g.quit = 1;
	pthread_mutex_lock(&g.lock);
	pthread_cond_broadcast(&g.cv);
	pthread_mutex_unlock(&g.lock);
	/* Closing the listener breaks the accept(); detached client threads
	 * see g.quit and unwind on their own. */
	if (g.listen_fd >= 0) {
		shutdown(g.listen_fd, SHUT_RDWR);
		close(g.listen_fd);
		g.listen_fd = -1;
	}
	pthread_join(g.thread, NULL);
	free(g.nv12);
	g.nv12 = NULL;
	g.cap = 0;
	g.running = 0;
}

void sim_view_publish(const uint8_t *luma, const uint8_t *chroma, int pitch,
		      int w, int h)
{
	size_t need;

	if (!g.running || w <= 0 || h <= 0)
		return;
	need = (size_t)pitch * h * 3 / 2;

	pthread_mutex_lock(&g.lock);
	if (g.cap < need) {
		uint8_t *grown = realloc(g.nv12, need);

		if (!grown) {
			pthread_mutex_unlock(&g.lock);
			return;
		}
		g.nv12 = grown;
		g.cap = need;
	}
	memcpy(g.nv12, luma, (size_t)pitch * h);
	memcpy(g.nv12 + (size_t)pitch * h, chroma, (size_t)pitch * h / 2);
	g.w = w;
	g.h = h;
	g.pitch = pitch;
	g.seq++;
	pthread_cond_broadcast(&g.cv);
	pthread_mutex_unlock(&g.lock);
}
