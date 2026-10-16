// Checks for SCA (subsidiary subcarrier) reception in dsp::demod::BroadcastFM and the multiplex analyzer.
//
// Build core first, then (as run_tests.sh does):
//   c++ -std=c++17 -O2 -I<repo>/core/src -I/opt/homebrew/include -o /tmp/test_sca core/test/test_sca.cpp \
//       -L<repo>/build/core -lsdrpp_core -L/opt/homebrew/lib -lvolk -lfftw3f -Wl,-rpath,<repo>/build/core
#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/demod/broadcast_fm.h>
#include <vector>
#include <cstdio>
#include <cmath>
#include <string>

static int failures = 0;
static void check(bool cond, const std::string& what) {
    printf("  %-70s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

// Amplitude of a tone in a real signal (single-bin DFT).
static double toneAmp(const std::vector<float>& x, double fs, double hz, size_t from, size_t count) {
    double re = 0, im = 0;
    for (size_t i = from; i < from + count && i < x.size(); i++) {
        const double ph = 2.0 * M_PI * hz * (double)i / fs;
        re += x[i] * cos(ph);
        im -= x[i] * sin(ph);
    }
    return 2.0 * sqrt(re * re + im * im) / (double)count;
}

int main() {
    const double fs = 250000.0, pi2 = 2.0 * M_PI;
    const size_t n = (size_t)(3.0 * fs);

    // A station: programme = 400 Hz tone (0.45 of the deviation), stereo pilot, and an SCA at 67 kHz, narrow-band FM (+/-5 kHz, 1 kHz tone), injection 0.1
    std::vector<dsp::complex_t> iq(n);
    double phase = 0, sub = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / fs;
        sub += pi2 * (67000.0 + 5000.0 * cos(pi2 * 1000.0 * t)) / fs;
        const double comp = 0.45 * sin(pi2 * 400.0 * t) + 0.09 * sin(pi2 * 19000.0 * t) + 0.1 * cos(sub);
        phase += pi2 * 75000.0 * comp / fs;
        iq[i] = { (float)(0.5 * cos(phase)), (float)(0.5 * sin(phase)) };
    }

    printf("SCA through the broadcast FM demodulator\n");
    dsp::demod::BroadcastFM demod;
    demod.init(NULL, 75000.0, fs, true, true, false);
    std::vector<dsp::stereo_t> out(n);
    int rdsCount = 0;
    const size_t blk = 8192;
    demod.setSCA(67000.0);
    for (size_t off = 0; off < n; off += blk) {
        const size_t c = std::min(blk, n - off);
        demod.process((int)c, iq.data() + off, out.data() + off, rdsCount);
    }
    std::vector<float> l(n), r(n);
    for (size_t i = 0; i < n; i++) { l[i] = out[i].l; r[i] = out[i].r; }
    const size_t from = (size_t)(1.0 * fs), len = (size_t)(1.0 * fs);
    const double a1k = toneAmp(l, fs, 1000.0, from, len), a400 = toneAmp(l, fs, 400.0, from, len);
    check(fabs(a1k - 0.5) < 0.06, "the subcarrier's 1 kHz tone comes out at 0.5 (5 kHz deviation, default gain): " + std::to_string(a1k));
    check(a400 < 0.02, "none of the main programme's 400 Hz tone: " + std::to_string(a400));
    check(fabs(toneAmp(l, fs, 1000.0, from, len) - toneAmp(r, fs, 1000.0, from, len)) < 1e-4, "both channels carry it");

    // Off: the programme again
    demod.setSCA(0.0);
    demod.reset();
    for (size_t off = 0; off < n; off += blk) {
        const size_t c = std::min(blk, n - off);
        demod.process((int)c, iq.data() + off, out.data() + off, rdsCount);
    }
    for (size_t i = 0; i < n; i++) { l[i] = out[i].l; }
    check(toneAmp(l, fs, 400.0, from, len) > 0.3, "SCA off: the 400 Hz programme is back: " + std::to_string(toneAmp(l, fs, 400.0, from, len)));

    // The subcarrier at 92 kHz is found by retuning the receiver
    printf("another subcarrier frequency\n");
    sub = 0; phase = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / fs;
        sub += pi2 * (92000.0 + 5000.0 * cos(pi2 * 2000.0 * t)) / fs;
        phase += pi2 * 75000.0 * (0.45 * sin(pi2 * 400.0 * t) + 0.1 * cos(sub)) / fs;
        iq[i] = { (float)(0.5 * cos(phase)), (float)(0.5 * sin(phase)) };
    }
    demod.setSCA(92000.0);
    demod.reset();
    for (size_t off = 0; off < n; off += blk) {
        const size_t c = std::min(blk, n - off);
        demod.process((int)c, iq.data() + off, out.data() + off, rdsCount);
    }
    for (size_t i = 0; i < n; i++) { l[i] = out[i].l; }
    check(toneAmp(l, fs, 2000.0, from, len) > 0.4, "92 kHz subcarrier: its 2 kHz tone: " + std::to_string(toneAmp(l, fs, 2000.0, from, len)));
    demod.setSCA(67000.0);   // nothing there
    demod.reset();
    for (size_t off = 0; off < n; off += blk) {
        const size_t c = std::min(blk, n - off);
        demod.process((int)c, iq.data() + off, out.data() + off, rdsCount);
    }
    for (size_t i = 0; i < n; i++) { l[i] = out[i].l; }
    check(toneAmp(l, fs, 2000.0, from, len) < 0.05, "tuned to 67 kHz where there is none: no 2 kHz tone: " + std::to_string(toneAmp(l, fs, 2000.0, from, len)));

    printf("the multiplex spectrum\n");
    dsp::demod::MultiplexSpectrum mux;
    mux.init(fs);
    mux.setEnabled(true);
    std::vector<float> comp(n);
    sub = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / fs;
        sub += pi2 * (67000.0 + 5000.0 * cos(pi2 * 1000.0 * t)) / fs;
        comp[i] = (float)(0.09 * sin(pi2 * 19000.0 * t) + 0.1 * cos(sub));
    }
    mux.feed((int)n, comp.data());
    std::vector<float> db;
    check(mux.get(db) && db.size() == (size_t)dsp::demod::MultiplexSpectrum::POINTS, "a spectrum of 500 points from 0 to 100 kHz");
    auto at = [&](double khz) { return db[(size_t)(khz / 100.0 * (double)db.size())]; };
    check(at(19.0) > at(30.0) + 20.0, "the 19 kHz pilot stands out");
    check(at(67.0) > at(80.0) + 15.0, "the 67 kHz subcarrier stands out");

    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
