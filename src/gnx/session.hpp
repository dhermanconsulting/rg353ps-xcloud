#pragma once

#include <optional>
#include <string>
#include <vector>

#include "auth.hpp"
#include "http.hpp"

namespace gnx {

// Stream quality tier.
//
// This was once believed to work through the device fingerprint alone
// (android -> 720p, windows -> 1080p, tizen -> 1080p high bitrate). Measured
// on a live account 2026-09-05, that is wrong: what the server encodes is
//
//     resolution = min(resolutionAlias tier, the client's declared max)
//     bitrate    = f(resolutionAlias tier)
//
// The alias is the lever and the fingerprint is close to irrelevant -- a
// tizen session asking for alias "720" gets 720p. So a tier here is really a
// bundle of (alias, declared max, fingerprint) chosen to agree with itself.
// P720HQ is the useful one on a small panel: the same 1280x720 the decoder
// already copes with, at roughly half again the bitrate, which is spent on
// exactly the fine detail a 2:1 downscale destroys. docs/RESOLUTION.md
// has the measurements.
enum class QualityTier { P720, P720HQ, P1080, P1080HQ };

// Both 720-class tiers: same 1280x720 encode, different bitrate.
inline bool is_720_tier(QualityTier tier) {
    return tier == QualityTier::P720 || tier == QualityTier::P720HQ;
}

enum class SessionState {
    New,
    Provisioning,
    WaitingForResources,
    ReadyToConnect,
    Provisioned,
    Failed,
};

// One xCloud streaming session: create -> poll state -> connect (passport
// token) -> SDP offer/answer -> ICE exchange -> keepalive loop.
class GssvSession {
public:
    GssvSession(Http& http, EndpointCredentials credentials,
                QualityTier tier = QualityTier::P1080HQ,
                std::string locale = "en-US");

    // POST /v5/sessions/cloud/play for a title.
    void start_cloud(const std::string& title_id);

    // POST /v5/sessions/home/play against your own console (remote play).
    // Use with the xhome offering's credentials, not the cloud ones.
    void start_home(const std::string& server_id);

    SessionState refresh_state();
    SessionState state() const { return state_; }
    const std::string& error_details() const { return error_details_; }

    // Must be called once when state reaches ReadyToConnect.
    void connect(const std::string& passport_token);

    // Sends our SDP offer, blocks (polling) until the server's answer arrives.
    std::string exchange_sdp(const std::string& offer_sdp);

    // candidates: raw "candidate:..." strings; ufrag is our local ice-ufrag.
    void send_ice_candidates(const std::vector<std::string>& candidates,
                             const std::string& ufrag);
    // Polls once; empty vector = nothing yet. Sets *end_of_candidates when
    // the server signalled it has no more candidates coming.
    std::vector<std::string> receive_ice_candidates(
        bool* end_of_candidates = nullptr);

    void keepalive();
    void stop();

    // Research hook. The tier normally moves the fingerprint, the claimed
    // display size and the control channel's resolution alias together, so a
    // change in what the server encodes cannot be attributed to any one of
    // them. These override the first two independently; empty/zero keeps the
    // tier's own value. Call before start_*(). See docs/RESOLUTION.md.
    void set_device_override(std::string os_name, int display_w,
                             int display_h);

    // Closes any stale sessions left on the account (server allows only one).
    // platform: "cloud" (xCloud) or "home" (own-console remote play).
    static void cleanup_stale_sessions(Http& http,
                                       const EndpointCredentials& credentials,
                                       const std::string& platform = "cloud");

private:
    std::string url(const std::string& suffix) const;
    std::vector<std::string> headers() const;

    Http& http_;
    EndpointCredentials credentials_;
    QualityTier tier_;
    std::string locale_;  // BCP-47 sent as the streamed console's system language
    std::string session_path_;
    SessionState state_ = SessionState::New;
    std::string error_details_;
    // set_device_override(); empty/zero = the tier's own value.
    std::string os_name_override_;
    int display_w_override_ = 0, display_h_override_ = 0;
};

// GET /v1/waittime/{titleId}: how long the queue for a title is, so a session
// that sits in WaitingForResources can be told apart from a protocol fault --
// which is the whole reason it is worth a request.
//
// The response shape is NOT verified: the endpoint is documented and nothing
// else, so this accepts the field names the other clients use and gives up
// quietly when none of them are there. `raw` takes the body either way, which
// is how the first live queue will pin the shape down: log it and read it.
//
// Returns the wait in seconds, or -1 when there is no usable answer.
int fetch_wait_time(Http& http, const EndpointCredentials& credentials,
                    const std::string& title_id, std::string* raw = nullptr);

// Force stereo Opus in the offer SDP (useinbandfec=1 -> ...;stereo=1).
std::string sdp_force_stereo(const std::string& sdp);
// Scale the offered H264 decode caps (max-fs/max-mbps) from 720p to 1080p60.
std::string sdp_scale_video_caps_1080(const std::string& sdp);
// Overwrite the offered H264 decode limits: max-fs (frame size in
// macroblocks), max-mbps (macroblock rate) and profile-level-id. Zero or
// empty leaves that one alone. These are the receiver's declared capability,
// which a conforming encoder may not exceed -- a much stronger constraint
// than the application-level capability messages. See docs/RESOLUTION.md.
std::string sdp_set_video_caps(const std::string& sdp, int max_fs,
                               int max_mbps, const std::string& level);
// Raise the offered H264 profile-level-id from 42e01f (level 3.1) to 42e020
// (level 3.2). xHome only: the console's own streaming agent is stricter than
// the cloud one, which accepts 3.1 happily.
std::string sdp_set_h264_level_32(const std::string& sdp);

}  // namespace gnx
