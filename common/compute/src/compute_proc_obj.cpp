#include "common/compute/compute_proc_obj.hpp"
#include "common/messages.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace common::compute {

ComputeProcObj::ComputeProcObj(std::shared_ptr<core::engine::ProcObj> assembler)
    : assembler_(std::move(assembler)) {}

// ---------------------------------------------------------------------------
// Julia set helpers
// ---------------------------------------------------------------------------

static uint32_t julia_iter(float zr, float zi, float cr, float ci, uint32_t max_iter) {
    for (uint32_t iter_idx = 0; iter_idx < max_iter; ++iter_idx) {
        float zr2 = zr * zr - zi * zi + cr;
        float zi2 = 2.0f * zr * zi + ci;
        zr = zr2;
        zi = zi2;
        if (zr * zr + zi * zi > 4.0f) return iter_idx;
    }
    return max_iter;
}

// Bernstein polynomial palette: dark blue → cyan → warm orange gradient.
// Output layout: 0xAABBGGRR (little-endian) so byte order in memory is R,G,B,A
// which matches GL_RGBA + GL_UNSIGNED_BYTE on the display side.
static uint32_t iter_to_rgba(uint32_t iter, uint32_t max_iter) {
    if (iter == max_iter) return 0xFF000000u;
    float t  = static_cast<float>(iter) / static_cast<float>(max_iter);
    float t2 = t * t;
    float t3 = t2 * t;
    float u  = 1.0f - t;
    float u2 = u * u;
    float u3 = u2 * u;
    auto r = static_cast<uint8_t>(9.0f  * u  * t3  * 255.0f);
    auto g = static_cast<uint8_t>(15.0f * u2 * t2  * 255.0f);
    auto b = static_cast<uint8_t>(8.5f  * u3 * t   * 255.0f);
    // Store as 0xAA_bb_gg_rr so memory bytes are [r, g, b, 0xFF] = GL_RGBA
    return (255u << 24)
         | (static_cast<uint32_t>(b) << 16)
         | (static_cast<uint32_t>(g) << 8)
         |  static_cast<uint32_t>(r);
}

static std::vector<uint32_t> compute_tile(const common::TileWorkMsg& work) {
    std::vector<uint32_t> pixels;
    pixels.reserve(work.width * work.height);

    // Scale maps one pixel to the complex plane; zoom > 1 zooms in
    // Use display dimensions for scale so cache pixels have the same resolution as display pixels.
    float scale = 2.0f / (work.zoom
                          * static_cast<float>(std::min(work.display_width, work.display_height)));

    for (uint32_t row_idx = 0; row_idx < work.height; ++row_idx) {
        float zi_px = (static_cast<float>(work.y0 + row_idx)
                       - static_cast<float>(work.img_height) * 0.5f) * scale
                      + work.center_y;

        for (uint32_t col_idx = 0; col_idx < work.width; ++col_idx) {
            float zr_px = (static_cast<float>(work.x0 + col_idx)
                           - static_cast<float>(work.img_width) * 0.5f) * scale
                          + work.center_x;

            uint32_t iter = julia_iter(zr_px, zi_px, work.c_real, work.c_imag, work.max_iter);
            pixels.push_back(iter_to_rgba(iter, work.max_iter));
        }
    }
    return pixels;
}

// ---------------------------------------------------------------------------
// ProcObj interface
// ---------------------------------------------------------------------------

void ComputeProcObj::process_msg(std::unique_ptr<core::engine::Message> msg) {
    auto* work = dynamic_cast<common::TileWorkMsg*>(msg.get());
    if (!work) return;

    auto result          = std::make_unique<common::TileResultMsg>();
    result->frame_id     = work->frame_id;
    result->tile_id      = work->tile_id;
    result->x0           = work->x0;
    result->y0           = work->y0;
    result->width        = work->width;
    result->height       = work->height;
    result->dispatch_time = work->dispatch_time;
    result->pixels       = compute_tile(*work);

    assembler_->post(std::move(result));
}

} // namespace common::compute
