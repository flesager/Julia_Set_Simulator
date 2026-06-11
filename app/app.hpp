#pragma once
#include "common/assembler/frame_assembler.hpp"
#include "common/controller/frame_controller.hpp"
#include "common/compute/compute_proc_obj.hpp"
#include "core/engine/core_engine.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace app {

struct AppConfig {
    uint16_t num_workers{4};
    uint8_t  num_compute_procs{3};
    uint32_t img_width{800};
    uint32_t img_height{600};
    uint32_t tile_width{64};
    uint32_t tile_height{64};
    float    c_real{-0.7f};
    float    c_imag{0.27015f};
    float    zoom{1.0f};
    float    center_x{0.0f};
    float    center_y{0.0f};
    float    target_fps{60.0f};
    uint32_t max_iter{128};
    float    cache_margin{1.5f}; // overscan factor; cache = img * margin per dimension
};

class App {
public:
    explicit App(AppConfig cfg);
    ~App();

    // Blocking: opens the ImGui window and runs until closed or stop() called.
    void run();

    // Called from a signal handler or another thread to request exit.
    void stop();
    bool is_running() const;

    // ── UI-facing accessors (called from ImGuiLayer on the main thread) ──

    float                 get_fps()              const;
    int64_t               get_last_frame_ms()    const;

    // Resize the fractal pipeline to a new display resolution.
    // Tears down and restarts the engine; all other cfg params are preserved.
    void set_display_size(uint32_t w, uint32_t h);
    std::vector<uint32_t> get_latest_frame() const;

    // Cache info needed by the display layer to compute UV sub-rectangle.
    common::controller::FrameControllerProcObj::CacheInfo get_cache_info() const;
    uint32_t get_cache_width()  const;
    uint32_t get_cache_height() const;

    bool is_paused() const;
    void pause();
    void resume();

    void set_target_fps(float fps);
    void set_julia_c(float real, float imag);
    void set_zoom(float zoom);
    void set_center(float cx, float cy);
    void set_view(float zoom, float cx, float cy);
    void set_max_iter(uint32_t max_iter);

    // Tears down and restarts the engine with new thread/proc counts.
    // Called from the UI thread when the user hits "Apply".
    void restart(uint16_t num_workers, uint8_t num_compute_procs);

    uint16_t get_num_workers()       const { return cfg_.num_workers; }
    uint8_t  get_num_compute_procs() const { return cfg_.num_compute_procs; }

private:
    void setup();
    void teardown();

    AppConfig cfg_;
    core::engine::CoreEngine engine_;

    std::shared_ptr<common::assembler::FrameAssemblerProcObj>     assembler_;
    std::shared_ptr<common::controller::FrameControllerProcObj>   controller_;
    std::vector<std::shared_ptr<common::compute::ComputeProcObj>> compute_procs_;

    bool running_{false};
};

} // namespace app
