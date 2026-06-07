#pragma once
#include "core/engine/proc_obj.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace core::engine {

// Reactor-based engine: dispatches pending ProcObj queues across N worker threads.
// At most N ProcObjs run concurrently; each ProcObj is only active on one thread at a time.
class CoreEngine {
public:
    ~CoreEngine();

    // Start num_workers worker threads
    void start(uint16_t num_workers);

    // Register a ProcObj: calls configure() then start(), then begins dispatching its messages
    void add_proc_obj(std::shared_ptr<ProcObj> obj);

    // Drain workers and call stop() on all registered ProcObjs
    void stop();

private:
    void worker_loop();
    void schedule(std::shared_ptr<ProcObj> obj);

    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;

    std::mutex proc_objs_mutex_;
    std::vector<std::shared_ptr<ProcObj>> proc_objs_;

    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    std::queue<std::shared_ptr<ProcObj>> ready_queue_;
};

} // namespace core::engine
