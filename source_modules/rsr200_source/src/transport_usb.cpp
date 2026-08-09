#include "transport_usb.h"
#include <cstring>

namespace rsr200 {

    namespace {
        std::string statusStr(FT_STATUS st) {
            return "status " + std::to_string((unsigned)st);
        }

        // FT_Create needs FT_CreateDeviceInfoList to have run first -- undocumented in the
        // function reference, but present in every FTDI sample (DRV_DriverInterface.cpp in
        // the D3XX SDK) and observed to matter: opening by index without it first can find
        // stale or zero devices.
        bool refreshDeviceList(DWORD& count) {
            return FT_SUCCESS(FT_CreateDeviceInfoList(&count));
        }
    }

    std::vector<std::string> UsbTransport::listDevices() {
        std::vector<std::string> out;
        DWORD count = 0;
        if (!refreshDeviceList(count) || count == 0) { return out; }

        std::vector<FT_DEVICE_LIST_INFO_NODE> nodes(count);
        if (FT_FAILED(FT_GetDeviceInfoList(nodes.data(), &count))) { return out; }

        for (DWORD i = 0; i < count; i++) {
            char desc[33] = { 0 };
            char serial[17] = { 0 };
            memcpy(desc, nodes[i].Description, 32);
            memcpy(serial, nodes[i].SerialNumber, 16);
            out.push_back(std::string(desc) + " (" + serial + ")");
        }
        return out;
    }

    bool UsbTransport::isSuperSpeed(int index) {
        DWORD count = 0;
        if (!refreshDeviceList(count) || index < 0 || (DWORD)index >= count) { return false; }
        std::vector<FT_DEVICE_LIST_INFO_NODE> nodes(count);
        if (FT_FAILED(FT_GetDeviceInfoList(nodes.data(), &count))) { return false; }
        return (nodes[index].Flags & FT_FLAGS_SUPERSPEED) != 0;
    }

    bool UsbTransport::open(int index, std::string& err) {
        close();
        DWORD count = 0;
        if (!refreshDeviceList(count)) {
            err = "FT_CreateDeviceInfoList failed";
            return false;
        }
        if (count == 0 || index < 0 || (DWORD)index >= count) {
            err = "no D3XX device at index " + std::to_string(index) +
                  " (" + std::to_string(count) + " found)";
            return false;
        }

        FT_HANDLE h = nullptr;
        FT_STATUS st = FT_Create((PVOID)(ULONG_PTR)index, FT_OPEN_BY_INDEX, &h);
        return openHandle(st, h, err);
    }

    bool UsbTransport::openBySerial(const std::string& serial, std::string& err) {
        close();
        DWORD count = 0;
        if (!refreshDeviceList(count)) {
            err = "FT_CreateDeviceInfoList failed";
            return false;
        }

        FT_HANDLE h = nullptr;
        FT_STATUS st = FT_Create((PVOID)serial.c_str(), FT_OPEN_BY_SERIAL_NUMBER, &h);
        return openHandle(st, h, err);
    }

    bool UsbTransport::openHandle(FT_STATUS createStatus, FT_HANDLE h, std::string& err) {
        if (FT_FAILED(createStatus) || !h) {
            err = "FT_Create failed (" + statusStr(createStatus) + ")";
            return false;
        }
        handle = h;

        // DP 2.1: no host-side read timeout. The radio paces the stream on its own; a
        // timeout here would only produce spurious short reads while nothing is wrong.
        FT_SetPipeTimeout(handle, USB_ENDPOINT_IN, 0);

        FT_STATUS st = FT_SetStreamPipe(handle, FALSE, FALSE, USB_ENDPOINT_IN, CHUNK_BYTES);
        if (FT_FAILED(st)) {
            err = "FT_SetStreamPipe failed (" + statusStr(st) + ")";
            close();
            return false;
        }
        streamPipeSet = true;

        if (!startQueuedReads(err)) {
            close();
            return false;
        }
        return true;
    }

    bool UsbTransport::startQueuedReads(std::string& err) {
        buffers.assign(QUEUE_DEPTH, std::vector<uint8_t>(CHUNK_BYTES));
        overlapped.assign(QUEUE_DEPTH, OVERLAPPED());
        for (ULONG i = 0; i < QUEUE_DEPTH; i++) {
            memset(&overlapped[i], 0, sizeof(OVERLAPPED));
            FT_STATUS st = FT_InitializeOverlapped(handle, &overlapped[i]);
            if (FT_FAILED(st)) {
                err = "FT_InitializeOverlapped failed (" + statusStr(st) + ")";
                return false;
            }
        }
        cursor = 0;
        packetInChunk = 0;
        haveChunk = false;
        drainSlot = 0;
        packetsInCurrentChunk = 0;
        for (size_t i = 0; i < QUEUE_DEPTH; i++) {
            if (!queueRead(i, err)) { return false; }
        }
        return true;
    }

    bool UsbTransport::queueRead(size_t slot, std::string& err) {
        ULONG got = 0;
        FT_STATUS st = FT_ReadPipeEx(handle, USB_ENDPOINT_IN, buffers[slot].data(),
                                     CHUNK_BYTES, &got, &overlapped[slot]);
        // Overlapped reads report their real completion through FT_GetOverlappedResult;
        // FT_IO_PENDING here is success, not an error.
        if (st != FT_IO_PENDING && FT_FAILED(st)) {
            err = "FT_ReadPipeEx failed (" + statusStr(st) + ")";
            return false;
        }
        return true;
    }

    void UsbTransport::abortReads() {
        if (!handle) { return; }
        FT_AbortPipe(handle, USB_ENDPOINT_IN);
    }

    void UsbTransport::close() {
        if (!handle) { return; }
        if (streamPipeSet) {
            FT_AbortPipe(handle, USB_ENDPOINT_IN);
            for (auto& ov : overlapped) { FT_ReleaseOverlapped(handle, &ov); }
            FT_ClearStreamPipe(handle, FALSE, FALSE, USB_ENDPOINT_IN);
        }
        FT_Close(handle);
        handle = nullptr;
        streamPipeSet = false;
        buffers.clear();
        overlapped.clear();
        cursor = 0;
        drainSlot = 0;
        packetInChunk = 0;
        packetsInCurrentChunk = 0;
        haveChunk = false;
    }

    UsbTransport::~UsbTransport() { close(); }

    bool UsbTransport::sendCommand(const uint8_t* data, size_t len) {
        if (!handle) { return false; }
        ULONG written = 0;
        FT_STATUS st = FT_WritePipe(handle, USB_ENDPOINT_OUT, (PUCHAR)data, (ULONG)len, &written, nullptr);
        return FT_SUCCESS(st) && written == len;
    }

    bool UsbTransport::nextFrame(std::vector<uint8_t>& out) {
        if (!handle) { return false; }

        // Each completed read may hold several packets (PACKETS_PER_READ) -- drain them one
        // at a time before waiting on the next chunk, so the fixed per-call WinUSB overhead
        // is paid once per chunk rather than once per packet.
        while (!haveChunk) {
            ULONG got = 0;
            // DP 3.3: `Stop stream` on USB closes the send endpoint entirely, so a stopped
            // stream shows up here as this call failing (FT_DEVICE_NOT_CONNECTED or an
            // aborted pipe), not as a clean end-of-data -- callers should treat any false
            // return as "the transport is no longer usable", not "try again".
            FT_STATUS st = FT_GetOverlappedResult(handle, &overlapped[cursor], &got, TRUE);
            if (FT_FAILED(st)) { return false; }

            drainSlot = cursor;
            cursor = (cursor + 1) % QUEUE_DEPTH;
            packetsInCurrentChunk = got / (ULONG)USB_PACKET_BYTES;
            packetInChunk = 0;

            if (packetsInCurrentChunk == 0) {
                // A short/empty completion -- seen occasionally around Start Stream. Nothing
                // to hand out; requeue this buffer and wait on the next one.
                std::string err;
                if (!queueRead(drainSlot, err)) { return false; }
                continue;
            }
            haveChunk = true;
        }

        const uint8_t* p = buffers[drainSlot].data() + packetInChunk * USB_PACKET_BYTES;
        out.assign(p, p + USB_PACKET_BYTES);
        packetInChunk++;

        if (packetInChunk >= packetsInCurrentChunk) {
            std::string err;
            if (!queueRead(drainSlot, err)) { return false; }
            haveChunk = false;
        }

        return true;
    }

}
