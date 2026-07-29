#pragma once
#include "../math/constants.h"
#include "../types.h"
#include <complex>
#include <cmath>
#include <algorithm>

namespace dsp::combine {

    // Accumulates the correlations an adaptive combiner needs -- sum(A * conj(B)) and
    // sum(|B|^2) -- optionally restricted to a slice of spectrum rather than the whole
    // band. See PHASING_PLAN.md section 2.4.
    //
    // The restriction is the point. Minimising total output power nulls whatever is
    // loudest, which when the DX peaks is the DX. Pointing the adaptation at a stretch of
    // spectrum containing only the interferer -- a carrier, a buzz, an adjacent broadcast
    // the user can see on the waterfall -- lets the weight be solved from the pest alone
    // and then applied to the whole band. It is the thing an SDR can do that an analogue
    // phasing box cannot.
    //
    // Both channels are mixed by the *same* rotation and filtered by the *same* cascade,
    // so their relative phase, which is the entire quantity being measured, is untouched.
    //
    // The band-limiting is two cascaded boxcar decimators rather than a designed filter.
    // A sharp filter at these ratios would need hundreds of taps at the full sample rate;
    // a two-stage cascade costs about one add per sample and gives a triangular response
    // with roughly -26 dB sidelobes. That is ample for weighting the correlation toward a
    // chosen part of the spectrum, which is all this is for -- it is not a channel filter,
    // and nearby strong signals will still pull on the estimate somewhat.
    class RefBand {
    public:
        void configure(double sampleRate, double offsetHz, double widthHz) {
            _sampleRate = sampleRate;

            const double inc = -2.0 * DB_M_PI * offsetHz / std::max(sampleRate, 1.0);
            rotStep = std::polar(1.0, inc);

            // Total decimation sets the passband: the cascade's first null sits at
            // sampleRate/D, so D ~ sampleRate/width puts the nominal width inside it.
            const double w = std::max(widthHz, 1.0);
            int D = (int)std::round(sampleRate / w);
            D = std::clamp(D, 1, 65536);
            len1 = std::min(D, 32);
            len2 = std::max(1, D / std::max(len1, 1));
            reset();
        }

        void reset() {
            rot = { 1.0, 0.0 };
            acc1a = acc1b = { 0.0, 0.0 };
            acc2a = acc2b = { 0.0, 0.0 };
            n1 = n2 = 0;
        }

        // Whole-band correlation: no mixing, no filtering, every sample counts equally.
        // Returns the number of terms accumulated, so a caller averaging across blocks can
        // normalise: the band-limited path yields far fewer terms than samples fed in.
        static int accumulateWideband(const complex_t* a, const complex_t* b, int count,
                                      std::complex<double>& rab, double& rbb, double* raa = NULL) {
            for (int i = 0; i < count; i++) {
                const std::complex<double> av(a[i].re, a[i].im);
                const std::complex<double> bv(b[i].re, b[i].im);
                rab += av * std::conj(bv);
                rbb += std::norm(bv);
                if (raa) { *raa += std::norm(av); }
            }
            return count;
        }

        // Correlation restricted to the configured band. State persists across calls so
        // the decimator does not restart mid-stream.
        // raa, when supplied, collects sum(|A|^2) over the same band. Measuring the null
        // depth inside the reference band is the only honest way to score the result: a
        // weight that cancels the interferer exactly can still raise total power, because
        // it is free to amplify everything else, so a whole-band power ratio marks the
        // correct answer down.
        int accumulate(const complex_t* a, const complex_t* b, int count,
                       std::complex<double>& rab, double& rbb, double* raa = NULL) {
            int terms = 0;
            for (int i = 0; i < count; i++) {
                // One rotation, applied to both channels, so the relative phase survives.
                const std::complex<double> av = std::complex<double>(a[i].re, a[i].im) * rot;
                const std::complex<double> bv = std::complex<double>(b[i].re, b[i].im) * rot;
                rot *= rotStep;

                acc1a += av;
                acc1b += bv;
                if (++n1 < len1) { continue; }

                const std::complex<double> y1a = acc1a / (double)len1;
                const std::complex<double> y1b = acc1b / (double)len1;
                acc1a = acc1b = { 0.0, 0.0 };
                n1 = 0;

                acc2a += y1a;
                acc2b += y1b;
                if (++n2 < len2) { continue; }

                const std::complex<double> y2a = acc2a / (double)len2;
                const std::complex<double> y2b = acc2b / (double)len2;
                acc2a = acc2b = { 0.0, 0.0 };
                n2 = 0;

                rab += y2a * std::conj(y2b);
                rbb += std::norm(y2b);
                if (raa) { *raa += std::norm(y2a); }
                terms++;
            }

            // Keep the mixer on the unit circle.
            const double m = std::abs(rot);
            if (m > 0.0) { rot /= m; }
            return terms;
        }

        int decimation() const { return len1 * len2; }

    private:
        double _sampleRate = 1.0;
        std::complex<double> rot{ 1.0, 0.0 };
        std::complex<double> rotStep{ 1.0, 0.0 };

        std::complex<double> acc1a{ 0.0, 0.0 }, acc1b{ 0.0, 0.0 };
        std::complex<double> acc2a{ 0.0, 0.0 }, acc2b{ 0.0, 0.0 };
        int len1 = 1, len2 = 1;
        int n1 = 0, n2 = 0;
    };
}
