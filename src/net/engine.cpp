#include "engine.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sstream>

extern "C" {
#include <peer.h>
}

extern "C" void gnx_peer_log_set(void (*cb)(const char *line));

namespace {
/* libpeer's LOG_REDIRECT sink funnels through this single active engine. */
gnx::stream::Engine *g_log_engine = nullptr;

/* libsrtp + usrsctp are process-wide: initialized with the first Engine and
 * released once, by Engine::global_shutdown, on the way out of the app. */
bool g_peer_initialized = false;
}  // namespace

namespace gnx::stream {

namespace {
/* Safety cap only: each entry is one access unit and the main thread drains
 * the queue every frame. Dropping individual units corrupts the stream, so on
 * overflow we clear and recover with a keyframe instead. */
constexpr size_t kMaxQueuedVideo = 16;

struct TierProfile {
	int width, height, bitrate_kbps, fps;
};

/* This device only ever offers the 720p tier: the panel is 640x480 and
 * software H.264 decode of 720p60 is already about 36% of the SoC.
 * maxBitrateKbps is the real quality/CPU lever -- 8000 is what the browser
 * section 6.5 settles on for 720p60 over Wi-Fi. */
/*
 * The smallest advertised bitrate that can still carry a 720p IDR. Used only
 * while the assembler is blind: see the REMB send site. 4000 is well under
 * the 8000 the 720p tier asks for normally and comfortably above what a
 * keyframe needs.
 */
constexpr uint32_t kKeyframeFloorKbps = 4000;

TierProfile tier_profile(QualityTier tier)
{
	switch (tier) {
	case QualityTier::P720: return {1280, 720, 8000, 60};
	/* Same size, higher declared ceiling: measured, the alias does most
	 * of the work (4.1-4.9 -> 6.3 Mbps) and raising this adds a little
	 * more on top (6.9 Mbps at 15000). docs/RESOLUTION.md. */
	case QualityTier::P720HQ: return {1280, 720, 15000, 60};
	case QualityTier::P1080: return {1920, 1080, 20000, 60};
	case QualityTier::P1080HQ: return {1920, 1080, 30000, 60};
	}
	return {1280, 720, 8000, 60};
}

/* Extract "candidate:..." lines from a local SDP for the /ice POST. */
std::vector<std::string> local_candidates_from_sdp(const std::string &sdp)
{
	std::vector<std::string> out;
	size_t at = 0;
	while ((at = sdp.find("a=candidate:", at)) != std::string::npos) {
		size_t end = sdp.find_first_of("\r\n", at);
		out.push_back(sdp.substr(at + 2, end - at - 2));
		at = end == std::string::npos ? sdp.size() : end;
	}
	return out;
}

std::string ufrag_from_sdp(const std::string &sdp)
{
	size_t at = sdp.find("a=ice-ufrag:");
	if (at == std::string::npos)
		return "";
	at += std::strlen("a=ice-ufrag:");
	size_t end = sdp.find_first_of("\r\n", at);
	return sdp.substr(at, end - at);
}

const char *session_state_name(SessionState state)
{
	switch (state) {
	case SessionState::New: return "new";
	case SessionState::Provisioning: return "provisioning";
	case SessionState::WaitingForResources: return "waiting for resources";
	case SessionState::ReadyToConnect: return "ready to connect";
	case SessionState::Provisioned: return "provisioned";
	case SessionState::Failed: return "failed";
	}
	return "?";
}

/*
 * Pull one field out of a message-channel payload.
 *
 * These arrive double-encoded -- a {"type":"Message","target":...,"content":
 * "..."} envelope whose content is itself a JSON *string* -- so the fields we
 * want appear as \"name\":\"value\" or \"name\":123 in the raw bytes. The
 * existing serverInitiatedDisconnect scan does exactly this inline; this is
 * the same thing named, because titleinfo needs it three times.
 *
 * Deliberately not a JSON parse: this runs on the worker inside
 * peer_connection_loop() for every message, and nothing here needs more than
 * a scalar. Returns an empty string when the field is absent.
 */
std::string json_field(const std::string &payload, const char *name)
{
	size_t at = payload.find(name);

	if (at == std::string::npos)
		return std::string();
	at += std::strlen(name);
	/* Step over the closing quote, escapes, the colon and any space. */
	while (at < payload.size() &&
	       (payload[at] == '\\' || payload[at] == '"' ||
		payload[at] == ':' || payload[at] == ' '))
		++at;
	size_t end = at;
	while (end < payload.size() &&
	       (std::isalnum((unsigned char)payload[end]) ||
		payload[end] == '_' || payload[end] == '-' ||
		payload[end] == '.' || payload[end] == '!'))
		++end;
	return payload.substr(at, end - at);
}

}  // namespace

Engine::Engine(XboxAuth &auth) : auth_(auth)
{
	http_.set_abort_flag(&quit_);  /* don't block shutdown on an HTTP call */
	/* One-time global init of libsrtp + usrsctp. Without this srtp_create()
	 * fails (no inbound SRTP, so no decryptable video) and usrsctp never
	 * associates (data channels never open). */
	if (!g_peer_initialized) {
		peer_init();
		g_peer_initialized = true;
	}
}

/* usrsctp's two service threads ("SCTP timer", "SCTP iterator") run until
 * usrsctp_finish(). Call this once, after the last Engine is destroyed. */
void Engine::global_shutdown()
{
	if (!g_peer_initialized)
		return;
	g_peer_initialized = false;
	peer_deinit();
}

Engine::~Engine() { stop(); }

void Engine::log(const std::string &line)
{
	std::lock_guard<std::mutex> lock(log_mutex_);
	if (!log_file_)
		return;
	std::fprintf(log_file_, "[%8llu] %s\n",
		     static_cast<unsigned long long>(SDL_GetTicks64()),
		     line.c_str());
	std::fflush(log_file_);
}

void Engine::start(StreamTarget target, const std::string &title_id,
		   QualityTier tier, const std::string &locale)
{
	stop();
	target_ = target;
	title_id_ = title_id;
	tier_ = tier;
	locale_ = locale;
	{
		std::lock_guard<std::mutex> lock(log_mutex_);
		/* The port launcher already redirects stderr to
		 * /userdata/system/logs/xcloud.log, so the stream log lands
		 * there with everything else. */
		log_file_ = stderr;
	}
	g_log_engine = this;
	gnx_peer_log_set([](const char *line) {
		if (g_log_engine)
			g_log_engine->log(std::string("  peer| ") + line);
	});
	log(std::string("stream start: ") +
	    (target == StreamTarget::Console ? "console " : "title ") +
	    title_id);
	quit_ = false;
	got_frame_ = false;
	channels_open_ = false;
	handshake_done_ = false;
	server_ended_ = false;
	reconnect_requested_ = false;
	resuming_ = false;
	last_media_ticks_ = 0;
	last_decode_ticks_ = 0;
	worker_tick_ = 0;
	input_sent_ = 0;
	input_drop_lock_ = 0;
	input_send_fail_ = 0;
	input_rx_ = 0;
	input_rx_last_ = 0;
	input_backoff_until_ = 0;
	input_backoff_skips_ = 0;
	input_idle_skips_ = 0;
	last_pad_ = PadSnapshot{};
	last_pad_send_ = 0;
	pad_dirty_ = true;
	last_pad_attempt_ = 0;
	{
		std::lock_guard<std::mutex> lock(pad_mutex_);
		pad_valid_ = false;
	}
	peer_state_ = PEER_CONNECTION_NEW;  /* previous session left it CLOSED */
	pli_sent_ = 0;
	video_bytes_ = 0;
	video_packets_ = 0;
	audio_packets_ = 0;
	overflows_ = 0;
	remb_kbps_ = 0;
	remb_last_dropped_ = remb_last_nacks_ = 0;
	remb_clean_since_ = remb_holdoff_until_ = 0;
	remb_lossy_seconds_ = 0;
	pli_backoff_ms_ = 0;
	jitter_.reset();
	/* A stream without sound is still worth watching, so a
	 * failure here is a warning, not a reason to abort. */
	if (!audio_.init())
		log("audio output unavailable; continuing without sound");
	state_ = EngineState::StartingSession;
	stream_epoch_ = SDL_GetTicks64();
	thread_ = std::thread(&Engine::worker, this);
}

void Engine::stop()
{
	quit_ = true;
	if (thread_.joinable())
		thread_.join();
	if (g_log_engine == this) {
		gnx_peer_log_set(nullptr);
		g_log_engine = nullptr;
	}
	{
		std::lock_guard<std::timed_mutex> lock(peer_mutex_);
		if (peer_) {
			peer_connection_close(peer_);
			peer_connection_destroy(peer_);
			peer_ = nullptr;
		}
	}
	audio_.shutdown();
	{
		std::lock_guard<std::mutex> lock(video_mutex_);
		video_queue_.clear();
	}
	{
		std::lock_guard<std::mutex> lock(log_mutex_);
		log_file_ = nullptr;
	}
}

std::string Engine::status() const
{
	std::lock_guard<std::mutex> lock(status_mutex_);
	return status_;
}

std::string Engine::error() const
{
	std::lock_guard<std::mutex> lock(status_mutex_);
	return error_;
}

void Engine::set_status(const std::string &status)
{
	std::lock_guard<std::mutex> lock(status_mutex_);
	status_ = status;
}

/* Orderly end of a session the server closed on us. Not a failure: the UI
 * treats Stopped as "go back to the library". */
void Engine::end_session()
{
	set_status("Session ended");
	state_ = EngineState::Stopped;
}

void Engine::fail(const std::string &error)
{
	log("FAIL: " + error);
	{
		std::lock_guard<std::mutex> lock(status_mutex_);
		error_ = error;
	}
	state_ = EngineState::Failed;
}

bool Engine::take_access_unit(std::vector<uint8_t> &out, uint64_t *arrival_ms)
{
	std::lock_guard<std::mutex> lock(video_mutex_);
	if (video_queue_.empty())
		return false;
	out = std::move(video_queue_.front().data);
	*arrival_ms = video_queue_.front().arrival_ms;
	video_queue_.pop_front();
	return true;
}

void Engine::set_video_wakeup(std::function<void()> wakeup)
{
	video_wakeup_ = std::move(wakeup);
}

size_t Engine::queued_video() const
{
	std::lock_guard<std::mutex> lock(video_mutex_);
	return video_queue_.size();
}

void Engine::note_decoded_frame()
{
	last_decode_ticks_.store(SDL_GetTicks64(), std::memory_order_relaxed);
	if (!got_frame_.exchange(true))
		log("first frame decoded");
	if (state_ == EngineState::WaitingForVideo)
		state_ = EngineState::Streaming;
}

Engine::Counters Engine::counters() const
{
	Counters c;
	c.pli = pli_sent_.load();
	c.video_packets = video_packets_.load();
	c.video_bytes = video_bytes_.load();
	c.audio_packets = audio_packets_.load();
	c.channels_open = channels_open_.load();
	c.handshake_done = handshake_done_.load();
	return c;
}

/* ---- libpeer callbacks -------------------------------------------------- */

void Engine::on_video(uint8_t *data, size_t size, void *user)
{
	/* Called on the worker thread inside peer_connection_loop() (peer_mutex_
	 * held). data is a raw RTP packet; the jitter buffer reorders and
	 * assembles complete access units and only emits keyframe-anchored
	 * ones. */
	auto *self = static_cast<Engine *>(user);
	self->last_media_ticks_.store(SDL_GetTicks64(), std::memory_order_relaxed);
	self->video_bytes_.fetch_add(size, std::memory_order_relaxed);
	self->video_packets_.fetch_add(1, std::memory_order_relaxed);
	bool want_keyframe = false;
	bool overflowed = false;
	self->jitter_.receive(
		data, size, SDL_GetTicks64(),
		[self, &overflowed](const uint8_t *au, size_t au_size) {
			Uint64 t = SDL_GetTicks64();
			if (self->last_au_emit_) {
				Uint64 gap = t - self->last_au_emit_;
				self->au_gap_sum_ += gap;
				self->au_gap_n_++;
				if (gap > self->au_gap_max_)
					self->au_gap_max_ = (uint32_t)gap;
				if (gap > 25)
					self->au_gap_over25_++;
				if (gap > 50)
					self->au_gap_over50_++;
			}
			self->last_au_emit_ = t;
			if (au_size > self->au_bytes_max_)
				self->au_bytes_max_ = (uint32_t)au_size;
			if (self->recorder_)
				self->recorder_->push(AuRecorder::Video,
						      self->jitter_.stats().frames,
						      t, au, au_size);
			std::lock_guard<std::mutex> lock(self->video_mutex_);
			if (self->video_queue_.size() >= kMaxQueuedVideo) {
				/* The decoder has fallen behind. Individual
				 * access units cannot be dropped -- every
				 * later P-frame references them, so the
				 * decoder would emit smeared garbage until
				 * the server's next natural IDR, which can be
				 * many seconds away. Drop the whole backlog
				 * and resync on a fresh keyframe instead. */
				self->video_queue_.clear();
				self->overflows_.fetch_add(
					1, std::memory_order_relaxed);
				overflowed = true;
			}
			/*
			 * PLI round trip. Every unrecoverable loss freezes
			 * the picture until a real IDR arrives (see
			 * video_jitter.cpp: a broken reference frame is never
			 * fed to the decoder), so how long the server takes
			 * to answer a keyframe request IS the cost of a loss.
			 * Measured here rather than guessed: the request tick
			 * is stamped in request_keyframe_locked().
			 */
			if (self->pli_pending_tick_) {
				/* Stop at the first VCL NAL rather than
				 * scanning the whole unit: type 1 and type 5
				 * both answer the question, and a full scan
				 * of every ~100 KB unit would run on every
				 * frame precisely while we are waiting out a
				 * loss, which is when the SoC has least to
				 * spare. Anything before it is SPS/PPS/SEI. */
				for (size_t k = 0; k + 4 < au_size; k++) {
					if (au[k] || au[k + 1] ||
					    au[k + 2] != 1)
						continue;
					uint8_t type = au[k + 3] & 0x1f;
					if (type != 1 && type != 5)
						continue;
					if (type == 5) {
						self->log("pli->idr " +
							  std::to_string(
								  t - self->pli_pending_tick_) +
							  " ms");
						self->pli_pending_tick_ = 0;
					}
					break;
				}
			}
			self->video_queue_.push_back(
				{std::vector<uint8_t>(au, au + au_size), t});
		},
		[self](uint16_t pid, uint16_t blp) {
			/* Retransmit request (peer_mutex_ already held). */
			if (self->peer_)
				peer_connection_send_nack(self->peer_, pid, blp);
		},
		&want_keyframe);
	/* Requested outside the lambda: video_mutex_ is held in there, and the
	 * keyframe request sends on a data channel. */
	if (want_keyframe || overflowed)
		self->request_keyframe_locked();
	if (self->video_wakeup_ && !self->video_queue_empty())
		self->video_wakeup_();
}

void Engine::on_audio(uint8_t *data, size_t size, void *user)
{
	/* Called on the worker thread with peer_mutex_ held. `data` is a whole
	 * RTP packet -- the patched rtp_decode_generic forwards header and
	 * payload together, like the H.264 path, because the caller needs the
	 * sequence number: Opus is stateful, so decoding out of order is
	 * audible. Parse the header here, then hand the payload straight to the
	 * audio thread; decoding happens there, never on this thread, so audio
	 * never waits behind video or RTCP work. */
	auto *self = static_cast<Engine *>(user);
	self->last_media_ticks_.store(SDL_GetTicks64(), std::memory_order_relaxed);
	self->audio_packets_.fetch_add(1, std::memory_order_relaxed);
	if (size < 12)
		return;
	uint8_t csrc_count = data[0] & 0x0F;
	bool has_extension = (data[0] & 0x10) != 0;
	bool has_padding = (data[0] & 0x20) != 0;
	uint16_t seq = (static_cast<uint16_t>(data[2]) << 8) | data[3];

	size_t offset = 12 + static_cast<size_t>(csrc_count) * 4;
	if (has_extension) {
		if (offset + 4 > size)
			return;
		uint16_t ext_words =
			(static_cast<uint16_t>(data[offset + 2]) << 8) |
			data[offset + 3];
		offset += 4 + static_cast<size_t>(ext_words) * 4;
	}
	size_t end = size;
	if (has_padding && end > offset) {
		uint8_t pad = data[end - 1];
		if (pad <= end - offset)
			end -= pad;
	}
	if (offset >= end)
		return;
	if (self->recorder_)
		self->recorder_->push(AuRecorder::Audio, seq, SDL_GetTicks64(),
				      data + offset, end - offset);
	self->audio_.submit(seq, data + offset, end - offset);
}

void Engine::on_channel_message(char *data, size_t size, void *user,
				uint16_t sid)
{
	static_cast<Engine *>(user)->handle_channel_message(sid, data, size);
}

void Engine::on_channel_open(void *user)
{
	static_cast<Engine *>(user)->channels_open_ = true;
}

void Engine::on_state_change(PeerConnectionState state, void *user)
{
	static_cast<Engine *>(user)->peer_state_ = state;
}

/* ---- data channel plumbing ---------------------------------------------- */

void Engine::open_data_channels()
{
	std::lock_guard<std::timed_mutex> lock(peer_mutex_);
	if (!peer_)
		return;
	/* The DTLS client uses even SCTP stream ids (RFC 8832). xCloud maps
	 * each channel by its DCEP label, so the ids only need to be
	 * distinct. */
	struct {
		const xcloud::ChannelConfig &cfg;
		uint16_t sid;
	} channels[] = {
		{xcloud::kControlChannel, 0},
		{xcloud::kInputChannel, 2},
		{xcloud::kMessageChannel, 4},
		{xcloud::kChatChannel, 6},
	};
	for (const auto &channel : channels) {
		DecpChannelType type =
			channel.cfg.max_retransmits == 0
				? (channel.cfg.ordered
					   ? DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT
					   : DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED)
				: (channel.cfg.ordered
					   ? DATA_CHANNEL_RELIABLE
					   : DATA_CHANNEL_RELIABLE_UNORDERED);
		uint32_t reliability =
			channel.cfg.max_retransmits < 0
				? 0
				: static_cast<uint32_t>(channel.cfg.max_retransmits);
		peer_connection_create_datachannel_sid(
			peer_, type, 0, reliability,
			const_cast<char *>(channel.cfg.label),
			const_cast<char *>(channel.cfg.protocol), channel.sid);
	}
	log("opened data channels (control/input/message/chat)");
}

void Engine::send_on_channel(const char *label, const std::string &payload)
{
	std::lock_guard<std::timed_mutex> lock(peer_mutex_);
	send_on_channel_locked(label, payload);
}

/* Caller must hold peer_mutex_. */
void Engine::send_on_channel_locked(const char *label,
				    const std::string &payload)
{
	if (!peer_)
		return;
	uint16_t sid = 0;
	/* control/message/chat carry JSON, so they must go as WebRTC string
	 * frames -- xCloud silently ignores JSON sent as binary and you lose
	 * the handshake and all quality control with no error. */
	if (peer_connection_lookup_sid(peer_, label, &sid) == 0) {
		peer_connection_datachannel_send_text_sid(
			peer_, const_cast<char *>(payload.data()),
			payload.size(), sid);
		log("send [" + std::string(label) +
		    " sid=" + std::to_string(sid) + "] " + payload.substr(0, 220));
	} else {
		log("send FAILED (no channel '" + std::string(label) + "')");
	}
}

bool Engine::send_binary_on_channel_locked(const char *label,
					   const std::vector<uint8_t> &payload)
{
	if (!peer_)
		return false;
	uint16_t sid = 0;
	if (peer_connection_lookup_sid(peer_, label, &sid) != 0)
		return false;
	return peer_connection_datachannel_send_sid(
		       peer_,
		       const_cast<char *>(
			       reinterpret_cast<const char *>(payload.data())),
		       payload.size(), sid) >= 0;
}

void Engine::handle_channel_message(uint16_t sid, const char *data, size_t size)
{
	/* IMPORTANT: libpeer invokes this from inside peer_connection_loop(),
	 * which the worker already runs while holding peer_mutex_. peer_mutex_
	 * is not recursive, so we must NOT re-lock it here -- doing so froze
	 * green-nx's worker the instant xCloud's first message arrived. peer_
	 * is guaranteed alive for the duration of this callback. */
	char *label = peer_ ? peer_connection_lookup_sid_label(peer_, sid) : nullptr;
	if (label && std::strcmp(label, "input") == 0) {
		handle_input_report(reinterpret_cast<const uint8_t *>(data), size);
		return;
	}
	{
		/* 512, not 220: the server's own messages run to ~300 bytes
		 * and the "target" field that says what each one IS sits at
		 * the end, so the shorter cap truncated exactly the part
		 * worth reading. See docs/PROTOCOL.md. */
		std::string preview(data, std::min<size_t>(size, 512));
		log("recv [" + std::string(label ? label : "sid?") +
		    " sid=" + std::to_string(sid) +
		    " len=" + std::to_string(size) + "] " + preview);
	}
	if (!label)
		return;

	/* End-of-session notice, e.g. target
	 * /streaming/sessionLifetimeManagement/serverInitiatedDisconnect with
	 * content {"reason":"KickForStopCommand"}. */
	if (std::strcmp(label, "message") == 0) {
		std::string payload(data, size);
		if (payload.find("serverInitiatedDisconnect") != std::string::npos) {
			std::string reason;
			size_t at = payload.find("reason");
			if (at != std::string::npos) {
				at += 6;
				while (at < payload.size() &&
				       (payload[at] == '\\' || payload[at] == '"' ||
					payload[at] == ':' || payload[at] == ' '))
					++at;
				size_t end = at;
				while (end < payload.size() &&
				       (std::isalnum((unsigned char)payload[end]) ||
					payload[end] == '_' || payload[end] == '-'))
					++end;
				reason = payload.substr(at, end - at);
			}
			log("server ended the session" +
			    (reason.empty() ? std::string() : " (" + reason + ")"));
			server_ended_ = true;
			return;
		}
	}

	/*
	 * /streaming/properties/titleinfo: {focused, state, titleaumid,
	 * titleid}. The server volunteers this and we used to only log it.
	 *
	 * It is recorded rather than acted on, deliberately. `state` is an
	 * enum nobody here has decoded -- every session observed so far
	 * reports state 4 with focused true, i.e. the steady state -- and
	 * wiring behaviour (say, leaving the stream when a title exits) to a
	 * value whose meaning is a guess would be worse than doing nothing.
	 * Logging only the TRANSITIONS makes the enum learnable: play on the
	 * device, quit a game, and the log says which number that was.
	 */
	if (std::strcmp(label, "message") == 0) {
		std::string payload(data, size);
		if (payload.find("titleinfo") != std::string::npos) {
			TitleInfo info;
			info.title_id = json_field(payload, "titleid");
			info.aumid = json_field(payload, "titleaumid");
			info.state = json_field(payload, "state");
			info.focused = json_field(payload, "focused") == "true";
			info.valid = true;
			{
				std::lock_guard<std::mutex> lock(title_mutex_);
				if (title_.title_id != info.title_id ||
				    title_.state != info.state ||
				    title_.focused != info.focused)
					log("titleinfo: id=" + info.title_id +
					    " state=" + info.state +
					    " focused=" +
					    (info.focused ? "yes" : "no") +
					    " aumid=" + info.aumid);
				title_ = info;
			}
		}
	}

	if (std::strcmp(label, "message") == 0 && !handshake_done_) {
		if (xcloud::is_handshake_ack(std::string(data, size))) {
			/* Handshake acked: authorize the control channel,
			 * announce the gamepad, then declare capabilities. */
			send_on_channel_locked("control",
					       xcloud::authorization_request());
			send_on_channel_locked("control",
					       xcloud::gamepad_changed(0, true));
			/* Omit this and the server may free-pick 1440p on
			 * titles that support it. -alias overrides it on its
			 * own, which is the only way to tell what the alias
			 * does from what the fingerprint does. */
			{
				/* THE lever: measured, resolution is
				 * min(this, our declared max) and bitrate is
				 * a function of this alone. The fingerprint
				 * barely matters. docs/RESOLUTION.md. */
				const char *alias =
					tier_ == QualityTier::P720      ? "720"
					: tier_ == QualityTier::P720HQ  ? "720HQ"
					: tier_ == QualityTier::P1080   ? "1080"
									: "1080HQ";
				if (!alias_override_.empty())
					alias = alias_override_.c_str();
				log(std::string("resolutionAlias: ") + alias);
				send_on_channel_locked(
					"control",
					xcloud::user_requested_resolution(alias));
			}
			TierProfile profile = tier_profile(tier_);
			/*
			 * -res / -bitrate. Both capability messages take
			 * these numbers, and both declare
			 * supportsCustomResolution, so asking for something
			 * other than the tier's own size is expressible.
			 * The server may still encode at its own resolution;
			 * the "stream is now WxH" line says what arrived.
			 */
			if (req_w_ > 0 && req_h_ > 0) {
				profile.width = req_w_;
				profile.height = req_h_;
			}
			if (req_bitrate_ > 0)
				profile.bitrate_kbps = req_bitrate_;
			log("asking for " + std::to_string(profile.width) + "x" +
			    std::to_string(profile.height) + " at " +
			    std::to_string(profile.bitrate_kbps) + " kbps");
			for (const std::string &message : xcloud::startup_messages(
				     profile.width, profile.height,
				     profile.bitrate_kbps, profile.fps))
				send_on_channel_locked("message", message);
			{
				std::lock_guard<std::mutex> lock(input_mutex_);
				send_binary_on_channel_locked(
					"input", input_.client_metadata());
			}
			/* Ask for an IDR immediately -- both the RTCP PLI that
			 * xCloud actually acts on and the app-level message --
			 * so video can start instead of waiting for the
			 * server's periodic keyframe. */
			peer_connection_request_keyframe(peer_);
			send_on_channel_locked("control",
					       xcloud::video_keyframe_requested());
			last_keyframe_req_ = SDL_GetTicks64();
			log("handshake complete, capabilities sent");
			handshake_done_ = true;
			if (state_ == EngineState::Negotiating)
				state_ = EngineState::WaitingForVideo;
		}
	}
}

void Engine::handle_input_report(const uint8_t *data, size_t size)
{
	/* Any server-originated input-channel traffic is proof the session's
	 * input side is alive. green-nx decodes vibration reports (type 128)
	 * here; this handheld has no rumble motors, so we only count. */
	(void)data;
	(void)size;
	input_rx_.fetch_add(1, std::memory_order_relaxed);
	input_rx_last_.store(SDL_GetTicks64(), std::memory_order_relaxed);
}

/* ---- worker ------------------------------------------------------------- */

void Engine::worker()
{
	/* Named so the per-thread CPU line in the pace log can tell the
	 * socket pump apart from the decoder's worker threads. */
	pthread_setname_np(pthread_self(), "xc-worker");
	/* This thread is the only thing that empties the UDP socket. When the
	 * decoder's two workers are busy on two cores and the presenter and
	 * audio hold the others for a moment, a normal-priority pump can sit
	 * runnable for milliseconds while 1000 packets/s pile into the socket
	 * buffer: jitter the jitter buffer then has to absorb. Low real-time
	 * priority (below audio 15 and present 12) gets it scheduled the
	 * moment a packet lands. Safe: the loop sleeps 1 ms whenever the
	 * socket is empty, so it can never monopolise a core. */
	if (realtime_) {
		struct sched_param sp;
		std::memset(&sp, 0, sizeof(sp));
		sp.sched_priority = 8;
		if (sched_setscheduler(0, SCHED_FIFO, &sp))
			log(std::string("worker: SCHED_FIFO refused: ") +
			    std::strerror(errno));
	}
	try {
		const bool console = target_ == StreamTarget::Console;

		set_status(console ? "Signing in to Xbox Live..."
				   : "Signing in to xCloud...");
		log("fetching streaming credentials");
		StreamingCredentials creds = auth_.fetch_streaming_credentials();
		creds_ = console ? creds.home : creds.cloud;
		if (creds_.host.empty()) {
			/* For the console path an empty host is the normal
			 * shape of "no Xbox on this account", and home_error
			 * is the only explanation anyone will get. */
			if (console)
				fail(creds.home_error.empty()
					     ? "No Xbox is linked to this account"
					     : "Remote play unavailable: " +
						       creds.home_error);
			else
				fail("xCloud is not available for this account");
			return;
		}

		set_status("Cleaning up old sessions...");
		GssvSession::cleanup_stale_sessions(http_, creds_,
						    console ? "home" : "cloud");
		log("stale-session cleanup done");

		/* One retry: a session can come up with a dead media path (ICE
		 * connects, DTLS never answers), and a fresh session re-rolls
		 * that server-side fault. */
		const int attempts = 2;
		int midstream_reconnects = 0;
		Uint64 reconnect_window_start = 0;
		for (int attempt = 0; attempt < attempts && !quit_; ++attempt) {
			if (attempt > 0) {
				set_status("Retrying the connection...");
				/* The dead session's teardown has to release
				 * the account's slot, or the fresh request
				 * queues in "waiting for resources". */
				for (int i = 0; i < 30 && !quit_; ++i)
					std::this_thread::sleep_for(
						std::chrono::milliseconds(100));
				if (quit_)
					break;
			}
			set_status("Requesting a session...");
			log("requesting session (attempt " +
			    std::to_string(attempt + 1) + " of " +
			    std::to_string(attempts) + ")");
			GssvSession session(http_, creds_, tier_, locale_);
			if (!os_override_.empty() || display_w_ > 0)
				session.set_device_override(os_override_,
							    display_w_,
							    display_h_);
			if (console)
				session.start_home(title_id_);
			else
				session.start_cloud(title_id_);
			log("session created, polling state");

			/* A console in instant-on standby has to come up
			 * before it can provision, which takes appreciably
			 * longer than a cloud blade being allocated. */
			set_status(console ? "Waking your Xbox..."
					   : "Waiting for a server...");
			bool connected = false;
			bool retry_transport = false;
			SessionState logged_state = SessionState::New;
			std::string session_error;
			int polls_in_state = 0;
			for (int poll = 0; !quit_; ++poll) {
				SessionState state = session.refresh_state();
				if (state != logged_state) {
					logged_state = state;
					polls_in_state = 0;
					log(std::string("session state: ") +
					    session_state_name(state) +
					    " (poll " + std::to_string(poll) + ")");
					/*
					 * Entering the queue is the one state
					 * worth a second request: without it
					 * "Waiting for a server" looks exactly
					 * like a protocol fault that never
					 * moves. Asked once per entry, not per
					 * poll. Console sessions have no queue.
					 */
					if (state == SessionState::WaitingForResources &&
					    !console) {
						std::string raw;
						int secs = fetch_wait_time(
							http_, creds_,
							title_id_, &raw);
						log("waittime: " +
						    (raw.empty() ? "(no body)" : raw));
						if (secs > 0) {
							char line[64];
							std::snprintf(
								line, sizeof(line),
								"In the queue: about %d min",
								(secs + 59) / 60);
							set_status(line);
						} else {
							set_status("In the queue...");
						}
					}
				}
				if (state == SessionState::ReadyToConnect &&
				    !connected) {
					set_status("Authenticating...");
					/* The Passport long play token is
					 * short-lived: mint it here, not at
					 * sign-in. */
					session.connect(auth_.fetch_passport_token());
					connected = true;
				} else if (state == SessionState::Provisioned) {
					bool peer_ok = false;
					bool resignal_refused = false;
					if (resuming_) {
						try {
							peer_ok = run_peer(session);
						} catch (const std::exception &error) {
							log(std::string("re-signal failed: ") +
							    error.what());
							resignal_refused = true;
						}
					} else {
						peer_ok = run_peer(session);
					}
					if (peer_ok) {
						session.stop();
						return;
					}
					if (!resignal_refused &&
					    reconnect_requested_.exchange(false)) {
						/* Mid-stream reconnect: keep
						 * the SAME session. The backend
						 * flips a surviving session back
						 * to a connectable state and
						 * accepts a fresh SDP/ICE
						 * exchange into the same
						 * sessionPath -- no new
						 * allocation, so none of the
						 * "waiting for resources"
						 * queue. */
						Uint64 rnow = SDL_GetTicks64();
						if (!reconnect_window_start ||
						    rnow - reconnect_window_start > 120000) {
							reconnect_window_start = rnow;
							midstream_reconnects = 0;
						}
						if (++midstream_reconnects > 3) {
							fail("The connection keeps dropping, please start the stream again");
							session.stop();
							return;
						}
						/* Two callers now: a dead SCTP
						 * input channel, and lost ICE
						 * consent. Both mean the
						 * transport went away under a
						 * session that is still there. */
						log("mid-stream reconnect (" +
						    std::to_string(midstream_reconnects) +
						    "/3 in this 2 min window)");
						resuming_ = true;
						rearm_for_resume();
						set_status("Reconnecting...");
						connected = false;
						polls_in_state = 0;
						continue;
					}
					retry_transport = true;
					break;
				} else if (state == SessionState::Failed) {
					if (resuming_) {
						log("resumed session unrecoverable: " +
						    session.error_details());
						retry_transport = true;
						break;
					}
					session_error = session.error_details();
					break;
				}
				/* A busy region can hold an allocation in
				 * WaitingForResources far longer than a fixed
				 * poll cap allows, so time out per state
				 * rather than over the whole wait. */
				int cap = resuming_ ? 30
					  : state == SessionState::WaitingForResources
						  ? 2570  /* ~30 min in the queue */
						  : 300;  /* ~3.5 min otherwise */
				if (++polls_in_state >= cap) {
					if (resuming_)
						retry_transport = true;
					break;
				}
				std::this_thread::sleep_for(
					std::chrono::milliseconds(700));
			}
			session.stop();
			if (quit_)
				return;
			if (retry_transport) {
				reconnect_requested_ = false;
				if (attempt == attempts - 1) {
					fail("The server's media connection never came up");
					return;
				}
				log("retrying with a fresh session (dead media path)");
				{
					std::lock_guard<std::timed_mutex> lock(peer_mutex_);
					if (peer_) {
						peer_connection_close(peer_);
						peer_connection_destroy(peer_);
						peer_ = nullptr;
					}
				}
				peer_state_ = PEER_CONNECTION_NEW;
				channels_open_ = false;
				handshake_done_ = false;
				if (!resuming_)
					state_ = EngineState::StartingSession;
				continue;
			}
			if (session_error.empty()) {
				fail("Timed out waiting for a session");
				return;
			}
			log("session attempt " + std::to_string(attempt + 1) +
			    " failed: " + session_error);
			/*
			 * A console asleep in ConnectedStandby fails the FIRST
			 * request every time and wakes up doing it. Measured
			 * 2026-09-06: the service delivers the start command,
			 * the console's agent answers AgentCommandError
			 * (0x80004005) at ServerStartStreamingV2CommandSent,
			 * and the console goes from ConnectedStandby to On. A
			 * request made once it is awake provisions in about a
			 * second.
			 *
			 * So this is a wake, not a failure, and surfacing it
			 * as one would mean every cold start looks broken and
			 * has to be retried by hand. Wait for the console to
			 * finish waking and ask again.
			 */
			if (console && attempt + 1 < attempts &&
			    session_error.find("AgentCommandError") !=
				    std::string::npos) {
				set_status("Waking your Xbox...");
				log("console was asleep; waiting for it to "
				    "wake, then retrying");
				/*
				 * Wait exactly as long as the console needs
				 * rather than guessing: /v6/servers/home
				 * reports its power state, so poll until it
				 * says On. Best effort -- a failed poll is
				 * not a reason to give up on the retry, so
				 * the cap applies either way.
				 */
				for (int i = 0; i < 30 && !quit_; ++i) {
					std::this_thread::sleep_for(
						std::chrono::seconds(1));
					bool awake = false;
					try {
						for (const HomeConsole &c :
						     fetch_home_consoles(http_,
									 creds_))
							if (c.server_id ==
								    title_id_ &&
							    c.power_state == "On")
								awake = true;
					} catch (const std::exception &) {
						continue;  /* try again */
					}
					if (awake) {
						log("console is awake after " +
						    std::to_string(i + 1) + " s");
						break;
					}
				}
				if (quit_)
					return;
				continue;
			}
			fail("Session failed: " + session_error);
			return;
		}
	} catch (const std::exception &error) {
		fail(error.what());
	}
}

void Engine::revive_input(const char *reason)
{
	Uint64 now = SDL_GetTicks64();
	Uint64 rx_last = input_rx_last_.load(std::memory_order_relaxed);
	{
		std::lock_guard<std::timed_mutex> lock(peer_mutex_);
		if (!peer_ || !handshake_done_)
			return;
		{
			/* client_metadata consumes a sequence number; a refused
			 * send has to give it back or the gap kills input. */
			std::lock_guard<std::mutex> input_lock(input_mutex_);
			if (!send_binary_on_channel_locked(
				    "input", input_.client_metadata()))
				input_.rollback_sequence();
		}
		send_on_channel_locked("control", xcloud::gamepad_changed(0, false));
		send_on_channel_locked("control", xcloud::gamepad_changed(0, true));
	}
	log(std::string("input revive (") + reason +
	    "): pad re-announced, server input traffic last seen " +
	    (rx_last && now >= rx_last
		     ? std::to_string((now - rx_last) / 1000) + "s ago"
		     : "never"));
}

void Engine::rearm_for_resume()
{
	{
		/* Dispose of the dead peer first: with it gone, no media
		 * callback can race the re-arm below. */
		std::lock_guard<std::timed_mutex> lock(peer_mutex_);
		if (peer_) {
			peer_connection_close(peer_);
			peer_connection_destroy(peer_);
			peer_ = nullptr;
		}
	}
	/* Stale tick state would let the watchdogs misread the old stream's
	 * timestamps and kill the fresh transport while it negotiates. */
	got_frame_ = false;
	last_media_ticks_ = 0;
	last_decode_ticks_ = 0;
	jitter_.reset();
	/* The jitter buffer's counters restart from zero; the REMB deltas
	 * must too, or the next tick reads the drop from 25 to 0 as loss. */
	remb_last_dropped_ = remb_last_nacks_ = 0;
	remb_lossy_seconds_ = 0;
	pli_backoff_ms_ = 0;
	{
		std::lock_guard<std::mutex> lock(video_mutex_);
		video_queue_.clear();
	}
	video_bytes_ = 0;
	video_packets_ = 0;
	audio_packets_ = 0;
	audio_.resync();
	input_rx_ = 0;
	input_rx_last_ = 0;
	pad_dirty_ = true;  /* the new transport needs the full pad state once */
	peer_state_ = PEER_CONNECTION_NEW;
	channels_open_ = false;
	handshake_done_ = false;
}

/* ---- WebRTC transport --------------------------------------------------- */

bool Engine::run_peer(GssvSession &session)
{
	if (!resuming_)
		state_ = EngineState::Negotiating;
	set_status("Negotiating connection...");

	PeerConfiguration config{};
	config.ice_servers[0].urls = "stun:stun.l.google.com:19302";
	config.audio_codec = CODEC_OPUS;
	config.video_codec = CODEC_H264;
	config.datachannel = DATA_CHANNEL_BINARY;
	config.onvideotrack = &Engine::on_video;
	config.onaudiotrack = &Engine::on_audio;
	config.user_data = this;

	{
		std::lock_guard<std::timed_mutex> lock(peer_mutex_);
		peer_ = peer_connection_create(&config);
		if (!peer_) {
			fail("Failed to create peer connection");
			return true;
		}
		peer_connection_oniceconnectionstatechange(peer_,
							   &Engine::on_state_change);
		/* The client must open these channels, but libpeer can only
		 * send DATA_CHANNEL_OPEN once the SCTP association is up, so
		 * creation is deferred until on_channel_open fires. */
		peer_connection_ondatachannel(peer_, &Engine::on_channel_message,
					      &Engine::on_channel_open, nullptr);
	}

	const char *offer = nullptr;
	{
		std::lock_guard<std::timed_mutex> lock(peer_mutex_);
		offer = peer_connection_create_offer(peer_);
	}
	if (!offer) {
		fail("Failed to create SDP offer");
		return true;
	}
	log("local offer created (" + std::to_string(std::strlen(offer)) +
	    " bytes)");

	/* The base offer already matches the known-good native client's
	 * template exactly (recvonly, PT 102, full fmtp, goog-remb/fir, stereo
	 * opus) -- that is what deps/patches/libpeer-rg353.patch does to
	 * sdp.c. No b=AS/TIAS lines: working clients do not send them, and the
	 * bitrate cap is declared via clientdevicecapabilities.maxBitrateKbps
	 * instead. This device never leaves the 720p tier, so the offer ships
	 * verbatim; sdp_force_stereo is a no-op safety net. */
	std::string munged = sdp_force_stereo(offer);
	if (!is_720_tier(tier_))
		munged = sdp_scale_video_caps_1080(munged);
	/* The one place the console path differs from the cloud one: its
	 * streaming agent is stricter about the advertised H264 level. */
	if (target_ == StreamTarget::Console)
		munged = sdp_set_h264_level_32(munged);
	/* Research: declare smaller decode limits than we can actually
	 * manage, to find out whether the encoder honours the receiver's
	 * stated H264 capability. See docs/RESOLUTION.md. */
	if (sdp_max_fs_ > 0 || sdp_max_mbps_ > 0 || !sdp_level_.empty()) {
		munged = sdp_set_video_caps(munged, sdp_max_fs_, sdp_max_mbps_,
					    sdp_level_);
		log("sdp caps overridden: max-fs=" +
		    std::to_string(sdp_max_fs_) + " max-mbps=" +
		    std::to_string(sdp_max_mbps_) + " level=" +
		    (sdp_level_.empty() ? "(kept)" : sdp_level_));
	}

	/* Pass the answer to libpeer VERBATIM. Never rewrite it: the server has
	 * already chosen the codec, and any reserialisation risks corrupting
	 * the CRLF line endings, which would make libpeer parse the ICE
	 * ufrag/pwd with a stray CR and send STUN checks with a wrong integrity
	 * key -- silently dropped by the server, so the connection simply never
	 * completes. */
	std::string answer = session.exchange_sdp(munged);
	log("answer received (" + std::to_string(answer.size()) + " bytes)");
	{
		std::lock_guard<std::mutex> lock(log_mutex_);
		if (log_file_)
			std::fprintf(log_file_,
				     "----- OFFER -----\n%s\n----- ANSWER -----\n%s\n-----\n",
				     munged.c_str(), answer.c_str());
	}

	/* Our candidates go to the server over /ice. They are embedded in the
	 * offer SDP too, but the official client posts them explicitly, and
	 * getting the shape wrong makes xCloud withhold its real candidate. */
	try {
		std::vector<std::string> local = local_candidates_from_sdp(munged);
		std::string ufrag = ufrag_from_sdp(munged);
		log("posting " + std::to_string(local.size()) +
		    " local candidates (ufrag " + ufrag + ")");
		if (!local.empty())
			session.send_ice_candidates(local, ufrag);
	} catch (const std::exception &error) {
		log(std::string("local candidate post failed: ") + error.what());
	}

	/* IMPORTANT: xCloud trickles its candidates via /ice, not in the answer
	 * SDP -- and libpeer builds candidate pairs exactly once, inside
	 * set_remote_description. So collect the server's candidates FIRST. */
	std::vector<std::string> remote;
	{
		Uint64 gather_deadline = SDL_GetTicks64() + 15000;
		bool done = false;
		int quiet_polls = 0;
		/* xCloud's first candidate is a dead placeholder (priority 100,
		 * typically on 13.104.x) that never answers STUN; the real
		 * Teredo candidate can trickle in seconds later. Settling for
		 * the placeholder alone is the single most common cause of
		 * "WebRTC connection failed". */
		auto has_real_candidate = [&remote]() {
			for (const std::string &c : remote) {
				int field = 0;
				unsigned long prio = 0;
				std::istringstream ss(c);
				std::string tok;
				while (ss >> tok && field < 4) {
					if (field == 3)
						prio = std::strtoul(tok.c_str(),
								    nullptr, 10);
					field++;
				}
				if (prio > 1000)
					return true;
			}
			return false;
		};
		while (!quit_ && !done && SDL_GetTicks64() < gather_deadline) {
			size_t before = remote.size();
			try {
				for (std::string &candidate :
				     session.receive_ice_candidates(&done))
					remote.push_back(std::move(candidate));
			} catch (const std::exception &error) {
				log(std::string("ice poll failed: ") + error.what());
			}
			quiet_polls = remote.size() == before ? quiet_polls + 1 : 0;
			/* No end marker but candidates stopped coming: assume
			 * complete -- but never settle while all we have is the
			 * dead placeholder. */
			if (!remote.empty() && has_real_candidate() && quiet_polls >= 4)
				break;
			if (!done)
				std::this_thread::sleep_for(
					std::chrono::milliseconds(300));
		}
	}
	log("collected " + std::to_string(remote.size()) + " remote candidates");
	for (const std::string &candidate : remote)
		log("  remote cand: " + candidate);
	for (const std::string &candidate : local_candidates_from_sdp(munged))
		log("  local  cand: " + candidate);
	if (remote.empty()) {
		fail("Server sent no ICE candidates");
		return true;
	}

	{
		std::lock_guard<std::timed_mutex> lock(peer_mutex_);
		for (const std::string &candidate : remote)
			peer_connection_add_ice_candidate(
				peer_, const_cast<char *>(candidate.c_str()));
		/* Builds pairs from every remote candidate above, then
		 * transitions to CHECKING. */
		peer_connection_set_remote_description(peer_, answer.c_str(),
						       SDP_TYPE_ANSWER);
	}
	log("remote description set, checking connectivity");

	/* GSSV keepalive is a blocking HTTPS request (up to a 15 s timeout). It
	 * must never run on this thread: the loop below is also the sole
	 * libpeer socket pump, and pausing it lets the UDP receive queue
	 * overflow -- green-nx saw a video hitch followed by a PLI almost
	 * exactly every 15 seconds. The RAII joiner covers every return path (a
	 * destroyed joinable thread would std::terminate). */
	worker_tick_ = 0;
	std::atomic<bool> keepalive_stop{false};
	std::thread keepalive_thread([this, &session, &keepalive_stop] {
		pthread_setname_np(pthread_self(), "xc-keepalive");
		Uint64 next = SDL_GetTicks64() + 15000;
		Uint64 prev_round = SDL_GetTicks64();
		bool stall_reported = false;
		while (!quit_ && !keepalive_stop) {
			Uint64 now = SDL_GetTicks64();
			/* Worker-stall watchdog. The pump loop's own watchdogs
			 * cannot see the pump wedging inside libpeer: the log
			 * just stops and the app freezes with no trace. This
			 * thread never touches peer_mutex_, so it survives to
			 * record it. tick comes from the worker thread's own
			 * clock read, which can be a few ms ahead of ours, so
			 * the now > tick guard matters -- without it the
			 * unsigned subtraction wraps to a huge value and every
			 * stream instantly reads as stalled. */
			Uint64 tick = worker_tick_.load(std::memory_order_relaxed);
			if (now - prev_round <= 2000 && !stall_reported && tick &&
			    now > tick && now - tick > 10000) {
				stall_reported = true;
				fail("Stream engine stalled, please start the stream again");
			}
			prev_round = now;
			if (now < next) {
				std::this_thread::sleep_for(
					std::chrono::milliseconds(
						std::min<Uint64>(100, next - now)));
				continue;
			}
			Uint64 started = now;
			try {
				session.keepalive();
			} catch (const std::exception &error) {
				if (!quit_ && !keepalive_stop)
					log(std::string("keepalive failed: ") +
					    error.what());
			}
			Uint64 elapsed = SDL_GetTicks64() - started;
			if (elapsed >= 100)
				log("keepalive took " + std::to_string(elapsed) +
				    "ms (off media thread)");
			next = SDL_GetTicks64() + 15000;
		}
	});
	struct KeepaliveJoiner {
		std::atomic<bool> &stop;
		std::thread &thread;
		~KeepaliveJoiner()
		{
			stop = true;
			if (thread.joinable())
				thread.join();
		}
	} keepalive_joiner{keepalive_stop, keepalive_thread};

	Uint64 ice_connected_at = 0;
	Uint64 last_rr = SDL_GetTicks64();
	Uint64 last_consent = SDL_GetTicks64();
	Uint64 last_stats = SDL_GetTicks64();
	Uint64 idr_wait_start = 0;
	Uint64 last_idr_wait_log = 0;
	bool decode_stall_resynced = false;
	int input_dead_seconds = 0;
	Uint64 negotiation_started = SDL_GetTicks64();
	bool opened_channels = false;
	bool sent_handshake = false;
	PeerConnectionState last_logged_state = PEER_CONNECTION_NEW;

	while (!quit_) {
		/* Drain every packet ready on the socket this cycle. 720p60 is
		 * well over a thousand packets a second; processing one per
		 * iteration and then sleeping overflowed the socket buffer and
		 * wrecked the video. Batch of 16, not 64: under load the worker
		 * gets preempted mid-batch holding the lock and the input path
		 * starves behind it. */
		bool drained_any = false;
		{
			std::lock_guard<std::timed_mutex> lock(peer_mutex_);
			for (int i = 0; peer_ && i < 16; ++i) {
				if (peer_connection_loop(peer_) > 0)
					drained_any = true;
				else
					break;  /* socket empty */
			}
		}

		Uint64 now = SDL_GetTicks64();
		PeerConnectionState current = peer_state_;
		if (current != last_logged_state) {
			last_logged_state = current;
			log(std::string("peer state: ") +
			    peer_connection_state_to_string(current));
		}

		/* Pad send, on this thread rather than the presenter's: the
		 * present loop is real-time and must not wait on peer_mutex_
		 * while its commit is racing the next vblank. 8 ms is the
		 * 125 Hz cadence the official client uses; send_gamepad's idle
		 * suppression turns an unchanged pad into a 250 ms heartbeat. */
		if (now - last_pad_attempt_ >= 8) {
			last_pad_attempt_ = now;
			bool valid;
			xcloud::GamepadFrame frame;
			{
				std::lock_guard<std::mutex> lock(pad_mutex_);
				valid = pad_valid_;
				frame = pad_frame_;
			}
			if (valid)
				send_gamepad(frame);
		}

		/* channels_open_ is set from libpeer's SCTP onopen (association
		 * up). Only now can DATA_CHANNEL_OPEN be sent. */
		if (channels_open_ && !opened_channels) {
			opened_channels = true;
			open_data_channels();
			set_status("Handshaking...");
		}

		if (opened_channels && !sent_handshake) {
			sent_handshake = true;
			send_on_channel("message", xcloud::message_handshake());
		}

		if (peer_state_ == PEER_CONNECTION_FAILED) {
			fail("WebRTC connection failed");
			return true;
		}
		if (server_ended_) {
			log("session ended by the server -- returning to the library");
			end_session();
			return true;
		}
		/* Dead media path: ICE is up (the front-door placeholder
		 * answers STUN) but DTLS/SCTP never completes, because nothing
		 * behind the front door talks back. Healthy sessions open their
		 * channels 1-2 s after connecting, so 12 s means never. Hand
		 * the decision to worker(): one fresh session re-rolls it. */
		if (!ice_connected_at && (current == PEER_CONNECTION_CONNECTED ||
					  current == PEER_CONNECTION_COMPLETED))
			ice_connected_at = now;
		if (ice_connected_at && !channels_open_ &&
		    now - ice_connected_at > 12000) {
			log("ICE connected but DTLS/SCTP never completed -- dead media path");
			return false;
		}
		if ((state_ == EngineState::Negotiating || resuming_) &&
		    now - negotiation_started > 45000) {
			fail("Connection timed out");
			return true;
		}
		/*
		 * Peer gone after it was up: libpeer's consent check timed
		 * out. Only meaningful once ICE connected -- CLOSED is also
		 * the enum's zero value, so an unconnected peer must not trip
		 * it.
		 *
		 * This used to end the stream outright, which is far too
		 * final for what it actually means. Consent is a handful of
		 * STUN exchanges; losing them is a Wi-Fi hiccup, not a dead
		 * session, and the session itself is usually still sitting
		 * there server-side. Reported on remote play as "works for a
		 * few minutes then unrecoverably kicks me out".
		 *
		 * So take the same route a dead data channel takes: keep the
		 * session and re-signal into it. That path is already bounded
		 * at three attempts in a two-minute window, after which it
		 * gives up with a message that says so -- a genuinely dead
		 * link still ends the stream, just not on the first lost
		 * packet.
		 */
		if (ice_connected_at && (current == PEER_CONNECTION_CLOSED ||
					 current == PEER_CONNECTION_DISCONNECTED)) {
			log("peer consent lost; re-signalling into the same "
			    "session");
			set_status("Reconnecting...");
			reconnect_requested_ = true;
			return false;
		}
		worker_tick_.store(now, std::memory_order_relaxed);

		/* Media-stall watchdog. RTP stops the moment a session really
		 * ends, but libpeer needs ~20 s of failed consent checks to
		 * notice and a half-open path may never close at all. */
		if (got_frame_) {
			Uint64 last_media =
				last_media_ticks_.load(std::memory_order_relaxed);
			if (last_media && now - last_media > 10000) {
				fail("Stream stalled: no video or audio for 10s");
				return true;
			}
			/* Video-only stall: after heavy loss the jitter buffer
			 * can wait forever for a clean IDR while audio and even
			 * video RTP keep flowing, so the watchdog above never
			 * fires. Five seconds of units going in with nothing
			 * decoded means the decoder is swallowing garbage:
			 * wipe the assembler and gate on a clean IDR. One shot
			 * per stall, re-armed once decoding resumes. */
			Uint64 last_decode =
				last_decode_ticks_.load(std::memory_order_relaxed);
			if (last_decode && now > last_decode) {
				if (now - last_decode > 5000 && !decode_stall_resynced) {
					decode_stall_resynced = true;
					{
						std::lock_guard<std::timed_mutex> lock(peer_mutex_);
						jitter_.reset();
						remb_last_dropped_ = remb_last_nacks_ = 0;
						remb_lossy_seconds_ = 0;
						request_keyframe_locked();
					}
					log("video decode stalled 5s: jitter reset, waiting for a clean IDR");
				} else if (now - last_decode <= 5000) {
					decode_stall_resynced = false;
				}
			}
			if (last_decode && now > last_decode &&
			    now - last_decode > 15000) {
				fail("Video stalled for 15s, please start the stream again");
				return true;
			}
		}

		/* Until a clean IDR has been accepted, keep asking for one.
		 * xCloud may start mid-GOP or drop our first request; a single
		 * ask is not enough. Gated on the assembler, not on the first
		 * decoded frame: once the IDR is in, the decode thread will
		 * get to it, and asking again meanwhile only buys more IDRs.
		 * Throttled (300 ms, doubling) in request_keyframe_locked. */
		if (handshake_done_ && jitter_.waiting_keyframe()) {
			std::lock_guard<std::timed_mutex> lock(peer_mutex_);
			request_keyframe_locked();
		}

		/* Make an IDR drought visible instead of silently dropping. */
		if (handshake_done_ && jitter_.waiting_keyframe()) {
			if (!idr_wait_start)
				idr_wait_start = now;
			if (now - last_idr_wait_log >= 2000 &&
			    now - idr_wait_start >= 2000) {
				last_idr_wait_log = now;
				log("waiting for IDR (" +
				    std::to_string((now - idr_wait_start) / 1000) +
				    "s, " + std::to_string(pli_sent_.load()) +
				    " PLIs sent)");
			}
		} else {
			idr_wait_start = 0;
		}

		/* Periodic RTCP Receiver Report + REMB. This is not mere
		 * etiquette: without valid receiver feedback a libwebrtc sender
		 * pins its encoder at the starvation floor and the picture is a
		 * smeared trickle. */
		if (now - last_rr > 1000) {
			last_rr = now;
			uint8_t fraction;
			uint32_t cumulative, highest_ext;
			/*
			 * Bandwidth adaptation through REMB. The cap we
			 * advertise is what the encoder targets, and a
			 * constant 8 Mbps on a Wi-Fi link that is losing
			 * packets just keeps feeding the loss. Multiplicative
			 * back-off on any dropped frame or NACK burst in the
			 * last second, additive ramp once the link has been
			 * clean for five seconds; floor at 3 Mbps, where 720p
			 * is still watchable. Measured need: a five-minute race
			 * with 64 NACKs and 25 dropped frames at a fixed
			 * 8 Mbps.
			 */
			/*
			 * -bitrate overrides the tier here too, and this is
			 * the half that matters: the capability message is
			 * read once at startup, but REMB is what the sender's
			 * encoder tracks second by second. A libwebrtc-shaped
			 * sender that cannot fit its resolution in the
			 * advertised bandwidth drops the resolution, which is
			 * the one lever we have found that xCloud actually
			 * acts on -- see docs/VIDEO-PACING.md, "Asking
			 * xCloud".
			 */
			const uint32_t cap = static_cast<uint32_t>(
				req_bitrate_ > 0 ? req_bitrate_
						 : tier_profile(tier_).bitrate_kbps);
			VideoJitterBuffer::Stats js = jitter_.stats();
			if (!remb_kbps_) {
				remb_kbps_ = cap;
				remb_clean_since_ = now;
			}
			/* A second counts as lossy on two dropped frames or
			 * five NACK messages (each covers up to 17 packets): a
			 * single 30 ms hole is not congestion. Two lossy
			 * seconds in a row, or one really bad one, back off by
			 * 15%; then hold for 3 s, because the encoder only
			 * sees the new cap a second or two later and reacting
			 * to the old rate's loss again would step twice for
			 * one event. */
			uint32_t d = js.dropped - remb_last_dropped_;
			uint32_t n = js.nacks - remb_last_nacks_;
			remb_last_dropped_ = js.dropped;
			remb_last_nacks_ = js.nacks;
			bool lossy = d >= 2 || n >= 5;
			bool bad = d >= 5 || n >= 15;
			remb_lossy_seconds_ = lossy ? remb_lossy_seconds_ + 1 : 0;
			if ((remb_lossy_seconds_ >= 2 || bad) &&
			    now >= remb_holdoff_until_) {
				uint32_t next = remb_kbps_ * 85 / 100;
				if (next < 3000)
					next = 3000;
				if (next != remb_kbps_)
					log("remb: loss, " + std::to_string(remb_kbps_) +
					    " -> " + std::to_string(next) + " kbps");
				remb_kbps_ = next;
				remb_clean_since_ = now;
				remb_holdoff_until_ = now + 3000;
				remb_lossy_seconds_ = 0;
			} else if (lossy) {
				remb_clean_since_ = now;
			} else if (remb_kbps_ < cap &&
				   now - remb_clean_since_ >= 5000) {
				uint32_t next = remb_kbps_ + 1000;
				if (next > cap)
					next = cap;
				remb_kbps_ = next;
				remb_clean_since_ = now;
				if (next == cap)
					log("remb: back at " + std::to_string(cap) +
					    " kbps");
			}
			if (jitter_.report_stats(&fraction, &cumulative,
						 &highest_ext)) {
				std::lock_guard<std::timed_mutex> lock(peer_mutex_);
				if (peer_) {
					peer_connection_send_receiver_report(
						peer_, fraction, cumulative,
						highest_ext, 0);
					/*
					 * While blind, never advertise a rate
					 * that cannot carry a keyframe.
					 *
					 * A PLI asks for an IDR, which is a
					 * large burst; if the bitrate we have
					 * advertised is too small to fit one,
					 * the encoder answers with more
					 * P-frames and we discard every one of
					 * them, forever. Measured on the
					 * device 2026-09-07 with the menu's
					 * bitrate cap left at 1200 kbps:
					 * remote play ran for 30 s, lost one
					 * packet, and then sat blind through
					 * nine PLIs receiving nothing but NAL
					 * type 1 until the stall watchdog
					 * fired. Clearing the cap fixed it
					 * outright.
					 *
					 * The user's cap is honoured the rest
					 * of the time; this only lifts it
					 * while we are asking for the frame
					 * that ends the stall, because the
					 * alternative to briefly ignoring the
					 * cap is a stream that never comes
					 * back.
					 */
					uint32_t advertise = remb_kbps_;
					if (jitter_.waiting_keyframe() &&
					    advertise < kKeyframeFloorKbps) {
						advertise = kKeyframeFloorKbps;
						if (!remb_lifted_) {
							remb_lifted_ = true;
							log("remb: blind, lifting "
							    + std::to_string(remb_kbps_)
							    + " -> " +
							    std::to_string(advertise)
							    + " kbps so a keyframe fits");
						}
					} else if (!jitter_.waiting_keyframe()) {
						remb_lifted_ = false;
					}
					peer_connection_send_remb(
						peer_, advertise * 1000u);
				}
			}
		}

		/* Once-a-second pipeline telemetry, and the dead-datachannel
		 * detector: a big lag spike can kill the SCTP association
		 * outright, and usrsctp never recovers it, so media keeps
		 * flowing while input and control are gone for good. */
		if (now - last_stats > 1000) {
			last_stats = now;
			VideoJitterBuffer::Stats j = jitter_.stats();
			AudioPlayer::Stats a = audio_.stats();
			uint32_t in_sent = input_sent_.exchange(0);
			uint32_t in_drop = input_drop_lock_.exchange(0);
			uint32_t in_fail = input_send_fail_.exchange(0);
			if (got_frame_ || handshake_done_) {
				char gap[96];
				std::snprintf(gap, sizeof(gap),
					      " au_gap=%.1f/%ums >25:%u >50:%u max=%uKB",
					      au_gap_n_ ? (double)au_gap_sum_ / au_gap_n_ : 0.0,
					      au_gap_max_, au_gap_over25_, au_gap_over50_,
					      au_bytes_max_ / 1024);
				au_gap_sum_ = 0;
				au_gap_n_ = au_gap_max_ = 0;
				au_gap_over25_ = au_gap_over50_ = 0;
				au_bytes_max_ = 0;
				log("video| pkt=" + std::to_string(j.packets) +
				    " frames=" + std::to_string(j.frames) +
				    " drop=" + std::to_string(j.dropped) +
				    " nack=" + std::to_string(j.nacks) +
				    " resync=" + std::to_string(j.resyncs) +
				    " pli=" + std::to_string(pli_sent_.load()) +
				    " overflow=" + std::to_string(overflows_.load()) +
				    " remb=" + std::to_string(remb_kbps_) +
				    gap +
				    " audio=" + std::to_string(audio_packets_.load()) +
				    " (play=" + std::to_string(a.played) +
				    " fail=" + std::to_string(a.failed) +
				    " lost=" + std::to_string(a.lost) +
				    " under=" + std::to_string(a.underruns) +
				    " drop=" + std::to_string(a.dropped_ms) + "ms" +
				    " q=" + std::to_string(a.queue_ms) + "ms" +
				    " ring=" + std::to_string(a.ring_ms) + "ms" +
				    " inmax=" + std::to_string(a.inbox_max) +
				    " waits=" + std::to_string(a.reorder_waits) +
				    " alsa=" + std::to_string(a.delay_min_ms) + "-" +
				    std::to_string(a.delay_max_ms) + "ms" +
				    " blk=" + std::to_string(a.write_block_max_ms) + "ms" +
				    " adj=" + std::to_string(a.servo_samples) + ")" +
				    " | input sent=" + std::to_string(in_sent) +
				    " drop=" + std::to_string(in_drop) +
				    " fail=" + std::to_string(in_fail) +
				    " rx=" + std::to_string(input_rx_.load()));
			}
			if (in_sent == 0 && in_fail > 0) {
				if (++input_dead_seconds >= 3) {
					log("input channel dead for 3s (sctp): reconnecting");
					set_status("Reconnecting...");
					reconnect_requested_ = true;
					return false;
				}
			} else {
				input_dead_seconds = 0;
			}
		}

		/* ICE consent freshness (RFC 7675): keep the peer's consent to
		 * send us media alive. A full WebRTC stack does this every ~5s;
		 * libpeer does not do it at all. */
		if (now - last_consent > 2000) {
			last_consent = now;
			std::lock_guard<std::timed_mutex> lock(peer_mutex_);
			if (peer_)
				peer_connection_send_consent(peer_);
		}

		/* Only yield when idle. While video is flowing we loop straight
		 * back and keep draining at full speed (the select() inside
		 * libpeer paces idle cycles). */
		if (!drained_any)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;  /* stop requested: a normal end, nothing to retry */
}

/* ---- input -------------------------------------------------------------- */

void Engine::send_gamepad(const xcloud::GamepadFrame &frame)
{
	if (!handshake_done_)
		return;
	/* Once the peer is gone every send fails inside libpeer and logs an
	 * error; at 62.5 Hz that fills the log until the app is killed. */
	PeerConnectionState peer_state = peer_state_;
	if (peer_state != PEER_CONNECTION_CONNECTED &&
	    peer_state != PEER_CONNECTION_COMPLETED)
		return;
	/* A refused send means usrsctp's send buffer is full: the association
	 * has stopped draining. Pushing another frame 16 ms later cannot help,
	 * it just keeps the buffer wedged and holds peer_mutex_ away from the
	 * media pump. Sends resume the instant it drains. */
	Uint64 backoff = input_backoff_until_.load(std::memory_order_relaxed);
	Uint64 pad_now = SDL_GetTicks64();
	if (backoff && pad_now < backoff) {
		input_backoff_skips_++;
		return;
	}
	/* Idle suppression, the way the official client does it. Every frame on
	 * this channel is reliable and ordered, so repeating an unchanged pad
	 * is what fills the send buffer during a lag spike and then makes the
	 * server work through stale frames before a current one. A genuinely
	 * idle pad still costs one heartbeat frame every 250 ms. */
	PadSnapshot pad;
	pad.buttons = (frame.nexus ? 1u << 0 : 0) | (frame.menu ? 1u << 1 : 0) |
		      (frame.view ? 1u << 2 : 0) | (frame.a ? 1u << 3 : 0) |
		      (frame.b ? 1u << 4 : 0) | (frame.x ? 1u << 5 : 0) |
		      (frame.y ? 1u << 6 : 0) | (frame.dpad_up ? 1u << 7 : 0) |
		      (frame.dpad_down ? 1u << 8 : 0) |
		      (frame.dpad_left ? 1u << 9 : 0) |
		      (frame.dpad_right ? 1u << 10 : 0) |
		      (frame.left_shoulder ? 1u << 11 : 0) |
		      (frame.right_shoulder ? 1u << 12 : 0) |
		      (frame.left_thumb ? 1u << 13 : 0) |
		      (frame.right_thumb ? 1u << 14 : 0);
	pad.lx = static_cast<int16_t>(frame.left_x * 32767.0f);
	pad.ly = static_cast<int16_t>(frame.left_y * 32767.0f);
	pad.rx = static_cast<int16_t>(frame.right_x * 32767.0f);
	pad.ry = static_cast<int16_t>(frame.right_y * 32767.0f);
	pad.lt = static_cast<uint16_t>(frame.left_trigger * 65535.0f);
	pad.rt = static_cast<uint16_t>(frame.right_trigger * 65535.0f);
	if (!pad_dirty_ && pad == last_pad_ && last_pad_send_ &&
	    pad_now >= last_pad_send_ && pad_now - last_pad_send_ < 250) {
		input_idle_skips_++;
		return;
	}
	/* Bounded wait, never a full block: if the worker wedges inside libpeer
	 * while holding peer_mutex_, an unbounded lock here freezes
	 * presentation and input polling with it. Dropping one full-state frame
	 * is invisible; freezing the app is not. */
	std::unique_lock<std::timed_mutex> lock(peer_mutex_, std::defer_lock);
	if (!lock.try_lock_for(std::chrono::milliseconds(8))) {
		input_drop_lock_++;
		return;
	}
	/* Serialize only once the send is certain to be attempted: every built
	 * packet consumes a sequence number, and the server stops applying
	 * input for the rest of the session the moment it sees a gap. */
	std::vector<uint8_t> packet;
	{
		std::lock_guard<std::mutex> input_lock(input_mutex_);
		packet = input_.gamepad_packet(
			frame, static_cast<double>(SDL_GetTicks64() - stream_epoch_));
	}
	if (send_binary_on_channel_locked("input", packet)) {
		input_sent_++;
		input_backoff_until_ = 0;
		last_pad_ = pad;
		last_pad_send_ = pad_now;
		pad_dirty_ = false;
	} else {
		input_send_fail_++;
		input_backoff_until_ = SDL_GetTicks64() + 100;
		/* Give the unused number back so the next frame stays
		 * contiguous. */
		std::lock_guard<std::mutex> input_lock(input_mutex_);
		input_.rollback_sequence();
	}
}

void Engine::set_pad(const xcloud::GamepadFrame &frame)
{
	std::lock_guard<std::mutex> lock(pad_mutex_);
	pad_frame_ = frame;
	pad_valid_ = true;
}

void Engine::request_keyframe()
{
	/* Called off the worker thread. Opportunistic: it self-throttles to 1/s
	 * and the worker sends PLIs on its own, so skipping when the lock is
	 * busy loses nothing. */
	std::unique_lock<std::timed_mutex> lock(peer_mutex_, std::try_to_lock);
	if (!lock.owns_lock())
		return;
	request_keyframe_locked();
}

/* Caller must hold peer_mutex_ (used from on_video, which runs under it). */
void Engine::request_keyframe_locked()
{
	if (!handshake_done_ || !peer_)
		return;
	Uint64 now = SDL_GetTicks64();
	/*
	 * Exponential throttle: 300 ms for the first ask, doubling while the
	 * assembler is still waiting for a clean IDR, capped at 2 s, and
	 * back to 300 ms once one arrives.
	 *
	 * Why not a fixed interval either way. green-nx's 1 s meant that when
	 * the IDR answering a PLI was itself lost -- routine on this Wi-Fi
	 * link -- the picture froze for a full second (two 2 s gaps in one
	 * five-minute race). A fixed 300 ms fixes that but the assembler
	 * asks again for every non-IDR frame while waiting, i.e. 3.3 PLIs/s,
	 * and libwebrtc will answer each one: up to 3.3 IDRs/s of 100-200 KB
	 * each, precisely while the link is losing packets. Doubling gets the
	 * fast retry without the storm.
	 */
	Uint64 interval = pli_backoff_ms_ ? pli_backoff_ms_ : 300;
	if (!jitter_.waiting_keyframe())
		interval = 300;
	if (now - last_keyframe_req_.load() < interval)
		return;
	last_keyframe_req_ = now;
	pli_backoff_ms_ = jitter_.waiting_keyframe()
				  ? std::min<Uint64>(interval * 2, 2000)
				  : 300;
	pli_sent_++;
	/* Stamp the first outstanding request only: the answer to it is the
	 * IDR we will see, and restamping on every retry would measure the
	 * last retry rather than the freeze the user actually sat through. */
	if (!pli_pending_tick_)
		pli_pending_tick_ = now;
	/* The RTCP PLI is the one xCloud actually acts on; the app-level
	 * message alone is not sufficient. */
	peer_connection_request_keyframe(peer_);
	send_on_channel_locked("control", xcloud::video_keyframe_requested());
}

}  // namespace gnx::stream
