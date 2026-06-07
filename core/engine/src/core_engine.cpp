#include "core/engine/core_engine.hpp"

namespace core::engine {

CoreEngine::~CoreEngine() {
    stop();
}

void CoreEngine::start(uint16_t num_workers) {
    running_ = true;
    workers_.reserve(num_workers);
    for (uint16_t worker_idx = 0; worker_idx < num_workers; ++worker_idx) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

void CoreEngine::add_proc_obj(std::shared_ptr<ProcObj> obj) {
    obj->set_scheduler([this](std::shared_ptr<ProcObj> p) {
        schedule(std::move(p));
    });
    obj->configure();
    obj->start();

    std::lock_guard<std::mutex> lock(proc_objs_mutex_);
    proc_objs_.push_back(std::move(obj));
}

void CoreEngine::stop() {
    if (!running_.exchange(false)) return;

    ready_cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();

    std::lock_guard<std::mutex> lock(proc_objs_mutex_);
    for (auto& obj : proc_objs_) obj->stop();
    proc_objs_.clear();
}

void CoreEngine::schedule(std::shared_ptr<ProcObj> obj) {
    {
        std::lock_guard<std::mutex> lock(ready_mutex_);
        ready_queue_.push(std::move(obj));
    }
    ready_cv_.notify_one();
}

void CoreEngine::worker_loop() {
    while (running_) {
        std::shared_ptr<ProcObj> obj;
        {
            std::unique_lock<std::mutex> lock(ready_mutex_);
            ready_cv_.wait(lock, [this] {
                return !ready_queue_.empty() || !running_;
            });
            if (!running_ && ready_queue_.empty()) return;
            obj = std::move(ready_queue_.front());
            ready_queue_.pop();
        }

        // Drain all pending messages for this ProcObj
        while (auto msg = obj->pop_msg()) {
            obj->process_msg(std::move(msg));
        }

        // Release the slot; if new messages arrived during processing, re-schedule
        obj->scheduled_.store(false);
        if (obj->has_messages()) {
            bool expected = false;
            if (obj->scheduled_.compare_exchange_strong(expected, true)) {
                schedule(obj);
            }
        }
    }
}

} // namespace core::engine
