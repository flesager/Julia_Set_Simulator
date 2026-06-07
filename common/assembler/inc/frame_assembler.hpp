#pragma once
#include "core/engine/proc_obj.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace common::assembler {

// Collects TileResultMsg from all ComputeProcObjs.
// When all tiles of a frame arrive, swaps the double-buffer and posts FrameDoneMsg
// to the controller. get_latest_frame() is safe to call from any thread (display).
class FrameAssemblerProcObj : public core::engine::ProcObj {
public:
    FrameAssemblerProcObj(uint32_t img_width, uint32_t img_height,
                          uint32_t tiles_per_frame);

    void set_controller(std::shared_ptr<core::engine::ProcObj> controller);

    void process_msg(std::unique_ptr<core::engine::Message> msg) override;

    // Returns a copy of the most recently completed framebuffer (RGBA, row-major).
    // Safe to call from the display/UI thread.
    std::vector<uint32_t> get_latest_frame() const;

private:
    uint32_t img_width_;
    uint32_t img_height_;
    uint32_t tiles_per_frame_;

    std::shared_ptr<core::engine::ProcObj> controller_;

    // Double-buffer: worker writes to back, display reads from front
    std::vector<uint32_t> front_buffer_;
    std::vector<uint32_t> back_buffer_;
    mutable std::mutex    swap_mutex_;

    uint32_t tiles_received_{0};
};

} // namespace common::assembler
