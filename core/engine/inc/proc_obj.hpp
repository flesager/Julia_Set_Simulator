#pragma once
#include "core/engine/message.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace core::engine {

class CoreEngine;

// A processing object: owns a single MPSC queue and processes messages one at a time.
// Derive from this class and implement process_msg().
class ProcObj : public std::enable_shared_from_this<ProcObj> {
public:
    virtual ~ProcObj() = default;

    // Lifecycle hooks called by CoreEngine
    virtual void configure() {}
    virtual void start() {}
    virtual void stop() {}

    // Called by a worker thread for each message in the queue
    virtual void process_msg(std::unique_ptr<Message> msg) = 0;

    // Post a message to this object's queue from any thread
    void post(std::unique_ptr<Message> msg);

private:
    friend class CoreEngine;

    void set_scheduler(std::function<void(std::shared_ptr<ProcObj>)> fn);
    std::unique_ptr<Message> pop_msg();
    bool has_messages() const;

    mutable std::mutex queue_mutex_;
    std::queue<std::unique_ptr<Message>> queue_;
    std::atomic<bool> scheduled_{false};
    std::function<void(std::shared_ptr<ProcObj>)> schedule_fn_;
};

} // namespace core::engine
