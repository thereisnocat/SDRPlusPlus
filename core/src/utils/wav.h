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

        uint8_t* bufU8 = NULL;
        int16_t* bufI16 = NULL;
        int32_t* bufI32 = NULL;
        size_t samplesWritten = 0;
    };
}
