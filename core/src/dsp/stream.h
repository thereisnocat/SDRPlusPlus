#pragma once
#include <string.h>
#include <mutex>
#include <condition_variable>
#include <volk/volk.h>
#include "buffer/buffer.h"

// 1MSample buffer
#define STREAM_BUFFER_SIZE 1000000

namespace dsp {
    class untyped_stream {
    public:
        virtual ~untyped_stream() {}
        virtual bool swap(int size) { return false; }
        virtual int read() { return -1; }
        virtual void flush() {}
        virtual void stopWriter() {}
        virtual void clearWriteStop() {}
        virtual void stopReader() {}
        virtual void clearReadStop() {}
    };

    template <class T>
    class stream : public untyped_stream {
    public:
        stream() {
            writeBuf = buffer::alloc<T>(STREAM_BUFFER_SIZE);
            readBuf = buffer::alloc<T>(STREAM_BUFFER_SIZE);
        }

        virtual ~stream() {
            free();
        }

        virtual void setBufferSize(int samples) {
            buffer::free(writeBuf);
            buffer::free(readBuf);
            writeBuf = buffer::alloc<T>(samples);
            readBuf = buffer::alloc<T>(samples);
        }

        virtual inline bool swap(int size) {
            {
                // Wait to either swap or stop
                std::unique_lock<std::mutex> lck(swapMtx);
                swapCV.wait(lck, [this] { return (canSwap || writerStop); });

                // If writer was stopped, abandon operation
                if (writerStop) { return false; }

                // Swap buffers
                dataSize = size;
                T* temp = writeBuf;
                writeBuf = readBuf;
                readBuf = temp;
                canSwap = false;
            }

            // Notify reader that some data is ready
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                dataReady = true;
            }
            rdyCV.notify_all();

            return true;
        }

        // Non-blocking counterpart to swap(): if the reader hasn't drained the previous
        // buffer yet, returns 0 immediately instead of waiting -- the caller just skips this
        // frame for this consumer rather than stalling. Meant for a low-priority consumer a
        // producer can't afford to ever wait on (see dsp::routing::Splitter's lowPriority
        // path, RECORDING_PERFORMANCE_PLAN.md section 2.1/phase 8): swap() couples how fast
        // *every* writer-side loop iterates to how fast the slowest reader drains, which is
        // exactly the coupling a low-priority consumer must not impose. Writing into writeBuf
        // beforehand is always safe regardless of the outcome here -- the reader only ever
        // touches readBuf, never writeBuf, so a skipped swap just means that write gets
        // overwritten by the next one, not that anything unsafe was touched.
        // Returns: 1 = swapped, 0 = reader still busy (skipped, not an error), -1 = stopped.
        virtual inline int trySwap(int size) {
            {
                std::unique_lock<std::mutex> lck(swapMtx);
                if (writerStop) { return -1; }
                if (!canSwap) { return 0; }

                // Swap buffers
                dataSize = size;
                T* temp = writeBuf;
                writeBuf = readBuf;
                readBuf = temp;
                canSwap = false;
            }

            // Notify reader that some data is ready
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                dataReady = true;
            }
            rdyCV.notify_all();

            return 1;
        }

        virtual inline int read() {
            // Wait for data to be ready or to be stopped
            std::unique_lock<std::mutex> lck(rdyMtx);
            rdyCV.wait(lck, [this] { return (dataReady || readerStop); });

            return (readerStop ? -1 : dataSize);
        }

        virtual inline void flush() {
            // Clear data ready
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                dataReady = false;
            }

            // Notify writer that buffers can be swapped
            {
                std::lock_guard<std::mutex> lck(swapMtx);
                canSwap = true;
            }

            swapCV.notify_all();
        }

        virtual void stopWriter() {
            {
                std::lock_guard<std::mutex> lck(swapMtx);
                writerStop = true;
            }
            swapCV.notify_all();
        }

        virtual void clearWriteStop() {
            writerStop = false;
        }

        virtual void stopReader() {
            {
                std::lock_guard<std::mutex> lck(rdyMtx);
                readerStop = true;
            }
            rdyCV.notify_all();
        }

        virtual void clearReadStop() {
            readerStop = false;
        }

        void free() {
            if (writeBuf) { buffer::free(writeBuf); }
            if (readBuf) { buffer::free(readBuf); }
            writeBuf = NULL;
            readBuf = NULL;
        }

        T* writeBuf;
        T* readBuf;

    private:
        std::mutex swapMtx;
        std::condition_variable swapCV;
        bool canSwap = true;

        std::mutex rdyMtx;
        std::condition_variable rdyCV;
        bool dataReady = false;

        bool readerStop = false;
        bool writerStop = false;

        int dataSize = 0;
    };
}