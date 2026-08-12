// Standalone smoke test against a real RSR200 over its LAN interface.
//
// Not part of the CMake project and not a unit test -- it needs actual hardware reachable on
// the network. Compiled and run by hand, the same way test_usb_live.cpp is. Proves, in
// order: LanTcpTransport can connect, Device::applyConfig()/startStream() get accepted, and
// the radio streams real samples back with a version reply embedded in one of them -- the
// LAN-side equivalent of test_usb_live.cpp, but through the Device layer instead of raw
// frame counting, since that's the exact code path the real module (main.cpp) uses.
//
// Usage:
//   c++ -std=c++17 -O2 -pthread -o /tmp/test_lan_live test_lan_live.cpp && \
//     /tmp/test_lan_live 192.168.1.176
//
// (host defaults to 192.168.1.176 if omitted; port defaults to 55557)

#include "../src/rsr200_lan_transport.h"
#include "../src/rsr200_device.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>

using namespace rsr200;

static uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    std::string host = argc > 1 ? argv[1] : "192.168.1.176";
    uint16_t port = argc > 2 ? (uint16_t)atoi(argv[2]) : 55557;

    printf("Connecting to RSR200 at %s:%u ...\n", host.c_str(), port);

    LanTcpTransport lan;
    if (!lan.connect(host, port)) {
        printf("connect() failed: %s\n", lan.lastError().c_str());
        return 1;
    }
    printf("TCP connected.\n");

    Device device;
    device.setTransport(&lan);

    std::atomic<uint32_t> sampleBlocks{ 0 };
    std::atomic<uint64_t> totalFrames{ 0 };
    std::atomic<uint32_t> gapEvents{ 0 };
    std::atomic<bool> sawVersion{ false };
    std::atomic<uint32_t> verSerial{ 0 };
    std::atomic<uint32_t> verFirmware{ 0 };
    std::atomic<bool> haveFirstSample{ false };
    float firstRe = 0, firstIm = 0;
    std::mutex errMtx;
    std::string lastErr;

    device.onError = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lck(errMtx);
        lastErr = msg;
        printf("[error] %s\n", msg.c_str());
    };
    device.onReply = [&](const Reply& r) {
        if (r.kind != REPLY_VERSION) { return; }
        verSerial = r.serial;
        verFirmware = r.firmware;
        sawVersion = true;
    };
    static uint32_t lastSeq = 0;
    static bool haveLastSeq = false;
    device.onSamples = [&](const SampleBlock& b) {
        sampleBlocks++;
        totalFrames += (uint64_t)b.frames;
        if (b.sequenceGap) { gapEvents++; }
        if (!haveFirstSample.load() && b.frames > 0 && b.chA) {
            firstRe = b.chA[0];
            firstIm = b.chA[1];
            haveFirstSample = true;
        }
        // Diagnostic: raw counter delta, not just the gap flag, for the first several
        // blocks -- to see whether it's off by a small constant (framing/offset bug) or
        // wildly inconsistent (real loss) or something else entirely.
        if (sampleBlocks.load() <= 15) {
            int64_t delta = haveLastSeq ? (int64_t)b.sequence - (int64_t)lastSeq : 0;
            printf("    [seq] block #%u: sequence=%u delta=%lld gap_flag=%s\n",
                   sampleBlocks.load(), b.sequence, (long long)delta, b.sequenceGap ? "yes" : "no");
            lastSeq = b.sequence;
            haveLastSeq = true;
        }
    };

    // Config{}'s own default member initializers already match what this needs: 125 MHz
    // clock, decimation exp 3 (rate 16), single channel 16-bit, tuned to 10 MHz -- a plain
    // connectivity/proof-of-life check, not aiming at any particular signal.
    Config cfg;

    const uint64_t start = nowMs();
    if (!device.applyConfig(cfg, start)) {
        printf("applyConfig() failed\n");
        return 1;
    }
    printf("applyConfig() sent (%.3f MSp/s).\n", cfg.sampleRateHz() / 1e6);

    // Ad-hoc, not modelled on Device (which has no "send an unsolicited command" method) --
    // same pattern main.cpp itself uses. The reply arrives embedded in the stream like any
    // other; Device::onReply above already watches for REPLY_VERSION.
    auto verCmd = cmdReadVersion(9999, /*lan=*/true);
    lan.sendCommand(verCmd.data(), verCmd.size());
    printf("Version query sent.\n");

    if (!device.startStream(start)) {
        printf("startStream() failed\n");
        return 1;
    }
    printf("Start Stream sent. Pumping for 5 seconds...\n");

    std::atomic<bool> stop{ false };
    std::thread pumpThread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (!device.pump()) { break; }
            device.service(nowMs());
        }
    });

    const uint64_t t0 = nowMs();
    while (nowMs() - t0 < 5000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        printf("  blocks=%u frames=%llu gapEvents=%u version=%s\n",
               sampleBlocks.load(), (unsigned long long)totalFrames.load(), gapEvents.load(),
               sawVersion.load() ? "yes" : "no");
    }

    // Interrupt the pump thread's blocked nextFrame() (LanTcpTransport::stop() closes the
    // socket, the only portable way to unstick a blocking recv()) before touching anything
    // it might still be using, then join -- same ordering main.cpp's stop() uses.
    stop.store(true, std::memory_order_relaxed);
    lan.stop();
    if (pumpThread.joinable()) { pumpThread.join(); }

    // Best-effort: the socket is already closed by lan.stop() above, so this will most
    // likely fail to actually send -- matches main.cpp's own comment on the same situation.
    auto stopCmd = cmdStopStream(9998, /*lan=*/true, lan.streamPort());
    lan.sendCommand(stopCmd.data(), stopCmd.size());
    lan.close();

    printf("\nVersion reply: %s", sawVersion.load() ? "yes" : "no");
    if (sawVersion.load()) {
        printf(" (serial=%u firmware=%u)", verSerial.load(), verFirmware.load());
    }
    printf("\nSample blocks: %u, total frames: %llu, gap events: %u\n",
           sampleBlocks.load(), (unsigned long long)totalFrames.load(), gapEvents.load());
    if (haveFirstSample.load()) {
        printf("First sample: I=%.4f Q=%.4f\n", firstRe, firstIm);
    }
    {
        std::lock_guard<std::mutex> lck(errMtx);
        if (!lastErr.empty()) { printf("Last reported error: %s\n", lastErr.c_str()); }
    }

    return (sawVersion.load() && sampleBlocks.load() > 0) ? 0 : 2;
}
