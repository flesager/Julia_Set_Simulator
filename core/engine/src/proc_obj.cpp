#include "core/engine/proc_obj.hpp"

namespace core::engine {

void ProcObj::post(std::unique_ptr<Message> msg) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push(std::move(msg));
    }
    // Only schedule once: if already in the ready queue, the worker will re-check
    bool expected = false;
    if (scheduled_.compare_exchange_strong(expected, true)) {
        if (schedule_fn_) schedule_fn_(shared_from_this());
    }
}

void ProcObj::set_scheduler(std::function<void(std::shared_ptr<ProcObj>)> fn) {
    schedule_fn_ = std::move(fn);
}

std::unique_ptr<Message> ProcObj::pop_msg() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.empty()) return nullptr;
    auto msg = std::move(queue_.front());
    queue_.pop();
    return msg;
}

bool ProcObj::has_messages() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return !queue_.empty();
}

} // namespace core::engine
