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
    _dcBlocking = dcBlocking;
    _invertIQ = false;

    // split reads preproc's output directly, same as always -- see iq_frontend.h's
    // bindRawIQStream() comment for why rawSplit isn't wired in here unconditionally.
    split.init(preproc.out);
    rawSplit.init(preproc.out);
    currentPreprocOut = preproc.out;

    // Second-channel pre-processing chain: a block-for-block mirror of the primary,
    // starting idle. It gets a real input, and starts, only when setSecondInput() is
    // called (a dual-channel source selected). See iq_frontend.h.
    inBuf2.init(NULL);
    inBuf2.bypass = !buffering;
    decim2.init(NULL, _decimRatio);
    dcBlock2.init(NULL, genDCBlockRate(effectiveSr));
    conjugate2.init(NULL);
    preproc2.init(&inBuf2.out);
    preproc2.addBlock(&decim2, _decimRatio > 1);
    preproc2.addBlock(&dcBlock2, dcBlocking);
    preproc2.addBlock(&conjugate2, false);
    split2.init(preproc2.out);

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

void IQFrontEnd::setSecondInput(dsp::stream<dsp::complex_t>* in) {
    inBuf2.setInput(in);
    if (_hasCh2) { return; }
    _hasCh2 = true;
    // Bring the mirror chain up to the primary's current decim / dc-block / invert state,
    // then start it. (start()/stop() below only touch ch2 while _hasCh2 is set, so a
    // later front-end restart keeps it in step.)
    inBuf2.bypass = inBuf.bypass;
    if (_decimRatio > 1) { decim2.setRatio(_decimRatio); }
    dcBlock2.setRate(genDCBlockRate(effectiveSr));
    preproc2.setBlockEnabled(&decim2, _decimRatio > 1, [=](dsp::stream<dsp::complex_t>* out){ split2.setInput(out); });
    preproc2.setBlockEnabled(&dcBlock2, _dcBlocking, [=](dsp::stream<dsp::complex_t>* out){ split2.setInput(out); });
    preproc2.setBlockEnabled(&conjugate2, _invertIQ, [=](dsp::stream<dsp::complex_t>* out){ split2.setInput(out); });
    preproc2.start();
    split2.start();
    inBuf2.start();
}

void IQFrontEnd::clearSecondInput() {
    if (!_hasCh2) { return; }
    _hasCh2 = false;
    inBuf2.stop();
    split2.stop();
    preproc2.stop();
    // The stale input pointer is harmless while stopped; the next setSecondInput()
    // replaces it before anything reads it again.
}

void IQFrontEnd::setSampleRate(double sampleRate) {
    // Temp stop the necessary blocks
    dcBlock.tempStop();
    if (_hasCh2) { dcBlock2.tempStop(); }
    for (auto& [name, vfo] : vfos) {
        vfo->tempStop();
    }

    // Update the samplerate
    _sampleRate = sampleRate;
    effectiveSr = _sampleRate / _decimRatio;
    dcBlock.setRate(genDCBlockRate(effectiveSr));
    if (_hasCh2) { dcBlock2.setRate(genDCBlockRate(effectiveSr)); }
    for (auto& [name, vfo] : vfos) {
        vfo->setInSamplerate(effectiveSr);
    }

    // Reconfigure the FFT
    updateFFTPath();

    // Restart blocks
    dcBlock.tempStart();
    if (_hasCh2) { dcBlock2.tempStart(); }
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

    if (_hasCh2) { decim2.tempStop(); }

    // Update the decimation ratio
    _decimRatio = ratio;
    if (_decimRatio > 1) { decim.setRatio(_decimRatio); if (_hasCh2) { decim2.setRatio(_decimRatio); } }
    setSampleRate(_sampleRate);

    // Restart the decimator if it was running
    decim.tempStart();
    if (_hasCh2) { decim2.tempStart(); }

    // Enable or disable in the chain
    preproc.setBlockEnabled(&decim, _decimRatio > 1, [=](dsp::stream<dsp::complex_t>* out){ currentPreprocOut = out; rawSplit.setInput(out); if (rawStreams.empty()) { split.setInput(out); } });
    preproc2.setBlockEnabled(&decim2, _decimRatio > 1, [=](dsp::stream<dsp::complex_t>* out){ split2.setInput(out); });

    // Update the DSP sample rate (TODO: Find a way to get rid of this)
    core::setInputSampleRate(_sampleRate);
}

void IQFrontEnd::setDCBlocking(bool enabled) {
    _dcBlocking = enabled;
    preproc.setBlockEnabled(&dcBlock, enabled, [=](dsp::stream<dsp::complex_t>* out){ currentPreprocOut = out; rawSplit.setInput(out); if (rawStreams.empty()) { split.setInput(out); } });
    if (_hasCh2) { preproc2.setBlockEnabled(&dcBlock2, enabled, [=](dsp::stream<dsp::complex_t>* out){ split2.setInput(out); }); }
}

void IQFrontEnd::setInvertIQ(bool enabled) {
    _invertIQ = enabled;
    preproc.setBlockEnabled(&conjugate, enabled, [=](dsp::stream<dsp::complex_t>* out){ currentPreprocOut = out; rawSplit.setInput(out); if (rawStreams.empty()) { split.setInput(out); } });
    if (_hasCh2) { preproc2.setBlockEnabled(&conjugate2, enabled, [=](dsp::stream<dsp::complex_t>* out){ split2.setInput(out); }); }
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

dsp::channel::RxVFO* IQFrontEnd::addVFO(std::string name, double sampleRate, double bandwidth, double offset, bool secondChannel) {
    // Make sure no other VFO with that name already exists
    if (vfos.find(name) != vfos.end()) {
        flog::error("[IQFrontEnd] Tried to add VFO with existing name.");
        return NULL;
    }
    if (secondChannel && !_hasCh2) {
        flog::error("[IQFrontEnd] Tried to add a second-channel VFO with no second channel.");
        return NULL;
    }

    // Create VFO and its input stream
    dsp::stream<dsp::complex_t>* vfoIn = new dsp::stream<dsp::complex_t>;
    dsp::channel::RxVFO* vfo = new dsp::channel::RxVFO(vfoIn, effectiveSr, sampleRate, bandwidth, offset);

    // Register them
    vfoStreams[name] = vfoIn;
    vfos[name] = vfo;
    vfoOnCh2[name] = secondChannel;
    if (secondChannel) { split2.bindStream(vfoIn); }
    else { bindIQStream(vfoIn); }

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

    if (vfoOnCh2.count(name) && vfoOnCh2[name]) { split2.unbindStream(vfoIn); }
    else { unbindIQStream(vfoIn); }
    vfoOnCh2.erase(name);
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

    // Second-channel chain, only if a second input is currently set.
    if (_hasCh2) {
        inBuf2.start();
        preproc2.start();
        split2.start();
    }

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

    if (_hasCh2) {
        inBuf2.stop();
        preproc2.stop();
        split2.stop();
    }

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