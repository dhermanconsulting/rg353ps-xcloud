#include "audio_jitter.hpp"

#include <utility>

namespace gnx::stream {

namespace {
// How long to hold later packets for a missing earlier one before calling it
// lost. Almost no time at all: audio has no retransmission, and a single
// Wi-Fi access-category queue does not reorder RTP, so a packet that is not
// here when its successor arrives is gone. Every millisecond of waiting is a
// millisecond the sound card drains without a top-up -- with the card held
// 45-75 ms deep, declaring the loss 40 ms late was itself enough to underrun
// before the silence fill went in. One loop tick's grace covers the rare
// genuine reorder.
constexpr uint64_t kMaxWaitMs = 5;
// Hard cap as a backstop against a pathological stream.
constexpr size_t kMaxLate = 32;
}  // namespace

void AudioJitterBuffer::push(uint16_t seq, const uint8_t* data, size_t size,
                             uint64_t now_ms) {
    if (!have_next_) {
        next_seq_ = seq;
        have_next_ = true;
    }
    // Already delivered past this sequence -- arrived too late, drop it.
    if (seq_before(seq, next_seq_)) return;
    // Duplicate of something already buffered, drop it.
    for (const auto& e : pending_)
        if (e.seq == seq) return;
    pending_.push_back({seq, now_ms, std::vector<uint8_t>(data, data + size)});
}

bool AudioJitterBuffer::pop(std::vector<uint8_t>& out, uint64_t now_ms) {
    if (pending_.empty()) return false;

    auto find_seq = [&](uint16_t want) -> int {
        for (size_t i = 0; i < pending_.size(); ++i)
            if (pending_[i].seq == want) return static_cast<int>(i);
        return -1;
    };

    int idx = find_seq(next_seq_);
    if (idx < 0) {
        // The next packet hasn't arrived. Wait for it, unless the oldest
        // packet we are holding has already waited longer than a reorder
        // could explain -- then the gap is loss: skip ahead to the oldest
        // packet we hold.
        uint16_t oldest = pending_.front().seq;
        uint64_t oldest_arrived = pending_.front().arrived_ms;
        for (const auto& e : pending_) {
            if (seq_before(e.seq, oldest)) oldest = e.seq;
            if (e.arrived_ms < oldest_arrived) oldest_arrived = e.arrived_ms;
        }
        if (now_ms - oldest_arrived < kMaxWaitMs && pending_.size() < kMaxLate)
            return false;
        lost_ += static_cast<uint16_t>(oldest - next_seq_);
        next_seq_ = oldest;
        idx = find_seq(next_seq_);
        if (idx < 0) return false;  // defensive; should not happen
    }

    out = std::move(pending_[idx].data);
    pending_.erase(pending_.begin() + idx);
    ++next_seq_;
    return true;
}

}  // namespace gnx::stream
