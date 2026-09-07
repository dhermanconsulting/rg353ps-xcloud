/*
 * replay_stream(): the offline harness. A recording (au_recorder.hpp) is fed
 * through the same decode thread, presenter and audio player with its
 * original arrival timing. See docs/VIDEO-PACING.md, "The harness".
 *
 * -impair "<spec>" puts a network impairment model between the file and the
 * pipeline, so the resync path, the audio reorder buffer's time-bounded
 * wait and the presenter's stall handling can be exercised from a clean
 * recording (the synthetic one from tools/mkstream.cpp, in the host
 * simulator). Comma-separated, applied to the recorded arrival times:
 *
 *   jitter=<ms>      Gaussian jitter on every release, clamped at three
 *                    sigma and never reordering a stream: video stays in
 *                    decode order and audio in sequence order, so the
 *                    reorder buffer sees ragged spacing and gaps, not swaps.
 *   gap=<at>:<len>   every packet due in [at, at+len) ms is held and the lot
 *                    released at at+len: a stall, which is exactly what Wi-Fi
 *                    power save and a background scan do on the device
 *                    (src/net/wifi_tune.c). Repeatable.
 *   loss=<pct>       a video access unit is dropped with this probability
 *                    and then -- as in the live path, where the jitter
 *                    buffer sets waiting_keyframe and asks for a PLI
 *                    (src/net/engine.cpp on_video) -- every following unit
 *                    until one carries an IDR. A lost audio packet is simply
 *                    not submitted, sequence number skipped, so the
 *                    AudioJitterBuffer's 40 ms wait and silence fill run.
 *   burst=<n>        losses come in runs averaging n packets (a two-state
 *                    Gilbert process whose stationary loss rate stays at
 *                    loss=); default 1, independent losses.
 *
 * The RNG is seeded from the spec string, so the same spec drops the same
 * units on every run and two builds can be compared honestly. The SUMMARY
 * line reports lost= (units the model dropped), resync= (times the stream
 * went blind), discarded= (units thrown away waiting for the IDR) and
 * alost= (audio packets dropped).
 *
 * arm_quit_after() is -quit-after <s>: the bound scripts/test.sh puts on
 * every run.
 */
#include "app.hpp"

#include "options.hpp"

#include <random>

namespace app {

namespace {

/* ---- impairment model --------------------------------------------------- */

struct Impair {
	double jitter_ms = 0;
	struct Gap {
		double at_ms, len_ms;
	};
	std::vector<Gap> gaps;
	double loss_pct = 0;
	double burst = 1;
	uint32_t seed = 0;
	bool enabled = false;
};

/* FNV-1a: the same spec is the same seed on every machine, and any change to
 * it is a different one. */
uint32_t hash_spec(const char *s)
{
	uint32_t h = 2166136261u;
	for (; *s; s++) {
		h ^= (uint8_t)*s;
		h *= 16777619u;
	}
	return h;
}

/* A typo in the spec must fail the run, not silently model nothing. */
bool parse_impair(const char *spec, Impair *m, std::string *err)
{
	*m = Impair();
	if (!spec || !*spec)
		return true;
	m->enabled = true;
	m->seed = hash_spec(spec);
	std::string s(spec);
	size_t pos = 0;
	while (pos <= s.size()) {
		size_t comma = s.find(',', pos);
		if (comma == std::string::npos)
			comma = s.size();
		std::string tok = s.substr(pos, comma - pos);
		pos = comma + 1;
		while (!tok.empty() && std::isspace((unsigned char)tok.front()))
			tok.erase(0, 1);
		while (!tok.empty() && std::isspace((unsigned char)tok.back()))
			tok.pop_back();
		if (tok.empty())
			continue;
		size_t eq = tok.find('=');
		if (eq == std::string::npos) {
			*err = "'" + tok + "' is not key=value";
			return false;
		}
		std::string key = tok.substr(0, eq), val = tok.substr(eq + 1);
		char *end = nullptr;
		double v = std::strtod(val.c_str(), &end);
		bool number = end != val.c_str() && *end == '\0' && v >= 0;
		if (key == "jitter") {
			if (!number) {
				*err = "jitter wants a millisecond count";
				return false;
			}
			m->jitter_ms = v;
		} else if (key == "gap") {
			char *end2 = nullptr;
			double len = end != val.c_str() && *end == ':'
					     ? std::strtod(end + 1, &end2)
					     : -1;
			if (v < 0 || len < 0 || !end2 || end2 == end + 1 ||
			    *end2) {
				*err = "gap wants <at_ms>:<len_ms>";
				return false;
			}
			m->gaps.push_back({v, len});
		} else if (key == "loss") {
			if (!number || v >= 100) {
				*err = "loss wants a percentage below 100";
				return false;
			}
			m->loss_pct = v;
		} else if (key == "burst") {
			if (!number || v < 1) {
				*err = "burst wants a run length of 1 or more";
				return false;
			}
			m->burst = v;
		} else {
			*err = "unknown key '" + key +
			       "' (jitter, gap, loss, burst)";
			return false;
		}
	}
	return true;
}

std::string describe_impair(const Impair &m)
{
	char buf[128];
	std::snprintf(buf, sizeof(buf), "jitter=%.1fms", m.jitter_ms);
	std::string s = buf;
	for (const Impair::Gap &g : m.gaps) {
		std::snprintf(buf, sizeof(buf), " gap=%.0f+%.0fms", g.at_ms,
			      g.len_ms);
		s += buf;
	}
	std::snprintf(buf, sizeof(buf), " loss=%.2f%% burst=%.0f seed=0x%08x",
		      m.loss_pct, m.burst, m.seed);
	return s + buf;
}

/*
 * Two-state loss process (Gilbert). In the good state a packet starts a run
 * with probability p_start; in the bad state each packet is lost and the run
 * continues with probability 1 - 1/burst, so runs average `burst` packets.
 * p_start is chosen so the stationary loss fraction is loss/100 whatever the
 * burst length: fraction = p_start*burst / (1 + p_start*burst).
 */
class LossProcess {
public:
	LossProcess(double loss_pct, double burst)
	{
		double p = loss_pct / 100.0;
		p_start_ = p > 0 ? p / (burst * (1.0 - p)) : 0.0;
		p_cont_ = 1.0 - 1.0 / burst;
	}
	bool lost(std::mt19937 &rng)
	{
		double u = uni_(rng);
		bad_ = bad_ ? u < p_cont_ : u < p_start_;
		return bad_;
	}

private:
	double p_start_, p_cont_;
	bool bad_ = false;
	std::uniform_real_distribution<double> uni_{0.0, 1.0};
};

/* The IDR test of video_pipeline.cpp's au_info (src/media is not this
 * file's to change): emulation prevention keeps 00 00 01 out of NAL
 * payloads, so a plain scan for a start code and NAL type 5 is exact. */
bool au_has_idr(const std::vector<uint8_t> &au)
{
	for (size_t k = 0; k + 3 < au.size(); k++) {
		if (au[k] == 0 && au[k + 1] == 0 && au[k + 2] == 1) {
			if ((au[k + 3] & 0x1f) == 5)
				return true;
			k += 3;
		}
	}
	return false;
}

/* A record whose release time the model has decided, waiting its turn. */
struct Pending {
	double due_ms;  /* relative to the recording's first record */
	uint64_t order; /* file position, the tie-breaker */
	gnx::stream::AuRecorder::Kind kind;
	uint32_t seq;
	std::vector<uint8_t> data;
};

/* Earliest due first; std::push_heap wants "less than" for a max-heap. */
struct Later {
	bool operator()(const Pending &a, const Pending &b) const
	{
		return a.due_ms != b.due_ms ? a.due_ms > b.due_ms
					    : a.order > b.order;
	}
};

}  // namespace

/*
 * -quit-after <s>: a bound on any run, for scripts/test.sh. Sets g_stop as
 * the signal handlers do, so every loop -- replay, library, sign-in -- exits
 * on its own path and the replay SUMMARY still prints. Detached; it polls
 * g_stop so it is gone as soon as the run ends first, and a run that ends
 * before the deadline simply never hears from it.
 */
void arm_quit_after(int secs)
{
	if (secs <= 0)
		return;
	std::thread([secs] {
		pthread_setname_np(pthread_self(), "xc-quit");
		const double deadline = now_ms() + secs * 1000.0;
		while (!g_stop && now_ms() < deadline)
			usleep(100000);
		if (!g_stop) {
			std::fprintf(stderr, "quit-after: %d s elapsed, stopping\n",
				     secs);
			g_stop = 1;
			g_abort = true;
		}
	}).detach();
}

/*
 * Replay a recording through the same pipeline and present loop, with the
 * original arrival timing scaled by -speed. No network, no session, no
 * account: the offline harness for pacing work. Audio packets go to the
 * same AudioPlayer the engine uses.
 */
void replay_stream(drm_out &out, Fonts &f, pad &p, const char *path)
{
	Impair model;
	{
		std::string err;
		if (!parse_impair(g_opts.impair, &model, &err)) {
			std::fprintf(stderr, "replay: bad -impair spec: %s\n",
				     err.c_str());
			return;
		}
	}

	gnx::stream::AuReader reader;
	if (!reader.open(path))
		return;

	gnx::stream::VideoPipeline pipe;
	gnx::stream::AudioPlayer audio;
	AsyncLog alog;
	GovernorGuard governor;
	std::mutex qmutex;
	std::deque<std::pair<std::vector<uint8_t>, uint64_t> > queue;
	std::atomic<bool> eof{false};
	std::atomic<bool> feeder_stop{false};
	std::atomic<uint32_t> fed{0};
	std::atomic<uint32_t> overflows{0};
	/* The impairment model's own accounting, for the SUMMARY. */
	std::atomic<uint32_t> lost{0}, resyncs{0}, discarded{0}, alost{0};
	const pid_t main_tid = (pid_t)syscall(SYS_gettid);
	PresentStats ps;
	double last_tick = 0;
	uint64_t prev_flip_us = 0;
	bool committed = false;
	bool video_mode = false;
	bool rt_set = false;

	if (!audio.init())
		std::fprintf(stderr, "replay: no audio output\n");

	/* Feeder: releases each record at its recorded arrival time, put
	 * through the impairment model when there is one. Mirrors the
	 * engine's worker: same queue cap, same overflow-and-resync
	 * behaviour (minus the keyframe request, which a file cannot
	 * honour -- the drop simply lands on whatever comes next).
	 *
	 * Records are read a little ahead into a heap ordered by release
	 * time, because jitter can move a packet earlier than one that
	 * precedes it in the file (the two streams are independent) and a
	 * gap holds a whole stretch of the file behind everything after it.
	 * A record can be released once every unread one is provably later:
	 * the model only ever moves a release EARLIER by the jitter clamp,
	 * and file timestamps are monotone, so once the newest record read
	 * is more than three sigma past the heap's top, the top is final.
	 * Without a model that is one record of read-ahead. */
	std::thread feeder([&] {
		pthread_setname_np(pthread_self(), "xc-worker");
		using Kind = gnx::stream::AuRecorder::Kind;
		std::mt19937 rng(model.seed);
		std::normal_distribution<double> jitter(
			0.0, model.jitter_ms > 0 ? model.jitter_ms : 1.0);
		LossProcess vloss(model.loss_pct, model.burst);
		LossProcess aloss(model.loss_pct, model.burst);
		const double clamp = 3.0 * model.jitter_ms;
		bool waiting_idr = false;
		std::vector<Pending> heap;
		bool file_done = false, have_first = false;
		uint64_t first_ts = 0, order = 0;
		double newest_t = 0, last_due[2] = {-1e18, -1e18};
		Kind kind;
		uint32_t seq;
		uint64_t ts;
		std::vector<uint8_t> data;
		const double start = now_ms();

		while (!g_stop && !feeder_stop) {
			while (!file_done &&
			       (heap.empty() ||
				newest_t - clamp <= heap.front().due_ms)) {
				if (!reader.next(&kind, &seq, &ts, data)) {
					file_done = true;
					break;
				}
				if (!have_first) {
					first_ts = ts;
					have_first = true;
				}
				const double t = (double)(ts - first_ts);
				newest_t = t;
				if (kind != Kind::Video && kind != Kind::Audio)
					continue;
				double due = t;
				if (model.enabled) {
					if (kind == Kind::Video) {
						/* The loss process runs on every
						 * unit, including those already
						 * doomed by an earlier loss, so
						 * a burst keeps its shape. */
						if (vloss.lost(rng)) {
							lost++;
							if (!waiting_idr) {
								waiting_idr = true;
								resyncs++;
							}
							continue;
						}
						if (waiting_idr) {
							if (!au_has_idr(data)) {
								discarded++;
								continue;
							}
							waiting_idr = false;
						}
					} else if (aloss.lost(rng)) {
						alost++;
						continue;
					}
					if (model.jitter_ms > 0)
						due += std::max(-clamp,
								std::min(clamp,
									 jitter(rng)));
					/* A gap may end inside another gap. */
					for (bool held = true; held;) {
						held = false;
						for (const Impair::Gap &g : model.gaps)
							if (due >= g.at_ms &&
							    due < g.at_ms + g.len_ms) {
								due = g.at_ms + g.len_ms;
								held = true;
							}
					}
					if (due < last_due[kind])
						due = last_due[kind];
					last_due[kind] = due;
				}
				heap.push_back({due, order++, kind, seq,
						std::move(data)});
				std::push_heap(heap.begin(), heap.end(), Later());
			}
			if (heap.empty())
				break;
			std::pop_heap(heap.begin(), heap.end(), Later());
			Pending rec = std::move(heap.back());
			heap.pop_back();

			/* Sleep in short slices so a stop lands within 50 ms
			 * even from inside a two-second gap. */
			for (;;) {
				double wait = start + rec.due_ms / g_opts.speed -
					      now_ms();
				if (wait <= 0 || g_stop || feeder_stop)
					break;
				usleep((useconds_t)(std::min(wait, 50.0) * 1000.0));
			}
			if (g_stop || feeder_stop)
				break;
			if (rec.kind == Kind::Video) {
				{
					std::lock_guard<std::mutex> lock(qmutex);
					if (queue.size() >= 16) {
						queue.clear();
						overflows++;
					}
					queue.push_back({std::move(rec.data),
							 (uint64_t)now_ms()});
				}
				fed++;
				pipe.notify();
			} else {
				audio.submit((uint16_t)rec.seq, rec.data.data(),
					     rec.data.size());
			}
		}
		eof = true;
	});

	pipe.start(g_opts.pipeline,
		   [&](std::vector<uint8_t> &au, uint64_t *arrival) {
			   std::lock_guard<std::mutex> lock(qmutex);
			   if (queue.empty())
				   return false;
			   au = std::move(queue.front().first);
			   *arrival = queue.front().second;
			   queue.pop_front();
			   return true;
		   });
	std::fprintf(stderr, "replay: %s at %.2fx, threads=%d reserve=%d drop_at=%d\n",
		     path, g_opts.speed, (int)g_opts.pipeline.threading,
		     g_opts.pipeline.reserve, g_opts.pipeline.drop_at);
	if (model.enabled)
		std::fprintf(stderr, "impair: %s\n", describe_impair(model).c_str());

	show_message(out, f, "Replaying...", path);
	ps.reset(now_ms(), sample_cpu(main_tid));
	uint32_t total_shown = 0, total_held = 0, total_skipped = 0;
	uint32_t total_late = 0, total_decoded = 0;
	double t_first = 0;

	while (!g_stop) {
		if (!video_mode) {
			pad_poll(&p, 16);
			if (quit_combo(p) || pad_take_press(&p, PAD_B))
				break;
			if (eof && !pipe.have_frame())
				break;
			if (pipe.have_frame()) {
				video_mode = true;
				if (g_opts.realtime)
					rt_set = set_realtime("present", 12);
				last_tick = now_ms();
				t_first = last_tick;
				ps.reset(last_tick, sample_cpu(main_tid));
			}
			continue;
		}
		if (eof) {
			/* End of the recording: let the reserve frame out and
			 * stop once nothing is left anywhere. */
			bool drained;
			{
				std::lock_guard<std::mutex> lock(qmutex);
				drained = queue.empty();
			}
			if (drained) {
				pipe.flush();
				if (pipe.queued() == 0)
					break;
			}
		}

		last_tick = now_ms();
		note_flip(ps, out.last_flip_us, prev_flip_us);

		if (!present_tick(out, f, pipe, ps, &committed))
			break;

		pad_poll(&p, 0);
		/* The overlay works here too: a recording is the steadiest
		 * way to compare downscale filters, the same frames every
		 * time. */
		options_menu_input(p, pad_now_ms());
		if (quit_combo(p) || pad_take_press(&p, PAD_B))
			break;
		double now = now_ms();
		if (now - ps.t0 >= 1000.0) {
			CpuSample c = sample_cpu(main_tid);
			gnx::stream::VideoPipeline::Stats d = pipe.snapshot();
			size_t auq;
			{
				std::lock_guard<std::mutex> lock(qmutex);
				auq = queue.size();
			}
			alog.push(report_pace(ps, d, now, c, auq,
					      out.stale_events));
			gnx::stream::AudioPlayer::Stats a = audio.stats();
			char aline[320];
			std::snprintf(aline, sizeof(aline),
				      "[%8llu] audio| play=%u fail=%u lost=%u under=%u"
				      " drop=%ums q=%ums ring=%ums inmax=%u waits=%u"
				      " alsa=%u-%ums blk=%ums adj=%d fed=%u overflow=%u\n",
				      (unsigned long long)now, a.played, a.failed,
				      a.lost, a.underruns, a.dropped_ms, a.queue_ms,
				      a.ring_ms, a.inbox_max, a.reorder_waits,
				      a.delay_min_ms, a.delay_max_ms,
				      a.write_block_max_ms, a.servo_samples,
				      fed.load(), overflows.load());
			alog.push(aline);
			total_shown += ps.shown;
			total_held += ps.held;
			total_skipped += d.skipped;
			total_late += ps.tick_late;
			total_decoded += d.decoded;
			ps.reset(now, c);
		}

		wait_refresh(out, committed, last_tick);
		if (out.last_flip_us) {
			double wake = now_ms() - out.last_flip_us / 1000.0;
			if (wake > ps.wake_max && wake < 100.0)
				ps.wake_max = wake;
		}
	}

	/* Whole-run summary: the number to compare between builds. The last
	 * four fields are the impairment model's; all zero without one. */
	{
		double secs = t_first ? (now_ms() - t_first) / 1000.0 : 0;
		gnx::stream::VideoPipeline::Stats d = pipe.snapshot();
		total_shown += ps.shown;
		total_held += ps.held;
		total_skipped += d.skipped;
		total_late += ps.tick_late;
		total_decoded += d.decoded;
		std::fprintf(stderr,
			     "replay| SUMMARY %.1fs decoded=%u shown=%u held=%u"
			     " skipped=%u late=%u fps=%.1f fed=%u overflow=%u"
			     " lost=%u resync=%u discarded=%u alost=%u\n",
			     secs, total_decoded, total_shown, total_held,
			     total_skipped, total_late,
			     secs > 0 ? total_shown / secs : 0.0, fed.load(),
			     overflows.load(), lost.load(), resyncs.load(),
			     discarded.load(), alost.load());
	}

	if (rt_set)
		set_normal();
	feeder_stop = true;
	if (feeder.joinable())
		feeder.join();
	pipe.stop();
	audio.shutdown();
	if (video_mode)
		drm_out_set_source(&out, kWidth, kHeight);
}

}  // namespace app
