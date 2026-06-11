#pragma once
#include "app.hpp"
#include <cstdint>

// Forward-declare SDL/GL handles so this header stays include-light.
struct SDL_Window;
using SDL_GLContext = void*;
using GLuint = unsigned int;

namespace app {

// Owns the SDL2 window, OpenGL context, and ImGui lifecycle.
// Call run(app) from the main thread — it blocks until the window is closed.
class ImGuiLayer {
public:
    ImGuiLayer(uint32_t img_w, uint32_t img_h, const AppConfig& cfg);
    ~ImGuiLayer();

    void run(App& app);

private:
    void init();
    void shutdown();
    void build_ui(App& app);
    // Resize fractal area + texture + pipeline to match the new window dimensions.
    void resize_to(App& app, int win_w, int win_h);

    uint32_t img_w_;
    uint32_t img_h_;
    uint32_t cache_w_;
    uint32_t cache_h_;
    float    cache_margin_;

    // UI state — initialised from AppConfig, then owned by the slider widgets
    float    c_real_;
    float    c_imag_;
    float    zoom_;
    float    center_x_;
    float    center_y_;
    float    target_fps_;
    int      max_iter_;
    int      num_workers_;
    int      num_compute_procs_;

    // Drag-to-pan state
    bool  drag_active_{false};
    float drag_start_mx_{0.0f};
    float drag_start_my_{0.0f};
    float drag_start_center_x_{0.0f};
    float drag_start_center_y_{0.0f};

    SDL_Window*   window_{nullptr};
    SDL_GLContext gl_ctx_{nullptr};
    GLuint        fractal_tex_{0};
    bool          running_{false};
};

} // namespace app
