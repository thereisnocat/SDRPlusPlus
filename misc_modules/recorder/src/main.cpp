#include <imgui.h>
#include <module.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/bench/peak_level_meter.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/routing/splitter.h>
#include <dsp/audio/volume.h>
#include <dsp/convert/stereo_to_mono.h>
#include <thread>
#include <ctime>
#include <cstddef>
#include <gui/gui.h>
#include <filesystem>
#include <signal_path/signal_path.h>
#include <config.h>
#include <gui/style.h>
#include <gui/widgets/volume_meter.h>
#include <regex>
#include <gui/widgets/folder_select.h>
#include <recorder_interface.h>
#include <core.h>
#include <utils/optionlist.h>
#include <utils/wav.h>
#include <utils/wav_meta.h>
#include <dsp/combine/channel_sync.h>
#include <radio_interface.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

#define SILENCE_LVL 10e-6

SDRPP_MOD_INFO{
    /* Name:            */ "recorder",
    /* Description:     */ "Recorder module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ -1
};

ConfigManager config;

enum TimeZone {
    TIME_ZONE_LOCAL,
    TIME_ZONE_UTC
};

class RecorderModule : public ModuleManager::Instance {
public:
    RecorderModule(std::string name) : folderSelect("%ROOT%/recordings") {
        this->name = name;
        root = (std::string)core::args["root"];
        strcpy(nameTemplate, "$t_$f_$h-$m-$s_$d-$M-$y");

        // Define the time zones
        timezones.define("local", "Local", TIME_ZONE_LOCAL);
        timezones.define("utc", "UTC", TIME_ZONE_UTC);

        // Define option lists
        containers.define("WAV", wav::FORMAT_WAV);
        // containers.define("RF64", wav::FORMAT_RF64); // Disabled for now
        // Order and labels are deliberate, not just alphabetical -- see
        // RECORDING_REFACTOR_PLAN.md section 3.4. Float32 is listed as the step up from
        // Int16 (it's the interoperable choice for anything needing more than 16-bit
        // precision -- SDR Console's own documented format is int16 or float32 only), with
        // Int32 last and labeled with its interop gap so it isn't the obvious reach for
        // "I want more precision" the way it was when RSR200's 24-bit mode first hit this.
        // Defined by key (the enum value, not list position), so reordering here doesn't
        // disturb anyone's already-saved sampleType preference.
        sampleTypes.define(wav::SAMP_TYPE_UINT8, "Uint8", wav::SAMP_TYPE_UINT8);
        sampleTypes.define(wav::SAMP_TYPE_INT16, "Int16", wav::SAMP_TYPE_INT16);
        sampleTypes.define(wav::SAMP_TYPE_FLOAT32, "Float32 (recommended for >16-bit)", wav::SAMP_TYPE_FLOAT32);
        sampleTypes.define(wav::SAMP_TYPE_INT32, "Int32 (not read by all tools, e.g. SDR Console)", wav::SAMP_TYPE_INT32);

        // Load default config for option lists
        timezoneId = timezones.valueId(TIME_ZONE_LOCAL);
        containerId = containers.valueId(wav::FORMAT_WAV);
        sampleTypeId = sampleTypes.valueId(wav::SAMP_TYPE_INT16);

        // Load config
        config.acquire();
        if (config.conf[name].contains("mode")) {
            recMode = config.conf[name]["mode"];
        }
        if (config.conf[name].contains("recPath")) {
            folderSelect.setPath(config.conf[name]["recPath"]);
        }
        if (config.conf[name].contains("timezone") && timezones.keyExists(config.conf[name]["timezone"])) {
            timezoneId = timezones.keyId(config.conf[name]["timezone"]);
        }
        if (config.conf[name].contains("container") && containers.keyExists(config.conf[name]["container"])) {
            containerId = containers.keyId(config.conf[name]["container"]);
        }
        if (config.conf[name].contains("sampleType") && sampleTypes.keyExists(config.conf[name]["sampleType"])) {
            sampleTypeId = sampleTypes.keyId(config.conf[name]["sampleType"]);
        }
        if (config.conf[name].contains("audioStream")) {
            selectedStreamName = config.conf[name]["audioStream"];
        }
        if (config.conf[name].contains("audioVolume")) {
            audioVolume = config.conf[name]["audioVolume"];
        }
        if (config.conf[name].contains("stereo")) {
            stereo = config.conf[name]["stereo"];
        }
        if (config.conf[name].contains("ignoreSilence")) {
            ignoreSilence = config.conf[name]["ignoreSilence"];
        }
        if (config.conf[name].contains("recordDualChannel")) {
            recordDualChannel = config.conf[name]["recordDualChannel"];
        }
        if (config.conf[name].contains("nameTemplate")) {
            std::string _nameTemplate = config.conf[name]["nameTemplate"];
            if (_nameTemplate.length() > sizeof(nameTemplate)-1) {
                _nameTemplate = _nameTemplate.substr(0, sizeof(nameTemplate)-1);
            }
            strcpy(nameTemplate, _nameTemplate.c_str());
        }
        config.release();

        // Init audio path
        volume.init(NULL, audioVolume, false);
        splitter.init(&volume.out);
        splitter.bindStream(&meterStream);
        meter.init(&meterStream);
        s2m.init(&stereoStream);

        // Init sinks
        basebandSink.init(NULL, complexHandler, this);
        stereoSink.init(&stereoStream, stereoHandler, this);
        monoSink.init(&s2m.out, monoHandler, this);

        gui::menu.registerEntry(name, menuHandler, this);
        core::modComManager.registerInterface("recorder", name, moduleInterfaceHandler, this);
    }

    ~RecorderModule() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        core::modComManager.unregisterInterface(name);
        gui::menu.removeEntry(name);
        stop();
        deselectStream();
        sigpath::sinkManager.onStreamRegistered.unbindHandler(&onStreamRegisteredHandler);
        sigpath::sinkManager.onStreamUnregister.unbindHandler(&onStreamUnregisterHandler);
        meter.stop();
    }

    void postInit() {
        // Enumerate streams
        audioStreams.clear();
        auto names = sigpath::sinkManager.getStreamNames();
        for (const auto& name : names) {
            audioStreams.define(name, name, name);
        }

        // Bind stream register/unregister handlers
        onStreamRegisteredHandler.ctx = this;
        onStreamRegisteredHandler.handler = streamRegisteredHandler;
        sigpath::sinkManager.onStreamRegistered.bindHandler(&onStreamRegisteredHandler);
        onStreamUnregisterHandler.ctx = this;
        onStreamUnregisterHandler.handler = streamUnregisterHandler;
        sigpath::sinkManager.onStreamUnregister.bindHandler(&onStreamUnregisterHandler);

        // Select the stream
        selectStream(selectedStreamName);
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void start() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (recording) { return; }

        // Configure the wav writer
        if (recMode == RECORDER_MODE_AUDIO) {
            if (selectedStreamName.empty()) { return; }
            samplerate = sigpath::sinkManager.getStreamSampleRate(selectedStreamName);
        }
        else {
            samplerate = sigpath::iqFrontEnd.getSampleRate();
        }
        // Dual channel capture records the raw channels upstream of the phaser, so the
        // recording can be re-phased later. Two complex channels interleave into four WAV
        // channels: I1 Q1 I2 Q2.
        recordingDual = (recMode == RECORDER_MODE_BASEBAND) && recordDualChannel && sigpath::phasing.isActive();

        writer.setFormat(containers[containerId]);
        if (recordingDual) {
            writer.setChannels(4);
        }
        else {
            writer.setChannels((recMode == RECORDER_MODE_AUDIO && !stereo) ? 1 : 2);
        }
        writer.setSampleType(sampleTypes[sampleTypeId]);
        writer.setSamplerate(samplerate);

        // Metadata chunks, which have to be queued before open()
        writer.clearChunks();
        if (recMode == RECORDER_MODE_BASEBAND) {
            const std::time_t now = std::time(NULL);
            wavmeta::AuxiChunk auxi = wavmeta::makeAuxi(gui::waterfall.getCenterFrequency(), samplerate, now, now);
            writer.addChunk("auxi", &auxi, sizeof(auxi));

            if (recordingDual) {
                wavmeta::PhasingInfo info;
                info.channelCount = 2;
                info.phaseCoherent = sigpath::phasing.isPhaseCoherent();
                info.sampleAligned = sigpath::phasing.isSampleAligned();
                int a = 0, b = 1;
                sigpath::phasing.getChannelPair(a, b);
                info.combinedA = 0;
                info.combinedB = 1;
                info.names = { sigpath::phasing.getChannelName(a), sigpath::phasing.getChannelName(b) };
                dualChA = a;
                dualChB = b;
                auto blob = wavmeta::serializePhasing(info);
                writer.addChunk(wavmeta::PHASING_CHUNK_ID, blob.data(), blob.size());
            }
        }

        // Open file
        std::string vfoName = (recMode == RECORDER_MODE_AUDIO) ? selectedStreamName : "";
        std::string extension = ".wav";
        std::string expandedPath = expandString(folderSelect.path + "/" + genFileName(nameTemplate, recMode, vfoName) + extension);
        if (!writer.open(expandedPath)) {
            flog::error("Failed to open file for recording: {0}", expandedPath);
            return;
        }

        // Open audio stream or baseband
        if (recMode == RECORDER_MODE_AUDIO) {
            // Start correct path depending on 
            if (stereo) {
                stereoSink.start();
            }
            else {
                s2m.start();
                monoSink.start();
            }
            splitter.bindStream(&stereoStream);
        }
        else if (recordingDual) {
            // Tap the two raw channels upstream of the combining.
            dualA = new dsp::stream<dsp::complex_t>();
            dualB = new dsp::stream<dsp::complex_t>();
            dualSync.init(2);
            dualBuf.resize(4 * STREAM_BUFFER_SIZE);
            sigpath::phasing.bindChannelStream(dualChA, dualA);
            sigpath::phasing.bindChannelStream(dualChB, dualB);
            dualRun = true;
            dualThread = std::thread(&RecorderModule::dualWorker, this);
        }
        else {
            // Create and bind IQ stream
            basebandStream = new dsp::stream<dsp::complex_t>();
            basebandSink.setInput(basebandStream);
            basebandSink.start();
            sigpath::iqFrontEnd.bindIQStream(basebandStream);
        }

        // Say so: a dual channel capture runs at four times the data rate of an audio
        // recording, and silently starting one is how a disk fills up.
        if (recMode == RECORDER_MODE_BASEBAND) {
            if (recordingDual) {
                flog::info("Recording both channels ('{0}', '{1}') as 4 interleaved WAV channels at {2} Hz",
                           sigpath::phasing.getChannelName(dualChA), sigpath::phasing.getChannelName(dualChB),
                           (uint64_t)samplerate);
            }
            else {
                flog::info("Recording baseband, 2 channels at {0} Hz", (uint64_t)samplerate);
            }
        }

        recording = true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (!recording) { return; }

        // Close audio stream or baseband
        if (recMode == RECORDER_MODE_AUDIO) {
            splitter.unbindStream(&stereoStream);
            monoSink.stop();
            stereoSink.stop();
            s2m.stop();
            
        }
        else if (recordingDual) {
            dualRun = false;
            dualA->stopReader();
            dualB->stopReader();
            if (dualThread.joinable()) { dualThread.join(); }
            sigpath::phasing.unbindChannelStream(dualChA, dualA);
            sigpath::phasing.unbindChannelStream(dualChB, dualB);
            delete dualA;
            delete dualB;
            dualA = NULL;
            dualB = NULL;
            recordingDual = false;
        }
        else {
            // Unbind and destroy IQ stream
            sigpath::iqFrontEnd.unbindIQStream(basebandStream);
            basebandSink.stop();
            delete basebandStream;
        }

        // Patch auxi's real stop time now that it's actually known -- start() had to write
        // a placeholder (equal to startTime) since addChunk() runs before the recording
        // that determines the real one has even happened. Must run before writer.close():
        // patchChunk(), like the RIFF/data sizes it mirrors, only works while the file is
        // still open to seek within.
        if (recMode == RECORDER_MODE_BASEBAND) {
            wavmeta::SystemTime stop = wavmeta::toSystemTime(std::time(NULL));
            writer.patchChunk("auxi", offsetof(wavmeta::AuxiChunk, stopTime), &stop, sizeof(stop));
        }

        // Close file
        writer.close();

        recording = false;
    }

private:
    // Interleaves the two raw channels into I1 Q1 I2 Q2 frames and writes them.
    //
    // The two taps come from separate splitters, so although a source normally writes the
    // same block size to every channel, nothing in the contract guarantees it. ChannelSync
    // handles the general case: read whichever channel is behind, and write only what both
    // can supply. Reading one buffer from each per pass would deadlock the moment the two
    // delivered different block sizes.
    void dualWorker() {
        while (dualRun) {
            const int ch = dualSync.nextChannel();
            dsp::stream<dsp::complex_t>* in = ch ? dualB : dualA;

            const int c = in->read();
            if (c < 0) { break; }
            dualSync.feed(ch, in->readBuf, c);
            in->flush();

            const int count = dualSync.available();
            if (count <= 0) { continue; }

            const dsp::complex_t* a = dualSync.data(0);
            const dsp::complex_t* b = dualSync.data(1);
            for (int i = 0; i < count; i++) {
                dualBuf[4 * i + 0] = a[i].re;
                dualBuf[4 * i + 1] = a[i].im;
                dualBuf[4 * i + 2] = b[i].re;
                dualBuf[4 * i + 3] = b[i].im;
            }

            {
                std::lock_guard<std::recursive_mutex> lck(recMtx);
                if (writer.isOpen()) { writer.write(dualBuf.data(), count); }
            }
            dualSync.consume(count);
            samplesWritten += count;
        }
    }

    static void menuHandler(void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Recording mode
        if (_this->recording) { style::beginDisabled(); }
        ImGui::BeginGroup();
        ImGui::Columns(2, CONCAT("RecorderModeColumns##_", _this->name), false);
        if (ImGui::RadioButton(CONCAT("Baseband##_recorder_mode_", _this->name), _this->recMode == RECORDER_MODE_BASEBAND)) {
            _this->recMode = RECORDER_MODE_BASEBAND;
            config.acquire();
            config.conf[_this->name]["mode"] = _this->recMode;
            config.release(true);
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("Audio##_recorder_mode_", _this->name), _this->recMode == RECORDER_MODE_AUDIO)) {
            _this->recMode = RECORDER_MODE_AUDIO;
            config.acquire();
            config.conf[_this->name]["mode"] = _this->recMode;
            config.release(true);
        }
        ImGui::Columns(1, CONCAT("EndRecorderModeColumns##_", _this->name), false);
        ImGui::EndGroup();

        // Recording path
        if (_this->folderSelect.render("##_recorder_fold_" + _this->name)) {
            if (_this->folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["recPath"] = _this->folderSelect.path;
                config.release(true);
            }
        }

        ImGui::LeftLabel("Name template");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##_recorder_name_template_", _this->name), _this->nameTemplate, 1023)) {
            config.acquire();
            config.conf[_this->name]["nameTemplate"] = _this->nameTemplate;
            config.release(true);
        }

        ImGui::LeftLabel("Time zone");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_recorder_timezone_", _this->name), &_this->timezoneId, _this->timezones.txt)) {
            config.acquire();
            config.conf[_this->name]["timezone"] = _this->timezones.key(_this->timezoneId);
            config.release(true);
        }

        ImGui::LeftLabel("Container");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_recorder_container_", _this->name), &_this->containerId, _this->containers.txt)) {
            config.acquire();
            config.conf[_this->name]["container"] = _this->containers.key(_this->containerId);
            config.release(true);
        }

        ImGui::LeftLabel("Sample type");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_recorder_st_", _this->name), &_this->sampleTypeId, _this->sampleTypes.txt)) {
            config.acquire();
            config.conf[_this->name]["sampleType"] = _this->sampleTypes.key(_this->sampleTypeId);
            config.release(true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Int16 is enough for any source whose real ADC resolution is 16 bits or\n"
                "less (which covers everything here except RSR200's 24-bit mode). For a\n"
                "source that genuinely exceeds 16 bits, use Float32, not Int32 -- Int32\n"
                "isn't one of the sample formats every third-party tool reads (SDR Console's\n"
                "own documented format, for instance, is int16 or float32 only)."
            );
        }

        // Dual channel capture, offered only when the selected source actually has
        // channels to capture.
        if (_this->recMode == RECORDER_MODE_BASEBAND && sigpath::phasing.isActive()) {
            if (ImGui::Checkbox(CONCAT("Record both channels##_recorder_dual_", _this->name), &_this->recordDualChannel)) {
                config.acquire();
                config.conf[_this->name]["recordDualChannel"] = _this->recordDualChannel;
                config.release(true);
            }
            if (_this->recordDualChannel) {
                // Tell the user the cost before they fill a disk rather than after. The
                // effective lever is the sample rate, not the sample type.
                const double sr = (double)sigpath::iqFrontEnd.getSampleRate();
                int bytesPerSamp = 2;
                switch (_this->sampleTypes[_this->sampleTypeId]) {
                    case wav::SAMP_TYPE_UINT8:   bytesPerSamp = 1; break;
                    case wav::SAMP_TYPE_INT16:   bytesPerSamp = 2; break;
                    case wav::SAMP_TYPE_INT32:   bytesPerSamp = 4; break;
                    case wav::SAMP_TYPE_FLOAT32: bytesPerSamp = 4; break;
                }
                const double mbPerMin = sr * 4.0 * bytesPerSamp * 60.0 / 1e6;
                ImGui::TextWrapped("4 channels (I1 Q1 I2 Q2), about %.0f MB/min", mbPerMin);
            }
        }

        if (_this->recording) { style::endDisabled(); }

        // Show additional audio options
        if (_this->recMode == RECORDER_MODE_AUDIO) {
            if (_this->recording) { style::beginDisabled(); }
            ImGui::LeftLabel("Stream");
            ImGui::FillWidth();
            if (ImGui::Combo(CONCAT("##_recorder_stream_", _this->name), &_this->streamId, _this->audioStreams.txt)) {
                _this->selectStream(_this->audioStreams.value(_this->streamId));
                config.acquire();
                config.conf[_this->name]["audioStream"] = _this->audioStreams.key(_this->streamId);
                config.release(true);
            }
            if (_this->recording) { style::endDisabled(); }

            _this->updateAudioMeter(_this->audioLvl);
            ImGui::FillWidth();
            ImGui::VolumeMeter(_this->audioLvl.l, _this->audioLvl.l, -60, 10);
            ImGui::FillWidth();
            ImGui::VolumeMeter(_this->audioLvl.r, _this->audioLvl.r, -60, 10);

            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_recorder_vol_", _this->name), &_this->audioVolume, 0, 1, "")) {
                _this->volume.setVolume(_this->audioVolume);
                config.acquire();
                config.conf[_this->name]["audioVolume"] = _this->audioVolume;
                config.release(true);
            }

            if (_this->recording) { style::beginDisabled(); }
            if (ImGui::Checkbox(CONCAT("Stereo##_recorder_stereo_", _this->name), &_this->stereo)) {
                config.acquire();
                config.conf[_this->name]["stereo"] = _this->stereo;
                config.release(true);
            }
            if (_this->recording) { style::endDisabled(); }

            if (ImGui::Checkbox(CONCAT("Ignore silence##_recorder_ignore_silence_", _this->name), &_this->ignoreSilence)) {
                config.acquire();
                config.conf[_this->name]["ignoreSilence"] = _this->ignoreSilence;
                config.release(true);
            }
        }

        // Record button
        bool canRecord = _this->folderSelect.pathIsValid();
        if (_this->recMode == RECORDER_MODE_AUDIO) { canRecord &= !_this->selectedStreamName.empty(); }
        if (!_this->recording) {
            if (ImGui::Button(CONCAT("Record##_recorder_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->start();
            }
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "Idle --:--:--");
        }
        else {
            if (ImGui::Button(CONCAT("Stop##_recorder_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->stop();
            }
            uint64_t seconds = _this->writer.getSamplesWritten() / _this->samplerate;
            time_t diff = seconds;
            tm* dtm = gmtime(&diff);

            if (_this->ignoreSilence && _this->ignoringSilence) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Paused %02d:%02d:%02d", dtm->tm_hour, dtm->tm_min, dtm->tm_sec);
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Recording %02d:%02d:%02d", dtm->tm_hour, dtm->tm_min, dtm->tm_sec);
            }
        }
    }

    void selectStream(std::string name) {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        deselectStream();

        if (audioStreams.empty()) {
            selectedStreamName.clear();
            return;
        }
        else if (!audioStreams.keyExists(name)) {
            selectStream(audioStreams.key(0));
            return;
        }

        audioStream = sigpath::sinkManager.bindStream(name);
        if (!audioStream) { return; }
        selectedStreamName = name;
        streamId = audioStreams.keyId(name);
        volume.setInput(audioStream);
        startAudioPath();
    }

    void deselectStream() {
        std::lock_guard<std::recursive_mutex> lck(recMtx);
        if (selectedStreamName.empty() || !audioStream) {
            selectedStreamName.clear();
            return;
        }
        if (recording && recMode == RECORDER_MODE_AUDIO) { stop(); }
        stopAudioPath();
        sigpath::sinkManager.unbindStream(selectedStreamName, audioStream);
        selectedStreamName.clear();
        audioStream = NULL;
    }

    void startAudioPath() {
        volume.start();
        splitter.start();
        meter.start();
    }

    void stopAudioPath() {
        volume.stop();
        splitter.stop();
        meter.stop();
    }

    static void streamRegisteredHandler(std::string name, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;

        // Add new stream to the list
        _this->audioStreams.define(name, name, name);

        // If no stream is selected, select new stream. If not, update the menu ID. 
        if (_this->selectedStreamName.empty()) {
            _this->selectStream(name);
        }
        else {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    static void streamUnregisterHandler(std::string name, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;

        // Remove stream from list
        _this->audioStreams.undefineKey(name);

        // If the stream is in used, deselect it and reselect default. Otherwise, update ID.
        if (_this->selectedStreamName == name) {
            _this->selectStream("");
        }
        else {
            _this->streamId = _this->audioStreams.keyId(_this->selectedStreamName);
        }
    }

    void updateAudioMeter(dsp::stereo_t& lvl) {
        // Note: Yes, using the natural log is on purpose, it just gives a more beautiful result.
        double frameTime = 1.0 / ImGui::GetIO().Framerate;
        lvl.l = std::clamp<float>(lvl.l - (frameTime * 50.0), -90.0f, 10.0f);
        lvl.r = std::clamp<float>(lvl.r - (frameTime * 50.0), -90.0f, 10.0f);
        dsp::stereo_t rawLvl = meter.getLevel();
        meter.resetLevel();
        dsp::stereo_t dbLvl = { 10.0f * logf(rawLvl.l), 10.0f * logf(rawLvl.r) };
        if (dbLvl.l > lvl.l) { lvl.l = dbLvl.l; }
        if (dbLvl.r > lvl.r) { lvl.r = dbLvl.r; }
    }

    std::map<int, const char*> radioModeToString = {
        { RADIO_IFACE_MODE_NFM, "NFM" },
        { RADIO_IFACE_MODE_WFM, "WFM" },
        { RADIO_IFACE_MODE_AM,  "AM"  },
        { RADIO_IFACE_MODE_DSB, "DSB" },
        { RADIO_IFACE_MODE_USB, "USB" },
        { RADIO_IFACE_MODE_CW,  "CW"  },
        { RADIO_IFACE_MODE_LSB, "LSB" },
        { RADIO_IFACE_MODE_RAW, "RAW" }
    };

    std::string genFileName(std::string templ, int mode, std::string name) {
        // Get data
        time_t now = time(0);
        tm* ltm = (timezones[timezoneId] == TIME_ZONE_UTC) ? gmtime(&now) : localtime(&now);
        char buf[1024];
        double freq = gui::waterfall.getCenterFrequency();
        if (gui::waterfall.vfos.find(name) != gui::waterfall.vfos.end()) {
            freq += gui::waterfall.vfos[name]->generalOffset;
        }

        // Select the recording type string
        std::string type = (recMode == RECORDER_MODE_AUDIO) ? "audio" : "baseband";

        // Format to string
        char freqStr[128];
        char hourStr[128];
        char minStr[128];
        char secStr[128];
        char dayStr[128];
        char monStr[128];
        char yearStr[128];
        const char* modeStr = (recMode == RECORDER_MODE_AUDIO) ? "Unknown" : "IQ";
        sprintf(freqStr, "%.0lfHz", freq);
        sprintf(hourStr, "%02d", ltm->tm_hour);
        sprintf(minStr, "%02d", ltm->tm_min);
        sprintf(secStr, "%02d", ltm->tm_sec);
        sprintf(dayStr, "%02d", ltm->tm_mday);
        sprintf(monStr, "%02d", ltm->tm_mon + 1);
        sprintf(yearStr, "%02d", ltm->tm_year + 1900);
        if (core::modComManager.getModuleName(name) == "radio") {
            int mode = -1;
            core::modComManager.callInterface(name, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
            if (mode >= 0) { modeStr = radioModeToString[mode]; };
        }

        // Replace in template
        templ = std::regex_replace(templ, std::regex("\\$t"), type);
        templ = std::regex_replace(templ, std::regex("\\$f"), freqStr);
        templ = std::regex_replace(templ, std::regex("\\$h"), hourStr);
        templ = std::regex_replace(templ, std::regex("\\$m"), minStr);
        templ = std::regex_replace(templ, std::regex("\\$s"), secStr);
        templ = std::regex_replace(templ, std::regex("\\$d"), dayStr);
        templ = std::regex_replace(templ, std::regex("\\$M"), monStr);
        templ = std::regex_replace(templ, std::regex("\\$y"), yearStr);
        templ = std::regex_replace(templ, std::regex("\\$r"), modeStr);
        return templ;
    }

    std::string expandString(std::string input) {
        input = std::regex_replace(input, std::regex("%ROOT%"), root);
        return std::regex_replace(input, std::regex("//"), "/");
    }

    static void complexHandler(dsp::complex_t* data, int count, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        _this->writer.write((float*)data, count);
    }

    static void stereoHandler(dsp::stereo_t* data, int count, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        if (_this->ignoreSilence) {
            float absMax = 0.0f;
            float* _data = (float*)data;
            int _count = count * 2;
            for (int i = 0; i < _count; i++) {
                float val = fabsf(_data[i]);
                if (val > absMax) { absMax = val; }
            }
            _this->ignoringSilence = (absMax < SILENCE_LVL);
            if (_this->ignoringSilence) { return; }
        }
        _this->writer.write((float*)data, count);
    }

    static void monoHandler(float* data, int count, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        if (_this->ignoreSilence) {
            float absMax = 0.0f;
            for (int i = 0; i < count; i++) {
                float val = fabsf(data[i]);
                if (val > absMax) { absMax = val; }
            }
            _this->ignoringSilence = (absMax < SILENCE_LVL);
            if (_this->ignoringSilence) { return; }
        }
        _this->writer.write(data, count);
    }

    static void moduleInterfaceHandler(int code, void* in, void* out, void* ctx) {
        RecorderModule* _this = (RecorderModule*)ctx;
        std::lock_guard lck(_this->recMtx);
        if (code == RECORDER_IFACE_CMD_GET_MODE) {
            int* _out = (int*)out;
            *_out = _this->recMode;
        }
        else if (code == RECORDER_IFACE_CMD_SET_MODE) {
            if (_this->recording) { return; }
            int* _in = (int*)in;
            _this->recMode = std::clamp<int>(*_in, 0, 1);
        }
        else if (code == RECORDER_IFACE_CMD_START) {
            if (!_this->recording) { _this->start(); }
        }
        else if (code == RECORDER_IFACE_CMD_STOP) {
            if (_this->recording) { _this->stop(); }
        }
    }

    std::string name;
    bool enabled = true;
    std::string root;
    char nameTemplate[1024];

    OptionList<std::string, TimeZone> timezones;
    OptionList<std::string, wav::Format> containers;
    OptionList<int, wav::SampleType> sampleTypes;
    FolderSelect folderSelect;

    int recMode = RECORDER_MODE_AUDIO;
    int timezoneId;
    int containerId;
    int sampleTypeId;
    bool stereo = true;
    std::string selectedStreamName = "";
    float audioVolume = 1.0f;
    bool ignoreSilence = false;
    dsp::stereo_t audioLvl = { -100.0f, -100.0f };

    bool recording = false;
    bool ignoringSilence = false;

    // Dual channel (phasing) capture
    bool recordDualChannel = false;
    bool recordingDual = false;
    int dualChA = 0, dualChB = 1;
    dsp::stream<dsp::complex_t>* dualA = NULL;
    dsp::stream<dsp::complex_t>* dualB = NULL;
    dsp::combine::ChannelSync dualSync;
    std::vector<float> dualBuf;
    std::thread dualThread;
    std::atomic<bool> dualRun{ false };
    std::atomic<uint64_t> samplesWritten{ 0 };
    wav::Writer writer;
    std::recursive_mutex recMtx;
    dsp::stream<dsp::complex_t>* basebandStream;
    dsp::stream<dsp::stereo_t> stereoStream;
    dsp::sink::Handler<dsp::complex_t> basebandSink;
    dsp::sink::Handler<dsp::stereo_t> stereoSink;
    dsp::sink::Handler<float> monoSink;

    OptionList<std::string, std::string> audioStreams;
    int streamId = 0;
    dsp::stream<dsp::stereo_t>* audioStream = NULL;
    dsp::audio::Volume volume;
    dsp::routing::Splitter<dsp::stereo_t> splitter;
    dsp::stream<dsp::stereo_t> meterStream;
    dsp::bench::PeakLevelMeter<dsp::stereo_t> meter;
    dsp::convert::StereoToMono s2m;

    uint64_t samplerate = 48000;

    EventHandler<std::string> onStreamRegisteredHandler;
    EventHandler<std::string> onStreamUnregisterHandler;

};

MOD_EXPORT void _INIT_() {
    // Create default recording directory
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root + "/recordings")) {
        flog::warn("Recordings directory does not exist, creating it");
        if (!std::filesystem::create_directory(root + "/recordings")) {
            flog::error("Could not create recordings directory");
        }
    }
    json def = json({});
    config.setPath(root + "/recorder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RecorderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete (RecorderModule*)inst;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}