// Standalone smoke test against a real, physically-attached RSR200 over USB.
//
// Not part of the CMake project and not a unit test -- it needs actual hardware. Compiled
// and run by hand while bringing up the USB transport (see RSR200_PLAN.md phase 6). Proves,
// in order: the D3XX driver enumerates the radio, UsbTransport can open it and keep reads
// queued, a command write reaches the radio, and the radio streams real packets back with a
// version reply embedded in one of them.
//
// DP 3.3: Stop Stream closes the USB send endpoint entirely, so after this program's clean
// exit the radio may need a moment (or a replug) before a second run's FT_Create succeeds --
// see ENGINEERING_NOTES.md 2.1 on the Fobos open/close finding for why that is worth
// remembering rather than re-discovering.
//
// The reader runs on its own thread doing nothing but nextFrame() and counter bookkeeping --
// no parsing, no printf -- at THREAD_PRIORITY_TIME_CRITICAL, isolated from everything else in
// the process. This is a second experiment, not the first: at QUEUE_DEPTH 8 there was real
// steady-state packet loss (~1%); bumping to 64 roughly halved it, but 256 bought almost
// nothing more, and throughput measured a fraction of the FT601's SuperSpeed ceiling -- so the
// buffer count was not the limit. This isolates whether serialized per-packet processing on
// one thread was.

#include "../src/transport_usb.h"
#include "../src/rsr200_protocol.h"
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>
#include <windows.h>

using namespace rsr200;

static uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct ReaderStats {
    std::atomic<uint32_t> packets{ 0 };
    std::atomic<uint32_t> gapEvents{ 0 };
    std::atomic<uint32_t> totalLoss{ 0 };
    std::atomic<uint32_t> shortPackets{ 0 };
    std::atomic<bool> sawVersion{ false };
    std::atomic<uint32_t> verSerial{ 0 };
    std::atomic<uint32_t> verFirmware{ 0 };
    std::atomic<bool> stopped{ false };   // set when nextFrame() itself gives up
};

// The hot path. Nothing here but the read, the counter arithmetic needed to detect a gap,
// and the one-time version-reply check -- everything else is left for main() to read out of
// the atomics after the fact, off this thread.
static void readerThread(UsbTransport* usb, ReaderStats* stats, std::atomic<bool>* stop) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::vector<uint8_t> frame;
    uint32_t prevCounter = 0;
    bool first = true;

    while (!stop->load(std::memory_order_relaxed)) {
        if (!usb->nextFrame(frame)) {
            stats->stopped.store(true, std::memory_order_relaxed);
            return;
        }
        if (frame.size() != USB_PACKET_BYTES) {
            stats->shortPackets.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const uint32_t counter = readU32(frame.data());
        if (!first) {
            const uint32_t step = counter - prevCounter;
            if (step != 1) {
                stats->gapEvents.fetch_add(1, std::memory_order_relaxed);
                stats->totalLoss.fetch_add(step - 1, std::memory_order_relaxed);
            }
        }
        first = false;
        prevCounter = counter;
        stats->packets.fetch_add(1, std::memory_order_relaxed);

        if (!stats->sawVersion.load(std::memory_order_relaxed)) {
            const uint8_t cmdNo = frame[USB_CMD_NO_OFFSET];
            if (cmdNo != 0) {
                const Reply r = parseEmbeddedCommand(frame.data() + USB_COMMAND_OFFSET);
                if (r.kind == REPLY_VERSION) {
                    stats->verSerial.store(r.serial, std::memory_order_relaxed);
                    stats->verFirmware.store(r.firmware, std::memory_order_relaxed);
                    stats->sawVersion.store(true, std::memory_order_relaxed);
                }
            }
        }
    }
}

int main() {
    auto devices = UsbTransport::listDevices();
    printf("D3XX devices found: %zu\n", devices.size());
    for (auto& d : devices) { printf("  %s\n", d.c_str()); }
    if (devices.empty()) {
        printf("No D3XX device enumerated -- is the radio connected and powered?\n");
        return 1;
    }

    printf("Link speed: %s\n", UsbTransport::isSuperSpeed(0) ? "SuperSpeed (USB 3.0)" : "Hi-Speed or slower (USB 2.0 fallback)");

    UsbTransport usb;
    std::string err;
    if (!usb.open(0, err)) {
        printf("open() failed: %s\n", err.c_str());
        return 1;
    }
    printf("Opened device 0, queued reads outstanding.\n");

    auto startCmd = cmdStartStream(1, /*lan=*/false, PORT_USB, /*sizeCode=*/7);
    if (!usb.sendCommand(startCmd.data(), startCmd.size())) {
        printf("Start Stream write failed\n");
        return 1;
    }
    printf("Start Stream sent.\n");

    auto verCmd = cmdReadVersion(2, /*lan=*/false);
    if (!usb.sendCommand(verCmd.data(), verCmd.size())) {
        printf("Read Version write failed\n");
        return 1;
    }
    printf("Read Version sent.\n");

    ReaderStats stats;
    std::atomic<bool> stop{ false };
    std::thread reader(readerThread, &usb, &stats, &stop);

    const uint64_t t0 = nowMs();
    uint32_t lastPrinted = 0;
    while (nowMs() - t0 < 5000 && stats.packets.load(std::memory_order_relaxed) < 20000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const uint32_t p = stats.packets.load(std::memory_order_relaxed);
        if (p != lastPrinted) {
            printf("packet %u: gapEvents=%u totalLoss=%u shortPackets=%u\n",
                   p, stats.gapEvents.load(), stats.totalLoss.load(), stats.shortPackets.load());
            lastPrinted = p;
        }
        if (stats.stopped.load(std::memory_order_relaxed)) { break; }
    }
    const uint64_t elapsedMs = nowMs() - t0;

    // Interrupt the reader thread's blocked nextFrame() first, and wait for it to actually
    // stop touching the buffer/overlapped pool, before close() is allowed to free any of it
    // -- releasing an OVERLAPPED out from under a thread still waiting on it is a race.
    stop.store(true, std::memory_order_relaxed);
    usb.abortReads();
    reader.join();

    auto stopCmd = cmdStopStream(3, /*lan=*/false, PORT_USB);
    usb.sendCommand(stopCmd.data(), stopCmd.size());
    usb.close();

    const uint32_t packets = stats.packets.load();
    const uint32_t gapEvents = stats.gapEvents.load();
    const uint32_t totalLoss = stats.totalLoss.load();
    const double mbPerSec = elapsedMs > 0
        ? ((double)packets * (double)USB_PACKET_BYTES) / 1e6 / ((double)elapsedMs / 1000.0)
        : 0.0;
    const double packetsPerSec = elapsedMs > 0 ? (double)packets * 1000.0 / (double)elapsedMs : 0.0;

    printf("\nVersion reply: %s", stats.sawVersion.load() ? "yes" : "no");
    if (stats.sawVersion.load()) {
        printf(" (serial=%u firmware=%u)", stats.verSerial.load(), stats.verFirmware.load());
    }
    printf("\n%u packets in %llu ms (%.0f pkt/s, %.1f MB/s). Gap events: %u. Total counter loss: %u (%.2f%% of produced packets).\n",
           packets, (unsigned long long)elapsedMs, packetsPerSec, mbPerSec,
           gapEvents, totalLoss, packets > 0 ? 100.0 * totalLoss / (packets + totalLoss) : 0.0);

    return (stats.sawVersion.load() && packets > 0) ? 0 : 2;
}
