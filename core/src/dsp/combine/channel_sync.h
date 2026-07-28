#pragma once
#include "../buffer/buffer.h"
#include "../stream.h"
#include "../types.h"
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstring>

namespace dsp::combine {

    // Accumulates samples from N streams that are logically simultaneous but need not
    // arrive in matching block sizes, and hands back the largest prefix every channel can
    // supply. See PHASING_PLAN.md section 1.4.
    //
    // This exists because the obvious approach does not work. Reading one buffer from each
    // input per pass requires the inputs to deliver the same *number of reads*, not merely
    // the same number of samples: a channel arriving in 4800-sample blocks runs dry while
    // one arriving in 3000-sample blocks still has data queued, and the reader then waits
    // forever on the exhausted side while the other backs up behind it. Callers should ask
    // nextChannel() which stream to read, so only the channel that is behind gets topped
    // up. That also bounds the buffers -- no channel can get more than one read ahead of
    // another -- and leaves upstream backpressure intact.
    //
    // Not thread-safe: intended to be owned by the single worker thread that reads the
    // streams.
    class ChannelSync {
    public:
        ChannelSync() {}

        ~ChannelSync() {
            for (auto& b : bufs) { buffer::free(b); }
        }

        void init(int channels, int capacity = STREAM_BUFFER_SIZE) {
            for (auto& b : bufs) { buffer::free(b); }
            bufs.clear();
            sizes.assign(channels, 0);
            cap = capacity;
            for (int i = 0; i < channels; i++) { bufs.push_back(buffer::alloc<complex_t>(cap)); }
            discards.store(0, std::memory_order_relaxed);
        }

        int channels() const { return (int)bufs.size(); }

        // The channel holding the fewest samples, i.e. the one to read from next. Ties go
        // to the lowest index, so channel 0 is read first from a cold start.
        int nextChannel() const {
            int best = 0;
            for (int i = 1; i < (int)sizes.size(); i++) {
                if (sizes[i] < sizes[best]) { best = i; }
            }
            return best;
        }

        void feed(int channel, const complex_t* src, int count) {
            if (channel < 0 || channel >= (int)bufs.size() || count <= 0) { return; }
            complex_t* buf = bufs[channel];
            int& size = sizes[channel];

            if (count >= cap) {
                // Pathological: one read alone fills the buffer. Keep the newest.
                discards.fetch_add(size + (count - cap), std::memory_order_relaxed);
                memcpy(buf, src + (count - cap), cap * sizeof(complex_t));
                size = cap;
                return;
            }

            if (size + count > cap) {
                // Drop the oldest held samples to make room. Dropping the oldest rather
                // than the newest keeps the channels as close to aligned as possible.
                const int over = size + count - cap;
                discards.fetch_add(over, std::memory_order_relaxed);
                memmove(buf, buf + over, (size - over) * sizeof(complex_t));
                size -= over;
            }

            memcpy(buf + size, src, count * sizeof(complex_t));
            size += count;
        }

        // How many samples every channel can currently supply.
        int available() const {
            if (sizes.empty()) { return 0; }
            return *std::min_element(sizes.begin(), sizes.end());
        }

        const complex_t* data(int channel) const { return bufs[channel]; }

        void consume(int count) {
            for (int i = 0; i < (int)bufs.size(); i++) {
                const int left = sizes[i] - count;
                if (left > 0) { memmove(bufs[i], bufs[i] + count, left * sizeof(complex_t)); }
                sizes[i] = std::max(0, left);
            }
        }

        void reset() {
            std::fill(sizes.begin(), sizes.end(), 0);
            discards.store(0, std::memory_order_relaxed);
        }

        // Samples dropped because a channel ran far enough ahead to overrun its buffer.
        // Must stay at zero in normal operation; a non-zero value means the channels are
        // badly out of step, not merely block-misaligned.
        uint64_t discardCount() const { return discards.load(std::memory_order_relaxed); }

    private:
        std::vector<complex_t*> bufs;
        std::vector<int> sizes;
        int cap = 0;
        std::atomic<uint64_t> discards{ 0 };
    };
}
