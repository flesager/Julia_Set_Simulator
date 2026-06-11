#pragma once
#include "core/engine/message.hpp"
#include <chrono>
#include <cstdint>
#include <vector>

namespace common {

// Dispatched by FrameControllerProcObj → ComputeProcObj (one per tile per frame)
struct TileWorkMsg : core::engine::Message {
    uint32_t frame_id{0};
    uint32_t tile_id{0};
    uint32_t x0{0}, y0{0};           // top-left pixel of this tile
    uint32_t width{0}, height{0};    // tile dimensions
    uint32_t img_width{0}, img_height{0};       // cache buffer dimensions (for centering)
    uint32_t display_width{800}, display_height{600}; // display dimensions (for scale)
    float    c_real{-0.7f}, c_imag{0.27015f};
    float    center_x{0.0f}, center_y{0.0f};
    float    zoom{1.0f};
    uint32_t max_iter{128};
    std::chrono::steady_clock::time_point dispatch_time;
};

// Dispatched by ComputeProcObj → FrameAssemblerProcObj
struct TileResultMsg : core::engine::Message {
    uint32_t frame_id{0};
    uint32_t tile_id{0};
    uint32_t x0{0}, y0{0};
    uint32_t width{0}, height{0};
    std::vector<uint32_t> pixels;    // row-major; each uint32_t = 0xAA_bb_gg_rr (LE) → bytes [R,G,B,A] → GL_RGBA
    std::chrono::steady_clock::time_point dispatch_time;
};

// Dispatched by FrameAssemblerProcObj → FrameControllerProcObj when all tiles arrive
struct FrameDoneMsg : core::engine::Message {
    uint32_t frame_id{0};
    std::chrono::steady_clock::time_point dispatch_time;
};

} // namespace common
