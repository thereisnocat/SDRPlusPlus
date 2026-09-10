#include <signal_path/vfo_manager.h>
#include <signal_path/signal_path.h>
#include <dsp/combine/phaser.h>
#include <gui/gui.h>

VFOManager::VFO::VFO(std::string name, int reference, double offset, double bandwidth, double sampleRate, double minBandwidth, double maxBandwidth, bool bandwidthLocked) {
    this->name = name;
    _bandwidth = bandwidth;
    _sampleRate = sampleRate;
    _offset = offset;
    dspVFO = sigpath::iqFrontEnd.addVFO(name, sampleRate, bandwidth, offset);

    // Stable output: consumers read relayOut for the life of the VFO. relay's input is
    // channel A now, retargeted to the phaser's output if a second channel attaches.
    relay.init(&dspVFO->out);
    relay.bindStream(&relayOut);
    relay.start();
    output = &relayOut;

    wtfVFO = new ImGui::WaterfallVFO;
    wtfVFO->setReference(reference);
    wtfVFO->setBandwidth(bandwidth);
    wtfVFO->setOffset(offset);
    wtfVFO->minBandwidth = minBandwidth;
    wtfVFO->maxBandwidth = maxBandwidth;
    wtfVFO->bandwidthLocked = bandwidthLocked;
    gui::waterfall.vfos[name] = wtfVFO;

    if (sigpath::iqFrontEnd.hasSecondChannel()) { attachSecondChannel(); }
}

VFOManager::VFO::~VFO() {
    detachSecondChannel();
    relay.stop();
    dspVFO->stop();
    gui::waterfall.vfos.erase(name);
    if (gui::waterfall.selectedVFO == name) {
        gui::waterfall.selectFirstVFO();
    }
    sigpath::iqFrontEnd.removeVFO(name);
    delete wtfVFO;
}

void VFOManager::VFO::attachSecondChannel() {
    if (decorr || !sigpath::iqFrontEnd.hasSecondChannel()) { return; }

    dspVFOb = sigpath::iqFrontEnd.addVFO(name + "$b", _sampleRate, _bandwidth, wtfVFO->centerOffset, true);
    if (!dspVFOb) { return; }
    dspVFOb->setPassband(dspVFO->getPassbandLo(), dspVFO->getPassbandHi());

    decorr = new dsp::combine::Phaser();
    decorr->init(&dspVFO->out, &dspVFOb->out);
    decorr->setMode(dsp::combine::Phaser::MODE_A_ONLY); // off by default -- bit-identical to channel A
    decorr->setSampleRate(_sampleRate);
    decorr->reset();

    // Retarget the relay off channel A *before* the phaser starts reading it -- a
    // dsp::stream has single-reader semantics -- then start the phaser.
    relay.setInput(&decorr->out);
    decorr->start();
}

void VFOManager::VFO::detachSecondChannel() {
    if (!decorr) { return; }
    // Stop the phaser first so it is no longer a reader of channel A, then point the relay
    // straight back at channel A.
    decorr->stop();
    relay.setInput(&dspVFO->out);
    delete decorr;
    decorr = NULL;
    sigpath::iqFrontEnd.removeVFO(name + "$b");
    dspVFOb = NULL;
}

void VFOManager::VFO::setOffset(double offset) {
    wtfVFO->setOffset(offset);
    dspVFO->setOffset(wtfVFO->centerOffset);
    if (dspVFOb) { dspVFOb->setOffset(wtfVFO->centerOffset); }
}

double VFOManager::VFO::getOffset() {
    return wtfVFO->generalOffset;
}

void VFOManager::VFO::setCenterOffset(double offset) {
    wtfVFO->setCenterOffset(offset);
    dspVFO->setOffset(offset);
    if (dspVFOb) { dspVFOb->setOffset(offset); }
}

void VFOManager::VFO::setBandwidth(double bandwidth, bool updateWaterfall) {
    if (_bandwidth == bandwidth) { return; }
    _bandwidth = bandwidth;
    if (updateWaterfall) {
        wtfVFO->setBandwidth(bandwidth);
        // Same gap as the one found in setReference() (see its own comment) -- for a
        // REF_LOWER/REF_UPPER VFO (USB/LSB), WaterfallVFO::setBandwidth() correctly recomputes
        // centerOffset to keep the tuned edge (lowerOffset/upperOffset) fixed while the other
        // edge moves, but stops at its own bookkeeping; nothing here was carrying that new
        // centerOffset down to the real demod VFO's actual tuned offset, so growing/shrinking
        // bandwidth this way left the *audio* tuned to the pre-change frequency until something
        // else happened to call setOffset()/setCenterOffset() again. Found live 2026-08-11
        // chasing "dragging the spectrum-preview filter wider than the Bandwidth field doesn't
        // widen the audio" -- growing the passband past the current bandwidth needs to grow
        // `bandwidth` itself to match (see RadioModule::applyPassbandEdges()), which routes
        // through here, which is what actually exposed this. For REF_CENTER VFOs (AM, SAM, ...)
        // this is a no-op change, matching setOffset()'s own idempotent-for-REF_CENTER shape.
        dspVFO->setOffset(wtfVFO->centerOffset);
        if (dspVFOb) { dspVFOb->setOffset(wtfVFO->centerOffset); }
    }
    dspVFO->setBandwidth(bandwidth);
    if (dspVFOb) { dspVFOb->setBandwidth(bandwidth); }
}

void VFOManager::VFO::setPassband(double lo, double hi) {
    dspVFO->setPassband(lo, hi);
    if (dspVFOb) { dspVFOb->setPassband(lo, hi); }
}

double VFOManager::VFO::getPassbandLo() {
    return dspVFO->getPassbandLo();
}

double VFOManager::VFO::getPassbandHi() {
    return dspVFO->getPassbandHi();
}

void VFOManager::VFO::setSampleRate(double sampleRate, double bandwidth) {
    _sampleRate = sampleRate;
    dspVFO->setOutSamplerate(sampleRate, bandwidth);
    if (dspVFOb) { dspVFOb->setOutSamplerate(sampleRate, bandwidth); }
    if (decorr) { decorr->setSampleRate(sampleRate); }
    wtfVFO->setBandwidth(bandwidth);
    // Same missing-resync gap as setBandwidth() above (see its own comment) -- wtfVFO->
    // setBandwidth() can move centerOffset for REF_LOWER/REF_UPPER VFOs, and nothing here
    // propagated that to the real demod VFO's actual tuned offset. Not known to have been
    // hit live through this particular call (selectDemod() always calls applyPassbandEdges()
    // right after, which happens to re-tune things correctly as a side effect), but it's the
    // identical latent bug, so fixing it here too rather than leaving a known duplicate in
    // place.
    dspVFO->setOffset(wtfVFO->centerOffset);
    if (dspVFOb) { dspVFOb->setOffset(wtfVFO->centerOffset); }
}

void VFOManager::VFO::setReference(int ref) {
    wtfVFO->setReference(ref);
}

void VFOManager::VFO::setSnapInterval(double interval) {
    wtfVFO->setSnapInterval(interval);
}

void VFOManager::VFO::setBandwidthLimits(double minBandwidth, double maxBandwidth, bool bandwidthLocked) {
    wtfVFO->minBandwidth = minBandwidth;
    wtfVFO->maxBandwidth = maxBandwidth;
    wtfVFO->bandwidthLocked = bandwidthLocked;
}

bool VFOManager::VFO::getBandwidthChanged(bool erase) {
    bool val = wtfVFO->bandwidthChanged;
    if (erase) { wtfVFO->bandwidthChanged = false; }
    return val;
}

double VFOManager::VFO::getBandwidth() {
    return wtfVFO->bandwidth;
}

int VFOManager::VFO::getReference() {
    return wtfVFO->reference;
}

void VFOManager::VFO::setColor(ImU32 color) {
    wtfVFO->color = color;
}

std::string VFOManager::VFO::getName() {
    return name;
}

VFOManager::VFOManager() {
}

VFOManager::VFO* VFOManager::createVFO(std::string name, int reference, double offset, double bandwidth, double sampleRate, double minBandwidth, double maxBandwidth, bool bandwidthLocked) {
    if (vfos.find(name) != vfos.end() || name == "") {
        return NULL;
    }
    VFOManager::VFO* vfo = new VFO(name, reference, offset, bandwidth, sampleRate, minBandwidth, maxBandwidth, bandwidthLocked);
    vfos[name] = vfo;
    onVfoCreated.emit(vfo);
    return vfo;
}

void VFOManager::deleteVFO(VFOManager::VFO* vfo) {
    std::string name = "";
    for (auto const& [_name, _vfo] : vfos) {
        if (_vfo == vfo) {
            name = _name;
            break;
        }
    }
    if (name == "") {
        return;
    }
    onVfoDelete.emit(vfo);
    vfos.erase(name);
    delete vfo;
    onVfoDeleted.emit(name);
}

void VFOManager::setOffset(std::string name, double offset) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setOffset(offset);
}

double VFOManager::getOffset(std::string name) {
    if (vfos.find(name) == vfos.end()) {
        return 0;
    }
    return vfos[name]->getOffset();
}

void VFOManager::setCenterOffset(std::string name, double offset) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setCenterOffset(offset);
}

void VFOManager::setBandwidth(std::string name, double bandwidth, bool updateWaterfall) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setBandwidth(bandwidth, updateWaterfall);
}

void VFOManager::setSampleRate(std::string name, double sampleRate, double bandwidth) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setSampleRate(sampleRate, bandwidth);
}

void VFOManager::setReference(std::string name, int ref) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setReference(ref);
}

void VFOManager::setBandwidthLimits(std::string name, double minBandwidth, double maxBandwidth, bool bandwidthLocked) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setBandwidthLimits(minBandwidth, maxBandwidth, bandwidthLocked);
}

bool VFOManager::getBandwidthChanged(std::string name, bool erase) {
    if (vfos.find(name) == vfos.end()) {
        return false;
    }
    return vfos[name]->getBandwidthChanged(erase);
}

double VFOManager::getBandwidth(std::string name) {
    if (vfos.find(name) == vfos.end()) {
        return NAN;
    }
    return vfos[name]->getBandwidth();
}

int VFOManager::getReference(std::string name) {
    if (vfos.find(name) == vfos.end()) {
        return -1;
    }
    return vfos[name]->getReference();
}

void VFOManager::setColor(std::string name, ImU32 color) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    return vfos[name]->setColor(color);
}

bool VFOManager::vfoExists(std::string name) {
    return (vfos.find(name) != vfos.end());
}

void VFOManager::updateFromWaterfall(ImGui::WaterFall* wtf) {
    for (auto const& [name, vfo] : vfos) {
        if (vfo->wtfVFO->centerOffsetChanged) {
            vfo->wtfVFO->centerOffsetChanged = false;
            vfo->dspVFO->setOffset(vfo->wtfVFO->centerOffset);
            if (vfo->dspVFOb) { vfo->dspVFOb->setOffset(vfo->wtfVFO->centerOffset); }
        }
    }
}

void VFOManager::refreshSecondChannels() {
    const bool have = sigpath::iqFrontEnd.hasSecondChannel();
    for (auto const& [name, vfo] : vfos) {
        if (have) { vfo->attachSecondChannel(); }
        else { vfo->detachSecondChannel(); }
    }
}