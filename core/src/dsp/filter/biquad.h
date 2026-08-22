#pragma once
#include <cmath>

// RBJ Audio EQ Cookbook biquad, Direct Form I. Originally written for dsp::filter::TubeWarmth
// (see that file's own comment on why a general-purpose framework wasn't worth pulling in for
// one fixed peaking filter and one fixed low-pass) and pulled out to here, 2026-08-20, once
// dsp::filter::ParametricEQ needed the same math plus two more filter types (shelves) --
// shared now that there are genuinely two callers, not written speculatively ahead of need.
//
// Coefficients are only ever recomputed when a setXxx() call configures the filter, never
// per-sample -- process() is nothing but the difference equation and four state variables.

namespace dsp::filter {

    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

        inline float process(float x) {
            float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }

        void reset() { x1 = x2 = y1 = y2 = 0.0f; }

        void setPeaking(double f0, double q, double gainDb, double sampleRate) {
            const double A = std::pow(10.0, gainDb / 40.0);
            const double w0 = 2.0 * M_PI_CONST * f0 / sampleRate;
            const double alpha = std::sin(w0) / (2.0 * q);
            const double cosw0 = std::cos(w0);

            const double a0 = 1.0 + alpha / A;
            b0 = (float)((1.0 + alpha * A) / a0);
            b1 = (float)((-2.0 * cosw0) / a0);
            b2 = (float)((1.0 - alpha * A) / a0);
            a1 = (float)((-2.0 * cosw0) / a0);
            a2 = (float)((1.0 - alpha / A) / a0);
        }

        void setLowPass(double fc, double q, double sampleRate) {
            const double w0 = 2.0 * M_PI_CONST * fc / sampleRate;
            const double alpha = std::sin(w0) / (2.0 * q);
            const double cosw0 = std::cos(w0);

            const double a0 = 1.0 + alpha;
            b0 = (float)(((1.0 - cosw0) / 2.0) / a0);
            b1 = (float)((1.0 - cosw0) / a0);
            b2 = b0;
            a1 = (float)((-2.0 * cosw0) / a0);
            a2 = (float)((1.0 - alpha) / a0);
        }

        // Boosts/cuts everything *below* f0 by gainDb, leaving well above f0 untouched --
        // ParametricEQ's own bottom band, matching how a real tone control's bass knob works.
        void setLowShelf(double f0, double q, double gainDb, double sampleRate) {
            const double A = std::pow(10.0, gainDb / 40.0);
            const double w0 = 2.0 * M_PI_CONST * f0 / sampleRate;
            const double alpha = std::sin(w0) / (2.0 * q);
            const double cosw0 = std::cos(w0);
            const double sqrtA = std::sqrt(A);
            const double twoSqrtAAlpha = 2.0 * sqrtA * alpha;

            const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAAlpha;
            b0 = (float)((A * ((A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAAlpha)) / a0);
            b1 = (float)((2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0)) / a0);
            b2 = (float)((A * ((A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAAlpha)) / a0);
            a1 = (float)((-2.0 * ((A - 1.0) + (A + 1.0) * cosw0)) / a0);
            a2 = (float)(((A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAAlpha) / a0);
        }

        // Boosts/cuts everything *above* f0 by gainDb, leaving well below f0 untouched --
        // ParametricEQ's own top band.
        void setHighShelf(double f0, double q, double gainDb, double sampleRate) {
            const double A = std::pow(10.0, gainDb / 40.0);
            const double w0 = 2.0 * M_PI_CONST * f0 / sampleRate;
            const double alpha = std::sin(w0) / (2.0 * q);
            const double cosw0 = std::cos(w0);
            const double sqrtA = std::sqrt(A);
            const double twoSqrtAAlpha = 2.0 * sqrtA * alpha;

            const double a0 = (A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAAlpha;
            b0 = (float)((A * ((A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAAlpha)) / a0);
            b1 = (float)((-2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0)) / a0);
            b2 = (float)((A * ((A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAAlpha)) / a0);
            a1 = (float)((2.0 * ((A - 1.0) - (A + 1.0) * cosw0)) / a0);
            a2 = (float)(((A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAAlpha) / a0);
        }

    private:
        // Not DB_M_PI from dsp/math/constants.h -- that header is meant for the dsp namespace's
        // float/double *sample-domain* constants and pulling it in here just for one value
        // would add a dependency this standalone-ish header doesn't otherwise need. Same bare-
        // M_PI MSVC trap it exists to avoid, though (MSVC's <cmath> only defines M_PI with
        // _USE_MATH_DEFINES set before the include), so this file carries its own, exactly the
        // comment in rsr200_protocol.h gives for doing the same thing there.
        static constexpr double M_PI_CONST = 3.14159265358979323846;
    };

}
