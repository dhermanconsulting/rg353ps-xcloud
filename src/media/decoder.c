#include "decoder.h"

#include <stdio.h>
#include <string.h>

/*
 * Shared codec-context setup. Threading: slice threading adds no latency,
 * frame threading delays output by (thread_count - 1) frames. Ask for slice
 * and read back active_thread_type after the first frame to find out what we
 * actually got - libavcodec silently ignores what the bitstream cannot
 * support.
 */
static int open_codec(struct decoder *d, const AVCodec *codec)
{
	switch (d->threading) {
	case DECODER_THREADS_FRAME2:
	case DECODER_THREADS_FRAME3:
		d->ctx->thread_count = d->threading == DECODER_THREADS_FRAME2 ? 2 : 3;
		d->ctx->thread_type = FF_THREAD_FRAME;
		/* LOW_DELAY fails libavcodec's frame_threading_supported()
		 * gate and silently demotes to slice threading, which on a
		 * single-slice stream is one thread. It buys nothing here
		 * anyway: Constrained Baseline has no B-frames, so output is
		 * never reordered. */
		break;
	case DECODER_THREADS_SLICE:
	default:
		d->ctx->thread_count = 4;
		d->ctx->thread_type = FF_THREAD_SLICE;
		d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
		break;
	}
	/* No AV_CODEC_FLAG2_FAST: on h264 its only effect is non-conformant
	 * deblocking across slice edges, which drifts from the encoder's
	 * reconstruction until the next IDR. It saves nothing on this
	 * stream. */

	switch (d->skip_loop) {
	case DECODER_LOOP_SKIP_NONREF:
		d->ctx->skip_loop_filter = AVDISCARD_NONREF;
		break;
	case DECODER_LOOP_SKIP_NONKEY:
		d->ctx->skip_loop_filter = AVDISCARD_NONKEY;
		break;
	case DECODER_LOOP_SKIP_ALL:
		d->ctx->skip_loop_filter = AVDISCARD_ALL;
		break;
	case DECODER_LOOP_ALL_FRAMES:
	default:
		break;
	}

	/* Report what this build can do rather than assuming: upstream
	 * removed lowres from the h264 decoder, so max_lowres is expected to
	 * be 0 here, and a request above it is clamped rather than failed. */
	if (d->lowres) {
		fprintf(stderr, "decoder: lowres %d requested, codec max %d\n",
			d->lowres, codec->max_lowres);
		d->ctx->lowres = d->lowres <= codec->max_lowres
					 ? d->lowres
					 : codec->max_lowres;
	}

	if (avcodec_open2(d->ctx, codec, NULL) < 0) {
		fprintf(stderr, "decoder: avcodec_open2 failed\n");
		return -1;
	}

	d->pkt = av_packet_alloc();
	d->frame = av_frame_alloc();
	if (!d->pkt || !d->frame)
		return -1;
	return 0;
}

static void report_threading(struct decoder *d)
{
	if (d->reported_threading)
		return;
	d->reported_threading = 1;
	fprintf(stderr, "decoder: active_thread_type=%s\n",
		d->ctx->active_thread_type == FF_THREAD_SLICE
			? "SLICE (no added latency)"
		: d->ctx->active_thread_type == FF_THREAD_FRAME
			? "FRAME (adds thread_count-1 frames of latency)"
			: "none");
}

/*
 * Streaming path: raw Annex-B, one whole access unit per call. No container
 * and no parser - the jitter buffer already emits exactly one complete AU.
 */
int decoder_open_h264(struct decoder *d, enum decoder_threading threading)
{
	return decoder_open_h264_opts(d, threading, DECODER_LOOP_ALL_FRAMES, 0);
}

int decoder_open_h264_opts(struct decoder *d, enum decoder_threading threading,
			   enum decoder_skip_loop skip_loop, int lowres)
{
	const AVCodec *codec;

	memset(d, 0, sizeof(*d));
	d->stream_index = -1;
	d->threading = threading;
	d->skip_loop = skip_loop;
	d->lowres = lowres;

	codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		fprintf(stderr, "decoder: no H.264 decoder\n");
		return -1;
	}
	d->ctx = avcodec_alloc_context3(codec);
	if (!d->ctx)
		return -1;

	/* Dimensions arrive in the stream's SPS, and xCloud can change
	 * resolution mid-session, so never pin width/height here. */
	if (open_codec(d, codec))
		return -1;

	fprintf(stderr, "decoder: %s (raw annex-b), threads=%d requested=%s\n",
		codec->name, d->ctx->thread_count,
		d->ctx->thread_type == FF_THREAD_FRAME ? "frame" : "slice");
	return 0;
}

int decoder_send(struct decoder *d, const uint8_t *au, size_t size,
		 int64_t pts)
{
	int rc;

	if (av_new_packet(d->pkt, (int)size) < 0)
		return -1;
	memcpy(d->pkt->data, au, size);
	d->pkt->pts = pts;
	d->pkt->dts = pts;
	rc = avcodec_send_packet(d->ctx, d->pkt);
	av_packet_unref(d->pkt);
	if (rc < 0 && rc != AVERROR(EAGAIN))
		fprintf(stderr, "decoder: send_packet %d\n", rc);
	return rc;
}

int decoder_receive(struct decoder *d)
{
	int rc = avcodec_receive_frame(d->ctx, d->frame);

	if (rc == 0) {
		report_threading(d);
		return 1;
	}
	if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF)
		fprintf(stderr, "decoder: receive_frame %d\n", rc);
	return 0;
}

int decoder_decode_au(struct decoder *d, const uint8_t *au, size_t size)
{
	int rc;

	if (size > 0) {
		/*
		 * One memcpy per access unit, about 100 KB at 720p and far
		 * below the decode cost. Referencing the caller's buffer
		 * across send_packet instead would need refcount bookkeeping
		 * we do not have, and the AU is freed the moment we return.
		 */
		if (av_new_packet(d->pkt, (int)size) < 0)
			return -1;
		memcpy(d->pkt->data, au, size);
		rc = avcodec_send_packet(d->ctx, d->pkt);
		av_packet_unref(d->pkt);
		if (rc < 0 && rc != AVERROR(EAGAIN)) {
			/* A corrupt unit must not kill the stream: the engine
			 * notices the decode stall and asks for a keyframe. */
			fprintf(stderr, "decoder: send_packet %d\n", rc);
			return 0;
		}
	}

	rc = avcodec_receive_frame(d->ctx, d->frame);
	if (rc == 0) {
		report_threading(d);
		return 1;
	}
	if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
		return 0;
	fprintf(stderr, "decoder: receive_frame %d\n", rc);
	return 0;
}

int decoder_open_file(struct decoder *d, const char *path)
{
	const AVCodec *codec;
	int rc;

	memset(d, 0, sizeof(*d));
	d->stream_index = -1;

	rc = avformat_open_input(&d->fmt, path, NULL, NULL);
	if (rc < 0) {
		fprintf(stderr, "decoder: open %s failed (%d)\n", path, rc);
		return -1;
	}
	if (avformat_find_stream_info(d->fmt, NULL) < 0) {
		fprintf(stderr, "decoder: no stream info\n");
		return -1;
	}
	for (unsigned i = 0; i < d->fmt->nb_streams; i++) {
		if (d->fmt->streams[i]->codecpar->codec_type ==
		    AVMEDIA_TYPE_VIDEO) {
			d->stream_index = (int)i;
			break;
		}
	}
	if (d->stream_index < 0) {
		fprintf(stderr, "decoder: no video stream\n");
		return -1;
	}

	codec = avcodec_find_decoder(
		d->fmt->streams[d->stream_index]->codecpar->codec_id);
	if (!codec) {
		fprintf(stderr, "decoder: no decoder for stream\n");
		return -1;
	}

	d->ctx = avcodec_alloc_context3(codec);
	if (!d->ctx)
		return -1;
	if (avcodec_parameters_to_context(
		    d->ctx, d->fmt->streams[d->stream_index]->codecpar) < 0)
		return -1;

	if (open_codec(d, codec))
		return -1;

	fprintf(stderr, "decoder: %s %dx%d, threads=%d requested=slice\n",
		codec->name, d->ctx->width, d->ctx->height,
		d->ctx->thread_count);
	return 0;
}

int decoder_next_frame(struct decoder *d)
{
	int rc;

	for (;;) {
		rc = avcodec_receive_frame(d->ctx, d->frame);
		if (rc == 0) {
			report_threading(d);
			return 1;
		}
		if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
			fprintf(stderr, "decoder: receive_frame %d\n", rc);
			return -1;
		}
		if (rc == AVERROR_EOF)
			return 0;

		/* Need more input. */
		if (d->eof) {
			avcodec_send_packet(d->ctx, NULL);  /* flush */
			d->eof = 2;
			continue;
		}
		rc = av_read_frame(d->fmt, d->pkt);
		if (rc < 0) {
			d->eof = 1;
			continue;
		}
		if (d->pkt->stream_index == d->stream_index)
			avcodec_send_packet(d->ctx, d->pkt);
		av_packet_unref(d->pkt);
	}
}

void decoder_close(struct decoder *d)
{
	if (d->frame)
		av_frame_free(&d->frame);
	if (d->pkt)
		av_packet_free(&d->pkt);
	if (d->ctx)
		avcodec_free_context(&d->ctx);
	if (d->fmt)
		avformat_close_input(&d->fmt);
	memset(d, 0, sizeof(*d));
}
