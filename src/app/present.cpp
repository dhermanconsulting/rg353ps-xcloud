/*
 * The presenter: one frame per refresh, paced by the page-flip event, plus
 * the per-second pace| accounting and the small system knobs a stream needs
 * (real-time priority, CPU governor, an async log). See docs/VIDEO-PACING.md.
 */
#include "app.hpp"

#include "options.hpp"

namespace app {

/* Published for stream_session(); see app.hpp. */
std::atomic<int> g_stream_w{0}, g_stream_h{0};

gnx::xcloud::GamepadFrame pad_to_frame(const pad &p)
{
	gnx::xcloud::GamepadFrame f;

	/* PAD_A..PAD_Y already mean the Xbox function: evdev_pad's layout
	 * (printed labels by default, see evdev_pad.h) did the translation. */
	f.a = p.down[PAD_A];
	f.b = p.down[PAD_B];
	f.x = p.down[PAD_X];
	f.y = p.down[PAD_Y];
	f.left_shoulder = p.down[PAD_L1];
	f.right_shoulder = p.down[PAD_R1];
	f.view = p.down[PAD_SELECT];
	f.menu = p.down[PAD_START];
	f.nexus = p.down[PAD_MODE];
	f.left_thumb = p.down[PAD_L3];
	f.right_thumb = p.down[PAD_R3];
	f.dpad_up = p.down[PAD_UP];
	f.dpad_down = p.down[PAD_DOWN];
	f.dpad_left = p.down[PAD_LEFT];
	f.dpad_right = p.down[PAD_RIGHT];
	/* No ABS_Z/ABS_RZ on this pad, so the triggers are digital: the u16
	 * trigger fields only ever see 0 or full scale. */
	f.left_trigger = p.down[PAD_L2] ? 1.0f : 0.0f;
	f.right_trigger = p.down[PAD_R2] ? 1.0f : 0.0f;
	f.left_x = p.lx / 32767.0f;
	f.left_y = p.ly / 32767.0f;
	f.right_x = p.rx / 32767.0f;
	f.right_y = p.ry / 32767.0f;
	return f;
}

/* SELECT+START, the usual handheld quit gesture. */
bool quit_combo(const pad &p)
{
	return p.down[PAD_SELECT] && p.down[PAD_START];
}

double now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/*
 * CPU time per thread, in clock ticks, from /proc/self/task. libavcodec's
 * worker threads inherit the process name, so anything still called "xcloud"
 * other than the main thread is the decoder's pool; the decode thread itself
 * is named.
 */
CpuSample sample_cpu(pid_t main_tid)
{
	CpuSample s;
	DIR *d = opendir("/proc/self/task");
	struct dirent *e;

	if (!d)
		return s;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[300];
		std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat",
			      e->d_name);
		FILE *fp = std::fopen(path, "r");
		if (!fp)
			continue;
		char buf[512];
		size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
		std::fclose(fp);
		buf[n] = 0;
		char *lp = std::strchr(buf, '(');
		char *rp = std::strrchr(buf, ')');
		if (!lp || !rp || rp < lp)
			continue;
		std::string comm(lp + 1, rp - lp - 1);
		unsigned long ut = 0, st = 0;
		/* After ")": state ppid pgrp session tty tpgid flags minflt
		 * cminflt majflt cmajflt utime stime */
		if (std::sscanf(rp + 2,
				"%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
				&ut, &st) != 2)
			continue;
		uint64_t t = ut + st;
		pid_t tid = (pid_t)std::atoi(e->d_name);
		if (tid == main_tid)
			s.main += t;
		else if (comm == "xc-worker")
			s.worker += t;
		else if (comm == "xc-audio")
			s.audio += t;
		else if (comm == "xcloud" || comm == "xc-decode")
			s.dec += t;
		else
			s.other += t;
		s.total += t;
	}
	closedir(d);
	/* The SoC thermal zone: with all four cores pinned at 1.8 GHz a long
	 * race could reach the throttle point, and a throttled decode would
	 * look like a network problem. Having the number next to the decode
	 * time settles that in one glance. */
	if (FILE *fp = std::fopen("/sys/class/thermal/thermal_zone0/temp", "r")) {
		if (std::fscanf(fp, "%d", &s.temp_mc) != 1)
			s.temp_mc = 0;
		std::fclose(fp);
	}
	/* VmRSS: the decoder's frame pool, the art cache and the jitter
	 * buffers all live here, and the device has 1 GB and no swap. */
	if (FILE *fp = std::fopen("/proc/self/status", "r")) {
		char line[128];
		while (std::fgets(line, sizeof(line), fp)) {
			if (!std::strncmp(line, "VmRSS:", 6)) {
				s.rss_kb = std::atol(line + 6);
				break;
			}
		}
		std::fclose(fp);
	}
	return s;
}

/*
 * Ask for a real-time priority. We are root on a box we own outright, so
 * SCHED_FIFO is the honest tool: the present thread must wake on the flip
 * event and commit within a couple of milliseconds, and the audio thread
 * must feed ALSA on time, whatever the decoder's worker threads are doing on
 * the other cores. Returns false (and says so) if refused.
 */
bool set_realtime(const char *what, int priority)
{
	struct sched_param sp;
	std::memset(&sp, 0, sizeof(sp));
	sp.sched_priority = priority;
	if (sched_setscheduler(0, SCHED_FIFO, &sp)) {
		std::fprintf(stderr, "xcloud: %s: SCHED_FIFO refused: %s\n", what,
			     std::strerror(errno));
		return false;
	}
	return true;
}

/*
 * Back to a normal thread. Every thread created afterwards inherits the
 * caller's policy, so leaving the main thread at SCHED_FIFO after a stream
 * would make the NEXT stream's worker, audio and decoder threads real-time
 * too -- and a real-time decoder starves everything else on the box.
 */
void set_normal()
{
	struct sched_param sp;
	std::memset(&sp, 0, sizeof(sp));
	sched_setscheduler(0, SCHED_OTHER, &sp);
}

StreamOptions g_opts;

/*
 * Convert one decoded frame into the plane's back buffer, resizing the output
 * first if the stream changed shape.
 *
 * xCloud does change resolution mid-session: a stall, a PLI and a fresh IDR
 * can come back at a different size, and the plane's buffers are sized for
 * the OLD one. Copying the old dimensions out of the new (smaller) frame
 * reads off the end of it, which is exactly the segfault in nv12_copy_luma
 * this guard exists to prevent. Sizes must therefore be re-checked on every
 * frame, not just the first.
 *
 * Returns 1 if a frame was written, 0 if there was nothing usable, -1 if the
 * output could not be reconfigured.
 */
int prepare_frame(drm_out &out, const AVFrame *frame)
{
	drm_out_buf *b;

	if (frame->width <= 0 || frame->height <= 0)
		return 0;  /* nothing decoded yet; not an error */
	/* nv12_* assume 8-bit planar 4:2:0. Anything else would be silently
	 * misread as if it were. */
	if (frame->format != AV_PIX_FMT_YUV420P &&
	    frame->format != AV_PIX_FMT_YUVJ420P) {
		std::fprintf(stderr, "xcloud: unexpected pixel format %d\n",
			     frame->format);
		return 0;
	}

	/*
	 * With -scale box|sharp, halve the frame here instead of letting the
	 * plane do it -- but only when halving lands exactly on the
	 * rectangle the frame would have been letterboxed into, which is the
	 * 1280x720 -> 640x360 case. Anything else keeps the old path, so a
	 * mid-session resolution change can never leave the picture the
	 * wrong shape.
	 */
	int want_w = frame->width, want_h = frame->height;
	bool halve = false;

	/*
	 * Fitting first: it only moves the plane's rectangles, and doing it
	 * before the size check means a change takes effect on the very next
	 * commit rather than waiting for the stream to change shape.
	 */
	drm_out_set_zoom(&out, g_opts.zoom_fill.load() ? 1 : 0,
			 g_opts.pan.load());

	/*
	 * The CPU halving needs the fitted rectangle to be exactly half the
	 * frame, which is only true when letterboxing. Filling scales 960x720
	 * into 640x480, so there is no halving to do here and the plane's
	 * scaler does all of it.
	 */
	const enum nv12_scale mode = g_opts.zoom_fill.load()
					     ? NV12_SCALE_HW
					     : (enum nv12_scale)g_opts.scale.load();

	if (mode != NV12_SCALE_HW && !(frame->width & 1) &&
	    !(frame->height & 1)) {
		int fit_w, fit_h;

		drm_out_fit(&out, frame->width, frame->height, &fit_w, &fit_h);
		if (fit_w == frame->width / 2 && fit_h == frame->height / 2) {
			want_w = fit_w;
			want_h = fit_h;
			halve = true;
		}
	}

	if (want_w != out.src_w || want_h != out.src_h) {
		std::fprintf(stderr, "xcloud: stream is now %dx%d%s\n",
			     frame->width, frame->height,
			     halve ? " (halved on the CPU)" : "");
		/* Publish what the SERVER sent, not what we display: the
		 * halving below is ours, and using the displayed size would
		 * make every title look as though it had gone native. */
		g_stream_w.store(frame->width, std::memory_order_relaxed);
		g_stream_h.store(frame->height, std::memory_order_relaxed);
		if (drm_out_set_source(&out, want_w, want_h))
			return -1;
	}

	b = drm_out_back_buffer(&out);
	if (halve) {
		nv12_scale2_luma(b->luma, b->pitch, frame->data[0],
				 frame->linesize[0], out.src_w, out.src_h,
				 mode);
		nv12_scale2_chroma(b->chroma, b->pitch, frame->data[1],
				   frame->linesize[1], frame->data[2],
				   frame->linesize[2], out.src_w, out.src_h);
		return 1;
	}
	nv12_copy_luma(b->luma, b->pitch, frame->data[0], frame->linesize[0],
		       out.src_w, out.src_h);
	nv12_interleave_chroma(b->chroma, b->pitch, frame->data[1],
			       frame->linesize[1], frame->data[2],
			       frame->linesize[2], out.src_w, out.src_h);
	return 1;
}

/*
 * Synthetic pad input for unattended runs: a slow stick wiggle so the camera
 * moves and the encoder has real motion to send, plus a tap of A every few
 * seconds to get through menus. Nothing about it is game-aware.
 */
gnx::xcloud::GamepadFrame autoplay_frame(gnx::xcloud::GamepadFrame f)
{
	double t = now_ms() / 1000.0;
	f.left_x = (float)(0.7 * std::sin(t * 2.0));
	f.left_y = (float)(0.4 * std::sin(t * 0.7));
	f.right_x = (float)(0.5 * std::sin(t * 1.3));
	f.a = std::fmod(t, 4.0) < 0.15;
	return f;
}
std::string report_pace(const PresentStats &ps,
			const gnx::stream::VideoPipeline::Stats &d, double now,
			const CpuSample &c, size_t au_queue, uint32_t stale)
{
	double secs = (now - ps.t0) / 1000.0;
	if (secs <= 0)
		return std::string();
	long tck = sysconf(_SC_CLK_TCK);
	/* A thread that exited between samples takes its ticks with it, so
	 * the delta can go negative; report that as zero rather than garbage. */
	auto pct = [&](uint64_t a, uint64_t b) {
		return a < b ? 0 : (int)((double)(a - b) * 100.0 / (double)tck / secs);
	};
	char line[640];
	std::snprintf(line, sizeof(line),
		      "[%8llu] pace| shown=%u held=%u skip=%u hold=%u/%u/%u/%u"
		      " q=%zu auq=%zu stale=%u"
		      " | dec=%u %.1f/%.1fms idr=%u(%.1fms) slices=%d-%d err=%u"
		      " | cvt=%.1f/%.1f commit=%.1f shadow=%.1f wake=%.1f"
		      " flip=%.2f/%.1f late=%u lat=%.0f/%.0fms"
		      " | cpu=%d%% main %d dec %d worker %d audio %d other %d"
		      " temp=%dC rss=%ldMB\n",
		      (unsigned long long)now, ps.shown, ps.held, d.skipped,
		      d.hold_hist[0], d.hold_hist[1], d.hold_hist[2], d.hold_hist[3],
		      d.qmax, au_queue, stale,
		      d.decoded, d.taken ? d.dec_sum / d.taken : 0.0, d.dec_max,
		      d.keyframes, d.idr_dec_max,
		      d.slices_min == 99 ? 0 : d.slices_min, d.slices_max,
		      d.decode_errors,
		      ps.shown ? ps.cvt_sum / ps.shown : 0.0, ps.cvt_max,
		      ps.commit_max, ps.shadow_max, ps.wake_max,
		      ps.ticks ? ps.tick_sum / ps.ticks : 0.0, ps.tick_max,
		      ps.tick_late, ps.lat_n ? ps.lat_sum / ps.lat_n : 0.0,
		      ps.lat_max, pct(c.total, ps.cpu.total),
		      pct(c.main, ps.cpu.main), pct(c.dec, ps.cpu.dec),
		      pct(c.worker, ps.cpu.worker), pct(c.audio, ps.cpu.audio),
		      pct(c.other, ps.cpu.other), c.temp_mc / 1000,
		      c.rss_kb / 1024);
	return line;
}

/*
 * Flip-to-flip interval from the kernel's own vblank timestamps, so the
 * figure is the panel's cadence and not our scheduling latency. Over 20 ms
 * means a refresh went by without our commit latching.
 */
void note_flip(PresentStats &ps, uint64_t flip_us, uint64_t &prev_flip_us)
{
	if (prev_flip_us && flip_us > prev_flip_us) {
		double gap = (flip_us - prev_flip_us) / 1000.0;
		ps.tick_sum += gap;
		ps.ticks++;
		if (gap > ps.tick_max)
			ps.tick_max = gap;
		if (gap > 20.0)
			ps.tick_late++;
	}
	/* The flip that just completed showed the frame committed last
	 * refresh; its pts is the arrival time of its access unit. */
	if (flip_us != prev_flip_us && ps.pending_pts >= 0 && flip_us) {
		double lat = flip_us / 1000.0 - (double)ps.pending_pts;
		if (lat >= 0 && lat < 2000) {
			ps.lat_sum += lat;
			ps.lat_n++;
			if (lat > ps.lat_max)
				ps.lat_max = lat;
		}
	}
	prev_flip_us = flip_us;
}

/*
 * One refresh of the vsync-paced present loop. Call with the previous flip
 * already complete (drm_out_wait_flip) or, on the first call, with nothing
 * pending. Picks a frame, converts it, commits it, and returns; the caller
 * does its shadow work and then waits for the flip.
 *
 * Returns false if the output could not be reconfigured (fatal).
 */
bool present_tick(drm_out &out, Fonts &f, gnx::stream::VideoPipeline &pipe,
		  PresentStats &ps, bool *committed)
{
	*committed = false;
	ps.pending_pts = -1;
	AVFrame *frame = pipe.pick();
	if (!frame) {
		ps.held++;
		return true;
	}
	double t0 = now_ms();
	int ok = prepare_frame(out, frame);
	int64_t pts = frame->pts;
	av_frame_free(&frame);
	double t1 = now_ms();
	if (ok < 0)
		return false;
	if (ok == 0)
		return true;
	/*
	 * The options overlay goes on after the conversion is timed and
	 * before the commit is, so having it open does not quietly inflate
	 * either column -- comparing cvt between downscale filters is
	 * exactly what it is there for.
	 */
	if (options_menu_open())
		options_menu_draw(out, f);
	double t1b = now_ms();
	if (drm_out_commit(&out))
		return false;
	double t2 = now_ms();
	*committed = true;
	/* Only a committed frame gets paired with the next flip; a tick
	 * that showed nothing must not lend its pts to the re-commit. The
	 * pts is the assembler's emit time, so the figure is "unit complete
	 * to vblank", which for a frame held behind a retransmit is up to
	 * the hold shorter than "last packet to vblank". */
	ps.pending_pts = pts;
	ps.shown++;
	ps.cvt_sum += t1 - t0;
	if (t1 - t0 > ps.cvt_max)
		ps.cvt_max = t1 - t0;
	if (t2 - t1b > ps.commit_max)
		ps.commit_max = t2 - t1b;
	return true;
}

/*
 * Wait out the rest of this refresh: the flip event if one is pending,
 * otherwise a plain vblank wait so a held picture still paces the loop.
 * Falls back to a clock sleep if the vblank ioctl refuses, so the loop can
 * never spin.
 */
void late_latch_wait(const drm_out &out)
{
	/* The panel period, learnt from the kernel's own flip timestamps
	 * (16.58 ms measured, not the nominal 16.67), so the prediction
	 * tracks the real clock. */
	static double period_us = 16580.0;
	static uint64_t prev_flip = 0;

	if (g_opts.latch_ms <= 0 || !out.last_flip_us)
		return;
	if (prev_flip && out.last_flip_us > prev_flip) {
		double d = (double)(out.last_flip_us - prev_flip);
		if (d > 15000.0 && d < 18000.0)
			period_us += (d - period_us) * 0.05;
	}
	prev_flip = out.last_flip_us;

	double target = out.last_flip_us / 1000.0 + period_us / 1000.0 -
			(double)g_opts.latch_ms;
	double now = now_ms();
	if (target > now && target - now < 20.0)
		usleep((useconds_t)((target - now) * 1000.0));
}

void wait_refresh(drm_out &out, bool committed, double last_tick)
{
	if (committed) {
		drm_out_wait_flip(&out, 200);
		return;
	}
	/* Held picture: re-commit what is on screen so this refresh still
	 * ends on a flip event, the same wait as every other refresh. */
	if (drm_out_recommit(&out) == 0) {
		drm_out_wait_flip(&out, 200);
		return;
	}
	if (drm_out_wait_vblank(&out) == 0)
		return;
	double due = last_tick + 16.667;
	double now = now_ms();
	if (due > now)
		usleep((useconds_t)((due - now) * 1000.0));
}

}  // namespace app
