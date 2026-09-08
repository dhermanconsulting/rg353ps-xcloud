/*
 * The in-app options menu: table, persistence, overlay and screen.
 *
 * Everything the command line can set that is worth changing by hand lives
 * in one table, so adding a switch is one row here and nothing else. Each
 * row is an index into a list of choices; apply() turns those indices into
 * the fields the rest of the client reads, which keeps the menu from having
 * to know how any of them are represented.
 */
#include "options.hpp"

#include <fstream>

#include "../../third_party/nlohmann/json.hpp"

using nlohmann::json;

namespace app {
namespace {

/* ---- the table ----------------------------------------------------------- */

/* Order matches kOpts, which is grouped: Standard first, then Advanced. */
enum {
	OPT_LAYOUT, OPT_SWAPXY, OPT_FIT, OPT_PAN, OPT_SCALE, OPT_TIER,
	OPT_LOCALE,
	OPT_LATCH, OPT_SIZE, OPT_BITRATE, OPT_THREADS, OPT_WIFI, OPT_RT,
	OPT_COUNT
};

const char *const kScale[]   = { "hardware", "box (CPU)", "sharp (CPU)" };
const char *const kLatch[]   = { "off", "2 ms", "4 ms", "6 ms", "8 ms",
				 "10 ms", "12 ms" };
const char *const kSize[]    = { "as sent", "640x360", "960x540", "1280x720" };
const char *const kBitrate[] = { "default", "1200k", "2000k", "3000k", "5000k",
				 "8000k", "12000k", "20000k" };
const char *const kThreads[] = { "frame x2", "frame x3", "slice" };
const char *const kOnOff[]   = { "off", "on" };
const char *const kFit[]     = { "letterbox", "fill screen" };
/* Percent across the slack the crop leaves, centre first: the middle is what
 * anyone wants nearly always, so it is both the default and the first stop. */
const char *const kPan[]     = { "centre", "left", "hard left",
				 "right", "hard right" };
const char *const kLayout[]  = { "printed labels", "Xbox positions" };
/* Order matches QualityTier. 720p high is the one worth reaching for on this
 * panel: same size, ~50% more bitrate, no extra decode. */
const char *const kTier[]    = { "720p", "720p high", "1080p", "1080p high" };
/* The streamed console's system language. Kept short on purpose: this picks
 * the dashboard and store locale, not the client's own text. */
const char *const kLocale[]  = { "en-GB", "en-US", "fr-FR", "de-DE", "es-ES",
				 "it-IT", "pt-BR", "ja-JP" };

/*
 * Which page a setting belongs on. Standard is what someone might reasonably
 * want to change about how the thing looks and feels; Advanced is the tuning
 * and the diagnostics, including the two that can make the client worse if
 * set wrongly (the bitrate cap deadlocked recovery until 2026-09-07, see
 * KNOWN-ISSUES 0b).
 */
enum OptGroup { GRP_STANDARD, GRP_ADVANCED, GRP_COUNT };

const char *const kGroupName[GRP_COUNT] = { "Standard", "Advanced" };

struct Opt {
	const char *id;       /* key in options.json */
	const char *label;
	const char *help;     /* one line, shown for the highlighted row */
	const char *const *choices;
	int n;
	bool live;            /* takes effect on the stream already running */
	OptGroup group;
	int def;              /* the recommended value, and what Reset uses */
	/*
	 * The long explanation, shown by the Explain button. Written for
	 * someone technical: what the setting does, what it costs, and when
	 * to reach for it. A blank line starts a new paragraph; draw_detail()
	 * wraps to the panel and honours them. Kept to one screen.
	 */
	const char *detail;
};

const Opt kOpts[OPT_COUNT] = {
	{ "layout", "Face buttons",
	  "Whether A is the button marked A, or the bottom button.",
	  kLayout, 2, true, GRP_STANDARD, 0,
	  "Sets how the four face buttons map to controller functions.\n"
	  "\n"
	  "Printed labels follow the letters marked on this device, so the "
	  "button marked A acts as A. Xbox positions follow the physical "
	  "arrangement of a standard controller, where the bottom button acts "
	  "as A.\n"
	  "\n"
	  "Choose Xbox positions if on-screen prompts appear to point at the "
	  "wrong button." },
	{ "swapxy", "Swap X and Y",
	  "Corrects X and Y if they act as each other.",
	  kOnOff, 2, true, GRP_STANDARD, 0,
	  "Exchanges the X and Y buttons.\n"
	  "\n"
	  "Use this only if the two act as each other in games. It is "
	  "independent of the Face buttons setting and affects nothing "
	  "else." },
	{ "fit", "Picture",
	  "Show the whole picture, or fill the screen and crop the sides.",
	  kFit, 2, true, GRP_STANDARD, 0,
	  "The stream is widescreen and this display is not, so one of the "
	  "two has to give.\n"
	  "\n"
	  "Letterbox shows the entire picture, with black bands above and "
	  "below. Fill screen enlarges it to reach all four edges, placing "
	  "roughly an eighth of each side beyond the display.\n"
	  "\n"
	  "Fill screen also asks more of the display hardware and can cause "
	  "occasional glitching. Letterbox is recommended unless the bands "
	  "bother you." },
	{ "pan", "Pan",
	  "Which part of the picture to keep when filling the screen.",
	  kPan, 5, true, GRP_STANDARD, 0,
	  "When Picture is set to fill screen, part of the image falls "
	  "outside the display. This chooses which part is kept.\n"
	  "\n"
	  "Centre suits almost every game. The other positions help when "
	  "something you need to watch sits hard against one edge, such as a "
	  "map or a lap counter.\n"
	  "\n"
	  "It has no effect while the picture is letterboxed." },
	{ "scale", "Downscale",
	  "Which method shrinks the picture to fit the display.",
	  kScale, 3, true, GRP_STANDARD, 2,
	  "The stream arrives larger than the display and has to be "
	  "reduced.\n"
	  "\n"
	  "Hardware lets the display controller do it. That is free, but it "
	  "handles fine detail poorly: features only one pixel wide, such as "
	  "the strokes of small text, can be dropped entirely.\n"
	  "\n"
	  "Box and Sharp do the work on the processor instead and treat every "
	  "pixel alike, at a cost of about a millisecond per frame. Sharp "
	  "also adds a little edge definition, and is what this is set to.\n"
	  "\n"
	  "Try Hardware if you would rather have the millisecond back, or Box "
	  "if Sharp's edges look overdone to you." },
	{ "tier", "Stream quality",
	  "Picture size and bitrate requested from the service.",
	  kTier, 4, false, GRP_STANDARD, 0,
	  "Asks the service for a quality level.\n"
	  "\n"
	  "720p matches this display and is recommended. 720p high requests "
	  "the same size at around half again the bitrate, which noticeably "
	  "improves fine detail and text, but needs a stronger network "
	  "connection to sustain.\n"
	  "\n"
	  "The 1080p settings are beyond what this device can decode smoothly "
	  "and are included only for testing." },
	{ "locale", "Console language",
	  "System language used by the console you stream from.",
	  kLocale, 8, false, GRP_STANDARD, 0,
	  "Sets the language the remote console runs in, which affects its "
	  "dashboard, its store, and any game that follows the system "
	  "language.\n"
	  "\n"
	  "It does not change the language of this application." },
	{ "latch", "Late latch",
	  "Reduces delay between input and picture. Risks dropped frames.",
	  kLatch, 7, true, GRP_ADVANCED, 0,
	  "Holds each frame back until closer to the moment the display "
	  "refreshes, so what you see is more recent. This can remove up to "
	  "about 10 milliseconds between pressing a button and seeing the "
	  "result.\n"
	  "\n"
	  "Set it too high and the frame misses the refresh, and the previous "
	  "one is shown again. Increase it a step at a time, and reduce it if "
	  "the picture starts to stutter." },
	/*
	 * Defaults to declaring 640x360 -- the rectangle this panel actually
	 * shows -- rather than the tier's 1280x720. Measured 2026-09-05: it
	 * costs nothing on the titles that ignore it (same 720p, same
	 * bitrate) and gets a native 640x360 encode out of the ones that have
	 * adopted XGameStreamingSetResolution, which removes the downscale
	 * altogether. Fortnite is one. docs/RESOLUTION.md.
	 */
	{ "size", "Preferred size",
	  "The picture size this device asks games to render at.",
	  kSize, 4, false, GRP_ADVANCED, 1,
	  "Tells the service what size picture this display wants.\n"
	  "\n"
	  "A small number of games act on it and render at that size. When "
	  "one does, the result is noticeably better here: no scaling is "
	  "needed, the network carries about a third as much, and the device "
	  "does far less decoding work.\n"
	  "\n"
	  "Most games ignore it, in which case the setting changes nothing and "
	  "costs nothing." },
	{ "bitrate", "Bitrate cap",
	  "Upper limit on stream bitrate. Leave at default.",
	  kBitrate, 8, false, GRP_ADVANCED, 0,
	  "Limits the bitrate the service may use.\n"
	  "\n"
	  "Lowering it does not produce a smaller picture. The service keeps "
	  "the same size and reduces quality instead.\n"
	  "\n"
	  "Setting it very low can stop a stream recovering after network "
	  "loss, because there is not enough bandwidth to send the complete "
	  "frame needed to resume. The default lets the service choose and is "
	  "recommended." },
	{ "threads", "Decode threads",
	  "How video decoding is spread across processor cores.",
	  kThreads, 3, false, GRP_ADVANCED, 0,
	  "Chooses how video decoding is divided up.\n"
	  "\n"
	  "Frame x2 is recommended. It decodes consecutive frames in "
	  "parallel, roughly doubling the headroom available, and adds a few "
	  "milliseconds of delay. Frame x3 adds little on this device.\n"
	  "\n"
	  "Slice decodes on a single core. The service sends frames in a form "
	  "that cannot be divided any further, so this is slower and is "
	  "included for comparison." },
	{ "wifi", "Wi-Fi tuning",
	  "Disables wireless power saving while streaming.",
	  kOnOff, 2, false, GRP_ADVANCED, 1,
	  "While a stream is running, switches off the wireless adapter's "
	  "power saving and stops it searching for other networks.\n"
	  "\n"
	  "Both add delay and cause brief interruptions, and together they "
	  "were the main cause of regular stuttering on this device. Normal "
	  "behaviour is restored when the stream ends.\n"
	  "\n"
	  "Leave this on unless investigating a network problem." },
	{ "rt", "Priority scheduling",
	  "Gives display and network work priority over decoding.",
	  kOnOff, 2, false, GRP_ADVANCED, 1,
	  "Raises the scheduling priority of the display and network tasks, "
	  "so they are dealt with promptly even while the processor is busy "
	  "decoding video.\n"
	  "\n"
	  "Without it, both compete with decoding for time and the picture "
	  "can arrive late. Leave this on; turning it off is only useful for "
	  "measuring the difference it makes." },
};

int g_val[OPT_COUNT];
bool g_open;
int g_cursor;

/*
 * The settings PAGE (the library's Settings tab) edits a staged copy and
 * commits it on Save; the in-stream OVERLAY still applies as you cycle.
 *
 * That difference is deliberate rather than an inconsistency. The overlay
 * exists so a filter can be changed while watching the same scene -- applying
 * on the next stream would defeat the whole point of it. The page is where
 * you sit and configure, and there "I cycled past a value and it took effect"
 * is a trap, particularly for the bitrate cap, which is one wrong step away
 * from a stream that cannot recover from a lost frame.
 */
int g_edit[OPT_COUNT];
int g_page_row;                /* cursor within the current sub-tab */
bool g_page_detail;            /* the Explain panel is open over the page */
OptGroup g_page_group = GRP_STANDARD;

/* The overlay's two action rows, after its live settings. */
enum { OVL_APPLY, OVL_SAVE, OVL_COUNT };
const char *const kOvlName[OVL_COUNT] = { "Apply now", "Apply and save" };

/* The two action rows live after the settings of the current sub-tab. */
enum { ACT_SAVE, ACT_RESET, ACT_COUNT };
const char *const kActName[ACT_COUNT] = { "Save changes",
					  "Reset all to defaults" };

const int kSizes[][2] = { { 0, 0 }, { 640, 360 }, { 960, 540 }, { 1280, 720 } };
const int kRates[]    = { 0, 1200, 2000, 3000, 5000, 8000, 12000, 20000 };
const int kLatchMs[]  = { 0, 2, 4, 6, 8, 10, 12 };

/* ---- table <-> the fields the client actually reads ---------------------- */

void apply()
{
	g_opts.scale = g_val[OPT_SCALE];
	g_opts.latch_ms = kLatchMs[g_val[OPT_LATCH]];
	g_opts.req_w = kSizes[g_val[OPT_SIZE]][0];
	g_opts.req_h = kSizes[g_val[OPT_SIZE]][1];
	g_opts.req_bitrate = kRates[g_val[OPT_BITRATE]];
	g_opts.pipeline.threading =
		g_val[OPT_THREADS] == 0   ? DECODER_THREADS_FRAME2
		: g_val[OPT_THREADS] == 1 ? DECODER_THREADS_FRAME3
					  : DECODER_THREADS_SLICE;
	g_opts.wifi_tune = g_val[OPT_WIFI] != 0;
	g_opts.realtime = g_val[OPT_RT] != 0;
	g_opts.tier = g_val[OPT_TIER];
	g_opts.locale = kLocale[g_val[OPT_LOCALE]];
	g_opts.pad_layout = g_val[OPT_LAYOUT];
	g_opts.pad_swapxy = g_val[OPT_SWAPXY] != 0;
	g_opts.zoom_fill = g_val[OPT_FIT] != 0;
	/* centre, left, hard left, right, hard right */
	static const int kPanPct[] = { 0, -50, -100, 50, 100 };
	g_opts.pan = kPanPct[g_val[OPT_PAN]];
}

/*
 * The face-button mapping is state inside the open pad, not something read
 * per frame, so the two rows that describe it have to be pushed to the
 * device as well as to g_opts. Both menus have the pad to hand: it is the
 * one they are being driven by.
 */
void apply_pad(pad &p)
{
	pad_set_layout(&p,
		       g_opts.pad_layout ? PAD_LAYOUT_POSITIONAL
					 : PAD_LAYOUT_LABELS,
		       g_opts.pad_swapxy ? 1 : 0);
}

/* Nearest entry in a list of values, so a flag that set something the menu
 * has no exact choice for still shows the closest thing rather than junk. */
int nearest(const int *values, int n, int want)
{
	int best = 0, best_d = -1;

	for (int i = 0; i < n; i++) {
		int d = values[i] > want ? values[i] - want : want - values[i];

		if (best_d < 0 || d < best_d) {
			best_d = d;
			best = i;
		}
	}
	return best;
}

/*
 * Fixed at options_load(), not derived per call, because the state
 * directory can move afterwards: fake_library() appends "/fake" to it so a
 * synthetic run cannot clobber the real caches. Deriving the path twice
 * meant loading from one file and saving to another, and the options a
 * simulator run changed were never seen again.
 */
std::string g_path;

std::string path()
{
	return g_path.empty() ? g_opts.state_dir + "/options.json" : g_path;
}

/* ---- the settings page --------------------------------------------------- */

/* Indices of the settings on one sub-tab, in table order. */
std::vector<int> group_rows(OptGroup g)
{
	std::vector<int> out;

	for (int i = 0; i < OPT_COUNT; i++)
		if (kOpts[i].group == g)
			out.push_back(i);
	return out;
}

/*
 * The rows the in-stream overlay offers: only the settings that take effect
 * on the stream already running. Anything read at session start would sit
 * there doing nothing until the next one, which is worse than not offering
 * it -- the whole point of the overlay is to change something and see it.
 */
std::vector<int> live_rows()
{
	std::vector<int> out;

	for (int i = 0; i < OPT_COUNT; i++)
		if (kOpts[i].live)
			out.push_back(i);
	return out;
}

bool page_dirty()
{
	for (int i = 0; i < OPT_COUNT; i++)
		if (g_edit[i] != g_val[i])
			return true;
	return false;
}

/* ---- drawing ------------------------------------------------------------- */

/*
 * The overlay is drawn straight into the frame in the back buffer, which is
 * write-combine memory: every call here writes and never reads (screen_rect*
 * are memsets and the glyph blit needs a flat background, which the panel
 * gives it). Sizes come from the buffer, not the panel, because with the
 * hardware downscale the buffer is 1280x720 and everything in it is halved
 * before it reaches the eye.
 */
void draw_panel(uint8_t *luma, uint8_t *chroma, int pitch, int W, int H,
		Fonts &f, bool overlay)
{
	const bool big = H >= 600;
	text_ctx *fl = big ? f.big : f.mid;
	text_ctx *fs = big ? f.mid : f.small;
	const int lh = text_line_height(fl);
	const int hh = text_line_height(fs);
	const int asc = text_ascent(fl);
	const int pad = lh / 2;
	std::vector<int> rows = live_rows();
	bool dirty = false;
	for (int i = 0; i < OPT_COUNT; i++)
		if (g_edit[i] != g_val[i])
			dirty = true;
	const int panel_w = W - 2 * (W / 12);
	const int panel_h = pad * 3 +
			    lh * ((int)rows.size() + OVL_COUNT +
				  (overlay ? 1 : 0)) +
			    hh * 2;
	const int px = (W - panel_w) / 2;
	const int py = (H - panel_h) / 2 > 0 ? (H - panel_h) / 2 : 0;

	screen_rect_colour(luma, chroma, pitch, W, H, px, py, panel_w, panel_h,
			   26, 128, 128);
	screen_frame(luma, pitch, W, H, px, py, panel_w, panel_h, big ? 2 : 1,
		     110);

	/* The full screen has the Painter's own header above it, so the
	 * panel only titles itself when it is floating over a game. */
	int y = py + pad;

	if (overlay) {
		const char *hint = "SELECT+X closes";
		int w = text_measure(fs, hint);

		text_draw(fl, luma, pitch, W, H, px + pad, y + asc, "Options",
			  235);
		text_draw(fs, luma, pitch, W, H, px + panel_w - pad - w,
			  y + asc, hint, 140);
		y += lh;
	}

	for (size_t r = 0; r < rows.size(); r++) {
		const int i = rows[r];
		const bool sel = (int)r == g_cursor;
		const char *v = kOpts[i].choices[g_edit[i]];
		int w = text_measure(fl, v);

		if (sel)
			screen_rect(luma, pitch, W, H, px + pad / 2, y,
				    panel_w - pad, lh, 64);
		text_draw(fl, luma, pitch, W, H, px + pad, y + asc,
			  kOpts[i].label, sel ? 245 : 190);
		text_draw(fl, luma, pitch, W, H, px + panel_w - pad - w,
			  y + asc, v,
			  sel		       ? 245
			  : g_edit[i] != g_val[i] ? 235
						 : 205);
		y += lh;
	}

	for (int a = 0; a < OVL_COUNT; a++) {
		const bool sel = g_cursor == (int)rows.size() + a;

		if (sel)
			screen_rect(luma, pitch, W, H, px + pad / 2, y,
				    panel_w - pad, lh, 64);
		text_draw(fl, luma, pitch, W, H, px + pad, y + asc,
			  kOvlName[a], sel ? 245 : (dirty ? 215 : 150));
		y += lh;
	}

	y += pad;
	text_draw(fs, luma, pitch, W, H, px + pad, y + text_ascent(fs),
		  g_cursor < (int)rows.size()
			  ? kOpts[rows[g_cursor]].help
			  : "Apply changes the running stream; save also keeps them",
		  165);
	y += hh;
	text_draw(fs, luma, pitch, W, H, px + pad, y + text_ascent(fs),
		  dirty ? "Unsaved changes  ·  B closes without applying"
			: "Only settings that act on the running stream",
		  130);
}

}  // namespace

/* ---- persistence --------------------------------------------------------- */

void options_load()
{
	g_path = g_opts.state_dir + "/options.json";

	std::ifstream in(path(), std::ios::binary);
	json j = in ? json::parse(in, nullptr, false) : json();

	/*
	 * A key the file does not carry falls back to that setting's `def`,
	 * which is the same value Reset stages and the same one the Explain
	 * panel names as the default -- so there is exactly one place a
	 * recommendation is written down.
	 *
	 * This used to fall back to 0 with three settings patched up
	 * afterwards (wifi, rt and the 640x360 preferred size), which meant
	 * `def` was only ever a hint to the reader: changing it moved what
	 * Reset did and what the panel claimed, and left what a fresh install
	 * actually ran with alone. Two of the three ways of saying "the
	 * default" disagreeing with the third is a trap, and the Downscale
	 * default walked into it the day it moved off hardware.
	 *
	 * Absence, not a reordering of the choice lists: a saved options.json
	 * that meant something still means it.
	 */
	for (int i = 0; i < OPT_COUNT; i++) {
		int v = j.is_object() ? j.value(kOpts[i].id, -1) : -1;

		g_val[i] = v >= 0 && v < kOpts[i].n ? v : kOpts[i].def;
	}
	apply();
}

void options_save()
{
	json j;

	for (int i = 0; i < OPT_COUNT; i++)
		j[kOpts[i].id] = g_val[i];

	const std::string p = path(), tmp = p + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);

		if (!out)
			return;
		const std::string body = j.dump();
		out.write(body.data(), (std::streamsize)body.size());
		if (!out)
			return;
	}
	if (std::rename(tmp.c_str(), p.c_str()))
		std::remove(tmp.c_str());
}

void options_capture()
{
	g_val[OPT_SCALE] = g_opts.scale >= 0 && g_opts.scale < 3
				   ? (int)g_opts.scale
				   : 0;
	g_val[OPT_LATCH] = nearest(kLatchMs, 7, g_opts.latch_ms);
	g_val[OPT_SIZE] = 0;
	for (int i = 1; i < 4; i++)
		if (kSizes[i][0] == g_opts.req_w && kSizes[i][1] == g_opts.req_h)
			g_val[OPT_SIZE] = i;
	g_val[OPT_BITRATE] = g_opts.req_bitrate > 0
				     ? nearest(kRates, 8, g_opts.req_bitrate)
				     : 0;
	g_val[OPT_THREADS] =
		g_opts.pipeline.threading == DECODER_THREADS_FRAME2   ? 0
		: g_opts.pipeline.threading == DECODER_THREADS_FRAME3 ? 1
								      : 2;
	g_val[OPT_WIFI] = g_opts.wifi_tune ? 1 : 0;
	g_val[OPT_RT] = g_opts.realtime ? 1 : 0;
	g_val[OPT_TIER] = g_opts.tier >= 0 && g_opts.tier < 4 ? g_opts.tier : 0;
	g_val[OPT_LOCALE] = 0;
	for (int i = 0; i < 8; i++)
		if (g_opts.locale == kLocale[i])
			g_val[OPT_LOCALE] = i;
	g_val[OPT_LAYOUT] = g_opts.pad_layout ? 1 : 0;
	g_val[OPT_SWAPXY] = g_opts.pad_swapxy ? 1 : 0;
	g_val[OPT_FIT] = g_opts.zoom_fill ? 1 : 0;
	g_val[OPT_PAN] = 0;
}

/* ---- the overlay --------------------------------------------------------- */

bool options_menu_open()
{
	return g_open;
}

void options_menu_input(pad &p, int now)
{
	std::vector<int> rows = live_rows();
	const int n = (int)rows.size() + OVL_COUNT;

	if (!g_open) {
		/*
		 * SELECT + X. Every plain button belongs to the game while a
		 * stream is running, so this needs a modifier; SELECT+START
		 * is already quit (see quit_combo).
		 */
		if (p.down[PAD_SELECT] && pad_take_press(&p, PAD_X)) {
			g_open = true;
			g_cursor = 0;
			for (int i = 0; i < OPT_COUNT; i++)
				g_edit[i] = g_val[i];
		}
		return;
	}
	if (pad_take_press(&p, PAD_B) ||
	    (p.down[PAD_SELECT] && pad_take_press(&p, PAD_X))) {
		/* Closing discards. Apply and Save are the two ways to keep a
		 * change, and neither is a side effect of leaving. */
		g_open = false;
		return;
	}
	if (pad_repeat(&p, PAD_UP, now))
		g_cursor = (g_cursor + n - 1) % n;
	if (pad_repeat(&p, PAD_DOWN, now))
		g_cursor = (g_cursor + 1) % n;

	const bool on_action = g_cursor >= (int)rows.size();
	int delta = 0;
	if (pad_repeat(&p, PAD_LEFT, now))
		delta = -1;
	if (pad_repeat(&p, PAD_RIGHT, now))
		delta = 1;
	if (!on_action && pad_take_press(&p, PAD_A))
		delta = 1;
	if (!on_action && delta) {
		const int i = rows[g_cursor];

		g_edit[i] = (g_edit[i] + delta + kOpts[i].n) % kOpts[i].n;
		return;
	}
	if (on_action && pad_take_press(&p, PAD_A)) {
		/* Apply takes effect on the stream now; Save does that and
		 * writes it, because saving something you have not put into
		 * effect would be a strange thing to offer. */
		for (int i = 0; i < OPT_COUNT; i++)
			g_val[i] = g_edit[i];
		apply();
		apply_pad(p);
		if (g_cursor - (int)rows.size() == OVL_SAVE)
			options_save();
	}
}

void options_menu_draw(drm_out &out, Fonts &f)
{
	drm_out_buf *b = drm_out_back_buffer(&out);

	if (!b)
		return;
	draw_panel(b->luma, b->chroma, b->pitch, out.src_w, out.src_h, f, true);
}

/* ---- the settings page, as a tab in the library -------------------------- */

void options_page_enter()
{
	for (int i = 0; i < OPT_COUNT; i++)
		g_edit[i] = g_val[i];
	g_page_row = 0;
	g_page_group = GRP_STANDARD;
	g_page_detail = false;
}

bool options_page_unsaved() { return page_dirty(); }

bool options_page_explaining() { return g_page_detail; }

/*
 * The sub-tab strip, driven from the library: it owns Left/Right up there,
 * because Left/Right anywhere else on this page cycle a value.
 */
void options_page_group_step(int dir)
{
	g_page_group = (OptGroup)((g_page_group + GRP_COUNT + dir) % GRP_COUNT);
	g_page_row = 0;
}

bool options_page_at_top() { return g_page_row == 0; }

void options_page_to_top() { g_page_row = 0; }

/*
 * Returns true when the page wants to keep the press, false to let the caller
 * have it (B with nothing staged, so the library can leave the tab).
 */
bool options_page_input(pad &p, int now)
{
	std::vector<int> rows = group_rows(g_page_group);
	const int n = (int)rows.size() + ACT_COUNT;

	/*
	 * While the explanation is up it takes every press, so nothing can be
	 * changed by a button aimed at dismissing it. B closes it here rather
	 * than leaving the tab, which is the same unwind rule as everywhere
	 * else: innermost thing first.
	 */
	if (g_page_detail) {
		const bool close = pad_take_press(&p, PAD_B) ||
				   pad_take_press(&p, PAD_Y) ||
				   pad_take_press(&p, PAD_A);
		/* Swallow the rest so the list cannot move underneath, but
		 * report "nothing happened" unless it actually did: the
		 * caller repaints on true, and a panel that claims a change
		 * every poll repaints at 60 Hz for no reason. */
		for (int b = 0; b < PAD_COUNT; b++)
			pad_take_press(&p, (enum pad_button)b);
		if (close)
			g_page_detail = false;
		return close;
	}
	/* Only settings have an explanation; the action rows are their own. */
	if (pad_take_press(&p, PAD_Y) && g_page_row < (int)rows.size()) {
		g_page_detail = true;
		return true;
	}

	/*
	 * The shoulders are not ours. They cycle the primary tabs from
	 * everywhere on this screen and mean nothing else anywhere, so the
	 * library takes them before this is ever called; Standard against
	 * Advanced is reached by going Up onto the strip, where Left and
	 * Right are free because they are not editing a value up there.
	 */

	/*
	 * Moving the cursor is a change like any other and has to be reported:
	 * the caller repaints on true and nothing else in this screen is
	 * animated, so returning false here left the highlight where it was
	 * drawn while g_page_row had already moved. The D-pad then looked
	 * dead until some other press forced a repaint and the selection
	 * appeared to jump several rows at once.
	 */
	const int was_row = g_page_row;

	/*
	 * The ends stop rather than wrap. They used to wrap, which was fine
	 * when this band was the only thing the D-pad could reach; now Up at
	 * the first row is how the cursor gets back to the sub-tabs (the
	 * library takes that press before we see it), and a Down that
	 * wrapped round to the top while Up escaped would be a strip that
	 * behaves differently at each end for no reason a thumb can feel.
	 */
	if (pad_repeat(&p, PAD_DOWN, now) && g_page_row < n - 1)
		g_page_row++;
	if (pad_repeat(&p, PAD_UP, now) && g_page_row > 0)
		g_page_row--;
	const bool moved = g_page_row != was_row;

	const bool on_action = g_page_row >= (int)rows.size();
	int delta = 0;
	if (pad_repeat(&p, PAD_LEFT, now))
		delta = -1;
	if (pad_repeat(&p, PAD_RIGHT, now))
		delta = +1;
	if (!on_action && pad_take_press(&p, PAD_A))
		delta = +1;
	if (!on_action && delta) {
		const int i = rows[g_page_row];
		g_edit[i] = (g_edit[i] + delta + kOpts[i].n) % kOpts[i].n;
		return true;
	}

	if (on_action && pad_take_press(&p, PAD_A)) {
		if (g_page_row - (int)rows.size() == ACT_SAVE) {
			for (int i = 0; i < OPT_COUNT; i++)
				g_val[i] = g_edit[i];
			apply();
			apply_pad(p);
			options_save();
		} else {
			/* Reset stages the defaults rather than writing them,
			 * so it can be looked at and undone with B like any
			 * other change. */
			for (int i = 0; i < OPT_COUNT; i++)
				g_edit[i] = kOpts[i].def;
		}
		return true;
	}
	return moved;
}

/*
 * The Explain panel: the setting, what it is on, what it defaults to, and the
 * long text wrapped to the panel with text_break(). Drawn over the page
 * rather than as its own screen so the value stays in view while you read
 * about it.
 */
void draw_detail(Painter &pt, Fonts &f, int top, int bottom, int opt)
{
	const Opt &o = kOpts[opt];
	const int pad = 16;
	const int wrap_w = kWidth - 2 * pad - 8;
	const int lh = text_line_height(f.small) + 2;
	int y = top + 8;
	char head[128];

	pt.rect(6, top, kWidth - 12, bottom - top - 2, 22);
	pt.frame(6, top, kWidth - 12, bottom - top - 2, 1, 90);

	pt.line(f.mid, pad, y + 20, o.label, 240);
	y += 30;
	std::snprintf(head, sizeof(head), "now: %s      default: %s",
		      o.choices[g_edit[opt]], o.choices[o.def]);
	pt.line(f.small, pad, y + 14, head, 165);
	y += 26;

	/*
	 * Wrap to the panel, honouring blank lines in the text as paragraph
	 * breaks. text_break() gives the number of bytes that fit; a newline
	 * ends the line early so a paragraph never runs into the next.
	 */
	for (const char *s = o.detail; *s && y + lh < bottom - 8;) {
		size_t n;

		if (*s == '\n') {          /* blank line between paragraphs */
			s++;
			if (*s == '\n') {
				s++;
				y += lh / 2;
			}
			continue;
		}
		n = text_break(f.small, s, wrap_w);
		if (!n)
			break;             /* one word wider than the panel */
		/* Never carry a line past a newline in the source. */
		for (size_t k = 0; k < n; k++)
			if (s[k] == '\n') {
				n = k;
				break;
			}
		pt.line(f.small, pad, y + 12, std::string(s, n).c_str(), 200);
		s += n;
		while (*s == ' ')
			s++;
		y += lh;
	}
}

void options_page_draw(Painter &pt, Fonts &f, int top, int bottom,
		       OptPageFocus focus)
{
	std::vector<int> rows = group_rows(g_page_group);
	const bool body = focus == OPT_FOCUS_BODY;
	const int lh = 30;
	int y = top + 4;

	if (g_page_detail && g_page_row < (int)rows.size()) {
		draw_detail(pt, f, top, bottom, rows[g_page_row]);
		return;
	}

	/*
	 * Sub-tab strip. Brighter, and with a lit band behind the whole row,
	 * while the cursor is on it: the highlight below has to be able to
	 * go dim without the page looking like nothing at all is selected.
	 */
	if (focus == OPT_FOCUS_GROUP)
		pt.rect(0, y - 2, kWidth, 26, 34);
	for (int g = 0; g < GRP_COUNT; g++) {
		const bool on = g == g_page_group;
		const int x = 20 + g * 120;

		if (on && focus == OPT_FOCUS_GROUP)
			pt.sel_rect(x - 10, y, 110, 22);
		else if (on)
			pt.rect(x - 10, y, 110, 22, 44);
		pt.line(f.small, x, y + 16, kGroupName[g],
			on ? (focus == OPT_FOCUS_GROUP ? 255 : 205) : 120);
	}
	pt.line(f.small, kWidth - 150, y + 16,
		page_dirty() ? "Unsaved changes" : "", 200);
	y += 30;

	for (size_t r = 0; r < rows.size(); r++) {
		const int i = rows[r];
		/* The cursor stays where it was while it is parked on a strip
		 * above, so you can see where Down will put you back -- but
		 * it is drawn faint, because it is not what the D-pad moves
		 * from up there. */
		const bool sel = (int)r == g_page_row;
		const bool on = sel && body;
		const bool changed = g_edit[i] != kOpts[i].def;
		const char *v = kOpts[i].choices[g_edit[i]];

		if (y + lh > bottom)
			break;
		if (on)
			pt.sel_rect(6, y - 2, kWidth - 12, lh - 2);
		else if (sel)
			pt.rect(6, y - 2, kWidth - 12, lh - 2, 30);
		pt.line(f.mid, 20, y + 20,
			(std::string(kOpts[i].label) +
			 (kOpts[i].live ? "" : " *")).c_str(),
			on ? 245 : 190);
		pt.line(f.mid,
			kWidth - 20 - text_measure(f.mid, v), y + 20, v,
			on	  ? 245
			: changed ? 215
				  : 150);
		y += lh;
	}

	for (int a = 0; a < ACT_COUNT; a++) {
		const bool sel = g_page_row == (int)rows.size() + a;
		const bool on = sel && body;

		if (y + lh > bottom)
			break;
		if (on)
			pt.sel_rect(6, y - 2, kWidth - 12, lh - 2);
		else if (sel)
			pt.rect(6, y - 2, kWidth - 12, lh - 2, 30);
		pt.line(f.mid, 20, y + 20, kActName[a],
			on ? 245 : (a == ACT_SAVE && page_dirty() ? 215 : 150));
		y += lh;
	}

	/* The help line for whatever is lit, plus what the default is when it
	 * is not already on it -- a recommendation is only useful if you can
	 * see how far you have wandered from it. */
	if (g_page_row < (int)rows.size()) {
		const int i = rows[g_page_row];
		std::string help = kOpts[i].help;

		if (g_edit[i] != kOpts[i].def)
			help += std::string("   Default: ") +
				kOpts[i].choices[kOpts[i].def];
		pt.line(f.small, 20, bottom - 22, help.c_str(), 165);
	} else {
		pt.line(f.small, 20, bottom - 22,
			"* takes effect on the next stream", 130);
	}
}


}  // namespace app
