/*
 * Shared declarations for the client's screens and the streaming path.
 *
 * main.cpp used to hold everything; it is split so the library screen, the
 * presenter, the streaming loop and the replay harness can be worked on
 * independently. Everything here lives in namespace app.
 */
#pragma once

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <cmath>
#include <csignal>
#include <deque>
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <time.h>
#include <unistd.h>

extern "C" {
#include "../input/evdev_pad.h"
#include "../media/decoder.h"
#include "../net/wifi_tune.h"
#include "../ui/screen.h"
#include "../ui/text.h"
#include "../video/drm_output.h"
#include "../video/nv12.h"
}

#include "../gnx/auth.hpp"
#include "../gnx/catalog.hpp"
#include "../gnx/http.hpp"
#include "../media/video_pipeline.hpp"
#include "../net/engine.hpp"

namespace app {

constexpr int kWidth = 640;
constexpr int kHeight = 480;

/* Set by the signal handlers in main.cpp; every loop polls it. */
extern volatile sig_atomic_t g_stop;
extern std::atomic<bool> g_abort;

/*
 * The build string: `git describe` at build time, or "dev" outside a
 * checkout. Printed at startup, by -version, and shown in the corner of the
 * library, because "is the handheld actually running the thing I just sent
 * it" is otherwise unanswerable with the device in your hands.
 *
 * A variable defined in main.cpp, deliberately, rather than the
 * XCLOUD_VERSION macro used directly. Only main.cpp is compiled with
 * -DXCLOUD_VERSION and only main.cpp is rebuilt unconditionally; the build's
 * staleness check compares timestamps, not flags, so a second translation
 * unit reading the macro would bake in whatever the version was the last
 * time that particular file happened to change, and then confidently
 * display it. A version string that lies is worse than none.
 */
extern const char *const g_version;
/* __TIME__ from that same always-rebuilt file, "HH:MM:SS". What the library
 * shows for a working build, because every one of those carries the SAME
 * describe string and so cannot answer "is this the binary I just sent". */
extern const char *const g_build_time;

struct Fonts {
	text_ctx *big = nullptr;
	text_ctx *mid = nullptr;
	text_ctx *small = nullptr;
	text_ctx *mono = nullptr;

	bool load()
	{
		big = text_create(TEXT_FONT_DEFAULT, 30);
		mid = text_create(TEXT_FONT_DEFAULT, 21);
		small = text_create(TEXT_FONT_DEFAULT, 16);
		mono = text_create(TEXT_FONT_MONO, 40);
		return big && mid && small && mono;
	}
};

/* Paint into the back buffer and put it on screen. */
class Painter {
public:
	Painter(drm_out &out, Fonts &f) : out_(out), f_(f)
	{
		b_ = drm_out_back_buffer(&out_);
		screen_clear(b_->luma, b_->chroma, b_->pitch, kWidth, kHeight, 16);
	}

	void header(const char *title, const char *right = nullptr)
	{
		screen_rect(b_->luma, b_->pitch, kWidth, kHeight, 0, 0, kWidth,
			    52, 40);
		text_draw(f_.big, b_->luma, b_->pitch, kWidth, kHeight, 18, 37,
			  title, 235);
		if (right) {
			int w = text_measure(f_.small, right);
			text_draw(f_.small, b_->luma, b_->pitch, kWidth, kHeight,
				  kWidth - w - 18, 34, right, 170);
		}
	}

	void centred(text_ctx *font, int y, const char *s, uint8_t level = 200)
	{
		text_draw_centred(font, b_->luma, b_->pitch, kWidth, kHeight, y,
				  s, level);
	}

	void line(text_ctx *font, int x, int y, const char *s, uint8_t level)
	{
		text_draw(font, b_->luma, b_->pitch, kWidth, kHeight, x, y, s,
			  level);
	}

	void rect(int x, int y, int w, int h, uint8_t level)
	{
		screen_rect(b_->luma, b_->pitch, kWidth, kHeight, x, y, w, h,
			    level);
	}

	void footer(const char *s)
	{
		centred(f_.small, kHeight - 18, s, 130);
	}

	/* The rest are what the library screen needs beyond flat luma. */
	void line_fit(text_ctx *font, int x, int y, const char *s, uint8_t level,
		      int max_w)
	{
		text_draw_fit(font, b_->luma, b_->pitch, kWidth, kHeight, x, y, s,
			      level, max_w);
	}

	void disc(int cx, int cy, int r, uint8_t level)
	{
		screen_disc(b_->luma, nullptr, b_->pitch, kWidth, kHeight, cx,
			    cy, r, level, 0, 0);
	}

	/* A disc in the selection green -- a pressed button on the tester's
	 * controller, marked the same way a chosen row is. */
	void sel_disc(int cx, int cy, int r)
	{
		uint8_t Y, cb, cr;

		screen_rgb_to_ycc(34, 132, 78, &Y, &cb, &cr);
		screen_disc(b_->luma, b_->chroma, b_->pitch, kWidth, kHeight,
			    cx, cy, r, Y, cb, cr);
	}

	/* A string slid through a fixed window; see text_draw_window. */
	void line_window(text_ctx *font, int x, int y, const char *s,
			 uint8_t level, int win_x, int win_w)
	{
		text_draw_window(font, b_->luma, b_->pitch, kWidth, kHeight, x,
				 y, s, level, win_x, win_w);
	}

	void frame(int x, int y, int w, int h, int t, uint8_t level)
	{
		screen_frame(b_->luma, b_->pitch, kWidth, kHeight, x, y, w, h, t,
			     level);
	}

	void rect_colour(int x, int y, int w, int h, uint8_t Y, uint8_t cb,
			 uint8_t cr)
	{
		screen_rect_colour(b_->luma, b_->chroma, b_->pitch, kWidth,
				   kHeight, x, y, w, h, Y, cb, cr);
	}

	/*
	 * THE selection bar -- there is one on the screen and it moves.
	 *
	 * A dark green, so it reads as "chosen" rather than as a grey band
	 * whatever it covers is sitting on. Every band that can hold the
	 * cursor paints it with this, tab strip included: the first attempt
	 * gave the strips a grey block of their own and left a dimmed green
	 * bar down in the list, which showed two things at once and so
	 * showed neither. Pressing Up should move a highlight, not trade one
	 * kind of marking for another.
	 */
	void sel_rect(int x, int y, int w, int h)
	{
		uint8_t Y, cb, cr;

		screen_rgb_to_ycc(18, 74, 44, &Y, &cb, &cr);
		rect_colour(x, y, w, h, Y, cb, cr);
	}

	/* An NV12 image with its colour (box art); see screen_blit_nv12. */
	void blit_nv12(int x, int y, const uint8_t *sluma, const uint8_t *schroma,
		       int spitch, int sw, int sh)
	{
		screen_blit_nv12(b_->luma, b_->chroma, b_->pitch, kWidth, kHeight,
				 x, y, sluma, schroma, spitch, sw, sh);
	}

	int present() { return drm_out_present(&out_); }

private:
	drm_out &out_;
	Fonts &f_;
	drm_out_buf *b_;
};

void show_message(drm_out &out, Fonts &f, const char *title, const char *l1,
		  const char *l2 = nullptr);
bool sign_in(drm_out &out, Fonts &f, pad &p, gnx::XboxAuth &auth);

/* ---- presenter (present.cpp) ------------------------------------------- */

gnx::stream::PadFrame pad_to_frame(const pad &p);
gnx::stream::PadFrame autoplay_frame(gnx::stream::PadFrame f);
bool quit_combo(const pad &p);
double now_ms();

struct CpuSample {
	uint64_t main = 0, dec = 0, worker = 0, audio = 0, other = 0, total = 0;
	int temp_mc = 0;  /* SoC temperature, millidegrees; 0 if unreadable */
	long rss_kb = 0;  /* resident set, kB: a 1 GB device with no swap */
};
CpuSample sample_cpu(pid_t main_tid);

bool set_realtime(const char *what, int priority);
void set_normal();

/*
 * Hold the CPU at full clock for the stream. The A55s idle at a low DVFS
 * step and the governor ramps on load; a 12 ms decode that starts at the
 * low step becomes a 20 ms decode. Saves the previous governor and puts it
 * back on exit. Silently does nothing if the sysfs node is missing.
 */
class GovernorGuard {
public:
	GovernorGuard()
	{
		/* The CPU, and the DDR controller: the decoder and the display
		 * both stream through memory, and the dmc's own ondemand
		 * governor was measured idling at 780 MHz mid-stream. */
		pin(0, "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor",
		    "cpufreq");
		pin(1, "/sys/class/devfreq/dmc/governor", "dmc");
	}
	~GovernorGuard()
	{
		for (int i = 0; i < 2; i++)
			if (changed_[i] && write(path_[i], saved_[i].c_str()))
				std::fprintf(stderr, "%s: governor restored to %s\n",
					     name_[i], saved_[i].c_str());
	}

private:
	void pin(int i, const char *path, const char *name)
	{
		path_[i] = path;
		name_[i] = name;
		FILE *fp = std::fopen(path, "r");
		if (!fp)
			return;
		char buf[64] = {0};
		if (std::fgets(buf, sizeof(buf), fp))
			saved_[i] = buf;
		std::fclose(fp);
		while (!saved_[i].empty() &&
		       (saved_[i].back() == '\n' || saved_[i].back() == ' '))
			saved_[i].pop_back();
		if (saved_[i].empty() || saved_[i] == "performance")
			return;
		if (write(path, "performance")) {
			std::fprintf(stderr, "%s: governor %s -> performance\n",
				     name, saved_[i].c_str());
			changed_[i] = true;
		} else {
			std::fprintf(stderr, "%s: could not set performance governor\n",
				     name);
		}
	}
	static bool write(const char *path, const char *v)
	{
		FILE *fp = std::fopen(path, "w");
		if (!fp)
			return false;
		bool ok = std::fputs(v, fp) >= 0;
		std::fclose(fp);
		return ok;
	}
	const char *path_[2] = {nullptr, nullptr};
	const char *name_[2] = {nullptr, nullptr};
	std::string saved_[2];
	bool changed_[2] = {false, false};
};

/*
 * Log lines from the present thread go through here so a slow write to the
 * SD-backed log file (the port launcher redirects stderr to a file) never
 * stalls a real-time thread in front of a vblank.
 */
class AsyncLog {
public:
	AsyncLog() : thread_(&AsyncLog::run, this) {}
	~AsyncLog()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			quit_ = true;
		}
		cv_.notify_all();
		thread_.join();
	}
	void push(std::string line)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			lines_.push_back(std::move(line));
		}
		cv_.notify_one();
	}

private:
	void run()
	{
		pthread_setname_np(pthread_self(), "xc-log");
		std::deque<std::string> batch;
		for (;;) {
			{
				std::unique_lock<std::mutex> lock(mutex_);
				cv_.wait(lock, [&] { return quit_ || !lines_.empty(); });
				batch.swap(lines_);
				if (batch.empty() && quit_)
					return;
			}
			for (const std::string &l : batch)
				std::fputs(l.c_str(), stderr);
			batch.clear();
		}
	}
	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<std::string> lines_;
	bool quit_ = false;
	std::thread thread_;
};

/* Command-line switches that only matter while streaming. */
struct StreamOptions {
	const char *record = nullptr;  /* -record <file>: save the stream */
	const char *replay = nullptr;  /* -replay <file>: play a recording */
	double speed = 1.0;            /* -speed <f>: replay rate multiplier */
	bool autoplay = false;         /* -autoplay: synthetic pad input */
	bool realtime = true;          /* -nort: no SCHED_FIFO */
	bool wifi_tune = true;         /* -nowifi: leave power save/bgscan */
	gnx::stream::PipelineConfig pipeline;
	/* Test-bench options, mostly for the host simulator (scripts/sim.sh). */
	const char *impair = nullptr;    /* -impair "<spec>": network impairment
					    model applied by the replay feeder
					    (see replay.cpp) */
	const char *ui_script = nullptr; /* -ui-script "<keys>": scripted pad
					    presses for unattended UI runs */
	int fake_catalog = 0;            /* -fake-catalog N: N synthetic titles,
					    no account or network needed */
	int fake_consoles = 0;           /* -fake-consoles N: N synthetic
					    remote-play consoles, likewise */
	/* Where the library keeps its caches (catalog.json, names.json,
	 * recent.json, art/): the state directory main.cpp derives, with the
	 * XCLOUD_STATE_DIR override applied. */
	std::string state_dir;
	/* -latch <ms>: late latch. 0 (default) commits right after the flip
	 * event, a whole refresh before the frame is scanned out. N commits
	 * N ms before the predicted next vblank instead, cutting up to ~10 ms
	 * of arrival-to-photon latency at the risk of a repeat if the commit
	 * misses. Watch `late=` and `lat=` in the pace line when tuning.
	 * Atomic for the same reason as `scale`. */
	std::atomic<int> latch_ms{0};
	/* -quit-after <s>: stop the whole run after s seconds, as SIGINT
	 * would, so an unattended simulator run can never linger. */
	int quit_after = 0;
	/*
	 * -scale hw|box|sharp: who halves 1280x720 onto the panel's 640x360
	 * letterbox. hw leaves it to VOP2's Esmart scaler, which discards
	 * alternate columns over most of the picture (see nv12.h); box and
	 * sharp do it here and hand the plane a source it need not scale.
	 * Only applies when halving lands exactly on the fitted rectangle;
	 * any other source size falls back to hw.
	 *
	 * Atomic because the options overlay changes it from the input
	 * thread while the present thread is reading it every frame -- which
	 * is the whole point of being able to change it mid-stream.
	 *
	 * `sharp` is the default since 2026-09-08, decided the only way it
	 * could be: by looking at the panel. It costs 1.2 ms of a 16.58 ms
	 * budget with `late=0` (KNOWN-ISSUES 1), which is a cheap price for
	 * glyph stems that survive the halving instead of vanishing by
	 * parity. `hw` is still there for anyone who wants the millisecond.
	 */
	std::atomic<int> scale{NV12_SCALE_SHARP};
	/*
	 * -res <w>x<h>: the encode size to ask xCloud for, via the
	 * clientdevicecapabilities and dimensionschanged messages, which
	 * carry supportsCustomResolution. 0 keeps the tier's own 1280x720.
	 * Asking for 640x360 spends the whole bitrate on the pixels the
	 * panel actually shows and removes the downscale altogether -- if
	 * the server honours it, which is what this flag is for finding out.
	 */
	int req_w = 0, req_h = 0;
	/* -bitrate <kbps>: override maxBitrateKbps (default 8000 at 720p).
	 * The other lever on how text survives: a starved encoder smears
	 * small glyphs before any scaler sees them. */
	int req_bitrate = 0;
	/*
	 * -layout labels|xbox and -swapxy: which Xbox button each face button
	 * is. Held here rather than only applied at pad_open() so the options
	 * menu can change it, which matters because the mapping is a
	 * judgement made with a thumb (KNOWN-ISSUES 5) and the device has no
	 * shell to re-run the client from.
	 * 0 = the printed labels, 1 = positional (Xbox shape).
	 */
	int pad_layout = 0;
	bool pad_swapxy = false;
	/*
	 * "Picture": letterbox the 16:9 stream into 640x360 with bars, or crop
	 * it to 4:3 and use the whole 640x480 panel. `pan` is -100..100 across
	 * whatever slack the crop leaves. Both are read by the present loop
	 * and pushed to the plane, so both take effect mid-stream.
	 */
	std::atomic<bool> zoom_fill{false};
	std::atomic<int> pan{0};
	/*
	 * The device fingerprint sent at session creation, which is what
	 * selects the encode resolution: P720 android, P1080 windows,
	 * P1080HQ tizen. 720p is the only tier that fits this SoC's decode
	 * budget, but the fingerprint is the one untried lever on the coarse
	 * text problem (KNOWN-ISSUES 1), so the menu can change it.
	 */
	int tier = (int)gnx::QualityTier::P720;
	/* The BCP-47 locale sent as the streamed console's system language. */
	std::string locale = "en-GB";
	/*
	 * Resolution research hooks: -alias, -osname, -display. Each moves one
	 * of the signals -tier otherwise moves together, so what the server
	 * encodes can be attributed to a single cause. Null/zero = the tier's
	 * own value. See docs/RESOLUTION.md.
	 */
	const char *alias = nullptr;
	const char *osname = nullptr;
	int display_w = 0, display_h = 0;
	/* -maxfs / -maxmbps / -level: the offer's declared H264 decode
	 * limits, which are a capability rather than a request. */
	int sdp_max_fs = 0, sdp_max_mbps = 0;
	const char *sdp_level = nullptr;
	/* -plitest <s>: request a keyframe every s seconds and log the
	 * PLI-to-IDR round trip, which is what a lost frame costs. */
	int plitest = 0;
};

/* Implemented in replay.cpp: a thread that sets g_stop after `secs`. */
void arm_quit_after(int secs);
extern StreamOptions g_opts;

/*
 * The present loop's own accounting, per interval: what the present thread
 * did with the frames the pipeline decoded. Printed together with the
 * pipeline's decode stats as the pace| line.
 */
struct PresentStats {
	double t0 = 0;
	uint32_t ticks = 0, shown = 0, held = 0;
	double cvt_sum = 0, cvt_max = 0, commit_max = 0, shadow_max = 0;
	double tick_sum = 0, tick_max = 0;  /* refresh-to-refresh */
	uint32_t tick_late = 0;             /* over 20 ms: a missed refresh */
	double wake_max = 0;                /* flip event -> loop resumed */
	/* Arrival of the unit's last packet -> the vblank that showed it.
	 * The end-to-end number everything else is in service of. */
	double lat_sum = 0, lat_max = 0;
	uint32_t lat_n = 0;
	int64_t pending_pts = -1;           /* pts of the frame committed */
	CpuSample cpu;

	void reset(double now, const CpuSample &c)
	{
		int64_t keep = pending_pts;
		*this = PresentStats();
		t0 = now;
		cpu = c;
		pending_pts = keep;
	}
};

std::string report_pace(const PresentStats &ps,
			const gnx::stream::VideoPipeline::Stats &d, double now,
			const CpuSample &c, size_t au_queue, uint32_t stale);
void note_flip(PresentStats &ps, uint64_t flip_us, uint64_t &prev_flip_us);
int prepare_frame(drm_out &out, const AVFrame *frame);
/* `f` is only for the options overlay, drawn over the frame before it is
 * committed; nothing else in the present path needs a font. */
bool present_tick(drm_out &out, Fonts &f, gnx::stream::VideoPipeline &pipe,
		  PresentStats &ps, bool *committed);
void wait_refresh(drm_out &out, bool committed, double last_tick);
/* Late latch (g_opts.latch_ms): sleep from the flip event until latch_ms
 * before the predicted next vblank, so the commit that follows carries the
 * freshest frame. No-op when latch_ms is 0. */
void late_latch_wait(const drm_out &out);

/* ---- screens and loops ------------------------------------------------- */

/*
 * What a streaming session is against: an xCloud title, or your own Xbox over
 * remote play. `id` is the title id or the console's serverId, per `target`;
 * `name` is only what the connecting screen shows.
 */
struct StreamRequest {
	gnx::stream::StreamTarget target = gnx::stream::StreamTarget::Cloud;
	std::string id;
	std::string name;
};

void stream_session(drm_out &out, Fonts &f, pad &p, gnx::XboxAuth &auth,
		    const StreamRequest &request);

/*
 * The size the server last actually encoded at, published by the presenter
 * (present.cpp) when it changes and read at teardown by stream_session().
 *
 * It is how we learn which titles honour a custom resolution: 720p is the
 * platform floor for everything that ignores XGameStreamingSetResolution, so
 * anything smaller means the game itself acted on the display details we sent
 * (docs/RESOLUTION.md). Zero until a frame has been presented.
 */
extern std::atomic<int> g_stream_w, g_stream_h;
/* One xCloud title: what the library screen launches. */
void stream_game(drm_out &out, Fonts &f, pad &p, gnx::XboxAuth &auth,
		 const gnx::Game &game);
void replay_stream(drm_out &out, Fonts &f, pad &p, const char *path);

/*
 * The library screen (library.cpp). `games` is the playable list, in any
 * order; the screen sorts it by name, resolves names and box art in the
 * background and keeps its caches under g_opts.state_dir. With `refresh`
 * the list came from catalog.json and a background fetch replaces it once
 * the real one arrives.
 */
void library(drm_out &out, Fonts &f, pad &p, gnx::Http &http,
	     gnx::XboxAuth &auth, std::vector<gnx::Game> &games,
	     const gnx::XboxProfile &profile, bool refresh);

/* The playable list from <state dir>/catalog.json; false if there is none. */
bool load_catalog_cache(std::vector<gnx::Game> &games);

/*
 * Fetch the catalog for the signed-in account (both offerings, merged),
 * keep the playable titles and write catalog.json. Throws as fetch_catalog
 * does.
 */
void fetch_playable(gnx::Http &http, gnx::XboxAuth &auth,
		    std::vector<gnx::Game> &playable);

/* -fake-catalog N: the library over synthetic titles (test bench). */
void fake_library(drm_out &out, Fonts &f, pad &p, int count);

const char *arg_value(int argc, char **argv, const char *key);

}  // namespace app
