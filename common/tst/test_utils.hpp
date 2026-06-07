#pragma once
#include "core/engine/message.hpp"
#include "core/engine/proc_obj.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// Queues every received message; count is atomic so tests can poll it.
// Access messages() only after engine.stop() — workers are joined by then.
class CaptureProcObj : public core::engine::ProcObj {
public:
    std::atomic<uint32_t> count{0};

    void process_msg(std::unique_ptr<core::engine::Message> msg) override {
        std::lock_guard<std::mutex> lock(mu_);
        messages_.push_back(std::move(msg));
        ++count;
    }

    std::vector<std::unique_ptr<core::engine::Message>>& messages() {
        return messages_;
    }

private:
    std::mutex mu_;
    std::vector<std::unique_ptr<core::engine::Message>> messages_;
};

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout = 2000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
