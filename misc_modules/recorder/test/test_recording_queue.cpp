// Checks for RecordingQueue: the RAM cap actually bounds queued memory, a full queue drops
// and counts rather than blocking the producer, an orderly stop() drains everything already
// queued, and a fresh start() resets the gap counter. See RECORDING_PERFORMANCE_PLAN.md
// phase 1.
//
//   c++ -std=c++17 -O2 -pthread -o /tmp/t test/test_recording_queue.cpp && /tmp/t
//
// Deliberately standalone (no core dependency) -- RecordingQueue takes a plain
// std::function write callback rather than a wav::Writer, exactly so it can be tested this
// way. See recording_queue.h's own comment for why.

#include "../src/recording_queue.h"
#include <cstdio>
#include <string>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <atomic>

// Counts allocations at or above a "this is clearly a block's float buffer, not some small
// STL/thread-internal bookkeeping allocation" threshold -- scoped to this whole test binary,
// which is fine since this file is always compiled standalone into its own executable (see
// run_tests.sh's STANDALONE list), never linked alongside other code that would also hit
// these overrides.
static std::atomic<long> bigAllocCount{ 0 };
static constexpr size_t BIG_ALLOC_THRESHOLD = 1024;   // bytes

void* operator new(size_t sz) {
    if (sz >= BIG_ALLOC_THRESHOLD) { bigAllocCount++; }
    void* p = std::malloc(sz);
    if (!p) { throw std::bad_alloc(); }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-62s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

int main() {
    printf("\nRecordingQueue\n\n");

    // -----------------------------------------------------------------
    printf("Basic drain: everything pushed comes out, in order, unmodified\n");
    {
        std::mutex outMtx;
        std::vector<std::vector<float>> received;
        RecordingQueue q([&](float* d, int c) {
            std::lock_guard<std::mutex> lck(outMtx);
            received.emplace_back(d, d + c);   // channels=1 in this test, so c == floats
        }, 1024 * 1024);
        q.start();

        std::vector<std::vector<float>> sent = {
            { 1.0f, 2.0f, 3.0f },
            { 4.0f, 5.0f },
            { 6.0f, 7.0f, 8.0f, 9.0f },
        };
        for (auto& b : sent) { q.push(b.data(), (int)b.size(), 1); }
        q.stop();   // blocks until fully drained

        check(received.size() == sent.size(), "every pushed block was written");
        bool orderAndContentMatch = (received.size() == sent.size());
        if (orderAndContentMatch) {
            for (size_t i = 0; i < sent.size(); i++) {
                if (received[i] != sent[i]) { orderAndContentMatch = false; break; }
            }
        }
        check(orderAndContentMatch, "blocks arrive in order with content unchanged");
        check(q.getGapCount() == 0, "nothing dropped when well under budget");
        check(q.getQueuedBytes() == 0, "queue is empty once fully drained");
    }

    // -----------------------------------------------------------------
    printf("\nRAM cap bounds queued memory and drops (not blocks) beyond it\n");
    {
        // A writer that blocks until the test explicitly releases it -- deterministically
        // holds the writer thread inside one write() call so pushes made meanwhile
        // accumulate in the queue instead of racing to be drained immediately.
        std::mutex gate;
        std::condition_variable enteredCv, releaseCv;
        bool entered = false, released = false;
        auto blockingWrite = [&](float*, int) {
            {
                std::lock_guard<std::mutex> lck(gate);
                entered = true;
            }
            enteredCv.notify_all();
            std::unique_lock<std::mutex> lck(gate);
            releaseCv.wait(lck, [&] { return released; });
        };

        const int blockFloats = 100;                          // 400 bytes/block
        const size_t bytesPerBlock = blockFloats * sizeof(float);
        const size_t budget = 3 * bytesPerBlock;               // room for exactly 3 queued blocks
        RecordingQueue q(blockingWrite, budget);
        q.start();

        std::vector<float> block(blockFloats, 1.0f);

        // First push is picked up by the writer thread almost immediately and blocks it
        // there -- wait for confirmation rather than racing on timing.
        q.push(block.data(), blockFloats, 1);
        {
            std::unique_lock<std::mutex> lck(gate);
            enteredCv.wait(lck, [&] { return entered; });
        }
        check(q.getQueuedBytes() == 0, "the in-flight block is not counted as queued");

        // Three more exactly fill the budget (the writer thread is stuck, so none of these
        // get drained meanwhile).
        q.push(block.data(), blockFloats, 1);
        q.push(block.data(), blockFloats, 1);
        q.push(block.data(), blockFloats, 1);
        check(q.getQueuedBytes() == budget, "queue fills to exactly the configured budget");
        check(q.getGapCount() == 0, "nothing dropped while still within budget");

        // A push that would exceed the budget must return promptly (not block the producer)
        // and be counted as a gap rather than silently queued or silently discarded.
        auto t0 = std::chrono::steady_clock::now();
        q.push(block.data(), blockFloats, 1);
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        check(elapsedMs < 100, "push() past the budget returns immediately, does not block");
        check(q.getGapCount() == 1, "the over-budget push is counted as a gap");
        check(q.getQueuedBytes() == budget, "queued bytes unchanged by a dropped push");

        // A second over-budget push counts again.
        q.push(block.data(), blockFloats, 1);
        check(q.getGapCount() == 2, "a further over-budget push increments the gap count again");

        // Release the writer thread and let it drain the rest.
        {
            std::lock_guard<std::mutex> lck(gate);
            released = true;
        }
        releaseCv.notify_all();
        q.stop();
        check(q.getQueuedBytes() == 0, "everything still queued at release time is drained by stop()");
    }

    // -----------------------------------------------------------------
    printf("\nA fresh start() resets the gap counter\n");
    {
        const size_t tinyBudget = 4;   // one float doesn't even fit
        RecordingQueue q([](float*, int) {}, tinyBudget);
        q.start();
        std::vector<float> block(10, 0.0f);
        q.push(block.data(), 10, 1);
        q.push(block.data(), 10, 1);
        q.stop();
        check(q.getGapCount() == 2, "gaps accumulated during the first recording");

        q.start();
        check(q.getGapCount() == 0, "a fresh start() resets the count for a new recording");
        q.stop();
    }

    // -----------------------------------------------------------------
    // See RECORDING_PERFORMANCE_PLAN.md phase 10: push() used to allocate a fresh buffer on
    // every call, synchronously on the same thread that drains the shared IQFrontEnd
    // splitter's bound stream for this recording -- slow enough at sustained high sample
    // rates to become the very hazard this class exists to remove, just relocated from disk
    // I/O to heap churn. Buffers are now recycled through a free list instead.
    printf("\nBuffers are recycled -- steady-state push() does not allocate a fresh block\n");
    {
        std::mutex outMtx;
        std::condition_variable outCv;
        int writtenCount = 0;
        RecordingQueue q([&](float*, int) {
            std::lock_guard<std::mutex> lck(outMtx);
            writtenCount++;
            outCv.notify_all();
        }, 1024 * 1024);
        q.start();

        // Well above BIG_ALLOC_THRESHOLD, so its allocations are unambiguously visible.
        const int blockFloats = 4096;
        std::vector<float> block(blockFloats, 1.0f);

        auto pushAndWaitDrained = [&](int expectedCount) {
            q.push(block.data(), blockFloats, 1);
            std::unique_lock<std::mutex> lck(outMtx);
            outCv.wait(lck, [&] { return writtenCount == expectedCount; });
        };

        // Warm up the free list -- the first several pushes are expected to allocate (there's
        // nothing to recycle yet). Pushing one at a time and waiting for the writer thread to
        // finish with each before the next arrives keeps the free list from ever needing more
        // than one recycled buffer here.
        for (int i = 1; i <= 5; i++) { pushAndWaitDrained(i); }

        bigAllocCount = 0;
        for (int i = 6; i <= 25; i++) { pushAndWaitDrained(i); }
        check(bigAllocCount.load() == 0, "no allocation >=1KB across 20 pushes once warmed up");

        q.stop();
    }

    printf("\n%s (%d failures)\n", failures == 0 ? "PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
