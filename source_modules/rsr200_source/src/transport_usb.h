#pragma once
#include "rsr200_device.h"
#include <FTD3XX.h>
#include <string>
#include <vector>

// USB transport over FTDI's D3XX driver. See RSR200_PLAN.md section 3.1 and 8.
//
// The radio is an FT601 SuperSpeed FIFO bridge: fixed 4096-byte packets from bulk endpoint
// 0x82, commands written to 0x02. DP 2.1 warns the FT601 has only a double 4096-byte
// buffer, so the PC must keep reads queued or data is lost -- this is FTDI's own documented
// pattern for that (WU_DataStreamerApp/APP_ReaderThread.cpp in the D3XX SDK): a stream pipe
// set once, a ring of overlapped reads kept perpetually in flight, and each buffer
// re-submitted the instant its read completes.

namespace rsr200 {

    class UsbTransport : public Transport {
    public:
        ~UsbTransport() override;

        Kind kind() const override { return KIND_USB; }

        // One line per connected D3XX device, "description (serial)", for a source menu.
        static std::vector<std::string> listDevices();

        // Whether device `index` negotiated USB 3.0 SuperSpeed vs. falling back to 2.0
        // Hi-Speed -- straight from FT_DEVICE_LIST_INFO_NODE.Flags (FT_FLAGS_SUPERSPEED /
        // FT_FLAGS_HISPEED), no guessing from throughput numbers.
        static bool isSuperSpeed(int index);

        // Opens the device at `index` in the D3XX enumeration order (0 = first). Starts the
        // queued read pipe immediately, matching DP 2.1's recommendation to keep reads
        // outstanding at all times. `err` is set on failure.
        bool open(int index, std::string& err);

        // Opens by serial number instead of enumeration index, for when more than one FT60x
        // device is attached and the radio's has been noted.
        bool openBySerial(const std::string& serial, std::string& err);

        void close();
        bool isOpen() const { return handle != nullptr; }

        // Interrupts a pending read without touching the OVERLAPPED/buffer pool. Call this
        // first when a reader is running on another thread, so its blocked
        // FT_GetOverlappedResult call returns and that thread stops touching the pool --
        // only then is it safe for close() to release it. Calling both from the same thread
        // that owns the read loop, close() alone is fine.
        void abortReads();

        // A plain blocking bulk write. Commands are rare (config changes, acks) and small
        // (8/12/16 bytes per DP 4), so there is no need for the overlapped machinery the
        // read side needs for throughput.
        bool sendCommand(const uint8_t* data, size_t len) override;

        // Waits for the oldest outstanding read to complete, hands its bytes to `out`, and
        // immediately re-queues that buffer. False means the device is gone or the read
        // failed -- DP 3.3's warning that `Stop stream` closes the endpoint entirely shows
        // up here as exactly this.
        bool nextFrame(std::vector<uint8_t>& out) override;

    private:
        // Each FT_ReadPipeEx call asks for PACKETS_PER_READ radio packets at once instead of
        // one, so QUEUE_DEPTH chunk-sized calls are in flight rather than QUEUE_DEPTH
        // packet-sized ones. This exists because raising QUEUE_DEPTH alone (8 -> 64 -> 256)
        // plateaued well under the FT601's SuperSpeed ceiling: throughput capped at the same
        // ~11000 reads/sec regardless of buffer count or which thread issued them, which
        // points at fixed per-call (WinUSB round-trip) overhead rather than a buffering
        // shortfall. Batching packets into fewer, larger calls is the standard answer to that
        // -- see RSR200_PLAN.md's transport_usb.cpp note and the D3XX SDK's own
        // WU_DataStreamerApp, which streams at a configurable transfer size for the same
        // reason.
        static constexpr ULONG QUEUE_DEPTH = 16;
        static constexpr ULONG PACKETS_PER_READ = 8;
        static constexpr ULONG CHUNK_BYTES = (ULONG)USB_PACKET_BYTES * PACKETS_PER_READ;

        bool openHandle(FT_STATUS createStatus, FT_HANDLE h, std::string& err);
        bool startQueuedReads(std::string& err);
        bool queueRead(size_t slot, std::string& err);

        FT_HANDLE handle = nullptr;
        std::vector<std::vector<uint8_t>> buffers;   // QUEUE_DEPTH buffers, CHUNK_BYTES each
        std::vector<OVERLAPPED> overlapped;
        size_t cursor = 0;

        // Packets already delivered out of the chunk currently being consumed. `cursor` is
        // the next slot to wait on; `drainSlot` is the (possibly different, already-completed)
        // slot currently being handed out one packet at a time.
        size_t drainSlot = 0;
        size_t packetInChunk = 0;
        size_t packetsInCurrentChunk = 0;
        bool haveChunk = false;

        bool streamPipeSet = false;
    };

}
