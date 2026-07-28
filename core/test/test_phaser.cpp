// Checks for dsp::combine::Phaser, the two-channel phasing combiner.
//
// Needs the core library, so build that first (cmake --build build), then:
//
//   c++ -std=c++17 -O2 -I<repo>/core/src -I/opt/homebrew/include \
//       -o /tmp/test_phaser core/test/test_phaser.cpp \
//       -L<repo>/build/core -lsdrpp_core -L/opt/homebrew/lib -lvolk -Wl,-rpath,<repo>/build/core
//
// The point of interest is the ring buffering. dsp::Operator, the obvious base class for
// a two-input block, discards both inputs whenever their counts disagree, and
// dsp::stream::flush() cannot partially consume. So these tests deliberately feed A and B
// in mismatched block sizes and assert that nothing is dropped and the output is still
// sample-accurate. See PHASING_PLAN.md sections 1.4 and 2.2.

#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/combine/phaser.h>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdio>
#include <cmath>
#include <string>

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-58s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

// Drives a phaser with two generated channels and collects the output.
// blockA/blockB are the write sizes, deliberately allowed to differ.
struct Harness {
    dsp::stream<dsp::complex_t> a, b;
    dsp::combine::Phaser phaser;
    std::vector<dsp::complex_t> collected;
    std::atomic<bool> collecting{ true };
    std::thread ta, tb, tc;

    // wanted tone in A only; interferer in both, in B scaled by (gain, phase).
    double interfGain, interfPhaseDeg;

    Harness(double gain, double phaseDeg) : interfGain(gain), interfPhaseDeg(phaseDeg) {
        phaser.init(&a, &b);
    }

    static dsp::complex_t tone(double amp, double cyclesPerSample, long n) {
        const double ph = 2.0 * M_PI * cyclesPerSample * (double)n;
        return { (float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)) };
    }

    // Channel A: wanted (0.01 cyc/samp, amp 0.1) + interferer (0.03 cyc/samp, amp 1.0)
    // Channel B: interferer only, times the complex weight g.
    void fill(dsp::complex_t* buf, int count, long& n, bool isB) {
        const double gm = std::pow(10.0, interfGain / 20.0);
        const double gp = interfPhaseDeg * M_PI / 180.0;
        const double gr = gm * std::cos(gp), gi = gm * std::sin(gp);
        for (int i = 0; i < count; i++, n++) {
            dsp::complex_t w = tone(0.1, 0.01, n);
            dsp::complex_t f = tone(1.0, 0.03, n);
            if (!isB) {
                buf[i] = { w.re + f.re, w.im + f.im };
            }
            else {
                buf[i] = { (float)(gr * f.re - gi * f.im), (float)(gr * f.im + gi * f.re) };
            }
        }
    }

    void run(int blocksA, int blockA, int blocksB, int blockB) {
        // Snap the working weight to the configured one before any samples flow, exactly
        // as Phasing::build() does. Without this the first block ramps up from w = 0 and
        // the measured residual includes that transient rather than the steady state.
        phaser.reset();
        phaser.start();

        tc = std::thread([this] {
            while (true) {
                int c = phaser.out.read();
                if (c < 0) { break; }
                if (collecting) {
                    for (int i = 0; i < c; i++) { collected.push_back(phaser.out.readBuf[i]); }
                }
                phaser.out.flush();
            }
        });

        ta = std::thread([this, blocksA, blockA] {
            long n = 0;
            for (int k = 0; k < blocksA; k++) {
                fill(a.writeBuf, blockA, n, false);
                if (!a.swap(blockA)) { return; }
            }
        });
        tb = std::thread([this, blocksB, blockB] {
            long n = 0;
            for (int k = 0; k < blocksB; k++) {
                fill(b.writeBuf, blockB, n, true);
                if (!b.swap(blockB)) { return; }
            }
        });

        ta.join();
        tb.join();
        // Let the phaser drain what it has buffered.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        phaser.stop();
        phaser.out.stopReader();
        tc.join();
        phaser.out.clearReadStop();
    }

    double outputPowerDb() const {
        if (collected.empty()) { return -300.0; }
        double acc = 0.0;
        for (const auto& s : collected) { acc += (double)s.re * s.re + (double)s.im * s.im; }
        return 10.0 * std::log10(acc / collected.size() + 1e-300);
    }
};

int main() {
    printf("\ndsp::combine::Phaser\n\n");

    // -----------------------------------------------------------------
    printf("Bypass (MODE_A_ONLY) is bit-identical to channel A\n");
    {
        Harness h(-3.0, 137.0);
        h.phaser.setMode(dsp::combine::Phaser::MODE_A_ONLY);
        h.run(20, 4096, 20, 4096);

        std::vector<dsp::complex_t> expect(h.collected.size());
        long n = 0;
        h.fill(expect.data(), (int)expect.size(), n, false);

        bool exact = !h.collected.empty();
        for (size_t i = 0; i < h.collected.size(); i++) {
            if (h.collected[i].re != expect[i].re || h.collected[i].im != expect[i].im) { exact = false; break; }
        }
        printf("        %zu samples through\n", h.collected.size());
        check(exact, "output is bit-identical to the input on channel A");
        check(h.phaser.getDiscardCount() == 0, "no samples discarded");
    }

    // -----------------------------------------------------------------
    printf("\nManual weight nulls a signal common to both channels\n");
    {
        const double gain = -3.0, phase = 137.0;
        Harness h(gain, phase);
        // B carries the interferer scaled by g, so w = 1/g removes it: -gain, -phase.
        h.phaser.setMode(dsp::combine::Phaser::MODE_MANUAL);
        h.phaser.setWeight((float)-gain, (float)-phase);
        h.run(20, 4096, 20, 4096);

        const double p = h.outputPowerDb();
        // What should remain is the wanted tone at amplitude 0.1, i.e. -20 dB.
        printf("        residual %.2f dB (wanted tone alone is -20.00 dB)\n", p);
        check(std::abs(p - (-20.0)) < 0.5, "interferer removed, wanted tone left standing");
        check(h.phaser.getDiscardCount() == 0, "no samples discarded");

        const float depth = h.phaser.getNullDepth();
        printf("        reported null depth %.2f dB\n", depth);
        check(depth > 10.0f, "null depth metric reports meaningful cancellation");
    }

    // -----------------------------------------------------------------
    // The reason this block does not use dsp::Operator.
    printf("\nMismatched block sizes are handled without loss\n");
    {
        const double gain = -3.0, phase = 137.0;
        Harness h(gain, phase);
        h.phaser.setMode(dsp::combine::Phaser::MODE_MANUAL);
        h.phaser.setWeight((float)-gain, (float)-phase);
        // Same total sample count, wildly different block sizes: 40x3000 vs 25x4800.
        h.run(40, 3000, 25, 4800);

        const double p = h.outputPowerDb();
        printf("        %zu samples out of 120000 in, residual %.2f dB\n", h.collected.size(), p);
        check(h.phaser.getDiscardCount() == 0, "no samples discarded despite unequal blocks");
        check(h.collected.size() > 100000, "nearly all input made it through");
        check(std::abs(p - (-20.0)) < 0.5, "null still correct across block boundaries");
    }

    // -----------------------------------------------------------------
    printf("\nWeight changes are ramped, not stepped\n");
    {
        // A single large jump in w must not put a discontinuity in the output. Feed a
        // constant DC signal so any step shows up directly as a sample-to-sample jump.
        dsp::stream<dsp::complex_t> a, b;
        dsp::combine::Phaser ph;
        ph.init(&a, &b);
        ph.setMode(dsp::combine::Phaser::MODE_MANUAL);
        ph.setWeight(-1000.0f, 0.0f);   // w ~ 0
        ph.start();

        std::vector<dsp::complex_t> got;
        std::thread rd([&] {
            while (true) {
                int c = ph.out.read();
                if (c < 0) { break; }
                for (int i = 0; i < c; i++) { got.push_back(ph.out.readBuf[i]); }
                ph.out.flush();
            }
        });

        const int N = 4096;
        for (int k = 0; k < 6; k++) {
            for (int i = 0; i < N; i++) {
                a.writeBuf[i] = { 1.0f, 0.0f };
                b.writeBuf[i] = { 1.0f, 0.0f };
            }
            if (k == 3) { ph.setWeight(0.0f, 0.0f); }  // jump w from 0 to 1
            a.swap(N);
            b.swap(N);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ph.stop();
        ph.out.stopReader();
        rd.join();
        ph.out.clearReadStop();

        float maxStep = 0.0f;
        for (size_t i = 1; i < got.size(); i++) {
            const float d = std::abs(got[i].re - got[i - 1].re);
            if (d > maxStep) { maxStep = d; }
        }
        printf("        %zu samples, largest sample-to-sample step %.6f\n", got.size(), maxStep);
        // Ramped over a 4096-sample block, the per-sample step is ~1/4096 = 0.00024.
        check(maxStep < 0.01f, "no discontinuity when the weight jumps");
        check(got.size() > 0, "samples were produced");
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
