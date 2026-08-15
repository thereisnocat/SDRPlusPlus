#include <server.h>
#include <signal_path/source.h>
#include <utils/flog.h>
#include <signal_path/signal_path.h>
#include <core.h>

SourceManager::SourceManager() {
}

void SourceManager::registerSource(std::string name, SourceHandler* handler) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (sources.find(name) != sources.end()) {
        flog::error("Tried to register new source with existing name: {0}", name);
        return;
    }
    sources[name] = handler;
    onSourceRegistered.emit(name);
}

void SourceManager::unregisterSource(std::string name) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (sources.find(name) == sources.end()) {
        flog::error("Tried to unregister non existent source: {0}", name);
        return;
    }
    onSourceUnregister.emit(name);
    if (name == selectedName) {
        if (selectedHandler != NULL) {
            sources[selectedName]->deselectHandler(sources[selectedName]->ctx);
        }
        selectedHandler = NULL;
    }
    sources.erase(name);
    channelSets.erase(name);
    if (name == selectedName) {
        // Detaches phasing and falls back to the null source.
        updateInput();
    }
    onSourceUnregistered.emit(name);
}

void SourceManager::registerChannels(const std::string& name, ChannelSet* set) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (set == NULL || set->count < 2) {
        flog::error("Tried to register a channel set with fewer than 2 channels for source: {0}", name);
        return;
    }
    if ((int)set->streams.size() < set->count) {
        flog::error("Channel set for source '{0}' claims {1} channels but supplies {2} streams",
                    name, set->count, (int)set->streams.size());
        return;
    }
    channelSets[name] = set;
    if (name == selectedName) { updateInput(); }
    onChannelsRegistered.emit(name);
}

void SourceManager::unregisterChannels(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (channelSets.find(name) == channelSets.end()) { return; }
    channelSets.erase(name);
    if (name == selectedName) { updateInput(); }
    onChannelsUnregistered.emit(name);
}

ChannelSet* SourceManager::getChannels(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    auto it = channelSets.find(name);
    return (it == channelSets.end()) ? NULL : it->second;
}

void SourceManager::updateInput() {
    // No lock here -- always called from a method that already holds `mtx` (see source.h).
    ChannelSet* set = getChannels(selectedName);
    sigpath::phasing.setChannelSet(set);

    dsp::stream<dsp::complex_t>* input;
    if (sigpath::phasing.isActive()) {
        // The source is writing to its channel streams; the combined result is what the
        // rest of the application sees. Bypass is Phaser::MODE_A_ONLY, not a rewiring.
        input = sigpath::phasing.getOutput();
    }
    else if (selectedHandler != NULL) {
        input = selectedHandler->stream;
    }
    else {
        input = &nullSource;
    }

    if (core::args["server"].b()) {
        server::setInput(input);
    }
    else {
        sigpath::iqFrontEnd.setInput(input);
    }
}

std::vector<std::string> SourceManager::getSourceNames() {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    std::vector<std::string> names;
    for (auto const& [name, src] : sources) { names.push_back(name); }
    return names;
}

std::string SourceManager::getSelectedName() const {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    return selectedName;
}

nlohmann::json SourceManager::captureSourceConfig(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    auto it = sources.find(name);
    if (it == sources.end() || it->second->captureConfigHandler == NULL) { return nlohmann::json{}; }
    return it->second->captureConfigHandler(it->second->ctx);
}

void SourceManager::applySourceConfig(const std::string& name, const nlohmann::json& cfg) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    auto it = sources.find(name);
    if (it == sources.end() || it->second->applyConfigHandler == NULL) { return; }
    it->second->applyConfigHandler(cfg, it->second->ctx);
}

std::string SourceManager::getSourceModuleType(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    auto it = sources.find(name);
    if (it == sources.end()) { return ""; }
    return it->second->moduleType;
}

void SourceManager::selectSource(std::string name) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (sources.find(name) == sources.end()) {
        flog::error("Tried to select non existent source: {0}", name);
        return;
    }
    if (selectedHandler != NULL) {
        sources[selectedName]->deselectHandler(sources[selectedName]->ctx);
    }
    selectedHandler = sources[name];
    selectedHandler->selectHandler(selectedHandler->ctx);
    selectedName = name;
    updateInput();
}

void SourceManager::showSelectedMenu() {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->menuHandler(selectedHandler->ctx);
}

void SourceManager::start() {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->startHandler(selectedHandler->ctx);
}

void SourceManager::stop() {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->stopHandler(selectedHandler->ctx);
}

void SourceManager::tune(double freq) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    if (selectedHandler == NULL) {
        return;
    }
    if (isTuningLocked()) {
        flog::warn("SourceManager: tune() ignored, locked for an active recording");
        return;
    }
    // TODO: No need to always retune the hardware in Panadapter mode
    selectedHandler->tuneHandler(abs(((tuneMode == TuningMode::NORMAL) ? (freq + tuneOffset) : ifFreq)), selectedHandler->ctx);
    onRetune.emit(freq + tuneOffset);
    currentFreq = freq;
}

void SourceManager::setTuningOffset(double offset) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    tuneOffset = offset;
    tune(currentFreq);
}

void SourceManager::setTuningMode(TuningMode mode) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    tuneMode = mode;
    tune(currentFreq);
}

void SourceManager::setPanadapterIF(double freq) {
    std::lock_guard<std::recursive_mutex> lck(mtx);
    ifFreq = freq;
    tune(currentFreq);
}
