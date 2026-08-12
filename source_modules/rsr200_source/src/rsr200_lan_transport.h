#pragma once
#include "rsr200_device.h"
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET SockFd;
#define RSR200_INVALID_SOCK INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
typedef int SockFd;
#define RSR200_INVALID_SOCK (-1)
#endif

// TCP transport for the RSR200's LAN interface. See PHASING_PLAN... no, RSR200_PLAN.md
// section 3.2/4.2: commands and, in this transport, the IQ stream too, share one TCP
// connection to port 55557. (UDP for the IQ stream is a distinct transport -- see
// RSR200_PLAN.md 3.3 -- kept separate because framing is what differs between them, per
// rsr200_device.h's own reasoning for why Transport exists at all.)
//
// TCP resynchronisation is the one genuinely transport-specific piece of work here: a LAN
// block can begin anywhere within a stream of recv()'d bytes, so this accumulates a byte
// buffer and uses the protocol layer's own findBlockStart/blockTrailerValid -- the same
// functions the device layer's tests already exercise -- to locate a complete block rather
// than trusting the first bytes read to be a boundary. Deliberately free of any socket
// abstraction beyond raw BSD sockets: no vendor SDK, no install step, and Winsock's API is
// close enough to BSD's that one implementation covers every platform SDR++ targets, which
// is the entire reason this transport goes first (RSR200_PLAN.md section 1).
namespace rsr200 {

    class LanTcpTransport : public Transport {
    public:
        ~LanTcpTransport() override { close(); }

        Kind kind() const override { return KIND_LAN_TCP; }

        // Connects and leaves the socket in blocking mode -- nextFrame() is meant to
        // block, matching the contract Device::pump() already assumes. recvTimeoutMs
        // bounds how long a single recv() can stall, so a dead link is detectable rather
        // than hanging nextFrame() forever; it is not a connect timeout.
        bool connect(const std::string& host, uint16_t port = 55557, int recvTimeoutMs = 2000) {
            close();

#ifdef _WIN32
            static bool wsaInit = false;
            if (!wsaInit) {
                WSADATA wsa;
                WSAStartup(MAKEWORD(2, 2), &wsa);
                wsaInit = true;
            }
#endif
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd == RSR200_INVALID_SOCK) { return setError("socket() failed"); }

            // Nagle batches small writes, which is exactly wrong for a command channel
            // where a caller wants a short packet on the wire immediately.
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

#ifdef _WIN32
            DWORD tv = (DWORD)recvTimeoutMs;
#else
            struct timeval tv{ recvTimeoutMs / 1000, (recvTimeoutMs % 1000) * 1000 };
#endif
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
                close();
                return setError("bad IPv4 address: " + host);
            }

            if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
                // strerror(errno)/WSAGetLastError() -- a bare "connect() failed" gives no way
                // to tell "nothing listening" from "firewalled" from "network unreachable"
                // apart, which matters a lot when bringing this up against real hardware for
                // the first time. See RSR200_PLAN.md's LAN section.
#ifdef _WIN32
                const std::string reason = "WSA error " + std::to_string(WSAGetLastError());
#else
                const std::string reason = strerror(errno);
#endif
                close();
                return setError("connect() to " + host + " failed: " + reason);
            }

            connected = true;
            stopped = false;
            recvBuf.clear();
            return true;
        }

        void close() {
            if (fd != RSR200_INVALID_SOCK) {
#ifdef _WIN32
                closesocket(fd);
#else
                ::close(fd);
#endif
                fd = RSR200_INVALID_SOCK;
            }
            connected = false;
        }

        void stop() {
            // Unstick a blocked nextFrame() from another thread. Closing the socket is
            // the portable way to do that with a blocking BSD socket -- there is no
            // cross-platform "cancel this recv()" call.
            stopped = true;
            close();
        }

        bool isConnected() const { return connected; }
        std::string lastError() const { std::lock_guard<std::mutex> lck(errMtx); return err; }

        bool sendCommand(const uint8_t* data, size_t len) override {
            if (!connected) { return false; }
            size_t sent = 0;
            while (sent < len) {
                const int n = ::send(fd, (const char*)data + sent, (int)(len - sent), 0);
                if (n <= 0) { setError("send() failed"); connected = false; return false; }
                sent += (size_t)n;
            }
            return true;
        }

        // Blocks until a full, validated block is available. False on stop or a dead
        // connection -- Device::pump() treats false as "the transport has stopped",
        // exactly what both of those are.
        bool nextFrame(std::vector<uint8_t>& out) override {
            if (!haveLayout) { return false; }

            for (;;) {
                if (stopped) { return false; }

                const ptrdiff_t start = findBlockStart(recvBuf.data(), recvBuf.size(), layout);
                if (start >= 0) {
                    out.assign(recvBuf.begin() + start, recvBuf.begin() + start + (ptrdiff_t)layout.blockBytes);
                    recvBuf.erase(recvBuf.begin(), recvBuf.begin() + start + (ptrdiff_t)layout.blockBytes);
                    return true;
                }

                // No complete, valid block yet. Keep at most one block's worth of trailing
                // bytes -- findBlockStart needs a full block from a sync candidate onward,
                // so anything older than that can never complete one and would otherwise
                // make this buffer grow without bound on a link that is producing garbage.
                if (recvBuf.size() > layout.blockBytes * 2) {
                    recvBuf.erase(recvBuf.begin(), recvBuf.end() - (ptrdiff_t)layout.blockBytes);
                }

                uint8_t chunk[65536];
                const int n = ::recv(fd, (char*)chunk, (int)sizeof(chunk), 0);
                if (n <= 0) {
                    // A recv() timeout and a genuine disconnect look the same here (n<=0);
                    // distinguishing them only matters for the error message, not for the
                    // return value, since the caller treats both as "stopped" either way.
                    if (!stopped) {
                        setError(n == 0 ? "connection closed by radio" : "recv() failed or timed out");
                    }
                    connected = false;
                    return false;
                }
                recvBuf.insert(recvBuf.end(), chunk, chunk + n);
            }
        }

        void setLayout(const BlockLayout& l) override {
            layout = l;
            haveLayout = true;
            // A format change mid-stream invalidates whatever partial data was buffered
            // under the old layout; resync from whatever arrives next.
            recvBuf.clear();
        }

    private:
        bool setError(const std::string& msg) {
            std::lock_guard<std::mutex> lck(errMtx);
            err = msg;
            return false;
        }

        SockFd fd = RSR200_INVALID_SOCK;
        std::atomic<bool> connected{ false };
        std::atomic<bool> stopped{ false };
        std::vector<uint8_t> recvBuf;
        BlockLayout layout;
        bool haveLayout = false;
        mutable std::mutex errMtx;
        std::string err;
    };
}
