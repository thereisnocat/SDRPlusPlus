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
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>   // timeBeginPeriod/timeEndPeriod -- see start()/stop() below
#pragma comment(lib, "winmm.lib")
#endif

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
        gui::playbackBar.playPauseCallback = nullptr;
        gui::playbackBar.stopCallback = nullptr;
        gui::playbackBar.scrubCallback = nullptr;
        gui::playbackBar.setLoopMarkerCallback = nullptr;
        gui::playbackBar.clearLoopMarkerCallback = nullptr;
        gui::playbackBar.setLoopEnabledCallback = nullptr;
        gui::playbackBar.transportCtx = nullptr;
        gui::playbackBar.recordingStartEpoch = 0;
        gui::playbackBar.recordingStartStr.clear();
        flog::info("FileSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        if (_this->running) { return; }
        if (_this->reader == NULL) { return; }
        _this->running = true;
#ifdef _WIN32
        // paceToRealTime() targets ~5ms blocks, but Windows' default scheduler timer
        // resolution is ~15.6ms: measured directly, std::this_thread::sleep_until() at this
        // granularity delivers blocks in a 0ms/~15.5ms alternating burst-then-stall pattern
        // instead of an even ~5ms cadence -- audible as stuttering, sounds exactly like a
        // bit rate mismatch even though the data and rate are both correct. macOS's default
        // sleep granularity doesn't have this floor, which is why this was invisible there.
        // timeBeginPeriod(1) raises the whole process's timer resolution to 1ms for as long
        // as playback runs; confirmed by direct measurement to bring sleep_until() back to
        // a tight ~3-7ms spread around the 5ms target with no bursts. Standard, widely-used
        // fix for exactly this class of problem (games, audio engines); has no effect on
        // Linux/macOS, which don't have this coarse a floor to begin with.
        timeBeginPeriod(1);
#endif
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
#ifdef _WIN32
        timeEndPeriod(1);   // Paired with start()'s timeBeginPeriod(1) -- see its comment.
#endif
        _this->stream.clearWriteStop();
        _this->streamA.clearWriteStop();
        _this->streamB.clearWriteStop();
        _this->running = false;
        _this->reader->rewind();
        // A fresh global Play always starts un-paused and not mid-scrub. Deliberately NOT
        // touching loopEnabled/loopMarkerA/loopMarkerB here -- a global stop/restart of the same
        // file is meant to preserve loop state (only loading a *different* file, in menuHandler
        // above, resets it), since stopping to fine-tune something mid-search shouldn't lose
        // markers you've already placed.
        _this->paused.store(false);
        _this->scrubDir.store(0);
        gui::playbackBar.active = false;
        gui::playbackBar.progress = 0.0f;
        gui::playbackBar.currentTimeSec = 0.0f;
        gui::playbackBar.paused = false;
        gui::playbackBar.scrubbingForward = false;
        gui::playbackBar.scrubbingReverse = false;
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

    // Transport callbacks. Every one of these, like seekBarCallback above, touches only atomics
    // and gui:: fields -- never `reader` -- since `reader` may only ever be called from the
    // worker thread (see the member comment on the atomics themselves).

    static void playPauseCallback(bool play, void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        _this->paused.store(!play);
        if (play) { _this->scrubDir.store(0); }   // resuming cancels any stuck scrub
        gui::playbackBar.paused = !play;
    }

    static void stopToStartCallback(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        _this->scrubDir.store(0);
        _this->paused.store(true);
        _this->stopToStartPending.store(true);   // worker does the actual seekToFraction(0)
        gui::playbackBar.paused = true;
        gui::playbackBar.scrubbingForward = false;
        gui::playbackBar.scrubbingReverse = false;
    }

    // direction: -1/0/+1. Called every GUI frame with the live held-button state, not a one-shot
    // toggle -- main_window.cpp calls this with the current IsMouseDown() result each frame
    // while a fast-forward/reverse button region is being pressed.
    static void scrubCallback(int direction, void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        _this->scrubDir.store(direction);
        gui::playbackBar.scrubbingForward = (direction > 0);
        gui::playbackBar.scrubbingReverse = (direction < 0);
    }

    // which: 0 = marker A, 1 = marker B. Shared by both ways of setting a marker -- the "Set
    // A"/"Set B" buttons (called with gui::playbackBar.progress) and dragging directly on the
    // timeline (called with the mouse-derived fraction) both just call this.
    static void setLoopMarkerCallback(int which, float fraction, void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        if (which == 0) {
            _this->loopMarkerA.store(fraction);
            gui::playbackBar.loopMarkerAFrac = fraction;
        }
        else {
            _this->loopMarkerB.store(fraction);
            gui::playbackBar.loopMarkerBFrac = fraction;
        }
    }

    static void setLoopEnabledCallback(bool enabled, void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        _this->loopEnabled.store(enabled);
        gui::playbackBar.loopEnabled = enabled;
    }

    // which: 0 = marker A, 1 = marker B. Unsets the marker entirely (back to -1, meaning
    // "not placed") rather than just repositioning it. Also turns looping off -- a loop
    // missing either endpoint can't run, and applyLoopBoundary() would just silently do
    // nothing until both are set again, which would leave the Loop button looking "on" while
    // not actually doing anything.
    static void clearLoopMarkerCallback(int which, void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        if (which == 0) {
            _this->loopMarkerA.store(-1.0f);
            gui::playbackBar.loopMarkerAFrac = -1.0f;
        }
        else {
            _this->loopMarkerB.store(-1.0f);
            gui::playbackBar.loopMarkerBFrac = -1.0f;
        }
        _this->loopEnabled.store(false);
        gui::playbackBar.loopEnabled = false;
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
                    // PCM width when not float -- the Recorder's SampleType covers uint8/
                    // int16/int32 (RSR200's 24-bit mode is padded into int32; there's no
                    // separate 24-bit container), but worker()/dualWorker() used to assume
                    // int16 unconditionally, hitting the exact same half-byte-stride bug as
                    // the float case above for any file that wasn't actually 16-bit PCM.
                    _this->pcmBits = _this->reader->getBitDepth();
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
                    gui::playbackBar.playPauseCallback = playPauseCallback;
                    gui::playbackBar.stopCallback = stopToStartCallback;
                    gui::playbackBar.scrubCallback = scrubCallback;
                    gui::playbackBar.setLoopMarkerCallback = setLoopMarkerCallback;
                    gui::playbackBar.clearLoopMarkerCallback = clearLoopMarkerCallback;
                    gui::playbackBar.setLoopEnabledCallback = setLoopEnabledCallback;
                    gui::playbackBar.transportCtx = _this;
                    // A newly-loaded file's transport/loop state starts fresh -- markers from
                    // whatever was previously open are meaningless here. (A *global* stop/
                    // restart of the same file, by contrast, deliberately leaves loop state
                    // alone -- see stop()'s own comment.)
                    _this->paused.store(false);
                    _this->scrubDir.store(0);
                    _this->loopEnabled.store(false);
                    _this->loopMarkerA.store(-1.0f);
                    _this->loopMarkerB.store(-1.0f);
                    gui::playbackBar.paused = false;
                    gui::playbackBar.scrubbingForward = false;
                    gui::playbackBar.scrubbingReverse = false;
                    gui::playbackBar.loopEnabled = false;
                    gui::playbackBar.loopMarkerAFrac = -1.0f;
                    gui::playbackBar.loopMarkerBFrac = -1.0f;
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
        if (_this->reader != NULL) {
            ImGui::Text("Detected: %d-bit %s", _this->pcmBits, _this->float32Mode ? "float" : "PCM");
        }
    }

    // Reads `count` interleaved PCM values (frames * channels) from the reader, converting
    // whatever raw width the file actually uses into `out`. Centralised so worker() and
    // dualWorker() can't drift out of sync on which PCM widths they support -- both used to
    // silently assume 16-bit regardless of what the Recorder's SampleType (and RSR200's
    // 24-bit-padded-into-32-bit output) actually wrote, misreading the byte stride the same
    // way the float/int16 confusion this was found alongside did.
    static void readPCM(FileSourceModule* _this, float* out, int count, std::vector<uint8_t>& rawBuf) {
        switch (_this->pcmBits) {
        case 32: {
            rawBuf.resize((size_t)count * sizeof(int32_t));
            _this->reader->readSamples(rawBuf.data(), rawBuf.size());
            volk_32i_s32f_convert_32f(out, (const int32_t*)rawBuf.data(), 2147483647.0f, count);
            break;
        }
        case 8: {
            rawBuf.resize((size_t)count * sizeof(uint8_t));
            _this->reader->readSamples(rawBuf.data(), rawBuf.size());
            // Volk has no unsigned-int conversion kernel (wav.cpp's writer notes the same
            // gap) -- exact inverse of the writer's (samples[i] * 127.0f) + 128.0f.
            for (int i = 0; i < count; i++) { out[i] = ((float)rawBuf[i] - 128.0f) / 127.0f; }
            break;
        }
        default: {   // 16-bit, and the fallback for any width we don't otherwise recognise
            rawBuf.resize((size_t)count * sizeof(int16_t));
            _this->reader->readSamples(rawBuf.data(), rawBuf.size());
            volk_16i_s32f_convert_32f(out, (const int16_t*)rawBuf.data(), 32768.0f, count);
            break;
        }
        }
    }

    // Paces a worker loop to real time, exactly as phasing_test_source's generator already
    // does (see its own worker() for the original). Without this, a file source has no
    // reason at all to run at anything resembling the recording's real rate -- swap() only
    // blocks on downstream backpressure, so the loop delivers blocks in whatever bursty,
    // CPU-scheduling-dependent pattern backpressure happens to allow, rather than the even,
    // predictably-paced cadence live hardware (or phasing_test_source's own synthetic
    // generator) naturally provides. That mattered in practice, not just in theory: a real
    // dual-channel RSR200 recording nulled 20+ dB shallower on playback than the same
    // antennas nulled live, with reference band and gain settings confirmed identical
    // between the two -- decorrelation's adaptive solve is sensitive to how evenly-spaced
    // the blocks feeding it are, and unpaced playback wasn't delivering that. Resets to
    // `now` after a seek, since the old schedule no longer means anything relative to the
    // new position -- without that, a seek would either sleep out a long stale interval or
    // spend a while blasting through blocks trying to "catch up" to a schedule that was
    // never real to begin with.
    static void paceToRealTime(std::chrono::steady_clock::time_point& nextBlock, int blockSize, double sampleRate) {
        nextBlock += std::chrono::nanoseconds((int64_t)(1e9 * (double)blockSize / sampleRate));
        const auto now = std::chrono::steady_clock::now();
        if (nextBlock < now) { nextBlock = now; }
        std::this_thread::sleep_until(nextBlock);
    }

    // How fast fast-forward/reverse scrubs, in file-seconds skipped per real-world tick -- e.g.
    // 2.0s of file per 30ms wall-clock tick is roughly 66x realtime. Tune by feel; nothing else
    // depends on the exact numbers.
    static constexpr double kScrubSecondsPerTick = 2.0;
    static constexpr int kScrubTickMs = 30;

    // Shared by all three worker loops (see the "identical in worker/dualWorker/floatWorker"
    // shape below) so the three can't drift out of sync the way readPCM's own comment warns
    // about for PCM width handling. Handles the new local Stop (seek to 0), silent scrub, and
    // pause -- all three skip the normal read/write/pace path entirely, which is what makes
    // scrub and pause silent: dsp::stream::swap() is the only thing that ever hands a block to
    // the downstream DSP chain, and none of these three call it. Returns true if the caller
    // should skip straight to its next loop iteration (a scrub tick or a paused idle tick was
    // handled instead of normal playback); false means proceed with a normal read/swap/pace.
    static bool handleTransportState(FileSourceModule* _this, std::chrono::steady_clock::time_point& nextBlock) {
        if (_this->stopToStartPending.exchange(false)) {
            _this->reader->seekToFraction(0.0f);
            gui::playbackBar.progress = 0.0f;
            gui::playbackBar.currentTimeSec = 0.0f;
            nextBlock = std::chrono::steady_clock::now();
        }

        int dir = _this->scrubDir.load();
        if (dir != 0) {
            uint64_t dataSize = _this->reader->getDataSize();
            double bps = (double)_this->reader->getBytesPerSecond();
            if (dataSize > 0 && bps > 0) {
                double curByte = (double)_this->reader->getCurrentByteOffset();
                double newByte = std::clamp(curByte + kScrubSecondsPerTick * bps * (double)dir, 0.0, (double)dataSize);
                _this->reader->seekToFraction((float)(newByte / (double)dataSize));
                gui::playbackBar.progress = (float)(newByte / (double)dataSize);
                gui::playbackBar.currentTimeSec = (float)(newByte / bps);
                // Clamped, not wrapped -- scrubbing off either end of the file just stops there
                // and disengages, rather than wrapping around to the other end.
                if (newByte <= 0.0 || newByte >= (double)dataSize) {
                    _this->scrubDir.store(0);
                    gui::playbackBar.scrubbingForward = false;
                    gui::playbackBar.scrubbingReverse = false;
                }
            }
            // Reset the pacing schedule before sleeping so paceToRealTime() doesn't try to
            // "catch up" a schedule that went stale while scrubbing, once normal playback
            // resumes -- same trick applied everywhere else a seek changes position out from
            // under the pacing clock (see paceToRealTime()'s own comment).
            nextBlock = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(kScrubTickMs));
            return true;
        }

        if (_this->paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            nextBlock = std::chrono::steady_clock::now();
            return true;
        }

        return false;
    }

    // Called once per block, right after progress/currentTimeSec are updated and before
    // swap(). Redirects the reader back to the loop start once playback crosses the loop end --
    // at ~200 blocks/sec this pre-empts WavReader::readSamples()'s own EOF-wrap-to-absolute-
    // start (see wavreader.h) in the overwhelming majority of cases, since the reader's position
    // essentially never gets to reach true EOF while a loop is keeping it inside a much smaller
    // range. (Edge case, considered and accepted: if the loop end sits within one block of true
    // EOF, that read could still hit the reader's own EOF-wrap first, briefly resuming from
    // absolute byte 0 for a single ~5ms block before this check catches it on the next
    // iteration and redirects to the loop start anyway -- an inaudible glitch for the "find and
    // repeat a station ID" use case this is for, not worth a predictive pre-read variant.)
    static void applyLoopBoundary(FileSourceModule* _this, uint64_t byteOff, uint64_t dataSize, std::chrono::steady_clock::time_point& nextBlock) {
        if (!_this->loopEnabled.load() || dataSize == 0) { return; }
        float a = _this->loopMarkerA.load(), b = _this->loopMarkerB.load();
        if (a < 0.0f || b < 0.0f) { return; }
        float loStart = std::min(a, b), loEnd = std::max(a, b);
        if (byteOff >= (uint64_t)((double)loEnd * (double)dataSize)) {
            _this->reader->seekToFraction(loStart);
            nextBlock = std::chrono::steady_clock::now();
        }
    }

    static void worker(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        double sampleRate = std::max(_this->reader->getSampleRate(), (uint32_t)1);
        int blockSize = std::min((int)(sampleRate / 200.0f), (int)STREAM_BUFFER_SIZE);
        std::vector<uint8_t> rawBuf;
        auto nextBlock = std::chrono::steady_clock::now();

        while (true) {
            if (_this->seekPending.exchange(false)) {
                _this->reader->seekToFraction(_this->seekFraction.load());
                nextBlock = std::chrono::steady_clock::now();
            }
            if (handleTransportState(_this, nextBlock)) { continue; }
            readPCM(_this, (float*)_this->stream.writeBuf, blockSize * 2, rawBuf);
            uint64_t dataSize = _this->reader->getDataSize();
            size_t byteOff = 0;
            if (dataSize > 0) {
                byteOff = _this->reader->getCurrentByteOffset();
                gui::playbackBar.progress = (float)((double)byteOff / (double)dataSize);
                gui::playbackBar.currentTimeSec = (float)byteOff / (float)_this->reader->getBytesPerSecond();
            }
            applyLoopBoundary(_this, byteOff, dataSize, nextBlock);
            if (!_this->stream.swap(blockSize)) { break; };
            paceToRealTime(nextBlock, blockSize, sampleRate);
        }
    }

    // Four WAV channels are two complex channels interleaved as I1 Q1 I2 Q2. Split them
    // back apart and hand both to the phasing front end, so a dual channel recording can
    // be re-phased exactly as if the radio were still connected.
    static void dualWorker(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        double sampleRate = std::max(_this->reader->getSampleRate(), (uint32_t)1);
        int blockSize = std::min((int)(sampleRate / 200.0f), (int)STREAM_BUFFER_SIZE);
        std::vector<uint8_t> rawBuf;
        std::vector<float> fBuf(blockSize * 4);
        auto nextBlock = std::chrono::steady_clock::now();

        while (true) {
            if (_this->seekPending.exchange(false)) {
                _this->reader->seekToFraction(_this->seekFraction.load());
                nextBlock = std::chrono::steady_clock::now();
            }
            if (handleTransportState(_this, nextBlock)) { continue; }

            if (_this->float32Mode) {
                _this->reader->readSamples(fBuf.data(), blockSize * 4 * sizeof(float));
            }
            else {
                readPCM(_this, fBuf.data(), blockSize * 4, rawBuf);
            }

            for (int i = 0; i < blockSize; i++) {
                _this->streamA.writeBuf[i] = { fBuf[4 * i + 0], fBuf[4 * i + 1] };
                _this->streamB.writeBuf[i] = { fBuf[4 * i + 2], fBuf[4 * i + 3] };
            }

            uint64_t dataSize = _this->reader->getDataSize();
            size_t byteOff = 0;
            if (dataSize > 0) {
                byteOff = _this->reader->getCurrentByteOffset();
                gui::playbackBar.progress = (float)((double)byteOff / (double)dataSize);
                gui::playbackBar.currentTimeSec = (float)byteOff / (float)_this->reader->getBytesPerSecond();
            }
            applyLoopBoundary(_this, byteOff, dataSize, nextBlock);

            // Swapped in the order the phaser reads them.
            if (!_this->streamA.swap(blockSize)) { break; }
            if (!_this->streamB.swap(blockSize)) { break; }
            paceToRealTime(nextBlock, blockSize, sampleRate);
        }
    }

    static void floatWorker(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        double sampleRate = std::max(_this->reader->getSampleRate(), (uint32_t)1);
        int blockSize = std::min((int)(sampleRate / 200.0f), (int)STREAM_BUFFER_SIZE);
        dsp::complex_t* inBuf = new dsp::complex_t[blockSize];
        auto nextBlock = std::chrono::steady_clock::now();

        while (true) {
            if (_this->seekPending.exchange(false)) {
                _this->reader->seekToFraction(_this->seekFraction.load());
                nextBlock = std::chrono::steady_clock::now();
            }
            if (handleTransportState(_this, nextBlock)) { continue; }
            _this->reader->readSamples(_this->stream.writeBuf, blockSize * sizeof(dsp::complex_t));
            uint64_t dataSize = _this->reader->getDataSize();
            size_t byteOff = 0;
            if (dataSize > 0) {
                byteOff = _this->reader->getCurrentByteOffset();
                gui::playbackBar.progress = (float)((double)byteOff / (double)dataSize);
                gui::playbackBar.currentTimeSec = (float)byteOff / (float)_this->reader->getBytesPerSecond();
            }
            applyLoopBoundary(_this, byteOff, dataSize, nextBlock);
            if (!_this->stream.swap(blockSize)) { break; };
            paceToRealTime(nextBlock, blockSize, sampleRate);
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

    // Transport control state -- same cross-thread convention as seekPending/seekFraction
    // above: the GUI thread (via gui::playbackBar's callbacks) only ever writes these atomics,
    // never touches `reader` directly; only the worker thread calls into `reader`, which is not
    // itself thread-safe. See gui.h's PlaybackBarInfo for what each of these mirrors.
    std::atomic<bool> paused{false};
    std::atomic<int> scrubDir{0};                 // -1 reverse, 0 off, +1 forward
    std::atomic<bool> stopToStartPending{false};   // the new local "Stop": pause + seek to 0
    std::atomic<bool> loopEnabled{false};
    std::atomic<float> loopMarkerA{-1.0f};
    std::atomic<float> loopMarkerB{-1.0f};

    double centerFreq = 100000000;

    bool float32Mode = false;
    int pcmBits = 16;   // 8, 16, or 32 -- which raw PCM width worker()/dualWorker() read
                        // when float32Mode is false. Auto-detected from the file's own
                        // bitDepth field on load; see menuHandler().
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
