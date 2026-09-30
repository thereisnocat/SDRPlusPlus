// Hardware experiment: does the RSR200 actually follow an ADC clock change across a
// stop/close/reopen/start cycle in one process (the sequence the recording scheduler drives)?
// Measures the real delivered sample rate for each clock and compares with clock/64.
#include "../src/transport_usb.h"
#include "../src/rsr200_device.h"
#include <cstdio>
#include <chrono>
#include <thread>
#include <windows.h>
using namespace rsr200;

static uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    double clocks[8]; int n = 0;
    for (int i = 1; i < argc && n < 8; i++) clocks[n++] = atof(argv[i]);
    if (n == 0) { clocks[0] = 98.4; clocks[1] = 104.9; clocks[2] = 94.2; clocks[3] = 104.9; n = 4; }

    UsbTransport usb;
    Device dev;
    for (int i = 0; i < n; i++) {
        std::string err;
        if (!usb.open(0, err)) { printf("open failed: %s\n", err.c_str()); return 1; }
        dev.setTransport(&usb);
        dev.onReply = [](const Reply& r) {
            if (r.kind == REPLY_CONFIRMATION) printf("  reply plain confirmation confirms=%u\n", r.confirmedCommand);
            if (r.kind == REPLY_SPECIAL)
                printf("  reply special instr=0x%02X data=%02X %02X %02X confirms=%u (clock %.1f MHz gpsBit=%d)\n",
                       r.echoedInstruction, r.data[0], r.data[1], r.data[2], r.confirmedCommand,
                       (r.data[0] | ((r.data[1] & 0x7F) << 8)) / 10.0, (r.data[1] >> 7) & 1);
        };
        Config c;
        c.adcClockHz = clocks[i] * 1e6;
        c.decimationExp = 5;
        c.format.channels = 2;
        c.format.bits = 24;
        c.opMode = OP_INDEPENDENT;
        c.switchRegister = SW_ADC2_TO_HF2;
        c.tunedHz = 1360000; if (getenv("GPS") && getenv("GPS")[0]=='0') c.gpsDiscipline = false;
        uint64_t now = nowMs();
        if (getenv("SACRIFICE")) {
            auto vc = cmdReadVersion(7000 + i, false);
            usb.sendCommand(vc.data(), vc.size());
            printf("  [pre] sent read-version alone\n");
        }
        if (getenv("DELAY")) { std::this_thread::sleep_for(std::chrono::milliseconds(atoi(getenv("DELAY")))); }
        if (getenv("PRECLOCK")) {
            // Send the clock command by itself and read the stream for a while before anything else.
            auto cc = cmdSetAdcClock(5000 + i, false, c.adcClockHz, c.gpsDiscipline);
            usb.sendCommand(cc.data(), cc.size());
            printf("  [pre] sent clock command %d alone\n", 5000 + i);
            uint64_t tp = nowMs();
            while (nowMs() - tp < atoi(getenv("PRECLOCK"))) { if (!dev.pump()) break; }
        }
        if (!dev.applyConfig(c, now)) { printf("applyConfig failed\n"); return 1; }
        dev.setHardwareDiversity(1.0, 0.0, now);
        if (!dev.startStream(now)) { printf("startStream failed\n"); return 1; }

        uint64_t frames = 0; uint64_t gaps = 0;
        dev.onSamples = [&](const SampleBlock& b) { frames += b.frames; if (b.sequenceGap) gaps++; };
        // skip the first second (settling), then measure 4 s
        uint64_t t0 = nowMs(), tStart = 0; uint64_t f0 = 0;
        bool started = false;
        while (nowMs() - t0 < 5000) {
            if (!dev.pump()) break;
            dev.service(nowMs());
            if (!started && nowMs() - t0 >= 1000) { started = true; tStart = nowMs(); f0 = frames; }
        }
        double sec = (nowMs() - tStart) / 1000.0;
        double rate = (frames - f0) / sec;
        printf("requested clock %.1f MHz (expect %.0f Sa/s)  measured %.0f Sa/s  => implied clock %.2f MHz  gaps=%llu\n",
               clocks[i], clocks[i] * 1e6 / 64, rate, rate * 64 / 1e6, (unsigned long long)gaps);
        fflush(stdout);

        usb.abortReads();
        auto stopCmd = cmdStopStream(9998, false, usb.streamPort());
        usb.sendCommand(stopCmd.data(), stopCmd.size());
        usb.close();
        dev.setTransport(nullptr);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}

