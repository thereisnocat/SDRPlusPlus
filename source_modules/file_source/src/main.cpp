#define NOMINMAX
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <wavreader.h>
#include <utils/wav_meta.h>
#include <core.h>
#include <gui/widgets/file_select.h>
#include <filesystem>
#include <regex>
#include <gui/tuner.h>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <ctime>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "file_source",
    /* Description:     */ "Wav file source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 1,
    /* Max instances    */ 1
};

ConfigManager config;

class FileSourceModule : public ModuleManager::Instance {
public:
    FileSourceModule(std::string name) : fileSelect("", { "Wav IQ Files (*.wav)", "*.wav", "All Files", "*" }) {
        this->name = name;

        if (core::args["server"].b()) { return; }

        config.acquire();
        fileSelect.setPath(config.conf["path"], true);
        config.release();

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource("File", &handler);
    }

    ~FileSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterChannels("File");
        sigpath::sourceManager.unregisterSource("File");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuSelected(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", _this->centerFreq);
        sigpath::iqFrontEnd.setBuffering(false);
        gui::waterfall.centerFrequencyLocked = true;
        //gui::freqSelect.minFreq = _this->centerFreq - (_this->sampleRate/2);
        //gui::freqSelect.maxFreq = _this->centerFreq + (_this->sampleRate/2);
        //gui::freqSelect.limitFreq = true;
        flog::info("FileSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        sigpath::iqFrontEnd.setBuffering(true);
        //gui::freqSelect.limitFreq = false;
        gui::waterfall.centerFrequencyLocked = false;
        gui::playbackBar.active = false;
        gui::playbackBar.seekCallback = nullptr;
        gui::playbackBar.seekCtx = nullptr;
        gui::playbackBar.recordingStartEpoch = 0;
        gui::playbackBar.recordingStartStr.clear();
        flog::info("FileSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        if (_this->running) { return; }
        if (_this->reader == NULL) { return; }
        _this->running = true;
        gui::playbackBar.active = true;
        gui::playbackBar.progress = 0.0f;
        gui::playbackBar.currentTimeSec = 0.0f;
        if (_this->dualChannel) {
            _this->workerThread = std::thread(dualWorker, _this);
        }
        else {
            _this->workerThread = _this->float32Mode ? std::thread(floatWorker, _this) : std::thread(worker, _this);
        }
        flog::info("FileSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        if (!_this->running) { return; }
        if (_this->reader == NULL) { return; }
        _this->stream.stopWriter();
        _this->streamA.stopWriter();
        _this->streamB.stopWriter();
        _this->workerThread.join();
        _this->stream.clearWriteStop();
        _this->streamA.clearWriteStop();
        _this->streamB.clearWriteStop();
        _this->running = false;
        _this->reader->rewind();
        gui::playbackBar.active = false;
        gui::playbackBar.progress = 0.0f;
        gui::playbackBar.currentTimeSec = 0.0f;
        flog::info("FileSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        flog::info("FileSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void seekBarCallback(float fraction, void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        _this->seekFraction.store(fraction);
        _this->seekPending.store(true);
    }

    static void menuHandler(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;

        if (_this->fileSelect.render("##file_source_" + _this->name)) {
            if (_this->fileSelect.pathIsValid()) {
                if (_this->reader != NULL) {
                    _this->reader->close();
                    delete _this->reader;
                }
                try {
                    _this->reader = new WavReader(_this->fileSelect.path);
                    if (_this->reader->getSampleRate() == 0) {
                        _this->reader->close();
                        delete _this->reader;
                        _this->reader = NULL;
                        throw std::runtime_error("Sample rate may not be zero");
                    }
                    _this->sampleRate = _this->reader->getSampleRate();
                    core::setInputSampleRate(_this->sampleRate);
                    // Trust the file's own fmt chunk over whatever the checkbox last said --
                    // codec 3 is WAVE_FORMAT_IEEE_FLOAT. Previously this was a manual-only
                    // toggle nothing set on load, so a float32 recording (e.g. the Recorder's
                    // 32-bit sample type) opened with it left at its default (false, i.e.
                    // int16) was read at half the correct byte stride: readSamples() pulled
                    // half as many true bytes per block as the data actually needs, so the
                    // file took roughly twice as long to play through -- audibly "half speed",
                    // not just wrong-sounding. The checkbox is left in place below as a manual
                    // override for files whose header might not be trustworthy.
                    _this->float32Mode = (_this->reader->getCodec() == 3);
                    _this->configureChannels();
                    std::string filename = std::filesystem::path(_this->fileSelect.path).filename().string();
                    _this->centerFreq = _this->getFrequency(filename);
                    // An embedded auxi chunk is more trustworthy than parsing the filename.
                    if (const auto* aux = _this->reader->getChunk("auxi")) {
                        if (aux->size() >= sizeof(wavmeta::AuxiChunk)) {
                            wavmeta::AuxiChunk a;
                            memcpy(&a, aux->data(), sizeof(a));
                            if (a.centerFreq > 0) { _this->centerFreq = (double)a.centerFreq; }
                        }
                    }
                    tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", _this->centerFreq);
                    //gui::freqSelect.minFreq = _this->centerFreq - (_this->sampleRate/2);
                    //gui::freqSelect.maxFreq = _this->centerFreq + (_this->sampleRate/2);
                    //gui::freqSelect.limitFreq = true;
                    gui::playbackBar.totalTimeSec = (float)((double)_this->reader->getDataSize() / (double)_this->reader->getBytesPerSecond());
                    gui::playbackBar.seekCallback = seekBarCallback;
                    gui::playbackBar.seekCtx = _this;
                    // Parse recording start time from the filename pattern:
                    // <type>_<freq>Hz_<HH>-<MM>-<SS>_<DD>-<MM>-<YYYY>
                    gui::playbackBar.recordingStartEpoch = 0;
                    gui::playbackBar.recordingStartStr.clear();
                    {
                        std::string fname = std::filesystem::path(_this->fileSelect.path).filename().string();
                        std::regex tsExpr("Hz_(\\d{2})-(\\d{2})-(\\d{2})_(\\d{2})-(\\d{2})-(\\d{4})");
                        std::smatch tsMatch;
                        if (std::regex_search(fname, tsMatch, tsExpr) && tsMatch.size() == 7) {
                            int HH   = std::stoi(tsMatch[1].str());
                            int MM   = std::stoi(tsMatch[2].str());
                            int SS   = std::stoi(tsMatch[3].str());
                            int DD   = std::stoi(tsMatch[4].str());
                            int Mon  = std::stoi(tsMatch[5].str());
                            int YYYY = std::stoi(tsMatch[6].str());
                            tm t = {};
                            t.tm_hour = HH; t.tm_min = MM; t.tm_sec = SS;
                            t.tm_mday = DD; t.tm_mon = Mon - 1; t.tm_year = YYYY - 1900;
                            t.tm_isdst = -1;
                            time_t epoch = mktime(&t);
                            if (epoch != (time_t)-1) {
                                gui::playbackBar.recordingStartEpoch = (int64_t)epoch;
                                char buf[32];
                                snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", YYYY, Mon, DD, HH, MM, SS);
                                gui::playbackBar.recordingStartStr = buf;
                            }
                        }
                    }
                }
                catch (const std::exception& e) {
                    flog::error("Error: {}", e.what());
                }
                config.acquire();
                config.conf["path"] = _this->fileSelect.path;
                config.release(true);
            }
        }

        ImGui::Checkbox("Float32 Mode##_file_source", &_this->float32Mode);
    }

    static void worker(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        double sampleRate = std::max(_this->reader->getSampleRate(), (uint32_t)1);
        int blockSize = std::min((int)(sampleRate / 200.0f), (int)STREAM_BUFFER_SIZE);
        int16_t* inBuf = new int16_t[blockSize * 2];

        while (true) {
            if (_this->seekPending.exchange(false)) {
                _this->reader->seekToFraction(_this->seekFraction.load());
            }
            _this->reader->readSamples(inBuf, blockSize * 2 * sizeof(int16_t));
            volk_16i_s32f_convert_32f((float*)_this->stream.writeBuf, inBuf, 32768.0f, blockSize * 2);
            uint64_t dataSize = _this->reader->getDataSize();
            if (dataSize > 0) {
                size_t byteOff = _this->reader->getCurrentByteOffset();
                gui::playbackBar.progress = (float)((double)byteOff / (double)dataSize);
                gui::playbackBar.currentTimeSec = (float)byteOff / (float)_this->reader->getBytesPerSecond();
            }
            if (!_this->stream.swap(blockSize)) { break; };
        }

        delete[] inBuf;
    }

    // Four WAV channels are two complex channels interleaved as I1 Q1 I2 Q2. Split them
    // back apart and hand both to the phasing front end, so a dual channel recording can
    // be re-phased exactly as if the radio were still connected.
    static void dualWorker(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        double sampleRate = std::max(_this->reader->getSampleRate(), (uint32_t)1);
        int blockSize = std::min((int)(sampleRate / 200.0f), (int)STREAM_BUFFER_SIZE);
        std::vector<int16_t> inBuf(blockSize * 4);
        std::vector<float> fBuf(blockSize * 4);

        while (true) {
            if (_this->seekPending.exchange(false)) {
                _this->reader->seekToFraction(_this->seekFraction.load());
            }

            if (_this->float32Mode) {
                _this->reader->readSamples(fBuf.data(), blockSize * 4 * sizeof(float));
            }
            else {
                _this->reader->readSamples(inBuf.data(), blockSize * 4 * sizeof(int16_t));
                volk_16i_s32f_convert_32f(fBuf.data(), inBuf.data(), 32768.0f, blockSize * 4);
            }

            for (int i = 0; i < blockSize; i++) {
                _this->streamA.writeBuf[i] = { fBuf[4 * i + 0], fBuf[4 * i + 1] };
                _this->streamB.writeBuf[i] = { fBuf[4 * i + 2], fBuf[4 * i + 3] };
            }

            uint64_t dataSize = _this->reader->getDataSize();
            if (dataSize > 0) {
                size_t byteOff = _this->reader->getCurrentByteOffset();
                gui::playbackBar.progress = (float)((double)byteOff / (double)dataSize);
                gui::playbackBar.currentTimeSec = (float)byteOff / (float)_this->reader->getBytesPerSecond();
            }

            // Swapped in the order the phaser reads them.
            if (!_this->streamA.swap(blockSize)) { break; }
            if (!_this->streamB.swap(blockSize)) { break; }
        }
    }

    static void floatWorker(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        double sampleRate = std::max(_this->reader->getSampleRate(), (uint32_t)1);
        int blockSize = std::min((int)(sampleRate / 200.0f), (int)STREAM_BUFFER_SIZE);
        dsp::complex_t* inBuf = new dsp::complex_t[blockSize];

        while (true) {
            if (_this->seekPending.exchange(false)) {
                _this->reader->seekToFraction(_this->seekFraction.load());
            }
            _this->reader->readSamples(_this->stream.writeBuf, blockSize * sizeof(dsp::complex_t));
            uint64_t dataSize = _this->reader->getDataSize();
            if (dataSize > 0) {
                size_t byteOff = _this->reader->getCurrentByteOffset();
                gui::playbackBar.progress = (float)((double)byteOff / (double)dataSize);
                gui::playbackBar.currentTimeSec = (float)byteOff / (float)_this->reader->getBytesPerSecond();
            }
            if (!_this->stream.swap(blockSize)) { break; };
        }

        delete[] inBuf;
    }

    // A four channel file is two complex channels: offer them to core for phasing so a
    // dual channel recording plays back exactly like the radio that made it.
    void configureChannels() {
        sigpath::sourceManager.unregisterChannels("File");
        dualChannel = false;
        if (reader == NULL || reader->getChannelCount() != 4) { return; }

        wavmeta::PhasingInfo info;
        bool haveInfo = false;
        if (const auto* c = reader->getChunk(std::string(wavmeta::PHASING_CHUNK_ID, 4).c_str())) {
            haveInfo = wavmeta::parsePhasing(c->data(), c->size(), info);
        }

        channels.count = 2;
        channels.streams = { &streamA, &streamB };
        if (haveInfo && info.names.size() >= 2) {
            channels.names = { info.names[0], info.names[1] };
        }
        else {
            // A four channel file with no chunk of ours is still probably two IQ channels;
            // play it back rather than refusing, just without the labels.
            channels.names = { "1", "2" };
        }
        channels.phaseCoherent = haveInfo ? info.phaseCoherent : false;
        channels.sampleAligned = haveInfo ? info.sampleAligned : true;

        dualChannel = true;
        sigpath::sourceManager.registerChannels("File", &channels);
        flog::info("FileSourceModule: 4-channel recording, offering channels '{0}' and '{1}'",
                   channels.names[0], channels.names[1]);
    }

    double getFrequency(std::string filename) {
        std::regex expr("[0-9]+Hz");
        std::smatch matches;
        std::regex_search(filename, matches, expr);
        if (matches.empty()) { return 0; }
        std::string freqStr = matches[0].str();
        return std::atof(freqStr.substr(0, freqStr.size() - 2).c_str());
    }

    FileSelect fileSelect;
    std::string name;
    dsp::stream<dsp::complex_t> stream;
    dsp::stream<dsp::complex_t> streamA;
    dsp::stream<dsp::complex_t> streamB;
    ChannelSet channels;
    bool dualChannel = false;
    SourceManager::SourceHandler handler;
    WavReader* reader = NULL;
    bool running = false;
    bool enabled = true;
    float sampleRate = 1000000;
    std::thread workerThread;
    std::atomic<bool> seekPending{false};
    std::atomic<float> seekFraction{0.0f};

    double centerFreq = 100000000;

    bool float32Mode = false;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["path"] = "";
    config.setPath(core::args["root"].s() + "/file_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    return new FileSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FileSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
