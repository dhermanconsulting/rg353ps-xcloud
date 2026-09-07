#pragma once

/*
 * Stream recorder: every video access unit and audio packet the engine emits,
 * with its arrival time, written to a file so the whole decode/present path
 * can be replayed on the device with the ORIGINAL timing and no live session.
 *
 * Starting an xCloud session costs ~25 s and needs Game Pass; a recording
 * makes every pacing experiment a 30 s offline run instead, and the same
 * input every time, so two builds can be compared honestly.
 *
 * The file write happens on its own thread. The engine's worker is the sole
 * libpeer socket pump, and an SD-card write that stalls for 200 ms would
 * overflow the UDP receive buffer and corrupt the very stream being recorded.
 *
 * Format, little-endian:
 *   "XCAU" u32 version(1)
 *   then records:  u8 kind (0 video AU, 1 audio packet), u8 pad[3],
 *                  u32 seq, u64 ts_ms (SDL_GetTicks64 at emit), u32 size,
 *                  u8 data[size]
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gnx::stream {

class AuRecorder {
public:
	enum Kind : uint8_t { Video = 0, Audio = 1 };

	~AuRecorder();

	bool open(const std::string &path);
	void close();
	bool active() const { return active_.load(std::memory_order_relaxed); }

	/* Any thread. Copies the payload and returns; never blocks on I/O. */
	void push(Kind kind, uint32_t seq, uint64_t ts_ms, const uint8_t *data,
		  size_t size);

	struct Stats {
		uint32_t records = 0;
		uint64_t bytes = 0;
		uint32_t dropped = 0;  /* queue full: writer could not keep up */
	};
	Stats stats() const;

private:
	struct Rec {
		Kind kind;
		uint32_t seq;
		uint64_t ts_ms;
		std::vector<uint8_t> data;
	};
	void thread_main();

	FILE *file_ = nullptr;
	std::thread thread_;
	std::atomic<bool> active_{false};
	std::atomic<bool> quit_{false};
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<Rec> queue_;
	std::atomic<uint32_t> records_{0};
	std::atomic<uint64_t> bytes_{0};
	std::atomic<uint32_t> dropped_{0};
};

/* Reader for the same format, for the replay harness. */
class AuReader {
public:
	~AuReader();
	bool open(const std::string &path);
	void close();
	/* False at end of file or on a corrupt record. */
	bool next(AuRecorder::Kind *kind, uint32_t *seq, uint64_t *ts_ms,
		  std::vector<uint8_t> &data);

private:
	FILE *file_ = nullptr;
};

}  // namespace gnx::stream
