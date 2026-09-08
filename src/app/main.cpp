/*
 * xcloud - entry point: argument parsing, setup, and the top-level state
 * machine that moves between sign-in, the library and a live stream.
 *
 * Auth, catalog and HTTP come from green-nx's platform-independent core
 * (GPL-3.0, src/gnx/). Display, text and input are ours: direct libdrm on a
 * VOP2 overlay plane, FreeType, and raw evdev. See docs/ARCHITECTURE.md.
 *
 * Launched from EmulationStation as a port, which passes
 *   -p1devicepath /dev/input/eventN
 * among other arguments.
 */
#include "app.hpp"

#include "options.hpp"

namespace app {

constexpr const char *kStateDir = "/userdata/ports/xcloud";
constexpr const char *kTokenStore = "/userdata/ports/xcloud/tokens.json";

struct drm_out g_out;
volatile sig_atomic_t g_stop;
std::atomic<bool> g_abort{false};

void on_signal(int)
{
	g_stop = 1;
	g_abort = true;
}

/*
 * There is no debugger on the device and no core dump to bring home, so a
 * crash otherwise leaves nothing but "Segmentation fault" and a log that
 * stops mid-line. backtrace_symbols_fd is async-signal-safe (it does not
 * allocate) and glibc is present, so this turns a silent death into
 * addresses we can feed to addr2line. Re-raises so the exit status still
 * reports the signal.
 */
void on_fatal(int sig)
{
	void *frames[32];
	int n;

	/* First, before anything that could fault again: a stream raises the
	 * Wi-Fi driver's scan_deny, and a crash must not leave it raised. */
	wifi_tune_panic();
	n = backtrace(frames, 32);

	static const char msg[] = "\n*** fatal signal, backtrace: ***\n";
	ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
	(void)ignored;
	backtrace_symbols_fd(frames, n, STDERR_FILENO);

	std::signal(sig, SIG_DFL);
	std::raise(sig);
}

const char *arg_value(int argc, char **argv, const char *key)
{
	for (int i = 1; i + 1 < argc; i++)
		if (!std::strcmp(argv[i], key))
			return argv[i + 1];
	return nullptr;
}

/*
 * Set by the build from `git describe`, or left as "dev" for a build made
 * outside a checkout. It is printed at startup and by -version, because the
 * alternative -- working out which of several pushes is actually on a
 * handheld -- is guesswork, and the log a user attaches to a bug report is
 * worth nothing without it.
 *
 * Only this file is compiled with -DXCLOUD_VERSION, and only this file is
 * rebuilt every time; g_version is how the rest of the client reads it
 * without risking a stale copy. See the note in app.hpp.
 */
#ifndef XCLOUD_VERSION
#define XCLOUD_VERSION "dev"
#endif

const char *const g_version = XCLOUD_VERSION;
const char *const g_build_time = __TIME__;   /* "HH:MM:SS" */

void print_version()
{
	std::printf("xcloud %s (built %s)\n", XCLOUD_VERSION, __DATE__);
}

/*
 * -selftest: check the things that depend on where the client is actually
 * running, and exit. No display, no network, no account.
 *
 * The point is that some behaviour cannot be proved on a build host. Whether
 * the token store survives a save is a property of the filesystem underneath
 * it, and the handheld's /userdata is ext4 while its ROM card is exFAT with
 * no permission bits at all -- so the only honest place to check is the
 * device. Cheap enough to run on every install.
 */
int run_selftest(const std::string &state_dir)
{
	int failures = 0;
	std::printf("xcloud %s selftest\n", XCLOUD_VERSION);
	std::printf("  state dir: %s\n", state_dir.c_str());

	std::string detail;
	const bool ok = gnx::XboxAuth::selftest_store(state_dir, &detail);
	std::printf("  %s token store: %s\n", ok ? "PASS" : "FAIL", detail.c_str());
	if (!ok)
		failures++;

	/* The state directory has to be writable or nothing persists: not the
	 * sign-in, not the settings, not the catalog cache. */
	const std::string probe = state_dir + "/.selftest-write";
	FILE *f = std::fopen(probe.c_str(), "w");
	if (f) {
		std::fclose(f);
		std::remove(probe.c_str());
		std::printf("  PASS state dir writable\n");
	} else {
		std::printf("  FAIL state dir not writable: %s\n",
			    std::strerror(errno));
		failures++;
	}

	std::printf("%s\n", failures ? "selftest: FAILED" : "selftest: OK");
	return failures ? 1 : 0;
}

}  // namespace app

using namespace app;

int main(int argc, char **argv)
{
	Fonts fonts;
	pad p;
	int rc = 1;

	/* Before anything else opens a device or a file: -version has to work
	 * on a machine that is not the handheld, and has to work when the
	 * client is otherwise too broken to start. */
	for (int i = 1; i < argc; i++)
		if (!std::strcmp(argv[i], "-version") ||
		    !std::strcmp(argv[i], "--version")) {
			print_version();
			return 0;
		}

	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
	std::signal(SIGHUP, on_signal);
	std::signal(SIGSEGV, on_fatal);
	std::signal(SIGBUS, on_fatal);
	std::signal(SIGABRT, on_fatal);

	/* ---- host simulator hook (scripts/sim.sh) --------------------------
	 * XCLOUD_STATE_DIR moves the state directory and token store off
	 * /userdata, which does not exist on the host. Unset on the device,
	 * where the two constants above apply unchanged. */
	const char *state_env = std::getenv("XCLOUD_STATE_DIR");
	const std::string state_dir = state_env && *state_env ? state_env
								: kStateDir;
	const std::string token_store = state_env && *state_env
						? state_dir + "/tokens.json"
						: kTokenStore;
	mkdir(state_dir.c_str(), 0755);
	g_opts.state_dir = state_dir;   /* the library's caches live there */
	/* ---- end host simulator hook ------------------------------------- */

	/* After the state directory is known, before anything opens the screen
	 * or the network: the selftest needs a real path and nothing else. */
	for (int i = 1; i < argc; i++)
		if (!std::strcmp(argv[i], "-selftest"))
			return run_selftest(state_dir);

	/* First line of every log, so a bug report identifies its build. */
	std::fprintf(stderr, "xcloud %s (built %s)\n", XCLOUD_VERSION, __DATE__);

	/* Saved options first, so an explicit flag below always wins over
	 * whatever the menu was last left on. */
	options_load();

	/* EmulationStation passes -p1devicepath, NOT -p1path. */
	const char *pad_path = arg_value(argc, argv, "-p1devicepath");

	/*
	 * -title <titleId> streams one game straight away, skipping the
	 * library. For bring-up over SSH: the whole transport can then be
	 * exercised without anyone holding the handheld to press A.
	 */
	const char *direct_title = arg_value(argc, argv, "-title");

	/*
	 * -console <serverId> does the same for your own Xbox over remote
	 * play (xHome) rather than the cloud. Remote play streams the
	 * console's dashboard, not a title, so there is nothing else to pick.
	 */
	const char *direct_console = arg_value(argc, argv, "-console");

	/* -list dumps every playable launch id to the log and exits;
	 * -consoles does the same for the account's consoles, which is where
	 * a -console serverId comes from. */
	bool list_titles = false;
	bool list_consoles = false;
	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "-list"))
			list_titles = true;
		if (!std::strcmp(argv[i], "-consoles"))
			list_consoles = true;
		/* -autoplay wiggles the sticks and taps A on a timer, so an
		 * unattended run over SSH still has motion to encode. */
		if (!std::strcmp(argv[i], "-autoplay"))
			g_opts.autoplay = true;
	}
	/* -record <file> saves every access unit and audio packet with its
	 * arrival time, for offline replay (see au_recorder.hpp); -replay
	 * <file> plays one back through the same pipeline, -speed scales its
	 * clock. */
	g_opts.record = arg_value(argc, argv, "-record");
	g_opts.replay = arg_value(argc, argv, "-replay");
	if (const char *v = arg_value(argc, argv, "-speed"))
		g_opts.speed = std::atof(v) > 0 ? std::atof(v) : 1.0;
	/* Pipeline knobs, for A/B runs on the device: -threads slice|frame2|
	 * frame3, -reserve N, -dropat N, -nort. */
	if (const char *v = arg_value(argc, argv, "-threads")) {
		if (!std::strcmp(v, "slice"))
			g_opts.pipeline.threading = DECODER_THREADS_SLICE;
		else if (!std::strcmp(v, "frame3"))
			g_opts.pipeline.threading = DECODER_THREADS_FRAME3;
		else
			g_opts.pipeline.threading = DECODER_THREADS_FRAME2;
	}
	if (const char *v = arg_value(argc, argv, "-reserve"))
		g_opts.pipeline.reserve = std::max(0, std::atoi(v));
	/*
	 * -skiploop none|nonref|nonkey|all: how much H.264 deblocking to skip.
	 * Decode is the scarcest thing on this SoC (10.7 ms of a 16.58 ms
	 * budget at 720p) and the panel halves the picture anyway, so some of
	 * what the filter reconstructs is discarded before it is seen. Costs
	 * conformance: later frames predict from the filtered picture, so
	 * error accumulates until the next IDR. docs/PERFORMANCE.md.
	 */
	if (const char *v = arg_value(argc, argv, "-skiploop")) {
		if (!std::strcmp(v, "nonref"))
			g_opts.pipeline.skip_loop = DECODER_LOOP_SKIP_NONREF;
		else if (!std::strcmp(v, "nonkey"))
			g_opts.pipeline.skip_loop = DECODER_LOOP_SKIP_NONKEY;
		else if (!std::strcmp(v, "all"))
			g_opts.pipeline.skip_loop = DECODER_LOOP_SKIP_ALL;
		else if (!std::strcmp(v, "none"))
			g_opts.pipeline.skip_loop = DECODER_LOOP_ALL_FRAMES;
		else
			std::fprintf(stderr, "xcloud: -skiploop wants none, "
					     "nonref, nonkey or all; keeping "
					     "none\n");
	}
	/* -lowres 1|2: decode at half or quarter size, if this build's h264
	 * decoder supports it at all. It reports its max_lowres at open. */
	if (const char *v = arg_value(argc, argv, "-lowres"))
		g_opts.pipeline.lowres = std::max(0, std::min(2, std::atoi(v)));
	if (const char *v = arg_value(argc, argv, "-dropat"))
		g_opts.pipeline.drop_at = std::max(2, std::atoi(v));
	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "-nort"))
			g_opts.realtime = false;
		if (!std::strcmp(argv[i], "-nowifi"))
			g_opts.wifi_tune = false;
		/* -layout labels|xbox and -swapxy: which Xbox button each
		 * face button is. Parsed here, with the other options rather
		 * than at pad_open below, so options_capture() sees them and
		 * the menu opens showing what is really in force. */
		if (!std::strcmp(argv[i], "-swapxy"))
			g_opts.pad_swapxy = true;
		/* Both directions, because the saved value persists now:
		 * without this there is no way to clear it from the command
		 * line once the menu has set it. */
		if (!std::strcmp(argv[i], "-noswapxy"))
			g_opts.pad_swapxy = false;
	}
	if (const char *v = arg_value(argc, argv, "-layout"))
		g_opts.pad_layout = !std::strcmp(v, "xbox") ||
					    !std::strcmp(v, "positional")
					    ? 1
					    : 0;
	/* -tier 720|1080|1080hq: the device fingerprint, which is what picks
	 * the encode resolution. See KNOWN-ISSUES 1 -- this is the lever, and
	 * having it on the command line is how it gets A/B'd over SSH. */
	if (const char *v = arg_value(argc, argv, "-tier")) {
		if (!std::strcmp(v, "1080"))
			g_opts.tier = (int)gnx::QualityTier::P1080;
		else if (!std::strcmp(v, "1080hq"))
			g_opts.tier = (int)gnx::QualityTier::P1080HQ;
		else if (!std::strcmp(v, "720hq"))
			g_opts.tier = (int)gnx::QualityTier::P720HQ;
		else if (!std::strcmp(v, "720"))
			g_opts.tier = (int)gnx::QualityTier::P720;
		else
			std::fprintf(stderr, "xcloud: -tier wants 720, 720hq, "
					     "1080 or 1080hq; keeping 720\n");
	}
	/* -locale <BCP-47>: the streamed console's system language. */
	if (const char *v = arg_value(argc, argv, "-locale"))
		g_opts.locale = v;
	/*
	 * Resolution research hooks (docs/RESOLUTION.md). -tier moves five
	 * signals at once, so a change in what the server encodes cannot be
	 * pinned on any one of them. These move three of them independently:
	 *
	 *   -alias <s>     the control channel's resolutionAlias. The known
	 *                  enum is Auto/720/720HQ/1080/1080HQ/1440; passing
	 *                  anything else is how we find out whether the
	 *                  server validates it.
	 *   -osname <s>    dev.os.name in the session's X-MS-Device-Info
	 *                  (android / windows / tizen).
	 *   -display WxH   dev.displayInfo.dimensions in the same header.
	 */
	g_opts.alias = arg_value(argc, argv, "-alias");
	g_opts.osname = arg_value(argc, argv, "-osname");
	/*
	 * -maxfs / -maxmbps / -level: the offer's declared H264 decode
	 * limits. Not a request but a capability statement, which a
	 * conforming encoder may not exceed -- max-fs is a frame size in
	 * macroblocks, so 920 (640x360) cannot be met by a 1280x720 frame
	 * (3600). The strongest lever available to a client.
	 */
	if (const char *v = arg_value(argc, argv, "-maxfs"))
		g_opts.sdp_max_fs = std::atoi(v);
	if (const char *v = arg_value(argc, argv, "-maxmbps"))
		g_opts.sdp_max_mbps = std::atoi(v);
	g_opts.sdp_level = arg_value(argc, argv, "-level");
	if (const char *v = arg_value(argc, argv, "-plitest"))
		g_opts.plitest = std::max(0, std::atoi(v));
	if (const char *v = arg_value(argc, argv, "-display")) {
		int w = 0, h = 0;

		if (std::sscanf(v, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
			g_opts.display_w = w;
			g_opts.display_h = h;
		} else {
			std::fprintf(stderr, "xcloud: -display wants <w>x<h>; "
					     "ignored\n");
		}
	}
	/* Test-bench options: a network impairment model for -replay, scripted
	 * pad presses, and a synthetic catalog. See app.hpp. */
	g_opts.impair = arg_value(argc, argv, "-impair");
	g_opts.ui_script = arg_value(argc, argv, "-ui-script");
	if (const char *v = arg_value(argc, argv, "-fake-catalog"))
		g_opts.fake_catalog = std::max(0, std::atoi(v));
	if (const char *v = arg_value(argc, argv, "-fake-consoles"))
		g_opts.fake_consoles = std::max(0, std::atoi(v));
	if (const char *v = arg_value(argc, argv, "-latch"))
		g_opts.latch_ms = std::max(0, std::min(12, std::atoi(v)));
	/*
	 * Text legibility knobs. -scale picks who halves 720p onto the
	 * panel; -res and -bitrate change what we ask xCloud to send in the
	 * first place. See app.hpp and docs/VIDEO-PACING.md.
	 */
	if (const char *v = arg_value(argc, argv, "-scale")) {
		if (!std::strcmp(v, "box"))
			g_opts.scale = (int)NV12_SCALE_BOX;
		else if (!std::strcmp(v, "sharp"))
			g_opts.scale = (int)NV12_SCALE_SHARP;
		else if (!std::strcmp(v, "hw"))
			g_opts.scale = (int)NV12_SCALE_HW;
		else
			std::fprintf(stderr, "xcloud: -scale wants hw, box or "
					     "sharp; keeping hw\n");
	}
	std::fprintf(stderr, "xcloud: downscale = %s\n",
		     g_opts.scale == NV12_SCALE_BOX	 ? "box (CPU)"
		     : g_opts.scale == NV12_SCALE_SHARP	 ? "sharp (CPU)"
							 : "hw (VOP2)");
	if (const char *v = arg_value(argc, argv, "-res")) {
		int w = 0, h = 0;

		if (std::sscanf(v, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
			g_opts.req_w = w;
			g_opts.req_h = h;
		} else {
			std::fprintf(stderr, "xcloud: -res wants <w>x<h>, e.g. "
					     "640x360; ignored\n");
		}
	}
	if (const char *v = arg_value(argc, argv, "-bitrate"))
		g_opts.req_bitrate = std::max(0, std::atoi(v));
	/* The menu now shows what is actually in force, flags included. */
	options_capture();

	/* ---- -quit-after <s> (test bench: scripts/test.sh) -----------------
	 * A bound on the whole run: after s seconds g_stop is set exactly as
	 * SIGINT would set it, so every loop -- replay, library, sign-in --
	 * leaves by its own exit and a container never lingers. */
	if (const char *v = arg_value(argc, argv, "-quit-after")) {
		g_opts.quit_after = std::max(0, std::atoi(v));
		arm_quit_after(g_opts.quit_after);
	}
	/* ---- end -quit-after ----------------------------------------------- */

	if (drm_out_open(&g_out, "/dev/dri/card0"))
		return 1;
	if (drm_out_configure(&g_out, kWidth, kHeight))
		goto out;
	if (!fonts.load()) {
		std::fprintf(stderr, "xcloud: could not load fonts\n");
		goto out;
	}
	if (pad_open(&p, pad_path))
		std::fprintf(stderr, "xcloud: no pad; keyboard-less UI will "
				     "not respond\n");
	/* Which Xbox button each face button is: from the saved options, with
	 * -layout/-swapxy already folded in above. The default follows the
	 * letters printed on the shell (see evdev_pad.h); the options menu
	 * changes it live, because deciding it needs a thumb. */
	pad_set_layout(&p,
		       g_opts.pad_layout ? PAD_LAYOUT_POSITIONAL
					 : PAD_LAYOUT_LABELS,
		       g_opts.pad_swapxy ? 1 : 0);

	/* ---- test bench: -ui-script and -fake-catalog ----------------------
	 * Scripted presses drive the screens with no one holding the handheld
	 * (the simulator has no pad), and a synthetic catalog puts the
	 * library up with no account and no network. Neither runs unless
	 * asked for, so the device path below is unchanged. */
	if (g_opts.ui_script && pad_set_script(&p, g_opts.ui_script))
		std::fprintf(stderr, "xcloud: -ui-script ignored\n");
	if (g_opts.fake_catalog) {
		fake_library(g_out, fonts, p, g_opts.fake_catalog);
		goto done;
	}
	/* ---- end test bench ----------------------------------------------- */

	/* Offline replay needs no account at all. */
	if (g_opts.replay) {
		replay_stream(g_out, fonts, p, g_opts.replay);
		goto done;
	}

	try {
		gnx::XboxAuth auth(token_store);  /* see the simulator hook */
		gnx::Http http;
		gnx::XboxProfile profile;

		auth.set_abort_flag(&g_abort);
		http.set_abort_flag(&g_abort);

		if (!auth.has_saved_login() && !sign_in(g_out, fonts, p, auth))
			goto done;

		if (direct_title) {
			gnx::Game game;
			game.title_id = direct_title;
			stream_game(g_out, fonts, p, auth, game);
			goto done;
		}

		/*
		 * Remote play needs neither the catalog nor the library: the
		 * console streams its dashboard and you navigate it with the
		 * pad. Both of these therefore run before any of that.
		 */
		if (list_consoles) {
			gnx::StreamingCredentials creds =
				auth.fetch_streaming_credentials();
			if (creds.home.host.empty()) {
				std::fprintf(stderr,
					     "xcloud: no remote play on this "
					     "account: %s\n",
					     creds.home_error.empty()
						     ? "no console linked"
						     : creds.home_error.c_str());
			} else {
				std::vector<gnx::HomeConsole> consoles =
					gnx::fetch_home_consoles(http,
								 creds.home);

				std::fprintf(stderr,
					     "xcloud: xhome host %s, %zu "
					     "console(s)\n",
					     creds.home.host.c_str(),
					     consoles.size());
				for (const gnx::HomeConsole &c : consoles)
					std::fprintf(stderr,
						     "console: %s  %s (%s, %s)\n",
						     c.server_id.c_str(),
						     c.name.c_str(),
						     c.console_type.c_str(),
						     c.power_state.c_str());
				if (consoles.empty())
					std::fprintf(stderr,
						     "xcloud: the account can "
						     "stream, but no console is "
						     "registered for remote "
						     "play\n");
			}
			goto done;
		}

		if (direct_console) {
			StreamRequest request;

			request.target = gnx::stream::StreamTarget::Console;
			request.id = direct_console;
			request.name = "Xbox";
			stream_session(g_out, fonts, p, auth, request);
			goto done;
		}

		show_message(g_out, fonts, "Signing in...", "Fetching your profile");
		try {
			profile = auth.fetch_profile();
		} catch (const std::exception &e) {
			std::fprintf(stderr, "xcloud: profile: %s\n", e.what());
		}

		{
			/* The playable list from the last run puts the
			 * library up at once; the screen then refreshes it
			 * in the background. Only a first run, or a lost
			 * cache, waits on the network here. */
			std::vector<gnx::Game> playable;
			bool cached = load_catalog_cache(playable);
			if (!cached) {
				show_message(g_out, fonts,
					     "Loading your library...",
					     "This takes a few seconds");
				fetch_playable(http, auth, playable);
			}

			/* -list dumps launch ids to the log and exits. The ids
			 * are what -title takes, and there is no other way to
			 * read one off a device with no shell on the panel. */
			if (list_titles) {
				for (const gnx::Game &g : playable)
					std::fprintf(stderr, "title: %s\n",
						     g.title_id.c_str());
				goto done;
			}

			if (playable.empty()) {
				show_message(g_out, fonts, "Nothing playable",
					     "This account has no Game Pass titles",
					     "B to quit");
				while (!g_stop && !pad_take_press(&p, PAD_B))
					pad_poll(&p, 200);
				goto done;
			}
			library(g_out, fonts, p, http, auth, playable,
				profile, cached);
		}
	} catch (const std::exception &e) {
		std::fprintf(stderr, "xcloud: %s\n", e.what());
		show_message(g_out, fonts, "Something went wrong", e.what(),
			     "B to quit");
		while (!g_stop && !pad_take_press(&p, PAD_B))
			pad_poll(&p, 200);
	}

done:
	rc = 0;
out:
	/* usrsctp starts two service threads that only stop inside
	 * usrsctp_finish(); leaving them running past main() is a crash
	 * waiting to happen. Safe when no Engine was ever created. */
	gnx::stream::Engine::global_shutdown();
	pad_close(&p);
	drm_out_close(&g_out);
	return rc;
}
