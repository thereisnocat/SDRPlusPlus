// Checks for the Phasing front end: the splitter-per-channel graph that sits between a
// multi-channel source and the IQ front end.
//
// Needs the core library, so build that first (cmake --build build), then:
//
//   c++ -std=c++17 -O2 -I<repo>/core/src -I/opt/homebrew/include \
//       -o /tmp/test_phasing core/test/test_phasing.cpp \
//       -L<repo>/build -lsdrpp_core -L/opt/homebrew/lib -lvolk -Wl,-rpath,<repo>/build
//
// The properties worth pinning down here are the ones that only emerge once the graph is
// assembled: that a recording tap does not steal samples from the combiner, and that a
// channel nobody is combining or tapping still gets drained rather than stalling the
// source. See PHASING_PLAN.md sections 2.1 and 4.1.

#include <dsp/types.h>
#include <dsp/stream.h>
#include <signal_path/phasing.h>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-58s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

// Writes `blocks` blocks of `size` samples of a constant value into a stream.
struct Producer {
    dsp::stream<dsp::complex_t> stream;
    std::thread th;
    std::atomic<int> written{ 0 };

    void start(int blocks, int size, float value) {
        th = std::thread([this, blocks, size, value] {
            for (int k = 0; k < blocks; k++) {
                for (int i = 0; i < size; i++) { stream.writeBuf[i] = { value, 0.0f }; }
                if (!stream.swap(size)) { return; }
                written += size;
            }
        });
    }

    void finish() {
        if (th.joinable()) { th.join(); }
    }

    void abandon() {
        stream.stopWriter();
        if (th.joinable()) { th.join(); }
        stream.clearWriteStop();
    }
};

// Drains a stream, counting samples and remembering the last value seen.
struct Collector {
    std::thread th;
    std::atomic<int> count{ 0 };
    std::atomic<float> last{ 0.0f };

    void start(dsp::stream<dsp::complex_t>* s) {
        th = std::thread([this, s] {
            while (true) {
                int c = s->read();
                if (c < 0) { break; }
                last = s->readBuf[c - 1].re;
                count += c;
                s->flush();
            }
        });
    }

    void stop(dsp::stream<dsp::complex_t>* s) {
        s->stopReader();
        if (th.joinable()) { th.join(); }
        s->clearReadStop();
    }
};

int main() {
    printf("\nPhasing front end\n\n");

    // -----------------------------------------------------------------
    printf("Attach, combine, detach\n");
    {
        Phasing ph;
        ph.init();
        check(!ph.isActive(), "inactive before any channel set is attached");

        Producer pa, pb;
        ChannelSet set;
        set.count = 2;
        set.streams = { &pa.stream, &pb.stream };
        set.names = { "A", "B" };
        set.phaseCoherent = true;
        set.sampleAligned = true;

        ph.setChannelSet(&set);
        check(ph.isActive(), "active once a two-channel set is attached");
        check(ph.getChannelCount() == 2, "reports the channel count");
        check(ph.getChannelName(0) == "A" && ph.getChannelName(1) == "B", "reports channel names");
        check(ph.isPhaseCoherent(), "reports coherence");

        Collector out;
        out.start(ph.getOutput());

        // Default mode is A_ONLY: the output should be channel A's value, 1.0.
        pa.start(10, 4096, 1.0f);
        pb.start(10, 4096, 7.0f);
        pa.finish();
        pb.finish();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        printf("        %d samples out, last value %.2f\n", out.count.load(), out.last.load());
        check(out.count > 0, "samples reach the output");
        check(std::abs(out.last - 1.0f) < 1e-6f, "bypass default passes channel A");

        out.stop(ph.getOutput());
        ph.setChannelSet(NULL);
        check(!ph.isActive(), "inactive after detaching");
    }

    // -----------------------------------------------------------------
    printf("\nA recording tap does not steal from the combiner\n");
    {
        Phasing ph;
        ph.init();

        Producer pa, pb;
        ChannelSet set;
        set.count = 2;
        set.streams = { &pa.stream, &pb.stream };
        set.names = { "A", "B" };

        ph.setChannelSet(&set);

        // Tap channel 1 the way the recorder will in a later phase.
        dsp::stream<dsp::complex_t> tap;
        ph.bindChannelStream(1, &tap);

        Collector out, tapped;
        out.start(ph.getOutput());
        tapped.start(&tap);

        const int total = 10 * 4096;
        pa.start(10, 4096, 1.0f);
        pb.start(10, 4096, 7.0f);
        pa.finish();
        pb.finish();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        printf("        combiner %d, tap %d (of %d produced)\n", out.count.load(), tapped.count.load(), total);
        check(out.count == total, "combiner received every sample");
        check(tapped.count == total, "tap received every sample");
        check(std::abs(tapped.last - 7.0f) < 1e-6f, "tap carries the raw channel, not the combination");
        check(ph.getDiscardCount() == 0, "nothing discarded");

        tapped.stop(&tap);
        out.stop(ph.getOutput());
        ph.unbindChannelStream(1, &tap);
        ph.setChannelSet(NULL);
    }

    // -----------------------------------------------------------------
    // A source writes every channel it registered. If a channel that is neither combined
    // nor tapped had no reader, its producer would block on the first swap and take the
    // whole source down with it.
    printf("\nAn uncombined, untapped channel is still drained\n");
    {
        Phasing ph;
        ph.init();

        Producer p0, p1, p2;
        ChannelSet set;
        set.count = 3;
        set.streams = { &p0.stream, &p1.stream, &p2.stream };
        set.names = { "A", "B", "C" };

        ph.setChannelSet(&set);

        Collector out;
        out.start(ph.getOutput());

        const int total = 8 * 4096;
        p0.start(8, 4096, 1.0f);
        p1.start(8, 4096, 2.0f);
        p2.start(8, 4096, 3.0f);   // combined with nothing, tapped by nobody
        p0.finish();
        p1.finish();
        p2.finish();

        printf("        channel 2 wrote %d of %d samples\n", p2.written.load(), total);
        check(p2.written == total, "the spare channel's producer ran to completion");

        out.stop(ph.getOutput());
        ph.setChannelSet(NULL);
    }

    // -----------------------------------------------------------------
    printf("\nSelecting a different channel pair\n");
    {
        Phasing ph;
        ph.init();

        Producer pa, pb, pc;
        ChannelSet set;
        set.count = 3;
        set.streams = { &pa.stream, &pb.stream, &pc.stream };
        set.names = { "A", "B", "C" };

        ph.setChannelSet(&set);
        int a = -1, b = -1;
        ph.getChannelPair(a, b);
        check(a == 0 && b == 1, "defaults to combining channels 0 and 1");

        // Combine 2 and 0 instead; in bypass that means the output becomes channel 2.
        ph.setChannelPair(2, 0);
        ph.getChannelPair(a, b);
        check(a == 2 && b == 0, "channel pair updated");

        Collector out;
        out.start(ph.getOutput());

        pa.start(8, 4096, 1.0f);
        pb.start(8, 4096, 2.0f);
        pc.start(8, 4096, 3.0f);
        pa.finish();
        pb.finish();
        pc.finish();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        printf("        output last value %.2f (channel 2 carries 3.0)\n", out.last.load());
        check(std::abs(out.last - 3.0f) < 1e-6f, "bypass now passes the newly selected channel");

        out.stop(ph.getOutput());
        ph.setChannelSet(NULL);
    }

    // -----------------------------------------------------------------
    printf("\nManual weight nulls through the assembled graph\n");
    {
        Phasing ph;
        ph.init();

        Producer pa, pb;
        ChannelSet set;
        set.count = 2;
        set.streams = { &pa.stream, &pb.stream };
        set.names = { "A", "B" };

        // Both channels carry the same constant, so w = 1 (0 dB, 0 deg) must cancel it.
        ph.setMode(dsp::combine::Phaser::MODE_MANUAL);
        ph.setWeight(0.0f, 0.0f);
        ph.setChannelSet(&set);

        Collector out;
        out.start(ph.getOutput());

        pa.start(10, 4096, 5.0f);
        pb.start(10, 4096, 5.0f);
        pa.finish();
        pb.finish();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        printf("        output last value %.6f, null depth %.1f dB\n", out.last.load(), ph.getNullDepth());
        check(std::abs(out.last) < 1e-5f, "identical channels cancel to zero");
        check(ph.getNullDepth() > 100.0f, "a perfect null reports a large depth, not zero");
        check(ph.getDiscardCount() == 0, "nothing discarded");

        out.stop(ph.getOutput());
        ph.setChannelSet(NULL);
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
