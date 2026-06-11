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
    , max_iter_(cfg.max_iter)
    , cache_w_(static_cast<uint32_t>(cfg.img_width  * cfg.cache_margin))
    , cache_h_(static_cast<uint32_t>(cfg.img_height * cfg.cache_margin))
    , ref_dim_(static_cast<float>(std::min(cfg.img_width, cfg.img_height))) {}

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
        last_frame_ms_.store(static_cast<int64_t>(avg_ms + 0.5f));
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

    // Frame is fully assembled in the texture now — promote pending params so the
    // UI thread's get_cache_info() always matches what's actually in the pixels.
    cache_cx_.store(pending_cx_);
    cache_cy_.store(pending_cy_);
    cache_zoom_.store(pending_zoom_);
    cache_valid_.store(true);

    if (!paused_.load()) {
        if (is_cache_hit()) {
            cache_idle_.store(true);
            // Recheck after marking idle: a setter may have raced between the two tests.
            // If params changed we must dispatch ourselves since wake_if_idle() saw idle=false.
            if (!is_cache_hit() && cache_idle_.exchange(false))
                dispatch_frame();
        } else {
            dispatch_frame();
        }
    }
}

void FrameControllerProcObj::dispatch_frame(bool force) {
    float c_real    = c_real_.load();
    float c_imag    = c_imag_.load();
    float zoom      = zoom_.load();
    float center_x  = center_x_.load();
    float center_y  = center_y_.load();
    uint32_t max_it = max_iter_.load();

    // Check cache validity unless the caller forces a dispatch (e.g. resume()).
    if (!force && cache_valid_.load() &&
        c_real == cache_c_real_ && c_imag == cache_c_imag_ && max_it == cache_max_iter_) {
        float cz    = cache_zoom_.load();
        float ratio = cz / zoom;
        if (ratio >= 1.0f / cfg_.cache_margin) {
            float px_scale = 2.0f / (cz * ref_dim_);
            float dhw      = static_cast<float>(cfg_.img_width)  * 0.5f * ratio;
            float dhh      = static_cast<float>(cfg_.img_height) * 0.5f * ratio;
            float opx      = (center_x - cache_cx_.load()) / px_scale;
            float opy      = (center_y - cache_cy_.load()) / px_scale;
            float cw2      = static_cast<float>(cache_w_) * 0.5f;
            float ch2      = static_cast<float>(cache_h_) * 0.5f;
            if (cw2 + opx - dhw >= 0.0f && cw2 + opx + dhw <= static_cast<float>(cache_w_) &&
                ch2 + opy - dhh >= 0.0f && ch2 + opy + dhh <= static_cast<float>(cache_h_)) {
                cache_idle_.store(true);
                return;
            }
        }
    }

    // Cache miss — record dispatched params and send tiles.
    // cache_cx_/cy_/zoom_ and cache_valid_ are updated only after the assembler
    // confirms all tiles are done (in process_msg), so the UI always sees info
    // that matches the pixels actually in the texture.
    pending_cx_     = center_x;
    pending_cy_     = center_y;
    pending_zoom_   = zoom;
    cache_c_real_   = c_real;
    cache_c_imag_   = c_imag;
    cache_max_iter_ = max_it;

    auto dispatch_time = std::chrono::steady_clock::now();
    uint32_t tile_id   = 0;

    for (uint32_t ty = 0; ty < cache_h_; ty += cfg_.tile_height) {
        for (uint32_t tx = 0; tx < cache_w_; tx += cfg_.tile_width) {
            auto work               = std::make_unique<common::TileWorkMsg>();
            work->frame_id          = frame_id_;
            work->tile_id           = tile_id++;
            work->x0                = tx;
            work->y0                = ty;
            work->width             = std::min(cfg_.tile_width,  cache_w_ - tx);
            work->height            = std::min(cfg_.tile_height, cache_h_ - ty);
            work->img_width         = cache_w_;
            work->img_height        = cache_h_;
            work->display_width     = cfg_.img_width;
            work->display_height    = cfg_.img_height;
            work->c_real            = c_real;
            work->c_imag            = c_imag;
            work->center_x          = center_x;
            work->center_y          = center_y;
            work->zoom              = zoom;
            work->max_iter          = max_it;
            work->dispatch_time     = dispatch_time;

            auto& target_proc = compute_procs_[next_compute_idx_];
            next_compute_idx_ = static_cast<uint32_t>(
                (next_compute_idx_ + 1u) % compute_procs_.size());
            target_proc->post(std::move(work));
        }
    }

    ++frame_id_;
}

bool FrameControllerProcObj::is_cache_hit() const {
    if (!cache_valid_.load()) return false;
    if (c_real_.load()  != cache_c_real_  ||
        c_imag_.load()  != cache_c_imag_  ||
        max_iter_.load() != cache_max_iter_) return false;

    float zoom     = zoom_.load();
    float center_x = center_x_.load();
    float center_y = center_y_.load();
    float cz       = cache_zoom_.load();
    float ratio    = cz / zoom;
    // Too zoomed in: cache pixels would be magnified beyond the margin factor.
    if (ratio < 1.0f / cfg_.cache_margin) return false;
    float px_scale = 2.0f / (cz * ref_dim_);
    float dhw      = static_cast<float>(cfg_.img_width)  * 0.5f * ratio;
    float dhh      = static_cast<float>(cfg_.img_height) * 0.5f * ratio;
    float opx      = (center_x - cache_cx_.load()) / px_scale;
    float opy      = (center_y - cache_cy_.load()) / px_scale;
    float cw2      = static_cast<float>(cache_w_) * 0.5f;
    float ch2      = static_cast<float>(cache_h_) * 0.5f;
    return (cw2 + opx - dhw >= 0.0f) && (cw2 + opx + dhw <= static_cast<float>(cache_w_)) &&
           (ch2 + opy - dhh >= 0.0f) && (ch2 + opy + dhh <= static_cast<float>(cache_h_));
}

void FrameControllerProcObj::wake_if_idle() {
    if (cache_idle_.exchange(false) && !paused_.load())
        dispatch_frame();
}

// ---------------------------------------------------------------------------
// Thread-safe setters (called from UI thread)
// ---------------------------------------------------------------------------

void FrameControllerProcObj::set_target_fps(float fps) { target_fps_.store(fps); }
void FrameControllerProcObj::set_julia_c(float real, float imag) {
    c_real_.store(real); c_imag_.store(imag);
    wake_if_idle();
}
void FrameControllerProcObj::set_zoom(float zoom) {
    zoom_.store(zoom);
    wake_if_idle();
}
void FrameControllerProcObj::set_center(float cx, float cy) {
    center_x_.store(cx); center_y_.store(cy);
    wake_if_idle();
}
void FrameControllerProcObj::set_view(float zoom, float cx, float cy) {
    zoom_.store(zoom);
    center_x_.store(cx);
    center_y_.store(cy);
    wake_if_idle();
}
void FrameControllerProcObj::set_max_iter(uint32_t max_iter) {
    max_iter_.store(max_iter);
    wake_if_idle();
}

void FrameControllerProcObj::pause() {
    cache_idle_.store(false); // clear idle so resume() triggers a real dispatch
    paused_.store(true);
}

void FrameControllerProcObj::resume() {
    if (!paused_.exchange(false)) return;
    dispatch_frame(/*force=*/true); // explicit user action — always re-render
}

FrameControllerProcObj::CacheInfo FrameControllerProcObj::get_cache_info() const {
    return {cache_cx_.load(), cache_cy_.load(), cache_zoom_.load(),
            cache_w_, cache_h_, cache_valid_.load()};
}

float   FrameControllerProcObj::get_fps()           const { return fps_.load(); }
int64_t FrameControllerProcObj::get_last_frame_ms() const { return last_frame_ms_.load(); }
bool    FrameControllerProcObj::is_paused()         const { return paused_.load(); }

} // namespace common::controller
