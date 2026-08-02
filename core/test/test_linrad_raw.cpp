// Linrad raw header layout and conversion checks.
//
//   c++ -std=c++17 -O2 -I../src -o /tmp/t test_linrad_raw.cpp && /tmp/t
//
// The header is a bare 41 bytes with no version field and no real magic number, so nothing
// in the format itself will tell a reader we got it wrong -- the file will simply be
// misinterpreted. These assertions are the only guard there is.
#include "utils/linrad_raw.h"

#include <cstdio>
#include <cstring>
#include <cmath>

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) { std::printf("  FAIL: %s\n", what); failures++; }
}

int main() {
    // ---- Packing -----------------------------------------------------------------
    // Natural alignment would pad this to 48 because of the two doubles. 41 is the whole
    // format; if this is wrong every byte after the header is off.
    check(sizeof(linrad::Header) == 41, "header is 41 bytes");

    check(offsetof(linrad::Header, proprietaryChunk) == 0,  "proprietaryChunk at 0");
    check(offsetof(linrad::Header, timestamp) == 4,         "timestamp at 4");
    check(offsetof(linrad::Header, passbandCenter) == 12,   "passbandCenter at 12");
    check(offsetof(linrad::Header, passbandDirection) == 20,"passbandDirection at 20");
    check(offsetof(linrad::Header, inputMode) == 24,        "inputMode at 24");
    check(offsetof(linrad::Header, rfChannels) == 28,       "rfChannels at 28");
    check(offsetof(linrad::Header, adChannels) == 32,       "adChannels at 32");
    check(offsetof(linrad::Header, adSpeed) == 36,          "adSpeed at 36");
    check(offsetof(linrad::Header, saveInitFlag) == 40,     "saveInitFlag at 40");

    // ---- Input mode --------------------------------------------------------------
    // A real dual-channel file carries 0x26. Decoding that is what tells us the size bits
    // are absent for int16, which is the assumption the whole conversion rests on.
    check(linrad::MODE_DUAL_IQ16 == 0x26, "dual channel int16 mode is 0x26");
    check((linrad::MODE_DUAL_IQ16 & linrad::TWO_CHANNELS) != 0, "0x26 has TWO_CHANNELS");
    check((linrad::MODE_DUAL_IQ16 & linrad::IQ_DATA) != 0,      "0x26 has IQ_DATA");
    check((linrad::MODE_DUAL_IQ16 & linrad::DIGITAL_IQ) != 0,   "0x26 has DIGITAL_IQ");
    check((linrad::MODE_DUAL_IQ16 & (linrad::BYTE_INPUT | linrad::DWORD_INPUT |
                                     linrad::FLOAT_INPUT | linrad::QWORD_INPUT)) == 0,
          "0x26 sets no sample-size bit, meaning int16");

    // ---- Field values ------------------------------------------------------------
    const std::time_t when = 1511398636;  // 2017-11-23 00:57:16Z
    const linrad::Header h = linrad::makeDualChannelHeader(1130000.0, 2000000.0, when);

    check(h.proprietaryChunk == -1, "leading sentinel is -1");
    check(h.rfChannels == 2, "two RF channels");
    check(h.adChannels == 4, "four A/D channels");
    check(h.adSpeed == 2000000, "sample rate carried through");
    check(h.saveInitFlag == 0, "save_init_flag zero");
    check(h.passbandDirection == 1, "direction is +1 by default");

    // MHz, not Hz. Off by 1e6 is the easiest possible mistake here and nothing downstream
    // would catch it -- WavViewDX would just show the wrong frequency.
    check(std::fabs(h.passbandCenter - 1.13) < 1e-9, "centre converted Hz -> MHz");
    check(std::fabs(h.timestamp - (double)when) < 1e-6, "timestamp is Unix epoch seconds");

    const linrad::Header inv = linrad::makeDualChannelHeader(1130000.0, 2000000.0, when, true);
    check(inv.passbandDirection == -1, "inverted spectrum gives -1");

    // ---- Round trip --------------------------------------------------------------
    const std::vector<uint8_t> bytes = linrad::serialize(h);
    check(bytes.size() == 41, "serialized form is 41 bytes");

    linrad::Header back{};
    check(linrad::parse(bytes.data(), bytes.size(), back), "parses its own output");
    check(back.adSpeed == h.adSpeed && back.rfChannels == h.rfChannels,
          "round trip preserves fields");
    check(std::fabs(back.passbandCenter - h.passbandCenter) < 1e-12,
          "round trip preserves centre");

    // ---- Rejection ---------------------------------------------------------------
    check(!linrad::parse(bytes.data(), 40, back), "rejects a short buffer");

    std::vector<uint8_t> bad = bytes;
    bad[0] = 0;  // sentinel no longer -1
    check(!linrad::parse(bad.data(), bad.size(), back), "rejects a missing sentinel");

    bad = bytes;
    const int32_t nonsense = 7;
    memcpy(bad.data() + 20, &nonsense, 4);
    check(!linrad::parse(bad.data(), bad.size(), back), "rejects a bad passband direction");

    if (failures) { std::printf("FAILED (%d)\n", failures); return 1; }
    std::printf("PASSED (0 failures)\n");
    return 0;
}
