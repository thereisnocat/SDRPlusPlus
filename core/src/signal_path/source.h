#pragma once
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <utils/event.h>
#include "channel_set.h"

class SourceManager {
public:
    SourceManager();

    struct SourceHandler {
        dsp::stream<dsp::complex_t>* stream;
        void (*menuHandler)(void* ctx);
        void (*selectHandler)(void* ctx);
        void (*deselectHandler)(void* ctx);
        void (*startHandler)(void* ctx);
        void (*stopHandler)(void* ctx);
        void (*tuneHandler)(double freq, void* ctx);
        void* ctx;
    };

    enum TuningMode {
        NORMAL,
        PANADAPTER
    };

    void registerSource(std::string name, SourceHandler* handler);
    void unregisterSource(std::string name);

    // Declare that a source can offer several coherent channels for phasing. Additive
    // and optional: a source that never calls this behaves exactly as it always has.
    // The set must stay valid until unregistered, and because registering rebuilds the
    // signal path, changes should be made while the source is stopped.
    void registerChannels(const std::string& name, ChannelSet* set);
    void unregisterChannels(const std::string& name);
    ChannelSet* getChannels(const std::string& name);

    // Name of the currently selected source, empty if none. Lets a module key its
    // settings per radio rather than sharing one set across all of them.
    const std::string& getSelectedName() const { return selectedName; }

    void selectSource(std::string name);
    void showSelectedMenu();
    void start();
    void stop();
    void tune(double freq);
    void setTuningOffset(double offset);
    void setTuningMode(TuningMode mode);
    void setPanadapterIF(double freq);

    // Retuning the source mid-recording changes what's actually being sampled partway
    // through a baseband file whose header already committed to a single centre frequency
    // -- a real recording-corrupting action, not a cosmetic one, for anything wideband
    // enough that "centre frequency" means the RF front end's own tuning rather than just
    // where a VFO sits within an already-fixed passband. tune() (and therefore
    // setTuningOffset()/setTuningMode(), which both call it) becomes a no-op while locked,
    // rather than leaving every caller -- GUI, rigctl, the network server -- to remember to
    // check this themselves. See RECORDING_REFACTOR_PLAN.md section 6.1.
    //
    // Reference-counted, not a flag: the Recorder module allows unlimited simultaneous
    // instances, so one recording stopping must not unlock tuning out from under a second
    // one that's still running. lockTuning(true)/lockTuning(false) is still call/release in
    // pairs from each caller's point of view -- only the internal representation cares that
    // more than one caller might hold it at once.
    void lockTuning(bool locked) { tuningLockCount = std::max(0, tuningLockCount + (locked ? 1 : -1)); }
    bool isTuningLocked() const { return tuningLockCount > 0; }

    std::vector<std::string> getSourceNames();

    Event<std::string> onSourceRegistered;
    Event<std::string> onSourceUnregister;
    Event<std::string> onSourceUnregistered;
    Event<std::string> onChannelsRegistered;
    Event<std::string> onChannelsUnregistered;
    Event<double> onRetune;

private:
    // Point the IQ front end at either the phaser's output or the source's own stream,
    // depending on whether the selected source currently offers channels.
    void updateInput();

    std::map<std::string, SourceHandler*> sources;
    std::map<std::string, ChannelSet*> channelSets;
    std::string selectedName;
    SourceHandler* selectedHandler = NULL;
    double tuneOffset;
    double currentFreq;
    double ifFreq = 0.0;
    TuningMode tuneMode = TuningMode::NORMAL;
    dsp::stream<dsp::complex_t> nullSource;
    int tuningLockCount = 0;
};