#pragma once
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <mutex>
#include <json.hpp>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <utils/event.h>
#include "channel_set.h"

// Every public method here locks `mtx` (below) for its whole body, including whatever
// SourceHandler callback it invokes (selectHandler/startHandler/.../captureConfigHandler) --
// added 2026-08-15 alongside the recording scheduler's engine thread (RECORDING_SCHEDULER_PLAN.md
// phase 5), the first thing in this codebase to call into SourceManager from anywhere other
// than the GUI thread. Before that, `sources`/`selectedName`/`selectedHandler`/etc. had no
// synchronization at all -- fine when only the GUI thread ever touched them (every frame, via
// sourcemenu::draw()'s showSelectedMenu() call), a genuine data race once a second thread does.
//
// std::recursive_mutex, not std::mutex: setTuningOffset()/setTuningMode()/setPanadapterIF() all
// call tune() internally, and Event::emit() (utils/event.h) calls every bound handler
// synchronously on the calling thread -- e.g. unregisterSource()'s onSourceUnregistered can
// reach back into sourcemenu::onSourcesChanged(), which calls SourceManager::selectSource()
// again, same thread, while the outer unregisterSource() call is still on the stack. Both are
// same-thread re-entry, which recursive_mutex allows and plain std::mutex would deadlock on.
//
// Held across the handler call itself (not released-then-reacquired around it), matching
// ModuleComManager::callInterface's own established pattern (core/src/module_com.cpp) --
// accepted here for the same reason it was accepted there: a handler that blocks for a while
// (RSR200's start(), for instance, opening USB or connecting over LAN) already stalls the GUI
// thread today regardless of this lock, since source start/stop has always run synchronously
// on whichever thread calls it. This lock adds a small, honest cost on top of that pre-existing
// behavior -- a second caller (e.g. the engine thread trying to read getSelectedName() while
// the GUI thread is mid-start()) waits for that same call to finish, rather than racing it.
//
// Lock-ordering invariant, to avoid a deadlock between this mutex and ModuleComManager's own:
// RecorderModule::start()/stop() call sigpath::sourceManager.lockTuning() while
// ModuleComManager::mtx is held (reached via callInterface()) -- i.e. ModuleComManager is the
// *outer* lock, SourceManager the *inner* one, in that one path. Nothing in this class may call
// into ModuleComManager (directly or transitively) while holding `mtx` -- that would be the
// reverse nesting and a real deadlock risk against the path above. In particular, no
// SourceHandler callback (captureConfigHandler/applyConfigHandler included) may call
// core::modComManager, only its own module's fields/config.
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

        // Optional -- the registering module's own SDRPP_MOD_INFO name (e.g. "rsr200_source"),
        // for callers that need to know which *module type* a given source name maps to
        // (RECORDING_SCHEDULER_PLAN.md section 3's sourceModuleType, guarding a saved snapshot
        // against later being applied to a same-named-but-different-module source). There is no
        // reliable way to derive this after the fact: the string passed to registerSource() is a
        // fixed display name every source module hardcodes ("RSR200", "RTL-SDR", "HackRF", ...),
        // completely independent of whatever instance name the user typed when creating the
        // module in Module Manager (a free-typed field with no default -- see
        // core/src/gui/menus/module_manager.cpp's `modName` combo) -- the two only coincide by
        // convention, not guarantee, so core::moduleManager.getInstanceModuleName(sourceName)
        // silently returns empty for anyone who named their instance anything else. Left blank
        // by any module that doesn't set it (matches every module's behavior before this field
        // existed -- getSourceModuleType() below just returns "" for those, same as today).
        std::string moduleType;
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
    // settings per radio rather than sharing one set across all of them. Returns a copy, not
    // a reference -- a reference into `selectedName` would let a caller read it outside the
    // lock that's supposed to protect it, exactly the kind of race this whole locking scheme
    // exists to close. (Existing callers that bind the result to `const std::string&` still
    // work unchanged -- that extends the temporary's lifetime to the reference's scope, same
    // as binding to any other prvalue.)
    std::string getSelectedName() const;

    // Capture/apply a named source's settings via its optional captureConfigHandler/
    // applyConfigHandler (above) -- works regardless of whether `name` is the currently
    // selected source. captureSourceConfig returns empty json{} for a nonexistent source or
    // one that doesn't implement capture; applySourceConfig is a no-op in both those cases.
    nlohmann::json captureSourceConfig(const std::string& name);
    void applySourceConfig(const std::string& name, const nlohmann::json& cfg);

    // The registering module's own SDRPP_MOD_INFO name for a given source name (see
    // SourceHandler::moduleType above) -- empty for a nonexistent source or one whose module
    // never set it.
    std::string getSourceModuleType(const std::string& name);

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
    // Reached from RecorderModule::start()/stop() while ModuleComManager::mtx is already held
    // (via callInterface()) -- ModuleComManager outer, SourceManager inner, consistent with
    // the lock-ordering invariant documented at the top of this file.
    void lockTuning(bool locked) {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        tuningLockCount = (std::max)(0, tuningLockCount + (locked ? 1 : -1));
    }
    bool isTuningLocked() const {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        return tuningLockCount > 0;
    }

    std::vector<std::string> getSourceNames();

    Event<std::string> onSourceRegistered;
    Event<std::string> onSourceUnregister;
    Event<std::string> onSourceUnregistered;
    Event<std::string> onChannelsRegistered;
    Event<std::string> onChannelsUnregistered;
    Event<double> onRetune;

private:
    // Point the IQ front end at either the phaser's output or the source's own stream,
    // depending on whether the selected source currently offers channels. Private and only
    // ever called from methods that already hold `mtx` -- does not lock it itself.
    void updateInput();

    // See the big comment at the top of this file for what this protects and why.
    mutable std::recursive_mutex mtx;

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