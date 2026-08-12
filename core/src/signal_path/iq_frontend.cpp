#include "iq_frontend.h"
#include "../dsp/window/blackman.h"
#include "../dsp/window/nuttall.h"
#include <utils/flog.h>
#include <gui/gui.h>
#include <core.h>

IQFrontEnd::~IQFrontEnd() {
    if (!_init) { return; }
    stop();
    dsp::buffer::free(fftWindowBuf);
    fftwf_destroy_plan(fftwPlan);
    fftwf_free(fftInBuf);
    fftwf_free(fftOutBuf);
}

void IQFrontEnd::init(dsp::stream<dsp::complex_t>* in, double sampleRate, bool buffering, int decimRatio, bool dcBlocking, int fftSize, double fftRate, FFTWindow fftWindow, float* (*acquireFFTBuffer)(void* ctx), void (*releaseFFTBuffer)(void* ctx), void* fftCtx) {
    _sampleRate = sampleRate;
    _decimRatio = decimRatio;
    _fftSize = fftSize;
    _fftRate = fftRate;
    _fftWindow = fftWindow;
    _acquireFFTBuffer = acquireFFTBuffer;
    _releaseFFTBuffer = releaseFFTBuffer;
    _fftCtx = fftCtx;

    effectiveSr = _sampleRate / _decimRatio;

    inBuf.init(in);
    inBuf.bypass = !buffering;

    decim.init(NULL, _decimRatio);
    dcBlock.init(NULL, genDCBlockRate(effectiveSr));
    conjugate.init(NULL);

    preproc.init(&inBuf.out);
    preproc.addBlock(&decim, _decimRatio > 1);
    preproc.addBlock(&dcBlock, dcBlocking);
    preproc.addBlock(&conjugate, false); // TODO: Replace by parameter

    // split reads preproc's output directly, same as always -- see iq_frontend.h's
    // bindRawIQStream() comment for why rawSplit isn't wired in here unconditionally.
    split.init(preproc.out);
    rawSplit.init(preproc.out);
    currentPreprocOut = preproc.out;

    // TODO: Do something to avoid basically repeating this code twice
    int skip;
    genReshapeParams(effectiveSr, _fftSize, _fftRate, skip, _nzFFTSize);
    reshape.init(&fftIn, fftSize, skip);
    fftSink.init(&reshape.out, handler, this);

    fftWindowBuf = dsp::buffer::alloc<float>(_nzFFTSize);
    if (_fftWindow == FFTWindow::RECTANGULAR) {
        for (int i = 0; i < _nzFFTSize; i++) { fftWindowBuf[i] = 0; }
    }
    else if (_fftWindow == FFTWindow::BLACKMAN) {
        for (int i = 0; i < _nzFFTSize; i++) { fftWindowBuf[i] = dsp::window::blackman(i, _nzFFTSize); }
    }
    else if (_fftWindow == FFTWindow::NUTTALL) {
        for (int i = 0; i < _nzFFTSize; i++) { fftWindowBuf[i] = dsp::window::nuttall(i, _nzFFTSize); }
    }

    fftInBuf = (fftwf_complex*)fftwf_malloc(_fftSize * sizeof(fftwf_complex));
    fftOutBuf = (fftwf_complex*)fftwf_malloc(_fftSize * sizeof(fftwf_complex));
    fftwPlan = fftwf_plan_dft_1d(_fftSize, fftInBuf, fftOutBuf, FFTW_FORWARD, FFTW_ESTIMATE);

    // Clear the rest of the FFT input buffer
    dsp::buffer::clear(fftInBuf, _fftSize - _nzFFTSize, _nzFFTSize);

    // Low-priority: a slow spectrum/waterfall FFT must never make VFOs or the recorder
    // wait behind it for their own copy of a frame -- see splitter.h's bindStream() doc
    // and RECORDING_PERFORMANCE_PLAN.md section 2.1.
    split.bindStream(&fftIn, true);

    _init = true;
}

void IQFrontEnd::setInput(dsp::stream<dsp::complex_t>* in) {
    inBuf.setInput(in);
}

void IQFrontEnd::setSampleRate(double sampleRate) {
    // Temp stop the necessary blocks
    dcBlock.tempStop();
    for (auto& [name, vfo] : vfos) {
        vfo->tempStop();
    }

    // Update the samplerate
    _sampleRate = sampleRate;
    effectiveSr = _sampleRate / _decimRatio;
    dcBlock.setRate(genDCBlockRate(effectiveSr));
    for (auto& [name, vfo] : vfos) {
        vfo->setInSamplerate(effectiveSr);
    }

    // Reconfigure the FFT
    updateFFTPath();

    // Restart blocks
    dcBlock.tempStart();
    for (auto& [name, vfo] : vfos) {
        vfo->tempStart();
    }
}

void IQFrontEnd::setBuffering(bool enabled) {
    inBuf.bypass = !enabled;
}

void IQFrontEnd::setDecimation(int ratio) {
    if (isDecimationLocked()) {
        flog::warn("IQFrontEnd: setDecimation() ignored, locked for an active recording");
        return;
    }

    // Temp stop the decimator
    decim.tempStop();

    // Update the decimation ratio
    _decimRatio = ratio;
    if (_decimRatio > 1) { decim.setRatio(_decimRatio); }
    setSampleRate(_sampleRate);

    // Restart the decimator if it was running
    decim.tempStart();

    // Enable or disable in the chain
    preproc.setBlockEnabled(&decim, _decimRatio > 1, [=](dsp::stream<dsp::complex_t>* out){ currentPreprocOut = out; rawSplit.setInput(out); if (rawStreams.empty()) { split.setInput(out); } });

    // Update the DSP sample rate (TODO: Find a way to get rid of this)
    core::setInputSampleRate(_sampleRate);
}

void IQFrontEnd::setDCBlocking(bool enabled) {
    preproc.setBlockEnabled(&dcBlock, enabled, [=](dsp::stream<dsp::complex_t>* out){ currentPreprocOut = out; rawSplit.setInput(out); if (rawStreams.empty()) { split.setInput(out); } });
}

void IQFrontEnd::setInvertIQ(bool enabled) {
    preproc.setBlockEnabled(&conjugate, enabled, [=](dsp::stream<dsp::complex_t>* out){ currentPreprocOut = out; rawSplit.setInput(out); if (rawStreams.empty()) { split.setInput(out); } });
}

void IQFrontEnd::bindIQStream(dsp::stream<dsp::complex_t>* stream) {
    split.bindStream(stream);
}

void IQFrontEnd::unbindIQStream(dsp::stream<dsp::complex_t>* stream) {
    split.unbindStream(stream);
}

void IQFrontEnd::bindRawIQStream(dsp::stream<dsp::complex_t>* stream) {
    if (rawStreams.empty()) {
        // First raw consumer: insert rawSplit between preproc and split now, not before --
        // see iq_frontend.h's comment on this method for why it isn't wired in
        // unconditionally.
        //
        // Order matters here in a way it's easy to get backwards: split must stop reading
        // preproc.out (via setInput(), below) *before* rawSplit ever starts reading it --
        // dsp::stream<T> has strictly single-reader semantics (one shared dataReady/canSwap
        // pair, not tracked per-reader), so any window where both are simultaneously live
        // readers of the same stream races on that internal state, and can leave one of them
        // stuck waiting on a signal the other already consumed. Retargeting split to
        // mainStream first means its thread just blocks harmlessly on an empty mainStream
        // for the brief moment until rawSplit starts feeding it -- no crash, no race. The
        // reverse order here (rawSplit started before split let go of preproc.out) was
        // exactly phase 13's bug -- see RECORDING_PERFORMANCE_PLAN.md phase 14: it's what
        // caused "stopping recording stops all audio" (unbindRawIQStream() had the same
        // ordering mistake, the other direction).
        split.setInput(&mainStream);
        rawSplit.bindStream(&mainStream);
        rawSplit.start();
    }
    rawSplit.bindStream(stream);
    rawStreams.insert(stream);
}

void IQFrontEnd::unbindRawIQStream(dsp::stream<dsp::complex_t>* stream) {
    rawSplit.unbindStream(stream);
    rawStreams.erase(stream);
    if (rawStreams.empty()) {
        // Last raw consumer gone: tear the indirection back down so split goes right back to
        // reading preproc's real output directly, at zero extra copy cost -- exactly the
        // no-recording case, unaffected by this feature ever having existed.
        //
        // Mirrors bindRawIQStream()'s ordering fix: rawSplit.stop() joins its worker thread
        // synchronously (dsp::block::doStop()), so by the time it returns rawSplit is
        // *guaranteed* no longer reading preproc.out -- only then is it safe for split to
        // start reading it again via setInput() below.
        rawSplit.stop();
        rawSplit.unbindStream(&mainStream);
        split.setInput(currentPreprocOut);
    }
}

dsp::channel::RxVFO* IQFrontEnd::addVFO(std::string name, double sampleRate, double bandwidth, double offset) {
    // Make sure no other VFO with that name already exists
    if (vfos.find(name) != vfos.end()) {
        flog::error("[IQFrontEnd] Tried to add VFO with existing name.");
        return NULL;
    }

    // Create VFO and its input stream
    dsp::stream<dsp::complex_t>* vfoIn = new dsp::stream<dsp::complex_t>;
    dsp::channel::RxVFO* vfo = new dsp::channel::RxVFO(vfoIn, effectiveSr, sampleRate, bandwidth, offset);

    // Register them
    vfoStreams[name] = vfoIn;
    vfos[name] = vfo;
    bindIQStream(vfoIn);

    // Start VFO
    vfo->start();

    return vfo;
}

void IQFrontEnd::removeVFO(std::string name) {
    // Make sure that a VFO with that name exists
    if (vfos.find(name) == vfos.end()) {
        flog::error("[IQFrontEnd] Tried to remove a VFO that doesn't exist.");
        return;
    }

    // Remove the VFO and stream from registry
    dsp::stream<dsp::complex_t>* vfoIn = vfoStreams[name];
    dsp::channel::RxVFO* vfo = vfos[name];

    // Stop the VFO
    vfo->stop();

    unbindIQStream(vfoIn);
    vfoStreams.erase(name);
    vfos.erase(name);

    // Delete the VFO and its input stream
    delete vfo;
    delete vfoIn;
}

void IQFrontEnd::setFFTSize(int size) {
    _fftSize = size;
    updateFFTPath(true);
}

void IQFrontEnd::setFFTRate(double rate) {
    _fftRate = rate;
    updateFFTPath();
}

void IQFrontEnd::setFFTWindow(FFTWindow fftWindow) {
    _fftWindow = fftWindow;
    updateFFTPath();
}

void IQFrontEnd::flushInputBuffer() {
    inBuf.flush();
}

void IQFrontEnd::start() {
    // Start input buffer
    inBuf.start();

    // Start pre-proc chain (automatically start all bound blocks)
    preproc.start();

    // Only if a raw consumer was already bound going into this start() (e.g. the front end
    // was restarted -- a source switch -- while a recording was active): restore rawSplit's
    // insertion. The ordinary case is rawStreams empty here, in which case rawSplit stays
    // idle and split reads preproc's output directly, same as always.
    if (!rawStreams.empty()) { rawSplit.start(); }

    // Start IQ splitter
    split.start();

    // Start all VFOs
    for (auto& [name, vfo] : vfos) {
        vfo->start();
    }

    // Start FFT chain
    reshape.start();
    fftSink.start();
}

void IQFrontEnd::stop() {
    // Stop input buffer
    inBuf.stop();

    // Stop pre-proc chain (automatically start all bound blocks)
    preproc.stop();

    // Only stop rawSplit if it's actually running (a raw consumer currently bound) -- see
    // start()'s matching comment.
    if (!rawStreams.empty()) { rawSplit.stop(); }

    // Stop IQ splitter
    split.stop();

    // Stop all VFOs
    for (auto& [name, vfo] : vfos) {
        vfo->stop();
    }

    // Stop FFT chain
    reshape.stop();
    fftSink.stop();
}

double IQFrontEnd::getEffectiveSamplerate() {
    return effectiveSr;
}

void IQFrontEnd::handler(dsp::complex_t* data, int count, void* ctx) {
    IQFrontEnd* _this = (IQFrontEnd*)ctx;

    // Apply window
    volk_32fc_32f_multiply_32fc((lv_32fc_t*)_this->fftInBuf, (lv_32fc_t*)data, _this->fftWindowBuf, _this->_nzFFTSize);

    // Execute FFT
    fftwf_execute(_this->fftwPlan);

    // Aquire buffer
    float* fftBuf = _this->_acquireFFTBuffer(_this->_fftCtx);

    // Convert the complex output of the FFT to dB amplitude
    if (fftBuf) {
        volk_32fc_s32f_power_spectrum_32f(fftBuf, (lv_32fc_t*)_this->fftOutBuf, _this->_fftSize, _this->_fftSize);
    }

    // Release buffer
    _this->_releaseFFTBuffer(_this->_fftCtx);
}

void IQFrontEnd::updateFFTPath(bool updateWaterfall) {
    // Temp stop branch
    reshape.tempStop();
    fftSink.tempStop();

    // Update reshaper settings
    int skip;
    genReshapeParams(effectiveSr, _fftSize, _fftRate, skip, _nzFFTSize);
    reshape.setKeep(_nzFFTSize);
    reshape.setSkip(skip);

    // Update window
    dsp::buffer::free(fftWindowBuf);
    fftWindowBuf = dsp::buffer::alloc<float>(_nzFFTSize);
    if (_fftWindow == FFTWindow::RECTANGULAR) {
        for (int i = 0; i < _nzFFTSize; i++) { fftWindowBuf[i] = 1.0f * ((i % 2) ? -1.0f : 1.0f); }
    }
    else if (_fftWindow == FFTWindow::BLACKMAN) {
        for (int i = 0; i < _nzFFTSize; i++) { fftWindowBuf[i] = dsp::window::blackman(i, _nzFFTSize) * ((i % 2) ? -1.0f : 1.0f); }
    }
    else if (_fftWindow == FFTWindow::NUTTALL) {
        for (int i = 0; i < _nzFFTSize; i++) { fftWindowBuf[i] = dsp::window::nuttall(i, _nzFFTSize) * ((i % 2) ? -1.0f : 1.0f); }
    }

    // Update FFT plan
    fftwf_free(fftInBuf);
    fftwf_free(fftOutBuf);
    fftInBuf = (fftwf_complex*)fftwf_malloc(_fftSize * sizeof(fftwf_complex));
    fftOutBuf = (fftwf_complex*)fftwf_malloc(_fftSize * sizeof(fftwf_complex));
    fftwPlan = fftwf_plan_dft_1d(_fftSize, fftInBuf, fftOutBuf, FFTW_FORWARD, FFTW_ESTIMATE);

    // Clear the rest of the FFT input buffer
    dsp::buffer::clear(fftInBuf, _fftSize - _nzFFTSize, _nzFFTSize);

    // Update waterfall (TODO: This is annoying, it makes this module non testable and will constantly clear the waterfall for any reason)
    if (updateWaterfall) { gui::waterfall.setRawFFTSize(_fftSize); }

    // Restart branch
    reshape.tempStart();
    fftSink.tempStart();
}