#include "audio_player.hpp"

#include <alsa/asoundlib.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace gnx::stream {

namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
/* xCloud sends one 20 ms Opus frame per RTP packet. */
constexpr int kFrameMs = 20;
constexpr int kFramesPerPacket = kRate / 1000 * kFrameMs;  /* 960 per channel */

/*
 * What we ask ALSA for. Through the device's plug -> softvol -> dmix chain
 * the slave is fixed at 44.1 kHz with a 4096-frame buffer, so these are
 * requests and the granted values are logged at open. A 200 ms request
 * simply gets the most the chain allows (about 93 ms).
 */
constexpr unsigned kBufferUs = 200000;
constexpr unsigned kPeriodUs = 20000;

/*
 * Latency targets, in ms of sound queued (ring + ALSA).
 *
 * The card starts once two periods (~46 ms) are queued and we top it up as
 * every 20 ms packet arrives, so steady state sits around 40-70 ms. Above
 * kMaxLatencyMs something stalled and released a burst: trim decoded PCM
 * back to kTrimToMs in whole packets, which is instant and does not touch
 * the Opus decoder's state. Between those, a slow drift between the Xbox's
 * 48 kHz clock and the RK817's is corrected one sample at a time.
 */
/*
 * Depth budget on a lossy link: the last top-up is one packet (20 ms) before
 * a loss, the loss is declared when the next packet arrives (+20 ms) plus
 * the reorder grace and a loop tick (~10 ms), so the card drains ~50 ms
 * before the silence fill lands. The floor of the servo band therefore
 * sits at 45 ms and the trim target at 60; a burst longer than the fill
 * cap still underruns, and that is what the re-prime is for.
 */
constexpr double kMaxLatencyMs = 120.0;
constexpr double kTrimToMs = 60.0;
constexpr double kServoHighMs = 75.0;  /* above: drop a sample now and then */
constexpr double kServoLowMs = 45.0;   /* below: repeat one */
constexpr int kServoEveryFrames = 960; /* +-0.1%: 20x faster than any drift */

/* Inbox cap: a wedged audio thread must not grow memory without bound on a
 * 1 GB device. At 4 s of audio it is a diagnostic, not a policy. */
constexpr size_t kMaxInbox = 200;

uint64_t now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void note_max(std::atomic<uint32_t> &slot, uint32_t v)
{
	uint32_t cur = slot.load(std::memory_order_relaxed);
	while (v > cur && !slot.compare_exchange_weak(cur, v))
		;
}

void note_min(std::atomic<uint32_t> &slot, uint32_t v)
{
	uint32_t cur = slot.load(std::memory_order_relaxed);
	while (v < cur && !slot.compare_exchange_weak(cur, v))
		;
}

}  // namespace

AudioPlayer::~AudioPlayer() { shutdown(); }

/*
 * Why two devices. The stock asound.conf routes "default" through plug ->
 * softvol -> dmix with the slave pinned at 44.1 kHz, and the RK3566 cannot
 * make that clock family exactly: measured on the Wreckfest replay, the DAC
 * ran about 0.14% slow against the stream, the servo could not hold it, and
 * every write went through a resampler and a 23 ms period quantiser.
 * "plughw:0,0" at 48 kHz gets the exact 24.576 MHz-derived clock, no
 * resampling and the buffer we ask for. It bypasses softvol, which is what
 * the volume keys drive, so the mixer is read and its gain applied here.
 * "default" stays as the fallback for when something else holds the card.
 */
bool AudioPlayer::open_alsa()
{
	/* Host simulator (scripts/sim.sh): XCLOUD_ALSA_DEVICE names the ONE
	 * device to open -- "null" there, which takes any format and never
	 * blocks -- with no fallback and no mixer, since neither the RK817 nor
	 * the softvol chain exists. Unset on the device, where the path below
	 * is unchanged. */
	if (const char *dev = std::getenv("XCLOUD_ALSA_DEVICE"); dev && *dev)
		return open_pcm(dev);

	direct_ = open_pcm("plughw:0,0");
	if (direct_) {
		if (!open_mixer())
			std::fprintf(stderr, "audio: no Master mixer control; "
					     "volume keys will not apply\n");
		return true;
	}
	std::fprintf(stderr, "audio: direct device busy or unavailable, "
			     "using default (dmix at 44.1 kHz)\n");
	return open_pcm("default");
}

bool AudioPlayer::open_mixer()
{
	const char *step = "snd_mixer_open";
	int rc = snd_mixer_open(&mixer_, 0);
	if (rc >= 0) {
		step = "snd_mixer_attach hw:0";
		rc = snd_mixer_attach(mixer_, "hw:0");
	}
	if (rc >= 0) {
		step = "snd_mixer_selem_register";
		rc = snd_mixer_selem_register(mixer_, nullptr, nullptr);
	}
	if (rc >= 0) {
		step = "snd_mixer_load";
		rc = snd_mixer_load(mixer_);
	}
	if (rc < 0) {
		std::fprintf(stderr, "audio: mixer: %s: %s\n", step,
			     snd_strerror(rc));
		if (mixer_)
			snd_mixer_close(mixer_);
		mixer_ = nullptr;
		return false;
	}
	snd_mixer_selem_id_t *sid;
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, "Master");
	master_ = snd_mixer_find_selem(mixer_, sid);
	if (!master_) {
		std::fprintf(stderr, "audio: mixer: no 'Master' element\n");
		snd_mixer_close(mixer_);
		mixer_ = nullptr;
		return false;
	}
	last_db_ = 1;  /* force the first read to log */
	poll_mixer();
	return true;
}

/* Read the softvol "Master" level the volume keys set and turn it into a
 * Q15 linear gain. The control is a dB scale (-51 dB .. 0 dB in 0.2 dB
 * steps on this device); the mixer API resolves that from the TLV so the
 * numbers are not hard-coded here, with a raw-percentage fallback if the
 * dB query is refused. */
void AudioPlayer::poll_mixer()
{
	if (!mixer_ || !master_)
		return;
	snd_mixer_handle_events(mixer_);
	long db = 0;
	int rc = snd_mixer_selem_get_playback_dB(master_,
						 SND_MIXER_SCHN_FRONT_LEFT, &db);
	if (rc < 0) {
		long v = 0, lo = 0, hi = 0;
		if (snd_mixer_selem_get_playback_volume(
			    master_, SND_MIXER_SCHN_FRONT_LEFT, &v) < 0 ||
		    snd_mixer_selem_get_playback_volume_range(master_, &lo,
							      &hi) < 0 ||
		    hi <= lo) {
			if (last_db_ == 1)
				std::fprintf(stderr, "audio: mixer: cannot read "
						     "Master: %s\n",
					     snd_strerror(rc));
			last_db_ = 0;
			return;
		}
		/* Same shape as the device's TLV: -51 dB at the bottom,
		 * 0 dB at the top, linear in dB between. */
		db = (long)(-5100.0 + 5100.0 * (double)(v - lo) / (double)(hi - lo));
	}
	if (db == last_db_)
		return;
	last_db_ = db;
	double lin = db >= 0 ? 1.0 : std::pow(10.0, db / 2000.0);
	gain_q15_ = (int32_t)(lin * 32768.0 + 0.5);
	std::fprintf(stderr, "audio: volume %.1f dB (gain %.3f)\n", db / 100.0,
		     lin);
}

bool AudioPlayer::open_pcm(const char *device)
{
	int rc = snd_pcm_open(&pcm_handle_, device, SND_PCM_STREAM_PLAYBACK, 0);

	if (rc < 0) {
		std::fprintf(stderr, "audio: snd_pcm_open %s: %s\n", device,
			     snd_strerror(rc));
		pcm_handle_ = nullptr;
		return false;
	}

	/*
	 * Explicit hw/sw params rather than snd_pcm_set_params: that helper
	 * sets start_threshold to the whole buffer, so playback could not
	 * begin until ~93 ms had been written, and it gives no say over
	 * avail_min. Everything here is a request the plug/dmix chain rounds
	 * to what its fixed slave allows.
	 */
	snd_pcm_hw_params_t *hw;
	snd_pcm_hw_params_alloca(&hw);
	const char *step = "hw_params_any";
	rc = snd_pcm_hw_params_any(pcm_handle_, hw);
	if (rc >= 0) {
		step = "set_rate_resample";
		rc = snd_pcm_hw_params_set_rate_resample(pcm_handle_, hw, 1);
	}
	if (rc >= 0) {
		step = "set_access";
		rc = snd_pcm_hw_params_set_access(pcm_handle_, hw,
						  SND_PCM_ACCESS_RW_INTERLEAVED);
	}
	if (rc >= 0) {
		step = "set_format";
		rc = snd_pcm_hw_params_set_format(pcm_handle_, hw,
						  SND_PCM_FORMAT_S16_LE);
	}
	if (rc >= 0) {
		step = "set_channels";
		rc = snd_pcm_hw_params_set_channels(pcm_handle_, hw, kChannels);
	}
	if (rc >= 0) {
		unsigned rate = kRate;
		step = "set_rate_near";
		rc = snd_pcm_hw_params_set_rate_near(pcm_handle_, hw, &rate, nullptr);
		if (rc >= 0 && rate != (unsigned)kRate)
			std::fprintf(stderr, "audio: rate %u granted for %d\n",
				     rate, kRate);
	}
	if (rc >= 0) {
		unsigned us = kBufferUs;
		step = "set_buffer_time_near";
		rc = snd_pcm_hw_params_set_buffer_time_near(pcm_handle_, hw, &us,
							    nullptr);
	}
	if (rc >= 0) {
		unsigned us = kPeriodUs;
		step = "set_period_time_near";
		rc = snd_pcm_hw_params_set_period_time_near(pcm_handle_, hw, &us,
							    nullptr);
	}
	if (rc >= 0) {
		step = "hw_params";
		rc = snd_pcm_hw_params(pcm_handle_, hw);
	}
	if (rc < 0) {
		std::fprintf(stderr, "audio: %s: %s\n", step, snd_strerror(rc));
		snd_pcm_close(pcm_handle_);
		pcm_handle_ = nullptr;
		return false;
	}
	snd_pcm_uframes_t buffer = 0, period = 0;
	snd_pcm_hw_params_get_buffer_size(hw, &buffer);
	snd_pcm_hw_params_get_period_size(hw, &period, nullptr);
	alsa_buffer_ = buffer;
	alsa_period_ = period;

	/*
	 * Start once 60 ms is queued (three 20 ms packets), stop only on a
	 * real underrun, wake for one period of room. No explicit priming
	 * and no silence: the start threshold IS the priming, and after an
	 * underrun snd_pcm_prepare plus the next writes restart the same
	 * way, without adding latency that a trim would then have to take
	 * back. 60 ms and not two periods: with 20 ms periods a 40 ms start
	 * left the level swinging 20-40 ms, and one packet 20 ms late (an
	 * ordinary Wi-Fi hiccup) underran it.
	 */
	snd_pcm_uframes_t start_at = (snd_pcm_uframes_t)kRate * 60 / 1000;
	if (start_at < period * 2)
		start_at = period * 2;
	if (start_at > buffer / 2)
		start_at = buffer / 2;
	snd_pcm_sw_params_t *sw;
	snd_pcm_sw_params_alloca(&sw);
	step = "sw_params_current";
	rc = snd_pcm_sw_params_current(pcm_handle_, sw);
	if (rc >= 0) {
		step = "set_start_threshold";
		rc = snd_pcm_sw_params_set_start_threshold(pcm_handle_, sw,
							   start_at);
	}
	if (rc >= 0) {
		step = "set_stop_threshold";
		rc = snd_pcm_sw_params_set_stop_threshold(pcm_handle_, sw, buffer);
	}
	if (rc >= 0) {
		step = "set_avail_min";
		rc = snd_pcm_sw_params_set_avail_min(pcm_handle_, sw, period);
	}
	if (rc >= 0) {
		step = "sw_params";
		rc = snd_pcm_sw_params(pcm_handle_, sw);
	}
	if (rc < 0) {
		std::fprintf(stderr, "audio: %s: %s\n", step, snd_strerror(rc));
		snd_pcm_close(pcm_handle_);
		pcm_handle_ = nullptr;
		return false;
	}
	std::fprintf(stderr, "audio: %s: buffer=%lu frames (%lu ms) "
			     "period=%lu frames (%lu ms) start=%lu frames\n",
		     device, (unsigned long)buffer,
		     (unsigned long)(buffer * 1000 / kRate),
		     (unsigned long)period, (unsigned long)(period * 1000 / kRate),
		     (unsigned long)start_at);
	return true;
}

bool AudioPlayer::open_decoder()
{
	/* libavcodec's native Opus decoder, so libopus is not a dependency.
	 * It reports FLTP; the libopus wrapper reports S16. decode() handles
	 * both rather than assuming. */
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);

	if (!codec) {
		std::fprintf(stderr, "audio: no Opus decoder in libavcodec\n");
		return false;
	}
	ctx_ = avcodec_alloc_context3(codec);
	if (!ctx_)
		return false;
	ctx_->sample_rate = kRate;
	ctx_->channels = kChannels;
	ctx_->channel_layout = AV_CH_LAYOUT_STEREO;
	ctx_->request_sample_fmt = AV_SAMPLE_FMT_S16;
	if (avcodec_open2(ctx_, codec, nullptr) < 0) {
		std::fprintf(stderr, "audio: avcodec_open2 (opus) failed\n");
		avcodec_free_context(&ctx_);
		return false;
	}
	frame_ = av_frame_alloc();
	pkt_ = av_packet_alloc();
	if (!frame_ || !pkt_)
		return false;
	std::fprintf(stderr, "audio: %s, %d Hz stereo\n", codec->name, kRate);
	return true;
}

bool AudioPlayer::init()
{
	shutdown();
	quit_ = false;
	resync_requested_ = false;
	reorder_.reset();
	ring_.clear();
	ring_head_ = 0;
	latency_lp_ = 0;
	servo_phase_ = 0;
	received_ = played_ = failed_ = underruns_ = dropped_ms_ = 0;
	ring_ms_ = 0;
	servo_samples_ = 0;
	{
		std::lock_guard<std::mutex> lock(inbox_mutex_);
		inbox_.clear();
	}
	if (!open_alsa())
		return false;
	if (!open_decoder()) {
		snd_pcm_close(pcm_handle_);
		pcm_handle_ = nullptr;
		return false;
	}
	/* Preferred decoder if the library is there; the FFmpeg one above
	 * is still opened so a missing libopus changes nothing. */
	if (!open_opus())
		std::fprintf(stderr, "audio: no libopus, lost packets become silence\n");
	running_ = true;
	thread_ = std::thread(&AudioPlayer::thread_main, this);
	return true;
}

void AudioPlayer::shutdown()
{
	quit_ = true;
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	running_ = false;
	close_opus();
	if (frame_)
		av_frame_free(&frame_);
	if (pkt_)
		av_packet_free(&pkt_);
	if (ctx_)
		avcodec_free_context(&ctx_);
	if (pcm_handle_) {
		snd_pcm_drop(pcm_handle_);
		snd_pcm_close(pcm_handle_);
		pcm_handle_ = nullptr;
	}
	if (mixer_) {
		snd_mixer_close(mixer_);
		mixer_ = nullptr;
		master_ = nullptr;
	}
	direct_ = false;
	gain_q15_ = 32768;
	{
		std::lock_guard<std::mutex> lock(inbox_mutex_);
		inbox_.clear();
	}
}

void AudioPlayer::submit(uint16_t seq, const uint8_t *data, size_t size)
{
	if (!running_ || size == 0)
		return;
	received_.fetch_add(1, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(inbox_mutex_);
		if (inbox_.size() >= kMaxInbox)
			inbox_.pop_front();
		inbox_.push_back({seq, now_ms(),
				  std::vector<uint8_t>(data, data + size)});
	}
	cv_.notify_one();
}

void AudioPlayer::resync() { resync_requested_ = true; }

bool AudioPlayer::open_opus()
{
	opus_lib_ = dlopen("libopus.so.0", RTLD_NOW | RTLD_LOCAL);
	if (!opus_lib_)
		return false;
	opus_create_ = (void *(*)(int32_t, int, int *))dlsym(opus_lib_,
							    "opus_decoder_create");
	opus_decode_ = (int (*)(void *, const unsigned char *, int32_t, int16_t *,
				int, int))dlsym(opus_lib_, "opus_decode");
	opus_destroy_ = (void (*)(void *))dlsym(opus_lib_, "opus_decoder_destroy");
	if (!opus_create_ || !opus_decode_ || !opus_destroy_) {
		close_opus();
		return false;
	}
	int err = 0;
	opus_dec_ = opus_create_(kRate, kChannels, &err);
	if (!opus_dec_ || err != 0) {
		opus_dec_ = nullptr;
		close_opus();
		return false;
	}
	std::fprintf(stderr, "audio: libopus decoder with loss concealment\n");
	return true;
}

void AudioPlayer::close_opus()
{
	if (opus_dec_ && opus_destroy_)
		opus_destroy_(opus_dec_);
	opus_dec_ = nullptr;
	if (opus_lib_)
		dlclose(opus_lib_);
	opus_lib_ = nullptr;
	opus_create_ = nullptr;
	opus_decode_ = nullptr;
	opus_destroy_ = nullptr;
}

void AudioPlayer::conceal(int packets)
{
	for (int i = 0; i < packets; i++) {
		size_t at = ring_.size();
		ring_.resize(at + (size_t)kFramesPerPacket * kChannels, 0);
		if (!opus_dec_)
			continue;  /* silence */
		int16_t *out = ring_.data() + at;
		/* NULL data asks libopus for concealment of one frame. */
		int n = opus_decode_(opus_dec_, nullptr, 0, out, kFramesPerPacket, 0);
		if (n <= 0) {
			std::memset(out, 0, (size_t)kFramesPerPacket * kChannels *
						    sizeof(int16_t));
			continue;
		}
		if (n < kFramesPerPacket)
			ring_.resize(at + (size_t)n * kChannels);
		if (gain_q15_ != 32768)
			for (int k = 0; k < n * kChannels; k++)
				out[k] = (int16_t)((out[k] * gain_q15_) >> 15);
	}
}

bool AudioPlayer::decode(const std::vector<uint8_t> &packet)
{
	if (opus_dec_) {
		/* libopus decodes straight to interleaved S16 at 48 kHz; a
		 * packet may carry up to 120 ms, so leave room for that. */
		const int max_frames = kRate * 120 / 1000;
		size_t at = ring_.size();
		ring_.resize(at + (size_t)max_frames * kChannels);
		int16_t *out = ring_.data() + at;
		int n = opus_decode_(opus_dec_, packet.data(), (int32_t)packet.size(),
				     out, max_frames, 0);
		if (n <= 0) {
			ring_.resize(at);
			return false;
		}
		ring_.resize(at + (size_t)n * kChannels);
		if (gain_q15_ != 32768)
			for (int k = 0; k < n * kChannels; k++)
				out[k] = (int16_t)((out[k] * gain_q15_) >> 15);
		return true;
	}

	if (av_new_packet(pkt_, (int)packet.size()) < 0)
		return false;
	std::memcpy(pkt_->data, packet.data(), packet.size());
	int rc = avcodec_send_packet(ctx_, pkt_);
	av_packet_unref(pkt_);
	if (rc < 0)
		return false;

	bool got = false;
	while (avcodec_receive_frame(ctx_, frame_) == 0) {
		int n = frame_->nb_samples;
		int ch = frame_->channels > 0 ? frame_->channels : kChannels;
		size_t at = ring_.size();

		ring_.resize(at + (size_t)n * kChannels);
		int16_t *out = ring_.data() + at;

		/* Interleave to S16 stereo by hand rather than pulling in
		 * libswresample: the native decoder emits planar float, the
		 * libopus wrapper emits S16, and a mono stream has to be
		 * duplicated to both speakers. The volume gain rides along in
		 * the same pass (unity unless on the direct device). */
		const float g = gain_q15_ / 32768.0f;
		switch (frame_->format) {
		case AV_SAMPLE_FMT_FLTP: {
			const float *l = (const float *)frame_->data[0];
			const float *r = ch > 1 ? (const float *)frame_->data[1] : l;
			for (int i = 0; i < n; i++) {
				float a = l[i] * g;
				float b = r[i] * g;
				a = a > 1.0f ? 1.0f : (a < -1.0f ? -1.0f : a);
				b = b > 1.0f ? 1.0f : (b < -1.0f ? -1.0f : b);
				out[i * 2] = (int16_t)(a * 32767.0f);
				out[i * 2 + 1] = (int16_t)(b * 32767.0f);
			}
			break;
		}
		case AV_SAMPLE_FMT_S16P: {
			const int16_t *l = (const int16_t *)frame_->data[0];
			const int16_t *r = ch > 1 ? (const int16_t *)frame_->data[1] : l;
			for (int i = 0; i < n; i++) {
				out[i * 2] = (int16_t)((l[i] * gain_q15_) >> 15);
				out[i * 2 + 1] = (int16_t)((r[i] * gain_q15_) >> 15);
			}
			break;
		}
		case AV_SAMPLE_FMT_S16: {
			const int16_t *s = (const int16_t *)frame_->data[0];
			for (int i = 0; i < n; i++) {
				int16_t l = s[i * ch];
				int16_t r = ch > 1 ? s[i * ch + 1] : s[i * ch];
				out[i * 2] = (int16_t)((l * gain_q15_) >> 15);
				out[i * 2 + 1] = (int16_t)((r * gain_q15_) >> 15);
			}
			break;
		}
		default:
			ring_.resize(at);
			return false;
		}
		got = true;
	}
	return got;
}

void AudioPlayer::ring_consume(size_t frames)
{
	ring_head_ += frames * kChannels;
	if (ring_head_ >= ring_.size()) {
		ring_.clear();
		ring_head_ = 0;
	} else if (ring_head_ > 4096 * (size_t)kChannels) {
		/* Compact once the consumed prefix is worth the memmove. */
		ring_.erase(ring_.begin(), ring_.begin() + (long)ring_head_);
		ring_head_ = 0;
	}
}

bool AudioPlayer::recover(long err)
{
	if (err == -EPIPE) {
		/* Underrun: the card emptied while we were away. prepare()
		 * resets the pointers; the next writes restart it once the
		 * start threshold is met again, exactly like the first start. */
		underruns_.fetch_add(1, std::memory_order_relaxed);
		return snd_pcm_prepare(pcm_handle_) == 0;
	}
	if (err == -ESTRPIPE) {
		while (snd_pcm_resume(pcm_handle_) == -EAGAIN && !quit_)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return snd_pcm_prepare(pcm_handle_) == 0;
	}
	return snd_pcm_recover(pcm_handle_, (int)err, 1) == 0;
}

bool AudioPlayer::feed()
{
	/* snd_pcm_avail (not avail_update) syncs with the slave through dmix
	 * before answering, so the number is honest. It also reports an
	 * XRUN as -EPIPE, which is how we learn about one without writing. */
	snd_pcm_sframes_t avail = snd_pcm_avail(pcm_handle_);
	if (avail < 0) {
		if (!recover(avail))
			return false;
		avail = snd_pcm_avail(pcm_handle_);
		if (avail < 0)
			return false;
	}

	snd_pcm_sframes_t delay = 0;
	if (snd_pcm_delay(pcm_handle_, &delay) < 0 || delay < 0)
		delay = 0;
	double queued_ms = (ring_frames() + (size_t)delay) * 1000.0 / kRate;
	note_min(delay_min_ms_, (uint32_t)(delay * 1000 / kRate));
	note_max(delay_max_ms_, (uint32_t)(delay * 1000 / kRate));

	/*
	 * Latency control, on decoded PCM only.
	 *
	 * Burst (after a stall, or at start when the server front-loads):
	 * trim whole packets off the front of the ring. The sound skips
	 * forward once, which is far less audible than the alternative of
	 * being a quarter of a second late for the rest of the session.
	 */
	if (queued_ms > kMaxLatencyMs) {
		size_t excess = (size_t)((queued_ms - kTrimToMs) * kRate / 1000.0);
		size_t packets = excess / kFramesPerPacket;
		size_t drop = packets * kFramesPerPacket;
		if (drop > ring_frames())
			drop = ring_frames() / kFramesPerPacket * kFramesPerPacket;
		if (drop) {
			ring_consume(drop);
			dropped_ms_.fetch_add((uint32_t)(drop / (kRate / 1000)),
					      std::memory_order_relaxed);
			queued_ms -= drop * 1000.0 / kRate;
		}
	}
	/* Drift: a 30 s low-pass on the queued depth, then one dropped or
	 * repeated sample per kServoEveryFrames while it sits outside the
	 * band. +-0.1% is inaudible and twenty times any real clock skew. */
	latency_lp_ += (queued_ms - latency_lp_) * (1.0 / 1500.0);

	size_t want = ring_frames();
	if (want > (size_t)avail)
		want = (size_t)avail;
	if (!want)
		return true;

	/* Drift servo, applied to the data about to be written: the card
	 * usually has room for everything, so the ring is empty between
	 * calls and any adjustment made after the write would find nothing
	 * to adjust. Dropping or repeating the sample at the head of this
	 * write is the same correction, made a millisecond earlier. */
	if (latency_lp_ > kServoHighMs || latency_lp_ < kServoLowMs) {
		servo_phase_ += (int)want;
		if (servo_phase_ >= kServoEveryFrames && ring_frames() >= 2) {
			servo_phase_ = 0;
			if (latency_lp_ > kServoHighMs) {
				ring_consume(1);
				want--;
				servo_samples_.fetch_add(1, std::memory_order_relaxed);
			} else {
				/* Repeat the next sample: insert a copy of it
				 * ahead of itself. */
				size_t at = ring_head_;
				ring_.insert(ring_.begin() + (long)at,
					     {ring_[at], ring_[at + 1]});
				if ((size_t)avail > want)
					want++;
				servo_samples_.fetch_sub(1, std::memory_order_relaxed);
			}
		}
	} else {
		servo_phase_ = 0;
	}
	if (!want)
		return true;

	const int16_t *src = ring_.data() + ring_head_;
	auto t0 = std::chrono::steady_clock::now();
	snd_pcm_sframes_t wrote = snd_pcm_writei(pcm_handle_, src, want);
	uint32_t blocked = (uint32_t)std::chrono::duration_cast<
		std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
		.count();
	note_max(write_block_max_ms_, blocked);
	if (wrote < 0) {
		recover(wrote);
		return true;
	}
	ring_consume((size_t)wrote);
	return true;
}

void AudioPlayer::thread_main()
{
	std::vector<InPacket> batch;
	std::vector<uint8_t> packet;

	pthread_setname_np(pthread_self(), "xc-audio");
	/* Above the present thread (12): each wake here is a few hundred
	 * microseconds of work, and a dropout is more audible than one
	 * repeated frame is visible. */
	{
		struct sched_param sp;
		std::memset(&sp, 0, sizeof(sp));
		sp.sched_priority = 15;
		if (sched_setscheduler(0, SCHED_FIFO, &sp))
			std::fprintf(stderr, "audio: SCHED_FIFO refused: %s\n",
				     std::strerror(errno));
	}

	while (!quit_) {
		{
			std::unique_lock<std::mutex> lock(inbox_mutex_);
			/* Short wait: with the ring non-empty this is also the
			 * poll interval for room in the card. */
			cv_.wait_for(lock, std::chrono::milliseconds(5),
				     [&] { return quit_ || !inbox_.empty(); });
			if (quit_)
				break;
			note_max(inbox_max_, (uint32_t)inbox_.size());
			batch.assign(std::make_move_iterator(inbox_.begin()),
				     std::make_move_iterator(inbox_.end()));
			inbox_.clear();
		}

		if (resync_requested_.exchange(false)) {
			reorder_.reset();
			if (ctx_)
				avcodec_flush_buffers(ctx_);
			/* A fresh RTP source shares no Opus state with the old
			 * one: start libopus over too. */
			if (opus_dec_ && opus_create_) {
				int err = 0;
				opus_destroy_(opus_dec_);
				opus_dec_ = opus_create_(kRate, kChannels, &err);
				if (err != 0)
					opus_dec_ = nullptr;
			}
			if (pcm_handle_) {
				snd_pcm_drop(pcm_handle_);
				snd_pcm_prepare(pcm_handle_);
			}
			ring_.clear();
			ring_head_ = 0;
			latency_lp_ = 0;
			batch.clear();
			continue;
		}

		uint64_t now = now_ms();
		if (direct_ && now - last_mixer_poll_ms_ >= 250) {
			last_mixer_poll_ms_ = now;
			poll_mixer();
		}
		for (InPacket &p : batch)
			reorder_.push(p.seq, p.data.data(), p.data.size(),
				      p.arrived_ms);
		batch.clear();

		uint32_t lost_before = reorder_.lost();
		while (reorder_.pop(packet, now)) {
			/* A packet the reorder buffer gave up on is 20 ms of
			 * sound that will never come. Fill it with silence so the
			 * card keeps running and latency stays where it was;
			 * without this every loss became an underrun, a restart
			 * and a re-prime (28 underruns in one five-minute race). */
			uint32_t lost_now = reorder_.lost();
			if (lost_now > lost_before) {
				size_t frames = (size_t)(lost_now - lost_before) *
						kFramesPerPacket;
				/* Up to 200 ms: the bursts measured on the
				 * device were ~9 packets. Beyond that the card
				 * underruns anyway and the trim would only cut
				 * the silence back out. */
				if (frames > (size_t)kFramesPerPacket * 10)
					frames = (size_t)kFramesPerPacket * 10;
				conceal((int)(frames / kFramesPerPacket));
				lost_before = lost_now;
			}
			if (decode(packet))
				played_.fetch_add(1, std::memory_order_relaxed);
			else
				failed_.fetch_add(1, std::memory_order_relaxed);
		}
		/* Packets in hand but the next sequence number missing: the
		 * reorder buffer is holding sound back for a gap. */
		if (reorder_.pending() > 0)
			reorder_waits_.fetch_add(1, std::memory_order_relaxed);

		if (pcm_handle_)
			feed();
		ring_ms_.store((uint32_t)(ring_frames() * 1000 / kRate),
			       std::memory_order_relaxed);
	}
}

AudioPlayer::Stats AudioPlayer::stats() const
{
	Stats s;
	s.received = received_.load();
	s.played = played_.load();
	s.failed = failed_.load();
	s.lost = reorder_.lost();
	s.underruns = underruns_.load();
	s.dropped_ms = dropped_ms_.load();
	s.ring_ms = ring_ms_.load();
	s.servo_samples = servo_samples_.load();
	{
		std::lock_guard<std::mutex> lock(inbox_mutex_);
		s.queue_ms = (uint32_t)inbox_.size() * kFrameMs + s.ring_ms;
	}
	s.inbox_max = inbox_max_.exchange(0);
	s.reorder_waits = reorder_waits_.exchange(0);
	s.write_block_max_ms = write_block_max_ms_.exchange(0);
	uint32_t lo = delay_min_ms_.exchange(~0u);
	s.delay_min_ms = lo == ~0u ? 0 : lo;
	s.delay_max_ms = delay_max_ms_.exchange(0);
	return s;
}

}  // namespace gnx::stream
