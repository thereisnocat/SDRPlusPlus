#pragma once
#include <imgui/imgui.h>
#include <fftw3.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <signal_path/vfo_manager.h>
#include <string>
#include <utils/event.h>
#include <mutex>
#include <gui/tuner.h>

#define WINDOW_FLAGS ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground

class MainWindow {
public:
    void init();
    void draw();
    void setViewBandwidthSlider(float bandwidth);
    bool sdrIsRunning();
    void setFirstMenuRender();

    static float* acquireFFTBuffer(void* ctx);
    static void releaseFFTBuffer(void* ctx);

    // TODO: Replace with it's own class
    void setVFO(double freq);

    void setPlayState(bool _playing);
    bool isPlaying();

    bool lockWaterfallControls = false;
    bool playButtonLocked = false;

    Event<bool> onPlayStateChange;

private:
    static void vfoAddedHandler(VFOManager::VFO* vfo, void* ctx);

    // FFT Variables
    int fftSize = 8192 * 8;
    std::mutex fft_mtx;
    fftwf_complex *fft_in, *fft_out;
    fftwf_plan fftwPlan;

    // GUI Variables
    bool firstMenuRender = true;
    bool startedWithMenuClosed = false;
    float fftMin = -70.0;
    float fftMax = 0.0;
    float bw = 8000000;
    // `playing` used to be touched only from the GUI thread (the Play/Stop button, ultimately
    // via setPlayState()); the recording scheduler's engine thread (RECORDING_SCHEDULER_PLAN.md
    // phase 5, extended 2026-08-15 to actually call setPlayState() once sigpath::sourceManager
    // itself was made thread-safe) is the first thing to call setPlayState()/sdrIsRunning()/
    // isPlaying() from anywhere else. recursive_mutex: setPlayState() calls
    // sigpath::sourceManager.start()/stop()/tune(), which is safe on its own now, but
    // onPlayStateChange.emit() (Event::emit() calls handlers synchronously on the calling
    // thread, utils/event.h) could in principle reach back into isPlaying()/sdrIsRunning() on
    // the same thread -- same-thread reentry, which recursive_mutex allows and plain
    // std::mutex would deadlock on. Never held while calling into core::modComManager or
    // acquiring sigpath::sourceManager's own lock from a *different* nesting order than
    // setPlayState()'s own (this lock outer, SourceManager's inner) -- matches the ordering
    // discipline documented in core/src/signal_path/source.h.
    mutable std::recursive_mutex playStateMtx;
    bool playing = false;
    bool showCredits = false;
    std::string audioStreamName = "";
    std::string sourceName = "";
    int menuWidth = 300;
    bool grabbingMenu = false;
    int newWidth = 300;
    int draggingLoopMarker = -1;  // -1 = none, 0 = dragging loop marker A, 1 = dragging B
    int fftHeight = 300;
    bool showMenu = true;
    int tuningMode = tuner::TUNER_MODE_NORMAL;
    dsp::stream<dsp::complex_t> dummyStream;
    bool demoWindow = false;
    int selectedWindow = 0;

    bool initComplete = false;
    bool autostart = false;

    EventHandler<VFOManager::VFO*> vfoCreatedHandler;
};