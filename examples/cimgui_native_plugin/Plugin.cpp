// cimgui_native_plugin: demo plugin that draws a native ImGui window inside
// UEVR's overlay. Originally used cimgui via GetProcAddress, but the export
// surface had gaps (ImDrawList_AddCircle/AddLine missing) and the resolved
// surface was easy to mis-call. Switched to static-imgui linkage using
// UEVR's exact source tree + imconfig (same pattern as lua_script_editor).
//
// F9 toggles the window.

#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {

constexpr const char* TAG = "[cimgui_native]";
constexpr WPARAM TOGGLE_KEY = VK_F9;

bool   g_window_open = true;
uint64_t g_frame = 0;
uint64_t g_button_clicks = 0;
uint64_t g_tick = 0;

bool  g_checkbox = false;
float g_slider = 0.5f;
float g_color[3] = { 0.3f, 0.7f, 1.0f };
char  g_text_buf[256] = "type something";

constexpr int FPS_SAMPLES = 60;
float g_fps_hist[FPS_SAMPLES] = {};
int   g_fps_idx = 0;
LARGE_INTEGER g_qpc_freq{};
LARGE_INTEGER g_qpc_last{};

float current_fps() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_qpc_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&g_qpc_freq);
        g_qpc_last = now;
        return 0.0f;
    }
    const double dt = double(now.QuadPart - g_qpc_last.QuadPart) / double(g_qpc_freq.QuadPart);
    g_qpc_last = now;
    return dt > 0.0 ? float(1.0 / dt) : 0.0f;
}

void draw_status_tab() {
    ImGui::Text("UEVR static-imgui native plugin");
    ImGui::Separator();
    ImGui::Text("Window frames    : %llu", (unsigned long long)g_frame);
    ImGui::Text("Engine ticks     : %llu", (unsigned long long)g_tick);
    ImGui::Text("Button clicks    : %llu", (unsigned long long)g_button_clicks);
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
        "ImGui context owned by UEVR. Drawn from a separate DLL with its");
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
        "own static imgui linkage rebound to host ctx + allocators.");
}

void draw_frame_tab() {
    const float fps = current_fps();
    g_fps_hist[g_fps_idx] = fps;
    g_fps_idx = (g_fps_idx + 1) % FPS_SAMPLES;

    float sum = 0.0f, mx = 0.0f;
    for (int i = 0; i < FPS_SAMPLES; ++i) {
        sum += g_fps_hist[i];
        if (g_fps_hist[i] > mx) mx = g_fps_hist[i];
    }
    const float avg = sum / float(FPS_SAMPLES);

    ImGui::Text("Instant : %6.1f fps", fps);
    ImGui::Text("Average : %6.1f fps  (last %d samples)", avg, FPS_SAMPLES);
    ImGui::Text("Peak    : %6.1f fps", mx);
    ImGui::Separator();
    ImGui::PlotLines("##fps", g_fps_hist, FPS_SAMPLES, g_fps_idx, nullptr,
        0.0f, mx > 0.0f ? mx : 1.0f, ImVec2(-1.0f, 60.0f));
    const float frac = mx > 0.0f ? (avg / mx) : 0.0f;
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), nullptr);
}

void draw_widgets_tab() {
    if (ImGui::Button("ping")) {
        ++g_button_clicks;
        API::get()->log_info("%s ping -> %llu", TAG,
            (unsigned long long)g_button_clicks);
    }
    ImGui::SameLine();
    ImGui::Checkbox("checkbox", &g_checkbox);
    ImGui::SliderFloat("slider", &g_slider, 0.0f, 1.0f);
    ImGui::ColorEdit3("color", g_color);
    ImGui::InputText("text", g_text_buf, sizeof(g_text_buf));
    ImGui::Spacing();
    ImGui::TextWrapped("All widget state lives in this plugin DLL.");
}

void draw_draw_tab() {
    // Native ImDrawList works because we linked imgui statically — no
    // cimgui export gap. The list belongs to the current window so drawing
    // is clipped to it.
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 center(origin.x + 150.0f, origin.y + 100.0f);

    dl->AddCircle(center, 60.0f, 0xFFFFFFFF, 32, 1.5f);
    const float theta = float(g_frame) * 0.05f;
    const ImVec2 tip(center.x + 50.0f * std::cos(theta),
                     center.y + 50.0f * std::sin(theta));
    const unsigned col = (unsigned(g_color[0] * 255) <<  0)
                       | (unsigned(g_color[1] * 255) <<  8)
                       | (unsigned(g_color[2] * 255) << 16)
                       | 0xFF000000u;
    dl->AddLine(center, tip, col, 2.0f);

    ImGui::Dummy(ImVec2(0, 220.0f));
    ImGui::TextDisabled("native ImDrawList drawn straight into host overlay");
}

void render(UEVR_ImGuiFrameCbData* data) {
    if (!g_window_open) return;
    if (data == nullptr) return;

    ImGui::SetCurrentContext((ImGuiContext*)data->context);
    ImGui::SetAllocatorFunctions(
        (ImGuiMemAllocFunc)data->malloc_fn,
        (ImGuiMemFreeFunc)data->free_fn,
        data->user_data);

    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        IMGUI_CHECKVERSION();
        API::get()->log_info("%s ImGui ABI ok: %s",
            TAG, ImGui::GetVersion());
    }

    ++g_frame;

    ImGui::SetNextWindowSize(ImVec2(420.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("cimgui_native_plugin", &g_window_open, 0)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("status"))  { draw_status_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("frame"))   { draw_frame_tab();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("widgets")) { draw_widgets_tab();ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("draw"))    { draw_draw_tab();   ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace

class CimguiNativePlugin : public Plugin {
public:
    void on_initialize() override {
        auto& api = *API::get();
        const auto fns = api.param()->functions;

        api.log_info("%s init. branch=%s commit=%s", TAG,
            fns->get_branch(), fns->get_commit_hash());

        if (fns->on_imgui_frame == nullptr) {
            api.log_warn("%s on_imgui_frame NULL — rebuild UEVRBackend.dll", TAG);
            return;
        }

        fns->on_imgui_frame([](UEVR_ImGuiFrameCbData* d) {
            try { render(d); } catch (...) {
                static bool once = false;
                if (!once) { once = true; API::get()->log_error("%s render exception", TAG); }
            }
        });
        api.log_info("%s ready (F9 to toggle)", TAG);
    }

    bool on_message(HWND, UINT msg, WPARAM wparam, LPARAM) override {
        if (msg == WM_KEYDOWN && wparam == TOGGLE_KEY) {
            g_window_open = !g_window_open;
            API::get()->log_info("%s window=%s", TAG,
                g_window_open ? "open" : "closed");
        }
        return true;
    }

    void on_post_engine_tick(API::UGameEngine*, float) override {
        ++g_tick;
    }
};

static CimguiNativePlugin g_plugin_instance;
