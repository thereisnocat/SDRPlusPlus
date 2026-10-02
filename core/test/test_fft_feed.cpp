// Checks for the spectrum feed's hand-off: Splitter::bindTap + Reshaper direct-feed mode.
//
// The defect this guards against: the FFT used to be a low-priority *stream* on the IQ
// splitter, and a stream is a single-slot hand-off, so whenever the reader thread was even a
// quarter of a millisecond late to wake, that frame was skipped -- a hole in the sample
// sequence. At the RSR200's USB rate (340-sample frames, ~4500 per second) that is common on
// Windows, and an FFT window stitched across holes splatters every strong carrier into a
// smooth skirt tens of kHz wide that isn't in the signal at all (found 2026-10-02: live
// spread on Windows, none in the recording, none on a Mac).
//
// The signal here is a numbered ramp (sample n has re = n), so a hole anywhere shows up as
// a step in the ramp.
//
// Header-only dependencies plus volk. Build and run:
//
//   c++ -std=c++17 -O2 -pthread -I<repo>/core/src -I/opt/homebrew/include \
//       -o /tmp/test_fft_feed core/test/test_fft_feed.cpp -L/opt/homebrew/lib -lvolk

#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/routing/splitter.h>
#include <dsp/buffer/reshaper.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdio>
#include <string>

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-66s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

static constexpr int BLOCK = 340;          // the RSR200 USB packet at 24-bit dual channel
static constexpr int KEEP = 4096;          // FFT window under test
using namespace std::chrono_literals;

// Counts the steps in a ramp of consecutive windows: a step is any place where the next
// sample isn't the previous one plus one (inside a window or across two windows).
struct RampChecker {
    bool haveLast = false;
    float last = 0;
    long steps = 0;
    long samples = 0;
    void feed(const dsp::complex_t* d, int n) {
        for (int i = 0; i < n; i++) {
            if (haveLast && d[i].re != last + 1.0f) { steps++; }
            last = d[i].re;
            haveLast = true;
        }
        samples += n;
    }
};

// A source that emits `blocks` numbered frames of BLOCK samples, paced to roughly the real
// USB rate, into `src`.
static std::thread startProducer(dsp::stream<dsp::complex_t>& src, int blocks, std::atomic<bool>& done) {
    return std::thread([&src, blocks, &done]() {
        float n = 0;
        for (int b = 0; b < blocks; b++) {
            for (int i = 0; i < BLOCK; i++) {
                src.writeBuf[i].re = n++;
                src.writeBuf[i].im = 0;
            }
            if (!src.swap(BLOCK)) { break; }
            std::this_thread::sleep_for(200us);
        }
        done = true;
    });
}

static void tapThunk(const dsp::complex_t* data, int count, void* ctx) {
    ((dsp::buffer::Reshaper<dsp::complex_t>*)ctx)->tryFeed(data, count);
}

// The reader's life: take a window, check it, and every so often stall the way a busy GUI
// does.
static void consume(dsp::stream<dsp::complex_t>& out, RampChecker& rc, int stallEvery, std::chrono::milliseconds stall, std::atomic<bool>& stop) {
    int w = 0;
    while (!stop) {
        int n = out.read();
        if (n < 0) { break; }
        rc.feed(out.readBuf, n);
        out.flush();
        if (stallEvery && (++w % stallEvery) == 0) { std::this_thread::sleep_for(stall); }
    }
}

// --- 1. The tap delivers every sample, in order, through a jittery reader. -----------------
static void test_tap_is_gapless_under_reader_jitter() {
    printf("tap feed, reader stalls 12 ms every 16th window\n");
    dsp::stream<dsp::complex_t> src, placeholder;
    dsp::routing::Splitter<dsp::complex_t> split(&src);
    dsp::buffer::Reshaper<dsp::complex_t> reshape;
    reshape.init(&placeholder, KEEP, 0);
    reshape.enableDirectFeed();
    split.bindTap(tapThunk, &reshape);
    reshape.start();
    split.start();

    std::atomic<bool> done{ false }, stop{ false };
    RampChecker rc;
    std::thread reader(consume, std::ref(reshape.out), std::ref(rc), 16, 12ms, std::ref(stop));
    auto producer = startProducer(src, 4000, done);
    producer.join();
    std::this_thread::sleep_for(300ms);

    const auto st = reshape.getFeedStats();
    stop = true;
    split.stop();
    reshape.stop();
    reshape.out.stopReader();   // the real downstream block does this when it stops
    reader.join();

    check(rc.samples > KEEP * 100, "enough windows were produced to mean something");
    check(rc.steps == 0, "no hole anywhere in the delivered sample sequence");
    check(st.dropped == 0, "nothing refused for lack of room");
    check(st.blocks >= 3900, "(sanity) the tap was offered the frames");
    check(st.behind > 0 && st.maxBacklog > BLOCK, "(sanity) the reader really was late -- frames waited in the ring");
    printf("    [%ld samples, %llu frames, %llu late, worst backlog %d samples]\n", rc.samples,
           (unsigned long long)st.blocks, (unsigned long long)st.behind, st.maxBacklog);
}

// --- 2. The same jitter through a low-priority *stream* does lose frames. -------------------
// Not a test of new code: it pins the defect, so check 1 can't pass vacuously (a scenario
// too gentle to hurt the old design would prove nothing about the new one).
static void test_old_low_priority_stream_loses_frames_under_the_same_jitter() {
    printf("old low-priority stream, reader stalls 3 ms every 4th frame\n");
    dsp::stream<dsp::complex_t> src, fftIn;
    dsp::routing::Splitter<dsp::complex_t> split(&src);
    split.bindStream(&fftIn, true);
    split.start();

    std::atomic<bool> done{ false }, stop{ false };
    RampChecker rc;
    // Reader of the single-slot stream, stalling like the display thread can.
    std::thread reader([&]() {
        int w = 0;
        while (!stop) {
            int n = fftIn.read();
            if (n < 0) { break; }
            rc.feed(fftIn.readBuf, n);
            fftIn.flush();
            if ((++w % 4) == 0) { std::this_thread::sleep_for(3ms); }
        }
    });
    auto producer = startProducer(src, 4000, done);
    producer.join();
    std::this_thread::sleep_for(100ms);
    stop = true;
    split.stop();
    fftIn.stopReader();
    reader.join();

    check(rc.steps > 100, "frames are lost: the sample sequence is full of holes");
    printf("    [%ld holes in %ld samples]\n", rc.steps, rc.samples);
}

// --- 3. The tap never blocks the producer, even with a dead reader. ------------------------
static void test_a_stalled_reader_costs_drops_not_producer_time() {
    printf("tap feed, reader never drains\n");
    dsp::stream<dsp::complex_t> src, placeholder;
    dsp::routing::Splitter<dsp::complex_t> split(&src);
    dsp::buffer::Reshaper<dsp::complex_t> reshape;
    reshape.init(&placeholder, KEEP, 0);
    reshape.enableDirectFeed();
    split.bindTap(tapThunk, &reshape);
    reshape.start();
    split.start();

    // A reshaper that nobody reads from fills its ring and the worker blocks on `out`.
    std::atomic<bool> done{ false };
    auto t0 = std::chrono::steady_clock::now();
    auto producer = startProducer(src, 6000, done);   // ~2M samples, well past the ring's headroom
    producer.join();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    const auto st = reshape.getFeedStats();
    split.stop();
    reshape.stop();

    check(done, "the producer finished all its frames");
    check(st.dropped > 0, "the overflow was counted as drops");
    check(elapsed < 6s, "the producer was never held up by the dead reader");
    printf("    [%llu dropped of %llu, %lld ms]\n", (unsigned long long)st.dropped, (unsigned long long)st.blocks,
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

// --- 4. Restart forgets old samples; a stopped reshaper refuses the feed. ------------------
static void test_restart_clears_the_ring_and_stopped_means_unwanted() {
    printf("restart behaviour\n");
    dsp::stream<dsp::complex_t> placeholder;
    dsp::buffer::Reshaper<dsp::complex_t> reshape;
    reshape.init(&placeholder, KEEP, 0);
    reshape.enableDirectFeed();

    std::vector<dsp::complex_t> blk(BLOCK);
    for (int i = 0; i < BLOCK; i++) { blk[i].re = (float)i; blk[i].im = 0; }

    check(!reshape.tryFeed(blk.data(), BLOCK), "a reshaper that isn't running refuses a feed");
    check(reshape.getFeedStats().blocks == 0, "...and doesn't count it");

    reshape.start();
    check(reshape.tryFeed(blk.data(), BLOCK), "a running one accepts it");
    reshape.stop();
    check(!reshape.tryFeed(blk.data(), BLOCK), "after stop() it refuses again");

    // Restart: what was fed before the stop must not be in front of what is fed after.
    reshape.start();
    std::vector<dsp::complex_t> fresh(KEEP);
    for (int i = 0; i < KEEP; i++) { fresh[i].re = 1000.0f + i; fresh[i].im = 0; }
    reshape.tryFeed(fresh.data(), KEEP);
    int n = reshape.out.read();
    check(n == KEEP && reshape.out.readBuf[0].re == 1000.0f, "the first window after a restart starts at the new data");
    reshape.out.flush();
    reshape.stop();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_tap_is_gapless_under_reader_jitter();
    test_old_low_priority_stream_loses_frames_under_the_same_jitter();
    test_a_stalled_reader_costs_drops_not_producer_time();
    test_restart_clears_the_ring_and_stopped_means_unwanted();
    printf("\n%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
