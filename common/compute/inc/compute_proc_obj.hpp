#pragma once
#include "core/engine/proc_obj.hpp"
#include <cstdint>
#include <memory>

namespace common::compute {

// Receives TileWorkMsg, computes Julia iterations for each pixel,
// posts TileResultMsg to the assembler.
class ComputeProcObj : public core::engine::ProcObj {
public:
    explicit ComputeProcObj(std::shared_ptr<core::engine::ProcObj> assembler);
    void process_msg(std::unique_ptr<core::engine::Message> msg) override;

private:
    std::shared_ptr<core::engine::ProcObj> assembler_;
};

} // namespace common::compute
