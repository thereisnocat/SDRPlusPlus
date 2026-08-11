// Checks for the RSR200 device layer: configuration ordering, command numbering, the
// acknowledgement and retry rules, and frame parsing.
//
//   c++ -std=c++17 -O2 -o /tmp/t test/test_device.cpp && /tmp/t
//
// Driven entirely by a fake transport, so the sequencing that will be hard to observe once
// a real radio is answering can be pinned down now.

#include "../src/rsr200_device.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace rsr200;

static int failures = 0;

static void check(bool cond, const std::string& what) {
    printf("  %-62s %s\n", what.c_str(), cond ? "ok" : "FAIL");
    if (!cond) { failures++; }
}

// Records what the device sends and hands back frames on demand.
struct FakeTransport : Transport {
    Kind _kind = KIND_LAN_TCP;
    std::vector<std::vector<uint8_t>> sent;
    std::deque<std::vector<uint8_t>> frames;
    BlockLayout lastLayout;
    bool layoutSet = false;
    bool refuse = false;

    Kind kind() const override { return _kind; }

    bool sendCommand(const uint8_t* d, size_t n) override {
        if (refuse) { return false; }
        sent.push_back(std::vector<uint8_t>(d, d + n));
        return true;
    }

    bool nextFrame(std::vector<uint8_t>& out) override {
        if (frames.empty()) { return false; }
        out = frames.front();
        frames.pop_front();
        return true;
    }

    void setLayout(const BlockLayout& l) override { lastLayout = l; layoutSet = true; }

    // Instruction bytes of everything sent, in order.
    std::vector<uint8_t> instructions() const {
        std::vector<uint8_t> v;
        for (const auto& c : sent) { v.push_back(c[4]); }
        return v;
    }
    int countOf(uint8_t instruction) const {
        int n = 0;
        for (const auto& c : sent) { if (c[4] == instruction) { n++; } }
        return n;
    }
    const std::vector<uint8_t>* firstOf(uint8_t instruction) const {
        for (const auto& c : sent) { if (c[4] == instruction) { return &c; } }
        return nullptr;
    }
    ptrdiff_t indexOf(uint8_t instruction) const {
        for (size_t i = 0; i < sent.size(); i++) { if (sent[i][4] == instruction) { return (ptrdiff_t)i; } }
        return -1;
    }
};

// Build a LAN block carrying a given counter, command number and one embedded reply.
static std::vector<uint8_t> makeLanBlock(const BlockLayout& l, uint32_t counter,
                                         uint8_t cmdNo, const uint8_t reply[8],
                                         uint8_t temp = 25) {
    std::vector<uint8_t> b(l.blockBytes, 0);
    writeU32(b.data() + l.counterOffset, counter);
    writeU32(b.data() + l.invCounterOffset, ~counter);
    memcpy(b.data() + l.syncOffset, SYNC_BYTES, sizeof(SYNC_BYTES));
    b[l.tempOffset] = temp;
    b[l.cmdNoOffset] = cmdNo;
    writeU32(b.data() + l.cmdCountOffset, reply ? 1 : 0);
    if (reply) { memcpy(b.data() + l.commandsOffset, reply, 8); }
    return b;
}

static void makeConfirmation(uint8_t out[8], uint32_t forCommand) {
    memset(out, 0, 8);
    writeU32(out + 4, forCommand);
}

int main() {
    printf("\nRSR200 device layer\n\n");

    // -----------------------------------------------------------------
    printf("Command numbering\n");
    {
        FakeTransport t;
        Device d;
        d.setTransport(&t);
        Config c;
        d.applyConfig(c, 0);

        bool anyZero = false;
        for (const auto& cmd : t.sent) { if (readU32(cmd.data()) == 0) { anyZero = true; } }
        // DP 3.2: the radio marks its own reports with number 0, so a PC command numbered 0
        // would be indistinguishable from one.
        check(!anyZero, "no command is ever numbered 0");

        bool ascending = true;
        for (size_t i = 1; i < t.sent.size(); i++) {
            if (readU32(t.sent[i].data()) <= readU32(t.sent[i - 1].data())) { ascending = false; }
        }
        check(ascending, "numbers advance with each command");
    }

    // -----------------------------------------------------------------
    printf("\nConfiguration order\n");
    {
        FakeTransport t;
        Device d;
        d.setTransport(&t);
        Config c;
        c.adcClockHz = 125e6;
        c.format = { 2, 16 };
        c.opMode = OP_INDEPENDENT;
        c.tunedHz = 9.5e6;
        check(d.applyConfig(c, 0), "a first configuration goes out");

        const ptrdiff_t clk = t.indexOf(instr::SET_ADC_CLOCK);
        const ptrdiff_t xmit = t.indexOf(instr::SET_DATA_TRANSMISSION);
        const ptrdiff_t lo = t.indexOf(instr::SET_GENERATORS);
        check(clk >= 0 && xmit >= 0 && lo >= 0, "clock, transmission and tuning are all sent");

        // DP 4.6: changing the clock or the transmission settings resynchronises the
        // channels. Tuning has to come after, or the LOs are set and then reset underneath.
        check(clk < lo && xmit < lo, "tuning follows the commands that resynchronise the channels");

        // Port mode should describe what was asked for.
        const std::vector<uint8_t>* x = t.firstOf(instr::SET_DATA_TRANSMISSION);
        check(x && (*x)[5] == IFACE_LAN, "the LAN interface is named");
        check(x && ((*x)[6] & (1 << 4)) != 0, "port mode says dual channel");
        check(x && ((*x)[6] & (1 << 5)) != 0, "port mode says 16 bit");
        check(x && ((*x)[7] & 0x03) == OP_INDEPENDENT, "DSP mode says Separate");
    }

    // -----------------------------------------------------------------
    printf("\nReconfiguring stops the stream first\n");
    {
        // DP 3.3: changing the LAN transmission settings stops streaming, and the restart
        // has to name a size code matching the new format.
        FakeTransport t;
        Device d;
        d.setTransport(&t);
        Config c;
        d.applyConfig(c, 0);
        d.startStream(0);
        t.sent.clear();

        Config c2 = c;
        c2.format = { 2, 24 };
        d.applyConfig(c2, 100);
        check(t.indexOf(instr::STOP_STREAM) == 0, "the stream is stopped before anything is changed");
        check(!d.isStreaming(), "the device knows it is no longer streaming");

        t.sent.clear();
        d.startStream(200);
        const std::vector<uint8_t>* s = t.firstOf(instr::START_STREAM);
        check(s && (*s)[6] == lanLayout({ 2, 24 }).startStreamSizeCode,
              "the restart names the size code for the new format");
        check(s && (*s)[5] == PORT_TCP, "and the transport's own port");
    }

    // -----------------------------------------------------------------
    printf("\nTuning\n");
    {
        FakeTransport t;
        Device d;
        d.setTransport(&t);
        Config c;
        c.adcClockHz = 125e6;
        d.applyConfig(c, 0);
        t.sent.clear();

        check(d.tune(95e6, 0), "tuning is accepted");
        const std::vector<uint8_t>* g = t.firstOf(instr::SET_GENERATORS);
        // DP 4.6: separate per-channel tuning leaves the phase relationship undefined.
        check(g && (*g)[5] == GEN_LO_BOTH, "both oscillators are set with one command");
        check(g && readU32(g->data() + 6) == 30000000u, "95 MHz at a 125 MHz clock tunes the LO to 30 MHz");

        const Tuning tn = d.currentTuning();
        check(tn.zone == 2 && tn.spectrumInverted, "and reports zone 2 with the spectrum reversed");

        check(t.countOf(instr::SET_GENERATORS) == 1, "no separate per-channel commands are sent");
    }

    // -----------------------------------------------------------------
    printf("\nAcknowledgement and retry\n");
    {
        FakeTransport t;
        Device d;
        d.setTransport(&t);
        Config c;
        d.applyConfig(c, 0);

        check(d.awaitingAck(), "the last command is awaiting acknowledgement");
        const uint32_t first = d.pendingNumber();

        // Nothing yet: too soon to retry.
        d.service(100);
        check(d.pendingNumber() == first && d.pendingAttempts() == 0, "no retry before the timeout");

        // DP 3.5: firmware 22x ignores the repeat counter, so a retry must go out under a
        // fresh number rather than repeating the old one.
        const size_t before = t.sent.size();
        d.service(1000);
        check(t.sent.size() == before + 1, "a timeout re-issues the command");
        check(d.pendingNumber() != first, "the retry carries a NEW command number");
        check(readU32(t.sent.back().data()) == d.pendingNumber(), "and that number is what went on the wire");
        check(t.sent.back()[4] == t.sent[before - 1][4], "the instruction is unchanged");

        // Acknowledging the current number clears it.
        const BlockLayout l = d.layout();
        uint8_t reply[8];
        makeConfirmation(reply, d.pendingNumber());
        t.frames.push_back(makeLanBlock(l, 1, 5, reply));
        d.pump();
        check(!d.awaitingAck(), "a matching confirmation clears the pending command");

        // Give up after the documented number of attempts.
        FakeTransport t2;
        Device d2;
        std::string err;
        d2.setTransport(&t2);
        d2.onError = [&](const std::string& m) { err = m; };
        d2.tune(10e6, 0);
        for (int i = 1; i <= Device::MAX_ATTEMPTS + 1; i++) { d2.service((uint64_t)i * 1000); }
        check(!err.empty(), "it eventually gives up and reports");
        check(!d2.awaitingAck(), "and stops retrying");
    }

    // -----------------------------------------------------------------
    printf("\nEmbedded replies\n");
    {
        FakeTransport t;
        Device d;
        d.setTransport(&t);
        Config c;
        d.applyConfig(c, 0);
        const BlockLayout l = d.layout();

        int replies = 0;
        d.onReply = [&](const Reply&) { replies++; };

        uint8_t reply[8];
        makeConfirmation(reply, 999);

        // DP 3.1: the same command data repeats in every block until the number changes.
        t.frames.push_back(makeLanBlock(l, 1, 7, reply));
        t.frames.push_back(makeLanBlock(l, 2, 7, reply));
        t.frames.push_back(makeLanBlock(l, 3, 8, reply));
        d.pump(); d.pump(); d.pump();
        check(replies == 2, "a reply is taken once per change of command number, not once per block");

        // A self-generated report must not clear a command we are waiting on.
        Device d2;
        FakeTransport t2;
        d2.setTransport(&t2);
        d2.tune(10e6, 0);
        const uint32_t waiting = d2.pendingNumber();
        uint8_t selfGen[8] = { instr::SET_ADC_CLOCK, 0xD0, 0x04, 0x00, 0, 0, 0, 0 };
        t2.frames.push_back(makeLanBlock(d2.layout(), 1, 3, selfGen));
        d2.pump();
        check(d2.awaitingAck() && d2.pendingNumber() == waiting,
              "a self-generated report does not satisfy a pending command");
    }

    // -----------------------------------------------------------------
    printf("\nFrame parsing\n");
    {
        FakeTransport t;
        Device d;
        d.setTransport(&t);
        Config c;
        c.format = { 2, 16 };
        d.applyConfig(c, 0);
        const BlockLayout l = d.layout();

        int frames = 0;
        bool sawB = false, gap = false;
        Status st;
        d.onSamples = [&](const SampleBlock& b) {
            frames = b.frames;
            sawB = (b.chB != nullptr);
            gap = b.sequenceGap;
            st = b.status;
        };

        t.frames.push_back(makeLanBlock(l, 10, 1, nullptr));
        d.pump();
        check(frames == l.samplesPerChannel, "a block delivers its full sample count");
        check(sawB, "dual channel delivers a second channel");
        check(!gap, "the first block is not a gap");
        check(st.temperatureC == 25, "the status header comes through");

        t.frames.push_back(makeLanBlock(l, 11, 1, nullptr));
        d.pump();
        check(!gap, "consecutive counters are not a gap");

        t.frames.push_back(makeLanBlock(l, 20, 1, nullptr));
        d.pump();
        check(gap, "a jump in the block counter is reported as lost data");

        // Auto-ATT scales the stream down 2 bits; the device has to scale it back.
        t.frames.push_back(makeLanBlock(l, 21, 1, nullptr, 0x80));
        d.pump();
        check(st.autoAttActive, "the Auto-ATT indicator is recognised");

        // A corrupted trailer is refused rather than delivered as garbage.
        std::string err;
        d.onError = [&](const std::string& m) { err = m; };
        std::vector<uint8_t> bad = makeLanBlock(l, 22, 1, nullptr);
        bad[l.syncOffset] ^= 0xFF;
        t.frames.push_back(bad);
        d.pump();
        check(!err.empty(), "a block failing its sync check is rejected");
    }

    // -----------------------------------------------------------------
    printf("\nSpectrum inversion follows the current tuning, not a fixed setting\n");
    {
        // A wanted frequency's Nyquist zone -- and therefore whether it comes off the ADC
        // mirrored -- depends on the exact frequency and the ADC clock, not on which of the
        // radio's inputs it happens to be reached through (found live 2026-08-11: VHF and
        // HF1 aren't uniformly one or the other). 125 MHz clock, half = 62.5 MHz: 30 MHz
        // falls in zone 1 (odd, not inverted) and 80 MHz in zone 2 (even, inverted), so one
        // Device exercises both without needing separate setups.
        FakeTransport t;
        Device d;
        d.setTransport(&t);
        Config c;
        c.format = { 1, 16 };
        c.adcClockHz = 125e6;
        d.applyConfig(c, 0);
        const BlockLayout l = d.layout();

        SampleBlock got;
        d.onSamples = [&](const SampleBlock& b) { got = b; };

        auto pokeSample = [&](std::vector<uint8_t>& block, int16_t iVal, int16_t qVal) {
            block[0] = (uint8_t)(iVal & 0xFF);
            block[1] = (uint8_t)((iVal >> 8) & 0xFF);
            block[2] = (uint8_t)(qVal & 0xFF);
            block[3] = (uint8_t)((qVal >> 8) & 0xFF);
        };

        d.tune(30e6, 0);
        check(d.currentTuning().zone == 1 && !d.currentTuning().spectrumInverted,
              "30 MHz at a 125 MHz clock is zone 1, not inverted");
        std::vector<uint8_t> notInverted = makeLanBlock(l, 1, 1, nullptr);
        pokeSample(notInverted, 1000, 2000);
        t.frames.push_back(notInverted);
        d.pump();
        check(std::abs(got.chA[1] - (2000.0f / 32768.0f)) < 1e-6f,
              "not inverted: Q comes through unchanged");

        d.tune(80e6, 0);
        check(d.currentTuning().zone == 2 && d.currentTuning().spectrumInverted,
              "80 MHz at the same clock is zone 2, inverted");
        std::vector<uint8_t> inverted = makeLanBlock(l, 2, 1, nullptr);
        pokeSample(inverted, 1000, 2000);
        t.frames.push_back(inverted);
        d.pump();
        check(std::abs(got.chA[0] - (1000.0f / 32768.0f)) < 1e-6f,
              "inverted: I is untouched");
        check(std::abs(got.chA[1] - (-2000.0f / 32768.0f)) < 1e-6f,
              "inverted: Q is negated to conjugate the spectrum");

        // Retuning back out of the even zone stops correcting again -- this isn't a sticky
        // per-session setting, it tracks the tuning live.
        d.tune(30e6, 0);
        std::vector<uint8_t> backToOdd = makeLanBlock(l, 3, 1, nullptr);
        pokeSample(backToOdd, 1000, 2000);
        t.frames.push_back(backToOdd);
        d.pump();
        check(std::abs(got.chA[1] - (2000.0f / 32768.0f)) < 1e-6f,
              "retuning back to an odd zone stops inverting again");
    }

    // -----------------------------------------------------------------
    printf("\nUSB framing uses the same device\n");
    {
        FakeTransport t;
        t._kind = Transport::KIND_USB;
        Device d;
        d.setTransport(&t);
        Config c;
        c.format = { 1, 16 };
        d.applyConfig(c, 0);

        // Commands take their USB lengths without the device knowing or caring.
        const std::vector<uint8_t>* g = t.firstOf(instr::SET_GENERATORS);
        check(g && g->size() == 12, "commands use the USB length on a USB transport");

        d.startStream(0);
        const std::vector<uint8_t>* s = t.firstOf(instr::START_STREAM);
        check(s && (*s)[5] == PORT_USB, "and the USB stream port");

        int frames = 0;
        d.onSamples = [&](const SampleBlock& b) { frames = b.frames; };
        std::vector<uint8_t> pkt(USB_PACKET_BYTES, 0);
        writeU32(pkt.data(), 1);
        pkt[USB_TEMP_OFFSET] = 30;
        pkt[USB_CMD_NO_OFFSET] = 0;
        t.frames.push_back(pkt);
        d.pump();
        check(frames == 1020, "a USB packet delivers 1020 samples at 1 channel 16 bit");
    }

    printf("\n%s (%d failure%s)\n\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
