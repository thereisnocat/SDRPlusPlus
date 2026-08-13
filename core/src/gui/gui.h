#pragma once
#include <gui/widgets/waterfall.h>
#include <gui/widgets/frequency_select.h>
#include <gui/widgets/menu.h>
#include <gui/dialogs/loading_screen.h>
#include <module.h>
#include <gui/main_window.h>
#include <gui/theme_manager.h>
#include <string>
#include <stdint.h>
namespace gui {
    SDRPP_EXPORT ImGui::WaterFall waterfall;
    SDRPP_EXPORT FrequencySelect freqSelect;
    SDRPP_EXPORT Menu menu;
    SDRPP_EXPORT ThemeManager themeManager;
    SDRPP_EXPORT MainWindow mainWindow;
    
    struct PlaybackBarInfo {
        bool active = false;
        float progress = 0.0f;        // 0.0 to 1.0
        float currentTimeSec = 0.0f;
        float totalTimeSec = 0.0f;
        std::string recordingStartStr;   // e.g. "2024-10-24 16:56:37"
        int64_t recordingStartEpoch = 0; // local-time epoch of recording start, 0 = unknown
        void (*seekCallback)(float fraction, void* ctx) = nullptr;
        void* seekCtx = nullptr;

        // Transport state -- read by the GUI, written by the source module's callbacks and its
        // own worker thread, same unlocked-write convention already used above for
        // progress/currentTimeSec (this is an ImGui-immediate-mode display value, not something
        // that needs a mutex to stay usable).
        bool paused = false;
        bool scrubbingForward = false;   // true while fast-forward is held, for button highlight
        bool scrubbingReverse = false;   // true while fast-reverse is held

        // A/B loop state. -1 = that marker hasn't been set yet. Order-agnostic by convention --
        // every consumer treats the loop bounds as min(A,B)/max(A,B), so which one the user
        // dropped first never matters.
        bool loopEnabled = false;
        float loopMarkerAFrac = -1.0f;
        float loopMarkerBFrac = -1.0f;

        // Transport callbacks. One shared ctx (transportCtx) rather than a ctx per callback --
        // unlike seekCtx above (which predates this and is left alone), there's only ever one
        // real producer of all five of these, so five separate ctx pointers would just be five
        // copies of the same value.
        void (*playPauseCallback)(bool play, void* ctx) = nullptr;
        void (*stopCallback)(void* ctx) = nullptr;   // pause + rewind to 0, does NOT tear down
                                                      // the source -- see main_window.cpp's
                                                      // existing global Play/Stop button for that
        void (*scrubCallback)(int direction, void* ctx) = nullptr; // -1/0/+1; called every GUI
                                                                    // frame with the live
                                                                    // held-button state, not a
                                                                    // one-shot toggle
        void (*setLoopMarkerCallback)(int which, float fraction, void* ctx) = nullptr; // 0=A,1=B
        void (*clearLoopMarkerCallback)(int which, void* ctx) = nullptr; // 0=A,1=B -- unsets it
                                                                          // (back to -1) and turns
                                                                          // looping off, since a
                                                                          // loop missing either
                                                                          // marker can't run
        void (*setLoopEnabledCallback)(bool enabled, void* ctx) = nullptr;
        void* transportCtx = nullptr;
    };
    SDRPP_EXPORT PlaybackBarInfo playbackBar;

    void selectSource(std::string name);
};