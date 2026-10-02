#pragma once
#include "../sink.h"

namespace dsp::routing {
    template <class T>
    class Splitter : public Sink<T> {
        using base_type = Sink<T>;
    public:
        Splitter() {}

        Splitter(stream<T>* in) { base_type::init(in); }

        // lowPriority: this consumer's swap() is done *after* every normal-priority
        // consumer's, so a slow low-priority consumer (a display/spectrum feed, where a
        // late frame is a non-issue) can never delay delivery to the real-time-sensitive
        // ones (a live VFO/audio path, a recorder). It does NOT make this consumer fully
        // non-blocking -- run() as a whole, and therefore how soon the *next* input frame
        // is read at all, still waits on it, so sustained backpressure here can still ripple
        // upstream. That's the real fix (see RECORDING_PERFORMANCE_PLAN.md section 2.1,
        // still open) this is a stopgap for: it only protects normal-priority consumers
        // against having to wait *behind* this one within the same frame, which is what was
        // actually producing audible live-audio clicks -- see the fftIn caller in
        // core/src/signal_path/iq_frontend.cpp.
        void bindStream(stream<T>* stream, bool lowPriority = false) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);

            // Check that the stream isn't already bound
            if (std::find(streams.begin(), streams.end(), stream) != streams.end() ||
                std::find(lowPriorityStreams.begin(), lowPriorityStreams.end(), stream) != lowPriorityStreams.end()) {
                throw std::runtime_error("[Splitter] Tried to bind stream to that is already bound");
            }

            // Add to the appropriate list
            base_type::tempStop();
            base_type::registerOutput(stream);
            (lowPriority ? lowPriorityStreams : streams).push_back(stream);
            base_type::tempStart();
        }

        // A tap is a plain callback run on this Splitter's own thread, after every bound
        // stream has been served, with the frame still in the input's read buffer (copy it,
        // don't keep the pointer). It exists for the one consumer that needs *every*
        // sample, in order, but must never make anyone else wait: the spectrum/waterfall
        // feed. A low-priority bound stream can't give it that -- see the comment above
        // bindStream()'s lowPriority -- because a stream is a single-slot hand-off, so a
        // skipped frame is a hole in the sample sequence. At ~4500 frames/s (the RSR200's
        // 340-sample USB packets) a reader thread that is merely a quarter of a millisecond
        // late to wake -- routine on Windows -- turns a large share of frames into holes,
        // and an FFT window stitched across holes splatters every strong carrier into a
        // smooth skirt tens of kHz wide that is not in the signal at all. A tap instead
        // copies the frame into the consumer's own buffer right here (a memcpy, no
        // wake-up on the critical path) and lets the consumer drain it whenever it gets to
        // it. The callback must be quick and must never block.
        using tap_handler_t = void (*)(const T* data, int count, void* ctx);

        void bindTap(tap_handler_t handler, void* ctx) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            taps.push_back({ handler, ctx });
            base_type::tempStart();
        }

        void unbindTap(tap_handler_t handler, void* ctx) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            for (auto it = taps.begin(); it != taps.end(); ++it) {
                if (it->handler == handler && it->ctx == ctx) {
                    taps.erase(it);
                    break;
                }
            }
            base_type::tempStart();
        }

        void unbindStream(stream<T>* stream) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);

            // Check that the stream is bound, in either list
            auto sit = std::find(streams.begin(), streams.end(), stream);
            auto lit = (sit == streams.end()) ? std::find(lowPriorityStreams.begin(), lowPriorityStreams.end(), stream) : lowPriorityStreams.end();
            if (sit == streams.end() && lit == lowPriorityStreams.end()) {
                throw std::runtime_error("[Splitter] Tried to unbind stream to that isn't bound");
            }

            // Remove from whichever list it was found in
            base_type::tempStop();
            if (sit != streams.end()) { streams.erase(sit); }
            else { lowPriorityStreams.erase(lit); }
            base_type::unregisterOutput(stream);
            base_type::tempStart();
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            // Normal-priority consumers first, so none of them can ever be made to wait
            // behind a low-priority one for their copy of this frame.
            for (const auto& stream : streams) {
                memcpy(stream->writeBuf, base_type::_in->readBuf, count * sizeof(T));
                if (!stream->swap(count)) {
                    base_type::_in->flush();
                    return -1;
                }
            }

            // trySwap(), not swap(): a low-priority consumer that's still behind on the
            // previous frame just gets this one skipped, rather than making run() -- and
            // therefore every normal-priority consumer's next frame -- wait on it. That
            // coupling (run() as a whole still gating on a blocking swap() here) is exactly
            // what made the bind-order-only version of this fix insufficient at sustained
            // high sample rates -- see RECORDING_PERFORMANCE_PLAN.md phase 8's follow-up.
            for (const auto& stream : lowPriorityStreams) {
                memcpy(stream->writeBuf, base_type::_in->readBuf, count * sizeof(T));
                if (stream->trySwap(count) < 0) {
                    base_type::_in->flush();
                    return -1;
                }
            }

            // Taps last, so they can never delay a consumer that is waiting on a frame.
            for (const auto& tap : taps) {
                tap.handler(base_type::_in->readBuf, count, tap.ctx);
            }

            base_type::_in->flush();

            return count;
        }

    protected:
        struct Tap {
            tap_handler_t handler;
            void* ctx;
        };
        std::vector<stream<T>*> streams;
        std::vector<stream<T>*> lowPriorityStreams;
        std::vector<Tap> taps;

    };
}
