// Multi-threaded Julia set renderer for WebAssembly.
// Uses the CoreEngine reactor (std::thread via -sPTHREAD) + HTML5 API + WebGL2.
// Requires SharedArrayBuffer (COOP+COEP headers); loaded by index.html only when
// window.crossOriginIsolated is true.
#include "app.hpp"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Layout constants (must match wasm_main.cpp)
// ---------------------------------------------------------------------------

static constexpr int k_img_w  = 800;
static constexpr int k_img_h  = 600;
static constexpr int k_ctrl_w = 300;

// ---------------------------------------------------------------------------
// Global render state (safe across emscripten_set_main_loop's longjmp)
// ---------------------------------------------------------------------------

struct State {
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl_ctx{0};
    GLuint fractal_tex{0};
    app::App* app{nullptr};

    // Local UI state — written by ImGui widgets, applied to app on change
    float c_real{-0.7f};
    float c_imag{0.27015f};
    float zoom{1.0f};
    float center_x{0.0f};
    float center_y{0.0f};
    int   max_iter{128};
    float target_fps{60.0f};

    // ImGui mouse state (updated by HTML5 callbacks)
    float mouse_x{0.0f};
    float mouse_y{0.0f};
    bool  mouse_btn[3]{false, false, false};
    float wheel_dy{0.0f};

    // FPS (render loop rate measured here; pipeline rate from app->get_fps())
    double last_time_ms{0.0};
    float  render_fps{0.0f};

    // Drag-to-pan state
    bool  drag_active{false};
    float drag_start_bx{0.0f};
    float drag_start_by{0.0f};
    float drag_start_center_x{0.0f};
    float drag_start_center_y{0.0f};
    bool  prev_mouse_left{false};
};

static State g_state;

// ---------------------------------------------------------------------------
// Emscripten → ImGui key mapping
// ---------------------------------------------------------------------------

static ImGuiKey dom_key_to_imgui(const char* key) {
    if (strcmp(key, "Tab")        == 0) return ImGuiKey_Tab;
    if (strcmp(key, "ArrowLeft")  == 0) return ImGuiKey_LeftArrow;
    if (strcmp(key, "ArrowRight") == 0) return ImGuiKey_RightArrow;
    if (strcmp(key, "ArrowUp")    == 0) return ImGuiKey_UpArrow;
    if (strcmp(key, "ArrowDown")  == 0) return ImGuiKey_DownArrow;
    if (strcmp(key, "Enter")      == 0) return ImGuiKey_Enter;
    if (strcmp(key, "Escape")     == 0) return ImGuiKey_Escape;
    if (strcmp(key, "Backspace")  == 0) return ImGuiKey_Backspace;
    if (strcmp(key, "Delete")     == 0) return ImGuiKey_Delete;
    if (strcmp(key, "Home")       == 0) return ImGuiKey_Home;
    if (strcmp(key, "End")        == 0) return ImGuiKey_End;
    if (strcmp(key, "PageUp")     == 0) return ImGuiKey_PageUp;
    if (strcmp(key, "PageDown")   == 0) return ImGuiKey_PageDown;
    if (strlen(key) == 1) {
        char c = key[0];
        if (c >= 'a' && c <= 'z') return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'a'));
        if (c >= 'A' && c <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
        if (c >= '0' && c <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
    }
    return ImGuiKey_None;
}

// ---------------------------------------------------------------------------
// HTML5 event callbacks
// ---------------------------------------------------------------------------

static EM_BOOL on_mouse_move(int, const EmscriptenMouseEvent* e, void*) {
    g_state.mouse_x = static_cast<float>(e->targetX);
    g_state.mouse_y = static_cast<float>(e->targetY);
    return EM_TRUE;
}

static EM_BOOL on_mouse_button(int event_type, const EmscriptenMouseEvent* e, void*) {
    int btn = static_cast<int>(e->button);
    if (btn >= 0 && btn < 3) {
        bool down = (event_type == EMSCRIPTEN_EVENT_MOUSEDOWN);
        g_state.mouse_btn[btn] = down;
        ImGui::GetIO().AddMouseButtonEvent(btn, down);
    }
    return EM_TRUE;
}

static EM_BOOL on_wheel(int, const EmscriptenWheelEvent* e, void*) {
    g_state.wheel_dy -= static_cast<float>(e->deltaY) * 0.01f;
    return EM_TRUE;
}

static EM_BOOL on_key(int event_type, const EmscriptenKeyboardEvent* e, void*) {
    ImGuiIO& io  = ImGui::GetIO();
    bool down    = (event_type == EMSCRIPTEN_EVENT_KEYDOWN);
    ImGuiKey key = dom_key_to_imgui(e->key);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
    if (down && strlen(e->key) == 1 && (unsigned char)e->key[0] >= 32)
        io.AddInputCharacter(static_cast<unsigned int>(e->key[0]));
    return EM_FALSE;
}

// ---------------------------------------------------------------------------
// ImGui platform layer — driven by HTML5 callbacks above
// ---------------------------------------------------------------------------

static void imgui_new_frame(float delta_secs, float scale_x, float scale_y) {
    State&   s  = g_state;
    ImGuiIO& io = ImGui::GetIO();

    int cw, ch;
    emscripten_get_canvas_element_size("#canvas", &cw, &ch);
    io.DisplaySize             = ImVec2(static_cast<float>(cw), static_cast<float>(ch));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime               = delta_secs;

    io.AddMousePosEvent(s.mouse_x * scale_x, s.mouse_y * scale_y);
    if (s.wheel_dy != 0.0f) {
        io.AddMouseWheelEvent(0.0f, s.wheel_dy);
        s.wheel_dy = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Fractal area mouse interaction — drag to pan, wheel to zoom toward cursor
// ---------------------------------------------------------------------------

static void handle_fractal_input(State& s, float bx, float by) {
    bool over_fractal = (bx >= 0.0f && bx < static_cast<float>(k_img_w) &&
                         by >= 0.0f && by < static_cast<float>(k_img_h));

    float scale  = 2.0f / s.zoom;
    float aspect = static_cast<float>(k_img_w) / static_cast<float>(k_img_h);

    // Drag with left button → pan
    bool mouse_left = s.mouse_btn[0];
    if (mouse_left && !s.prev_mouse_left && over_fractal) {
        s.drag_active         = true;
        s.drag_start_bx       = bx;
        s.drag_start_by       = by;
        s.drag_start_center_x = s.center_x;
        s.drag_start_center_y = s.center_y;
    }
    if (!mouse_left) s.drag_active = false;
    if (s.drag_active) {
        s.center_x = s.drag_start_center_x
                   + (s.drag_start_bx - bx) / static_cast<float>(k_img_w) * scale * aspect;
        s.center_y = s.drag_start_center_y
                   + (s.drag_start_by - by) / static_cast<float>(k_img_h) * scale;
        s.app->set_center(s.center_x, s.center_y);
    }
    s.prev_mouse_left = mouse_left;

    // Wheel over fractal → zoom toward cursor
    if (s.wheel_dy != 0.0f && over_fractal) {
        float zr = (bx / static_cast<float>(k_img_w) - 0.5f) * scale * aspect + s.center_x;
        float zi = (by / static_cast<float>(k_img_h) - 0.5f) * scale            + s.center_y;

        s.zoom *= std::exp(s.wheel_dy * 0.2f);
        s.zoom  = std::max(0.01f, std::min(5000.0f, s.zoom));

        float ns = 2.0f / s.zoom;
        s.center_x = zr - (bx / static_cast<float>(k_img_w) - 0.5f) * ns * aspect;
        s.center_y = zi - (by / static_cast<float>(k_img_h) - 0.5f) * ns;

        s.app->set_zoom(s.zoom);
        s.app->set_center(s.center_x, s.center_y);

        s.wheel_dy = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// ImGui UI layout
// ---------------------------------------------------------------------------

static void build_ui(State& s) {
    const float fw = static_cast<float>(k_img_w);
    const float fh = static_cast<float>(k_img_h);
    const float cp = static_cast<float>(k_ctrl_w);

    // Fractal viewport (left)
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(fw, fh), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##fractal", nullptr,
        ImGuiWindowFlags_NoTitleBar     | ImGuiWindowFlags_NoResize      |
        ImGuiWindowFlags_NoMove         | ImGuiWindowFlags_NoScrollbar   |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::Image(static_cast<ImTextureID>(s.fractal_tex), ImVec2(fw, fh));
    ImGui::End();

    // Controls panel (right)
    ImGui::SetNextWindowPos(ImVec2(fw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(cp, fh), ImGuiCond_Always);
    ImGui::Begin("Controls", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Multi-thread");
    ImGui::Separator();
    ImGui::Text("Render   %.1f fps", s.render_fps);
    ImGui::Text("Pipeline %.1f fps", s.app->get_fps());
    ImGui::Text("Threads  %u workers / %u compute",
                static_cast<unsigned>(s.app->get_num_workers()),
                static_cast<unsigned>(s.app->get_num_compute_procs()));
    ImGui::Separator();

    if (s.app->is_paused()) {
        if (ImGui::Button("Start", ImVec2(-1, 0))) s.app->resume();
    } else {
        if (ImGui::Button("Stop",  ImVec2(-1, 0))) s.app->pause();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Julia  c = a + bi");
    bool c_changed = false;
    float bw = ImGui::GetFrameHeight(), sp = ImGui::GetStyle().ItemSpacing.x;
    if (ImGui::Button("-##ar")) { s.c_real -= 0.0005f; c_changed = true; } ImGui::SameLine();
    ImGui::SetNextItemWidth(-bw - sp);
    c_changed |= ImGui::SliderFloat("##ar", &s.c_real, -2.0f, 2.0f, "a=%.4f");
    ImGui::SameLine();
    if (ImGui::Button("+##ar")) { s.c_real += 0.0005f; c_changed = true; }
    if (ImGui::Button("-##ai")) { s.c_imag -= 0.0005f; c_changed = true; } ImGui::SameLine();
    ImGui::SetNextItemWidth(-bw - sp);
    c_changed |= ImGui::SliderFloat("##ai", &s.c_imag, -2.0f, 2.0f, "b=%.4f");
    ImGui::SameLine();
    if (ImGui::Button("+##ai")) { s.c_imag += 0.0005f; c_changed = true; }
    if (c_changed) s.app->set_julia_c(s.c_real, s.c_imag);

    ImGui::Separator();
    ImGui::TextDisabled("Viewport");
    if (ImGui::SliderFloat("zoom", &s.zoom, 0.1f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
        s.app->set_zoom(s.zoom);
    bool center_changed = false;
    center_changed |= ImGui::SliderFloat("center x", &s.center_x, -2.0f, 2.0f, "%.3f");
    center_changed |= ImGui::SliderFloat("center y", &s.center_y, -2.0f, 2.0f, "%.3f");
    if (center_changed) s.app->set_center(s.center_x, s.center_y);

    ImGui::Separator();
    ImGui::TextDisabled("Performance");
    if (ImGui::SliderInt("max iter", &s.max_iter, 16, 4096))
        s.app->set_max_iter(static_cast<uint32_t>(s.max_iter));
    if (ImGui::SliderFloat("target fps", &s.target_fps, 0.0f, 120.0f, "%.0f"))
        s.app->set_target_fps(s.target_fps);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Per-frame callback (driven by requestAnimationFrame via emscripten_set_main_loop)
// ---------------------------------------------------------------------------

static void render_frame() {
    State& s = g_state;

    // Timestamp at frame start — covers full frame period for accurate FPS.
    double now_ms    = emscripten_get_now();
    double dt_ms     = (s.last_time_ms > 0.0) ? now_ms - s.last_time_ms : 1000.0 / 60.0;
    s.last_time_ms   = now_ms;
    float delta_secs = static_cast<float>(dt_ms / 1000.0);
    if (dt_ms > 0.0) {
        float inst_fps = static_cast<float>(1000.0 / dt_ms);
        s.render_fps = (s.render_fps <= 0.0f) ? inst_fps : (0.9f * s.render_fps + 0.1f * inst_fps);
    }

    // Canvas CSS scale — computed once, shared with input handling and ImGui.
    int    cw, ch;
    double css_w, css_h;
    emscripten_get_canvas_element_size("#canvas", &cw, &ch);
    css_w = cw; css_h = ch;
    emscripten_get_element_css_size("#canvas", &css_w, &css_h);
    float scale_x    = (css_w > 0.0) ? static_cast<float>(cw) / static_cast<float>(css_w) : 1.0f;
    float scale_y    = (css_h > 0.0) ? static_cast<float>(ch) / static_cast<float>(css_h) : 1.0f;
    float backing_mx = s.mouse_x * scale_x;
    float backing_my = s.mouse_y * scale_y;

    // Fractal-area mouse interaction: pan (drag) and zoom (wheel toward cursor).
    handle_fractal_input(s, backing_mx, backing_my);

    // Get the latest frame assembled by the CoreEngine pipeline (worker threads)
    auto pixels = s.app->get_latest_frame();
    if (!pixels.empty()) {
        glBindTexture(GL_TEXTURE_2D, s.fractal_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, k_img_w, k_img_h,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }

    imgui_new_frame(delta_secs, scale_x, scale_y);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    build_ui(s);
    ImGui::Render();

    // Render (reuse cw/ch from the canvas-scale block at the top of this function)
    glViewport(0, 0, cw, ch);
    glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    State& s = g_state;

    // Create and start the App + CoreEngine pipeline.
    // Declared static so the object survives emscripten_set_main_loop's longjmp.
    // The engine is never stopped (no teardown called) — it runs until the tab is closed.
    app::AppConfig cfg;
    cfg.num_workers       = 4;
    cfg.num_compute_procs = 3;
    cfg.img_width         = static_cast<uint32_t>(k_img_w);
    cfg.img_height        = static_cast<uint32_t>(k_img_h);
    cfg.tile_width        = 64;
    cfg.tile_height       = 64;
    cfg.c_real            = s.c_real;
    cfg.c_imag            = s.c_imag;
    cfg.target_fps        = s.target_fps;
    cfg.max_iter          = static_cast<uint32_t>(s.max_iter);

    static app::App julia_app(cfg);
    s.app = &julia_app;

    // Create WebGL2 context bound to <canvas id="canvas">
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion = 2;
    attrs.minorVersion = 0;
    attrs.alpha        = false;
    attrs.depth        = false;
    attrs.antialias    = false;
    s.gl_ctx = emscripten_webgl_create_context("#canvas", &attrs);
    if (s.gl_ctx <= 0) {
        std::fprintf(stderr, "Failed to create WebGL2 context\n");
        return 1;
    }
    emscripten_webgl_make_context_current(s.gl_ctx);

    // Install HTML5 event callbacks on the canvas / window
    emscripten_set_mousemove_callback("#canvas", nullptr, EM_TRUE, on_mouse_move);
    emscripten_set_mousedown_callback("#canvas", nullptr, EM_TRUE, on_mouse_button);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, on_mouse_button);
    emscripten_set_wheel_callback("#canvas", nullptr, EM_TRUE, on_wheel);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, on_key);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, on_key);

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendPlatformName = "emscripten_html5";
    ImGui::StyleColorsDark();
    ImGui_ImplOpenGL3_Init("#version 300 es");

    emscripten_set_canvas_element_size("#canvas", k_img_w + k_ctrl_w, k_img_h);

    // Fractal texture (initially empty; filled by render_frame once pipeline sends frames)
    glGenTextures(1, &s.fractal_tex);
    glBindTexture(GL_TEXTURE_2D, s.fractal_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 k_img_w, k_img_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Hand control to the browser; render_frame() is called each requestAnimationFrame.
    emscripten_set_main_loop(render_frame, 0, 1);
    return 0;
}
