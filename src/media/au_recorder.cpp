#include "au_recorder.hpp"

#include <cstring>
#include <pthread.h>

namespace gnx::stream {

namespace {

constexpr size_t kMaxQueued = 256;  /* ~4 s of video at 60 fps */
const char kMagic[4] = {'X', 'C', 'A', 'U'};
constexpr uint32_t kVersion = 1;

void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

void put_u64(uint8_t *p, uint64_t v)
{
	put_u32(p, (uint32_t)v);
	put_u32(p + 4, (uint32_t)(v >> 32));
}

uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

uint64_t get_u64(const uint8_t *p)
{
	return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

}  // namespace

AuRecorder::~AuRecorder() { close(); }

bool AuRecorder::open(const std::string &path)
{
	close();
	file_ = std::fopen(path.c_str(), "wb");
	if (!file_) {
		std::fprintf(stderr, "record: cannot open %s\n", path.c_str());
		return false;
	}
	/* A large stdio buffer so the writer thread issues few, big writes. */
	setvbuf(file_, nullptr, _IOFBF, 1 << 20);
	uint8_t hdr[8];
	std::memcpy(hdr, kMagic, 4);
	put_u32(hdr + 4, kVersion);
	std::fwrite(hdr, 1, sizeof(hdr), file_);
	quit_ = false;
	records_ = 0;
	bytes_ = 0;
	dropped_ = 0;
	active_ = true;
	thread_ = std::thread(&AuRecorder::thread_main, this);
	std::fprintf(stderr, "record: writing %s\n", path.c_str());
	return true;
}

void AuRecorder::close()
{
	if (!active_ && !file_)
		return;
	active_ = false;
	quit_ = true;
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	if (file_) {
		std::fclose(file_);
		file_ = nullptr;
	}
	std::fprintf(stderr, "record: closed, %u records, %llu bytes, %u dropped\n",
		     records_.load(), (unsigned long long)bytes_.load(),
		     dropped_.load());
}

void AuRecorder::push(Kind kind, uint32_t seq, uint64_t ts_ms,
		      const uint8_t *data, size_t size)
{
	if (!active_)
		return;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (queue_.size() >= kMaxQueued) {
			dropped_.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		queue_.push_back({kind, seq, ts_ms,
				  std::vector<uint8_t>(data, data + size)});
	}
	cv_.notify_one();
}

void AuRecorder::thread_main()
{
	pthread_setname_np(pthread_self(), "xc-record");
	std::deque<Rec> batch;
	for (;;) {
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
			batch.swap(queue_);
		}
		for (const Rec &r : batch) {
			uint8_t hdr[20];
			hdr[0] = r.kind;
			hdr[1] = hdr[2] = hdr[3] = 0;
			put_u32(hdr + 4, r.seq);
			put_u64(hdr + 8, r.ts_ms);
			put_u32(hdr + 16, (uint32_t)r.data.size());
			std::fwrite(hdr, 1, sizeof(hdr), file_);
			std::fwrite(r.data.data(), 1, r.data.size(), file_);
			records_.fetch_add(1, std::memory_order_relaxed);
			bytes_.fetch_add(r.data.size(), std::memory_order_relaxed);
		}
		batch.clear();
		if (quit_) {
			std::lock_guard<std::mutex> lock(mutex_);
			if (queue_.empty())
				break;
		}
	}
	std::fflush(file_);
}

AuRecorder::Stats AuRecorder::stats() const
{
	Stats s;
	s.records = records_.load();
	s.bytes = bytes_.load();
	s.dropped = dropped_.load();
	return s;
}

/* ---- reader ------------------------------------------------------------- */

AuReader::~AuReader() { close(); }

bool AuReader::open(const std::string &path)
{
	close();
	file_ = std::fopen(path.c_str(), "rb");
	if (!file_) {
		std::fprintf(stderr, "replay: cannot open %s\n", path.c_str());
		return false;
	}
	uint8_t hdr[8];
	if (std::fread(hdr, 1, sizeof(hdr), file_) != sizeof(hdr) ||
	    std::memcmp(hdr, kMagic, 4) != 0 || get_u32(hdr + 4) != kVersion) {
		std::fprintf(stderr, "replay: %s is not a recording\n",
			     path.c_str());
		close();
		return false;
	}
	return true;
}

void AuReader::close()
{
	if (file_) {
		std::fclose(file_);
		file_ = nullptr;
	}
}

bool AuReader::next(AuRecorder::Kind *kind, uint32_t *seq, uint64_t *ts_ms,
		    std::vector<uint8_t> &data)
{
	uint8_t hdr[20];
	if (!file_ || std::fread(hdr, 1, sizeof(hdr), file_) != sizeof(hdr))
		return false;
	uint32_t size = get_u32(hdr + 16);
	if (size > 8u * 1024 * 1024)
		return false;
	data.resize(size);
	if (size && std::fread(data.data(), 1, size, file_) != size)
		return false;
	*kind = (AuRecorder::Kind)hdr[0];
	*seq = get_u32(hdr + 4);
	*ts_ms = get_u64(hdr + 8);
	return true;
}

}  // namespace gnx::stream
