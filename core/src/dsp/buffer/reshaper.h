#pragma once
#include "../block.h"
#include "ring_buffer.h"
#include <atomic>
#include <mutex>

// IMPORTANT: THIS IS TRASH AND MUST BE REWRITTEN IN THE FUTURE

namespace dsp::buffer {
    // NOTE: I'm not proud of this, it's BAD and just taken from the previous DSP, but it works...
    template <class T>
    class Reshaper : public block {
        using base_type = block;
    public:
        Reshaper() {}

        Reshaper(stream<T>* in, int keep, int skip) { init(in, keep, skip); }

        // NOTE: For some reason, the base class destructor doesn't get called.... this is a temporary fix I guess
        // I also don't check for _block_init for the exact sample reason, something's weird
        ~Reshaper() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<T>* in, int keep, int skip) {
            _in = in;
            _keep = keep;
            _skip = skip;
            ringBuf.init(keep * 2);
            base_type::registerInput(_in);
            base_type::registerOutput(&out);
            base_type::_block_init = true;
        }

        void setInput(stream<T>* in) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            base_type::unregisterInput(_in);
            _in = in;
            base_type::registerInput(_in);
            base_type::tempStart();
        }

        void setKeep(int keep) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _keep = keep;
            ringBuf.setMaxLatency(maxLatencyFor(keep));
            base_type::tempStart();
        }

        // Direct-feed mode: instead of reading `in` on a thread of its own, the ring buffer
        // is filled by whoever calls tryFeed() -- in practice a Splitter tap running on the
        // source's own thread. Nothing is ever handed across a thread boundary one block at
        // a time, so a reader that is late to wake costs latency (buffered in the ring, which
        // is sized to hold well over a hundred milliseconds) instead of lost samples; and a
        // producer that must not wait never does. Call before start().
        void enableDirectFeed() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _directFeed = true;
            ringBuf.setMaxLatency(maxLatencyFor(_keep));
        }

        // Feed statistics (direct-feed mode), cumulative since the last resetFeedStats().
        struct FeedStats {
            uint64_t blocks = 0;     // blocks offered
            uint64_t behind = 0;     // blocks that arrived with at least a whole block of the
                                     // previous data still unread -- i.e. the reader was late
                                     // enough that a single-slot hand-off would have lost one
            uint64_t dropped = 0;    // blocks refused for lack of room (the reader was stalled
                                     // for longer than the ring's whole headroom)
            int maxBacklog = 0;      // most samples ever waiting unread
        };

        FeedStats getFeedStats() {
            std::lock_guard<std::mutex> lck(feedMtx);
            return feedStats;
        }

        void resetFeedStats() {
            std::lock_guard<std::mutex> lck(feedMtx);
            feedStats = FeedStats();
        }

        // Non-blocking. Returns false (and counts a drop) if there was no room, or if the
        // reshaper isn't running -- in which case the block is simply not wanted.
        bool tryFeed(const T* data, int count) {
            std::lock_guard<std::mutex> lck(feedMtx);
            if (!feeding) { return false; }
            feedStats.blocks++;
            const int backlog = ringBuf.getReadable();
            if (backlog >= count) { feedStats.behind++; }
            if (backlog > feedStats.maxBacklog) { feedStats.maxBacklog = backlog; }
            if (!ringBuf.tryWrite(const_cast<T*>(data), count)) {
                feedStats.dropped++;
                return false;
            }
            return true;
        }

        void setSkip(int skip) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _skip = skip;
            base_type::tempStart();
        }

        int run() {
            int count = _in->read();
            if (count < 0) { return -1; }
            ringBuf.write(_in->readBuf, count);
            _in->flush();
            return count;
        }

        stream<T> out;

    private:
        void doStart() override {
            if (_directFeed) {
                // Start from an empty ring so a restart (an FFT size or rate change)
                // never splices old samples to new ones, then open the feed.
                std::lock_guard<std::mutex> lck(feedMtx);
                ringBuf.clear();
                bufferWorkerThread = std::thread(&Reshaper<T>::bufferWorker, this);
                feeding = true;
                return;
            }
            workThread = std::thread(&Reshaper<T>::loop, this);
            bufferWorkerThread = std::thread(&Reshaper<T>::bufferWorker, this);
        }

        void loop() {
            while (run() >= 0)
                ;
        }

        void doStop() override {
            if (_directFeed) {
                std::lock_guard<std::mutex> lck(feedMtx);
                feeding = false;
            }
            _in->stopReader();
            ringBuf.stopReader();
            out.stopWriter();
            ringBuf.stopWriter();

            if (workThread.joinable()) {
                workThread.join();
            }
            if (bufferWorkerThread.joinable()) {
                bufferWorkerThread.join();
            }

            _in->clearReadStop();
            ringBuf.clearReadStop();
            out.clearWriteStop();
            ringBuf.clearWriteStop();
        }

        void bufferWorker() {
            T* buf = new T[_keep];
            bool delay = _skip < 0;

            int readCount = std::min<int>(_keep + _skip, _keep);
            int skip = std::max<int>(_skip, 0);
            int delaySize = (-_skip) * sizeof(T);
            int delayCount = (-_skip);

            T* start = &buf[std::max<int>(-_skip, 0)];
            T* delayStart = &buf[_keep + _skip];

            while (true) {
                if (delay) {
                    memmove(buf, delayStart, delaySize);
                    if constexpr (std::is_same_v<T, complex_t> || std::is_same_v<T, stereo_t>) {
                        for (int i = 0; i < delayCount; i++) {
                            buf[i].re /= 10.0f;
                            buf[i].im /= 10.0f;
                        }
                    }
                }
                if (ringBuf.readAndSkip(start, readCount, skip) < 0) { break; };
                memcpy(out.writeBuf, buf, _keep * sizeof(T));
                if (!out.swap(_keep)) { break; }
            }
            delete[] buf;
        }

        // Ring headroom for a given window size. The classic mode keeps it at two windows;
        // direct-feed mode wants far more, since it is the only buffering between a
        // producer that can never wait and a reader that is allowed to be late: about a
        // quarter second at typical rates, bounded by the ring's own capacity.
        int maxLatencyFor(int keep) {
            if (!_directFeed) { return keep * 2; }
            return std::min<int>(std::max<int>(keep * 4, 262144), ringBuf.capacity() - 4096);
        }

        stream<T>* _in;
        int _outBlockSize;
        RingBuffer<T> ringBuf;
        bool _directFeed = false;
        std::mutex feedMtx;
        bool feeding = false;
        FeedStats feedStats;
        std::thread bufferWorkerThread;
        std::thread workThread;
        int _keep, _skip;
    };
}