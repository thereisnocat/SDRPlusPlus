#pragma once
#include <mutex>
#include <fstream>
#include <string>
#include <stack>
#include <stdint.h>
#include <vector>

namespace riff {
#pragma pack(push, 1)
    struct ChunkHeader {
        char id[4];
        uint32_t size;
    };
#pragma pack(pop)

    struct ChunkDesc {
        ChunkHeader hdr;
        std::streampos pos;
        // True, non-wrapping size, tracked alongside hdr.size (which stays exactly what
        // gets written on disk for a normal, non-oversized chunk -- realSize is what lets
        // endChunk() notice when hdr.size would have silently wrapped past its uint32_t
        // ceiling, without changing anything about how ordinary chunks are written. See
        // RECORDING_REFACTOR_PLAN.md section 3.1 for why this exists and the RF64 pattern
        // it implements.
        uint64_t realSize = 0;
    };

    // The RF64 'ds64' chunk this codebase actually needs: riffSize, dataSize, sampleCount,
    // and an always-empty (tableLength = 0) table of any *other* oversized chunk. Nothing
    // in this codebase ever produces a second oversized chunk alongside 'data', so the
    // table is never populated -- writing tableLength = 0 is spec-legal and is exactly what
    // real RF64 writers do in the overwhelmingly common case.
#pragma pack(push, 1)
    struct Ds64Data {
        uint64_t riffSize;
        uint64_t dataSize;
        uint64_t sampleCount;
        uint32_t tableLength;
    };
#pragma pack(pop)
    static_assert(sizeof(Ds64Data) == 28, "ds64 payload must be exactly 28 bytes");

    class Writer {
    public:
        Writer() {}
        // Writer(const Writer&& b);
        ~Writer();

        bool open(std::string path, const char form[4]);
        bool isOpen();
        void close();

        void beginList(const char id[4]);
        void endList();

        void beginChunk(const char id[4]);
        void endChunk();

        void write(const uint8_t* data, size_t len);

        // The file position immediately after the header of whichever chunk is currently
        // open -- i.e. where that chunk's payload starts. Meant to be called right after
        // beginChunk(), so a caller can remember where a specific chunk's content landed
        // and patch part of it later with patchAt(), the same way endChunk() already
        // patches sizes after the fact.
        std::streampos tellp();

        // Overwrites len bytes at an already-written file position, then returns to wherever
        // writing left off -- the same seek-back-and-restore pattern endChunk() already uses
        // for chunk sizes, generalised to arbitrary content. Only meaningful before close();
        // once the file is closed there is nothing open left to seek within.
        void patchAt(std::streampos pos, const void* data, size_t len);

        // WAV-specific info riff::Writer has no way to derive on its own (it doesn't know
        // what a "sample" is, only bytes) but needs for the ds64 chunk's sampleCount field
        // if the file ends up needing RF64. Safe to call any time before close(); the value
        // used is whatever was set most recently. A plain 0 (the default) is spec-legal --
        // ds64 readers that care about sampleCount can always derive it from dataSize and
        // the fmt chunk's own block-align instead.
        void setDs64SampleCount(uint64_t count) { ds64SampleCount = count; }

        // Forces the RF64 conversion at close() regardless of whether any chunk actually
        // grew past the 32-bit ceiling. Not needed for normal recording -- the automatic
        // per-chunk overflow check already upgrades exactly the files that need it -- but
        // useful for exercising the RF64 path in a test without generating a real
        // multi-gigabyte file. Call any time after open().
        void forceRF64() { needsRF64 = true; }

    private:
        void beginRIFF(const char form[4]);
        void endRIFF();

        // Reserves a "ds64"-chunk-sized placeholder -- written as an inert "JUNK" chunk --
        // immediately after the RIFF header, before the caller writes anything else. See
        // RECORDING_REFACTOR_PLAN.md section 3.1: if the file never needs it, it stays
        // exactly that, a completely ordinary, spec-legal chunk any RIFF reader already
        // knows to skip. If it does, close() converts it into a real ds64 chunk and
        // backpatches the outer header to match, rather than the file having committed to
        // one shape or the other from the start.
        void reserveDs64Placeholder();

        std::recursive_mutex mtx;
        std::ofstream file;
        std::stack<ChunkDesc> chunks;

        // Backing storage for file.rdbuf()->pubsetbuf() -- see open()'s own comment
        // (RECORDING_PERFORMANCE_PLAN.md phase 6). Has to outlive the buffer's use by the
        // stream, so it's a member, not a local; sized once (open() only resizes it if still
        // empty) rather than reallocated on every open() of the same long-lived Writer.
        std::vector<char> writeBuf;
        static constexpr size_t WRITE_BUFFER_BYTES = 1 * 1024 * 1024;

        std::streampos ds64PlaceholderPos = 0;
        bool haveDs64Placeholder = false;
        uint64_t ds64SampleCount = 0;

        // Set the moment any chunk's real size stops fitting in its own 32-bit on-disk
        // field (or by forceRF64()). In every real use of this class there is exactly one
        // chunk this can ever be true for -- "data", the only chunk this codebase ever
        // grows past a few kilobytes.
        bool needsRF64 = false;
        // The most recently closed *non-outermost* chunk's true size -- i.e. whichever
        // chunk closed right before the outer RIFF/RF64 chunk itself did. In every real use
        // of this class that is always "data" (the last thing opened, kept open until the
        // very end), so this doubles as "the data chunk's real size" without this class
        // needing to know chunk names or semantics. Updated on every non-outermost
        // endChunk(), not just an oversized one -- forceRF64() needs the real value here
        // even when nothing actually overflowed.
        uint64_t lastChildRealSize = 0;
        // The outer RIFF/RF64 chunk's own true size, captured the moment it closes (i.e.
        // the stack becomes empty in endChunk()) -- needed for ds64's riffSize field.
        uint64_t outerRealSize = 0;
    };

    // class Reader {
    // public:
    //     Reader();
    //     Reader(const Reader&& b);
    //     ~Reader();

    //     bool open(std::string path);
    //     bool isOpen();
    //     void close();

    //     const std::string& form();

    // private:

    //     std::string _form;
    //     std::recursive_mutex mtx;
    //     std::ofstream file;
    // };
}
