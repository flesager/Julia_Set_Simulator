#include "common/controller/frame_controller.hpp"
#include "common/messages.hpp"
#include <algorithm>
#include <chrono>
#include <thread>

namespace common::controller {

FrameControllerProcObj::FrameControllerProcObj(
    Config cfg,
    std::vector<std::shared_ptr<core::engine::ProcObj>> compute_procs)
    : cfg_(cfg)
    , compute_procs_(std::move(compute_procs))
    , target_fps_(cfg.target_fps)
    , c_real_(cfg.c_real)
    , c_imag_(cfg.c_imag)
    , zoom_(cfg.zoom)
    , center_x_(cfg.center_x)
    , center_y_(cfg.center_y)
    , max_iter_(cfg.max_iter) {}

void FrameControllerProcObj::start() {
    dispatch_frame();
}

void FrameControllerProcObj::process_msg(std::unique_ptr<core::engine::Message> msg) {
    auto* done = dynamic_cast<common::FrameDoneMsg*>(msg.get());
    if (!done) return;

    auto now = std::chrono::steady_clock::now();
    int64_t frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - done->dispatch_time).count();

    // Update rolling FPS average
    frame_times_ms_[fps_idx_] = frame_ms;
    fps_idx_    = static_cast<uint8_t>((fps_idx_ + 1u) % fps_window_size);
    if (fps_filled_ < fps_window_size) ++fps_filled_;

    if (fps_filled_ > 0 && frame_ms > 0) {
        int64_t total_ms = 0;
        for (uint8_t slot_idx = 0; slot_idx < fps_filled_; ++slot_idx)
            total_ms += frame_times_ms_[slot_idx];
        float avg_ms = static_cast<float>(total_ms) / static_cast<float>(fps_filled_);
        fps_.store(1000.0f / avg_ms);
    }

    // Throttle: if we're faster than target_fps, sleep for the remainder
    // Note: this blocks a worker thread for the sleep duration; acceptable since
    // the controller fires at most once per frame and the sleep is short.
    float target = target_fps_.load();
    if (target > 0.0f) {
        int64_t target_ms = static_cast<int64_t>(1000.0f / target);
        int64_t sleep_ms  = target_ms - frame_ms;
        if (sleep_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }

    if (!paused_.load())
        dispatch_frame();
}

void FrameControllerProcObj::dispatch_frame() {
    auto dispatch_time = std::chrono::steady_clock::now();

    float c_real   = c_real_.load();
    float c_imag   = c_imag_.load();
    float zoom     = zoom_.load();
    float center_x = center_x_.load();
    float center_y = center_y_.load();

    uint32_t tile_id = 0;
    for (uint32_t ty = 0; ty < cfg_.img_height; ty += cfg_.tile_height) {
        for (uint32_t tx = 0; tx < cfg_.img_width; tx += cfg_.tile_width) {
            auto work             = std::make_unique<common::TileWorkMsg>();
            work->frame_id        = frame_id_;
            work->tile_id         = tile_id++;
            work->x0              = tx;
            work->y0              = ty;
            work->width           = std::min(cfg_.tile_width,  cfg_.img_width  - tx);
            work->height          = std::min(cfg_.tile_height, cfg_.img_height - ty);
            work->img_width       = cfg_.img_width;
            work->img_height      = cfg_.img_height;
            work->c_real          = c_real;
            work->c_imag          = c_imag;
            work->center_x        = center_x;
            work->center_y        = center_y;
            work->zoom            = zoom;
            work->max_iter        = max_iter_.load();
            work->dispatch_time   = dispatch_time;

            auto& target_proc = compute_procs_[next_compute_idx_];
            next_compute_idx_ = static_cast<uint32_t>(
                (next_compute_idx_ + 1u) % compute_procs_.size());
            target_proc->post(std::move(work));
        }
    }

    ++frame_id_;
}

// ---------------------------------------------------------------------------
// Thread-safe setters (called from UI thread)
// ---------------------------------------------------------------------------

void FrameControllerProcObj::set_target_fps(float fps) { target_fps_.store(fps); }
void FrameControllerProcObj::set_julia_c(float real, float imag) {
    c_real_.store(real);
    c_imag_.store(imag);
}
void FrameControllerProcObj::set_zoom(float zoom)         { zoom_.store(zoom); }
void FrameControllerProcObj::set_center(float cx, float cy) {
    center_x_.store(cx);
    center_y_.store(cy);
}
void FrameControllerProcObj::set_max_iter(uint32_t max_iter) { max_iter_.store(max_iter); }

void FrameControllerProcObj::pause() { paused_.store(true); }

void FrameControllerProcObj::resume() {
    if (!paused_.exchange(false)) return;
    dispatch_frame(); // re-enter the pipeline from the UI thread
}

float FrameControllerProcObj::get_fps()   const { return fps_.load(); }
bool  FrameControllerProcObj::is_paused() const { return paused_.load(); }

} // namespace common::controller
