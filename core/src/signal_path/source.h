#pragma once
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <json.hpp>
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

        // Optional, additive -- both null by default, so every existing source module keeps
        // compiling and behaving identically without any change on their part. Lets an
        // external module (the recording scheduler -- RECORDING_SCHEDULER_PLAN.md section 2.2)
        // capture a snapshot of this source's current settings and re-apply it later, without
        // knowing anything about that source's own field names/semantics. There is no reliable
        // way to get this "for free" from a module's own persisted config: re-selecting a
        // source (selectHandler above) does not reload its settings from disk in any module
        // checked so far (RECORDING_SCHEDULER_PLAN.md section 2.2's survey, corrected
        // 2026-08-15 -- this was first thought to work for some modules and does not).
        //
        // captureConfigHandler returns an opaque snapshot of the source's current live
        // settings (empty json{} if unimplemented). applyConfigHandler applies a
        // previously-captured snapshot back -- must be safe to call whether or not this source
        // is currently the selected/running one (a scheduled apply may target a radio that
        // isn't active right now), so implementations must not assume they can safely touch
        // anything beyond their own live fields and config file -- e.g. not call
        // core::setInputSampleRate() unless first confirming they're actually the selected
        // source.
        nlohmann::json (*captureConfigHandler)(void* ctx) = nullptr;
        void (*applyConfigHandler)(const nlohmann::json& cfg, void* ctx) = nullptr;
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

    // Capture/apply a named source's settings via its optional captureConfigHandler/
    // applyConfigHandler (above) -- works regardless of whether `name` is the currently
    // selected source. captureSourceConfig returns empty json{} for a nonexistent source or
    // one that doesn't implement capture; applySourceConfig is a no-op in both those cases.
    nlohmann::json captureSourceConfig(const std::string& name);
    void applySourceConfig(const std::string& name, const nlohmann::json& cfg);

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
    // Parenthesised (std::max) -- windows.h's max() macro turns an unparenthesised
    // std::max(...) in a header into error C2589 on MSVC. Same trap as bare M_PI; see
    // ENGINEERING_NOTES.md / project memory for the others this has already caught.
    void lockTuning(bool locked) { tuningLockCount = (std::max)(0, tuningLockCount + (locked ? 1 : -1)); }
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