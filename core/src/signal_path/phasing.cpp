#include "phasing.h"
#include "signal_path.h"
#include <utils/flog.h>
#include <algorithm>

void Phasing::init() {
    phaser.init(&feedA, &feedB);
    phaser.setMode(dsp::combine::Phaser::MODE_A_ONLY);
    phaser.setWeight(-1000.0f, 0.0f);
    _init = true;
}

Phasing::~Phasing() {
    if (!_init) { return; }
    teardown();
}

void Phasing::setChannelSet(ChannelSet* set) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (!_init) { return; }
    if (set == channels && built == (set != NULL)) { return; }

    teardown();
    channels = set;
    if (channels && channels->count >= 2) { build(); }

    onChannelSetChanged.emit(built);
}

void Phasing::build() {
    if (built || !channels) { return; }

    const int n = channels->count;
    if ((int)channels->streams.size() < n) {
        flog::error("[Phasing] ChannelSet claims {0} channels but supplied {1} streams", n, (int)channels->streams.size());
        return;
    }

    // Keep the selected pair inside the set.
    if (chA >= n) { chA = 0; }
    if (chB >= n) { chB = (n > 1) ? 1 : 0; }
    if (chA == chB) { chB = (chA + 1) % n; }

    // One splitter per channel. Those without anything bound still run, draining their
    // channel so the source's worker is never left blocked on an unread stream.
    splitters.resize(n, NULL);
    for (int i = 0; i < n; i++) {
        splitters[i] = new dsp::routing::Splitter<dsp::complex_t>(channels->streams[i]);
    }

    splitters[chA]->bindStream(&feedA);
    splitters[chB]->bindStream(&feedB);
    applyTaps();

    if (!_perVfo) {
        // Global-combiner mode: the internal Phaser reads feedA/feedB and its output
        // (getOutput()) goes to the front end. In per-VFO mode it stays dormant -- the
        // front end reads feedA/feedB raw via getChannelOutput() and combines per VFO.
        phaser.setSampleRate(sigpath::iqFrontEnd.getSampleRate());
        phaser.reset();
        phaser.start();
    }
    for (int i = 0; i < n; i++) { splitters[i]->start(); }

    built = true;
    flog::info("[Phasing] Built graph for {0} channels ({1} mode), pair {2}/{3}", n,
               _perVfo ? "per-VFO" : "global-combiner", chA, chB);
}

void Phasing::teardown() {
    if (!built) {
        channels = NULL;
        return;
    }

    for (auto& s : splitters) {
        if (s) { s->stop(); }
    }
    phaser.stop();

    for (auto& s : splitters) { delete s; }
    splitters.clear();

    channels = NULL;
    built = false;
}

void Phasing::applyTaps() {
    if (splitters.empty()) { return; }
    for (auto& [channel, streams] : taps) {
        if (channel < 0 || channel >= (int)splitters.size()) { continue; }
        for (auto& s : streams) { splitters[channel]->bindStream(s); }
    }
}

bool Phasing::isActive() {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    return built;
}

dsp::stream<dsp::complex_t>* Phasing::getChannelOutput(int which) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (!built) { return NULL; }
    return which ? &feedB : &feedA;
}

dsp::stream<dsp::complex_t>* Phasing::getOutput() {
    return &phaser.out;
}

int Phasing::getChannelCount() {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    return channels ? channels->count : 0;
}

std::string Phasing::getChannelName(int channel) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (!channels || channel < 0 || channel >= (int)channels->names.size()) { return "?"; }
    return channels->names[channel];
}

bool Phasing::isPhaseCoherent() {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    return channels ? channels->phaseCoherent : false;
}

bool Phasing::isSampleAligned() {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    return channels ? channels->sampleAligned : false;
}

void Phasing::bindChannelStream(int channel, dsp::stream<dsp::complex_t>* stream) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    auto& list = taps[channel];
    if (std::find(list.begin(), list.end(), stream) != list.end()) { return; }
    list.push_back(stream);
    if (built && channel >= 0 && channel < (int)splitters.size()) {
        splitters[channel]->bindStream(stream);
    }
}

void Phasing::unbindChannelStream(int channel, dsp::stream<dsp::complex_t>* stream) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    auto it = taps.find(channel);
    if (it == taps.end()) { return; }
    auto& list = it->second;
    auto sit = std::find(list.begin(), list.end(), stream);
    if (sit == list.end()) { return; }
    list.erase(sit);
    if (built && channel >= 0 && channel < (int)splitters.size()) {
        splitters[channel]->unbindStream(stream);
    }
}

void Phasing::setChannelPair(int a, int b) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (a == chA && b == chB) { return; }

    if (!built) {
        chA = a;
        chB = b;
        return;
    }

    const int n = (int)splitters.size();
    if (a < 0 || a >= n || b < 0 || b >= n || a == b) { return; }

    // Rebinding has to happen with the graph stopped: the splitters and the phaser are
    // running threads blocked on these very streams.
    for (auto& s : splitters) { s->stop(); }
    if (!_perVfo) { phaser.stop(); }

    splitters[chA]->unbindStream(&feedA);
    splitters[chB]->unbindStream(&feedB);
    chA = a;
    chB = b;
    splitters[chA]->bindStream(&feedA);
    splitters[chB]->bindStream(&feedB);

    if (!_perVfo) {
        phaser.reset();
        phaser.start();
    }
    for (auto& s : splitters) { s->start(); }
}

void Phasing::getChannelPair(int& a, int& b) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    a = chA;
    b = chB;
}

void Phasing::setMode(dsp::combine::Phaser::Mode mode) { phaser.setMode(mode); }
dsp::combine::Phaser::Mode Phasing::getMode() { return phaser.getMode(); }
void Phasing::setWeight(float gainDb, float phaseDeg) { phaser.setWeight(gainDb, phaseDeg); }
void Phasing::getWeight(float& gainDb, float& phaseDeg) { phaser.getWeight(gainDb, phaseDeg); }
void Phasing::setDelay(float samples) { phaser.setDelay(samples); }
float Phasing::getDelay() { return phaser.getDelay(); }
void Phasing::setAdaptRate(float rate) { phaser.setAdaptRate(rate); }
float Phasing::getAdaptRate() { return phaser.getAdaptRate(); }
void Phasing::setCovarianceEstimator(bool settle, float forgetting, double settleSeconds) { phaser.setCovarianceEstimator(settle, forgetting, settleSeconds); }
void Phasing::getCovarianceEstimator(bool& settle, float& forgetting, double& settleSeconds) { phaser.getCovarianceEstimator(settle, forgetting, settleSeconds); }
void Phasing::resolveCovariance() { phaser.resolveCovariance(); }
bool Phasing::isCovarianceSettled() { return phaser.isCovarianceSettled(); }
float Phasing::covarianceFillFraction() { return phaser.covarianceFillFraction(); }
void Phasing::setSampleRate(double sampleRate) { phaser.setSampleRate(sampleRate); }
void Phasing::setWideband(bool enabled, int taps) { phaser.setWideband(enabled, taps); }
void Phasing::getWideband(bool& enabled, int& taps) { phaser.getWideband(enabled, taps); }
bool Phasing::isWidebandActive() { return phaser.isWidebandActive(); }
bool Phasing::isWidebandDecorrelating() { return phaser.isWidebandDecorrelating(); }
float Phasing::getWidebandCoherence() { return phaser.getWidebandCoherence(); }
void Phasing::getWidebandActiveBins(int& active, int& total) { phaser.getWidebandActiveBins(active, total); }
void Phasing::setReferenceBand(bool enabled, double offsetHz, double widthHz) { phaser.setReferenceBand(enabled, offsetHz, widthHz); }
void Phasing::getReferenceBand(bool& enabled, double& offsetHz, double& widthHz) { phaser.getReferenceBand(enabled, offsetHz, widthHz); }
void Phasing::captureNoise(double seconds) { phaser.captureNoise(seconds); }
bool Phasing::isCapturingNoise() { return phaser.isCapturingNoise(); }
bool Phasing::hasNoiseReference() { return phaser.hasNoiseReference(); }
void Phasing::clearNoiseReference() { phaser.clearNoiseReference(); }
bool Phasing::getWhitening(dsp::combine::Matrix2& out) { return phaser.getWhitening(out); }
void Phasing::setWhitening(const dsp::combine::Matrix2& w) { phaser.setWhitening(w); }
void Phasing::setWhiteningEnabled(bool enabled) { phaser.setWhiteningEnabled(enabled); }
bool Phasing::getWhiteningEnabled() { return phaser.getWhiteningEnabled(); }
float Phasing::getCoherence() { return phaser.getCoherence(); }
float Phasing::getComponentSeparation() { return phaser.getComponentSeparation(); }
void Phasing::getCombineCoefficients(dsp::complex_t& k0, dsp::complex_t& k1) { phaser.getCombineCoefficients(k0, k1); }
dsp::combine::Phaser::Metrics Phasing::getMetrics() { return phaser.getMetrics(); }
float Phasing::getNullDepth() { return phaser.getNullDepth(); }
bool Phasing::isNullDepthBandLimited() { return phaser.isNullDepthBandLimited(); }
uint64_t Phasing::getDiscardCount() { return phaser.getDiscardCount(); }
