#pragma once
#include <string>
#include <vector>
#include <map>
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

    void selectSource(std::string name);
    void showSelectedMenu();
    void start();
    void stop();
    void tune(double freq);
    void setTuningOffset(double offset);
    void setTuningMode(TuningMode mode);
    void setPanadapterIF(double freq);

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
};