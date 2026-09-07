#include <stdarg.h>
#include <stdio.h>

#include "sdp.h"

int sdp_append(char* sdp, const char* format, ...) {
  va_list argptr;

  va_start(argptr, format);

  if (sdp[0] == '\0') {
    vsnprintf(sdp, CONFIG_SDP_BUFFER_SIZE, format, argptr);
  } else {
    vsnprintf(sdp + strlen(sdp), CONFIG_SDP_BUFFER_SIZE - strlen(sdp), format, argptr);
  }

  if (sdp[strlen(sdp) - 1] != '\n') {
    strcat(sdp, "\r\n");
  }

  va_end(argptr);
  return 0;
}

void sdp_reset(char* sdp) {
  memset(sdp, 0, CONFIG_SDP_BUFFER_SIZE);
}

void sdp_append_h264(char* sdp) {
  // This section mirrors the offer of a known-good native xCloud client
  // (payload 102, recvonly, the Android-app fmtp template WITH
  // packetization-mode/max-fs/max-mbps). xCloud pattern-matches the video
  // m-line against per-device codec templates: half-matching lines get the
  // video track rejected ("m=video 0"), and a template miss without
  // packetization-mode lands in a minimal-capability fallback whose encoder
  // is pinned to single-NALU packets at a starvation bitrate (~150 kbps).
  sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF 102");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=mid:video");
  sdp_append(sdp, "a=rtcp-mux");
  sdp_append(sdp, "a=rtcp-rsize");
  sdp_append(sdp, "a=rtpmap:102 H264/90000");
  sdp_append(sdp,
             "a=fmtp:102 level-asymmetry-allowed=0;packetization-mode=1;"
             "profile-level-id=42e01f;max-fs=3600;max-mbps=108000");
  sdp_append(sdp, "a=rtcp-fb:102 goog-remb");
  sdp_append(sdp, "a=rtcp-fb:102 ccm fir");
  sdp_append(sdp, "a=rtcp-fb:102 nack");
  sdp_append(sdp, "a=rtcp-fb:102 nack pli");
  sdp_append(sdp, "a=recvonly");
}

void sdp_append_pcma(char* sdp) {
  sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVP 8");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtpmap:8 PCMA/8000");
  sdp_append(sdp, "a=ssrc:4 cname:webrtc-pcma");
  sdp_append(sdp, "a=sendrecv");
  sdp_append(sdp, "a=mid:audio");
  sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_pcmu(char* sdp) {
  sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVP 0");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=rtpmap:0 PCMU/8000");
  sdp_append(sdp, "a=ssrc:5 cname:webrtc-pcmu");
  sdp_append(sdp, "a=sendrecv");
  sdp_append(sdp, "a=mid:audio");
  sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_opus(char* sdp) {
  // Mirrors the known-good native client: SAVPF, stereo fmtp, recvonly.
  sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVPF 111");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=mid:audio");
  sdp_append(sdp, "a=rtcp-mux");
  sdp_append(sdp, "a=rtcp-rsize");
  sdp_append(sdp, "a=rtpmap:111 opus/48000/2");
  sdp_append(sdp, "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1");
  sdp_append(sdp, "a=recvonly");
}

void sdp_append_datachannel(char* sdp) {
  sdp_append(sdp, "m=application 50712 UDP/DTLS/SCTP webrtc-datachannel");
  sdp_append(sdp, "c=IN IP4 0.0.0.0");
  sdp_append(sdp, "a=mid:datachannel");
  sdp_append(sdp, "a=sctp-port:5000");
  sdp_append(sdp, "a=max-message-size:262144");
}

void sdp_create(char* sdp, int b_video, int b_audio, int b_datachannel) {
  char bundle[64];
  sdp_append(sdp, "v=0");
  sdp_append(sdp, "o=- 1495799811084970 1495799811084970 IN IP4 0.0.0.0");
  sdp_append(sdp, "s=-");
  sdp_append(sdp, "t=0 0");
  sdp_append(sdp, "a=msid-semantic: iot");
#if ICE_LITE
  sdp_append(sdp, "a=ice-lite");
#endif
  memset(bundle, 0, sizeof(bundle));

  strcat(bundle, "a=group:BUNDLE");

  if (b_video) {
    strcat(bundle, " video");
  }

  if (b_audio) {
    strcat(bundle, " audio");
  }

  if (b_datachannel) {
    strcat(bundle, " datachannel");
  }

  sdp_append(sdp, bundle);
}
