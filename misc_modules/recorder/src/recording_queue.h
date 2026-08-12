#pragma once
#include <cstdint>
#include <cstddef>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <atomic>
#include <functional>

// Decouples the recorder's disk I/O from the live DSP path. See
// RECORDING_PERFORMANCE_PLAN.md for the full design rationale -- short version: writing
// directly to disk on the same thread that drains the shared IQFrontEnd splitter can, under a
// transient disk stall, block that thread long enough to propagate backpressure all the way
// back to the source and cause dropped samples (and, per that plan's section 2.1, stall every
// *other* consumer of the same front-end too, not just the recording). This class is the fix:
// the DSP-facing side (push()) does the minimum possible work -- copy into a bounded queue and
// return -- and a dedicated writer thread drains it through whatever write function the caller
// supplied, off the DSP critical path entirely.
//
// Deliberately not coupled to wav::Writer specifically (a plain std::function instead) --
// nothing about batching and pacing writes onto a background thread needs to know anything
// about WAV/RIFF, and staying decoupled means this class can be tested on its own, with a
// plain in-memory callback, without linking against core (matching rsr200_protocol.h's own
// "deliberately free of any SDR++ dependency" precedent for exactly this reason).
//
// Bounded by a fixed RAM budget (not a fixed time target -- see the plan doc's section 5 for
// why), so the buffered *duration* automatically shrinks at higher configured bandwidths
// instead of a fixed number of seconds silently costing an unreasonable amount of memory. When
// the budget is exhausted, push() drops the incoming block rather than blocking (which would
// just reintroduce the exact hazard this class exists to remove) or growing without bound (an
// OOM risk on a long enough sustained shortfall) -- and counts it, visibly, rather than
// silently.
class RecordingQueue {
public:
    // Matches wav::Writer::write()'s own signature exactly (float*, not const float* --
    // historical on that class's part, not this one's), so the common case is binding it
    // directly: RecordingQueue q([this](float* d, int c){ writer.write(d, c); });
    using WriteFn = std::function<void(float* data, int count)>;

    // ramBudgetBytes: how much memory queued-but-not-yet-written sample data may occupy
    // before push() starts dropping. Independent of whatever on-disk sample type the write
    // function's own target eventually converts to -- everything queued here is the raw
    // float data the caller already had, exactly what used to go straight into
    // wav::Writer::write(); that conversion (Int16/Int32/Uint8/Float32) still happens inside
    // that same, unmodified call, just now on this class's writer thread instead of the
    // caller's.
    explicit RecordingQueue(WriteFn writeFn, size_t ramBudgetBytes = 512ull * 1024 * 1024)
        : writeFn(std::move(writeFn)), ramBudgetBytes(ramBudgetBytes) {}

    ~RecordingQueue() { stop(); }

    // Starts the writer thread. Resets the gap counter -- a fresh recording starts at 0,
    // same as RSR200's own "Sequence gaps seen" this mirrors.
    void start() {
        if (running.load()) { return; }
        {
            std::lock_guard<std::mutex> lck(mtx);
            draining = false;
            queuedBytes = 0;
        }
        gapCount = 0;
        running = true;
        writerThread = std::thread(&RecordingQueue::run, this);
    }

    // Stops accepting new samples immediately, then blocks until the writer thread has
    // drained whatever was already queued and exited -- so the caller can safely finalize
    // whatever it was writing to (close a file, patch trailing metadata) right after this
    // returns, with every sample that was ever successfully push()ed guaranteed written.
    //
    // Safe to call from a different thread than push() runs on, but only meaningful once the
    // caller has already guaranteed push() will not be called again -- every real call site
    // (a dsp::block's own worker thread, joined via doStop()/dualWorker's own join before its
    // RecorderModule::stop() gets this far) already provides that ordering; this class does
    // not attempt to detect or guard against a push() arriving concurrently with stop().
    void stop() {
        if (!running.load()) { return; }
        {
            std::lock_guard<std::mutex> lck(mtx);
            draining = true;
        }
        cv.notify_all();
        if (writerThread.joinable()) { writerThread.join(); }
        running = false;
    }

    // Copies count frames (== count * channels floats, exactly what wav::Writer::write()
    // already expects) into the queue and returns immediately -- never blocks the caller on
    // disk I/O. Drops the block and increments the gap counter instead if queuing it would
    // exceed the RAM budget. Meant for exactly one producer thread per instance, matching
    // every existing recorder call site (one DSP sink/worker thread each).
    void push(const float* data, int count, int channels) {
        const size_t floats = (size_t)count * (size_t)channels;
        const size_t bytes = floats * sizeof(float);
        std::lock_guard<std::mutex> lck(mtx);
        if (queuedBytes + bytes > ramBudgetBytes) {
            gapCount++;
            return;
        }
        // Recycle a previously-used buffer instead of allocating fresh every call -- pushed
        // chunk sizes are steady in practice (a fixed configured rate/decimation), so
        // assign() hits its no-reallocation fast path (pure memcpy) almost every time once
        // the free list has warmed up. This matters: push() runs synchronously on whichever
        // thread drains the shared IQFrontEnd splitter's bound stream for this recording
        // (complexHandler/dualWorker), an ordinary blocking, normal-priority consumer of that
        // splitter (see RECORDING_PERFORMANCE_PLAN.md section 2.1/phase 8) -- a malloc+copy
        // on every single call, at a high enough sustained sample rate, is slow enough on its
        // own to become the very hazard this class exists to remove, just moved from "disk
        // I/O on this thread" to "heap allocation on this thread." See phase 10.
        std::vector<float> buf;
        if (!freeList.empty()) {
            buf = std::move(freeList.back());
            freeList.pop_back();
        }
        buf.assign(data, data + floats);
        blocks.push_back(Block{ std::move(buf), count });
        queuedBytes += bytes;
        cv.notify_all();
    }

    // Number of blocks dropped so far because the RAM budget was exhausted -- the recorder's
    // own "Recording gaps: N" readout (RECORDING_PERFORMANCE_PLAN.md phase 5) reads this
    // directly.
    uint64_t getGapCount() const { return gapCount.load(); }

    // Bytes currently queued but not yet written. Informational (e.g. a live "buffered: N.Ns"
    // readout derived from this and the configured rate) -- not needed for correctness.
    size_t getQueuedBytes() const {
        std::lock_guard<std::mutex> lck(mtx);
        return queuedBytes;
    }

    size_t getRamBudgetBytes() const { return ramBudgetBytes; }

private:
    struct Block {
        std::vector<float> data;
        int count;   // frames, passed straight through to the write function
    };

    void run() {
        while (true) {
            Block block;
            {
                std::unique_lock<std::mutex> lck(mtx);
                cv.wait(lck, [this] { return !blocks.empty() || draining; });
                if (blocks.empty()) {
                    // Only actually exit once draining *and* fully drained -- a wake with
                    // draining already true but blocks still nonempty (the common shutdown
                    // case: stop() flips draining while a backlog still exists) falls through
                    // to the write below instead, same as any other wake.
                    if (draining) { return; }
                    continue;
                }
                block = std::move(blocks.front());
                blocks.pop_front();
                queuedBytes -= block.data.size() * sizeof(float);
            }
            writeFn(block.data.data(), block.count);

            // Hand the now-empty buffer back for push() to reuse rather than letting it
            // free() here and push() malloc() a fresh one next time.
            {
                std::lock_guard<std::mutex> lck(mtx);
                freeList.push_back(std::move(block.data));
            }
        }
    }

    WriteFn writeFn;
    size_t ramBudgetBytes;

    mutable std::mutex mtx;
    std::condition_variable cv;
    std::deque<Block> blocks;
    // Recycled buffers from already-written blocks, reused by push() -- see push()'s own
    // comment. Self-bounded in practice: it can never hold more buffers than were ever
    // simultaneously in flight, which the RAM budget already caps.
    std::vector<std::vector<float>> freeList;
    size_t queuedBytes = 0;
    bool draining = false;

    std::atomic<bool> running{ false };
    std::atomic<uint64_t> gapCount{ 0 };
    std::thread writerThread;
};
