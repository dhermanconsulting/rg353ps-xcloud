/*
 * Box-art thumbnails for the library, ready to blit.
 *
 * A worker thread downloads each poster once (the Store's image service
 * scales server-side, so "?h=120" fetches an 80x120 JPEG of ~5 KB rather
 * than the 1440x2160 original), keeps the bytes at <state dir>/art/
 * <product_id>.jpg, decodes them with libavcodec's mjpeg or png decoder --
 * both are in the FFmpeg the device ships, libswscale is not -- and
 * box-filters the picture down to the on-screen size as NV12 planes. The
 * main thread only ever blits: get() is a hash lookup, pump() moves
 * finished pictures across under a mutex.
 *
 * Memory: a 38x58 thumbnail is 3.3 KB, so the whole ~600-title library is
 * ~2 MB decoded; a budget caps it anyway and the least recently painted
 * pictures go first (they are re-read from the SD card, not the network).
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace app {

struct Thumb {
	int w = 0, h = 0;              /* both even; 0x0 is a cached miss */
	std::vector<uint8_t> luma;     /* w * h */
	std::vector<uint8_t> chroma;   /* w * h / 2, interleaved Cb/Cr, pitch w */
	uint32_t last_use = 0;         /* Thumbs::tick() count when last drawn */

	bool ok() const { return w > 0; }
	size_t bytes() const { return luma.size() + chroma.size(); }
};

class Thumbs {
public:
	/* Pictures are scaled to fit max_w x max_h keeping their aspect. */
	Thumbs(std::string dir, int max_w, int max_h, size_t budget_bytes);
	~Thumbs();

	/*
	 * Main thread. The decoded picture for `key` (the product id), or
	 * nullptr while it is not in memory -- in which case a fetch is
	 * queued, newest first, so what is on screen loads before what was
	 * scrolled past. An empty url yields nullptr and queues nothing.
	 */
	const Thumb *get(const std::string &key, const std::string &url);

	/* Main thread: take finished pictures. True if any arrived. */
	bool pump();

	/* Once per repaint, for the least-recently-used accounting. */
	void tick() { tick_++; }

	size_t bytes() const { return bytes_; }
	int queued() const { return queued_.load(); }

private:
	struct Job {
		std::string key, url;
	};
	struct Done {
		std::string key;
		Thumb thumb;
	};

	void worker();
	void evict();

	std::string dir_;
	int max_w_, max_h_;
	size_t budget_;
	size_t bytes_ = 0;
	uint32_t tick_ = 0;

	std::unordered_map<std::string, Thumb> cache_;   /* main thread only */
	std::unordered_set<std::string> pending_;        /* guarded by mutex_ */
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<Job> jobs_;
	std::deque<Done> done_;
	std::atomic<bool> quit_{false};
	std::atomic<int> queued_{0};
	/* For the exit log: how the pictures were obtained. */
	std::atomic<unsigned> from_disk_{0}, downloaded_{0}, misses_{0};
	std::thread thread_;
};

}  // namespace app
