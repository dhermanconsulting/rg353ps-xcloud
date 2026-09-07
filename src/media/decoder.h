/*
 * Software H.264 decode via libavcodec.
 *
 * Hardware decode (h264_rkmpp) is deliberately not used: on this firmware the
 * userspace librockchip_mpp writes a register layout the mpp_rkvdec2 driver
 * does not expect, which faults the VPU. Software decode measured 2.54x
 * realtime at 720p60 on this SoC, so it is sufficient.
 */
#ifndef XCLOUD_DECODER_H
#define XCLOUD_DECODER_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

/*
 * Threading model. Measured 2026-09-04: xCloud sends ONE slice per frame, so
 * slice threading is single-threaded decode whatever thread_count says --
 * 11-12 ms per 720p frame with spikes past 20 ms, on one of four cores.
 * Frame threading pipelines consecutive frames across cores. It releases
 * frame N only when packet N+1 arrives (thread_count-1 frames of delay), but
 * at 60 fps that is 16.7 ms against a 12 ms decode, so the added latency is
 * about 5 ms and the throughput headroom roughly doubles.
 */
enum decoder_threading {
	DECODER_THREADS_SLICE,   /* single-threaded on this stream */
	DECODER_THREADS_FRAME2,  /* 2 frames in flight */
	DECODER_THREADS_FRAME3,  /* 3 frames in flight */
};

/*
 * Deblocking. H.264's in-loop filter is a large slice of decode time, and
 * this panel halves the picture before anyone sees it, so a good deal of
 * what the filter reconstructs is thrown away by the downscale regardless.
 * Skipping it is not free -- the filtered frame is what later frames predict
 * from, so error accumulates until the next IDR -- which is why the levels
 * below run from "only where nothing predicts from it" upwards.
 */
enum decoder_skip_loop {
	DECODER_LOOP_ALL_FRAMES,  /* default: filter everything, conformant */
	DECODER_LOOP_SKIP_NONREF, /* frames nothing predicts from */
	DECODER_LOOP_SKIP_NONKEY, /* everything but keyframes */
	DECODER_LOOP_SKIP_ALL,    /* never filter */
};

struct decoder {
	AVFormatContext *fmt;
	AVCodecContext *ctx;
	AVPacket *pkt;
	AVFrame *frame;
	int stream_index;
	int eof;
	int reported_threading;
	enum decoder_threading threading;
	enum decoder_skip_loop skip_loop;
	/* Decode at 1/2 or 1/4 size, if this build's h264 decoder supports it
	 * at all (upstream dropped lowres for h264 years ago; max_lowres is
	 * reported at open so we stop guessing). 0 = full size. */
	int lowres;
};

int decoder_open_file(struct decoder *d, const char *path);

/*
 * Open a decoder for raw Annex-B H.264 fed a whole access unit at a time
 * (the streaming path: no container, no AVFormatContext, no parser -- the
 * jitter buffer already emits exactly one complete AU per call).
 */
int decoder_open_h264(struct decoder *d, enum decoder_threading threading);
/* As above, plus the deblocking and lowres knobs. */
int decoder_open_h264_opts(struct decoder *d, enum decoder_threading threading,
			   enum decoder_skip_loop skip_loop, int lowres);

/*
 * Submit one Annex-B access unit and try to take a frame back.
 * Returns 1 with d->frame valid, 0 if the decoder needs more input, negative
 * on error. Cheap to call with size 0 to drain a frame already in flight.
 */
int decoder_decode_au(struct decoder *d, const uint8_t *au, size_t size);

/*
 * Submit one access unit without taking output. Returns 0 if accepted,
 * AVERROR(EAGAIN) if the decoder wants its output drained first (drain with
 * decoder_receive and resend the SAME unit), negative on error. `pts` is
 * carried through to the frame's pts untouched; the caller uses it for the
 * unit's arrival time so end-to-end latency can be measured at the flip.
 */
int decoder_send(struct decoder *d, const uint8_t *au, size_t size,
		 int64_t pts);

/*
 * Take another frame the decoder already holds, without new input. Returns
 * 1 with d->frame valid, 0 if there is none. With frame threading a single
 * access unit can leave more than one frame ready.
 */
int decoder_receive(struct decoder *d);

/*
 * Decode until a frame is available.
 * Returns 1 with d->frame valid, 0 at end of stream, negative on error.
 */
int decoder_next_frame(struct decoder *d);

void decoder_close(struct decoder *d);

#endif /* XCLOUD_DECODER_H */
