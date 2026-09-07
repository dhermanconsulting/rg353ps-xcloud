#pragma once

/*
 * Native xCloud streaming session: GSSV signalling + libpeer WebRTC + data
 * channels, ported from green-nx's src/switch/stream/engine.cpp (GPL-3.0).
 *
 * What changed from green-nx, and why:
 *
 *  - No SDL. green-nx's only real SDL dependency here is the millisecond
 *    clock, so src/net/compat.h supplies SDL_GetTicks64 over CLOCK_MONOTONIC
 *    and every call site is left alone. That keeps this file diffable against
 *    upstream, which matters because most of its content is empirically
 *    derived xCloud behaviour that would be painful to rediscover.
 *
 *  - The engine does not own the decoder or the display. It reassembles
 *    access units into a queue; the main thread drains it with
 *    take_access_unit() and drives our own libavcodec decoder and DRM output.
 *    green-nx's decode thread and deko3d presentation path are gone with it.
 *
 *  - Audio is counted but not played yet. on_audio keeps the media watchdogs
 *    honest; ALSA output is a later milestone. The Switch AudioPlayer is an
 *    SDL2-audio design and does not transplant.
 *
 *  - No rumble actuation (nothing to actuate here), but server input-channel
 *    traffic is still counted: it is what the input-wedge detector reads.
 */

#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "compat.h"

#include "../gnx/auth.hpp"
#include "../gnx/catalog.hpp"
#include "../gnx/session.hpp"
#include "../gnx/xcloud_protocol.hpp"
#include "../media/au_recorder.hpp"
#include "../media/audio_player.hpp"
#include "../media/video_jitter.hpp"

extern "C" {
#include <peer_connection.h>
}

namespace gnx::stream {

enum class EngineState {
	Idle,
	StartingSession,  // REST: create session, wait for provisioning
	Negotiating,      // SDP/ICE exchange + DTLS
	WaitingForVideo,  // connected, waiting for the first frame
	Streaming,
	Failed,
	Stopped,
};

/*
 * Which GSSV offering the session is created against.
 *
 * Cloud is xCloud proper: a datacentre blade, addressed by title id, metered
 * against the account's Game Pass cloud hours. Console is xHome remote play:
 * your own Xbox, addressed by the serverId from fetch_home_consoles(), not
 * metered, and streaming the console's dashboard rather than one title -- so
 * there is no title id to give it.
 *
 * Everything downstream of session creation is identical bar one detail: the
 * console's streaming agent wants H264 level 3.2 in the offer (see
 * sdp_set_h264_level_32).
 */
enum class StreamTarget {
	Cloud,
	Console,
};

class Engine {
public:
	explicit Engine(XboxAuth &auth);
	~Engine();

	// Releases the process-wide WebRTC state (libsrtp, usrsctp and its two
	// service threads) that the first Engine brings up. Call once at exit,
	// after every Engine is gone; no Engine may be created afterwards.
	static void global_shutdown();

	// `id` is a title id when target is Cloud, a console serverId when it
	// is Console.
	void start(StreamTarget target, const std::string &id, QualityTier tier,
		   const std::string &locale = "en-GB");
	void stop();

	EngineState state() const { return state_; }
	std::string status() const;
	std::string error() const;

	// Decode thread: pops one assembled H.264 access unit (Annex-B, already
	// keyframe-gated and in decode order) and the millisecond tick at which
	// its last packet arrived. False when nothing is ready.
	bool take_access_unit(std::vector<uint8_t> &out, uint64_t *arrival_ms);
	// Access units waiting for the decoder right now.
	size_t queued_video() const;
	// Main thread tells the engine a frame reached the screen: arms the
	// media watchdogs and moves the state to Streaming.
	void note_decoded_frame();

	// Optional stream recorder (see au_recorder.hpp). Set before start();
	// the engine does not own it.
	void set_recorder(AuRecorder *recorder) { recorder_ = recorder; }
	// Called on the worker each time an access unit is queued, so the
	// decode thread can wake at once instead of on its idle timeout.
	void set_video_wakeup(std::function<void()> wakeup);
	// Whether the worker takes SCHED_FIFO (see worker()). Set before
	// start(); the client's -nort switch clears it.
	void set_realtime(bool on) { realtime_ = on; }
	// Override the encode size and bitrate cap the tier would ask for.
	// The capability messages carry supportsCustomResolution, so a size
	// other than the tier's own is at least expressible; whether xCloud
	// honours it is a question for the device. Zero keeps the tier's
	// value. Set before start(); see the client's -res and -bitrate.
	void set_requested_video(int width, int height, int bitrate_kbps)
	{
		req_w_ = width;
		req_h_ = height;
		req_bitrate_ = bitrate_kbps;
	}
	// Research hooks (see docs/RESOLUTION.md). The tier moves five
	// things at once -- the session fingerprint, the claimed display size,
	// the control channel's resolutionAlias, the capability messages'
	// max size/bitrate and the SDP decode caps -- so nothing the server
	// does in response can be attributed to any one of them. These move
	// the first three on their own. Empty/zero keeps the tier's value.
	void set_alias_override(std::string alias)
	{
		alias_override_ = std::move(alias);
	}
	void set_device_override(std::string os_name, int w, int h)
	{
		os_override_ = std::move(os_name);
		display_w_ = w;
		display_h_ = h;
	}
	// Overwrite the offer's declared H264 decode limits. Unlike everything
	// else here this is a protocol-level constraint rather than a request:
	// a conforming encoder may not exceed the receiver's capability.
	void set_sdp_caps(int max_fs, int max_mbps, std::string level)
	{
		sdp_max_fs_ = max_fs;
		sdp_max_mbps_ = max_mbps;
		sdp_level_ = std::move(level);
	}
	bool video_queue_empty() const
	{
		std::lock_guard<std::mutex> lock(video_mutex_);
		return video_queue_.empty();
	}

	void send_gamepad(const xcloud::GamepadFrame &frame);
	// Present thread: publish the pad state and return at once. The worker
	// sends it (rate limited, idle suppressed), so the vsync-paced present
	// loop never waits on peer_mutex_ in the window before the flip latches.
	void set_pad(const xcloud::GamepadFrame &frame);
	void request_keyframe();

	// Diagnostics for the on-screen status line.
	struct Counters {
		uint32_t pli = 0;
		uint32_t video_packets = 0;
		uint64_t video_bytes = 0;
		uint32_t audio_packets = 0;
		bool channels_open = false;
		bool handshake_done = false;
	};
	Counters counters() const;

	// What the server says is running, from
	// /streaming/properties/titleinfo. `state` is kept as the raw string
	// because its enum has not been decoded -- every session observed so
	// far reports 4 with focused true. Empty/false until the first
	// message arrives; `valid` says whether one ever did.
	struct TitleInfo {
		std::string title_id;  // e.g. "10e2ddde"
		std::string aumid;     // e.g. "Wreckfest_7b23meqmzvs8p!wreckfest"
		std::string state;
		bool focused = false;
		bool valid = false;
	};
	TitleInfo title_info() const
	{
		std::lock_guard<std::mutex> lock(title_mutex_);
		return title_;
	}

	void log(const std::string &line);  // also the libpeer log sink

private:
	void worker();
	// Runs the WebRTC session to completion. Returns false only when ICE
	// connected but DTLS/SCTP never came up (dead media path) -- worker()
	// then retries once with a fresh session. Every other outcome,
	// including ordinary failures, returns true.
	bool run_peer(GssvSession &session);
	void rearm_for_resume();
	void revive_input(const char *reason);
	void set_status(const std::string &status);
	void end_session();
	void fail(const std::string &error);
	void handle_channel_message(uint16_t sid, const char *data, size_t size);
	void handle_input_report(const uint8_t *data, size_t size);
	void open_data_channels();
	void request_keyframe_locked();  // caller holds peer_mutex_
	void send_on_channel(const char *label, const std::string &payload);
	void send_on_channel_locked(const char *label, const std::string &payload);
	bool send_binary_on_channel_locked(const char *label,
					   const std::vector<uint8_t> &payload);

	static void on_video(uint8_t *data, size_t size, void *user);
	static void on_audio(uint8_t *data, size_t size, void *user);
	static void on_channel_message(char *data, size_t size, void *user,
				       uint16_t sid);
	static void on_channel_open(void *user);
	static void on_state_change(PeerConnectionState state, void *user);

	XboxAuth &auth_;
	Http http_;  // worker-thread HTTP client

	std::atomic<EngineState> state_{EngineState::Idle};
	mutable std::mutex status_mutex_;
	std::string status_;
	std::string error_;

	// The offering's credentials: cloud or home, per target_.
	EndpointCredentials creds_;
	StreamTarget target_ = StreamTarget::Cloud;
	// Title id (Cloud) or console serverId (Console).
	std::string title_id_;
	QualityTier tier_ = QualityTier::P720;
	std::string locale_ = "en-GB";
	// set_requested_video(): 0 means "use the tier's own value".
	int req_w_ = 0, req_h_ = 0, req_bitrate_ = 0;
	// Research overrides; empty/zero = the tier's own value.
	std::string alias_override_, os_override_;
	int display_w_ = 0, display_h_ = 0;
	int sdp_max_fs_ = 0, sdp_max_mbps_ = 0;
	std::string sdp_level_;

	FILE *log_file_ = nullptr;
	std::mutex log_mutex_;

	PeerConnection *peer_ = nullptr;
	// timed_mutex: send_gamepad takes it with a bounded wait so a wedged
	// worker can never freeze input and presentation behind it.
	std::timed_mutex peer_mutex_;
	std::atomic<PeerConnectionState> peer_state_{PEER_CONNECTION_NEW};
	std::atomic<bool> channels_open_{false};
	std::atomic<bool> handshake_done_{false};
	std::atomic<bool> quit_{false};
	std::atomic<bool> server_ended_{false};
	std::atomic<bool> reconnect_requested_{false};
	std::atomic<bool> resuming_{false};
	std::atomic<Uint64> last_media_ticks_{0};
	std::atomic<Uint64> last_decode_ticks_{0};
	std::atomic<Uint64> worker_tick_{0};
	std::atomic<uint32_t> input_sent_{0};
	std::atomic<uint32_t> input_drop_lock_{0};
	std::atomic<uint32_t> input_send_fail_{0};
	std::atomic<uint32_t> input_rx_{0};
	std::atomic<Uint64> input_rx_last_{0};
	std::atomic<Uint64> input_backoff_until_{0};
	std::atomic<uint32_t> input_backoff_skips_{0};
	std::atomic<uint32_t> input_idle_skips_{0};

	// Idle suppression (input thread only): every frame on the reliable
	// input channel must be delivered in order, so repeating an unchanged
	// pad 125 times a second is pure buffer pressure.
	struct PadSnapshot {
		uint32_t buttons = 0;
		int16_t lx = 0, ly = 0, rx = 0, ry = 0;
		uint16_t lt = 0, rt = 0;
		bool operator==(const PadSnapshot &o) const
		{
			return buttons == o.buttons && lx == o.lx &&
			       ly == o.ly && rx == o.rx && ry == o.ry &&
			       lt == o.lt && rt == o.rt;
		}
	};
	PadSnapshot last_pad_;
	Uint64 last_pad_send_ = 0;
	std::atomic<bool> pad_dirty_{true};
	/* set_pad() slot, read by the worker. */
	std::mutex pad_mutex_;
	xcloud::GamepadFrame pad_frame_;
	bool pad_valid_ = false;
	Uint64 last_pad_attempt_ = 0;  /* worker only */

	VideoJitterBuffer jitter_;  // worker thread only (RTP -> access units)
	AudioPlayer audio_;         // owns its own decode/output thread
	mutable std::mutex video_mutex_;
	struct QueuedAu {
		std::vector<uint8_t> data;
		uint64_t arrival_ms;
	};
	std::deque<QueuedAu> video_queue_;
	std::atomic<bool> got_frame_{false};
	std::atomic<uint64_t> video_bytes_{0};
	std::atomic<uint32_t> video_packets_{0};
	/* Times the decoder fell so far behind that the whole backlog had to
	 * be dropped and resynced on a keyframe. Any nonzero value here is
	 * visible corruption, so it belongs in the stats line. */
	std::atomic<uint32_t> overflows_{0};
	std::atomic<uint32_t> audio_packets_{0};

	/* Access-unit arrival cadence, worker thread only. The jitter buffer
	 * emits one unit per frame; the gaps between emits are what the
	 * presenter has to absorb, so they belong in the log next to what the
	 * presenter did with them. Reset every stats line. */
	Uint64 last_au_emit_ = 0;
	uint64_t au_gap_sum_ = 0;
	uint32_t au_gap_n_ = 0, au_gap_max_ = 0;
	uint32_t au_gap_over25_ = 0, au_gap_over50_ = 0;
	uint32_t au_bytes_max_ = 0;
	AuRecorder *recorder_ = nullptr;
	std::function<void()> video_wakeup_;  /* set before start() */
	bool realtime_ = true;

	xcloud::InputSerializer input_;
	std::mutex input_mutex_;
	Uint64 stream_epoch_ = 0;
	Uint64 pli_backoff_ms_ = 0;  /* see request_keyframe_locked */
	/* Bandwidth adaptation (worker only): the REMB we advertise, backed
	 * off on loss and ramped back when the link is clean. */
	uint32_t remb_kbps_ = 0;
	uint32_t remb_last_dropped_ = 0, remb_last_nacks_ = 0;
	Uint64 remb_clean_since_ = 0;
	Uint64 remb_holdoff_until_ = 0;
	int remb_lossy_seconds_ = 0;
	std::atomic<Uint64> last_keyframe_req_{0};
	std::atomic<uint32_t> pli_sent_{0};
	/* Tick of the oldest outstanding keyframe request, cleared when an
	 * IDR arrives; the difference is logged as "pli->idr N ms". Zero when
	 * nothing is outstanding. Worker thread only. */
	Uint64 pli_pending_tick_ = 0;
	/* Whether the advertised REMB is currently lifted above the user's
	 * cap because we are blind; only there to log the lift once. */
	bool remb_lifted_ = false;
	/* Last /streaming/properties/titleinfo, written by the worker and
	 * read by whatever screen wants it. */
	mutable std::mutex title_mutex_;
	TitleInfo title_;

	std::thread thread_;
};

}  // namespace gnx::stream
