#pragma once

#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <vector>
#include <string>

class WavReader {
public:
    WavReader(std::string path) {
        file = std::ifstream(path.c_str(), std::ios::binary);
        valid = false;

        // Validate RIFF/WAVE header. "RF64" is RIFF's own large-file variant (see
        // RECORDING_REFACTOR_PLAN.md section 3.1) -- same layout otherwise, so accepting
        // it here is the only header-level change RF64 needs.
        char riffSig[4], waveType[4];
        uint32_t riffSize;
        file.read(riffSig, 4);
        file.read((char*)&riffSize, 4);
        file.read(waveType, 4);
        const bool isRF64 = memcmp(riffSig, "RF64", 4) == 0;
        if (!isRF64 && memcmp(riffSig, "RIFF", 4) != 0) { return; }
        if (memcmp(waveType, "WAVE", 4) != 0) { return; }

        // Populated if a real "ds64" chunk is found (RF64 requires it as the first chunk
        // after the header). 0 means either this isn't RF64, or it is but ds64 was missing/
        // malformed -- the actual-file-size fallback below covers that case too, which
        // matters for files that got their 0xFFFFFFFF sentinel patched in by hand rather
        // than written by a real ds64-aware writer.
        uint64_t ds64DataSize = 0;

        // Scan chunks to find "fmt " and "data"
        bool foundFmt = false, foundData = false;
        while (!file.eof()) {
            char chunkId[4];
            uint32_t chunkSize;
            file.read(chunkId, 4);
            if (file.gcount() < 4) { break; }
            file.read((char*)&chunkSize, 4);
            if (file.gcount() < 4) { break; }
            std::streampos chunkDataPos = file.tellg();

            if (memcmp(chunkId, "fmt ", 4) == 0 && !foundFmt) {
                if (chunkSize >= sizeof(FormatHeader)) {
                    file.read((char*)&fmt, sizeof(FormatHeader));
                    foundFmt = true;
                }
            }
            else if (memcmp(chunkId, "ds64", 4) == 0) {
                // riffSize(u64) + dataSize(u64) + sampleCount(u64) + tableLength(u32) = 28
                // bytes minimum; only dataSize is needed here.
                if (chunkSize >= 28) {
                    uint64_t vals[3];
                    file.read((char*)vals, sizeof(vals));
                    ds64DataSize = vals[1];
                }
            }
            else if (memcmp(chunkId, "auxi", 4) == 0 || memcmp(chunkId, "sdpc", 4) == 0) {
                // Metadata chunks are small; keep them so the caller can read the centre
                // frequency and, for dual channel recordings, the channel description.
                if (chunkSize > 0 && chunkSize <= (1u << 16)) {
                    std::vector<uint8_t> buf(chunkSize);
                    file.read((char*)buf.data(), chunkSize);
                    chunks[std::string(chunkId, 4)] = std::move(buf);
                }
            }
            else if (memcmp(chunkId, "data", 4) == 0) {
                _dataOffset = static_cast<size_t>(chunkDataPos);
                // Prefer a real ds64 dataSize when one was found. Otherwise, fall back to
                // actual remaining file size -- covers >4GB files whose 32-bit chunkSize
                // wrapped in a writer that predates RF64 support, and files that had the
                // 0xFFFFFFFF sentinel patched in by hand without a real ds64 chunk to match.
                std::streampos savedPos = file.tellg();
                file.seekg(0, std::ios::end);
                uint64_t fileSize = static_cast<uint64_t>(file.tellg());
                file.seekg(savedPos);
                uint64_t actualSize = fileSize - _dataOffset;
                uint64_t declaredSize = ds64DataSize ? ds64DataSize : static_cast<uint64_t>(chunkSize);
                _dataSize = (actualSize > declaredSize) ? actualSize : declaredSize;
                foundData = true;
                file.seekg(static_cast<std::streamoff>(_dataOffset));
                break;
            }

            // Advance past this chunk; RIFF aligns chunks to 2-byte boundaries. A chunk
            // whose size is itself the RF64 sentinel (only ever "data" in practice, and
            // that case already broke out above before reaching here) has nothing sane to
            // advance by, so leave the read position where it is rather than seek by 4
            // billion bytes.
            if (chunkSize != 0xFFFFFFFFu) {
                uint32_t advance = chunkSize + (chunkSize & 1u);
                file.seekg(static_cast<std::streamoff>(static_cast<size_t>(chunkDataPos) + advance));
            }
        }

        valid = foundFmt && foundData;
    }

    uint16_t getBitDepth() {
        return fmt.bitDepth;
    }

    // 1 = WAVE_FORMAT_PCM, 3 = WAVE_FORMAT_IEEE_FLOAT -- see wav.h's own Codec enum, which
    // the writer populates from the same values. Lets a reader tell PCM and float samples
    // apart from the file itself instead of requiring the user to know and set it by hand.
    uint16_t getCodec() {
        return fmt.codec;
    }

    uint16_t getChannelCount() {
        return fmt.channelCount;
    }

    // Returns NULL if the file carries no such chunk.
    const std::vector<uint8_t>* getChunk(const char* id) const {
        auto it = chunks.find(std::string(id, 4));
        return (it == chunks.end()) ? NULL : &it->second;
    }

    uint32_t getSampleRate() {
        return fmt.sampleRate;
    }

    bool isValid() {
        return valid;
    }

    void readSamples(void* data, size_t size) {
        char* _data = (char*)data;
        file.read(_data, size);
        int read = file.gcount();
        if (read < (int)size) {
            file.clear();
            file.seekg(static_cast<std::streamoff>(_dataOffset));
            file.read(&_data[read], size - read);
        }
        bytesRead += size;
    }

    uint64_t getDataSize() {
        return _dataSize;
    }

    size_t getCurrentByteOffset() {
        std::streampos pos = file.tellg();
        size_t posVal = static_cast<size_t>(pos);
        if (posVal < _dataOffset) { return 0; }
        return posVal - _dataOffset;
    }

    uint32_t getBytesPerSecond() {
        return std::max(fmt.bytesPerSecond, (uint32_t)1);
    }

    void seekToFraction(float fraction) {
        fraction = std::max(0.0f, std::min(1.0f, fraction));
        uint64_t targetByte = static_cast<uint64_t>(static_cast<double>(fraction) * static_cast<double>(_dataSize));
        if (fmt.bytesPerSample > 0) {
            targetByte -= targetByte % fmt.bytesPerSample;
        }
        file.seekg(static_cast<std::streamoff>(_dataOffset + targetByte));
    }

    void rewind() {
        file.seekg(static_cast<std::streamoff>(_dataOffset));
    }

    void close() {
        file.close();
    }

private:
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

    bool valid = false;
    std::ifstream file;
    size_t bytesRead = 0;

    FormatHeader fmt = {};
    std::map<std::string, std::vector<uint8_t>> chunks;
    size_t _dataOffset = 0;
    uint64_t _dataSize = 0;
};