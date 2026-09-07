#include "video_pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <pthread.h>

extern "C" {
#include <libavutil/frame.h>
}

namespace gnx::stream {

namespace {

double now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

constexpr double kPeriodMs = 16.667;  /* the panel: 60 Hz, nominally */

/*
 * Slice count and IDR presence. Emulation prevention guarantees 00 00 01
 * never occurs inside a NAL payload, so a plain scan is exact. The slice
 * count decides whether slice threading can do anything at all; xCloud
 * sends one slice per frame, so it cannot.
 */
void au_info(const uint8_t *p, size_t n, int *vcl, bool *idr)
{
	*vcl = 0;
	*idr = false;
	for (size_t k = 0; k + 3 < n; k++) {
		if (p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 1) {
			uint8_t t = p[k + 3] & 0x1f;
			if (t == 1 || t == 5)
				(*vcl)++;
			if (t == 5)
				*idr = true;
			k += 3;
		}
	}
}

}  // namespace

VideoPipeline::~VideoPipeline() { stop(); }

bool VideoPipeline::start(const PipelineConfig &config, TakeAu take)
{
	stop();
	config_ = config;
	take_ = std::move(take);
	quit_ = false;
	flushing_ = false;
	failed_ = false;
	frames_decoded_ = 0;
	hold_refreshes_ = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stats_ = Stats();
		wake_gen_ = 0;
	}
	thread_ = std::thread(&VideoPipeline::decode_loop, this);
	return true;
}

void VideoPipeline::stop()
{
	quit_ = true;
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	std::lock_guard<std::mutex> lock(mutex_);
	for (Queued &q : frames_)
		av_frame_free(&q.frame);
	frames_.clear();
}

void VideoPipeline::notify()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		wake_gen_++;
	}
	cv_.notify_one();
}

size_t VideoPipeline::queued() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return frames_.size();
}

void VideoPipeline::decode_loop()
{
	pthread_setname_np(pthread_self(), "xc-decode");

	struct decoder dec;
	bool open = false;
	std::vector<uint8_t> au;
	uint64_t arrival = 0;

	while (!quit_) {
		/* Read the wake generation BEFORE asking for a unit, so a
		 * notify() that lands between the empty answer and the wait
		 * is seen rather than lost. */
		uint64_t gen;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			gen = wake_gen_;
		}
		if (!take_(au, &arrival)) {
			std::unique_lock<std::mutex> lock(mutex_);
			if (wake_gen_ == gen && !quit_)
				cv_.wait_for(lock, std::chrono::milliseconds(4));
			continue;
		}
		if (!open) {
			if (decoder_open_h264_opts(&dec, config_.threading,
						   config_.skip_loop,
						   config_.lowres)) {
				std::fprintf(stderr, "pipeline: decoder open failed\n");
				failed_ = true;
				return;
			}
			open = true;
		}

		int vcl;
		bool idr;
		au_info(au.data(), au.size(), &vcl, &idr);

		double t0 = now_ms();
		int rc;
		int got = 0;
		/* Send, then take every frame the decoder has ready. With frame
		 * threading the frame that comes out belongs to an EARLIER
		 * unit; that is the pipelining, not a bug. EAGAIN on send means
		 * output is pending: drain it and resend the same unit, never
		 * drop it. */
		for (;;) {
			rc = decoder_send(&dec, au.data(), au.size(),
					  (int64_t)arrival);
			while (decoder_receive(&dec)) {
				AVFrame *f = av_frame_alloc();
				if (f && av_frame_ref(f, dec.frame) == 0) {
					std::lock_guard<std::mutex> lock(mutex_);
					frames_.push_back({f, now_ms()});
					got++;
				} else if (f) {
					av_frame_free(&f);
				}
			}
			if (rc != AVERROR(EAGAIN))
				break;
		}
		double dt = now_ms() - t0;

		if (got)
			frames_decoded_.fetch_add(got, std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stats_.taken++;
			stats_.decoded += got;
			stats_.dec_sum += dt;
			if (dt > stats_.dec_max)
				stats_.dec_max = dt;
			if (idr) {
				stats_.keyframes++;
				if (dt > stats_.idr_dec_max)
					stats_.idr_dec_max = dt;
			}
			if (vcl < stats_.slices_min)
				stats_.slices_min = vcl;
			if (vcl > stats_.slices_max)
				stats_.slices_max = vcl;
			if (rc < 0 && rc != AVERROR(EAGAIN))
				stats_.decode_errors++;
			if (frames_.size() > stats_.qmax)
				stats_.qmax = frames_.size();
		}
	}
	if (open)
		decoder_close(&dec);
}

AVFrame *VideoPipeline::pick()
{
	std::lock_guard<std::mutex> lock(mutex_);
	const int steady = 1 + (flushing_ ? 0 : config_.reserve);
	const double now = now_ms();

	auto drop_front = [&]() {
		av_frame_free(&frames_.front().frame);
		frames_.pop_front();
		stats_.queue_drops++;
		stats_.skipped++;
	};

	/*
	 * Behind. Two very different causes, two responses:
	 *
	 * A stall that released a whole burst at once (a Wi-Fi dropout, a
	 * reconnect) shows up as a deep queue. Drop straight to the reserve
	 * depth: the picture jumps once, and latency matters more than the
	 * frames in between. Eight frames deep is a stall; anything
	 * shallower is left to the age rule below.
	 *
	 * Everything else -- a keyframe that took 41 ms to decode (measured,
	 * live), or the source clock running ahead of the panel by one frame
	 * every few seconds -- is handled by AGE: skip exactly one frame per
	 * refresh while the oldest has waited longer than two refreshes
	 * beyond the reserve. After a decode spike that plays the backlog at
	 * double speed for a few refreshes, which is far less jarring than a
	 * jump; for drift it is a single 16 ms step. Age, not count: a count
	 * straddling two arrivals flickers, an age does not.
	 */
	if ((int)frames_.size() >= steady + 8) {
		while ((int)frames_.size() > steady)
			drop_front();
	} else if ((int)frames_.size() > steady) {
		/* Two refreshes of slack beyond the reserve, not one: the
		 * decoder often releases two frames between ticks after a
		 * brief stall, and with one refresh of slack (4 ms margin)
		 * that ordinary jitter read as "behind" -- 47 skips per run
		 * against 12. A frame that has waited two whole refreshes is
		 * genuinely surplus. */
		double age = now - frames_.front().ready_ms;
		if (age > (config_.reserve + 2) * kPeriodMs)
			drop_front();
	}

	if ((int)frames_.size() < steady) {
		if (hold_refreshes_)
			hold_refreshes_++;
		stats_.held++;
		return nullptr;
	}

	AVFrame *f = frames_.front().frame;
	frames_.pop_front();
	stats_.picked++;
	if (hold_refreshes_) {
		uint32_t h = hold_refreshes_ > 4 ? 4 : hold_refreshes_;
		stats_.hold_hist[h - 1]++;
	}
	hold_refreshes_ = 1;
	return f;
}

VideoPipeline::Stats VideoPipeline::snapshot()
{
	std::lock_guard<std::mutex> lock(mutex_);
	Stats s = stats_;
	stats_ = Stats();
	return s;
}

}  // namespace gnx::stream
