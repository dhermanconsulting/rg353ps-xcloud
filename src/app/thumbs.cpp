/*
 * Box-art thumbnails. See thumbs.hpp.
 */
#include "thumbs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <pthread.h>
#include <sys/stat.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include "../ui/screen.h"
}

#include "../gnx/http.hpp"

namespace app {

namespace {

/*
 * Height asked of the Store's image service. 120 makes a poster 80x120 and
 * ~5 KB; the on-screen picture is smaller still, so the download is
 * already a fraction of the 1440x2160 original and the box filter below has
 * two source pixels per output pixel to average, which is what keeps thin
 * title lettering legible at 38 px wide.
 */
constexpr int kFetchHeight = 120;

/* Newest requests go to the front; anything past this many is dropped and
 * re-queued if it scrolls back into view. Bounds the backlog after a long
 * scroll to seconds, not minutes. */
constexpr size_t kMaxQueue = 48;

std::vector<uint8_t> read_file(const std::string &path)
{
	std::vector<uint8_t> bytes;
	std::ifstream in(path, std::ios::binary);

	if (!in)
		return bytes;
	in.seekg(0, std::ios::end);
	std::streamoff n = in.tellg();
	if (n <= 0 || n > 4 << 20)   /* a poster is 5 KB; 4 MB is a bug */
		return bytes;
	bytes.resize((size_t)n);
	in.seekg(0);
	in.read((char *)bytes.data(), n);
	if (!in)
		bytes.clear();
	return bytes;
}

bool write_file(const std::string &path, const std::vector<uint8_t> &bytes)
{
	std::string tmp = path + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		out.write((const char *)bytes.data(), (std::streamsize)bytes.size());
		if (!out)
			return false;
	}
	if (std::rename(tmp.c_str(), path.c_str())) {
		std::remove(tmp.c_str());
		return false;
	}
	return true;
}

/*
 * Box-filter one 8-bit plane from sw x sh to dw x dh. Each output pixel is
 * the mean of the source block it covers; blocks are at least one pixel,
 * so a source smaller than the target is point-sampled rather than
 * crashing. `dstep` and `doff` let the two chroma planes be written
 * interleaved straight into an NV12 chroma row.
 */
void box_scale(const uint8_t *src, int sw, int sh, int spitch, uint8_t *dst,
	       int dw, int dh, int dpitch, int dstep, int doff)
{
	for (int dy = 0; dy < dh; dy++) {
		int sy0 = dy * sh / dh;
		int sy1 = std::max(sy0 + 1, (dy + 1) * sh / dh);
		uint8_t *out = dst + (size_t)dy * dpitch + doff;

		for (int dx = 0; dx < dw; dx++) {
			int sx0 = dx * sw / dw;
			int sx1 = std::max(sx0 + 1, (dx + 1) * sw / dw);
			unsigned sum = 0, n = (unsigned)((sy1 - sy0) * (sx1 - sx0));

			for (int y = sy0; y < sy1; y++) {
				const uint8_t *row = src + (size_t)y * spitch;
				for (int x = sx0; x < sx1; x++)
					sum += row[x];
			}
			out[(size_t)dx * dstep] = (uint8_t)((sum + n / 2) / n);
		}
	}
}

/*
 * A decoded frame, whatever its pixel format, as three full-resolution
 * planes of limited-range BT.601 Y, Cb, Cr -- the convention the UI buffer,
 * the panel path and the simulator's PNG writer all share.
 *
 * Reads pixels through libavutil's format descriptor rather than switching
 * on formats: the mjpeg decoder yields yuvj420p/422p/444p (full range) and
 * the png decoder rgb24, rgba, gray8, pal8 and friends, and one generic
 * reader handles all of them. Alpha is composited over black, the row
 * background the picture is blitted onto. 16-bit formats are refused.
 */
bool frame_to_ycc(const AVFrame *f, std::vector<uint8_t> &Y,
		  std::vector<uint8_t> &Cb, std::vector<uint8_t> &Cr)
{
	const AVPixFmtDescriptor *d =
		av_pix_fmt_desc_get((enum AVPixelFormat)f->format);

	if (!d || d->nb_components < 1 || f->width < 1 || f->height < 1)
		return false;
	if (d->flags & (AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL))
		return false;
	for (int c = 0; c < d->nb_components; c++)
		if (d->comp[c].depth != 8)
			return false;

	const bool rgb = d->flags & AV_PIX_FMT_FLAG_RGB;
	const bool pal = d->flags & AV_PIX_FMT_FLAG_PAL;
	const bool has_alpha = d->flags & AV_PIX_FMT_FLAG_ALPHA;
	const int nb = d->nb_components;
	/* Full range: what JPEG carries (the "yuvj" formats say so in their
	 * name as well as in color_range), and gray from PNG. */
	const bool full = f->color_range == AVCOL_RANGE_JPEG ||
			  !std::strncmp(d->name, "yuvj", 4) || nb <= 2;
	const int w = f->width, h = f->height;

	auto sample = [&](int c, int x, int y) -> int {
		const AVComponentDescriptor &cd = d->comp[c];
		if (!rgb && (c == 1 || c == 2)) {
			x >>= d->log2_chroma_w;
			y >>= d->log2_chroma_h;
		}
		return f->data[cd.plane][(size_t)y * f->linesize[cd.plane] +
					 (size_t)x * cd.step + cd.offset];
	};

	Y.resize((size_t)w * h);
	Cb.resize((size_t)w * h);
	Cr.resize((size_t)w * h);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int a = 255;
			uint8_t yy, cb, cr;

			if (pal || rgb) {
				int r, g, b;
				if (pal) {
					uint32_t p = ((const uint32_t *)f->data[1])
						[sample(0, x, y)];
					a = (int)(p >> 24);
					r = (int)(p >> 16) & 255;
					g = (int)(p >> 8) & 255;
					b = (int)p & 255;
				} else {
					r = sample(0, x, y);
					g = sample(1, x, y);
					b = sample(2, x, y);
					if (has_alpha && nb == 4)
						a = sample(3, x, y);
				}
				screen_rgb_to_ycc((uint8_t)r, (uint8_t)g,
						  (uint8_t)b, &yy, &cb, &cr);
			} else {
				int ly = sample(0, x, y);
				int lcb = 128, lcr = 128;
				if (nb >= 3) {
					lcb = sample(1, x, y);
					lcr = sample(2, x, y);
					if (has_alpha && nb == 4)
						a = sample(3, x, y);
				} else if (has_alpha && nb == 2) {
					a = sample(1, x, y);
				}
				if (full) {
					ly = 16 + (ly * 219 + 127) / 255;
					lcb = 128 + (lcb - 128) * 224 / 255;
					lcr = 128 + (lcr - 128) * 224 / 255;
				}
				yy = (uint8_t)ly;
				cb = (uint8_t)lcb;
				cr = (uint8_t)lcr;
			}
			if (a != 255) {
				yy = (uint8_t)(16 + (yy - 16) * a / 255);
				cb = (uint8_t)(128 + (cb - 128) * a / 255);
				cr = (uint8_t)(128 + (cr - 128) * a / 255);
			}
			Y[(size_t)y * w + x] = yy;
			Cb[(size_t)y * w + x] = cb;
			Cr[(size_t)y * w + x] = cr;
		}
	}
	return true;
}

/* Decode a JPEG or PNG in memory and scale it into `out`. False = miss. */
bool decode_image(const std::vector<uint8_t> &bytes, int max_w, int max_h,
		  Thumb &out)
{
	const bool png = bytes.size() > 8 && bytes[0] == 0x89 &&
			 bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
	const bool jpeg = bytes.size() > 3 && bytes[0] == 0xFF &&
			  bytes[1] == 0xD8;
	if (!png && !jpeg)
		return false;

	const AVCodec *codec = avcodec_find_decoder(png ? AV_CODEC_ID_PNG
						       : AV_CODEC_ID_MJPEG);
	if (!codec)
		return false;
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	bool ok = false;

	if (!ctx || !pkt || !frame)
		goto out;
	/* One small still: a thread pool would cost more to start than the
	 * decode takes. */
	ctx->thread_count = 1;
	if (avcodec_open2(ctx, codec, nullptr) < 0)
		goto out;
	/* av_new_packet adds the zeroed padding the bitstream readers need
	 * past the end of the data. */
	if (av_new_packet(pkt, (int)bytes.size()) < 0)
		goto out;
	std::memcpy(pkt->data, bytes.data(), bytes.size());
	if (avcodec_send_packet(ctx, pkt) < 0)
		goto out;
	if (avcodec_receive_frame(ctx, frame) < 0) {
		/* Still images come straight out, but be safe: flush. */
		avcodec_send_packet(ctx, nullptr);
		if (avcodec_receive_frame(ctx, frame) < 0)
			goto out;
	}

	{
		std::vector<uint8_t> Y, Cb, Cr;
		if (!frame_to_ycc(frame, Y, Cb, Cr))
			goto out;

		const int sw = frame->width, sh = frame->height;
		double scale = std::min((double)max_w / sw, (double)max_h / sh);
		if (scale > 1.0)
			scale = 1.0;   /* never enlarge: it only blurs */
		int dw = std::max(2, (int)(sw * scale) & ~1);
		int dh = std::max(2, (int)(sh * scale) & ~1);

		out.w = dw;
		out.h = dh;
		out.luma.assign((size_t)dw * dh, 16);
		out.chroma.assign((size_t)dw * dh / 2, 128);
		box_scale(Y.data(), sw, sh, sw, out.luma.data(), dw, dh, dw, 1, 0);
		/* Chroma at half resolution: each sample averages the 2x2
		 * luma block's worth of source, written interleaved Cb, Cr. */
		box_scale(Cb.data(), sw, sh, sw, out.chroma.data(), dw / 2,
			  dh / 2, dw, 2, 0);
		box_scale(Cr.data(), sw, sh, sw, out.chroma.data(), dw / 2,
			  dh / 2, dw, 2, 1);
		ok = true;
	}
out:
	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&ctx);
	if (!ok) {
		out.w = out.h = 0;
		out.luma.clear();
		out.chroma.clear();
	}
	return ok;
}

}  // namespace

Thumbs::Thumbs(std::string dir, int max_w, int max_h, size_t budget_bytes)
	: dir_(std::move(dir)), max_w_(max_w), max_h_(max_h),
	  budget_(budget_bytes)
{
	mkdir(dir_.c_str(), 0755);
	thread_ = std::thread(&Thumbs::worker, this);
}

Thumbs::~Thumbs()
{
	quit_ = true;   /* also the HTTP abort flag */
	wake_.notify_all();
	if (thread_.joinable())
		thread_.join();
	if (from_disk_ || downloaded_ || misses_)
		std::fprintf(stderr,
			     "art: %u pictures from disk, %u downloaded, %u misses\n",
			     from_disk_.load(), downloaded_.load(), misses_.load());
}

const Thumb *Thumbs::get(const std::string &key, const std::string &url)
{
	auto found = cache_.find(key);
	if (found != cache_.end()) {
		found->second.last_use = tick_;
		return &found->second;
	}
	if (url.empty() || key.empty())
		return nullptr;

	std::lock_guard<std::mutex> lock(mutex_);
	if (pending_.insert(key).second) {
		jobs_.push_front({ key, url });
		if (jobs_.size() > kMaxQueue) {
			pending_.erase(jobs_.back().key);
			jobs_.pop_back();
		}
		queued_ = (int)jobs_.size();
		wake_.notify_one();
	}
	return nullptr;
}

bool Thumbs::pump()
{
	std::deque<Done> done;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (done_.empty())
			return false;
		done.swap(done_);
		for (const Done &d : done)
			pending_.erase(d.key);
	}
	for (Done &d : done) {
		bytes_ += d.thumb.bytes();
		d.thumb.last_use = tick_;
		cache_[d.key] = std::move(d.thumb);
	}
	if (bytes_ > budget_)
		evict();
	return true;
}

void Thumbs::evict()
{
	/* Oldest first, down to nine tenths of the budget so this does not
	 * run again on the very next picture. Never what was painted this
	 * tick: those are on screen. */
	std::vector<std::pair<uint32_t, std::string>> order;
	for (const auto &[key, t] : cache_)
		if (t.ok() && t.last_use != tick_)
			order.emplace_back(t.last_use, key);
	std::sort(order.begin(), order.end());
	size_t freed = 0, dropped = 0;
	for (const auto &[use, key] : order) {
		if (bytes_ <= budget_ - budget_ / 10)
			break;
		auto it = cache_.find(key);
		freed += it->second.bytes();
		bytes_ -= it->second.bytes();
		cache_.erase(it);
		dropped++;
	}
	std::fprintf(stderr, "art: over budget, dropped %zu pictures (%zu KB)\n",
		     dropped, freed / 1024);
}

void Thumbs::worker()
{
	pthread_setname_np(pthread_self(), "xc-art");
	gnx::Http http;
	http.set_abort_flag(&quit_);

	while (!quit_) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
			if (quit_)
				return;
			job = std::move(jobs_.front());
			jobs_.pop_front();
			queued_ = (int)jobs_.size();
		}

		const std::string path = dir_ + "/" + job.key + ".jpg";
		std::vector<uint8_t> bytes = read_file(path);
		bool fresh = false;

		if (bytes.empty()) {
			/* fetch_names asks for ?h=300; we want the smaller one. */
			std::string url = job.url;
			size_t q = url.find('?');
			if (q != std::string::npos)
				url.erase(q);
			url += "?h=" + std::to_string(kFetchHeight);
			try {
				gnx::HttpResponse r = http.get(url);
				if (r.ok() && !r.body.empty()) {
					bytes.assign(r.body.begin(), r.body.end());
					fresh = true;
				} else if (!quit_) {
					std::fprintf(stderr, "art: %s: HTTP %ld\n",
						     job.key.c_str(), r.status);
				}
			} catch (const std::exception &e) {
				if (!quit_)
					std::fprintf(stderr, "art: %s: %s\n",
						     job.key.c_str(), e.what());
			}
		}

		Done d;
		d.key = job.key;
		if (!bytes.empty() && !decode_image(bytes, max_w_, max_h_, d.thumb)) {
			std::fprintf(stderr, "art: %s: cannot decode %zu bytes\n",
				     job.key.c_str(), bytes.size());
			std::remove(path.c_str());   /* do not keep a bad file */
		} else if (fresh && d.thumb.ok()) {
			write_file(path, bytes);
		}
		if (!d.thumb.ok())
			misses_++;
		else if (fresh)
			downloaded_++;
		else
			from_disk_++;

		std::lock_guard<std::mutex> lock(mutex_);
		done_.push_back(std::move(d));
	}
}

}  // namespace app
