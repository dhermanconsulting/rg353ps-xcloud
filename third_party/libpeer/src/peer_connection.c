#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "agent.h"
#include "config.h"
#include "dtls_srtp.h"
#include "peer_connection.h"
#include "ports.h"
#include "rtcp.h"
#include "rtp.h"
#include "sctp.h"
#include "sdp.h"

#define STATE_CHANGED(pc, curr_state)                                 \
  if (pc->oniceconnectionstatechange && pc->state != curr_state) {    \
    pc->oniceconnectionstatechange(curr_state, pc->config.user_data); \
    pc->state = curr_state;                                           \
  }

struct PeerConnection {
  PeerConfiguration config;
  PeerConnectionState state;
  Agent agent;
  DtlsSrtp dtls_srtp;
  Sctp sctp;

  char sdp[CONFIG_SDP_BUFFER_SIZE];

  void (*onicecandidate)(char* sdp, void* user_data);
  void (*oniceconnectionstatechange)(PeerConnectionState state, void* user_data);
  void (*on_connected)(void* userdata);
  void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* user_data);

  uint8_t temp_buf[CONFIG_MTU];
  uint8_t agent_buf[CONFIG_MTU];
  int agent_ret;
  int b_local_description_created;

  RtpEncoder artp_encoder;
  RtpEncoder vrtp_encoder;
  RtpDecoder vrtp_decoder;
  RtpDecoder artp_decoder;

  uint32_t remote_assrc;
  uint32_t remote_vssrc;

  // Last received Sender Report, for RTCP RR LSR/DLSR (receiver liveness).
  uint32_t rtcp_last_sr_lsr;
  uint32_t rtcp_last_sr_time;
};

static void peer_connection_outgoing_rtp_packet(uint8_t* data, size_t size, void* user_data) {
  PeerConnection* pc = (PeerConnection*)user_data;
  dtls_srtp_encrypt_rtp_packet(&pc->dtls_srtp, data, (int*)&size);
  agent_send(&pc->agent, data, size);
}

static int peer_connection_dtls_srtp_recv(void* ctx, unsigned char* buf, size_t len) {
  int ret = -1;
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  // Hand mbedtls the datagram peer_connection_loop already pulled off the
  // socket -- exactly once. agent_ret must be consumed here: leaving it set
  // re-served the same datagram on every subsequent bio call, so a record
  // mbedtls silently discards (anti-replay duplicate after a lag spike, bad
  // MAC) made it fetch the same bytes forever and the worker never returned
  // from dtls_srtp_read, wedging the whole app.
  if (pc->agent_ret > 0 && pc->agent_ret <= len) {
    ret = pc->agent_ret;
    pc->agent_ret = -1;
    memcpy(buf, pc->agent_buf, ret);
    return ret;
  }

  // Until the bring-up completes (pc->state reaches COMPLETED) the handshake
  // owns the socket, so poll it here. Never gate this on dtls_srtp->state:
  // key derivation flips that to CONNECTED in the middle of the handshake,
  // before the server's final CCS+Finished flight arrives -- gating on it
  // left that flight unread forever and every handshake died with -0x6800
  // after the 7 s retransmit ladder (v1.0.29pre1). Never block and never
  // return 0 either: mbedtls reads 0 as "connection closed" (the -0x7280 in
  // the field logs), and blocking starves the retransmission timer in
  // dtls_srtp_do_handshake that resends lost flights.
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    ret = agent_recv(&pc->agent, buf, len);
    return ret > 0 ? ret : MBEDTLS_ERR_SSL_WANT_READ;
  }

  // Data phase: the socket belongs to peer_connection_loop, never read it
  // from inside the bio. No stashed datagram left means this read is done.
  return MBEDTLS_ERR_SSL_WANT_READ;
}

static int peer_connection_dtls_srtp_send(void* ctx, const uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  // LOGD("send %.4x %.4x, %ld", *(uint16_t*)buf, *(uint16_t*)(buf + 2), len);
  return agent_send(&pc->agent, buf, len);
}

static void peer_connection_incoming_rtcp(PeerConnection* pc, uint8_t* buf, size_t len) {
  RtcpHeader* rtcp_header;
  size_t pos = 0;

  while (pos < len) {
    rtcp_header = (RtcpHeader*)(buf + pos);

    switch (rtcp_header->type) {
      case RTCP_SR: {
        // Sender Report: remember the sender's NTP timestamp (middle 32 bits)
        // and arrival time so our Receiver Reports can reflect LSR/DLSR. Some
        // WebRTC senders throttle/pause video without valid receiver feedback.
        uint8_t* sr = buf + pos;
        if (pos + 16 <= len) {
          uint32_t ntp_sec, ntp_frac;
          memcpy(&ntp_sec, sr + 8, 4);
          memcpy(&ntp_frac, sr + 12, 4);
          ntp_sec = ntohl(ntp_sec);
          ntp_frac = ntohl(ntp_frac);
          pc->rtcp_last_sr_lsr = (ntp_sec << 16) | (ntp_frac >> 16);
          pc->rtcp_last_sr_time = ports_get_epoch_time();
        }
        break;
      }
      case RTCP_RR:
        LOGD("RTCP_PR");
        if (rtcp_header->rc > 0) {
// TODO: REMB, GCC ...etc
#if 0
          RtcpRr rtcp_rr = rtcp_parse_rr(buf);
          uint32_t fraction = ntohl(rtcp_rr.report_block[0].flcnpl) >> 24;
          uint32_t total = ntohl(rtcp_rr.report_block[0].flcnpl) & 0x00FFFFFF;
          if(pc->on_receiver_packet_loss && fraction > 0) {

            pc->on_receiver_packet_loss((float)fraction/256.0, total, pc->config.user_data);
          }
#endif
        }
        break;
      case RTCP_PSFB: {
        int fmt = rtcp_header->rc;
        LOGD("RTCP_PSFB %d", fmt);
        // PLI and FIR
        if ((fmt == 1 || fmt == 4) && pc->config.on_request_keyframe) {
          pc->config.on_request_keyframe(pc->config.user_data);
        }
      }
      default:
        break;
    }

    pos += 4 * ntohs(rtcp_header->length) + 4;
  }
}

const char* peer_connection_state_to_string(PeerConnectionState state) {
  switch (state) {
    case PEER_CONNECTION_NEW:
      return "new";
    case PEER_CONNECTION_CHECKING:
      return "checking";
    case PEER_CONNECTION_CONNECTED:
      return "connected";
    case PEER_CONNECTION_COMPLETED:
      return "completed";
    case PEER_CONNECTION_FAILED:
      return "failed";
    case PEER_CONNECTION_CLOSED:
      return "closed";
    case PEER_CONNECTION_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

PeerConnectionState peer_connection_get_state(PeerConnection* pc) {
  return pc->state;
}

void* peer_connection_get_sctp(PeerConnection* pc) {
  return &pc->sctp;
}

PeerConnection* peer_connection_create(PeerConfiguration* config) {
  PeerConnection* pc = calloc(1, sizeof(PeerConnection));
  if (!pc) {
    return NULL;
  }

  memcpy(&pc->config, config, sizeof(PeerConfiguration));

  agent_create(&pc->agent);

  memset(&pc->sctp, 0, sizeof(pc->sctp));

  if (pc->config.audio_codec) {
    rtp_encoder_init(&pc->artp_encoder, pc->config.audio_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->artp_decoder, pc->config.audio_codec,
                     pc->config.onaudiotrack, pc->config.user_data);
  }

  if (pc->config.video_codec) {
    rtp_encoder_init(&pc->vrtp_encoder, pc->config.video_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->vrtp_decoder, pc->config.video_codec,
                     pc->config.onvideotrack, pc->config.user_data);
  }

  return pc;
}

void peer_connection_destroy(PeerConnection* pc) {
  if (pc) {
    sctp_destroy_association(&pc->sctp);
    dtls_srtp_deinit(&pc->dtls_srtp);
    agent_destroy(&pc->agent);
    free(pc);
    pc = NULL;
  }
}

void peer_connection_close(PeerConnection* pc) {
  pc->state = PEER_CONNECTION_CLOSED;
}

int peer_connection_send_audio(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }
  return rtp_encoder_encode(&pc->artp_encoder, buf, len);
}

int peer_connection_send_video(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }
  return rtp_encoder_encode(&pc->vrtp_encoder, buf, len);
}

// Send an RTCP PLI (RFC 4585) asking the sender for a fresh intra frame.
// Standard WebRTC clients do this automatically; without it xCloud only ever
// streams P-frames and the decoder never gets a keyframe/parameter sets to
// start from. The app-level videoKeyframeRequested message is not sufficient.
int peer_connection_request_keyframe(PeerConnection* pc) {
  if (pc == NULL || pc->state != PEER_CONNECTION_COMPLETED)
    return -1;
  uint8_t rtcp[128];
  int len = rtcp_get_pli(rtcp, 12, htonl(pc->remote_vssrc));
  if (len < 0)
    return -1;
  uint32_t sender_ssrc = htonl(SSRC_H264);
  memcpy(rtcp + 4, &sender_ssrc, 4);  // SSRC of packet sender (us)
  dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, rtcp, &len);
  return agent_send(&pc->agent, rtcp, len);
}

// ICE consent keepalive: refresh the peer's consent to send media to us.
int peer_connection_send_consent(PeerConnection* pc) {
  if (pc == NULL || pc->state != PEER_CONNECTION_COMPLETED)
    return -1;
  return agent_send_binding_request(&pc->agent);
}

// Send an RTCP Generic NACK (RFC 4585 RTPFB, FMT=1) to request retransmission
// of lost video packets. pid = first lost sequence number; blp = bitmask of
// the following 16 packets also lost. Lets frames complete despite loss instead
// of dropping the whole frame and stalling.
int peer_connection_send_nack(PeerConnection* pc, uint16_t pid, uint16_t blp) {
  if (pc == NULL || pc->state != PEER_CONNECTION_COMPLETED)
    return -1;
  uint8_t rtcp[128];
  memset(rtcp, 0, 16);
  RtcpHeader* h = (RtcpHeader*)rtcp;
  h->version = 2;
  h->rc = 1;  // FMT=1 (Generic NACK)
  h->type = RTCP_RTPFB;
  h->length = htons(3);  // (16/4) - 1
  uint32_t sender_ssrc = htonl(SSRC_H264);
  uint32_t media_ssrc = htonl(pc->remote_vssrc);
  memcpy(rtcp + 4, &sender_ssrc, 4);
  memcpy(rtcp + 8, &media_ssrc, 4);
  uint16_t be_pid = htons(pid);
  uint16_t be_blp = htons(blp);
  memcpy(rtcp + 12, &be_pid, 2);
  memcpy(rtcp + 14, &be_blp, 2);
  int len = 16;
  dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, rtcp, &len);
  return agent_send(&pc->agent, rtcp, len);
}

// Send an RTCP REMB (Receiver Estimated Max Bitrate, PSFB FMT=15). This is the
// bandwidth feedback a libwebrtc sender needs to raise the encoder bitrate:
// with no REMB/transport-cc negotiated the sender pins the encoder at its
// starvation floor and the picture is a smeared trickle. Requires
// "a=rtcp-fb:<pt> goog-remb" in the negotiated SDP.
int peer_connection_send_remb(PeerConnection* pc, uint32_t bitrate_bps) {
  if (pc == NULL || pc->state != PEER_CONNECTION_COMPLETED)
    return -1;
  uint8_t rtcp[128];
  memset(rtcp, 0, 24);
  RtcpHeader* h = (RtcpHeader*)rtcp;
  h->version = 2;
  h->rc = 15;  // FMT=15 (Application Layer Feedback)
  h->type = RTCP_PSFB;
  h->length = htons(5);  // (24/4) - 1
  uint32_t sender_ssrc = htonl(SSRC_H264);
  memcpy(rtcp + 4, &sender_ssrc, 4);
  // media SSRC is 0 for REMB; targets go in the FCI SSRC list.
  rtcp[12] = 'R';
  rtcp[13] = 'E';
  rtcp[14] = 'M';
  rtcp[15] = 'B';
  rtcp[16] = 1;  // one SSRC in the list
  // Bitrate as 6-bit exponent + 18-bit mantissa.
  uint32_t exp = 0, mantissa = bitrate_bps;
  while (mantissa > 0x3ffff) {
    mantissa >>= 1;
    exp++;
  }
  rtcp[17] = (uint8_t)((exp << 2) | ((mantissa >> 16) & 0x3));
  rtcp[18] = (uint8_t)((mantissa >> 8) & 0xff);
  rtcp[19] = (uint8_t)(mantissa & 0xff);
  uint32_t media_ssrc = htonl(pc->remote_vssrc);
  memcpy(rtcp + 20, &media_ssrc, 4);
  int len = 24;
  dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, rtcp, &len);
  return agent_send(&pc->agent, rtcp, len);
}

// Send a COMPOUND RTCP Receiver Report (RR + SDES/CNAME) so the WebRTC sender
// treats us as a live, well-behaved receiver and keeps the video encoder
// running. A bare RR (no SDES) violates RFC 3550's compound-packet rule and
// some senders ignore it, then pause video for lack of receiver feedback.
// Stats come from the caller's jitter buffer; LSR/DLSR reflect the last SR.
int peer_connection_send_receiver_report(PeerConnection* pc, uint8_t fraction_lost,
                                         uint32_t cumulative_lost, uint32_t highest_seq_ext,
                                         uint32_t jitter) {
  if (pc == NULL || pc->state != PEER_CONNECTION_COMPLETED)
    return -1;
  static const char kCname[] = "green-nx";
  uint8_t rtcp[128];
  memset(rtcp, 0, sizeof(rtcp));
  uint32_t our_ssrc = htonl(SSRC_H264);

  // ---- Receiver Report (32 bytes) ----
  RtcpHeader* rr = (RtcpHeader*)rtcp;
  rr->version = 2;
  rr->rc = 1;  // one report block
  rr->type = RTCP_RR;
  rr->length = htons(7);  // (32/4) - 1
  memcpy(rtcp + 4, &our_ssrc, 4);
  uint32_t media_ssrc = htonl(pc->remote_vssrc);
  memcpy(rtcp + 8, &media_ssrc, 4);
  uint32_t flcnpl = htonl(((uint32_t)fraction_lost << 24) | (cumulative_lost & 0x00ffffff));
  memcpy(rtcp + 12, &flcnpl, 4);
  uint32_t be_ehsnr = htonl(highest_seq_ext);
  memcpy(rtcp + 16, &be_ehsnr, 4);
  uint32_t be_jitter = htonl(jitter);
  memcpy(rtcp + 20, &be_jitter, 4);
  uint32_t lsr = htonl(pc->rtcp_last_sr_lsr);
  memcpy(rtcp + 24, &lsr, 4);
  uint32_t dlsr = 0;
  if (pc->rtcp_last_sr_time) {
    // DLSR is in 1/65536 SECONDS; ports_get_epoch_time() is MILLISECONDS.
    // The old "(delta_ms << 16)" was 1000x too large: the sender's RTT math
    // (RTT = now - LSR - DLSR) went hugely negative -> wrapped to an absurd
    // RTT -> its bandwidth estimator saw a congested link and pinned the
    // encoder at the starvation floor. This one line was the low-bitrate bug.
    uint32_t delta_ms = ports_get_epoch_time() - pc->rtcp_last_sr_time;
    dlsr = htonl((uint32_t)(((uint64_t)delta_ms << 16) / 1000));
  }
  memcpy(rtcp + 28, &dlsr, 4);

  // ---- SDES with CNAME (makes the packet a valid compound RTCP) ----
  int sdes = 32;
  RtcpHeader* sh = (RtcpHeader*)(rtcp + sdes);
  sh->version = 2;
  sh->rc = 1;  // one chunk
  sh->type = RTCP_SDES;
  memcpy(rtcp + sdes + 4, &our_ssrc, 4);
  int p = sdes + 8;
  rtcp[p++] = 1;  // CNAME item
  rtcp[p++] = (uint8_t)(sizeof(kCname) - 1);
  memcpy(rtcp + p, kCname, sizeof(kCname) - 1);
  p += sizeof(kCname) - 1;
  rtcp[p++] = 0;  // end of item list
  while ((p - sdes) % 4 != 0) rtcp[p++] = 0;  // pad chunk to 32 bits
  sh->length = htons(((p - sdes) / 4) - 1);

  int len = p;
  dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, rtcp, &len);
  return agent_send(&pc->agent, rtcp, len);
}

int peer_connection_datachannel_send(PeerConnection* pc, char* message, size_t len) {
  return peer_connection_datachannel_send_sid(pc, message, len, 0);
}

int peer_connection_datachannel_send_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }
  if (pc->config.datachannel == DATA_CHANNEL_STRING)
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_STRING, sid);
  else
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_BINARY, sid);
}

// Force a WebRTC "string"/DOMSTRING frame regardless of the connection's
// default datachannel type. WebRTC peers key JSON control messages on this
// PPID; sending them as binary makes the peer ignore them.
int peer_connection_datachannel_send_text_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }
  return sctp_outgoing_data(&pc->sctp, message, len, PPID_STRING, sid);
}

int peer_connection_create_datachannel(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol) {
  return peer_connection_create_datachannel_sid(pc, channel_type, priority, reliability_parameter, label, protocol, 0);
}

int peer_connection_create_datachannel_sid(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol, uint16_t sid) {
  int rtrn = -1;

  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return rtrn;
  }

  //  0                   1                   2                   3
  //  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |  Message Type |  Channel Type |            Priority           |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                    Reliability Parameter                      |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |         Label Length          |       Protocol Length         |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                             Label                             |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                            Protocol                           |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  int msg_size = 12 + strlen(label) + strlen(protocol);
  uint16_t priority_big_endian = htons(priority);
  uint32_t reliability_big_endian = ntohl(reliability_parameter);
  uint16_t label_length = htons(strlen(label));
  uint16_t protocol_length = htons(strlen(protocol));
  char* msg = calloc(1, msg_size);
  if (!msg) {
    return rtrn;
  }

  msg[0] = DATA_CHANNEL_OPEN;
  msg[1] = channel_type;  // was left 0 -> every channel wrongly reliable/ordered
  memcpy(msg + 2, &priority_big_endian, sizeof(uint16_t));
  memcpy(msg + 4, &reliability_big_endian, sizeof(uint32_t));
  memcpy(msg + 8, &label_length, sizeof(uint16_t));
  memcpy(msg + 10, &protocol_length, sizeof(uint16_t));
  memcpy(msg + 12, label, strlen(label));
  memcpy(msg + 12 + strlen(label), protocol, strlen(protocol));

  rtrn = sctp_outgoing_data(&pc->sctp, msg, msg_size, PPID_CONTROL, sid);
  free(msg);
  // Record the label->sid mapping for locally-opened channels so that
  // peer_connection_lookup_sid()/_label() work for sends (the mapping was
  // previously only added when a DATA_CHANNEL_OPEN was *received*).
  if (rtrn >= 0) {
    sctp_add_stream_mapping(&pc->sctp, label, sid);
    // Register the policy only after the DCEP open above has gone out, so the
    // handshake itself is never sent unreliably. Channel type bit 7 = the
    // unordered variant; the low bits pick the partial-reliability flavour
    // (0x01 = retransmission-limited, which is what a WebRTC channel opened
    // with maxRetransmits uses).
    uint8_t flavour = channel_type & 0x7f;
    sctp_set_stream_reliability(&pc->sctp, sid, (channel_type & 0x80) != 0,
                                flavour == 0x01, reliability_parameter);
  }
  return rtrn;
}

static char* peer_connection_dtls_role_setup_value(DtlsSrtpRole d) {
  return d == DTLS_SRTP_ROLE_SERVER ? "a=setup:passive" : "a=setup:active";
}

int peer_connection_loop(PeerConnection* pc) {
  uint32_t ssrc = 0;
  memset(pc->agent_buf, 0, sizeof(pc->agent_buf));
  pc->agent_ret = -1;

  switch (pc->state) {
    case PEER_CONNECTION_NEW:
      break;

    case PEER_CONNECTION_CHECKING:
      if (agent_select_candidate_pair(&pc->agent) < 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
      } else if (agent_connectivity_check(&pc->agent) == 0) {
        STATE_CHANGED(pc, PEER_CONNECTION_CONNECTED);
      }
      break;

    case PEER_CONNECTION_CONNECTED:

      if (dtls_srtp_handshake(&pc->dtls_srtp, NULL) == 0) {
        LOGD("DTLS-SRTP handshake done");

        if (pc->config.datachannel) {
          LOGI("SCTP create socket");
          sctp_create_association(&pc->sctp, &pc->dtls_srtp);
          pc->sctp.userdata = pc->config.user_data;
        }

        STATE_CHANGED(pc, PEER_CONNECTION_COMPLETED);
      }
      break;
    case PEER_CONNECTION_COMPLETED:
      if ((pc->agent_ret = agent_recv(&pc->agent, pc->agent_buf, sizeof(pc->agent_buf))) > 0) {
        LOGD("agent_recv %d", pc->agent_ret);

        if (rtcp_probe(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTCP packet");
          dtls_srtp_decrypt_rtcp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);
          peer_connection_incoming_rtcp(pc, pc->agent_buf, pc->agent_ret);

        } else if (dtls_srtp_probe(pc->agent_buf)) {
          int received = pc->agent_ret;
          int ret = dtls_srtp_read(&pc->dtls_srtp, pc->temp_buf, sizeof(pc->temp_buf));
          LOGD("Got DTLS data %d", ret);

          if (ret > 0) {
            sctp_incoming_data(&pc->sctp, (char*)pc->temp_buf, ret);
          }

          // The bio consumed agent_ret; restore the datagram length so the
          // caller's drain loop still sees a processed packet, not an empty
          // socket.
          pc->agent_ret = received;

        } else if (rtp_packet_validate(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTP packet");

          dtls_srtp_decrypt_rtp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);

          ssrc = rtp_get_ssrc(pc->agent_buf);
          if (ssrc == pc->remote_assrc) {
            rtp_decoder_decode(&pc->artp_decoder, pc->agent_buf, pc->agent_ret);
          } else if (ssrc == pc->remote_vssrc) {
            rtp_decoder_decode(&pc->vrtp_decoder, pc->agent_buf, pc->agent_ret);
          } else {
            // Fallback demux by RTP payload type. libpeer normally routes media
            // by SSRC, learned from the answer's "a=ssrc:" lines. A home Xbox
            // console sometimes omits a=ssrc for the audio m-section, leaving
            // remote_assrc = 0, so every Opus packet (nonzero SSRC) matched
            // neither branch and was dropped -> no console audio. This offer
            // bundles exactly one recvonly audio (PT 111) and one video
            // (PT 102) stream (see sdp.c), so demux the stray packet by its
            // payload type and latch its SSRC; the next packet then takes the
            // fast path above.
            uint8_t pt = pc->agent_buf[1] & 0x7f;
            if (pt == 111) {
              LOGI("audio SSRC %" PRIu32 " latched by PT (no a=ssrc in answer)", ssrc);
              pc->remote_assrc = ssrc;
              rtp_decoder_decode(&pc->artp_decoder, pc->agent_buf, pc->agent_ret);
            } else if (pt == 102) {
              LOGI("video SSRC %" PRIu32 " latched by PT (no a=ssrc in answer)", ssrc);
              pc->remote_vssrc = ssrc;
              rtp_decoder_decode(&pc->vrtp_decoder, pc->agent_buf, pc->agent_ret);
            }
          }

        } else {
          LOGW("Unknown data");
        }
      }

      if (CONFIG_KEEPALIVE_TIMEOUT > 0 && (ports_get_epoch_time() - pc->agent.binding_request_time) > CONFIG_KEEPALIVE_TIMEOUT) {
        LOGI("binding request timeout");
        STATE_CHANGED(pc, PEER_CONNECTION_CLOSED);
      }

      break;
    case PEER_CONNECTION_FAILED:
      break;
    case PEER_CONNECTION_DISCONNECTED:
      break;
    case PEER_CONNECTION_CLOSED:
      break;
    default:
      break;
  }

  // >0 if an inbound packet was processed this call; lets the caller drain the
  // socket at full speed (one packet per call would drop most video packets).
  return pc->agent_ret;
}

void peer_connection_set_remote_description(PeerConnection* pc, const char* sdp, SdpType type) {
  char* start = (char*)sdp;
  char* line = NULL;
  char buf[256];
  char* val_start = NULL;
  uint32_t* ssrc = NULL;
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;
  int is_update = 0;
  Agent* agent = &pc->agent;

  while ((line = strstr(start, "\r\n"))) {
    line = strstr(start, "\r\n");
    strncpy(buf, start, line - start);
    buf[line - start] = '\0';

    if (strstr(buf, "a=setup:passive")) {
      role = DTLS_SRTP_ROLE_CLIENT;
    }

    if (strstr(buf, "a=fingerprint")) {
      strncpy(pc->dtls_srtp.remote_fingerprint, buf + 22, DTLS_SRTP_FINGERPRINT_LENGTH);
    }

    if (strstr(buf, "a=ice-ufrag") &&
        strlen(agent->remote_ufrag) != 0 &&
        (strncmp(buf + strlen("a=ice-ufrag:"), agent->remote_ufrag, strlen(agent->remote_ufrag)) == 0)) {
      is_update = 1;
    }

    if (strstr(buf, "m=video")) {
      ssrc = &pc->remote_vssrc;
    } else if (strstr(buf, "m=audio")) {
      ssrc = &pc->remote_assrc;
    }

    if ((val_start = strstr(buf, "a=ssrc:")) && ssrc) {
      *ssrc = strtoul(val_start + 7, NULL, 10);
      LOGD("SSRC: %" PRIu32, *ssrc);
    }

    start = line + 2;
  }

  if (is_update) {
    return;
  }

  agent_set_remote_description(&pc->agent, (char*)sdp);
  if (type == SDP_TYPE_ANSWER) {
    agent_update_candidate_pairs(&pc->agent);
    STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
  }
}

static const char* peer_connection_create_sdp(PeerConnection* pc, SdpType sdp_type) {
  char* description = (char*)pc->temp_buf;

  memset(pc->temp_buf, 0, sizeof(pc->temp_buf));
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;

  pc->sctp.connected = 0;

  switch (sdp_type) {
    case SDP_TYPE_OFFER:
      /* xCloud (and most WebRTC servers) answer a=setup:passive, i.e. they are
       * the DTLS server. The offerer must therefore be the DTLS client and
       * advertise a=setup:active, otherwise both sides wait for a ClientHello
       * and the handshake never starts. */
      role = DTLS_SRTP_ROLE_CLIENT;
      agent_clear_candidates(&pc->agent);
      pc->agent.mode = AGENT_MODE_CONTROLLING;
      break;
    case SDP_TYPE_ANSWER:
      role = DTLS_SRTP_ROLE_CLIENT;
      pc->agent.mode = AGENT_MODE_CONTROLLED;
      break;
    default:
      break;
  }

  dtls_srtp_reset_session(&pc->dtls_srtp);
  dtls_srtp_init(&pc->dtls_srtp, role, pc);
  pc->dtls_srtp.udp_recv = peer_connection_dtls_srtp_recv;
  pc->dtls_srtp.udp_send = peer_connection_dtls_srtp_send;

  memset(pc->sdp, 0, sizeof(pc->sdp));
  // TODO: check if we have video or audio codecs
  sdp_create(pc->sdp,
             pc->config.video_codec != CODEC_NONE,
             pc->config.audio_codec != CODEC_NONE,
             pc->config.datachannel);

  agent_create_ice_credential(&pc->agent);
  sdp_append(pc->sdp, "a=ice-ufrag:%s", pc->agent.local_ufrag);
  sdp_append(pc->sdp, "a=ice-pwd:%s", pc->agent.local_upwd);
  sdp_append(pc->sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
  sdp_append(pc->sdp, peer_connection_dtls_role_setup_value(role));

  if (pc->config.video_codec == CODEC_H264) {
    sdp_append_h264(pc->sdp);
  }

  switch (pc->config.audio_codec) {
    case CODEC_PCMA:
      sdp_append_pcma(pc->sdp);
      break;
    case CODEC_PCMU:
      sdp_append_pcmu(pc->sdp);
      break;
    case CODEC_OPUS:
      sdp_append_opus(pc->sdp);
    default:
      break;
  }

  if (pc->config.datachannel) {
    sdp_append_datachannel(pc->sdp);
  }

  pc->b_local_description_created = 1;

  agent_gather_candidate(&pc->agent, NULL, NULL, NULL);  // host address
  for (int i = 0; i < sizeof(pc->config.ice_servers) / sizeof(pc->config.ice_servers[0]); ++i) {
    if (pc->config.ice_servers[i].urls) {
      LOGI("ice server: %s", pc->config.ice_servers[i].urls);
      agent_gather_candidate(&pc->agent, pc->config.ice_servers[i].urls, pc->config.ice_servers[i].username, pc->config.ice_servers[i].credential);
    }
  }

  agent_get_local_description(&pc->agent, description, sizeof(pc->temp_buf));
  sdp_append(pc->sdp, description);

  if (pc->onicecandidate) {
    pc->onicecandidate(pc->sdp, pc->config.user_data);
  }

  return pc->sdp;
}

const char* peer_connection_create_offer(PeerConnection* pc) {
  return peer_connection_create_sdp(pc, SDP_TYPE_OFFER);
}

const char* peer_connection_create_answer(PeerConnection* pc) {
  const char* sdp = peer_connection_create_sdp(pc, SDP_TYPE_ANSWER);
  agent_update_candidate_pairs(&pc->agent);
  STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
  return sdp;
}

int peer_connection_send_rtcp_pil(PeerConnection* pc, uint32_t ssrc) {
  int ret = -1;
  uint8_t plibuf[128];
  rtcp_get_pli(plibuf, 12, ssrc);

  // TODO: encrypt rtcp packet
  // guint size = 12;
  // dtls_transport_encrypt_rctp_packet(pc->dtls_transport, plibuf, &size);
  // ret = nice_agent_send(pc->nice_agent, pc->stream_id, pc->component_id, size, (gchar*)plibuf);

  return ret;
}

// callbacks
void peer_connection_on_connected(PeerConnection* pc, void (*on_connected)(void* userdata)) {
  pc->on_connected = on_connected;
}

void peer_connection_on_receiver_packet_loss(PeerConnection* pc,
                                             void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* userdata)) {
  pc->on_receiver_packet_loss = on_receiver_packet_loss;
}

void peer_connection_onicecandidate(PeerConnection* pc, void (*onicecandidate)(char* sdp, void* userdata)) {
  pc->onicecandidate = onicecandidate;
}

void peer_connection_oniceconnectionstatechange(PeerConnection* pc,
                                                void (*oniceconnectionstatechange)(PeerConnectionState state, void* userdata)) {
  pc->oniceconnectionstatechange = oniceconnectionstatechange;
}

void peer_connection_ondatachannel(PeerConnection* pc,
                                   void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid),
                                   void (*onopen)(void* userdata),
                                   void (*onclose)(void* userdata)) {
  if (pc) {
    sctp_onopen(&pc->sctp, onopen);
    sctp_onclose(&pc->sctp, onclose);
    sctp_onmessage(&pc->sctp, onmessage);
  }
}

int peer_connection_lookup_sid(PeerConnection* pc, const char* label, uint16_t* sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (strncmp(pc->sctp.stream_table[i].label, label, sizeof(pc->sctp.stream_table[i].label)) == 0) {
      *sid = pc->sctp.stream_table[i].sid;
      return 0;
    }
  }
  return -1;  // Not found
}

char* peer_connection_lookup_sid_label(PeerConnection* pc, uint16_t sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (pc->sctp.stream_table[i].sid == sid) {
      return pc->sctp.stream_table[i].label;
    }
  }
  return NULL;  // Not found
}

int peer_connection_add_ice_candidate(PeerConnection* pc, char* candidate) {
  Agent* agent = &pc->agent;
  if (agent->remote_candidates_count >= AGENT_MAX_CANDIDATES) {
    LOGW("remote candidate list full (%d), ignoring: %s", AGENT_MAX_CANDIDATES, candidate);
    return -1;
  }
  if (ice_candidate_from_description(&agent->remote_candidates[agent->remote_candidates_count], candidate, candidate + strlen(candidate)) != 0) {
    return -1;
  }
  LOGD("Add candidate: %s", candidate);
  agent->remote_candidates_count++;
  return 0;
}
