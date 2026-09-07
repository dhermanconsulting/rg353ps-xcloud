/*
 * mkstream: a synthetic .xcau recording for the host simulator.
 *
 * The only real recording (rec-wreckfest.xcau, docs/VIDEO-PACING.md) lives
 * on the handheld, and scripts/sim.sh needs SOMETHING to replay on a machine
 * that has never seen the device. This makes a stream that looks like
 * xCloud's on every axis the client cares about:
 *
 *   - 1280x720 H.264 Constrained Baseline, ONE slice per frame (the
 *     `slices=1-1` in the pace| line that decides slice threading is
 *     useless), no B-frames, ~8 Mbps, 60 fps, an IDR every 10 s: xCloud
 *     sends IDRs only on request, so they are rare but present.
 *   - Annex-B access units exactly as the jitter buffer emits them, SPS and
 *     PPS in-band ahead of each IDR (libx264 does that by itself when the
 *     global-header flag is left off).
 *   - 20 ms Opus packets, 48 kHz stereo, 96 kbps, RTP sequence numbers
 *     incrementing per packet, of a slow chirp so a gap is audible.
 *   - Arrival times on an ideal cadence (16.667 ms video, 20 ms audio) from
 *     a start offset, plus optional Gaussian jitter (-jitter <ms>), clamped
 *     so neither stream is ever reordered.
 *
 * Content is a moving test scene -- bouncing shapes of distinct luma, a
 * scrolling text band drawn with the client's own FreeType renderer
 * (src/ui/text.c), a slowly panning noise texture, a frame counter big
 * enough to read in a 640x360 PNG -- so the decode cost resembles a game
 * rather than a static card.
 *
 * Encodes with libavcodec's libx264 and libopus; the Debian bullseye FFmpeg
 * in docker/Dockerfile.host-bullseye is built with both (libx264-160 and
 * libopus0 are dependencies of libavcodec58 there). HOST ONLY: built by
 * scripts/build-host.sh into out/host/mkstream, run by scripts/mkstream.sh,
 * never part of the device build.
 *
 *   out/host/mkstream <out.xcau> <seconds> [-jitter ms]
 *
 * The file is written directly in the format documented in
 * src/media/au_recorder.hpp (the AuRecorder's queue drops records when its
 * writer thread falls behind, which is right for a live stream and wrong for
 * a generator), then read back with AuReader so the two can never drift
 * apart unnoticed.
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include "../src/ui/text.h"
}

#include "../src/media/au_recorder.hpp"

namespace {

constexpr int kW = 1280, kH = 720;
constexpr int kFps = 60;
constexpr double kVideoPeriodMs = 1000.0 / kFps;  /* 16.667 */
constexpr int kAudioRate = 48000;
constexpr int kAudioPacketMs = 20;
constexpr int kAudioBitrate = 96000;
constexpr int kVideoBitrate = 8000000;
/*
 * IDR interval, frames. xCloud itself sends IDRs only on request, but the
 * replay harness has no request path: after a simulated loss the feeder
 * drops units until the next IDR in the file, so this interval is the
 * length of every simulated freeze. One second (a 0.5 s freeze on average)
 * is what a live PLI round trip and its 300 ms throttle cost; ten seconds
 * turned a 1 % loss run into a black screen. -keyint overrides.
 */
int kKeyint = kFps;
/* Arrival clock origin: SDL_GetTicks64 on the device is time since the
 * process started, so a real recording never begins at 0. Neither does
 * this one, which keeps a "first_ts == 0" sentinel from ever biting. */
constexpr uint64_t kStartMs = 100000;
constexpr uint16_t kAudioSeq0 = 0x1000;
/* The band's text runs at 60 fps, so a bounce, a scroll and a pan all have
 * to be sub-second events to read as motion in a 20 s clip. */
constexpr int kBandY = 600, kBandH = 92;

struct Record {
	uint8_t kind;   /* AuRecorder::Video or ::Audio */
	uint32_t seq;
	double t_ms;    /* arrival time, before rounding */
	std::vector<uint8_t> data;
};

/* ---- the scene ---------------------------------------------------------- */

struct Shape {
	double x, y, vx, vy;
	int r;
	bool square;
	uint8_t luma, cb, cr;
};

class Scene {
public:
	~Scene()
	{
		if (counter_)
			text_destroy(counter_);
		if (band_)
			text_destroy(band_);
		if (small_)
			text_destroy(small_);
	}

	bool init()
	{
		/* The client's own renderer, so a glyph costs the encoder what
		 * a HUD costs it. text.c falls back to the container's
		 * /usr/share/fonts/truetype/dejavu/ when the device path is
		 * missing. */
		counter_ = text_create(TEXT_FONT_MONO, 104);
		band_ = text_create(TEXT_FONT_DEFAULT, 52);
		small_ = text_create(TEXT_FONT_MONO, 34);
		if (!counter_ || !band_ || !small_) {
			std::fprintf(stderr, "mkstream: cannot load the DejaVu fonts"
					     " (%s and the text.c fallbacks)\n",
				     TEXT_FONT_DEFAULT);
			return false;
		}

		/* Low-contrast noise: enough detail that the encoder cannot
		 * coast on flat blocks, not so much that it eats the whole
		 * bit budget and the shapes turn to mush. Panning a texture
		 * with no matchable structure is close to the worst case for
		 * motion estimation, which is the point. */
		noise_.resize((size_t)kNoiseW * kNoiseH);
		uint32_t s = 0x9e3779b9u;
		for (uint8_t &v : noise_) {
			s = s * 1664525u + 1013904223u;
			v = (uint8_t)(72 + ((s >> 24) % 25));
		}

		/* Eight shapes, luma steps of 30 so each is distinguishable in a
		 * greyscale PNG, colours so the chroma planes carry motion too. */
		static const uint8_t luma[8] = {235, 205, 175, 145, 115, 85, 55, 30};
		static const uint8_t cb[8] = {90, 54, 240, 16, 226, 128, 160, 100};
		static const uint8_t cr[8] = {240, 34, 110, 146, 226, 128, 90, 180};
		std::mt19937 rng(20260905u);
		std::uniform_real_distribution<double> ang(0.0, 2.0 * M_PI);
		for (int i = 0; i < 8; i++) {
			Shape sh;
			sh.r = 36 + i * 6;
			sh.square = (i & 1) != 0;
			sh.x = 120 + i * 140;
			sh.y = 220 + (i % 3) * 120;
			double a = ang(rng), speed = 3.0 + i * 0.8;
			sh.vx = speed * std::cos(a);
			sh.vy = speed * std::sin(a);
			sh.luma = luma[i];
			sh.cb = cb[i];
			sh.cr = cr[i];
			shapes_.push_back(sh);
		}

		band_text_ = "XCLOUD SYNTHETIC STREAM  1280x720 60 fps H.264"
			     " Constrained Baseline 8 Mbps, one slice per frame,"
			     " IDR every 10 s  *  The quick brown fox jumps over"
			     " the lazy dog  *  0123456789  *  ";
		band_w_ = text_measure(band_, band_text_.c_str());
		return true;
	}

	void render(AVFrame *f, int n)
	{
		uint8_t *Y = f->data[0];
		const int py = f->linesize[0];

		/* Background: the noise tile panned on two slow sinusoids, at
		 * integer offsets so it is a pure translation. */
		int px = (int)(256 + 255 * std::sin(n * 2.0 * M_PI / 900.0));
		int pyy = (int)(128 + 127 * std::cos(n * 2.0 * M_PI / 1300.0));
		for (int y = 0; y < kH; y++)
			std::memcpy(Y + (size_t)y * py,
				    &noise_[(size_t)(y + pyy) * kNoiseW + px], kW);
		for (int y = 0; y < kH / 2; y++) {
			std::memset(f->data[1] + (size_t)y * f->linesize[1], 132, kW / 2);
			std::memset(f->data[2] + (size_t)y * f->linesize[2], 122, kW / 2);
		}

		/* Shapes bounce inside the area above the band. A dark rim
		 * gives every one a hard edge, which is what costs bits. */
		for (Shape &s : shapes_) {
			s.x += s.vx;
			s.y += s.vy;
			if (s.x < s.r) { s.x = s.r; s.vx = -s.vx; }
			if (s.x > kW - 1 - s.r) { s.x = kW - 1 - s.r; s.vx = -s.vx; }
			if (s.y < s.r) { s.y = s.r; s.vy = -s.vy; }
			if (s.y > kBandY - 1 - s.r) { s.y = kBandY - 1 - s.r; s.vy = -s.vy; }
			draw(f, s, s.r + 4, 16, 128, 128);
			draw(f, s, s.r, s.luma, s.cb, s.cr);
		}

		/* The scrolling band: 4 px per frame, a seamless ribbon made of
		 * three copies of the text so the wrap is invisible. */
		rect(f, 0, kBandY, kW, kBandH, 22, 128, 128);
		const int period = band_w_ + 240;
		int x = kW - (n * 4) % period;
		for (int k = -1; k <= 1; k++)
			text_draw(band_, Y, py, kW, kH, x + k * period, kBandY + 64,
				  band_text_.c_str(), 225);

		/* Frame counter, 104 px mono: still legible at half size in a
		 * 640x360 screenshot. Below it the time and frames to the next
		 * IDR, and a bar that fills over each 10 s GOP so a keyframe
		 * boundary is visible in a still. */
		rect(f, 24, 24, 640, 176, 12, 128, 128);
		char buf[80];
		std::snprintf(buf, sizeof(buf), "%06d", n);
		text_draw(counter_, Y, py, kW, kH, 40, 120, buf, 235);
		std::snprintf(buf, sizeof(buf), "t=%8.3fs  idr in %3d",
			      n / (double)kFps, kKeyint - n % kKeyint);
		text_draw(small_, Y, py, kW, kH, 40, 164, buf, 190);
		rect(f, 40, 178, 608, 10, 48, 128, 128);
		rect(f, 40, 178, 608 * (n % kKeyint) / kKeyint, 10, 200, 128, 128);
	}

private:
	static constexpr int kNoiseW = kW + 512, kNoiseH = kH + 256;

	/* One luma row of a shape plus, on even rows, the chroma under it. */
	static void span(AVFrame *f, int y, int x0, int x1, uint8_t luma,
			 uint8_t cb, uint8_t cr)
	{
		if (y < 0 || y >= kH)
			return;
		x0 = std::max(0, x0);
		x1 = std::min(kW - 1, x1);
		if (x0 > x1)
			return;
		std::memset(f->data[0] + (size_t)y * f->linesize[0] + x0, luma,
			    x1 - x0 + 1);
		if (y & 1)
			return;
		int cx0 = x0 / 2, cx1 = x1 / 2;
		std::memset(f->data[1] + (size_t)(y / 2) * f->linesize[1] + cx0, cb,
			    cx1 - cx0 + 1);
		std::memset(f->data[2] + (size_t)(y / 2) * f->linesize[2] + cx0, cr,
			    cx1 - cx0 + 1);
	}

	static void rect(AVFrame *f, int x, int y, int w, int h, uint8_t luma,
			 uint8_t cb, uint8_t cr)
	{
		for (int yy = y; yy < y + h; yy++)
			span(f, yy, x, x + w - 1, luma, cb, cr);
	}

	static void draw(AVFrame *f, const Shape &s, int r, uint8_t luma,
			 uint8_t cb, uint8_t cr)
	{
		int cx = (int)s.x, cy = (int)s.y;
		for (int dy = -r; dy <= r; dy++) {
			int half = s.square ? r
					    : (int)std::sqrt((double)(r * r - dy * dy));
			span(f, cy + dy, cx - half, cx + half, luma, cb, cr);
		}
	}

	std::vector<uint8_t> noise_;
	std::vector<Shape> shapes_;
	text_ctx *counter_ = nullptr, *band_ = nullptr, *small_ = nullptr;
	std::string band_text_;
	int band_w_ = 0;
};

/* ---- stream inspection -------------------------------------------------- */

/* What the client's au_info (src/media/video_pipeline.cpp) would see, plus
 * the SPS fields that name the profile. Emulation prevention keeps 00 00 01
 * out of NAL payloads, so the scan is exact. */
struct AuInfo {
	int vcl = 0;
	bool idr = false, sps = false, pps = false;
	int profile_idc = -1, constraints = 0, level_idc = 0;
};

AuInfo inspect_au(const std::vector<uint8_t> &au)
{
	AuInfo i;
	for (size_t k = 0; k + 3 < au.size(); k++) {
		if (au[k] != 0 || au[k + 1] != 0 || au[k + 2] != 1)
			continue;
		uint8_t t = au[k + 3] & 0x1f;
		if (t == 1 || t == 5)
			i.vcl++;
		if (t == 5)
			i.idr = true;
		if (t == 8)
			i.pps = true;
		if (t == 7 && k + 6 < au.size()) {
			i.sps = true;
			i.profile_idc = au[k + 4];
			i.constraints = au[k + 5];
			i.level_idc = au[k + 6];
		}
		k += 3;
	}
	return i;
}

/* ---- encoders ----------------------------------------------------------- */

AVCodecContext *open_video()
{
	const AVCodec *c = avcodec_find_encoder_by_name("libx264");
	if (!c) {
		std::fprintf(stderr, "mkstream: this libavcodec has no libx264"
				     " encoder -- add libx264 to"
				     " docker/Dockerfile.host-bullseye and rebuild"
				     " the image\n");
		return nullptr;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(c);
	ctx->width = kW;
	ctx->height = kH;
	ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	ctx->time_base = AVRational{1, kFps};
	ctx->framerate = AVRational{kFps, 1};
	/* "CBR" in x264's sense: bitrate == vbv-maxrate. The 500 ms buffer
	 * lets an IDR run to a few hundred KB, as xCloud's do (54 ms to
	 * decode on the device), instead of being starved to the size of a
	 * P-frame. */
	ctx->bit_rate = kVideoBitrate;
	ctx->rc_max_rate = kVideoBitrate;
	ctx->rc_buffer_size = kVideoBitrate / 2;
	ctx->gop_size = kKeyint;
	ctx->keyint_min = kKeyint;
	ctx->max_b_frames = 0;
	ctx->slices = 1;
	/* One thread. tune=zerolatency turns on x264's sliced threading,
	 * which cuts every frame into `threads` slices -- the one thing this
	 * stream must not have. Single-threaded is also bit-exact between
	 * runs. veryfast 720p on one x86 core still encodes faster than
	 * real time. */
	ctx->thread_count = 1;
	av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
	av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
	av_opt_set(ctx->priv_data, "profile", "baseline", 0);
	/* No surprise IDRs on a bounce: the fixture's keyframe cadence has to
	 * be exactly kKeyint or the replay thresholds mean nothing. Set through
	 * x264's own option rather than AVCodecContext::scenechange_threshold,
	 * which is deprecated and which the libx264 wrapper ignores. */
	av_opt_set_int(ctx->priv_data, "sc_threshold", 0, 0);
	/* Belt and braces, in x264's own words, applied after the preset. */
	char x264_params[256];
	std::snprintf(x264_params, sizeof(x264_params),
		      "keyint=%d:min-keyint=%d:scenecut=0:bframes=0:slices=1"
		      ":threads=1:sliced-threads=0:vbv-maxrate=8000:vbv-bufsize=4000",
		      kKeyint, kKeyint);
	av_opt_set(ctx->priv_data, "x264-params", x264_params, 0);
	int rc = avcodec_open2(ctx, c, nullptr);
	if (rc < 0) {
		char e[128];
		av_strerror(rc, e, sizeof(e));
		std::fprintf(stderr, "mkstream: libx264 open failed: %s\n", e);
		avcodec_free_context(&ctx);
		return nullptr;
	}
	return ctx;
}

AVCodecContext *open_audio()
{
	const AVCodec *c = avcodec_find_encoder_by_name("libopus");
	if (!c) {
		std::fprintf(stderr, "mkstream: this libavcodec has no libopus"
				     " encoder -- add libopus to"
				     " docker/Dockerfile.host-bullseye and rebuild"
				     " the image\n");
		return nullptr;
	}
	AVCodecContext *ctx = avcodec_alloc_context3(c);
	ctx->sample_rate = kAudioRate;
	ctx->channels = 2;
	ctx->channel_layout = AV_CH_LAYOUT_STEREO;
	ctx->sample_fmt = AV_SAMPLE_FMT_S16;
	ctx->bit_rate = kAudioBitrate;
	ctx->time_base = AVRational{1, kAudioRate};
	av_opt_set(ctx->priv_data, "frame_duration", "20", 0);
	/* Constant-size packets (240 bytes at 96 kbps): the audio| line's
	 * byte counts then mean something across runs. */
	av_opt_set(ctx->priv_data, "vbr", "off", 0);
	int rc = avcodec_open2(ctx, c, nullptr);
	if (rc < 0) {
		char e[128];
		av_strerror(rc, e, sizeof(e));
		std::fprintf(stderr, "mkstream: libopus open failed: %s\n", e);
		avcodec_free_context(&ctx);
		return nullptr;
	}
	return ctx;
}

/* Take every packet the encoder has ready. */
template <typename Fn>
bool drain(AVCodecContext *ctx, AVPacket *pkt, Fn on_packet)
{
	for (;;) {
		int rc = avcodec_receive_packet(ctx, pkt);
		if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
			return true;
		if (rc < 0)
			return false;
		on_packet(pkt);
		av_packet_unref(pkt);
	}
}

/* ---- the file ----------------------------------------------------------- */

void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

bool write_xcau(const char *path, const std::vector<Record> &recs)
{
	FILE *fp = std::fopen(path, "wb");
	if (!fp) {
		std::fprintf(stderr, "mkstream: cannot write %s\n", path);
		return false;
	}
	setvbuf(fp, nullptr, _IOFBF, 1 << 20);
	uint8_t hdr[20] = {'X', 'C', 'A', 'U'};
	put_u32(hdr + 4, 1);  /* version, au_recorder.cpp kVersion */
	bool ok = std::fwrite(hdr, 1, 8, fp) == 8;
	for (const Record &r : recs) {
		uint64_t ts = (uint64_t)std::llround(r.t_ms);
		hdr[0] = r.kind;
		hdr[1] = hdr[2] = hdr[3] = 0;
		put_u32(hdr + 4, r.seq);
		put_u32(hdr + 8, (uint32_t)ts);
		put_u32(hdr + 12, (uint32_t)(ts >> 32));
		put_u32(hdr + 16, (uint32_t)r.data.size());
		ok = ok && std::fwrite(hdr, 1, 20, fp) == 20 &&
		     std::fwrite(r.data.data(), 1, r.data.size(), fp) == r.data.size();
	}
	ok = std::fclose(fp) == 0 && ok;
	if (!ok)
		std::fprintf(stderr, "mkstream: short write to %s\n", path);
	return ok;
}

/* Read it back through the client's own reader: the same count, monotone
 * timestamps, and an IDR-led first unit, or the file is no use. */
bool verify_xcau(const char *path, size_t expect)
{
	gnx::stream::AuReader reader;
	if (!reader.open(path))
		return false;
	gnx::stream::AuRecorder::Kind kind;
	uint32_t seq;
	uint64_t ts, last = 0;
	std::vector<uint8_t> data;
	size_t n = 0;
	bool first_video = true;
	while (reader.next(&kind, &seq, &ts, data)) {
		if (n && ts < last) {
			std::fprintf(stderr, "mkstream: read-back: record %zu goes"
					     " backwards in time\n", n);
			return false;
		}
		if (kind == gnx::stream::AuRecorder::Video && first_video) {
			first_video = false;
			if (!inspect_au(data).idr) {
				std::fprintf(stderr, "mkstream: read-back: the first"
						     " unit is not an IDR\n");
				return false;
			}
		}
		last = ts;
		n++;
	}
	if (n != expect) {
		std::fprintf(stderr, "mkstream: read-back: %zu records, wrote %zu\n",
			     n, expect);
		return false;
	}
	return true;
}

int usage()
{
	std::fprintf(stderr, "usage: mkstream <out.xcau> <seconds> [-jitter ms]"
			     " [-keyint frames]\n");
	return 2;
}

}  // namespace

int main(int argc, char **argv)
{
	if (argc < 3)
		return usage();
	const char *out = argv[1];
	const int seconds = std::atoi(argv[2]);
	double jitter_ms = 0;
	for (int i = 3; i < argc; i++) {
		if (!std::strcmp(argv[i], "-jitter") && i + 1 < argc)
			jitter_ms = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "-keyint") && i + 1 < argc)
			kKeyint = std::atoi(argv[++i]);
		else
			return usage();
	}
	if (seconds <= 0 || seconds > 3600 || jitter_ms < 0 || kKeyint < 1 ||
	    kKeyint > 3600)
		return usage();

	Scene scene;
	if (!scene.init())
		return 1;
	AVCodecContext *vctx = open_video();
	AVCodecContext *actx = open_audio();
	if (!vctx || !actx)
		return 1;
	AVPacket *pkt = av_packet_alloc();

	/* ---- video ------------------------------------------------------- */
	const int nframes = seconds * kFps;
	std::vector<Record> video;
	video.reserve(nframes);
	uint64_t vbytes = 0, vmax = 0;
	int vcl_min = 99, vcl_max = 0, first_idr_slots = 0;
	std::string idr_at;
	AuInfo sps;
	auto on_video = [&](AVPacket *p) {
		Record r;
		r.kind = gnx::stream::AuRecorder::Video;
		r.seq = (uint32_t)p->pts + 1;
		r.t_ms = kStartMs + p->pts * kVideoPeriodMs;
		r.data.assign(p->data, p->data + p->size);
		AuInfo i = inspect_au(r.data);
		vcl_min = std::min(vcl_min, i.vcl);
		vcl_max = std::max(vcl_max, i.vcl);
		if (i.idr) {
			if (first_idr_slots++ < 8)
				idr_at += " " + std::to_string(p->pts);
			if (i.sps && sps.profile_idc < 0)
				sps = i;
		}
		vbytes += (uint64_t)p->size;
		vmax = std::max(vmax, (uint64_t)p->size);
		video.push_back(std::move(r));
	};
	{
		AVFrame *frame = av_frame_alloc();
		frame->format = AV_PIX_FMT_YUV420P;
		frame->width = kW;
		frame->height = kH;
		if (av_frame_get_buffer(frame, 32) < 0) {
			std::fprintf(stderr, "mkstream: no frame buffer\n");
			return 1;
		}
		for (int n = 0; n < nframes; n++) {
			if (av_frame_make_writable(frame) < 0)
				return 1;
			scene.render(frame, n);
			frame->pts = n;
			int rc;
			while ((rc = avcodec_send_frame(vctx, frame)) == AVERROR(EAGAIN))
				if (!drain(vctx, pkt, on_video))
					return 1;
			if (rc < 0 || !drain(vctx, pkt, on_video)) {
				std::fprintf(stderr, "mkstream: video encode failed at"
						     " frame %d\n", n);
				return 1;
			}
			if (n % (5 * kFps) == 0)
				std::fprintf(stderr, "mkstream: video %d/%d frames\n",
					     n, nframes);
		}
		avcodec_send_frame(vctx, nullptr);
		if (!drain(vctx, pkt, on_video))
			return 1;
		av_frame_free(&frame);
	}

	/* ---- audio ------------------------------------------------------- */
	const int npackets = seconds * 1000 / kAudioPacketMs;
	const int frame_size = actx->frame_size;  /* 960 at 20 ms */
	std::vector<Record> audio;
	audio.reserve(npackets);
	uint64_t abytes = 0;
	size_t amin = ~(size_t)0, amax = 0;
	uint32_t acount = 0;
	auto on_audio = [&](AVPacket *p) {
		/* Indexed by arrival, not by pts: libopus reports its 6.5 ms
		 * lookahead as a negative first pts, which is a container's
		 * business, not a stream's. */
		Record r;
		r.kind = gnx::stream::AuRecorder::Audio;
		r.seq = (uint16_t)(kAudioSeq0 + acount);
		r.t_ms = kStartMs + (double)acount * kAudioPacketMs;
		r.data.assign(p->data, p->data + p->size);
		abytes += (uint64_t)p->size;
		amin = std::min(amin, (size_t)p->size);
		amax = std::max(amax, (size_t)p->size);
		acount++;
		audio.push_back(std::move(r));
	};
	{
		AVFrame *af = av_frame_alloc();
		af->format = AV_SAMPLE_FMT_S16;
		af->channel_layout = AV_CH_LAYOUT_STEREO;
		af->channels = 2;
		af->sample_rate = kAudioRate;
		af->nb_samples = frame_size;
		if (av_frame_get_buffer(af, 0) < 0) {
			std::fprintf(stderr, "mkstream: no audio buffer\n");
			return 1;
		}
		/* A chirp between 220 and 880 Hz on a 12 s cycle, 0.35 full
		 * scale, the right channel a quarter turn behind: continuous
		 * tone, so a 20 ms hole is a click and a longer one a silence. */
		double phase = 0;
		for (int k = 0; k < npackets; k++) {
			if (av_frame_make_writable(af) < 0)
				return 1;
			int16_t *s = (int16_t *)af->data[0];
			for (int i = 0; i < frame_size; i++) {
				double t = (double)((int64_t)k * frame_size + i) / kAudioRate;
				double freq = 440.0 * std::pow(2.0, std::sin(2.0 * M_PI * t / 12.0));
				phase += 2.0 * M_PI * freq / kAudioRate;
				if (phase > 2.0 * M_PI)
					phase -= 2.0 * M_PI;
				s[2 * i] = (int16_t)(0.35 * 32767.0 * std::sin(phase));
				s[2 * i + 1] = (int16_t)(0.35 * 32767.0 * std::cos(phase));
			}
			af->pts = (int64_t)k * frame_size;
			int rc;
			while ((rc = avcodec_send_frame(actx, af)) == AVERROR(EAGAIN))
				if (!drain(actx, pkt, on_audio))
					return 1;
			if (rc < 0 || !drain(actx, pkt, on_audio)) {
				std::fprintf(stderr, "mkstream: audio encode failed at"
						     " packet %d\n", k);
				return 1;
			}
		}
		/* No flush: it would add a padded partial packet, and the
		 * stream is meant to be exactly 50 packets a second. */
		av_frame_free(&af);
	}

	/* ---- timing ------------------------------------------------------ */
	if (jitter_ms > 0) {
		/* Gaussian, clamped at three sigma, and never earlier than
		 * the previous packet of the same stream: the jitter buffers
		 * see ragged spacing, never a swap. */
		std::mt19937 rng(0x5eed);
		std::normal_distribution<double> nd(0.0, jitter_ms);
		for (std::vector<Record> *list : {&video, &audio}) {
			double last = -1e18;
			for (Record &r : *list) {
				double j = std::max(-3.0 * jitter_ms,
						    std::min(3.0 * jitter_ms, nd(rng)));
				r.t_ms = std::max(last, r.t_ms + j);
				last = r.t_ms;
			}
		}
	}
	std::vector<Record> all;
	all.reserve(video.size() + audio.size());
	for (Record &r : video)
		all.push_back(std::move(r));
	for (Record &r : audio)
		all.push_back(std::move(r));
	/* Stable: on a tie, video (generated first) stays ahead of audio and
	 * each stream keeps its own order. */
	std::stable_sort(all.begin(), all.end(),
			 [](const Record &a, const Record &b) { return a.t_ms < b.t_ms; });

	if (!write_xcau(out, all) || !verify_xcau(out, all.size()))
		return 1;

	std::fprintf(stderr,
		     "mkstream: video %zu AUs %.1f MB %.2f Mbps max %llu KB,"
		     " IDR at frames%s (%d), slices/AU %d-%d,"
		     " SPS profile_idc=%d constraint_set1=%d level=%d\n",
		     video.size(), vbytes / 1e6, vbytes * 8.0 / seconds / 1e6,
		     (unsigned long long)(vmax / 1024), idr_at.c_str(),
		     first_idr_slots, vcl_min, vcl_max, sps.profile_idc,
		     (sps.constraints >> 6) & 1, sps.level_idc);
	std::fprintf(stderr,
		     "mkstream: audio %u packets %.0f KB %.1f kbps, %zu-%zu B each,"
		     " seq %u..%u\n",
		     acount, abytes / 1e3, abytes * 8.0 / seconds / 1e3, amin, amax,
		     kAudioSeq0, (kAudioSeq0 + acount - 1) & 0xffff);
	std::fprintf(stderr,
		     "mkstream: wrote %s: %zu records, %d s, jitter %.1f ms,"
		     " read back OK\n",
		     out, all.size(), seconds, jitter_ms);

	av_packet_free(&pkt);
	avcodec_free_context(&vctx);
	avcodec_free_context(&actx);
	return 0;
}
