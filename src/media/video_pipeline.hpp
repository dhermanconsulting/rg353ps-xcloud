#pragma once

/*
 * Decode thread + decoded-frame queue + present-time pick policy.
 *
 * Why this exists. The first streaming loop did everything on one thread:
 * drain every queued access unit, decode each, present the last, block until
 * the flip. One unit per pass fits in a refresh; two do not, so the pass ends
 * on the second vblank, shows one frame, and two more units arrive meanwhile.
 * Every backlog depth is a stable state, jitter walks it upward, and half the
 * decoded frames never reached the screen (measured: shown=20-46 of 60/s).
 *
 * Here the decoder runs on its own thread and never skips a unit (P-frames
 * reference every predecessor). Decoded frames queue in source order. The
 * present thread, paced by the page-flip event, picks one frame per refresh
 * with a small reserve so a late arrival becomes a queue dip rather than a
 * visible repeat, and drops to the newest only when genuinely behind.
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include "decoder.h"
}

struct AVFrame;

namespace gnx::stream {

struct PipelineConfig {
	enum decoder_threading threading = DECODER_THREADS_FRAME2;
	/* Decoded frames kept in hand before presenting. 0 = present as soon
	 * as one exists; 1 = one refresh of latency, absorbs ~16 ms of
	 * arrival/decode jitter. Measured on the Wreckfest recording
	 * (2026-09-04): with frame threading, 0 gave FEWER holds and skips
	 * than 1 (154/12 vs 165/22), because the frame-threaded decoder
	 * already holds one frame in its pipeline; an explicit reserve on
	 * top only adds latency. Single-threaded decode wants 1. */
	int reserve = 0;
	/* Kept for the command line; the pick policy now derives its
	 * thresholds from `reserve` (see VideoPipeline::pick). */
	int drop_at = 4;
	/* -skiploop: how much of H.264's in-loop deblocking to skip. Costs
	 * conformance -- later frames predict from the filtered picture, so
	 * error accumulates to the next IDR -- and buys decode time, which is
	 * the scarcest thing on this SoC. See docs/PERFORMANCE.md. */
	enum decoder_skip_loop skip_loop = DECODER_LOOP_ALL_FRAMES;
	/* -lowres: decode at 1/2 or 1/4 size if the build supports it. */
	int lowres = 0;
};

class VideoPipeline {
public:
	/* Access-unit source, called on the decode thread. False when nothing
	 * is ready; the thread then waits for notify() or a short timeout.
	 * arrival_ms is when the unit's last packet arrived (CLOCK_MONOTONIC
	 * milliseconds); it rides through the decoder as the frame's pts so
	 * the presenter can measure arrival-to-flip latency. */
	using TakeAu = std::function<bool(std::vector<uint8_t> &, uint64_t *arrival_ms)>;

	~VideoPipeline();

	bool start(const PipelineConfig &config, TakeAu take);
	void stop();
	/* An access unit became available: wake the decoder immediately
	 * instead of on its idle timeout. */
	void notify();

	/* True once any frame has been decoded. */
	bool have_frame() const { return frames_decoded_.load() > 0; }
	/* The decoder could not be opened; nothing will ever be decoded. */
	bool failed() const { return failed_.load(); }
	/* No more input is coming: present without a reserve from now on. */
	void flush() { flushing_ = true; }
	size_t queued() const;

	/* Present thread, once per refresh. Returns a frame to show (caller
	 * owns it: av_frame_free when done) or nullptr to hold the current
	 * picture. Applies the reserve and drop policy and counts what it
	 * did. */
	AVFrame *pick();

	/* Everything the pace| line reports, reset on every snapshot() so each
	 * line covers one interval.
	 *
	 * dec_sum/dec_max time the send+receive call, not the decode. With
	 * slice threading they are the same thing (the call blocks for the
	 * whole decode: 11-12 ms measured). With frame threading the call
	 * returns as soon as a worker accepts the unit, so they read as a
	 * millisecond or less -- and a spike there means BOTH workers were
	 * busy, i.e. real backpressure. The decoder's CPU shows up in the
	 * "dec" column of the cpu figures instead. */
	struct Stats {
		uint32_t taken = 0, decoded = 0;
		double dec_sum = 0, dec_max = 0;
		uint32_t keyframes = 0;
		double idr_dec_max = 0;
		int slices_min = 99, slices_max = 0;
		uint32_t queue_drops = 0;   /* decoded frames dropped as too old */
		uint32_t picked = 0, held = 0, skipped = 0;
		uint32_t hold_hist[4] = {0, 0, 0, 0}; /* refreshes a frame stayed: 1,2,3,4+ */
		size_t qmax = 0;
		uint32_t decode_errors = 0;
		uint32_t au_queue_max = 0;  /* deepest the source queue got */
	};
	Stats snapshot();

	const PipelineConfig &config() const { return config_; }

private:
	void decode_loop();

	PipelineConfig config_;
	TakeAu take_;
	std::thread thread_;
	std::atomic<bool> quit_{false};
	std::atomic<bool> flushing_{false};
	std::atomic<bool> failed_{false};
	std::atomic<uint32_t> frames_decoded_{0};

	struct Queued {
		AVFrame *frame;
		double ready_ms;  /* when it came out of the decoder */
	};
	mutable std::mutex mutex_;       /* frames_ + stats_ + wake_gen_ */
	std::condition_variable cv_;     /* decode thread wakes on notify() */
	uint64_t wake_gen_ = 0;          /* bumped by notify(), under mutex_ */
	std::deque<Queued> frames_;
	Stats stats_;
	uint32_t hold_refreshes_ = 0;    /* present thread only */
};

}  // namespace gnx::stream
