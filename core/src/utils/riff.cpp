#include "riff.h"
#include <string.h>
#include <stdexcept>

namespace riff {
    const char* RIFF_SIGNATURE      = "RIFF";
    const char* RF64_SIGNATURE      = "RF64";
    const char* DS64_SIGNATURE      = "ds64";
    const char* JUNK_SIGNATURE      = "JUNK";
    const char* LIST_SIGNATURE      = "LIST";
    const size_t RIFF_LABEL_SIZE    = 4;
    const uint32_t SIZE64_SENTINEL  = 0xFFFFFFFFu;

    // Writer::Writer(const Writer&& b) {
    //     //file = std::move(b.file);
    // }

    Writer::~Writer() {
        close();
    }

    bool Writer::open(std::string path, const char form[4]) {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        // Open file
        file = std::ofstream(path, std::ios::out | std::ios::binary);
        if (!file.is_open()) { return false; }

        // Begin RIFF chunk
        beginRIFF(form);

        // Reserve room for a ds64 chunk immediately, before the caller writes anything
        // else -- RF64 requires ds64 to be the first chunk after the header, and there is
        // no way to insert it there later once fmt/data have been written after it.
        reserveDs64Placeholder();

        return true;
    }

    bool Writer::isOpen() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        return file.is_open();
    }

    void Writer::close() {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        if (!isOpen()) { return; }

        // Finalize RIFF chunk. This also correctly writes the 0xFFFFFFFF size sentinel for
        // the outer chunk itself if anything inside it (in practice, always "data") turned
        // out to be oversized -- endChunk()'s overflow check applies uniformly to every
        // chunk including this one, not just its children.
        endRIFF();

        // If anything was oversized, the file is still, at this point, an ordinary RIFF/
        // WAVE file with a too-small size field lying about its own length and a "JUNK"
        // chunk sitting where "ds64" needs to go. Convert it: rewrite the outer magic and
        // its size sentinel, and turn the placeholder into a real ds64 chunk with the true
        // sizes. A file that never exceeded the 32-bit ceiling never reaches this branch --
        // it stays exactly the same completely ordinary WAV file it always would have been,
        // "JUNK" chunk and all, which every RIFF reader already knows to skip.
        if (needsRF64 && haveDs64Placeholder) {
            // "RIFF" -> "RF64"
            file.seekp(0);
            file.write(RF64_SIGNATURE, RIFF_LABEL_SIZE);

            // ds64's own payload: riffSize is the *outer* chunk's real size (same "total
            // file size minus the 8-byte RIFF header" convention the classic 32-bit RIFF
            // size field already uses, just not truncated); dataSize is the last child
            // chunk to close before the outer one did, which in every real use of this
            // class is "data" -- using lastChildRealSize rather than only-set-on-overflow
            // bookkeeping means this comes out right for forceRF64() too, when nothing
            // actually overflowed but a real ds64.dataSize is still needed.
            Ds64Data ds64{};
            ds64.riffSize = outerRealSize;
            ds64.dataSize = lastChildRealSize;
            ds64.sampleCount = ds64SampleCount;
            ds64.tableLength = 0;

            file.seekp(ds64PlaceholderPos);
            file.write(DS64_SIGNATURE, RIFF_LABEL_SIZE);
            uint32_t ds64Size = (uint32_t)sizeof(Ds64Data);
            file.write((char*)&ds64Size, sizeof(ds64Size));
            file.write((char*)&ds64, sizeof(ds64));
        }

        // Close file
        file.close();
    }

    void Writer::beginList(const char id[4]) {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        // Create chunk with the LIST ID and write id
        beginChunk(LIST_SIGNATURE);
        write((uint8_t*)id, RIFF_LABEL_SIZE);
    }

    void Writer::endList() {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        if (chunks.empty()) {
            throw std::runtime_error("No chunk to end");
        }
        if (memcmp(chunks.top().hdr.id, LIST_SIGNATURE, RIFF_LABEL_SIZE)) {
            throw std::runtime_error("Top chunk not LIST chunk");
        }

        endChunk();
    }

    void Writer::beginChunk(const char id[4]) {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        // Create and write header
        ChunkDesc desc;
        desc.pos = file.tellp();
        memcpy(desc.hdr.id, id, sizeof(desc.hdr.id));
        desc.hdr.size = 0;
        desc.realSize = 0;
        file.write((char*)&desc.hdr, sizeof(ChunkHeader));

        // Save descriptor
        chunks.push(desc);
    }

    void Writer::endChunk() {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        if (chunks.empty()) {
            throw std::runtime_error("No chunk to end");
        }

        // Get descriptor
        ChunkDesc desc = chunks.top();
        chunks.pop();

        // The chunk still open once this pop empties the stack is the outermost one
        // (RIFF/RF64 itself); anything else closing is its most recent child, which in
        // every real use of this class is "data" (see lastChildRealSize's declaration).
        const bool isOuter = chunks.empty();

        // The on-disk 32-bit size field: the real value if it fits, or the RF64 escape
        // sentinel (0xFFFFFFFF) if it doesn't. Applying the overflow check the same way to
        // every chunk, not just "data" specifically, is what makes a naturally oversized
        // "data" chunk correctly force the outer chunk's own field to the sentinel too --
        // its realSize has propagated everything its children accumulated by the time it
        // closes, so the exact same check applies there.
        //
        // The outer chunk gets one more condition ORed in: needsRF64 already being true
        // when *it* closes, regardless of whether its own realSize happens to be huge.
        // Once the file has committed to the "RF64" magic at all -- whether because a
        // child genuinely overflowed, or because forceRF64() said so outright -- the outer
        // size field is supposed to consistently mean "see ds64," not sometimes hold a real
        // small value. Without this, forceRF64() on a small file (exactly the case the
        // round-trip tests use to exercise this path without a real multi-gigabyte file)
        // would produce an outer chunk with "RF64" magic but a non-sentinel size field --
        // spec-inconsistent, and not what a strict external reader would expect.
        const bool naturallyOversized = desc.realSize >= (uint64_t)SIZE64_SENTINEL;
        const bool oversized = naturallyOversized || (isOuter && needsRF64);
        const uint32_t onDiskSize = oversized ? SIZE64_SENTINEL : (uint32_t)desc.realSize;
        if (naturallyOversized) {
            needsRF64 = true;
        }
        // Remembered unconditionally, not just on overflow, since forceRF64() needs real
        // values here too.
        if (isOuter) {
            outerRealSize = desc.realSize;
        }
        else {
            lastChildRealSize = desc.realSize;
        }

        // Write size
        auto pos = file.tellp();
        auto npos = desc.pos;
        npos += 4;
        file.seekp(npos);
        file.write((char*)&onDiskSize, sizeof(onDiskSize));
        file.seekp(pos);

        // If parent chunk, propagate the *real* size up, not the on-disk one -- otherwise
        // an oversized child's 0xFFFFFFFF sentinel would get added into the parent's count
        // as if it were 4 billion real bytes.
        if (!chunks.empty()) {
            chunks.top().realSize += desc.realSize + sizeof(ChunkHeader);
        }
    }

    void Writer::write(const uint8_t* data, size_t len) {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        if (chunks.empty()) {
            throw std::runtime_error("No chunk to write into");
        }
        file.write((char*)data, len);
        chunks.top().realSize += len;
    }

    std::streampos Writer::tellp() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        return file.tellp();
    }

    void Writer::patchAt(std::streampos pos, const void* data, size_t len) {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!file.is_open()) { return; }
        auto cur = file.tellp();
        file.seekp(pos);
        file.write((const char*)data, len);
        file.seekp(cur);
    }

    void Writer::beginRIFF(const char form[4]) {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        if (!chunks.empty()) {
            throw std::runtime_error("Can't create RIFF chunk on an existing RIFF file");
        }

        // Create chunk with RIFF ID and write form
        beginChunk(RIFF_SIGNATURE);
        write((uint8_t*)form, RIFF_LABEL_SIZE);
    }

    void Writer::endRIFF() {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        if (chunks.empty()) {
            throw std::runtime_error("No chunk to end");
        }
        if (memcmp(chunks.top().hdr.id, RIFF_SIGNATURE, RIFF_LABEL_SIZE)) {
            throw std::runtime_error("Top chunk not RIFF chunk");
        }

        endChunk();
    }

    void Writer::reserveDs64Placeholder() {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        ds64PlaceholderPos = file.tellp();
        Ds64Data blank{};
        beginChunk(JUNK_SIGNATURE);
        write((const uint8_t*)&blank, sizeof(blank));
        endChunk();
        haveDs64Placeholder = true;
    }
}
