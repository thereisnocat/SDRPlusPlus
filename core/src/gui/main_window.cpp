#include <gui/main_window.h>
#include <gui/gui.h>
#include "imgui.h"
#include <stdio.h>
#include <ctime>
#include <thread>
#include <complex>
#include <functional>
#include <cmath>
#include <gui/widgets/waterfall.h>
#include <gui/widgets/frequency_select.h>
#include <signal_path/iq_frontend.h>
#include <gui/icons.h>
#include <gui/widgets/bandplan.h>
#include <gui/style.h>
#include <config.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/menus/source.h>
#include <gui/menus/display.h>
#include <gui/menus/bandplan.h>
#include <gui/menus/sink.h>
#include <gui/menus/vfo_color.h>
#include <gui/menus/module_manager.h>
#include <gui/menus/theme.h>
#include <gui/dialogs/credits.h>
#include <filesystem>
#include <signal_path/source.h>
#include <gui/dialogs/loading_screen.h>
#include <gui/colormaps.h>
#include <gui/widgets/snr_meter.h>
#include <gui/tuner.h>

void MainWindow::init() {
    LoadingScreen::show("Initializing UI");
    gui::waterfall.init();
    gui::waterfall.setRawFFTSize(fftSize);

    credits::init();

    core::configManager.acquire();
    json menuElements = core::configManager.conf["menuElements"];
    std::string modulesDir = core::configManager.conf["modulesDirectory"];
    std::string resourcesDir = core::configManager.conf["resourcesDirectory"];
    core::configManager.release();

    // Assert that directories are absolute
    modulesDir = std::filesystem::absolute(modulesDir).string();
    resourcesDir = std::filesystem::absolute(resourcesDir).string();

    // Load menu elements
    gui::menu.order.clear();
    for (auto& elem : menuElements) {
        if (!elem.contains("name")) {
            flog::error("Menu element is missing name key");
            continue;
        }
        if (!elem["name"].is_string()) {
            flog::error("Menu element name isn't a string");
            continue;
        }
        if (!elem.contains("open")) {
            flog::error("Menu element is missing open key");
            continue;
        }
        if (!elem["open"].is_boolean()) {
            flog::error("Menu element name isn't a string");
            continue;
        }
        Menu::MenuOption_t opt;
        opt.name = elem["name"];
        opt.open = elem["open"];
        gui::menu.order.push_back(opt);
    }

    gui::menu.registerEntry("Source", sourcemenu::draw, NULL);
    gui::menu.registerEntry("Sinks", sinkmenu::draw, NULL);
    gui::menu.registerEntry("Band Plan", bandplanmenu::draw, NULL);
    gui::menu.registerEntry("Display", displaymenu::draw, NULL);
    gui::menu.registerEntry("Theme", thememenu::draw, NULL);
    gui::menu.registerEntry("VFO Color", vfo_color_menu::draw, NULL);
    gui::menu.registerEntry("Module Manager", module_manager_menu::draw, NULL);

    gui::freqSelect.init();

    // Set default values for waterfall in case no source init's it
    gui::waterfall.setBandwidth(8000000);
    gui::waterfall.setViewBandwidth(8000000);

    fft_in = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fft_out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fftwPlan = fftwf_plan_dft_1d(fftSize, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);

    sigpath::iqFrontEnd.init(&dummyStream, 8000000, true, 1, false, 1024, 20.0, IQFrontEnd::FFTWindow::NUTTALL, acquireFFTBuffer, releaseFFTBuffer, this);
    sigpath::iqFrontEnd.start();

    // Must be ready before any source module is loaded, since a source may declare its
    // channels as soon as it is instantiated.
    sigpath::phasing.init();

    vfoCreatedHandler.handler = vfoAddedHandler;
    vfoCreatedHandler.ctx = this;
    sigpath::vfoManager.onVfoCreated.bindHandler(&vfoCreatedHandler);

    flog::info("Loading modules");

    // Load modules from /module directory
    if (std::filesystem::is_directory(modulesDir)) {
        for (const auto& file : std::filesystem::directory_iterator(modulesDir)) {
            std::string path = file.path().generic_string();
            if (file.path().extension().generic_string() != SDRPP_MOD_EXTENTSION) {
                continue;
            }
            if (!file.is_regular_file()) { continue; }
            flog::info("Loading {0}", path);
            LoadingScreen::show("Loading " + file.path().filename().string());
            core::moduleManager.loadModule(path);
        }
    }
    else {
        flog::warn("Module directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    // Read module config
    core::configManager.acquire();
    std::vector<std::string> modules = core::configManager.conf["modules"];
    auto modList = core::configManager.conf["moduleInstances"].items();
    core::configManager.release();

    // Load additional modules specified through config
    for (auto const& path : modules) {
#ifndef __ANDROID__
        std::string apath = std::filesystem::absolute(path).string();
        flog::info("Loading {0}", apath);
        LoadingScreen::show("Loading " + std::filesystem::path(path).filename().string());
        core::moduleManager.loadModule(apath);
#else
        core::moduleManager.loadModule(path);
#endif
    }

    // Create module instances
    for (auto const& [name, _module] : modList) {
        std::string mod = _module["module"];
        bool enabled = _module["enabled"];
        flog::info("Initializing {0} ({1})", name, mod);
        LoadingScreen::show("Initializing " + name + " (" + mod + ")");
        core::moduleManager.createInstance(name, mod);
        if (!enabled) { core::moduleManager.disableInstance(name); }
    }

    // Load color maps
    LoadingScreen::show("Loading color maps");
    flog::info("Loading color maps");
    if (std::filesystem::is_directory(resourcesDir + "/colormaps")) {
        for (const auto& file : std::filesystem::directory_iterator(resourcesDir + "/colormaps")) {
            std::string path = file.path().generic_string();
            LoadingScreen::show("Loading " + file.path().filename().string());
            flog::info("Loading {0}", path);
            if (file.path().extension().generic_string() != ".json") {
                continue;
            }
            if (!file.is_regular_file()) { continue; }
            colormaps::loadMap(path);
        }
    }
    else {
        flog::warn("Color map directory {0} does not exist, not loading modules from directory", modulesDir);
    }

    gui::waterfall.updatePalletteFromArray(colormaps::maps["Turbo"].map, colormaps::maps["Turbo"].entryCount);

    sourcemenu::init();
    sinkmenu::init();
    bandplanmenu::init();
    displaymenu::init();
    vfo_color_menu::init();
    module_manager_menu::init();

    // TODO for 0.2.5
    // Fix gain not updated on startup, soapysdr

    // Update UI settings
    LoadingScreen::show("Loading configuration");
    core::configManager.acquire();
    fftMin = core::configManager.conf["min"];
    fftMax = core::configManager.conf["max"];
    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMax(fftMax);

    double frequency = core::configManager.conf["frequency"];

    showMenu = core::configManager.conf["showMenu"];
    startedWithMenuClosed = !showMenu;

    gui::freqSelect.setFrequency(frequency);
    gui::freqSelect.frequencyChanged = false;
    sigpath::sourceManager.tune(frequency);
    gui::waterfall.setCenterFrequency(frequency);
    bw = 1.0;
    gui::waterfall.vfoFreqChanged = false;
    gui::waterfall.centerFreqMoved = false;
    gui::waterfall.selectFirstVFO();

    menuWidth = core::configManager.conf["menuWidth"];
    newWidth = menuWidth;

    fftHeight = core::configManager.conf["fftHeight"];
    gui::waterfall.setFFTHeight(fftHeight);

    tuningMode = core::configManager.conf["centerTuning"] ? tuner::TUNER_MODE_CENTER : tuner::TUNER_MODE_NORMAL;
    gui::waterfall.VFOMoveSingleClick = (tuningMode == tuner::TUNER_MODE_CENTER);

    core::configManager.release();

    // Correct the offset of all VFOs so that they fit on the screen
    float finalBwHalf = gui::waterfall.getBandwidth() / 2.0;
    for (auto& [_name, _vfo] : gui::waterfall.vfos) {
        if (_vfo->lowerOffset < -finalBwHalf) {
            sigpath::vfoManager.setCenterOffset(_name, (_vfo->bandwidth / 2) - finalBwHalf);
            continue;
        }
        if (_vfo->upperOffset > finalBwHalf) {
            sigpath::vfoManager.setCenterOffset(_name, finalBwHalf - (_vfo->bandwidth / 2));
            continue;
        }
    }

    autostart = core::args["autostart"].b();
    initComplete = true;

    core::moduleManager.doPostInitAll();
}

float* MainWindow::acquireFFTBuffer(void* ctx) {
    return gui::waterfall.getFFTBuffer();
}

void MainWindow::releaseFFTBuffer(void* ctx) {
    gui::waterfall.pushFFT();
}

void MainWindow::vfoAddedHandler(VFOManager::VFO* vfo, void* ctx) {
    MainWindow* _this = (MainWindow*)ctx;
    std::string name = vfo->getName();
    core::configManager.acquire();
    if (!core::configManager.conf["vfoOffsets"].contains(name)) {
        core::configManager.release();
        return;
    }
    double offset = core::configManager.conf["vfoOffsets"][name];
    core::configManager.release();

    double viewBW = gui::waterfall.getViewBandwidth();
    double viewOffset = gui::waterfall.getViewOffset();

    double viewLower = viewOffset - (viewBW / 2.0);
    double viewUpper = viewOffset + (viewBW / 2.0);

    double newOffset = std::clamp<double>(offset, viewLower, viewUpper);

    sigpath::vfoManager.setCenterOffset(name, _this->initComplete ? newOffset : offset);
}

// -----------------------------------------------------------------------------------------
// Playback bar transport controls (RSR200_PLAN.md-adjacent feature, see the plan file for the
// A/B loop workflow this is for: scanning a long baseband recording for a station ID, then
// repeating just that section). No PAUSE/FF/RW/loop icon assets exist in this repo, and there's
// no icon-authoring tooling available to add them -- every glyph below is drawn procedurally
// with ImDrawList primitives instead, the same precedent the seek bar's own progress fill
// (further down in this file) already established for this exact playback bar.
// -----------------------------------------------------------------------------------------

// Shared hit-region + background chrome for every new playback-bar button, so the five
// transport buttons and three marker/loop buttons don't each reimplement the same
// InvisibleButton/hover/disabled boilerplate. heldOut, if non-null, reports whether the button
// is currently being pressed (not just clicked) -- used by fast-forward/reverse, which scrub
// for as long as the button is held rather than toggling on a single click. rightClickedOut, if
// non-null, reports a right-click separately from the (left-click) return value -- used by
// Set A/Set B to clear a marker without needing a whole separate button for it.
//
// activeColor, if non-zero, overrides the theme's ImGuiCol_ButtonActive for a *persistently*
// toggled-on state (as opposed to "currently being pressed"), with a white outline added on
// top -- ImGuiCol_ButtonActive alone turned out to read as barely different from the normal
// button color in this theme, which was the whole reason the Loop toggle didn't look like it
// had done anything (live feedback: "no indication of state; I can't tell when it's been
// invoked"). Only Loop uses this; the others don't have a comparable persistent-on state.
static bool transportGlyphButton(const char* id, ImVec2 size,
                                  const std::function<void(ImDrawList*, ImVec2, ImVec2)>& drawGlyph,
                                  bool active, bool disabled, bool* heldOut = nullptr,
                                  ImU32 activeColor = 0, bool* rightClickedOut = nullptr) {
    ImGui::PushID(id);
    if (disabled) { style::beginDisabled(); }
    ImGui::InvisibleButton("##btn", size);
    bool held = !disabled && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool clicked = !disabled && ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool rightClicked = !disabled && ImGui::IsItemClicked(ImGuiMouseButton_Right);
    if (heldOut) { *heldOut = held; }
    if (rightClickedOut) { *rightClickedOut = rightClicked; }
    ImVec2 tl = ImGui::GetItemRectMin();
    ImVec2 br = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool useActiveColor = active && activeColor != 0;
    ImU32 bg = useActiveColor ? activeColor
               : (active || held) ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
               : (!disabled && ImGui::IsItemHovered()) ? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
                                                          : ImGui::GetColorU32(ImGuiCol_Button);
    dl->AddRectFilled(tl, br, bg, 3.0f * style::uiScale);
    if (useActiveColor) {
        dl->AddRect(tl, br, IM_COL32(255, 255, 255, 220), 3.0f * style::uiScale, 0, 2.0f * style::uiScale);
    }
    drawGlyph(dl, tl, br);
    if (disabled) { style::endDisabled(); }
    ImGui::PopID();
    return clicked;
}

static void drawPlayGlyph(ImDrawList* dl, ImVec2 tl, ImVec2 br) {
    ImVec2 c((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);
    float s = std::min(br.x - tl.x, br.y - tl.y) * 0.28f;
    ImU32 col = IM_COL32(255, 255, 255, 255);
    dl->AddTriangleFilled(ImVec2(c.x - s * 0.6f, c.y - s), ImVec2(c.x - s * 0.6f, c.y + s), ImVec2(c.x + s, c.y), col);
}

static void drawPauseGlyph(ImDrawList* dl, ImVec2 tl, ImVec2 br) {
    ImVec2 c((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);
    float s = std::min(br.x - tl.x, br.y - tl.y) * 0.28f;
    float barW = s * 0.55f;
    ImU32 col = IM_COL32(255, 255, 255, 255);
    dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x - s + barW, c.y + s), col);
    dl->AddRectFilled(ImVec2(c.x + s - barW, c.y - s), ImVec2(c.x + s, c.y + s), col);
}

static void drawStopGlyph(ImDrawList* dl, ImVec2 tl, ImVec2 br) {
    ImVec2 c((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);
    float s = std::min(br.x - tl.x, br.y - tl.y) * 0.24f;
    dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), IM_COL32(255, 255, 255, 255));
}

// dir: +1 draws two right-pointing chevrons (fast-forward), -1 draws two left-pointing ones
// (fast-reverse).
static void drawChevronPair(ImDrawList* dl, ImVec2 tl, ImVec2 br, int dir) {
    ImVec2 c((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);
    float s = std::min(br.x - tl.x, br.y - tl.y) * 0.22f;
    float gap = s * 1.1f;
    ImU32 col = IM_COL32(255, 255, 255, 255);
    for (int i = -1; i <= 1; i += 2) {
        float ox = c.x + (float)i * gap * 0.5f;
        if (dir > 0) {
            dl->AddTriangleFilled(ImVec2(ox - s * 0.5f, c.y - s), ImVec2(ox - s * 0.5f, c.y + s), ImVec2(ox + s * 0.5f, c.y), col);
        }
        else {
            dl->AddTriangleFilled(ImVec2(ox + s * 0.5f, c.y - s), ImVec2(ox + s * 0.5f, c.y + s), ImVec2(ox - s * 0.5f, c.y), col);
        }
    }
}
static void drawFFGlyph(ImDrawList* dl, ImVec2 tl, ImVec2 br) { drawChevronPair(dl, tl, br, 1); }
static void drawRewindGlyph(ImDrawList* dl, ImVec2 tl, ImVec2 br) { drawChevronPair(dl, tl, br, -1); }

// A colored letter, matching the marker color drawn on the timeline itself (green A / red B) --
// simpler and more legible at button size than trying to cram a flag shape and a label into the
// same small glyph, while still reading as a distinct icon rather than a generic text button.
static void drawLetterGlyph(ImDrawList* dl, ImVec2 tl, ImVec2 br, const char* label, ImU32 col) {
    ImVec2 tsz = ImGui::CalcTextSize(label);
    ImVec2 c((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);
    dl->AddText(ImVec2(c.x - tsz.x * 0.5f, c.y - tsz.y * 0.5f), col, label);
}
static void drawSetAGlyph(ImDrawList* dl, ImVec2 tl, ImVec2 br) { drawLetterGlyph(dl, tl, br, "A", IM_COL32(120, 255, 120, 255)); }
static void drawSetBGlyph(ImDrawList* dl, ImVec2 tl, ImVec2 br) { drawLetterGlyph(dl, tl, br, "B", IM_COL32(255, 120, 120, 255)); }

// Standard hand-drawn "repeat" icon: an open circular arc plus a small arrowhead at one end,
// oriented along the arc's own tangent there.
static void drawLoopGlyph(ImDrawList* dl, ImVec2 tl, ImVec2 br) {
    ImVec2 c((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);
    float r = std::min(br.x - tl.x, br.y - tl.y) * 0.22f;
    ImU32 col = IM_COL32(255, 255, 255, 255);
    const float a0 = -2.4f, a1 = 2.0f;   // radians; the gap between them is where the arrowhead sits
    dl->PathArcTo(c, r, a0, a1, 20);
    dl->PathStroke(col, 0, 2.2f * style::uiScale);
    ImVec2 tip(c.x + r * cosf(a1), c.y + r * sinf(a1));
    ImVec2 tangent(-sinf(a1), cosf(a1));   // unit tangent, direction of travel along the arc
    ImVec2 normal(cosf(a1), sinf(a1));     // unit outward radial
    float aw = r * 0.55f;
    ImVec2 p1(tip.x + tangent.x * aw, tip.y + tangent.y * aw);
    ImVec2 p2(tip.x - normal.x * aw * 0.8f, tip.y - normal.y * aw * 0.8f);
    dl->AddTriangleFilled(tip, p1, p2, col);
}

void MainWindow::draw() {
    ImGui::Begin("Main", NULL, WINDOW_FLAGS);
    ImVec4 textCol = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    ImGui::WaterfallVFO* vfo = NULL;
    if (gui::waterfall.selectedVFO != "") {
        vfo = gui::waterfall.vfos[gui::waterfall.selectedVFO];
    }

    // Handle VFO movement
    if (vfo != NULL) {
        if (vfo->centerOffsetChanged) {
            if (tuningMode == tuner::TUNER_MODE_CENTER) {
                tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            }
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency() + vfo->generalOffset);
            gui::freqSelect.frequencyChanged = false;
            core::configManager.acquire();
            core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
            core::configManager.release(true);
        }
    }

    sigpath::vfoManager.updateFromWaterfall(&gui::waterfall);

    // Handle selection of another VFO
    if (gui::waterfall.selectedVFOChanged) {
        gui::freqSelect.setFrequency((vfo != NULL) ? (vfo->generalOffset + gui::waterfall.getCenterFrequency()) : gui::waterfall.getCenterFrequency());
        gui::waterfall.selectedVFOChanged = false;
        gui::freqSelect.frequencyChanged = false;
    }

    // Handle change in selected frequency
    if (gui::freqSelect.frequencyChanged) {
        gui::freqSelect.frequencyChanged = false;
        tuner::tune(tuningMode, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
        if (vfo != NULL) {
            vfo->centerOffsetChanged = false;
            vfo->lowerOffsetChanged = false;
            vfo->upperOffsetChanged = false;
        }
        core::configManager.acquire();
        core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
        if (vfo != NULL) {
            core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
        }
        core::configManager.release(true);
    }

    // Handle dragging the frequency scale
    if (gui::waterfall.centerFreqMoved) {
        gui::waterfall.centerFreqMoved = false;
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        if (vfo != NULL) {
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency() + vfo->generalOffset);
        }
        else {
            gui::freqSelect.setFrequency(gui::waterfall.getCenterFrequency());
        }
        core::configManager.acquire();
        core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
        core::configManager.release(true);
    }

    int _fftHeight = gui::waterfall.getFFTHeight();
    if (fftHeight != _fftHeight) {
        fftHeight = _fftHeight;
        core::configManager.acquire();
        core::configManager.conf["fftHeight"] = fftHeight;
        core::configManager.release(true);
    }

    // To Bar
    // ImGui::BeginChild("TopBarChild", ImVec2(0, 49.0f * style::uiScale), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImVec2 btnSize(30 * style::uiScale, 30 * style::uiScale);
    ImGui::PushID(ImGui::GetID("sdrpp_menu_btn"));
    if (ImGui::ImageButton(icons::MENU, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_Menu, false)) {
        showMenu = !showMenu;
        core::configManager.acquire();
        core::configManager.conf["showMenu"] = showMenu;
        core::configManager.release(true);
    }
    ImGui::PopID();

    ImGui::SameLine();

    bool tmpPlaySate = playing;
    if (playButtonLocked && !tmpPlaySate) { style::beginDisabled(); }
    if (playing) {
        ImGui::PushID(ImGui::GetID("sdrpp_stop_btn"));
        if (ImGui::ImageButton(icons::STOP, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            setPlayState(false);
        }
        ImGui::PopID();
    }
    else { // TODO: Might need to check if there even is a device
        ImGui::PushID(ImGui::GetID("sdrpp_play_btn"));
        if (ImGui::ImageButton(icons::PLAY, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol) || ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            setPlayState(true);
        }
        ImGui::PopID();
    }
    if (playButtonLocked && !tmpPlaySate) { style::endDisabled(); }

    // Handle auto-start
    if (autostart) {
        autostart = false;
        setPlayState(true);
    }

    ImGui::SameLine();
    float origY = ImGui::GetCursorPosY();

    sigpath::sinkManager.showVolumeSlider(gui::waterfall.selectedVFO, "##_sdrpp_main_volume_", 248 * style::uiScale, btnSize.x, 5, true);

    ImGui::SameLine();

    ImGui::SetCursorPosY(origY);
    gui::freqSelect.draw();

    ImGui::SameLine();

    ImGui::SetCursorPosY(origY);
    if (tuningMode == tuner::TUNER_MODE_CENTER) {
        ImGui::PushID(ImGui::GetID("sdrpp_ena_st_btn"));
        if (ImGui::ImageButton(icons::CENTER_TUNING, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol)) {
            tuningMode = tuner::TUNER_MODE_NORMAL;
            gui::waterfall.VFOMoveSingleClick = false;
            core::configManager.acquire();
            core::configManager.conf["centerTuning"] = false;
            core::configManager.release(true);
        }
        ImGui::PopID();
    }
    else { // TODO: Might need to check if there even is a device
        ImGui::PushID(ImGui::GetID("sdrpp_dis_st_btn"));
        if (ImGui::ImageButton(icons::NORMAL_TUNING, btnSize, ImVec2(0, 0), ImVec2(1, 1), 5, ImVec4(0, 0, 0, 0), textCol)) {
            tuningMode = tuner::TUNER_MODE_CENTER;
            gui::waterfall.VFOMoveSingleClick = true;
            tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, gui::freqSelect.frequency);
            core::configManager.acquire();
            core::configManager.conf["centerTuning"] = true;
            core::configManager.release(true);
        }
        ImGui::PopID();
    }

    ImGui::SameLine();

    int snrOffset = 87.0f * style::uiScale;
    int snrWidth = std::clamp<int>(ImGui::GetWindowSize().x - ImGui::GetCursorPosX() - snrOffset, 100.0f * style::uiScale, 300.0f * style::uiScale);
    int snrPos = std::max<int>(ImGui::GetWindowSize().x - (snrWidth + snrOffset), ImGui::GetCursorPosX());

    ImGui::SetCursorPosX(snrPos);
    ImGui::SetCursorPosY(origY + (5.0f * style::uiScale));
    ImGui::SetNextItemWidth(snrWidth);
    ImGui::SNRMeter((vfo != NULL) ? gui::waterfall.selectedVFOSNR : 0);

    // Note: this is what makes the vertical size correct, needs to be fixed
    ImGui::SameLine();

    // ImGui::EndChild();

    // Logo button
    ImGui::SetCursorPosX(ImGui::GetWindowSize().x - (48 * style::uiScale));
    ImGui::SetCursorPosY(10.0f * style::uiScale);
    if (ImGui::ImageButton(icons::LOGO, ImVec2(32 * style::uiScale, 32 * style::uiScale), ImVec2(0, 0), ImVec2(1, 1), 0)) {
        showCredits = true;
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        showCredits = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        showCredits = false;
    }

    // Reset waterfall lock
    lockWaterfallControls = showCredits;

    // Handle menu resize
    ImVec2 winSize = ImGui::GetWindowSize();
    ImVec2 mousePos = ImGui::GetMousePos();
    if (!lockWaterfallControls && showMenu) {
        float curY = ImGui::GetCursorPosY();
        bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (grabbingMenu) {
            newWidth = mousePos.x;
            newWidth = std::clamp<float>(newWidth, 250, winSize.x - 250);
            ImGui::GetForegroundDrawList()->AddLine(ImVec2(newWidth, curY), ImVec2(newWidth, winSize.y - 10), ImGui::GetColorU32(ImGuiCol_SeparatorActive));
        }
        if (mousePos.x >= newWidth - (2.0f * style::uiScale) && mousePos.x <= newWidth + (2.0f * style::uiScale) && mousePos.y > curY) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (click) {
                grabbingMenu = true;
            }
        }
        else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }
        if (!down && grabbingMenu) {
            grabbingMenu = false;
            menuWidth = newWidth;
            core::configManager.acquire();
            core::configManager.conf["menuWidth"] = menuWidth;
            core::configManager.release(true);
        }
    }

    // Process menu keybinds
    displaymenu::checkKeybinds();

    // Left Column
    if (showMenu) {
        ImGui::Columns(3, "WindowColumns", false);
        ImGui::SetColumnWidth(0, menuWidth);
        ImGui::SetColumnWidth(1, std::max<int>(winSize.x - menuWidth - (60.0f * style::uiScale), 100.0f * style::uiScale));
        ImGui::SetColumnWidth(2, 60.0f * style::uiScale);
        ImGui::BeginChild("Left Column");

        if (gui::menu.draw(firstMenuRender)) {
            core::configManager.acquire();
            json arr = json::array();
            for (int i = 0; i < gui::menu.order.size(); i++) {
                arr[i]["name"] = gui::menu.order[i].name;
                arr[i]["open"] = gui::menu.order[i].open;
            }
            core::configManager.conf["menuElements"] = arr;

            // Update enabled and disabled modules
            for (auto [_name, inst] : core::moduleManager.instances) {
                if (!core::configManager.conf["moduleInstances"].contains(_name)) { continue; }
                core::configManager.conf["moduleInstances"][_name]["enabled"] = inst.instance->isEnabled();
            }

            core::configManager.release(true);
        }
        if (startedWithMenuClosed) {
            startedWithMenuClosed = false;
        }
        else {
            firstMenuRender = false;
        }

        if (ImGui::CollapsingHeader("Debug")) {
            ImGui::Text("Frame time: %.3f ms/frame", ImGui::GetIO().DeltaTime * 1000.0f);
            ImGui::Text("Framerate: %.1f FPS", ImGui::GetIO().Framerate);
            ImGui::Text("Center Frequency: %.0f Hz", gui::waterfall.getCenterFrequency());
            ImGui::Text("Source name: %s", sourceName.c_str());
            ImGui::Checkbox("Show demo window", &demoWindow);
            ImGui::Text("ImGui version: %s", ImGui::GetVersion());

            // ImGui::Checkbox("Bypass buffering", &sigpath::iqFrontEnd.inputBuffer.bypass);

            // ImGui::Text("Buffering: %d", (sigpath::iqFrontEnd.inputBuffer.writeCur - sigpath::iqFrontEnd.inputBuffer.readCur + 32) % 32);

            if (ImGui::Button("Test Bug")) {
                flog::error("Will this make the software crash?");
            }

            if (ImGui::Button("Testing something")) {
                gui::menu.order[0].open = true;
                firstMenuRender = true;
            }

            ImGui::Checkbox("WF Single Click", &gui::waterfall.VFOMoveSingleClick);
            ImGui::Checkbox("Lock Menu Order", &gui::menu.locked);

            ImGui::Spacing();
        }

        ImGui::EndChild();
    }
    else {
        // When hiding the menu bar
        ImGui::Columns(3, "WindowColumns", false);
        ImGui::SetColumnWidth(0, 8 * style::uiScale);
        ImGui::SetColumnWidth(1, winSize.x - ((8 + 60) * style::uiScale));
        ImGui::SetColumnWidth(2, 60.0f * style::uiScale);
    }

    // Right Column
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::NextColumn();
    ImGui::PopStyleVar();

    const float pbBarH = 12.0f * style::uiScale;
    const float pbPad = 4.0f * style::uiScale;
    const float pbBtnH = 26.0f * style::uiScale;
    // Extra hit-region above the seek bar so the A/B marker triangles (drawn poking up above
    // it) are actually clickable/draggable -- see the InvisibleButton further down for why.
    const float markerFlagH = 8.0f * style::uiScale;
    bool hasRecTime = gui::playbackBar.active && (gui::playbackBar.recordingStartEpoch != 0);
    float numTextLines = hasRecTime ? 2.0f : 1.0f;
    const float pbTotalH = (hasRecTime ? 5.0f : 4.0f) * pbPad + markerFlagH + pbBarH + pbBtnH + numTextLines * ImGui::GetTextLineHeight();
    float pbReserve = gui::playbackBar.active ? pbTotalH : 0.0f;

    ImGui::BeginChild("Waterfall", ImVec2(0, ImGui::GetContentRegionAvail().y - pbReserve));

    gui::waterfall.draw();

    ImGui::EndChild();

    if (gui::playbackBar.active) {
        float barWidth = ImGui::GetContentRegionAvail().x;

        // pbTotalH above is a manual sum of exactly the Dummy/InvisibleButton/button-row
        // heights and pbPad gaps below -- it doesn't (and can't, without duplicating ImGui's
        // own internals) account for ImGui's automatic ItemSpacing.y between each of those
        // widgets when they land on separate lines. The original code only had one such
        // transition (Dummy -> the seek bar); adding the button row's own Dummy and
        // Button-row-start added two more, and each one of those untracked gaps stacked up
        // enough to push the recording-time text below the bottom of the reserved region
        // (live feedback: "the second line is partially cut off"). Zeroing ItemSpacing.y here
        // (keeping .x, so the button row's own SameLine gaps still look normal) makes
        // pbTotalH's arithmetic actually match what gets drawn, instead of chasing the exact
        // gap count by hand.
        ImVec2 origSpacing = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(origSpacing.x, 0.0f));

        ImGui::Dummy(ImVec2(barWidth, pbPad));

        // The invisible hit-region extends markerFlagH above the visually-drawn bar (barTL/
        // barBR below are offset down into the lower portion of it) so the A/B marker
        // triangles -- which poke up above the bar itself, see drawTimelineMarker() further
        // down -- are actually clickable/draggable. Previously they were drawn above the
        // InvisibleButton's own rect, in a purely decorative area no widget owned, so a click
        // landing exactly on a marker never registered at all (live feedback: "dragging only
        // works on the bar, not on the indicator above it").
        ImGui::InvisibleButton("##playback_seek", ImVec2(barWidth, markerFlagH + pbBarH));
        ImVec2 hitTL = ImGui::GetItemRectMin();
        ImVec2 hitBR = ImGui::GetItemRectMax();
        ImVec2 barTL(hitTL.x, hitTL.y + markerFlagH);
        ImVec2 barBR = hitBR;

        auto markerX = [&](float frac) { return barTL.x + (barBR.x - barTL.x) * frac; };
        bool hasA = gui::playbackBar.loopMarkerAFrac >= 0.0f;
        bool hasB = gui::playbackBar.loopMarkerBFrac >= 0.0f;
        const float markerHitPx = 6.0f * style::uiScale;

        // A/B marker drag takes priority over a plain seek-click -- checked first, and the
        // existing seek-click branch below only fires when no marker is being dragged this
        // frame. draggingLoopMarker persists across frames (a MainWindow member) so a drag
        // that started here keeps tracking the mouse even if it briefly leaves the bar's rect.
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            float mx = ImGui::GetMousePos().x;
            if (hasA && std::abs(mx - markerX(gui::playbackBar.loopMarkerAFrac)) <= markerHitPx) { draggingLoopMarker = 0; }
            else if (hasB && std::abs(mx - markerX(gui::playbackBar.loopMarkerBFrac)) <= markerHitPx) { draggingLoopMarker = 1; }
        }
        if (draggingLoopMarker != -1 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float dragFrac = std::clamp((ImGui::GetMousePos().x - barTL.x) / (barBR.x - barTL.x), 0.0f, 1.0f);
            if (gui::playbackBar.setLoopMarkerCallback) {
                gui::playbackBar.setLoopMarkerCallback(draggingLoopMarker, dragFrac, gui::playbackBar.transportCtx);
            }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { draggingLoopMarker = -1; }

        // Right-click a marker directly (on the bar or on its flag above it, now that the hit
        // region covers both) to clear it -- the on-timeline counterpart to right-clicking the
        // Set A/Set B buttons.
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            float mx = ImGui::GetMousePos().x;
            if (hasA && std::abs(mx - markerX(gui::playbackBar.loopMarkerAFrac)) <= markerHitPx) {
                if (gui::playbackBar.clearLoopMarkerCallback) { gui::playbackBar.clearLoopMarkerCallback(0, gui::playbackBar.transportCtx); }
            }
            else if (hasB && std::abs(mx - markerX(gui::playbackBar.loopMarkerBFrac)) <= markerHitPx) {
                if (gui::playbackBar.clearLoopMarkerCallback) { gui::playbackBar.clearLoopMarkerCallback(1, gui::playbackBar.transportCtx); }
            }
        }

        if (draggingLoopMarker == -1 && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float frac = (ImGui::GetMousePos().x - barTL.x) / (barBR.x - barTL.x);
            frac = std::clamp(frac, 0.0f, 1.0f);
            if (gui::playbackBar.seekCallback) {
                gui::playbackBar.seekCallback(frac, gui::playbackBar.seekCtx);
            }
        }

        float progress = std::clamp(gui::playbackBar.progress, 0.0f, 1.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        dl->AddRectFilled(barTL, barBR, ImGui::GetColorU32(ImGuiCol_FrameBg), 2.0f * style::uiScale);
        ImVec2 fillBR(barTL.x + (barBR.x - barTL.x) * progress, barBR.y);
        if (fillBR.x > barTL.x) {
            dl->AddRectFilled(barTL, fillBR, ImGui::GetColorU32(ImGuiCol_PlotHistogram), 2.0f * style::uiScale);
        }

        // A/B loop region highlight (only once both markers are placed) + the marker flags
        // themselves, drawn over the progress fill so they stay visible regardless of playhead
        // position.
        if (hasA && hasB) {
            float loStart = std::min(gui::playbackBar.loopMarkerAFrac, gui::playbackBar.loopMarkerBFrac);
            float loEnd = std::max(gui::playbackBar.loopMarkerAFrac, gui::playbackBar.loopMarkerBFrac);
            dl->AddRectFilled(ImVec2(markerX(loStart), barTL.y), ImVec2(markerX(loEnd), barBR.y), IM_COL32(255, 220, 0, 60));
        }
        auto drawTimelineMarker = [&](float frac, ImU32 col) {
            float x = markerX(frac);
            dl->AddTriangleFilled(ImVec2(x - 5 * style::uiScale, barTL.y - 6 * style::uiScale),
                                   ImVec2(x + 5 * style::uiScale, barTL.y - 6 * style::uiScale),
                                   ImVec2(x, barTL.y), col);
            dl->AddLine(ImVec2(x, barTL.y), ImVec2(x, barBR.y), col, 2.0f * style::uiScale);
        };
        if (hasA) { drawTimelineMarker(gui::playbackBar.loopMarkerAFrac, IM_COL32(80, 220, 80, 255)); }
        if (hasB) { drawTimelineMarker(gui::playbackBar.loopMarkerBFrac, IM_COL32(220, 80, 80, 255)); }

        // --- Transport + A/B loop button row ---
        ImGui::Dummy(ImVec2(barWidth, pbPad));
        ImVec2 pbBtn(pbBtnH, pbBtnH);
        bool hasFile = gui::playbackBar.stopCallback != nullptr;

        if (transportGlyphButton("##pb_stop", pbBtn, drawStopGlyph, false, !hasFile) && gui::playbackBar.stopCallback) {
            gui::playbackBar.stopCallback(gui::playbackBar.transportCtx);
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Stop (pause and rewind to start)"); }
        ImGui::SameLine();

        // Fast-reverse/fast-forward are press-and-hold, not click-to-toggle: scrubCallback is
        // called every frame below with the live held state of both buttons, and file_source's
        // worker loop just mirrors whatever direction is currently held.
        bool rwHeld = false;
        transportGlyphButton("##pb_rw", pbBtn, drawRewindGlyph, gui::playbackBar.scrubbingReverse, !hasFile, &rwHeld);
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Hold: fast reverse (silent)"); }
        ImGui::SameLine();

        bool isPlaying = !gui::playbackBar.paused;
        if (transportGlyphButton("##pb_playpause", pbBtn, isPlaying ? drawPauseGlyph : drawPlayGlyph, false, !hasFile) && gui::playbackBar.playPauseCallback) {
            // gui::playbackBar.paused doubles as the "should this click now play" flag: true
            // means currently paused, so the click means play.
            gui::playbackBar.playPauseCallback(gui::playbackBar.paused, gui::playbackBar.transportCtx);
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip(isPlaying ? "Pause" : "Play"); }
        ImGui::SameLine();

        bool ffHeld = false;
        transportGlyphButton("##pb_ff", pbBtn, drawFFGlyph, gui::playbackBar.scrubbingForward, !hasFile, &ffHeld);
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Hold: fast forward (silent)"); }

        if (hasFile && gui::playbackBar.scrubCallback) {
            int dir = 0;
            if (rwHeld != ffHeld) { dir = rwHeld ? -1 : 1; }   // both or neither held -> no scrub
            gui::playbackBar.scrubCallback(dir, gui::playbackBar.transportCtx);
        }

        ImGui::SameLine(0, 20.0f * style::uiScale);

        bool setARightClick = false;
        if (transportGlyphButton("##pb_seta", pbBtn, drawSetAGlyph, false, !hasFile, nullptr, 0, &setARightClick)
            && gui::playbackBar.setLoopMarkerCallback) {
            gui::playbackBar.setLoopMarkerCallback(0, gui::playbackBar.progress, gui::playbackBar.transportCtx);
        }
        if (setARightClick && gui::playbackBar.clearLoopMarkerCallback) {
            gui::playbackBar.clearLoopMarkerCallback(0, gui::playbackBar.transportCtx);
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Left click: set loop start here\nRight click: clear"); }
        ImGui::SameLine();

        bool setBRightClick = false;
        if (transportGlyphButton("##pb_setb", pbBtn, drawSetBGlyph, false, !hasFile, nullptr, 0, &setBRightClick)
            && gui::playbackBar.setLoopMarkerCallback) {
            gui::playbackBar.setLoopMarkerCallback(1, gui::playbackBar.progress, gui::playbackBar.transportCtx);
        }
        if (setBRightClick && gui::playbackBar.clearLoopMarkerCallback) {
            gui::playbackBar.clearLoopMarkerCallback(1, gui::playbackBar.transportCtx);
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Left click: set loop end here\nRight click: clear"); }
        ImGui::SameLine();

        // Bright green + white outline when on -- ImGuiCol_ButtonActive alone (used everywhere
        // else in this row) reads as barely different from the normal button color in this
        // theme, which was the reported problem ("no indication of state"). Loop is the only
        // control here with a *persistent* on/off state worth calling out this strongly; the
        // others are momentary actions or already have their own always-visible state (the
        // Play/Pause glyph itself, the marker flags on the timeline).
        bool canLoop = hasFile && hasA && hasB;
        if (transportGlyphButton("##pb_loop", pbBtn, drawLoopGlyph, gui::playbackBar.loopEnabled, !canLoop,
                                  nullptr, IM_COL32(40, 170, 60, 255))
            && canLoop && gui::playbackBar.setLoopEnabledCallback) {
            gui::playbackBar.setLoopEnabledCallback(!gui::playbackBar.loopEnabled, gui::playbackBar.transportCtx);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(canLoop ? (gui::playbackBar.loopEnabled ? "Loop: ON (click to disable)" : "Loop: off (click to enable)")
                                       : "Set both loop markers first");
        }

        auto fmtTime = [](float sec) -> std::string {
            int s = (int)sec; int m = s / 60; s %= 60; int h = m / 60; m %= 60;
            char buf[32];
            if (h > 0) { snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s); }
            else { snprintf(buf, sizeof(buf), "%d:%02d", m, s); }
            return buf;
        };

        float textY = ImGui::GetItemRectMax().y + pbPad;

        if (hasRecTime) {
            // Line 1: recording start date and time (fixed label)
            const std::string& startStr = gui::playbackBar.recordingStartStr;
            ImVec2 startSz = ImGui::CalcTextSize(startStr.c_str());
            dl->AddText(ImVec2(barTL.x + ((barBR.x - barTL.x) - startSz.x) * 0.5f, textY),
                        ImGui::GetColorU32(ImGuiCol_Text), startStr.c_str());
            textY += ImGui::GetTextLineHeight() + pbPad;

            // Line 2: absolute current time / absolute end time (local clock)
            auto fmtAbsTime = [](int64_t epochSec) -> std::string {
                time_t t = (time_t)epochSec;
                tm* ltm = localtime(&t);
                char buf[32];
                snprintf(buf, sizeof(buf), "%02d:%02d:%02d", ltm->tm_hour, ltm->tm_min, ltm->tm_sec);
                return buf;
            };
            int64_t curAbs = gui::playbackBar.recordingStartEpoch + (int64_t)gui::playbackBar.currentTimeSec;
            int64_t endAbs = gui::playbackBar.recordingStartEpoch + (int64_t)gui::playbackBar.totalTimeSec;
            std::string timeStr = fmtAbsTime(curAbs) + " / " + fmtAbsTime(endAbs);
            ImVec2 textSz = ImGui::CalcTextSize(timeStr.c_str());
            dl->AddText(ImVec2(barTL.x + ((barBR.x - barTL.x) - textSz.x) * 0.5f, textY),
                        ImGui::GetColorU32(ImGuiCol_Text), timeStr.c_str());
        }
        else {
            std::string timeStr = fmtTime(gui::playbackBar.currentTimeSec) + " / " + fmtTime(gui::playbackBar.totalTimeSec);
            ImVec2 textSz = ImGui::CalcTextSize(timeStr.c_str());
            dl->AddText(ImVec2(barTL.x + ((barBR.x - barTL.x) - textSz.x) * 0.5f, textY),
                        ImGui::GetColorU32(ImGuiCol_Text), timeStr.c_str());
        }

        ImGui::PopStyleVar();
    }

    if (!lockWaterfallControls) {
        // Handle arrow keys
        if (vfo != NULL && (gui::waterfall.mouseInFFT || gui::waterfall.mouseInWaterfall)) {
            bool freqChanged = false;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !gui::freqSelect.digitHovered) {
                double nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset - vfo->snapInterval;
                nfreq = roundl(nfreq / vfo->snapInterval) * vfo->snapInterval;
                tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
                freqChanged = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !gui::freqSelect.digitHovered) {
                double nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset + vfo->snapInterval;
                nfreq = roundl(nfreq / vfo->snapInterval) * vfo->snapInterval;
                tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
                freqChanged = true;
            }
            if (freqChanged) {
                core::configManager.acquire();
                core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
                if (vfo != NULL) {
                    core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
                }
                core::configManager.release(true);
            }
        }

        // Handle scrollwheel
        int wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0 && (gui::waterfall.mouseInFFT || gui::waterfall.mouseInWaterfall)) {
            double nfreq;
            if (vfo != NULL) {
                // Select factor depending on modifier keys
                double interval;
                if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
                    interval = vfo->snapInterval * 10.0;
                }
                else if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                    interval = vfo->snapInterval * 0.1;
                }
                else {
                    interval = vfo->snapInterval;
                }

                nfreq = gui::waterfall.getCenterFrequency() + vfo->generalOffset + (interval * wheel);
                nfreq = roundl(nfreq / interval) * interval;
            }
            else {
                nfreq = gui::waterfall.getCenterFrequency() - (gui::waterfall.getViewBandwidth() * wheel / 20.0);
            }
            tuner::tune(tuningMode, gui::waterfall.selectedVFO, nfreq);
            gui::freqSelect.setFrequency(nfreq);
            core::configManager.acquire();
            core::configManager.conf["frequency"] = gui::waterfall.getCenterFrequency();
            if (vfo != NULL) {
                core::configManager.conf["vfoOffsets"][gui::waterfall.selectedVFO] = vfo->generalOffset;
            }
            core::configManager.release(true);
        }
    }

    ImGui::NextColumn();
    ImGui::BeginChild("WaterfallControls");

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - (ImGui::CalcTextSize("Zoom").x / 2.0));
    ImGui::TextUnformatted("Zoom");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - 10 * style::uiScale);
    ImVec2 wfSliderSize(20.0 * style::uiScale, 150.0 * style::uiScale);
    if (ImGui::VSliderFloat("##_7_", wfSliderSize, &bw, 1.0, 0.0, "")) {
        double factor = (double)bw * (double)bw;

        // Map 0.0 -> 1.0 to 1000.0 -> bandwidth
        double wfBw = gui::waterfall.getBandwidth();
        double delta = wfBw - 1000.0;
        double finalBw = std::min<double>(1000.0 + (factor * delta), wfBw);

        gui::waterfall.setViewBandwidth(finalBw);
        if (vfo != NULL) {
            gui::waterfall.setViewOffset(vfo->centerOffset); // center vfo on screen
        }
    }

    ImGui::NewLine();

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - (ImGui::CalcTextSize("Max").x / 2.0));
    ImGui::TextUnformatted("Max");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - 10 * style::uiScale);
    if (ImGui::VSliderFloat("##_8_", wfSliderSize, &fftMax, 0.0, -160.0f, "")) {
        fftMax = std::max<float>(fftMax, fftMin + 10);
        core::configManager.acquire();
        core::configManager.conf["max"] = fftMax;
        core::configManager.release(true);
    }

    ImGui::NewLine();

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - (ImGui::CalcTextSize("Min").x / 2.0));
    ImGui::TextUnformatted("Min");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - 10 * style::uiScale);
    ImGui::SetItemUsingMouseWheel();
    if (ImGui::VSliderFloat("##_9_", wfSliderSize, &fftMin, 0.0, -160.0f, "")) {
        fftMin = std::min<float>(fftMax - 10, fftMin);
        core::configManager.acquire();
        core::configManager.conf["min"] = fftMin;
        core::configManager.release(true);
    }

    ImGui::EndChild();

    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setWaterfallMax(fftMax);

    ImGui::End();

    if (showCredits) {
        credits::show();
    }

    if (demoWindow) {
        ImGui::ShowDemoWindow();
    }
}

void MainWindow::setPlayState(bool _playing) {
    if (_playing == playing) { return; }
    if (_playing) {
        sigpath::iqFrontEnd.flushInputBuffer();
        sigpath::sourceManager.start();
        sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
        playing = true;
        onPlayStateChange.emit(true);
    }
    else {
        playing = false;
        onPlayStateChange.emit(false);
        sigpath::sourceManager.stop();
        sigpath::iqFrontEnd.flushInputBuffer();
    }
}

void MainWindow::setViewBandwidthSlider(float bandwidth) {
    bw = bandwidth;
}

bool MainWindow::sdrIsRunning() {
    return playing;
}

bool MainWindow::isPlaying() {
    return playing;
}

void MainWindow::setFirstMenuRender() {
    firstMenuRender = true;
}