#include "common/assembler/frame_assembler.hpp"
#include "common/messages.hpp"
#include <algorithm>

namespace common::assembler {

FrameAssemblerProcObj::FrameAssemblerProcObj(uint32_t img_width, uint32_t img_height,
                                             uint32_t tiles_per_frame)
    : img_width_(img_width)
    , img_height_(img_height)
    , tiles_per_frame_(tiles_per_frame)
    , front_buffer_(img_width * img_height, 0xFF000000u)
    , back_buffer_(img_width * img_height, 0xFF000000u) {}

void FrameAssemblerProcObj::set_controller(
    std::shared_ptr<core::engine::ProcObj> controller) {
    controller_ = std::move(controller);
}

void FrameAssemblerProcObj::process_msg(std::unique_ptr<core::engine::Message> msg) {
    auto* result = dynamic_cast<common::TileResultMsg*>(msg.get());
    if (!result) return;

    // Copy tile pixels into the correct rows of the back buffer
    for (uint32_t row_idx = 0; row_idx < result->height; ++row_idx) {
        uint32_t dst_offset = (result->y0 + row_idx) * img_width_ + result->x0;
        uint32_t src_offset = row_idx * result->width;
        std::copy(result->pixels.begin() + src_offset,
                  result->pixels.begin() + src_offset + result->width,
                  back_buffer_.begin() + dst_offset);
    }

    ++tiles_received_;
    if (tiles_received_ < tiles_per_frame_) return;

    // All tiles arrived: swap buffers so the display sees the new frame
    {
        std::lock_guard<std::mutex> lock(swap_mutex_);
        std::swap(front_buffer_, back_buffer_);
    }

    tiles_received_ = 0;

    auto done           = std::make_unique<common::FrameDoneMsg>();
    done->frame_id      = result->frame_id;
    done->dispatch_time = result->dispatch_time;
    controller_->post(std::move(done));
}

std::vector<uint32_t> FrameAssemblerProcObj::get_latest_frame() const {
    std::lock_guard<std::mutex> lock(swap_mutex_);
    return front_buffer_;
}

} // namespace common::assembler
