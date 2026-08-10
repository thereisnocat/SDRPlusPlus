#pragma once
#include <string>
#include <fstream>
#include <stdint.h>
#include <mutex>
#include <vector>
#include <array>
#include "riff.h"

namespace wav {    
    #pragma pack(push, 1)
    struct FormatHeader {
        uint16_t codec;
        uint16_t channelCount;
        uint32_t sampleRate;
        uint32_t bytesPerSecond;
        uint16_t bytesPerSample;
        uint16_t bitDepth;
    };
    #pragma pack(pop)

    // FORMAT_WAV (the default) always writes a completely ordinary RIFF/WAVE file, and
    // upgrades it to RF64 automatically -- and only -- if the recording actually grows past
    // what a 32-bit chunk size can hold. Small recordings never pay any RF64 cost or
    // carry any RF64-specific bytes beyond one harmless, spec-legal "JUNK" placeholder
    // chunk any RIFF reader already knows to skip. See RECORDING_REFACTOR_PLAN.md section
    // 3.1 for the exact mechanism (riff::Writer's ds64 placeholder-then-backpatch).
    // FORMAT_RF64 forces the RF64 header from the very first byte regardless of actual
    // size -- not needed for normal recording, but useful for testing the RF64 path
    // without needing to actually generate a multi-gigabyte file.
    enum Format {
        FORMAT_WAV,
        FORMAT_RF64
    };

    enum SampleType {
        SAMP_TYPE_UINT8,
        SAMP_TYPE_INT16,
        SAMP_TYPE_INT32,
        SAMP_TYPE_FLOAT32
    };

    enum Codec {
        CODEC_PCM   = 1,
        CODEC_FLOAT = 3
    };

    class Writer {
    public:
        Writer(int channels = 2, uint64_t samplerate = 48000, Format format = FORMAT_WAV, SampleType type = SAMP_TYPE_INT16);
        ~Writer();

        bool open(std::string path);
        bool isOpen();
        void close();

        void setChannels(int channels);

        // Queue a RIFF chunk to be written between "fmt " and "data". Must be called
        // before open(): open() opens the data chunk immediately after the format chunk,
        // so there is no way to insert one afterwards. Cleared by close().
        void addChunk(const char id[4], const void* data, size_t len);
        void clearChunks();
        void setSamplerate(uint64_t samplerate);
        void setFormat(Format format);
        void setSampleType(SampleType type);

        // Overwrites len bytes at offsetInChunk within a chunk previously added via
        // addChunk(), identified by its id -- e.g. correcting a recording's real stop time
        // in an "auxi" chunk once it's actually known, which is after the chunk had to be
        // queued (addChunk() must run before open()). No-op if no chunk with that id was
        // added, or the file isn't open. Like the RIFF/data chunk sizes this mirrors, only
        // meaningful before close() -- there is nothing left open to seek within afterward.
        void patchChunk(const char id[4], size_t offsetInChunk, const void* data, size_t len);

        size_t getSamplesWritten() { return samplesWritten; }

        void write(float* samples, int count);

    private:
        std::recursive_mutex mtx;
        FormatHeader hdr;
        riff::Writer rw;

        int _channels;
        uint64_t _samplerate;
        Format _format;
        SampleType _type;
        size_t bytesPerSamp;

        std::vector<std::pair<std::array<char, 4>, std::vector<uint8_t>>> extraChunks;
        // Where each added chunk's payload landed once open() actually wrote it, for
        // patchChunk() to seek back into. Empty (and patchChunk() a no-op) until open().
        std::vector<std::pair<std::array<char, 4>, std::streampos>> extraChunkPositions;

        uint8_t* bufU8 = NULL;
        int16_t* bufI16 = NULL;
        int32_t* bufI32 = NULL;
        size_t samplesWritten = 0;
    };
}
