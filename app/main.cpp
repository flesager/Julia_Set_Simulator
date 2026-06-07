#include "app.hpp"
#include <atomic>
#include <csignal>
#include <cstdlib>

namespace {
std::atomic<bool> g_running{true};
app::App*         g_app{nullptr};

void on_signal(int) {
    g_running.store(false);
    if (g_app) g_app->stop();
}
} // namespace

int main() {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    app::AppConfig cfg;
    // Defaults produce a classic Julia set at a comfortable 60 fps cap.
    // These will become ImGui sliders once the graphical layer is added.
    cfg.num_workers       = 4;
    cfg.num_compute_procs = 3;
    cfg.img_width         = 800;
    cfg.img_height        = 600;
    cfg.tile_width        = 64;
    cfg.tile_height       = 64;
    cfg.c_real            = -0.7f;
    cfg.c_imag            =  0.27015f;
    cfg.target_fps        = 60.0f;
    cfg.max_iter          = 128;

    app::App julia_app(cfg);
    g_app = &julia_app;

    julia_app.run();

    return EXIT_SUCCESS;
}
