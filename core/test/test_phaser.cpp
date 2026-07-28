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
#include <complex>

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
    bool identical = false;

    void fill(dsp::complex_t* buf, int count, long& n, bool isB) {
        if (identical) { isB = false; }
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

        // Skip the first block: manual mode runs both channels through a delay line, so
        // the output begins with the line's fill (zeros stepping up to the signal). That
        // is a stream-start transient, not the weight change under test here.
        float maxStep = 0.0f;
        for (size_t i = N + 1; i < got.size(); i++) {
            const float d = std::abs(got[i].re - got[i - 1].re);
            if (d > maxStep) { maxStep = d; }
        }
        printf("        %zu samples, largest step after the delay line fills %.6f\n", got.size(), maxStep);
        // Ramped over a 4096-sample block, the per-sample step is ~1/4096 = 0.00024.
        check(maxStep < 0.01f, "no discontinuity when the weight jumps");
        check(got.size() > (size_t)N, "samples were produced");
    }

    // -----------------------------------------------------------------
    printf("\nThe delay control shifts channel B\n");
    {
        // Identical channels with w = 1 cancel exactly, so any residual is the delay.
        Harness h(0.0, 0.0);
        h.identical = true;
        h.phaser.setMode(dsp::combine::Phaser::MODE_MANUAL);
        h.phaser.setWeight(0.0f, 0.0f);
        h.phaser.setDelay(0.0f);
        h.run(20, 4096, 20, 4096);
        const double aligned = h.outputPowerDb();
        printf("        delay 0.0: residual %.1f dB\n", aligned);
        check(aligned < -60.0, "identical channels cancel when aligned");

        Harness h2(0.0, 0.0);
        h2.identical = true;
        h2.phaser.setMode(dsp::combine::Phaser::MODE_MANUAL);
        h2.phaser.setWeight(0.0f, 0.0f);
        h2.phaser.setDelay(1.0f);
        h2.run(20, 4096, 20, 4096);
        const double shifted = h2.outputPowerDb();

        // Channel A carries tones at 0.01 and 0.03 cycles/sample. One sample of skew
        // rotates them by 2*pi*f, leaving |1 - exp(-j2*pi*f)| of each behind.
        const double r1 = std::abs(1.0 - std::polar(1.0, -2.0 * M_PI * 0.01));
        const double r2 = std::abs(1.0 - std::polar(1.0, -2.0 * M_PI * 0.03));
        const double predicted = 10.0 * std::log10(0.1 * 0.1 * r1 * r1 + 1.0 * r2 * r2);
        printf("        delay 1.0: residual %.2f dB, predicted %.2f dB\n", shifted, predicted);
        check(std::abs(shifted - predicted) < 1.0, "one sample of delay leaves the predicted residual");
        check(shifted > aligned + 20.0, "a delay measurably spoils an otherwise perfect null");

        // A whole-sample delay is a plain copy; only a fractional one exercises the
        // interpolator. Testing integers alone once hid an inverted fractional term that
        // made the effective delay di - f instead of di + f.
        Harness h3(0.0, 0.0);
        h3.identical = true;
        h3.phaser.setMode(dsp::combine::Phaser::MODE_MANUAL);
        h3.phaser.setWeight(0.0f, 0.0f);
        h3.phaser.setDelay(0.5f);
        h3.run(20, 4096, 20, 4096);
        const double frac = h3.outputPowerDb();
        const double f1 = std::abs(1.0 - std::polar(1.0, -2.0 * M_PI * 0.01 * 0.5));
        const double f2 = std::abs(1.0 - std::polar(1.0, -2.0 * M_PI * 0.03 * 0.5));
        const double fracPredicted = 10.0 * std::log10(0.1 * 0.1 * f1 * f1 + 1.0 * f2 * f2);
        printf("        delay 0.5: residual %.2f dB, predicted %.2f dB\n", frac, fracPredicted);
        check(std::abs(frac - fracPredicted) < 1.0, "half a sample of delay lands where it should");
    }

    // -----------------------------------------------------------------
    // Moving the delay control must not splice the signal. A step change in delay is a
    // step change in time, which breaks up audibly while a control is being dragged.
    printf("\nSweeping the delay does not splice the output\n");
    {
        dsp::stream<dsp::complex_t> a, b;
        dsp::combine::Phaser ph;
        ph.init(&a, &b);
        ph.setMode(dsp::combine::Phaser::MODE_MANUAL);
        ph.setWeight(0.0f, 0.0f);
        ph.setDelay(0.0f);
        ph.reset();
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

        const int N = 5000, BLOCKS = 40;
        long n = 0;
        for (int k = 0; k < BLOCKS; k++) {
            for (int i = 0; i < N; i++, n++) {
                const double p = 2.0 * M_PI * 0.01 * (double)n;
                dsp::complex_t s = { (float)std::cos(p), (float)std::sin(p) };
                a.writeBuf[i] = s;
                b.writeBuf[i] = s;
            }
            // Sweep across whole-sample boundaries, which is where the sign error showed.
            if (k >= 10 && k < 30) { ph.setDelay((float)(k - 10) * 0.1f); }
            a.swap(N);
            b.swap(N);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ph.stop();
        ph.out.stopReader();
        rd.join();
        ph.out.clearReadStop();

        double worst = 0.0, typical = 0.0;
        for (size_t i = N + 1; i < got.size(); i++) {
            const double d = std::hypot(got[i].re - got[i - 1].re, got[i].im - got[i - 1].im);
            typical += d;
            if (d > worst) { worst = d; }
        }
        typical /= (double)(got.size() - N - 1);
        printf("        worst step %.6f, typical %.6f, ratio %.1f\n", worst, typical, worst / (typical + 1e-12));
        check(worst < typical * 10.0, "no splice when the delay is swept across whole samples");
    }

    // -----------------------------------------------------------------
    // The whole reason for a synthetic source: the correct answer is known, so
    // convergence can be scored rather than eyeballed. On the air you can null an
    // interferer but never learn what the right weight was.
    printf("\nAuto-null converges on the known weight\n");
    {
        const double gain = -3.0, phase = 137.0;
        Harness h(gain, phase);
        h.phaser.setMode(dsp::combine::Phaser::MODE_AUTO);
        h.phaser.setWeight(-1000.0f, 0.0f);   // start from nothing
        h.phaser.setAdaptRate(0.25f);
        h.run(40, 4096, 40, 4096);

        float gotGain = 0.0f, gotPhase = 0.0f;
        h.phaser.getWeight(gotGain, gotPhase);
        printf("        converged to %+.3f dB, %+.3f deg (want %+.3f, %+.3f)\n",
               gotGain, gotPhase, -gain, -phase);
        check(std::abs(gotGain - (float)-gain) < 0.5f, "gain converged to within 0.5 dB");
        check(std::abs(gotPhase - (float)-phase) < 2.0f, "phase converged to within 2 degrees");

        // The ceiling here is set by the wanted tone, which survives by design: channel A
        // holds it at 0.1 alongside the interferer at 1.0, so a perfect cancellation of the
        // interferer still leaves 10*log10(1.01/0.01) = 20.04 dB and no more. A depth much
        // above that would mean the wanted signal was being nulled too.
        const float depth = h.phaser.getNullDepth();
        printf("        null depth at the end: %.1f dB (ceiling is 20.0)\n", depth);
        check(depth > 19.0f && depth < 21.0f, "nulls the interferer and stops there");
    }

    printf("\nHold freezes the weight\n");
    {
        const double gain = -3.0, phase = 137.0;
        Harness h(gain, phase);
        h.phaser.setMode(dsp::combine::Phaser::MODE_AUTO);
        h.phaser.setWeight(-1000.0f, 0.0f);
        h.phaser.setAdaptRate(0.25f);
        h.run(30, 4096, 30, 4096);
        float g1 = 0.0f, p1 = 0.0f;
        h.phaser.getWeight(g1, p1);

        // Freeze, then feed channels whose correlation is entirely different. A still
        // adapting weight would chase it; a frozen one must not move.
        h.phaser.setMode(dsp::combine::Phaser::MODE_HOLD);
        Harness h2(20.0, -50.0);
        h2.phaser.setMode(dsp::combine::Phaser::MODE_HOLD);
        h2.phaser.setWeight(g1, p1);
        h2.run(20, 4096, 20, 4096);
        float g2 = 0.0f, p2 = 0.0f;
        h2.phaser.getWeight(g2, p2);
        printf("        held %+.3f dB %+.3f deg through different material -> %+.3f dB %+.3f deg\n",
               g1, p1, g2, p2);
        check(g1 == g2 && p1 == p2, "hold does not move the weight");
    }

    printf("\nReference band ignores signals outside it\n");
    {
        // Wanted tone at 0.01 cyc/sample sits in A only; the interferer at 0.03 is in
        // both. Point the reference band at the interferer and the solution should be the
        // interferer's weight, undisturbed by the wanted tone's presence.
        const double gain = -3.0, phase = 137.0;
        Harness h(gain, phase);
        h.phaser.setMode(dsp::combine::Phaser::MODE_AUTO);
        h.phaser.setWeight(-1000.0f, 0.0f);
        h.phaser.setAdaptRate(0.25f);
        h.phaser.setSampleRate(1.0);                       // work in cycles/sample
        h.phaser.setReferenceBand(true, 0.03, 0.004);      // centred on the interferer
        h.run(40, 4096, 40, 4096);

        float gotGain = 0.0f, gotPhase = 0.0f;
        h.phaser.getWeight(gotGain, gotPhase);
        printf("        converged to %+.3f dB, %+.3f deg (want %+.3f, %+.3f)\n",
               gotGain, gotPhase, -gain, -phase);
        check(std::abs(gotGain - (float)-gain) < 1.0f, "reference band solved the interferer's gain");
        check(std::abs(gotPhase - (float)-phase) < 5.0f, "reference band solved the interferer's phase");
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
