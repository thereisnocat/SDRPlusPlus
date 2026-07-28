// Exercises the exact worker/stop shape the module uses, against a real dsp::stream and
// a real consumer, to prove start/stream/stop neither deadlocks nor drops samples. This
// covers the SDR++ glue that test_signal_model.cpp deliberately does not touch.
//
// Needs the core library, so build it first (cmake --build build), then:
//
//   c++ -std=c++17 -O2 -I<repo>/core/src -I/opt/homebrew/include \
//       -o /tmp/worker_test test/test_worker.cpp \
//       -L<repo>/build/core -lsdrpp_core -L/opt/homebrew/lib -lvolk -Wl,-rpath,<repo>/build/core
//
#include <dsp/types.h>
#include <dsp/stream.h>
#include "../src/signal_model.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>

using namespace phtest;

int main() {
    dsp::stream<dsp::complex_t> out;
    Generator gen;
    Params p;
    p.sampleRate = 1000000.0;
    p.noiseEnabled = false;
    const int blockSize = (int)(p.sampleRate / 200.0);

    std::atomic<bool> run{true};
    std::atomic<long> produced{0}, consumed{0};
    std::atomic<double> power{0.0};

    gen.reset();
    std::thread producer([&]{
        auto next = std::chrono::steady_clock::now();
        while (run) {
            gen.generate(p, blockSize, (float*)out.writeBuf);
            if (!out.swap(blockSize)) { break; }
            produced += blockSize;
            next += std::chrono::nanoseconds((int64_t)(1e9 * (double)blockSize / p.sampleRate));
            auto now = std::chrono::steady_clock::now();
            if (next < now) { next = now; }
            std::this_thread::sleep_until(next);
        }
    });

    std::thread consumer([&]{
        while (true) {
            int c = out.read();
            if (c < 0) { break; }
            double acc = 0.0;
            for (int i = 0; i < c; i++) {
                acc += (double)out.readBuf[i].re * out.readBuf[i].re + (double)out.readBuf[i].im * out.readBuf[i].im;
            }
            power = 10.0 * std::log10(acc / c + 1e-300);
            consumed += c;
            out.flush();
        }
    });

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Stop exactly as the module does.
    run = false;
    out.stopWriter();
    producer.join();
    out.stopReader();
    consumer.join();
    out.clearWriteStop();
    out.clearReadStop();

    double rate = produced / elapsed;
    printf("\nworker/stream lifecycle\n\n");
    printf("  produced %ld samples in %.2fs -> %.0f sps (nominal %.0f)\n", produced.load(), elapsed, rate, p.sampleRate);
    printf("  consumed %ld samples\n", consumed.load());
    printf("  measured power of delivered stream: %.2f dB\n", power.load());

    int bad = 0;
    bool paced = std::abs(rate - p.sampleRate) / p.sampleRate < 0.10;
    printf("  %-52s %s\n", "producer paced to within 10%% of real time", paced ? "ok" : "FAIL");
    if (!paced) bad = 1;
    bool joined = true;  // reaching here at all means both joins returned
    printf("  %-52s %s\n", "start/stop completes without deadlock", joined ? "ok" : "FAIL");
    bool delivered = consumed > 0 && consumed <= produced;
    printf("  %-52s %s\n", "consumer received a sane sample count", delivered ? "ok" : "FAIL");
    if (!delivered) bad = 1;
    // -20 dBFS wanted + -10 dBFS interferer summed in channel A.
    double expected = 10.0 * std::log10(std::pow(10.0, -20.0/10.0) + std::pow(10.0, -10.0/10.0));
    bool lvl = std::abs(power - expected) < 0.1;
    printf("  %-52s %s (%.2f vs %.2f dB)\n", "delivered power matches the model", lvl ? "ok" : "FAIL", power.load(), expected);
    if (!lvl) bad = 1;

    printf("\n%s\n\n", bad ? "FAILED" : "PASSED");
    return bad;
}
