// Checks LanTcpTransport against a synthetic TCP server, deliberately mangling the framing
// the way a real socket would: garbage before the first block, a block split across
// multiple recv()s, and two blocks glued into a single write. See RSR200_PLAN.md section
// 3.2. No radio needed -- this is the same "prove it against a known-correct source before
// the hardware exists" approach the rest of this project has used throughout.
//
//   c++ -std=c++17 -O2 -pthread -o /tmp/t test/test_lan_transport.cpp && /tmp/t
#include "../src/rsr200_lan_transport.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <string>

using namespace rsr200;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-62s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

// Builds one syntactically valid LAN block: real trailer (sync words, counter, inverse
// counter), IQ payload zeroed since only the transport's framing is under test here --
// what the bytes mean is the device layer's job, already covered by test_device.cpp.
static std::vector<uint8_t> makeBlock(const BlockLayout& l, uint32_t counter) {
    std::vector<uint8_t> b(l.blockBytes, 0);
    memcpy(b.data() + l.syncOffset, SYNC_BYTES, sizeof(SYNC_BYTES));
    writeU32(b.data() + l.counterOffset, counter);
    writeU32(b.data() + l.invCounterOffset, ~counter);
    // A distinctive byte early in each block, so a test can tell blocks apart without
    // decoding IQ payload.
    b[0] = (uint8_t)(counter & 0xFF);
    return b;
}

// Minimal loopback server: accepts one connection, then does exactly what the test script
// hands it -- write these bytes, sleep this long, write these bytes... -- so a test can
// control precisely how the framing arrives at the transport.
struct FakeServer {
    SockFd listenFd = RSR200_INVALID_SOCK;
    SockFd clientFd = RSR200_INVALID_SOCK;
    uint16_t port = 0;
    std::thread acceptThread;
    std::vector<uint8_t> received;   // whatever the transport sent us (commands)

    bool start() {
        listenFd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;   // ask the OS for a free port
        if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) != 0) { return false; }
        socklen_t alen = sizeof(addr);
        getsockname(listenFd, (sockaddr*)&addr, &alen);
        port = ntohs(addr.sin_port);
        return listen(listenFd, 1) == 0;
    }

    void acceptOne() {
        acceptThread = std::thread([this] {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            clientFd = accept(listenFd, (sockaddr*)&peer, &plen);
        });
    }

    void waitForClient() {
        if (acceptThread.joinable()) { acceptThread.join(); }
    }

    void sendRaw(const std::vector<uint8_t>& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            const int n = ::send(clientFd, (const char*)data.data() + sent, (int)(data.size() - sent), 0);
            if (n <= 0) { return; }
            sent += (size_t)n;
        }
    }

    // Reads whatever the transport has sent as a command, with a short timeout -- for
    // checking sendCommand() actually put bytes on the wire.
    std::vector<uint8_t> recvCommand(size_t expect, int timeoutMs = 500) {
        struct timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        std::vector<uint8_t> buf(expect);
        size_t got = 0;
        while (got < expect) {
            const int n = ::recv(clientFd, (char*)buf.data() + got, (int)(expect - got), 0);
            if (n <= 0) { break; }
            got += (size_t)n;
        }
        buf.resize(got);
        return buf;
    }

    void stop() {
        if (clientFd != RSR200_INVALID_SOCK) {
#ifdef _WIN32
            closesocket(clientFd);
#else
            ::close(clientFd);
#endif
        }
        if (listenFd != RSR200_INVALID_SOCK) {
#ifdef _WIN32
            closesocket(listenFd);
#else
            ::close(listenFd);
#endif
        }
        if (acceptThread.joinable()) { acceptThread.join(); }
    }
};

int main() {
    printf("\nRSR200 LAN TCP transport\n\n");

    const StreamFormat fmt{ 1, 16 };   // 1 channel, 16 bit -- smallest block, fastest test
    const BlockLayout layout = lanLayout(fmt);
    printf("  block size for 1ch/16bit: %zu bytes\n", layout.blockBytes);

    // -- Connection and clean shutdown --------------------------------------------
    {
        FakeServer server;
        check(server.start(), "fake server binds a loopback port");
        server.acceptOne();

        LanTcpTransport t;
        check(t.connect("127.0.0.1", server.port), "transport connects");
        server.waitForClient();
        check(t.isConnected(), "reports connected");

        // stop() deliberately does NOT fully disconnect -- it only shuts down the read
        // side (unsticking a blocked nextFrame()/readPacket() on another thread), leaving
        // the write side usable so a caller can still send a Stop Stream command
        // afterward. A full close() used to happen here instead, which meant Stop Stream
        // could never actually reach the radio -- see stop()'s own comment and
        // RSR200_PLAN.md's LAN section for why this matters in practice, not just in
        // theory.
        t.stop();
        check(t.isConnected(), "stop() alone does not disconnect");
        check(t.sendCommand((const uint8_t*)"12345678", 8), "the write side still works after stop()");
        t.close();
        check(!t.isConnected(), "close() is what actually disconnects");
        server.stop();
    }

    // -- Framing: garbage prefix, a split block, and two glued together -----------
    {
        FakeServer server;
        server.start();
        server.acceptOne();

        LanTcpTransport t;
        t.connect("127.0.0.1", server.port);
        server.waitForClient();
        t.setLayout(layout);

        std::vector<uint8_t> stream;
        // Garbage that happens to contain a byte matching the sync word's first byte, so
        // a resync that merely looks for that one byte rather than validating the whole
        // trailer would misfire here.
        stream.push_back(SYNC_BYTES[0]);
        stream.push_back(0xAA);
        stream.push_back(0x00);

        const std::vector<uint8_t> b1 = makeBlock(layout, 1);
        const std::vector<uint8_t> b2 = makeBlock(layout, 2);
        const std::vector<uint8_t> b3 = makeBlock(layout, 3);

        stream.insert(stream.end(), b1.begin(), b1.end());
        // b2 and b3 glued into the stream back to back, exactly as a fast sender's bytes
        // would coalesce over TCP with no framing of its own.
        stream.insert(stream.end(), b2.begin(), b2.end());
        stream.insert(stream.end(), b3.begin(), b3.end());

        // Send it in three arbitrarily-sized pieces. Both split points land inside b2 (not
        // on a block boundary), so b2 has to be reassembled across two recv()s, and the
        // last piece starts 4 bytes before b3 begins, so b3 arrives glued to the tail of
        // b2 with no write-boundary between them -- the two cases that actually exercise
        // accumulation, as opposed to every block conveniently starting a fresh recv().
        const size_t splitA = 3 + layout.blockBytes + 100;
        const size_t splitB = 3 + layout.blockBytes * 2 - 4;
        std::thread feeder([&] {
            server.sendRaw(std::vector<uint8_t>(stream.begin(), stream.begin() + splitA));
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            server.sendRaw(std::vector<uint8_t>(stream.begin() + splitA, stream.begin() + splitB));
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            server.sendRaw(std::vector<uint8_t>(stream.begin() + splitB, stream.end()));
        });

        std::vector<uint8_t> got1, got2, got3;
        check(t.nextFrame(got1), "first block delivered despite a leading garbage byte");
        check(got1.size() == layout.blockBytes, "  and is exactly one block long");
        check(got1[0] == 1, "  and is block 1, not the garbage or a false sync");

        check(t.nextFrame(got2), "second block delivered despite arriving split across recv()s");
        check(got2[0] == 2, "  and is block 2 in order");

        check(t.nextFrame(got3), "third block delivered despite being glued to the second");
        check(got3[0] == 3, "  and is block 3");

        feeder.join();
        t.stop();
        server.stop();
    }

    // -- sendCommand actually reaches the peer -------------------------------------
    {
        FakeServer server;
        server.start();
        server.acceptOne();

        LanTcpTransport t;
        t.connect("127.0.0.1", server.port);
        server.waitForClient();

        const std::vector<uint8_t> cmd = cmdReadVersion(/*no=*/7, /*lan=*/true);
        check(t.sendCommand(cmd.data(), cmd.size()), "sendCommand reports success");

        const std::vector<uint8_t> arrived = server.recvCommand(cmd.size());
        check(arrived == cmd, "and the exact bytes arrive at the peer");

        t.stop();
        server.stop();
    }

    // -- readPacket(): the pre-streaming "packet mode" reply path -------------------
    // See RSR200_PLAN.md's "First live LAN connection" section: before Start Stream, the
    // radio replies to each command as its own standalone fixed-size packet (8 bytes for an
    // ordinary confirmation, 12 for the version query specifically), not embedded in a
    // block. This is what Device::send() calls to read one.
    {
        FakeServer server;
        server.start();
        server.acceptOne();

        LanTcpTransport t;
        t.connect("127.0.0.1", server.port);
        server.waitForClient();

        // An 8-byte confirmation, split across two writes -- the same kind of TCP-level
        // split nextFrame()'s own test above exercises for blocks, but for a packet small
        // enough that a real send could very plausibly land it in two recv()s too.
        uint8_t confirmation[8] = { 0, 0, 0, 0, 0x2A, 0, 0, 0 };   // confirms command 0x2A
        std::thread feeder([&] {
            server.sendRaw(std::vector<uint8_t>(confirmation, confirmation + 3));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            server.sendRaw(std::vector<uint8_t>(confirmation + 3, confirmation + 8));
        });
        std::vector<uint8_t> got;
        check(t.readPacket(got, 8), "an 8-byte confirmation is read despite arriving split");
        check(got.size() == 8 && memcmp(got.data(), confirmation, 8) == 0,
              "  and the exact bytes come through unchanged");
        feeder.join();

        // The 12-byte version-query reply, immediately followed by extra bytes that don't
        // belong to it -- the start of what would be the first real streaming block, in
        // real use. Those extra bytes must still be there afterward, not consumed or lost:
        // this is exactly the boundary that was silently corrupting the first block's
        // framing before readPacket() existed at all.
        uint8_t verReply[12] = { 12, 0, 0, 0, instr::READ_VERSION, 0x34, 0x12, 0x00, 0x25, 0x02, 0x00, 0x00 };
        uint8_t trailing[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
        std::vector<uint8_t> combined(verReply, verReply + 12);
        combined.insert(combined.end(), trailing, trailing + 4);
        server.sendRaw(combined);
        std::vector<uint8_t> gotVer;
        check(t.readPacket(gotVer, 12), "the 12-byte version reply is read");
        check(gotVer.size() == 12 && memcmp(gotVer.data(), verReply, 12) == 0,
              "  and matches exactly, not off by the trailing bytes");

        // Confirms the shared-buffer design directly: whatever's left over after readPacket()
        // consumed exactly 12 bytes must still be available to whichever reader looks next --
        // here, another readPacket() rather than nextFrame(), since block-sized data isn't
        // set up in this test, but the mechanism (recvBuf) is the same either way.
        std::vector<uint8_t> gotTrailing;
        check(t.readPacket(gotTrailing, 4) && memcmp(gotTrailing.data(), trailing, 4) == 0,
              "leftover bytes after a readPacket() are neither lost nor duplicated");

        t.stop();
        server.stop();
    }

    // -- A dead connection makes readPacket() return false too, not hang ------------
    {
        FakeServer server;
        server.start();
        server.acceptOne();

        LanTcpTransport t;
        t.connect("127.0.0.1", server.port, /*recvTimeoutMs=*/300);
        server.waitForClient();
        server.stop();   // drop the connection from the far end

        std::vector<uint8_t> got;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = t.readPacket(got, 8);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(!ok, "readPacket returns false on a dropped connection");
        check(ms < 5000, "and does not hang indefinitely");
        t.stop();
    }

    // -- A dead connection makes nextFrame return false, not hang -------------------
    {
        FakeServer server;
        server.start();
        server.acceptOne();

        LanTcpTransport t;
        t.connect("127.0.0.1", server.port, /*recvTimeoutMs=*/300);
        server.waitForClient();
        t.setLayout(layout);
        server.stop();   // drop the connection from the far end

        std::vector<uint8_t> got;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = t.nextFrame(got);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(!ok, "nextFrame returns false on a dropped connection");
        check(ms < 5000, "and does not hang indefinitely");
        t.stop();
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
