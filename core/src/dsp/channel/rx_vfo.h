#pragma once
#include "frequency_xlator.h"
#include "../multirate/rational_resampler.h"
#include "../taps/band_pass.h"
#include <algorithm>

namespace dsp::channel {
    class RxVFO : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;
    public:
        RxVFO() {}

        RxVFO(stream<complex_t>* in, double inSamplerate, double outSamplerate, double bandwidth, double offset) { init(in, inSamplerate, outSamplerate, bandwidth, offset); }

        ~RxVFO() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            taps::free(ftaps);
        }

        void init(stream<complex_t>* in, double inSamplerate, double outSamplerate, double bandwidth, double offset) {
            _inSamplerate = inSamplerate;
            _outSamplerate = outSamplerate;
            _bandwidth = bandwidth;
            _passbandLo = -bandwidth / 2.0;
            _passbandHi = bandwidth / 2.0;
            _offset = offset;
            filterNeeded = (_bandwidth != _outSamplerate);
            ftaps.taps = NULL;

            xlator.init(NULL, -_offset, _inSamplerate);
            resamp.init(NULL, _inSamplerate, _outSamplerate);
            generateTaps();
            filter.init(NULL, ftaps);

            base_type::init(in);
        }

        void setInSamplerate(double inSamplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _inSamplerate = inSamplerate;
            xlator.setOffset(-_offset, _inSamplerate);
            resamp.setInSamplerate(_inSamplerate);
            base_type::tempStart();
        }

        void setOutSamplerate(double outSamplerate, double bandwidth) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _outSamplerate = outSamplerate;
            _bandwidth = bandwidth;
            // The sample rate is the one thing that's a genuine hard physical ceiling on the
            // passband -- can't filter wider than what's actually there -- so this is the one
            // change that still has to re-clamp. See clampPassband()'s comment for why
            // *bandwidth* changes deliberately no longer do this.
            clampPassband();
            filterNeeded = passbandTrimmed() || (_bandwidth != _outSamplerate);
            resamp.setOutSamplerate(_outSamplerate);
            if (filterNeeded) {
                generateTaps();
                filter.setTaps(ftaps);
            }
            base_type::tempStart();
        }

        void setBandwidth(double bandwidth) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            std::lock_guard<std::mutex> lck2(filterMtx);
            _bandwidth = bandwidth;
            // Deliberately does NOT touch _passbandLo/_passbandHi or re-clamp them -- bandwidth
            // is bookkeeping (drives filterNeeded's fast-path check below, and whatever the
            // caller uses getBandwidth()-equivalent info for) and, now, whatever a UI caller
            // wants to keep in sync with the passband it's separately managing, not a second
            // ceiling the passband must live inside. See clampPassband()'s comment.
            filterNeeded = passbandTrimmed() || (_bandwidth != _outSamplerate);
            if (filterNeeded) {
                generateTaps();
                filter.setTaps(ftaps);
            }
        }

        // Independent low/high passband edges (Hz, offsets from the tuned center; lo <= 0 <=
        // hi), for filters asymmetric around the carrier -- e.g. to trim just the side facing
        // an adjacent channel without narrowing the whole passband. Clamped only against the
        // true hard limit -- the sample rate itself (see clampPassband()) -- not against
        // "bandwidth"; passing the default -bandwidth/2..+bandwidth/2 here is exactly
        // equivalent to never calling this at all, including keeping the "no filter needed"
        // fast path modes like RAW rely on (bandwidth == outSamplerate) when the passband
        // isn't actually trimmed.
        void setPassband(double lo, double hi) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            std::lock_guard<std::mutex> lck2(filterMtx);
            _passbandLo = lo;
            _passbandHi = hi;
            clampPassband();
            filterNeeded = passbandTrimmed() || (_bandwidth != _outSamplerate);
            if (filterNeeded) {
                generateTaps();
                filter.setTaps(ftaps);
            }
        }

        double getPassbandLo() { return _passbandLo; }
        double getPassbandHi() { return _passbandHi; }

        void setOffset(double offset) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _offset = offset;
            xlator.setOffset(-_offset, _inSamplerate);
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            xlator.reset();
            resamp.reset();
            filter.reset();
            base_type::tempStart();
        }

        inline int process(int count, const complex_t* in, complex_t* out) {
            xlator.process(count, in, out);
            if (!filterNeeded) {
                return resamp.process(count, out, out);
            }
            count = resamp.process(count, out, out);
            {
                std::lock_guard<std::mutex> lck(filterMtx);
                filter.process(count, out, out);
            }
            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int outCount = process(count, base_type::_in->readBuf, out.writeBuf);

            // Swap if some data was generated
            base_type::_in->flush();
            if (outCount) {
                if (!out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

    protected:
        // A symmetric passband (lo == -bandwidth/2, hi == bandwidth/2) makes bandPass()
        // degenerate to exactly the same real-valued-in-a-complex-wrapper coefficients the old
        // plain taps::lowPass() call here produced -- same cutoff, same zero imaginary part on
        // every tap (offsetOmega is exactly 0 when the band is symmetric around DC) -- so this
        // one call covers both cases with no separate symmetric-only fast path needed. The
        // actual filtered *values* are therefore unchanged for any caller that never sets an
        // asymmetric passband; what does change for everyone is the filter now always running
        // as complex-tap FIR rather than real-tap, since one shared filter member has to be
        // able to handle both -- a modest, unavoidable fixed CPU cost of the feature existing
        // at all, independent of whether it's actually in use.
        void generateTaps() {
            taps::free(ftaps);
            // Parenthesised (std::max) -- windows.h's own max() macro turns an unparenthesised
            // std::max(...) into MSVC error C2589. Same trap as bare M_PI; see the fix for
            // iq_frontend.h's lockDecimation() for the full explanation.
            double width = (std::max)(_passbandHi - _passbandLo, MIN_PASSBAND_WIDTH_HZ);
            double transWidth = (std::max)(width * 0.1, MIN_PASSBAND_WIDTH_HZ / 2.0);
            ftaps = taps::bandPass<complex_t>(_passbandLo, _passbandHi, transWidth, _outSamplerate);
        }

        bool passbandTrimmed() const {
            return _passbandLo != -_bandwidth / 2.0 || _passbandHi != _bandwidth / 2.0;
        }

        // Clamps _passbandLo/_passbandHi against the true hard limit -- the sample rate itself,
        // since nothing can filter wider than what's actually there -- and enforces a minimum
        // gap between them (estimateTapCount() is inversely proportional to transition width,
        // so an unbounded near-zero-width passband, e.g. a UI drag with both edges pulled
        // together, could otherwise ask generateTaps() for an enormous filter).
        //
        // Deliberately *not* clamped against _bandwidth. An earlier version was, on the theory
        // that "bandwidth" was the outer window and passband was a trim within it -- but
        // "bandwidth" is a single symmetric scalar (width, not independent edges), so any
        // caller wanting a genuinely asymmetric passband (lo and hi not equidistant from 0, the
        // whole point of this feature) would immediately get re-clamped back towards symmetric
        // the moment anything touched _bandwidth again -- which in practice meant every edit to
        // the UI's numeric Bandwidth field silently threw away whatever asymmetric trim was in
        // effect. The sample rate is a real, unavoidable ceiling; "bandwidth" turned out to be
        // an artificial second one this feature has no reason to respect.
        void clampPassband() {
            double halfSr = _outSamplerate / 2.0;
            _passbandLo = std::clamp(_passbandLo, -halfSr, 0.0);
            _passbandHi = std::clamp(_passbandHi, 0.0, halfSr);
            // If the whole sample rate is narrower than the minimum, there's no room to enforce
            // it separately -- just use the full width (still narrower than
            // MIN_PASSBAND_WIDTH_HZ, but that's a hard physical limit of how little bandwidth
            // is available, not something clamping harder here could fix).
            if (2.0 * halfSr <= MIN_PASSBAND_WIDTH_HZ) {
                _passbandLo = -halfSr;
                _passbandHi = halfSr;
                return;
            }
            if (_passbandHi - _passbandLo < MIN_PASSBAND_WIDTH_HZ) {
                double mid = std::clamp((_passbandLo + _passbandHi) / 2.0, -halfSr + MIN_PASSBAND_WIDTH_HZ / 2.0, halfSr - MIN_PASSBAND_WIDTH_HZ / 2.0);
                _passbandLo = mid - MIN_PASSBAND_WIDTH_HZ / 2.0;
                _passbandHi = mid + MIN_PASSBAND_WIDTH_HZ / 2.0;
            }
        }

        static constexpr double MIN_PASSBAND_WIDTH_HZ = 50.0;

        FrequencyXlator xlator;
        multirate::RationalResampler<complex_t> resamp;
        filter::FIR<complex_t, complex_t> filter;
        tap<complex_t> ftaps;
        bool filterNeeded;

        double _inSamplerate;
        double _outSamplerate;
        double _bandwidth;
        double _passbandLo;
        double _passbandHi;
        double _offset;

        std::mutex filterMtx;
    };
}