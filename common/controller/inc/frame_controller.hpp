#pragma once
#include "core/engine/proc_obj.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace common::controller {

// Manages the frame loop:
//   start() → dispatch_frame() → [wait for FrameDoneMsg] → update FPS →
//   throttle sleep → dispatch_frame() → …
//
// All setter methods are thread-safe (called from the UI thread).
class FrameControllerProcObj : public core::engine::ProcObj {
public:
    struct Config {
        uint32_t img_width{800};
        uint32_t img_height{600};
        uint32_t tile_width{64};
        uint32_t tile_height{64};
        float    c_real{-0.7f};
        float    c_imag{0.27015f};
        float    center_x{0.0f};
        float    center_y{0.0f};
        float    zoom{1.0f};
        uint32_t max_iter{128};
        float    target_fps{60.0f};  // 0 = unlimited
    };

    FrameControllerProcObj(Config cfg,
                           std::vector<std::shared_ptr<core::engine::ProcObj>> compute_procs);

    void start() override;
    void process_msg(std::unique_ptr<core::engine::Message> msg) override;

    // Thread-safe setters/controls for UI interaction
    void set_target_fps(float fps);
    void set_julia_c(float real, float imag);
    void set_zoom(float zoom);
    void set_center(float cx, float cy);
    void set_max_iter(uint32_t max_iter);

    void pause();   // stop dispatching frames after the current one finishes
    void resume();  // re-enter the frame loop from the UI thread

    float    get_fps()      const;
    bool     is_paused()    const;

private:
    void dispatch_frame();

    Config cfg_;
    std::vector<std::shared_ptr<core::engine::ProcObj>> compute_procs_;
    uint32_t frame_id_{0};
    uint32_t next_compute_idx_{0};

    std::atomic<float>    target_fps_;
    std::atomic<float>    c_real_;
    std::atomic<float>    c_imag_;
    std::atomic<float>    zoom_;
    std::atomic<float>    center_x_;
    std::atomic<float>    center_y_;
    std::atomic<uint32_t> max_iter_;
    std::atomic<bool>     paused_{false};
    std::atomic<float>    fps_{0.0f};

    // Rolling average over the last fps_window_size frames
    static constexpr uint8_t fps_window_size = 30;
    std::array<int64_t, fps_window_size> frame_times_ms_{};
    uint8_t fps_idx_{0};
    uint8_t fps_filled_{0};
};

} // namespace common::controller
