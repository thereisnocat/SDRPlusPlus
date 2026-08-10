#pragma once
#include "pll.h"

namespace dsp::loop {
    class CarrierTrackingPLL : public PLL {
        using base_type = PLL;
    public:
        CarrierTrackingPLL() {}

        CarrierTrackingPLL(stream<complex_t>* in, double bandwidth, double initPhase = 0.0, double initFreq = 0.0, double minFreq = -FL_M_PI, double maxFreq = FL_M_PI) {
            base_type::init(in, bandwidth, initFreq, initPhase, minFreq, maxFreq);
        }

        inline int process(int count, complex_t* in, complex_t* out) {
            for (int i = 0; i < count; i++) {
                out[i] = in[i] * math::phasor(-pcl.phase);
                lastError = math::normalizePhase(in[i].phase() - pcl.phase);
                pcl.advance(lastError);
            }
            return count;
        }

        // Phase error (radians) measured on the most recently processed sample -- the same
        // value process() already folds into the loop above, just kept around for callers that
        // want a lock-quality readout (e.g. Synchronous AM's "carrier locked" indicator). Purely
        // informational: reading it has no effect on the loop.
        inline float getLastError() const { return lastError; }

    protected:
        float lastError = 0.0f;
    };
}