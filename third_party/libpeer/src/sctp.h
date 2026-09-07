#ifndef SCTP_H_
#define SCTP_H_

#include "config.h"
#include "dtls_srtp.h"
#include "utils.h"

typedef enum DecpMsgType {

  DATA_CHANNEL_OPEN = 0x03,
  DATA_CHANNEL_ACK = 0x02,

} DecpMsgType;

typedef enum DataChannelPpid {

  DATA_CHANNEL_PPID_CONTROL = 50,
  DATA_CHANNEL_PPID_DOMSTRING = 51,
  DATA_CHANNEL_PPID_BINARY_PARTIAL = 52,
  DATA_CHANNEL_PPID_BINARY = 53,
  DATA_CHANNEL_PPID_DOMSTRING_PARTIAL = 54

} DataChannelPpid;

#if !CONFIG_USE_USRSCTP

typedef struct SctpChunkParam {
  uint16_t type;
  uint16_t length;
  uint8_t value[0];

} SctpChunkParam;

typedef enum SctpParamType {

  SCTP_PARAM_STATE_COOKIE = 7,

} SctpParamType;

typedef enum SctpHeaderType {

  SCTP_DATA = 0,
  SCTP_INIT = 1,
  SCTP_INIT_ACK = 2,
  SCTP_SACK = 3,
  SCTP_HEARTBEAT = 4,
  SCTP_HEARTBEAT_ACK = 5,
  SCTP_ABORT = 6,
  SCTP_SHUTDOWN = 7,
  SCTP_SHUTDOWN_ACK = 8,
  SCTP_ERROR = 9,
  SCTP_COOKIE_ECHO = 10,
  SCTP_COOKIE_ACK = 11,
  SCTP_ECNE = 12,
  SCTP_CWR = 13,
  SCTP_SHUTDOWN_COMPLETE = 14,
  SCTP_AUTH = 15,
  SCTP_ASCONF_ACK = 128,
  SCTP_ASCONF = 130,
  SCTP_FORWARD_TSN = 192

} SctpHeaderType;

typedef struct SctpChunkCommon {
  uint8_t type;
  uint8_t flags;
  uint16_t length;

} SctpChunkCommon;

typedef struct SctpForwardTsnChunk {
  SctpChunkCommon common;
  uint32_t new_cumulative_tsn;
  uint16_t stream_number;
  uint16_t stream_sequence_number;

} SctpForwardTsnChunk;

typedef struct SctpHeader {
  uint16_t source_port;
  uint16_t destination_port;
  uint32_t verification_tag;
  uint32_t checksum;

} SctpHeader;

typedef struct SctpPacket {
  SctpHeader header;
  uint8_t chunks[0];

} SctpPacket;

typedef struct SctpSackChunk {
  SctpChunkCommon common;
  uint32_t cumulative_tsn_ack;
  uint32_t a_rwnd;
  uint16_t number_of_gap_ack_blocks;
  uint16_t number_of_dup_tsns;
  uint8_t blocks[0];

} SctpSackChunk;

typedef struct SctpDataChunk {
  uint8_t type;
  uint8_t iube;
  uint16_t length;
  uint32_t tsn;
  uint16_t sid;
  uint16_t sqn;
  uint32_t ppid;
  uint8_t data[0];

} SctpDataChunk;

typedef struct SctpInitChunk {
  SctpChunkCommon common;
  uint32_t initiate_tag;
  uint32_t a_rwnd;
  uint16_t number_of_outbound_streams;
  uint16_t number_of_inbound_streams;
  uint32_t initial_tsn;
  SctpChunkParam param[0];

} SctpInitChunk;

typedef struct SctpCookieEchoChunk {
  SctpChunkCommon common;
  uint8_t cookie[0];
} SctpCookieEchoChunk;

#endif

typedef enum SctpDataPpid {

  PPID_CONTROL = 50,
  PPID_STRING = 51,
  PPID_BINARY = 53,
  PPID_STRING_EMPTY = 56,
  PPID_BINARY_EMPTY = 57

} SctpDataPpid;

#define SCTP_MAX_STREAMS 16

typedef struct {
  char label[32];  // Stream label
  uint16_t sid;    // Stream ID
  // Per-stream delivery policy, taken from the DCEP channel type the channel
  // was opened with. Without this every send went out reliable+ordered no
  // matter what the channel negotiated: a high-rate unreliable channel (game
  // input at 125 Hz) then piles undroppable frames up behind a loss, filling
  // the send buffer (sendv EAGAIN storms) and forcing the peer to wait for
  // ancient frames before it sees current ones.
  uint8_t unordered;         // send with SCTP_UNORDERED
  uint8_t partial_reliable;  // apply the PR-SCTP retransmission limit below
  uint32_t max_retransmits;  // pr_value for SCTP_PR_SCTP_RTX
} SctpStreamEntry;

typedef struct Sctp {
  struct socket* sock;

  int local_port;
  int remote_port;
  int connected;
  uint32_t verification_tag;
  uint32_t tsn;
  DtlsSrtp* dtls_srtp;
  int stream_count;
  SctpStreamEntry stream_table[SCTP_MAX_STREAMS];

  /* datachannel */
  void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid);
  void (*onopen)(void* userdata);
  void (*onclose)(void* userdata);

  void* userdata;
  uint8_t buf[CONFIG_MTU];
} Sctp;

int sctp_create_association(Sctp* sctp, DtlsSrtp* dtls_srtp);

void sctp_destroy_association(Sctp* sctp);

void sctp_usrsctp_init();

void sctp_usrsctp_deinit();

int sctp_is_connected(Sctp* sctp);

void sctp_incoming_data(Sctp* sctp, char* buf, size_t len);

int sctp_outgoing_data(Sctp* sctp, char* buf, size_t len, SctpDataPpid ppid, uint16_t sid);

void sctp_add_stream_mapping(Sctp* sctp, const char* label, uint16_t sid);

// Applies a channel's negotiated delivery policy to everything sent on that
// stream afterwards (see SctpStreamEntry). Call once the channel is open: the
// DCEP handshake itself must stay reliable and ordered.
void sctp_set_stream_reliability(Sctp* sctp, uint16_t sid, int unordered, int partial_reliable, uint32_t max_retransmits);

void sctp_onmessage(Sctp* sctp, void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid));

void sctp_onopen(Sctp* sctp, void (*onopen)(void* userdata));

void sctp_onclose(Sctp* sctp, void (*onclose)(void* userdata));

#endif  // SCTP_H_
