#include "imgui_layer.hpp"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#include <SDL.h>
#include <GL/gl.h>
#include <cmath>
#include <cstdio>

namespace app {

static constexpr int   k_ctrl_panel_w = 300;
static constexpr float k_glsl_version[] = {'#'};   // unused — version passed as string

ImGuiLayer::ImGuiLayer(uint32_t img_w, uint32_t img_h, const AppConfig& cfg)
    : img_w_(img_w)
    , img_h_(img_h)
    , c_real_(cfg.c_real)
    , c_imag_(cfg.c_imag)
    , zoom_(1.0f)
    , center_x_(0.0f)
    , center_y_(0.0f)
    , target_fps_(cfg.target_fps)
    , max_iter_(static_cast<int>(cfg.max_iter))
    , num_workers_(static_cast<int>(cfg.num_workers))
    , num_compute_procs_(static_cast<int>(cfg.num_compute_procs)) {}

ImGuiLayer::~ImGuiLayer() {
    shutdown();
}

// ---------------------------------------------------------------------------
// SDL2 + OpenGL3 + ImGui initialisation
// ---------------------------------------------------------------------------

void ImGuiLayer::init() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);   // no depth buffer needed

    int win_w = static_cast<int>(img_w_) + k_ctrl_panel_w;
    int win_h = static_cast<int>(img_h_);
    window_ = SDL_CreateWindow(
        "Julia Set Simulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        return;
    }

    gl_ctx_ = SDL_GL_CreateContext(window_);
    SDL_GL_MakeCurrent(window_, gl_ctx_);
    SDL_GL_SetSwapInterval(0);  // we throttle ourselves; don't also vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window_, gl_ctx_);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    // Allocate the fractal texture (filled on first frame)
    glGenTextures(1, &fractal_tex_);
    glBindTexture(GL_TEXTURE_2D, fractal_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 static_cast<int>(img_w_), static_cast<int>(img_h_),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    running_ = true;
}

void ImGuiLayer::shutdown() {
    if (!gl_ctx_) return;
    if (fractal_tex_) { glDeleteTextures(1, &fractal_tex_); fractal_tex_ = 0; }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx_); gl_ctx_ = nullptr;
    SDL_DestroyWindow(window_);    window_  = nullptr;
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Main render loop
// ---------------------------------------------------------------------------

void ImGuiLayer::run(App& app) {
    init();
    if (!window_) return;

    while (running_ && app.is_running()) {
        // Poll SDL events
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running_ = false;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                running_ = false;
        }

        // Upload the latest completed fractal frame to the GPU texture
        auto pixels = app.get_latest_frame();
        if (!pixels.empty()) {
            glBindTexture(GL_TEXTURE_2D, fractal_tex_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            static_cast<int>(img_w_), static_cast<int>(img_h_),
                            GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        }

        // Build ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        build_ui(app);

        // Render
        ImGui::Render();
        int draw_w, draw_h;
        SDL_GL_GetDrawableSize(window_, &draw_w, &draw_h);
        glViewport(0, 0, draw_w, draw_h);
        glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window_);
    }
}

// ---------------------------------------------------------------------------
// UI layout
// ---------------------------------------------------------------------------

void ImGuiLayer::build_ui(App& app) {
    float fw = static_cast<float>(img_w_);
    float fh = static_cast<float>(img_h_);
    float cp = static_cast<float>(k_ctrl_panel_w);

    // ── Fractal viewport (left, fills image area) ─────────────────────────
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(fw, fh), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##fractal", nullptr,
        ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoResize  |
        ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoScrollbar|
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::Image(static_cast<ImTextureID>(fractal_tex_),
                 ImVec2(fw, fh));

    // Mouse interaction on the fractal viewport
    {
        ImGuiIO& io    = ImGui::GetIO();
        float    scale = 2.0f / zoom_;
        float    asp   = fw / fh;

        // Drag to pan — start only when clicking on the fractal image
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            drag_active_          = true;
            ImVec2 p              = ImGui::GetMousePos();
            drag_start_mx_        = p.x;
            drag_start_my_        = p.y;
            drag_start_center_x_  = center_x_;
            drag_start_center_y_  = center_y_;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_active_ = false;
        if (drag_active_) {
            ImVec2 p = ImGui::GetMousePos();
            center_x_ = drag_start_center_x_ + (drag_start_mx_ - p.x) / fw * scale * asp;
            center_y_ = drag_start_center_y_ + (drag_start_my_ - p.y) / fh * scale;
            app.set_center(center_x_, center_y_);
        }

        // Wheel zoom toward cursor
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
            ImVec2 p  = ImGui::GetMousePos();
            float  zr = (p.x / fw - 0.5f) * scale * asp + center_x_;
            float  zi = (p.y / fh - 0.5f) * scale        + center_y_;
            zoom_ *= std::exp(io.MouseWheel * 0.2f);
            zoom_  = std::max(0.01f, std::min(5000.0f, zoom_));
            float ns  = 2.0f / zoom_;
            center_x_ = zr - (p.x / fw - 0.5f) * ns * asp;
            center_y_ = zi - (p.y / fh - 0.5f) * ns;
            app.set_zoom(zoom_);
            app.set_center(center_x_, center_y_);
        }
    }

    ImGui::End();

    // ── Controls panel (right) ────────────────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(fw, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(cp, fh), ImGuiCond_Always);
    ImGui::Begin("Controls", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    // FPS display
    ImGui::Text("FPS  %.1f", app.get_fps());
    ImGui::Separator();

    // Start / Stop
    if (app.is_paused()) {
        if (ImGui::Button("Start", ImVec2(-1, 0))) app.resume();
    } else {
        if (ImGui::Button("Stop",  ImVec2(-1, 0))) app.pause();
    }

    // ── Julia parameter ──
    ImGui::Separator();
    ImGui::TextDisabled("Julia  c = a + bi");
    bool  c_changed = false;
    float step      = 0.0005f;
    float bw        = ImGui::GetFrameHeight();
    float sp        = ImGui::GetStyle().ItemSpacing.x;
    float sq        = 150.0f;
    float base_x    = ImGui::GetCursorPosX();
    float base_y    = ImGui::GetCursorPosY();
    float y_mid     = base_y + bw + sp;
    float mid_yo    = (sq - bw) * 0.5f;
    // +b above square, centred
    ImGui::SetCursorPos(ImVec2(base_x + bw + sp + (sq - bw) * 0.5f, base_y));
    if (ImGui::Button("+##bi")) { c_imag_ += step; c_changed = true; }
    // -a left of square, vertically centred
    ImGui::SetCursorPos(ImVec2(base_x, y_mid + mid_yo));
    if (ImGui::Button("-##ar")) { c_real_ -= step; c_changed = true; }
    // 2D picker square
    ImGui::SetCursorPos(ImVec2(base_x + bw + sp, y_mid));
    ImVec2 sq_scr = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##julia_2d", ImVec2(sq, sq));
    if (ImGui::IsItemActive()) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        float  nx = fminf(fmaxf((mp.x - sq_scr.x) / sq, 0.0f), 1.0f);
        float  ny = fminf(fmaxf((mp.y - sq_scr.y) / sq, 0.0f), 1.0f);
        c_real_   = -2.0f + nx * 4.0f;
        c_imag_   =  2.0f - ny * 4.0f;
        c_changed = true;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2      p2 = ImVec2(sq_scr.x + sq, sq_scr.y + sq);
    dl->AddRectFilled(sq_scr, p2, IM_COL32(30, 30, 30, 255));
    dl->AddRect(sq_scr, p2, IM_COL32(150, 150, 150, 255));
    dl->AddLine(ImVec2(sq_scr.x,            sq_scr.y + sq * 0.5f),
                ImVec2(p2.x,                sq_scr.y + sq * 0.5f), IM_COL32(60, 60, 60, 255));
    dl->AddLine(ImVec2(sq_scr.x + sq * 0.5f, sq_scr.y),
                ImVec2(sq_scr.x + sq * 0.5f, p2.y),               IM_COL32(60, 60, 60, 255));
    float px = sq_scr.x + (c_real_ + 2.0f) / 4.0f * sq;
    float py = sq_scr.y + (2.0f - c_imag_) / 4.0f * sq;
    dl->AddCircleFilled(ImVec2(px, py), 5.0f, IM_COL32(255, 100,  50, 255));
    dl->AddCircle(      ImVec2(px, py), 5.0f, IM_COL32(255, 200, 150, 255));
    // +a right of square, vertically centred
    ImGui::SetCursorPos(ImVec2(base_x + bw + sp + sq + sp, y_mid + mid_yo));
    if (ImGui::Button("+##ar")) { c_real_ += step; c_changed = true; }
    // -b below square, centred
    ImGui::SetCursorPos(ImVec2(base_x + bw + sp + (sq - bw) * 0.5f, y_mid + sq + sp));
    if (ImGui::Button("-##bi")) { c_imag_ -= step; c_changed = true; }
    // value readout and cursor advance
    ImGui::SetCursorPos(ImVec2(base_x, y_mid + sq + sp + bw + sp));
    ImGui::Text("a=%.4f  b=%.4f", c_real_, c_imag_);
    if (c_changed) app.set_julia_c(c_real_, c_imag_);

    // ── Viewport ──
    ImGui::Separator();
    ImGui::TextDisabled("Viewport");
    if (ImGui::SliderFloat("zoom", &zoom_, 0.1f, 20.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic))
        app.set_zoom(zoom_);

    bool pan_changed = false;
    pan_changed |= ImGui::SliderFloat("center x", &center_x_, -2.0f, 2.0f, "%.3f");
    pan_changed |= ImGui::SliderFloat("center y", &center_y_, -2.0f, 2.0f, "%.3f");
    if (pan_changed) app.set_center(center_x_, center_y_);

    // ── Performance ──
    ImGui::Separator();
    ImGui::TextDisabled("Performance");
    if (ImGui::SliderFloat("target FPS", &target_fps_, 0.0f, 120.0f,
                           target_fps_ < 1.0f ? "unlimited" : "%.0f"))
        app.set_target_fps(target_fps_);

    if (ImGui::SliderInt("max iter", &max_iter_, 16, 4096))
        app.set_max_iter(static_cast<uint32_t>(max_iter_));

    // ── Engine topology ──
    ImGui::Separator();
    ImGui::TextDisabled("Engine (restart to apply)");
    ImGui::SliderInt("threads",       &num_workers_,      1, 100);
    ImGui::SliderInt("compute procs", &num_compute_procs_, 1, 100);

    bool changed = (num_workers_       != static_cast<int>(app.get_num_workers()) ||
                    num_compute_procs_ != static_cast<int>(app.get_num_compute_procs()));
    if (!changed) ImGui::BeginDisabled();
    if (ImGui::Button("Apply & restart", ImVec2(-1, 0)))
        app.restart(static_cast<uint16_t>(num_workers_),
                    static_cast<uint8_t>(num_compute_procs_));
    if (!changed) ImGui::EndDisabled();

    ImGui::End();
}

} // namespace app
