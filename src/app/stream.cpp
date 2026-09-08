/*
 * One live session, from engine start to teardown.
 *
 * Split in two at the backend boundary. run_session() is the session itself
 * -- pipeline, input thread, present pacing, status and error screens -- and
 * speaks only to IStreamEngine, so a second backend reuses it whole rather
 * than growing a second copy that then has to be kept in step with this one.
 * stream_session() is the xCloud half above it: it builds the Engine, applies
 * the options that only mean anything to GSSV, starts it, and afterwards
 * records what the session taught us about the title.
 *
 * The target is either an xCloud title or your own Xbox over remote play;
 * only session setup differs, and that difference lives in the engine.
 */
#include "app.hpp"

#include "library_data.hpp"
#include "options.hpp"

namespace app {

/* Both appear all through the loop below, and the qualified names push the
 * lines they sit on well past the margin at four tabs of indent. */
using gnx::stream::EngineState;
using gnx::stream::IStreamEngine;

/*
 * Drive one already-started session to its end. The engine reassembles
 * access units on its worker; the pipeline decodes them on its own thread;
 * this thread presents one frame per refresh and handles the pad in the
 * shadow of the flip.
 *
 * Returns with the engine stopped and the plane back on the UI source.
 * `title` is only what the connecting screen shows.
 */
static void run_session(drm_out &out, Fonts &f, pad &p,
			IStreamEngine &engine,
			const std::string &title)
{
	gnx::stream::VideoPipeline pipe;
	AsyncLog alog;
	GovernorGuard governor;
	bool video_mode = false;   /* plane is showing 720p video, not the UI */
	std::string shown_status;
	int status_ticks = 0;
	const pid_t main_tid = (pid_t)syscall(SYS_gettid);
	PresentStats ps;
	double last_tick = 0;
	uint64_t prev_flip_us = 0;
	bool committed = false;
	bool rt_set = false;
	/* Previous interval's totals, for the net| bitrate line below. */
	uint64_t prev_bytes = 0;
	uint32_t prev_packets = 0;
	double last_plitest = 0;

	engine.set_video_wakeup([&pipe] { pipe.notify(); });

	/*
	 * Input on its own thread once video is up. Polling the pad from the
	 * present loop sampled it once per refresh, so a press could wait up
	 * to 16.7 ms just to be noticed, before the 8 ms send cadence. This
	 * thread blocks in poll() on the pad's fd with an 8 ms timeout, so a
	 * press is picked up the moment the kernel reports it and published
	 * to the engine at 125 Hz at the latest. The pad struct is not
	 * thread-safe: from the moment this starts until it is joined, the
	 * present loop only reads the quit flag it publishes.
	 */
	std::atomic<bool> input_stop{false};
	std::atomic<bool> input_quit{false};
	std::thread input_thread;
	auto start_input = [&]() {
		input_thread = std::thread([&]() {
			pthread_setname_np(pthread_self(), "xc-input");
			while (!input_stop) {
				pad_poll(&p, 8);
				if (quit_combo(p))
					input_quit = true;
				/*
				 * The options overlay lives on this thread
				 * because this is the thread that owns the
				 * pad. While it is open the game gets a
				 * neutral frame, or choosing a menu entry
				 * would also steer the car.
				 */
				options_menu_input(p, pad_now_ms());
				if (options_menu_open()) {
					engine.set_pad(
						gnx::stream::PadFrame());
					continue;
				}
				gnx::stream::PadFrame frame = pad_to_frame(p);
				if (g_opts.autoplay)
					frame = autoplay_frame(frame);
				engine.set_pad(frame);
			}
		});
	};
	auto stop_input = [&]() {
		input_stop = true;
		if (input_thread.joinable())
			input_thread.join();
	};
	pipe.start(g_opts.pipeline,
		   [&engine](std::vector<uint8_t> &au, uint64_t *arrival) {
			   return engine.take_access_unit(au, arrival);
		   });
	ps.reset(now_ms(), sample_cpu(main_tid));

	auto draw_status = [&](const char *line2) {
		Painter pt(out, f);
		pt.header("Starting", title.c_str());
		pt.centred(f.mid, 210, shown_status.empty()
					       ? "Connecting..."
					       : shown_status.c_str(), 235);
		if (line2)
			pt.centred(f.small, 250, line2, 170);
		pt.footer("B to cancel");
		pt.present();
	};

	while (!g_stop) {
		EngineState state = engine.state();

		if (state == EngineState::Failed) {
			std::string err = engine.error();
			/* The pad goes back to this thread for the error
			 * screen; the input thread must be gone first. */
			stop_input();
			pipe.stop();
			engine.stop();
			if (video_mode)
				drm_out_set_source(&out, kWidth, kHeight);
			show_message(out, f, "Stream failed", err.c_str(),
				     "B to go back");
			while (!g_stop && !pad_take_press(&p, PAD_B))
				pad_poll(&p, 200);
			break;
		}
		if (state == EngineState::Stopped)
			break;

		if (!video_mode) {
			pad_poll(&p, 16);
			if (quit_combo(p) || pad_take_press(&p, PAD_B))
				break;
			engine.set_pad(pad_to_frame(p));
			if (pipe.failed()) {
				pipe.stop();
				engine.stop();
				show_message(out, f, "Stream failed",
					     "The video decoder could not start",
					     "B to go back");
				while (!g_stop && !pad_take_press(&p, PAD_B))
					pad_poll(&p, 200);
				break;
			}
			if (pipe.have_frame()) {
				/* First frame decoded: switch the plane to
				 * video and start pacing off the flip. The
				 * status screen's flip is complete (Painter
				 * blocks), so nothing is pending. */
				video_mode = true;
				start_input();
				if (g_opts.realtime && !rt_set)
					rt_set = set_realtime("present", 12);
				last_tick = now_ms();
				ps.reset(last_tick, sample_cpu(main_tid));
				continue;
			}
			/* Still connecting: repaint only when the status text
			 * changes, plus a slow tick so a stalled stage still
			 * looks alive. */
			std::string status = engine.status();
			if (status != shown_status || ++status_ticks > 60) {
				shown_status = status;
				status_ticks = 0;
				IStreamEngine::Counters c = engine.counters();
				char detail[128];
				std::snprintf(detail, sizeof(detail),
					      "channels %s  handshake %s  video %u pkt",
					      c.channels_open ? "up" : "-",
					      c.handshake_done ? "ok" : "-",
					      c.video_packets);
				draw_status(state == EngineState::Negotiating ||
						    state == EngineState::WaitingForVideo
						    ? detail
						    : nullptr);
			}
			continue;
		}

		/* ---- one refresh ---- */
		last_tick = now_ms();
		note_flip(ps, out.last_flip_us, prev_flip_us);

		late_latch_wait(out);
		if (!present_tick(out, f, pipe, ps, &committed))
			break;
		if (committed)
			engine.note_decoded_frame();

		/* Shadow work, kept short: the driver programs the plane from
		 * a worker on this CPU, so a real-time thread that lingers
		 * here delays its own flip. The pad lives on its own thread
		 * now; only its quit flag is read here. */
		double t_shadow = now_ms();
		if (input_quit)
			break;
		/*
		 * -plitest <s>: ask for a keyframe on a timer and let the
		 * engine log "pli->idr N ms". Every unrecoverable loss
		 * freezes the picture until an IDR arrives, so this round
		 * trip is the cost of a loss, and it had never been
		 * measured. docs/PERFORMANCE.md.
		 */
		if (g_opts.plitest > 0 &&
		    last_tick - last_plitest > g_opts.plitest * 1000.0) {
			last_plitest = last_tick;
			engine.request_keyframe();
		}
		double now = now_ms();
		if (now - t_shadow > ps.shadow_max)
			ps.shadow_max = now - t_shadow;
		if (now - ps.t0 >= 1000.0) {
			CpuSample c = sample_cpu(main_tid);
			/*
			 * Delivered video bitrate, which the pace line has no
			 * other way of showing. It is what tells two streams
			 * of the SAME resolution apart -- the 720 and 720HQ
			 * resolution aliases differ only in this -- and it is
			 * the number the encoder spends on glyph detail. See
			 * docs/RESOLUTION.md.
			 */
			IStreamEngine::Counters ct = engine.counters();
			double secs = (now - ps.t0) / 1000.0;
			char netline[128];
			std::snprintf(netline, sizeof(netline),
				      "[%8llu] net| video %.0f kbps (%u pkt/s)",
				      (unsigned long long)now,
				      secs > 0 ? (ct.video_bytes - prev_bytes) *
							 8.0 / 1000.0 / secs
					       : 0.0,
				      ct.video_packets - prev_packets);
			prev_bytes = ct.video_bytes;
			prev_packets = ct.video_packets;
			alog.push(netline);
			alog.push(report_pace(ps, pipe.snapshot(), now, c,
					      engine.queued_video(),
					      out.stale_events));
			ps.reset(now, c);
		}

		wait_refresh(out, committed, last_tick);
		/* Flip IRQ time -> loop resumed: the scheduling latency the
		 * real-time priority is there to bound. */
		if (out.last_flip_us) {
			double wake = now_ms() - out.last_flip_us / 1000.0;
			if (wake > ps.wake_max && wake < 100.0)
				ps.wake_max = wake;
		}
	}

	if (rt_set)
		set_normal();
	stop_input();
	pipe.stop();
	engine.stop();
	if (video_mode)
		drm_out_set_source(&out, kWidth, kHeight);
}

/*
 * One xCloud or remote-play session: build the engine, apply the options
 * that only GSSV understands, hand it to the loop above.
 */
void stream_session(drm_out &out, Fonts &f, pad &p, gnx::XboxAuth &auth,
		    const StreamRequest &request)
{
	gnx::stream::Engine engine(auth);
	gnx::stream::AuRecorder recorder;
	struct wifi_tune wifi;
	bool wifi_applied = false;

	const std::string &title =
		request.name.empty() ? request.id : request.name;

	if (g_opts.record && recorder.open(g_opts.record))
		engine.set_recorder(&recorder);
	if (g_opts.wifi_tune)
		wifi_applied = wifi_tune_apply(&wifi, nullptr) > 0;

	/* 720p is the only tier this device can actually keep up with: the
	 * panel is 640x480 and software decode of 720p60 is already about a
	 * third of the SoC. It stays the default for that reason, but the
	 * tier IS the device fingerprint, which is the one untried lever on
	 * the coarse-text problem (KNOWN-ISSUES 1), so the options menu can
	 * change it and find out. */
	engine.set_realtime(g_opts.realtime);
	engine.set_requested_video(g_opts.req_w, g_opts.req_h,
				   g_opts.req_bitrate);
	if (g_opts.alias)
		engine.set_alias_override(g_opts.alias);
	if (g_opts.osname || g_opts.display_w > 0)
		engine.set_device_override(g_opts.osname ? g_opts.osname : "",
					   g_opts.display_w, g_opts.display_h);
	if (g_opts.sdp_max_fs > 0 || g_opts.sdp_max_mbps > 0 || g_opts.sdp_level)
		engine.set_sdp_caps(g_opts.sdp_max_fs, g_opts.sdp_max_mbps,
				    g_opts.sdp_level ? g_opts.sdp_level : "");
	engine.start(request.target, request.id,
		     (gnx::QualityTier)g_opts.tier, g_opts.locale);

	run_session(out, f, p, engine, title);

	/*
	 * Did this title honour the size we asked for? Recorded per title so
	 * the library can mark it: on this panel a native 640x360 stream is a
	 * different class of experience (no downscale, a third of the
	 * bitrate, a quarter of the decode), and which games do it is not
	 * predictable from anything else about them -- of 27 swept, one did.
	 * Cloud titles only: remote play streams a console, not a game.
	 */
	if (request.target == gnx::stream::StreamTarget::Cloud &&
	    !request.id.empty()) {
		int w = g_stream_w.load(std::memory_order_relaxed);
		if (w > 0)
			note_native(request.id, w < 1280);
	}

	/* After run_session(), so the engine is stopped and nothing is still
	 * writing access units into the recorder. */
	recorder.close();
	if (wifi_applied)
		wifi_tune_restore(&wifi);
}

void stream_game(drm_out &out, Fonts &f, pad &p, gnx::XboxAuth &auth,
		 const gnx::Game &game)
{
	StreamRequest request;

	request.target = gnx::stream::StreamTarget::Cloud;
	request.id = game.title_id;
	request.name = game.name;
	stream_session(out, f, p, auth, request);
}

}  // namespace app
