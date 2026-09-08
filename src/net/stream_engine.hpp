#pragma once

/*
 * IStreamEngine: what the session loop needs from a streaming backend.
 *
 * src/app/stream.cpp used to talk to gnx::stream::Engine directly, but it
 * only ever called twelve of its methods, and none of those twelve mention
 * xCloud. Everything that does -- the GSSV session, the quality tier, the
 * SDP capability overrides, the resolution research hooks -- is set up
 * before the loop starts and never touched again. So the seam was already
 * there; this file only gives it a name.
 *
 * Why bother: a Moonlight backend would reuse the display, decode, pacing,
 * audio and input paths unchanged and differ only in where the access units
 * come from. Naming the boundary means that backend is a new class beside
 * Engine rather than a second copy of the session loop that then has to be
 * kept in step with this one.
 *
 * The shape is a pull: the backend queues assembled access units and the
 * decode thread drains them with take_access_unit(). moonlight-common-c
 * pushes complete units at a callback instead, which is the same queue with
 * the fill side inverted -- its callback enqueues and calls the wakeup, and
 * the loop above is none the wiser.
 *
 * What is deliberately NOT here: start(). Starting a session is where the
 * backends genuinely differ (a title id and a quality tier against GSSV; a
 * paired host and an app id against a GameStream server), and no useful
 * common signature covers both. The caller constructs and starts a concrete
 * engine, then hands the loop this interface.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../gnx/xcloud_protocol.hpp"

namespace gnx::stream {

/*
 * Where a session has got to. Backend-neutral despite the names: the two
 * middle states are "asking the service for a session" and "agreeing a
 * media path with it", which GameStream also does -- pairing and launch
 * over HTTPS, then the RTSP/ENet handshake.
 */
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
 * Absolute pad state, as both backends want it: an Xbox controller with
 * sticks in -1..1 and triggers in 0..1. It is xcloud::GamepadFrame because
 * that struct is already neutral -- it carries no xCloud encoding, only the
 * layout -- and it lives in src/gnx/ only because that is where green-nx put
 * it. Moving it would break the "diffs clean against upstream" invariant
 * that src/gnx/PROVENANCE.md asks us to keep, for no gain: a Moonlight
 * backend packs these same fields into LiSendControllerEvent's button flags,
 * int16 sticks and uint8 triggers.
 */
using PadFrame = xcloud::GamepadFrame;

class IStreamEngine {
public:
	virtual ~IStreamEngine() = default;

	// ---- lifecycle ---------------------------------------------------
	// Idempotent: the loop calls it on every exit path, including after
	// the backend has already stopped itself.
	virtual void stop() = 0;
	virtual EngineState state() const = 0;
	// One line for the connecting screen, and the reason for Failed.
	virtual std::string status() const = 0;
	virtual std::string error() const = 0;

	// ---- video -------------------------------------------------------
	// Decode thread: pops one assembled H.264 access unit (Annex-B,
	// already keyframe-gated and in decode order) and the millisecond
	// tick at which its last packet arrived. False when nothing is ready.
	virtual bool take_access_unit(std::vector<uint8_t> &out,
				      uint64_t *arrival_ms) = 0;
	// Access units waiting for the decoder right now; the pace line
	// reports it, because a queue that only grows is the backlog state
	// docs/VIDEO-PACING.md is about.
	virtual size_t queued_video() const = 0;
	// A frame reached the screen. Arms the backend's media watchdogs and
	// moves the state to Streaming.
	virtual void note_decoded_frame() = 0;
	// Ask the server for an IDR. Everything after an unrecoverable loss
	// is undecodable until one arrives, so this is the recovery path --
	// and what -plitest times.
	virtual void request_keyframe() = 0;
	// Called on the backend's own thread each time an access unit is
	// queued, so the decode thread wakes at once instead of on its idle
	// timeout. Set before the pipeline starts.
	virtual void set_video_wakeup(std::function<void()> wakeup) = 0;

	// ---- input -------------------------------------------------------
	// Publish pad state and return at once: the backend sends it on its
	// own schedule (rate limited, idle suppressed), so the vsync-paced
	// present loop never blocks behind a send in the window before the
	// flip latches.
	virtual void set_pad(const PadFrame &frame) = 0;

	// ---- diagnostics -------------------------------------------------
	// What the connecting screen and the net| line show. A backend that
	// has no analogue of a field leaves it zero; channels_open and
	// handshake_done are "the control path is up" and "the server has
	// acknowledged us", which both protocols have in some form.
	struct Counters {
		uint32_t pli = 0;
		uint32_t video_packets = 0;
		uint64_t video_bytes = 0;
		uint32_t audio_packets = 0;
		bool channels_open = false;
		bool handshake_done = false;
	};
	virtual Counters counters() const = 0;
};

}  // namespace gnx::stream
