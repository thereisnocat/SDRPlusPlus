#pragma once
#include "../processor.h"
#include "../loop/agc.h"
#include "../loop/carrier_tracking_pll.h"
#include "../correction/dc_blocker.h"
#include "../convert/mono_to_stereo.h"
#include "../filter/fir.h"
#include "../taps/low_pass.h"

namespace dsp::demod {
    // Synchronous AM: a carrier-tracking PLL locks onto the transmitted carrier and derotates
    // the signal so the carrier sits on the real axis, then the in-phase component is used
    // directly as the demodulated audio instead of the |signal| envelope regular AM uses.
    // Unlike envelope detection, this stays coherent through selective fading (a common
    // shortwave/mediumwave problem where the carrier and one sideband fade independently and
    // briefly invert phase relative to each other, which envelope detection hears as a sharp
    // burst of distortion) and tolerates the receiver being slightly mistuned, since the PLL
    // pulls the residual offset in instead of beating against it. Structurally this mirrors
    // dsp::demod::AM closely -- same AGC/DC-block/LPF chain -- with the derotating PLL taking
    // the place of the envelope detector.
    template <class T>
    class SAM : public Processor<dsp::complex_t, T> {
        using base_type = Processor<dsp::complex_t, T>;
    public:
        enum AGCMode {
            CARRIER,
            AUDIO,
        };

        SAM() {}

        SAM(stream<complex_t>* in, AGCMode agcMode, double bandwidth, double agcAttack, double agcDecay, double dcBlockRate, double pllBandwidth, double pllCaptureRange, double samplerate) {
            init(in, agcMode, bandwidth, agcAttack, agcDecay, dcBlockRate, pllBandwidth, pllCaptureRange, samplerate);
        }

        ~SAM() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            taps::free(lpfTaps);
        }

        void init(stream<complex_t>* in, AGCMode agcMode, double bandwidth, double agcAttack, double agcDecay, double dcBlockRate, double pllBandwidth, double pllCaptureRange, double samplerate) {
            _agcMode = agcMode;
            _bandwidth = bandwidth;
            _samplerate = samplerate;

            carrierAgc.init(NULL, 1.0, agcAttack, agcDecay, 10e6, 10.0, INFINITY);
            audioAgc.init(NULL, 1.0, agcAttack, agcDecay, 10e6, 10.0, INFINITY);
            pll.init(NULL, pllBandwidth, 0.0, 0.0, -pllCaptureRange, pllCaptureRange);
            dcBlock.init(NULL, dcBlockRate);
            lpfTaps = taps::lowPass(bandwidth / 2.0, (bandwidth / 2.0) * 0.1, samplerate);
            lpf.init(NULL, lpfTaps);

            if constexpr (std::is_same_v<T, float>) {
                audioAgc.out.free();
            }
            dcBlock.out.free();
            lpf.out.free();

            base_type::init(in);
        }

        void setAGCMode(AGCMode agcMode) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _agcMode = agcMode;
            reset();
            base_type::tempStart();
        }

        void setBandwidth(double bandwidth) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            if (bandwidth == _bandwidth) { return; }
            _bandwidth = bandwidth;
            std::lock_guard<std::mutex> lck2(lpfMtx);
            taps::free(lpfTaps);
            lpfTaps = taps::lowPass(_bandwidth / 2.0, (_bandwidth / 2.0) * 0.1, _samplerate);
            lpf.setTaps(lpfTaps);
        }

        void setAGCAttack(double attack) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            carrierAgc.setAttack(attack);
            audioAgc.setAttack(attack);
        }

        void setAGCDecay(double decay) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            carrierAgc.setDecay(decay);
            audioAgc.setDecay(decay);
        }

        void setDCBlockRate(double rate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dcBlock.setRate(rate);
        }

        void setPLLBandwidth(double bandwidth) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            pll.setBandwidth(bandwidth);
        }

        // TODO: Implement setSamplerate

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            carrierAgc.reset();
            audioAgc.reset();
            pll.reset();
            dcBlock.reset();
            base_type::tempStart();
        }

        // Approximate lock indicator: radians of phase error on the most recently processed
        // sample. Small and stable once the loop has captured the carrier; large and noisy
        // while still searching or if there's no carrier to lock to. This is a single raw
        // sample, not a block average -- callers displaying it (e.g. a UI indicator) are
        // expected to smooth it themselves.
        float getLockError() { return pll.getLastError(); }

        int process(int count, complex_t* in, T* out) {
            // Apply carrier AGC if needed
            if (_agcMode == AGCMode::CARRIER) {
                carrierAgc.process(count, in, carrierAgc.out.writeBuf);
                in = carrierAgc.out.writeBuf;
            }

            if constexpr (std::is_same_v<T, float>) {
                pll.process(count, in, pll.out.writeBuf);
                for (int i = 0; i < count; i++) { out[i] = pll.out.writeBuf[i].re; }
                dcBlock.process(count, out, out);
                if (_agcMode == AGCMode::AUDIO) {
                    audioAgc.process(count, out, out);
                }
                {
                    std::lock_guard<std::mutex> lck(lpfMtx);
                    lpf.process(count, out, out);
                }
            }
            if constexpr (std::is_same_v<T, stereo_t>) {
                pll.process(count, in, pll.out.writeBuf);
                for (int i = 0; i < count; i++) { audioAgc.out.writeBuf[i] = pll.out.writeBuf[i].re; }
                dcBlock.process(count, audioAgc.out.writeBuf, audioAgc.out.writeBuf);
                if (_agcMode == AGCMode::AUDIO) {
                    audioAgc.process(count, audioAgc.out.writeBuf, audioAgc.out.writeBuf);
                }
                {
                    std::lock_guard<std::mutex> lck(lpfMtx);
                    lpf.process(count, audioAgc.out.writeBuf, audioAgc.out.writeBuf);
                }
                convert::MonoToStereo::process(count, audioAgc.out.writeBuf, out);
            }

            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    protected:
        AGCMode _agcMode;

        double _samplerate;
        double _bandwidth;

        loop::AGC<complex_t> carrierAgc;
        loop::AGC<float> audioAgc;
        loop::CarrierTrackingPLL pll;
        correction::DCBlocker<float> dcBlock;
        tap<float> lpfTaps;
        filter::FIR<float, float> lpf;
        std::mutex lpfMtx;

    };
}
