#include <gui/widgets/stepped_slider.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>


namespace ImGui {
    bool SliderFloatWithSteps(const char* label, float* v, float v_min, float v_max, float v_step, const char* display_format) {
        if (!display_format) {
            display_format = "%.3f";
        }

        // A real SliderFloat operating on *v directly, snapped to the nearest step afterwards --
        // not an ImGui::SliderInt over a [0,N] step index dressed up with a pre-rendered label,
        // which is what this used to do. That approach broke Ctrl+Click-to-type: the popup text
        // box parses whatever you type back using the *slider's own data type* (int, here), not
        // the format string used only to pre-fill the box's display text, so typing an exact
        // value silently landed on v_min + (the number you typed) * v_step instead. E.g. on a
        // 70..200 slider with a 0.1 step, typing "150.0" landed on 70 + 150*0.1 = 85.0, not
        // 150.0 -- there was no way to set an exact value by typing it, only by dragging and
        // hoping the step you wanted happened to fall under the mouse. Driving a real SliderFloat
        // instead means Ctrl+Click parses back as the actual float value shown, so typing works
        // correctly; the snap below then just makes sure drags and typed entries alike still land
        // on a step the underlying hardware/API actually supports.
        bool value_changed = ImGui::SliderFloat(label, v, v_min, v_max, display_format, ImGuiSliderFlags_AlwaysClamp);

        if (value_changed && v_step > 0.0f) {
            *v = v_min + roundf((*v - v_min) / v_step) * v_step;
            *v = std::clamp(*v, v_min, v_max);
        }
        return value_changed;
    }
}
