#include "evdev_pad.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/*
 * Face buttons are looked up through face_map() below; the codes here are
 * what the device tree assigns by Xbox POSITION (see the header), and the
 * layout decides which Xbox function each position gets.
 */
static const struct {
	int code;
	enum pad_button button;
} k_map[] = {
	{ BTN_TL,         PAD_L1 },     { BTN_TR,         PAD_R1 },
	{ BTN_TL2,        PAD_L2 },     { BTN_TR2,        PAD_R2 },
	{ BTN_SELECT,     PAD_SELECT }, { BTN_START,      PAD_START },
	{ BTN_MODE,       PAD_MODE },
	{ BTN_THUMBL,     PAD_L3 },     { BTN_THUMBR,     PAD_R3 },
	{ BTN_DPAD_UP,    PAD_UP },     { BTN_DPAD_DOWN,  PAD_DOWN },
	{ BTN_DPAD_LEFT,  PAD_LEFT },   { BTN_DPAD_RIGHT, PAD_RIGHT },
};

static const char *k_names[PAD_COUNT] = {
	"A", "B", "X", "Y", "L1", "R1", "L2", "R2",
	"Select", "Start", "Mode", "L3", "R3",
	"Up", "Down", "Left", "Right",
};

const char *pad_button_name(enum pad_button b)
{
	return (b >= 0 && b < PAD_COUNT) ? k_names[b] : "?";
}

const char *pad_evdev_name(int code)
{
	static const struct { int code; const char *name; } names[] = {
		{ BTN_SOUTH, "BTN_SOUTH" }, { BTN_EAST,  "BTN_EAST" },
		{ BTN_NORTH, "BTN_NORTH" }, { BTN_WEST,  "BTN_WEST" },
		{ BTN_TL,    "BTN_TL" },    { BTN_TR,    "BTN_TR" },
		{ BTN_TL2,   "BTN_TL2" },   { BTN_TR2,   "BTN_TR2" },
		{ BTN_SELECT, "BTN_SELECT" }, { BTN_START, "BTN_START" },
		{ BTN_MODE,  "BTN_MODE" },
		{ BTN_THUMBL, "BTN_THUMBL" }, { BTN_THUMBR, "BTN_THUMBR" },
		{ BTN_DPAD_UP,   "BTN_DPAD_UP" },
		{ BTN_DPAD_DOWN, "BTN_DPAD_DOWN" },
		{ BTN_DPAD_LEFT, "BTN_DPAD_LEFT" },
		{ BTN_DPAD_RIGHT, "BTN_DPAD_RIGHT" },
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (names[i].code == code)
			return names[i].name;
	return NULL;
}

const char *pad_button_label(const struct pad *p, enum pad_button b)
{
	/* Positional: bottom is PAD_A and the shell prints B there, and so
	 * on round the diamond (see face_map). */
	if (p && p->layout == PAD_LAYOUT_POSITIONAL) {
		switch (b) {
		case PAD_A: return "B";
		case PAD_B: return "A";
		case PAD_X: return "Y";
		case PAD_Y: return "X";
		default: break;
		}
	}
	return pad_button_name(b);
}

/*
 * Physical position -> Xbox function for the four face buttons.
 *
 * Codes by position on this device: BTN_SOUTH bottom, BTN_EAST right,
 * BTN_NORTH top, BTN_WEST left -- the usual Linux meaning of the names.
 * Evidence: mainline's rk3566-anbernic-rg353x.dtsi wires SOUTH to the pin
 * of the bottom button (printed B), EAST to the right one (A), NORTH to the
 * top one (X) and WEST to the left one (Y); the vendor tree uses the same
 * four codes on the same board, and the bottom button was measured to emit
 * BTN_SOUTH. Only the bottom one has been pressed under a scope, so if X and
 * Y ever feel swapped, `-swapxy` exists.
 *
 * Positional: bottom A, right B, left X, top Y -- what an Xbox pad is.
 * Labels: the shell prints B bottom, A right, Y left, X top, so the button
 * that SAYS A is on the right (BTN_EAST) and so on.
 */
static int face_map(const struct pad *p, int code, enum pad_button *out)
{
	int positional = p->layout == PAD_LAYOUT_POSITIONAL;
	enum pad_button b;

	switch (code) {
	case BTN_SOUTH: b = positional ? PAD_A : PAD_B; break;
	case BTN_EAST:  b = positional ? PAD_B : PAD_A; break;
	case BTN_NORTH: b = positional ? PAD_Y : PAD_X; break;  /* top */
	case BTN_WEST:  b = positional ? PAD_X : PAD_Y; break;  /* left */
	default: return 0;
	}
	if (p->swap_xy) {
		if (b == PAD_X)
			b = PAD_Y;
		else if (b == PAD_Y)
			b = PAD_X;
	}
	*out = b;
	return 1;
}

void pad_set_layout(struct pad *p, enum pad_layout layout, int swap_xy)
{
	p->layout = layout;
	p->swap_xy = swap_xy;
	fprintf(stderr, "pad: face buttons follow the %s (%s)%s\n",
		layout == PAD_LAYOUT_POSITIONAL ? "position" : "printed labels",
		layout == PAD_LAYOUT_POSITIONAL
			? "bottom=A right=B left=X top=Y"
			: "right=A bottom=B top=X left=Y",
		swap_xy ? ", X and Y swapped" : "");
}

static int has_bit(const unsigned long *bits, int n)
{
	return (bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

/* Does this fd look like our gamepad? */
static int is_gamepad(int fd)
{
	unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];

	memset(keys, 0, sizeof(keys));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
		return 0;
	return has_bit(keys, BTN_SOUTH) && has_bit(keys, BTN_DPAD_UP);
}

static void read_axis_range(struct pad *p)
{
	struct input_absinfo info;

	p->axis_min = -1800;
	p->axis_max = 1800;
	p->axis_flat = 32;
	if (ioctl(p->fd, EVIOCGABS(ABS_X), &info) == 0 &&
	    info.maximum > info.minimum) {
		p->axis_min = info.minimum;
		p->axis_max = info.maximum;
		p->axis_flat = info.flat;
	}
}

int pad_open(struct pad *p, const char *path)
{
	char name[256] = { 0 };

	memset(p, 0, sizeof(*p));
	p->fd = -1;
	p->layout = PAD_LAYOUT_LABELS;

	if (path && *path) {
		p->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (p->fd >= 0 && !is_gamepad(p->fd)) {
			fprintf(stderr, "pad: %s is not a gamepad, searching\n",
				path);
			close(p->fd);
			p->fd = -1;
		}
	}

	if (p->fd < 0) {
		DIR *d = opendir("/dev/input");
		struct dirent *e;

		if (!d) {
			fprintf(stderr, "pad: /dev/input: %s\n", strerror(errno));
			return -1;
		}
		while ((e = readdir(d))) {
			char buf[300];
			int fd;

			if (strncmp(e->d_name, "event", 5))
				continue;
			snprintf(buf, sizeof(buf), "/dev/input/%s", e->d_name);
			fd = open(buf, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0)
				continue;
			if (is_gamepad(fd)) {
				p->fd = fd;
				break;
			}
			close(fd);
		}
		closedir(d);
	}

	if (p->fd < 0) {
		fprintf(stderr, "pad: no gamepad found\n");
		return -1;
	}

	if (ioctl(p->fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
		snprintf(name, sizeof(name), "(unknown)");
	read_axis_range(p);
	fprintf(stderr, "pad: %s (axes %d..%d, deadzone %d)\n", name,
		p->axis_min, p->axis_max, p->axis_flat);
	return 0;
}

void pad_close(struct pad *p)
{
	if (p->fd >= 0)
		close(p->fd);
	p->fd = -1;
	free(p->script);
	p->script = NULL;
	p->script_len = p->script_pos = 0;
}

/* ---- scripted input ----------------------------------------------------- */

/*
 * A tap is held for 90 ms, about what a thumb does, then 60 ms of nothing so
 * two taps of the same button are two edges and not one long hold. The gap
 * is also what keeps a press from landing in the same pad_poll as the
 * release of the previous step.
 */
#define PAD_SCRIPT_TAP_MS 90
#define PAD_SCRIPT_GAP_MS 60

enum pad_script_kind { SCRIPT_PRESS, SCRIPT_WAIT, SCRIPT_QUIT };

struct pad_script_step {
	enum pad_script_kind kind;
	uint32_t buttons;   /* SCRIPT_PRESS: bit per pad_button */
	int ms;             /* hold (PRESS) or pause (WAIT) length */
};

static int button_by_name(const char *name, size_t len)
{
	for (int b = 0; b < PAD_COUNT; b++) {
		const char *n = k_names[b];
		size_t i;

		for (i = 0; i < len && n[i]; i++)
			if (tolower((unsigned char)name[i]) !=
			    tolower((unsigned char)n[i]))
				break;
		if (i == len && !n[i])
			return b;
	}
	return -1;
}

/* "<name>" or "<name>+<name>..." -> button mask; 0 if any name is unknown. */
static uint32_t buttons_of(const char *s, size_t len)
{
	uint32_t mask = 0;

	while (len) {
		const char *plus = memchr(s, '+', len);
		size_t n = plus ? (size_t)(plus - s) : len;
		int b = button_by_name(s, n);

		if (b < 0)
			return 0;
		mask |= 1u << b;
		if (!plus)
			break;
		s += n + 1;
		len -= n + 1;
	}
	return mask;
}

/* One comma-separated token -> a step. Returns 0 if it does not parse. */
static int parse_step(const char *tok, size_t len, struct pad_script_step *st)
{
	const char *colon;

	while (len && isspace((unsigned char)*tok)) { tok++; len--; }
	while (len && isspace((unsigned char)tok[len - 1])) len--;
	if (!len)
		return 0;

	if (len == 4 && !strncasecmp(tok, "quit", 4)) {
		st->kind = SCRIPT_QUIT;
		return 1;
	}
	if (len > 5 && !strncasecmp(tok, "wait:", 5)) {
		st->kind = SCRIPT_WAIT;
		st->ms = atoi(tok + 5);
		return st->ms > 0;
	}
	if (len > 5 && !strncasecmp(tok, "hold:", 5)) {
		tok += 5;
		len -= 5;
		colon = memchr(tok, ':', len);
		if (!colon)
			return 0;
		st->kind = SCRIPT_PRESS;
		st->buttons = buttons_of(tok, (size_t)(colon - tok));
		st->ms = atoi(colon + 1);
		return st->buttons && st->ms > 0;
	}
	st->kind = SCRIPT_PRESS;
	st->buttons = buttons_of(tok, len);
	st->ms = PAD_SCRIPT_TAP_MS;
	return st->buttons != 0;
}

int pad_set_script(struct pad *p, const char *script)
{
	struct pad_script_step *steps;
	int n = 0, cap = 16;
	const char *s = script;

	if (!script)
		return -1;
	steps = malloc((size_t)cap * sizeof(*steps));
	if (!steps)
		return -1;
	for (;;) {
		const char *comma = strchr(s, ',');
		size_t len = comma ? (size_t)(comma - s) : strlen(s);
		struct pad_script_step st;

		memset(&st, 0, sizeof(st));
		if (len && !parse_step(s, len, &st)) {
			fprintf(stderr, "pad: script: cannot parse \"%.*s\"\n",
				(int)len, s);
			free(steps);
			return -1;
		}
		if (len) {
			if (n == cap) {
				struct pad_script_step *grown =
					realloc(steps, (size_t)cap * 2 * sizeof(*steps));
				if (!grown) {
					free(steps);
					return -1;
				}
				steps = grown;
				cap *= 2;
			}
			steps[n++] = st;
		}
		if (!comma)
			break;
		s = comma + 1;
	}
	free(p->script);
	p->script = steps;
	p->script_len = n;
	p->script_pos = 0;
	p->script_wait_until = 0;
	p->script_done = 0;
	memset(p->script_release_at, 0, sizeof(p->script_release_at));
	fprintf(stderr, "pad: script of %d steps installed%s\n", n,
		p->fd < 0 ? " (no pad device: scripted input only)" : "");
	return 0;
}

int pad_script_done(const struct pad *p)
{
	return p->script && p->script_done;
}

/* Milliseconds until the script next needs pad_poll's attention; -1 = never. */
static int script_due_in(const struct pad *p, int now)
{
	int have = 0, due = 0;

	if (!p->script || p->script_done)
		return -1;
	for (int b = 0; b < PAD_COUNT; b++) {
		int at = p->script_release_at[b];

		if (at && (!have || at - now < due)) {
			due = at - now;
			have = 1;
		}
	}
	if (p->script_pos < p->script_len) {
		int next = p->script_wait_until - now;

		if (!have || next < due) {
			due = next;
			have = 1;
		}
	}
	if (!have)
		return -1;
	return due < 0 ? 0 : due;   /* overdue: handle it now */
}

/* Let go of holds that are over and run every step that is due. */
static int script_step(struct pad *p, int now)
{
	int changed = 0;

	if (!p->script)
		return 0;
	for (int b = 0; b < PAD_COUNT; b++) {
		if (!p->script_release_at[b] || now < p->script_release_at[b])
			continue;
		p->script_release_at[b] = 0;
		p->down[b] = 0;
		changed = 1;
	}
	while (!p->script_done && p->script_pos < p->script_len &&
	       now >= p->script_wait_until) {
		const struct pad_script_step *st = &p->script[p->script_pos++];

		switch (st->kind) {
		case SCRIPT_QUIT:
			p->script_done = 1;
			break;
		case SCRIPT_WAIT:
			p->script_wait_until = now + st->ms;
			break;
		case SCRIPT_PRESS:
			for (int b = 0; b < PAD_COUNT; b++) {
				if (!(st->buttons & (1u << b)))
					continue;
				if (!p->down[b])
					p->pressed[b] = 1;
				p->down[b] = 1;
				p->script_release_at[b] = now + st->ms;
			}
			p->script_wait_until = now + st->ms + PAD_SCRIPT_GAP_MS;
			changed = 1;
			/* One press per poll, so a caller sees each edge. */
			return changed;
		}
	}
	return changed;
}

/* Scale a raw axis to int16, honouring the driver's flat (deadzone). */
static int16_t scale_axis(const struct pad *p, int value)
{
	int centre = (p->axis_max + p->axis_min) / 2;
	int range = (p->axis_max - p->axis_min) / 2;
	int v = value - centre;

	if (range <= 0)
		return 0;
	if (v > -p->axis_flat && v < p->axis_flat)
		return 0;
	if (v > range)
		v = range;
	if (v < -range)
		v = -range;
	return (int16_t)((long)v * 32767 / range);
}

/* ---- virtual pad (the simulator's browser view) -------------------------- */

/*
 * Written by the view's HTTP thread, read by whichever thread polls. Plain
 * relaxed atomics: the five values need not be consistent with each other
 * for one poll -- a stick that lags a button press by a millisecond is
 * invisible, and a mutex here would put a socket in the pad's path.
 */
static uint32_t g_virtual_buttons;
static int16_t g_virtual_axis[4];

void pad_virtual_set(uint32_t buttons, int16_t lx, int16_t ly, int16_t rx,
		     int16_t ry)
{
	__atomic_store_n(&g_virtual_axis[0], lx, __ATOMIC_RELAXED);
	__atomic_store_n(&g_virtual_axis[1], ly, __ATOMIC_RELAXED);
	__atomic_store_n(&g_virtual_axis[2], rx, __ATOMIC_RELAXED);
	__atomic_store_n(&g_virtual_axis[3], ry, __ATOMIC_RELAXED);
	__atomic_store_n(&g_virtual_buttons, buttons, __ATOMIC_RELAXED);
}

/* Fold the injected state in, generating the same edges a device would. */
static int virtual_step(struct pad *p)
{
	uint32_t m = __atomic_load_n(&g_virtual_buttons, __ATOMIC_RELAXED);
	int changed = 0;

	p->lx = __atomic_load_n(&g_virtual_axis[0], __ATOMIC_RELAXED);
	p->ly = __atomic_load_n(&g_virtual_axis[1], __ATOMIC_RELAXED);
	p->rx = __atomic_load_n(&g_virtual_axis[2], __ATOMIC_RELAXED);
	p->ry = __atomic_load_n(&g_virtual_axis[3], __ATOMIC_RELAXED);

	if (m == p->vmask)
		return 0;
	for (int b = 0; b < PAD_COUNT; b++) {
		int now = (int)((m >> b) & 1);

		if (now == (int)((p->vmask >> b) & 1))
			continue;
		if (now)
			p->pressed[b] = 1;
		p->down[b] = (uint8_t)now;
		changed = 1;
	}
	p->vmask = m;
	return changed;
}

int pad_poll(struct pad *p, int timeout_ms)
{
	struct pollfd pfd = { .fd = p->fd, .events = POLLIN };
	struct input_event ev;
	int changed = 0;
	int rc;

	if (p->script) {
		/* Wake for the script's next edge if that comes first, so a
		 * caller sleeping 200 ms between polls still sees a 90 ms tap. */
		int due = script_due_in(p, pad_now_ms());

		if (due >= 0 && (timeout_ms < 0 || due < timeout_ms))
			timeout_ms = due;
	}

	if (p->fd < 0) {
		/* No pad (the host simulator, or a handheld whose pad node
		 * went missing): still honour the timeout. Every menu loop in
		 * main.cpp uses this call as its only delay, so an instant -1
		 * turned each into a busy spin, and the sign-in screen, which
		 * counts its polling interval in timeouts, would have polled
		 * Microsoft back to back. With a pad this branch never runs. */
		if (timeout_ms > 0)
			poll(NULL, 0, timeout_ms);
		/* Only here: with a real device open the injected state is
		 * ignored, so it can never fight the handheld's own pad. */
		int v = virtual_step(p);

		if (p->script)
			return script_step(p, pad_now_ms()) || v;
		return v ? 1 : -1;
	}

	rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0)
		return errno == EINTR ? 0 : -1;
	if (rc == 0)
		return script_step(p, pad_now_ms());

	while (read(p->fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_KEY) {
			enum pad_button face;

			/* Before any mapping: what the driver actually said,
			 * which is what the button tester reports. */
			if (ev.value) {
				p->last_code = ev.code;
				p->last_value = ev.value;
			}
			if (face_map(p, ev.code, &face)) {
				if (ev.value && !p->down[face])
					p->pressed[face] = 1;
				p->down[face] = ev.value ? 1 : 0;
				changed = 1;
				continue;
			}
			for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); i++) {
				if (k_map[i].code != ev.code)
					continue;
				if (ev.value && !p->down[k_map[i].button])
					p->pressed[k_map[i].button] = 1;
				p->down[k_map[i].button] = ev.value ? 1 : 0;
				changed = 1;
			}
		} else if (ev.type == EV_ABS) {
			int16_t v = scale_axis(p, ev.value);

			switch (ev.code) {
			case ABS_X:  p->lx = v; changed = 1; break;
			case ABS_Y:  p->ly = v; changed = 1; break;
			case ABS_RX: p->rx = v; changed = 1; break;
			case ABS_RY: p->ry = v; changed = 1; break;
			default: break;
			}
		}
	}
	if (p->script)
		changed |= script_step(p, pad_now_ms());
	return changed;
}

int pad_take_press(struct pad *p, enum pad_button b)
{
	int was;

	if (b < 0 || b >= PAD_COUNT)
		return 0;
	was = p->pressed[b];
	p->pressed[b] = 0;
	return was;
}

int pad_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * Auto-repeat cadence. 350 ms before the first repeat is long enough that a
 * deliberate single press never doubles; 60 ms is a readable scroll speed;
 * after a second of holding, 25 ms so crossing 585 entries is possible at all.
 */
#define PAD_REPEAT_DELAY_MS  350
#define PAD_REPEAT_MS         60
#define PAD_REPEAT_FAST_MS    25
#define PAD_REPEAT_ACCEL_MS 1000

int pad_repeat(struct pad *p, enum pad_button b, int now_ms)
{
	int held, interval;

	if (b < 0 || b >= PAD_COUNT)
		return 0;
	if (!p->down[b]) {
		p->repeat_at[b] = 0;
		return 0;
	}
	if (!p->repeat_at[b]) {           /* the press itself */
		p->repeat_since[b] = now_ms;
		p->repeat_at[b] = now_ms + PAD_REPEAT_DELAY_MS;
		/* The press edge is consumed here so a caller mixing
		 * pad_repeat and pad_take_press on one button cannot act
		 * twice on the same press. */
		p->pressed[b] = 0;
		return 1;
	}
	if (now_ms < p->repeat_at[b])
		return 0;
	held = now_ms - p->repeat_since[b];
	interval = held > PAD_REPEAT_ACCEL_MS ? PAD_REPEAT_FAST_MS : PAD_REPEAT_MS;
	/* Schedule from now, not from the missed deadline: a slow frame would
	 * otherwise leave a backlog that fires several ticks at once. */
	p->repeat_at[b] = now_ms + interval;
	return 1;
}
