// Inspects a real Linrad dual-channel .raw recording made with Reinhard Weiss' RSR200
// Recorder, to check whether both ADCs actually produced data -- an independent control
// against a completely different piece of software than ours or the DP's HDSDR ExtIO,
// bearing on RSR200_PLAN.md section 10's ADC2/HF2 finding. Not a unit test; takes a file
// path on the command line.

#include "../../../core/src/utils/linrad_raw.h"
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <ctime>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: %s <path to .raw>\n", argv[0]);
        return 1;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        printf("could not open %s\n", argv[1]);
        return 1;
    }

    uint8_t headerBuf[sizeof(linrad::Header)];
    if (fread(headerBuf, 1, sizeof(headerBuf), f) != sizeof(headerBuf)) {
        printf("file too short for a Linrad header\n");
        return 1;
    }

    linrad::Header h;
    if (!linrad::parse(headerBuf, sizeof(headerBuf), h)) {
        printf("not a recognisable Linrad header\n");
        return 1;
    }

    std::time_t ts = (std::time_t)h.timestamp;
    char timeBuf[64];
    ctime_s(timeBuf, sizeof(timeBuf), &ts);
    printf("Timestamp: %s", timeBuf);
    printf("Passband center: %.6f MHz\n", h.passbandCenter);
    printf("Passband direction: %+d\n", h.passbandDirection);
    printf("Input mode: 0x%02X (dual=%d iq=%d digitalIq=%d)\n", h.inputMode,
           (h.inputMode & linrad::TWO_CHANNELS) != 0, (h.inputMode & linrad::IQ_DATA) != 0,
           (h.inputMode & linrad::DIGITAL_IQ) != 0);
    printf("RF channels: %d, AD channels: %d, sample rate: %d\n", h.rfChannels, h.adChannels, h.adSpeed);

    if (h.adChannels != 4) {
        printf("Not a 4-word-per-sample (I1 Q1 I2 Q2) dual-channel file -- stopping.\n");
        fclose(f);
        return 1;
    }

    // Stream through in chunks rather than loading the whole (multi-GB) file at once.
    const size_t CHUNK_SAMPLES = 1 << 20;   // per channel, per chunk
    std::vector<int16_t> buf(CHUNK_SAMPLES * 4);

    double sumSqA = 0.0, sumSqB = 0.0;
    double sumA = 0.0, sumB = 0.0;
    int64_t nonzeroA = 0, nonzeroB = 0;
    int64_t totalSamples = 0;
    int16_t minA = 32767, maxA = -32768, minB = 32767, maxB = -32768;

    // Per-chunk RMS so a "one ADC works for a while then stops" pattern would show up,
    // the same discipline as the live USB captures.
    int chunkIndex = 0;
    printf("\nchunk    samples/ch      RMS A       RMS B     ratio B/A\n");

    while (true) {
        size_t got = fread(buf.data(), sizeof(int16_t), CHUNK_SAMPLES * 4, f);
        size_t samplesInChunk = got / 4;
        if (samplesInChunk == 0) { break; }

        double chunkSqA = 0.0, chunkSqB = 0.0;
        for (size_t i = 0; i < samplesInChunk; i++) {
            const int16_t i1 = buf[i * 4 + 0];
            const int16_t q1 = buf[i * 4 + 1];
            const int16_t i2 = buf[i * 4 + 2];
            const int16_t q2 = buf[i * 4 + 3];

            chunkSqA += (double)i1 * i1 + (double)q1 * q1;
            chunkSqB += (double)i2 * i2 + (double)q2 * q2;
            sumA += i1 + q1;
            sumB += i2 + q2;
            if (i1 != 0 || q1 != 0) { nonzeroA++; }
            if (i2 != 0 || q2 != 0) { nonzeroB++; }
            if (i1 < minA) minA = i1; if (i1 > maxA) maxA = i1;
            if (q1 < minA) minA = q1; if (q1 > maxA) maxA = q1;
            if (i2 < minB) minB = i2; if (i2 > maxB) maxB = i2;
            if (q2 < minB) minB = q2; if (q2 > maxB) maxB = q2;
        }
        sumSqA += chunkSqA;
        sumSqB += chunkSqB;
        totalSamples += (int64_t)samplesInChunk;

        if (chunkIndex % 32 == 0 || samplesInChunk < CHUNK_SAMPLES) {
            const double rmsA = std::sqrt(chunkSqA / samplesInChunk);
            const double rmsB = std::sqrt(chunkSqB / samplesInChunk);
            printf("%5d  %12zu  %10.2f  %10.2f  %8.4f\n", chunkIndex, samplesInChunk, rmsA, rmsB,
                   rmsA > 0 ? rmsB / rmsA : 0.0);
        }
        chunkIndex++;
        if (samplesInChunk < CHUNK_SAMPLES) { break; }   // last (short) chunk
    }
    fclose(f);

    const double rmsA = totalSamples > 0 ? std::sqrt(sumSqA / totalSamples) : 0.0;
    const double rmsB = totalSamples > 0 ? std::sqrt(sumSqB / totalSamples) : 0.0;
    printf("\nTotal samples/channel: %lld (%.1f seconds at %d Sa/s)\n",
           (long long)totalSamples, (double)totalSamples / h.adSpeed, h.adSpeed);
    printf("Channel A (ch1): RMS=%.3f  nonzero=%lld/%lld (%.2f%%)  range=[%d,%d]\n",
           rmsA, (long long)nonzeroA, (long long)totalSamples,
           100.0 * nonzeroA / totalSamples, minA, maxA);
    printf("Channel B (ch2): RMS=%.3f  nonzero=%lld/%lld (%.2f%%)  range=[%d,%d]\n",
           rmsB, (long long)nonzeroB, (long long)totalSamples,
           100.0 * nonzeroB / totalSamples, minB, maxB);

    return 0;
}
