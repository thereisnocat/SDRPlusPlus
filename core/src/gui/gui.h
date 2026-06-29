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
    };
    SDRPP_EXPORT PlaybackBarInfo playbackBar;

    void selectSource(std::string name);
};