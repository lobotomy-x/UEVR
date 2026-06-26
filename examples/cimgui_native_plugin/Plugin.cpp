// cimgui_native_plugin: proves the on_imgui_frame + cimgui-redirect path
// end-to-end with no Lua bridge.
//
// How it works:
//   * UEVR hooks LoadLibraryExW so any plugin asking for "cimgui.dll" gets
//     back UEVRBackend.dll's HMODULE (PluginLoader.cpp::cimgui::setup_hook).
//   * UEVRBackend.dll exports the full cimgui C ABI (see
//     dependencies/submodules/cimgui/cimgui_exports.def).
//   * So we LoadLibrary("cimgui.dll"), GetProcAddress every symbol we want,
//     and call them inside our on_imgui_frame callback. UEVR's ImGuiContext
//     is already current at that point, so we draw directly into the host
//     overlay without linking any imgui.cpp into our DLL.
//
// Output: a native ImGui window titled "cimgui_native_plugin" with a tab bar
// containing four panels: status, frame stats, dynamic widgets, and a draw-
// list demo. Press F9 to toggle visibility.

#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "uevr/Plugin.hpp"

using namespace uevr;

namespace {

// ----- Minimal cimgui ABI surface ----------------------------------------
struct ImVec2 { float x, y; };
struct ImVec4 { float x, y, z, w; };
using ImGuiID = unsigned int;
using ImGuiWindowFlags = int;
using ImGuiCol = int;

struct CImgui {
    bool resolved = false;

    bool   (*Begin)(const char*, bool*, ImGuiWindowFlags) = nullptr;
    void   (*End)() = nullptr;
    bool   (*BeginTabBar)(const char*, int) = nullptr;
    void   (*EndTabBar)() = nullptr;
    bool   (*BeginTabItem)(const char*, bool*, int) = nullptr;
    void   (*EndTabItem)() = nullptr;
    void   (*Text)(const char*, ...) = nullptr;
    void   (*TextColored)(ImVec4, const char*, ...) = nullptr;
    void   (*TextWrapped)(const char*, ...) = nullptr;
    void   (*Separator)() = nullptr;
    void   (*SameLine)(float, float) = nullptr;
    void   (*Spacing)() = nullptr;
    bool   (*Button)(const char*, ImVec2) = nullptr;
    bool   (*Checkbox)(const char*, bool*) = nullptr;
    bool   (*SliderFloat)(const char*, float*, float, float, const char*, int) = nullptr;
    bool   (*ColorEdit3)(const char*, float[3], int) = nullptr;
    bool   (*InputText)(const char*, char*, size_t, int, void*, void*) = nullptr;
    void   (*ProgressBar)(float, ImVec2, const char*) = nullptr;
    void   (*SetNextWindowSize)(ImVec2, int) = nullptr;
    void   (*PushID_Int)(int) = nullptr;
    void   (*PopID)() = nullptr;
    void   (*GetCursorScreenPos)(ImVec2*) = nullptr;
    void*  (*GetWindowDrawList)() = nullptr;
    void   (*ImDrawList_AddCircle)(void*, ImVec2, float, unsigned, int, float) = nullptr;
    void   (*ImDrawList_AddLine)(void*, ImVec2, ImVec2, unsigned, float) = nullptr;

    bool load() {
        if (resolved) return true;
        HMODULE h = ::LoadLibraryExW(L"cimgui.dll", nullptr, 0);
        if (h == nullptr) return false;

        #define G(field, sym) field = (decltype(field))::GetProcAddress(h, sym)
        G(Begin,            "igBegin");
        G(End,              "igEnd");
        G(BeginTabBar,      "igBeginTabBar");
        G(EndTabBar,        "igEndTabBar");
        G(BeginTabItem,     "igBeginTabItem");
        G(EndTabItem,       "igEndTabItem");
        G(Text,             "igText");
        G(TextColored,      "igTextColored");
        G(TextWrapped,      "igTextWrapped");
        G(Separator,        "igSeparator");
        G(SameLine,         "igSameLine");
        G(Spacing,          "igSpacing");
        G(Button,           "igButton");
        G(Checkbox,         "igCheckbox");
        G(SliderFloat,      "igSliderFloat");
        G(ColorEdit3,       "igColorEdit3");
        G(InputText,        "igInputText");
        G(ProgressBar,      "igProgressBar");
        G(SetNextWindowSize,"igSetNextWindowSize");
        G(PushID_Int,       "igPushID_Int");
        G(PopID,            "igPopID");
        G(GetCursorScreenPos,"igGetCursorScreenPos");
        G(GetWindowDrawList,"igGetWindowDrawList");
        G(ImDrawList_AddCircle, "ImDrawList_AddCircle");
        G(ImDrawList_AddLine,   "ImDrawList_AddLine");
        #undef G

        resolved = Begin && End && Text && Button;
        return resolved;
    }
};

constexpr const char* TAG = "[cimgui_native]";
constexpr WPARAM TOGGLE_KEY = VK_F9;

CImgui g_ig;
bool g_window_open = true;
uint64_t g_frame = 0;
uint64_t g_button_clicks = 0;
uint64_t g_tick = 0;

// Widget state owned by C++
bool  g_checkbox = false;
float g_slider = 0.5f;
float g_color[3] = { 0.3f, 0.7f, 1.0f };
char  g_text_buf[256] = "type something";

// FPS ring buffer
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
    g_ig.Text("UEVR cimgui-redirect native plugin");
    g_ig.Separator();
    g_ig.Text("Window frames    : %llu", (unsigned long long)g_frame);
    g_ig.Text("Engine ticks     : %llu", (unsigned long long)g_tick);
    g_ig.Text("Button clicks    : %llu", (unsigned long long)g_button_clicks);
    g_ig.Spacing();
    g_ig.TextColored(ImVec4{0.4f, 1.0f, 0.4f, 1.0f},
        "ImGui context owned by UEVR. Drawn from a separate DLL.");
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

    g_ig.Text("Instant : %6.1f fps", fps);
    g_ig.Text("Average : %6.1f fps  (last %d samples)", avg, FPS_SAMPLES);
    g_ig.Text("Peak    : %6.1f fps", mx);
    g_ig.Separator();
    const float frac = mx > 0.0f ? (avg / mx) : 0.0f;
    g_ig.ProgressBar(frac, ImVec2{ -1.0f, 0.0f }, nullptr);
}

void draw_widgets_tab() {
    if (g_ig.Button("ping", ImVec2{0, 0})) {
        ++g_button_clicks;
        API::get()->log_info("%s ping -> %llu", TAG,
            (unsigned long long)g_button_clicks);
    }
    g_ig.SameLine(0.0f, -1.0f);
    g_ig.Checkbox("checkbox", &g_checkbox);
    g_ig.SliderFloat("slider", &g_slider, 0.0f, 1.0f, "%.3f", 0);
    g_ig.ColorEdit3("color", g_color, 0);
    if (g_ig.InputText)
        g_ig.InputText("text", g_text_buf, sizeof(g_text_buf), 0, nullptr, nullptr);
    g_ig.Spacing();
    g_ig.TextWrapped("All state lives in this plugin DLL. Pushes into the host's ImGui via UEVR's exported cimgui ABI.");
}

void draw_drawlist_tab() {
    if (!g_ig.GetWindowDrawList || !g_ig.GetCursorScreenPos) {
        g_ig.Text("ImDrawList helpers unavailable");
        return;
    }

    ImVec2 origin{};
    g_ig.GetCursorScreenPos(&origin);
    void* dl = g_ig.GetWindowDrawList();
    const ImVec2 center{ origin.x + 150.0f, origin.y + 100.0f };

    if (g_ig.ImDrawList_AddCircle && dl) {
        const unsigned col = 0xFFFFFFFF;
        g_ig.ImDrawList_AddCircle(dl, center, 60.0f, col, 32, 1.5f);
        const float theta = float(g_frame) * 0.05f;
        const ImVec2 tip{
            center.x + 50.0f * cosf(theta),
            center.y + 50.0f * sinf(theta)
        };
        const unsigned col_line =
              (unsigned(g_color[0] * 255) <<  0)
            | (unsigned(g_color[1] * 255) <<  8)
            | (unsigned(g_color[2] * 255) << 16)
            |  0xFF000000u;
        g_ig.ImDrawList_AddLine(dl, center, tip, col_line, 2.0f);
    }
    // Reserve some vertical space so subsequent items don't overlap.
    g_ig.Text("\n\n\n\n\n\n\n\n\n  drawing into UEVR's overlay via raw cimgui");
}

void render() {
    if (!g_ig.load()) return;
    if (!g_window_open) return;

    ++g_frame;
    g_ig.SetNextWindowSize(ImVec2{ 420.0f, 360.0f }, 1 /* ImGuiCond_FirstUseEver */);

    if (!g_ig.Begin("cimgui_native_plugin", &g_window_open, 0)) {
        g_ig.End();
        return;
    }

    if (g_ig.BeginTabBar && g_ig.BeginTabBar("##tabs", 0)) {
        if (g_ig.BeginTabItem("status", nullptr, 0)) { draw_status_tab();   g_ig.EndTabItem(); }
        if (g_ig.BeginTabItem("frame",  nullptr, 0)) { draw_frame_tab();    g_ig.EndTabItem(); }
        if (g_ig.BeginTabItem("widgets",nullptr, 0)) { draw_widgets_tab();  g_ig.EndTabItem(); }
        if (g_ig.BeginTabItem("draw",   nullptr, 0)) { draw_drawlist_tab(); g_ig.EndTabItem(); }
        g_ig.EndTabBar();
    } else {
        draw_status_tab();
    }

    g_ig.End();
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

        fns->on_imgui_frame([](UEVR_ImGuiFrameCbData*) { render(); });
        api.log_info("%s ready (F9 to toggle window)", TAG);
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
