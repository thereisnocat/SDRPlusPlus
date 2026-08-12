#pragma once
#include "../dsp/buffer/frame_buffer.h"
#include "../dsp/buffer/reshaper.h"
#include "../dsp/multirate/power_decimator.h"
#include "../dsp/correction/dc_blocker.h"
#include "../dsp/chain.h"
#include "../dsp/routing/splitter.h"
#include "../dsp/channel/rx_vfo.h"
#include "../dsp/sink/handler_sink.h"
#include "../dsp/math/conjugate.h"
#include <fftw3.h>
#include <algorithm>
#include <set>

class IQFrontEnd {
public:
    ~IQFrontEnd();

    enum FFTWindow {
        RECTANGULAR,
        BLACKMAN,
        NUTTALL
    };

    void init(dsp::stream<dsp::complex_t>* in, double sampleRate, bool buffering, int decimRatio, bool dcBlocking, int fftSize, double fftRate, FFTWindow fftWindow, float* (*acquireFFTBuffer)(void* ctx), void (*releaseFFTBuffer)(void* ctx), void* fftCtx);

    void setInput(dsp::stream<dsp::complex_t>* in);
    void setSampleRate(double sampleRate);
    inline double getSampleRate() { return _sampleRate / _decimRatio; }

    void setBuffering(bool enabled);
    void setDecimation(int ratio);
    void setInvertIQ(bool enabled);
    void setDCBlocking(bool enabled);

    // Changing decimation mid-recording writes samples at a new rate into a file whose
    // header already declared the old one -- the same class of rate-mismatch corruption
    // this whole recording refactor exists to fix, just from a different cause.
    // setDecimation() becomes a no-op while locked. See RECORDING_REFACTOR_PLAN.md
    // section 6.1.
    // Reference-counted for the same reason SourceManager::lockTuning() is: the Recorder
    // module allows unlimited simultaneous instances, so one recording stopping must not
    // unlock decimation out from under a second one still running.
    // Parenthesised (std::max) -- windows.h's max() macro turns an unparenthesised
    // std::max(...) in a header into error C2589 on MSVC. Same trap as bare M_PI; see
    // ENGINEERING_NOTES.md / project memory for the others this has already caught.
    void lockDecimation(bool locked) { decimationLockCount = (std::max)(0, decimationLockCount + (locked ? 1 : -1)); }
    bool isDecimationLocked() const { return decimationLockCount > 0; }

    void bindIQStream(dsp::stream<dsp::complex_t>* stream);
    void unbindIQStream(dsp::stream<dsp::complex_t>* stream);

    // For a consumer that needs the full undecimated-by-this-stage rate but must never
    // compete with the FFT/VFO copies for time on split's own thread -- the recorder's
    // baseband tap is the motivating case. See RECORDING_PERFORMANCE_PLAN.md phase 12: at a
    // high enough sample rate, split's own per-iteration memcpy cost across all its bound
    // consumers -- proportional to (bound consumer count) x (sample rate), regardless of how
    // fast any one of them drains -- became a real throughput ceiling on its single thread
    // once the recorder's tap was one of the consumers sharing it, throttling delivery to
    // every consumer including live audio's VFO.
    //
    // rawSplit -- a second Splitter, its own dedicated thread -- is inserted between preproc
    // and split ONLY while at least one raw consumer is bound, and removed the instant the
    // last one unbinds (see the .cpp): split reads preproc's output directly at ordinary
    // times, exactly as before this feature existed, and only pays the cost of an extra
    // full-rate copy stage while something is actually using it. Phase 12's first version of
    // this wired rawSplit in unconditionally, which regressed the *no-recording* case by
    // permanently doubling copy volume even when nothing needed it -- see phase 13.
    void bindRawIQStream(dsp::stream<dsp::complex_t>* stream);
    void unbindRawIQStream(dsp::stream<dsp::complex_t>* stream);

    dsp::channel::RxVFO* addVFO(std::string name, double sampleRate, double bandwidth, double offset);
    void removeVFO(std::string name);

    void setFFTSize(int size);
    void setFFTRate(double rate);
    void setFFTWindow(FFTWindow fftWindow);

    void flushInputBuffer();

    void start();
    void stop();

    double getEffectiveSamplerate();

protected:
    static void handler(dsp::complex_t* data, int count, void* ctx);
    void updateFFTPath(bool updateWaterfall = false);

    static inline double genDCBlockRate(double sampleRate) {
        return 50.0 / sampleRate;
    }

    static inline void genReshapeParams(double sampleRate, int size, double rate, int& skip, int& nzSampCount) {
        int fftInterval = round(sampleRate / rate);
        nzSampCount = std::min<int>(fftInterval, size);
        skip = fftInterval - nzSampCount;
    }

    // Input buffer
    dsp::buffer::SampleFrameBuffer<dsp::complex_t> inBuf;

    // Pre-processing chain
    dsp::multirate::PowerDecimator<dsp::complex_t> decim;
    dsp::math::Conjugate conjugate;
    dsp::correction::DCBlocker<dsp::complex_t> dcBlock;
    dsp::chain<dsp::complex_t> preproc;

    // Splitting. split normally reads preproc's output directly, same as always. rawSplit and
    // mainStream only come into play while bindRawIQStream() has at least one consumer bound
    // -- see that method's declaration comment above and its definition in the .cpp.
    dsp::routing::Splitter<dsp::complex_t> split;
    dsp::routing::Splitter<dsp::complex_t> rawSplit;
    dsp::stream<dsp::complex_t> mainStream;
    std::set<dsp::stream<dsp::complex_t>*> rawStreams;
    // Tracks preproc's real current output stream so unbindRawIQStream() can point split back
    // at it directly once the last raw consumer is gone, without needing a public getter on
    // Sink<T>/block for a protected member (_in) that nothing else needs exposed.
    dsp::stream<dsp::complex_t>* currentPreprocOut = NULL;

    // FFT
    dsp::stream<dsp::complex_t> fftIn;
    dsp::buffer::Reshaper<dsp::complex_t> reshape;
    dsp::sink::Handler<dsp::complex_t> fftSink;

    // VFOs
    std::map<std::string, dsp::stream<dsp::complex_t>*> vfoStreams;
    std::map<std::string, dsp::channel::RxVFO*> vfos;

    // Parameters
    double _sampleRate;
    double _decimRatio;
    int decimationLockCount = 0;
    int _fftSize;
    double _fftRate;
    FFTWindow _fftWindow;
    float* (*_acquireFFTBuffer)(void* ctx);
    void (*_releaseFFTBuffer)(void* ctx);
    void* _fftCtx;

    // Processing data
    int _nzFFTSize;
    float* fftWindowBuf;
    fftwf_complex *fftInBuf, *fftOutBuf;
    fftwf_plan fftwPlan;
    float* fftDbOut;

    double effectiveSr;

    bool _init = false;

};