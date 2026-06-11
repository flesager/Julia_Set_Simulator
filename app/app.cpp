#include "app.hpp"
#ifndef __EMSCRIPTEN__
#include "imgui_layer.hpp"
#endif

namespace app {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

App::App(AppConfig cfg) : cfg_(std::move(cfg)) {
    setup();
}

App::~App() {
    teardown();
}

void App::setup() {
    uint32_t cache_w = static_cast<uint32_t>(cfg_.img_width  * cfg_.cache_margin);
    uint32_t cache_h = static_cast<uint32_t>(cfg_.img_height * cfg_.cache_margin);

    uint32_t tiles_x       = (cache_w + cfg_.tile_width  - 1u) / cfg_.tile_width;
    uint32_t tiles_y       = (cache_h + cfg_.tile_height - 1u) / cfg_.tile_height;
    uint32_t tiles_per_frame = tiles_x * tiles_y;

    assembler_ = std::make_shared<common::assembler::FrameAssemblerProcObj>(
        cache_w, cache_h, tiles_per_frame);

    common::controller::FrameControllerProcObj::Config ctrl_cfg;
    ctrl_cfg.img_width    = cfg_.img_width;
    ctrl_cfg.img_height   = cfg_.img_height;
    ctrl_cfg.tile_width   = cfg_.tile_width;
    ctrl_cfg.tile_height  = cfg_.tile_height;
    ctrl_cfg.c_real       = cfg_.c_real;
    ctrl_cfg.c_imag       = cfg_.c_imag;
    ctrl_cfg.zoom         = cfg_.zoom;
    ctrl_cfg.center_x     = cfg_.center_x;
    ctrl_cfg.center_y     = cfg_.center_y;
    ctrl_cfg.max_iter     = cfg_.max_iter;
    ctrl_cfg.target_fps   = cfg_.target_fps;
    ctrl_cfg.cache_margin = cfg_.cache_margin;

    for (uint8_t compute_idx = 0; compute_idx < cfg_.num_compute_procs; ++compute_idx)
        compute_procs_.push_back(
            std::make_shared<common::compute::ComputeProcObj>(assembler_));

    std::vector<std::shared_ptr<core::engine::ProcObj>> ctrl_procs(
        compute_procs_.begin(), compute_procs_.end());
    controller_ = std::make_shared<common::controller::FrameControllerProcObj>(
        ctrl_cfg, std::move(ctrl_procs));

    assembler_->set_controller(controller_);

    engine_.start(cfg_.num_workers);
    for (auto& cp : compute_procs_)
        engine_.add_proc_obj(cp);
    engine_.add_proc_obj(assembler_);
    engine_.add_proc_obj(controller_); // triggers controller->start() → first frame
}

void App::teardown() {
    engine_.stop();
    compute_procs_.clear();
    assembler_.reset();
    controller_.reset();
}

void App::restart(uint16_t num_workers, uint8_t num_compute_procs) {
    cfg_.num_workers       = num_workers;
    cfg_.num_compute_procs = num_compute_procs;
    teardown();
    setup();
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

void App::run() {
    running_ = true;
#ifndef __EMSCRIPTEN__
    ImGuiLayer ui(cfg_.img_width, cfg_.img_height, cfg_);
    ui.run(*this);
    running_ = false;
#endif
}

void App::stop()          { running_ = false; }
bool App::is_running() const { return running_; }

// ---------------------------------------------------------------------------
// UI-facing accessors
// ---------------------------------------------------------------------------

float   App::get_fps()           const { return controller_->get_fps(); }
int64_t App::get_last_frame_ms() const { return controller_->get_last_frame_ms(); }

void App::set_display_size(uint32_t w, uint32_t h) {
    if (cfg_.img_width == w && cfg_.img_height == h) return;
    teardown();
    cfg_.img_width  = w;
    cfg_.img_height = h;
    setup();
}

std::vector<uint32_t> App::get_latest_frame() const {
    return assembler_->get_latest_frame();
}

bool App::is_paused() const              { return controller_->is_paused(); }
void App::pause()                        { controller_->pause(); }
void App::resume()                       { controller_->resume(); }

void App::set_target_fps(float fps)      { cfg_.target_fps = fps; controller_->set_target_fps(fps); }
void App::set_julia_c(float r, float i)  { cfg_.c_real = r; cfg_.c_imag = i; controller_->set_julia_c(r, i); }
void App::set_zoom(float zoom)           { cfg_.zoom = zoom; controller_->set_zoom(zoom); }
void App::set_center(float cx, float cy) { cfg_.center_x = cx; cfg_.center_y = cy; controller_->set_center(cx, cy); }
void App::set_view(float zoom, float cx, float cy) { cfg_.zoom = zoom; cfg_.center_x = cx; cfg_.center_y = cy; controller_->set_view(zoom, cx, cy); }
void App::set_max_iter(uint32_t n)       { cfg_.max_iter = n; controller_->set_max_iter(n); }

common::controller::FrameControllerProcObj::CacheInfo App::get_cache_info() const {
    return controller_->get_cache_info();
}
uint32_t App::get_cache_width()  const { return controller_->cache_width();  }
uint32_t App::get_cache_height() const { return controller_->cache_height(); }

} // namespace app
