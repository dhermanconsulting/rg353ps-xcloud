#pragma once

/*
 * Opus 48 kHz stereo -> ALSA.
 *
 * green-nx's AudioPlayer does not transplant: it feeds libnx's audout directly
 * and carries a clock-skew servo resampler built around that device's
 * behaviour. Only its reorder buffer is portable, and that is
 * media/audio_jitter.hpp.
 *
 * Shape here:
 *   submit()  network thread. Copies the packet into an inbox and returns.
 *   thread    reorders, decodes with libavcodec into a PCM ring, and feeds
 *             ALSA only as much as it will take without blocking. The sound
 *             card's own buffer is the jitter buffer; the ring is what did
 *             not fit yet.
 *
 * What the first version got wrong, measured on the device
 * (docs/KNOWN-ISSUES.md issue 2):
 *   - It handed a whole batch of decoded packets to ONE blocking
 *     snd_pcm_writei. A 200 ms write into a 69 ms buffer blocks ~200 ms,
 *     ten more packets arrive meanwhile, and the batch size is
 *     self-perpetuating: every backlog depth is a stable state. Audio
 *     played ~250-300 ms late.
 *   - It shed ENCODED packets when the backlog grew. Each shed punched a
 *     sequence gap, the reorder buffer waited 32 packets for it, output
 *     starved, ALSA underran, then a 600 ms burst refilled the backlog
 *     past the shedding threshold again. lost == dropped_ms/20, exactly.
 *
 * So: never block for more than the card asks (write min(avail, ring)),
 * never shed encoded data (trim decoded PCM instead), bound the reorder
 * wait by time, and correct clock drift by dropping or repeating single
 * samples rather than whole frames.
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "audio_jitter.hpp"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_mixer snd_mixer_t;
typedef struct _snd_mixer_elem snd_mixer_elem_t;

namespace gnx::stream {

class AudioPlayer {
public:
	~AudioPlayer();

	/* False if ALSA or the Opus decoder could not be opened. A stream
	 * without audio is still worth watching, so callers treat this as a
	 * warning rather than a failure. */
	bool init();
	void shutdown();

	/* One Opus RTP payload, tagged with its RTP sequence. Returns
	 * immediately; safe from the network thread. */
	void submit(uint16_t seq, const uint8_t *data, size_t size);

	/* Forget the current stream without closing the device. A mid-stream
	 * reconnect brings a new RTP source whose random starting sequence the
	 * reorder buffer would otherwise reject wholesale, and whose Opus
	 * state shares nothing with the old one's. */
	void resync();

	struct Stats {
		uint32_t received = 0;   /* packets handed to submit() */
		uint32_t played = 0;     /* packets decoded and written */
		uint32_t failed = 0;     /* decode failures */
		uint32_t lost = 0;       /* gaps the reorder buffer skipped */
		uint32_t underruns = 0;  /* ALSA ran dry */
		uint32_t dropped_ms = 0; /* PCM trimmed to bound latency */
		uint32_t queue_ms = 0;   /* inbox + ring: sound not yet in ALSA */
		uint32_t ring_ms = 0;    /* decoded PCM waiting for room */
		int32_t servo_samples = 0; /* drift correction: +dropped/-repeated */
		/* Diagnostics, reset on every stats() call. */
		uint32_t inbox_max = 0;        /* deepest the inbox got */
		uint32_t reorder_waits = 0;    /* loops that ended waiting on a gap */
		uint32_t write_block_max_ms = 0; /* longest single snd_pcm_writei */
		uint32_t delay_min_ms = 0;     /* ALSA queue depth before writes */
		uint32_t delay_max_ms = 0;
	};
	Stats stats() const;

private:
	void thread_main();
	bool open_alsa();
	bool open_pcm(const char *device);
	bool open_mixer();
	void poll_mixer();
	bool open_decoder();
	/* Decode one Opus packet into interleaved S16 stereo, appended to
	 * ring_. Returns false if the packet was undecodable. */
	bool decode(const std::vector<uint8_t> &packet);
	/* Fill in `packets` lost 20 ms frames: libopus packet-loss
	 * concealment when the library is present, silence otherwise. */
	void conceal(int packets);
	bool open_opus();
	void close_opus();
	/* Feed ALSA what it will take without blocking. Returns false if the
	 * device is in a state no write can fix right now. */
	bool feed();
	bool recover(long err);
	size_t ring_frames() const { return (ring_.size() - ring_head_) / 2; }
	void ring_consume(size_t frames);

	snd_pcm_t *pcm_handle_ = nullptr;
	AVCodecContext *ctx_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *pkt_ = nullptr;
	/*
	 * libopus, loaded at runtime if the device has it (libavcodec on the
	 * device links it, so it is normally there; the FFmpeg decoder stays
	 * as the fallback). Its decoder does packet-loss concealment: a
	 * missing 20 ms frame is extrapolated from the last one instead of
	 * being a hole, which on the handheld's Wi-Fi (250 lost packets in
	 * one race) is the difference between a click and nothing audible.
	 * Prototypes declared here so no opus headers are needed to build.
	 */
	void *opus_lib_ = nullptr;
	void *opus_dec_ = nullptr;
	void *(*opus_create_)(int32_t, int, int *) = nullptr;
	int (*opus_decode_)(void *, const unsigned char *, int32_t, int16_t *,
			    int, int) = nullptr;
	void (*opus_destroy_)(void *) = nullptr;
	unsigned long alsa_buffer_ = 0;  /* frames, as granted */
	unsigned long alsa_period_ = 0;
	/* Direct path: the codec opened at 48 kHz, bypassing the asound.conf
	 * chain, so the volume keys' softvol gain has to be applied here. */
	bool direct_ = false;
	snd_mixer_t *mixer_ = nullptr;
	snd_mixer_elem_t *master_ = nullptr;
	int32_t gain_q15_ = 32768;       /* audio thread only */
	long last_db_ = 0;
	uint64_t last_mixer_poll_ms_ = 0;

	AudioJitterBuffer reorder_;      /* audio thread only */
	std::vector<int16_t> ring_;      /* decoded PCM, audio thread only */
	size_t ring_head_ = 0;           /* samples already consumed */
	double latency_lp_ = 0;          /* low-passed ring+ALSA depth, ms */
	int servo_phase_ = 0;            /* samples until the next adjustment */

	std::thread thread_;
	std::atomic<bool> quit_{false};
	std::atomic<bool> resync_requested_{false};
	std::atomic<bool> running_{false};

	mutable std::mutex inbox_mutex_;
	std::condition_variable cv_;
	struct InPacket {
		uint16_t seq;
		uint64_t arrived_ms;
		std::vector<uint8_t> data;
	};
	std::deque<InPacket> inbox_;

	std::atomic<uint32_t> received_{0};
	std::atomic<uint32_t> played_{0};
	std::atomic<uint32_t> failed_{0};
	std::atomic<uint32_t> underruns_{0};
	std::atomic<uint32_t> dropped_ms_{0};
	std::atomic<uint32_t> ring_ms_{0};
	std::atomic<int32_t> servo_samples_{0};
	/* Interval counters, reset by the const stats() getter. */
	mutable std::atomic<uint32_t> inbox_max_{0};
	mutable std::atomic<uint32_t> reorder_waits_{0};
	mutable std::atomic<uint32_t> write_block_max_ms_{0};
	mutable std::atomic<uint32_t> delay_min_ms_{~0u};
	mutable std::atomic<uint32_t> delay_max_ms_{0};
};

}  // namespace gnx::stream
