/*
 * Raw evdev gamepad reader for the Anbernic RG353.
 *
 * Measured on device: /dev/input/event3 "retrogame_joypad" carries ALL
 * seventeen buttons and both sticks. The separate adc-keys and gpio-keys
 * nodes only carry the F key and volume up/down, so they are ignored.
 *
 *   BTN_SOUTH BTN_EAST BTN_NORTH BTN_WEST BTN_TL BTN_TR BTN_TL2 BTN_TR2
 *   BTN_SELECT BTN_START BTN_MODE BTN_THUMBL BTN_THUMBR
 *   BTN_DPAD_UP BTN_DPAD_DOWN BTN_DPAD_LEFT BTN_DPAD_RIGHT
 *   ABS_X ABS_Y ABS_RX ABS_RY  (min -1800, max 1800, flat 32)
 *
 * The D-pad is buttons, not a hat. There is no ABS_Z/ABS_RZ, so L2 and R2 are
 * DIGITAL - the xCloud trigger fields get 0 or 65535 and nothing between.
 *
 * Face buttons: the shell is labelled Nintendo-style (B bottom, A right,
 * Y left, X top) while the codes follow Linux's positional names: the bottom
 * button (labelled B) emits BTN_SOUTH, the right one (labelled A) BTN_EAST,
 * the top one (labelled X) BTN_NORTH and the left one (labelled Y) BTN_WEST
 * (see face_map in evdev_pad.c for the evidence). PAD_A..PAD_Y here mean the
 * XBOX function the game sees, and pad_set_layout() chooses whether that
 * follows the printed label (default: press the button that says A to get
 * Xbox A, like green-nx's Nintendo mode) or the position (bottom is A, as on
 * an Xbox pad).
 *
 * This deliberately does not use SDL_GameController: SDL 2.0.22 computes a
 * different joystick GUID than newer SDL, and the two published community
 * mappings for this pad disagree about A and B.
 */
#ifndef XCLOUD_EVDEV_PAD_H
#define XCLOUD_EVDEV_PAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum pad_button {
	PAD_A, PAD_B, PAD_X, PAD_Y,
	PAD_L1, PAD_R1, PAD_L2, PAD_R2,
	PAD_SELECT, PAD_START, PAD_MODE,
	PAD_L3, PAD_R3,
	PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
	PAD_COUNT
};

enum pad_layout {
	PAD_LAYOUT_LABELS,      /* the printed letter is the Xbox button */
	PAD_LAYOUT_POSITIONAL,  /* bottom=A right=B left=X top=Y, Xbox shape */
};

struct pad {
	int fd;
	uint8_t down[PAD_COUNT];      /* current state */
	uint8_t pressed[PAD_COUNT];   /* edge since last poll; poll clears it */
	int16_t lx, ly, rx, ry;       /* normalised to -32768..32767 */
	int axis_min, axis_max, axis_flat;
	/* pad_repeat bookkeeping, per button. */
	int repeat_at[PAD_COUNT];     /* ms of the next tick; 0 = not held */
	int repeat_since[PAD_COUNT];  /* ms the hold began, for acceleration */
	enum pad_layout layout;
	int swap_xy;                  /* escape hatch if the X/Y wiring differs */

	/* Scripted presses (pad_set_script); NULL on the device unless asked. */
	struct pad_script_step *script;
	int script_len, script_pos;
	int script_wait_until;            /* ms: the next step is due then */
	int script_release_at[PAD_COUNT]; /* ms a synthetic hold lets go; 0=none */
	int script_done;                  /* the "quit" step was reached */

	/* Last virtual state applied (pad_virtual_set), for edge detection. */
	uint32_t vmask;
};

/*
 * Inject pad state from somewhere other than a device: the host
 * simulator's browser view (src/video/sim_view.c) turns keystrokes into
 * this. `buttons` is a bitmask of (1 << enum pad_button).
 *
 * Applied by pad_poll ONLY when there is no real device open, so it can
 * never fight a handheld's own pad. Callable from another thread.
 */
void pad_virtual_set(uint32_t buttons, int16_t lx, int16_t ly, int16_t rx,
		     int16_t ry);

/*
 * Open the pad. `path` may be the value EmulationStation passes as
 * -p1devicepath; pass NULL to search /dev/input for a device advertising
 * BTN_SOUTH. Returns 0 on success.
 */
int pad_open(struct pad *p, const char *path);
void pad_close(struct pad *p);

/* Choose how the four face buttons map (see the header comment). Call AFTER
 * pad_open, which resets the pad to the labels layout; logs the table. */
void pad_set_layout(struct pad *p, enum pad_layout layout, int swap_xy);

/*
 * Drain pending events, updating state. timeout_ms < 0 blocks forever.
 * Returns 1 if anything changed, 0 on timeout, negative on error.
 */
int pad_poll(struct pad *p, int timeout_ms);

/* Consume a press edge. */
int pad_take_press(struct pad *p, enum pad_button b);

/*
 * Held-button auto-repeat: returns 1 on the initial press and then on each
 * repeat tick while the button stays down. `now_ms` is any monotonic
 * millisecond clock; pad_now_ms() below supplies one.
 *
 * Menus need this rather than pad_take_press: the library has 585 entries and
 * a press-edge-only list means 585 presses. The cadence is the usual one --
 * an initial delay, then steady repeats, accelerating after a second so long
 * runs do not crawl.
 */
int pad_repeat(struct pad *p, enum pad_button b, int now_ms);

/* Monotonic milliseconds, for pad_repeat. */
int pad_now_ms(void);

/* Name for logging. */
const char *pad_button_name(enum pad_button b);

/*
 * The letter PRINTED on the shell for an Xbox function, for on-screen button
 * hints. With PAD_LAYOUT_LABELS this is the same letter; with the positional
 * layout the button that gives PAD_A is the bottom one, which says "B".
 */
const char *pad_button_label(const struct pad *p, enum pad_button b);

/*
 * Scripted input, for driving the UI with no one holding the handheld (the
 * host simulator has no pad at all). A comma-separated list of steps:
 *
 *   A, Down, R1 ...          tap a button (pad_button_name() names, any case;
 *                            "Select+Start" taps several at once)
 *   hold:<name>:<ms>         hold a button that long, so pad_repeat ticks
 *   wait:<ms>                do nothing for a while
 *   quit                     set pad_script_done(); loops treat it as the
 *                            SELECT+START quit combo
 *
 * pad_poll consumes the steps on the wall clock and synthesises the same
 * press/release edges a real pad would, so pad_take_press and pad_repeat see
 * no difference. Works with or without a device open. Call after pad_open.
 * Returns 0, or -1 (nothing installed) if a step does not parse.
 */
int pad_set_script(struct pad *p, const char *script);
int pad_script_done(const struct pad *p);

#ifdef __cplusplus
}
#endif

#endif /* XCLOUD_EVDEV_PAD_H */
