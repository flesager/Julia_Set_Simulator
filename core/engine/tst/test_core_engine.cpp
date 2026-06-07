#include "core/engine/core_engine.hpp"
#include "core/engine/message.hpp"
#include "core/engine/proc_obj.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace core::engine;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Generic message carrying a signed 32-bit payload
struct IntMessage : public Message {
    explicit IntMessage(int32_t v) : value(v) {}
    int32_t value;
};

// Waits until predicate is true or a timeout expires. Returns true if predicate met.
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout = 2000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// ProcObj that counts received messages and records the last value
class CountingObj : public ProcObj {
public:
    std::atomic<uint32_t> count{0};
    std::atomic<int32_t>  last_value{-1};

    void process_msg(std::unique_ptr<Message> msg) override {
        if (auto* m = dynamic_cast<IntMessage*>(msg.get())) {
            last_value.store(m->value);
            ++count;
        }
    }
};

// ProcObj that records which lifecycle hooks were called
class LifecycleObj : public ProcObj {
public:
    std::atomic<bool> configured{false};
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};

    void configure() override { configured = true; }
    void start()     override { started = true; }
    void stop()      override { stopped = true; }
    void process_msg(std::unique_ptr<Message>) override {}
};

// ---------------------------------------------------------------------------
// CoreEngine lifecycle
// ---------------------------------------------------------------------------

TEST(CoreEngineTest, StartAndStopWithNoObjects) {
    CoreEngine engine;
    engine.start(2);
    engine.stop();
}

TEST(CoreEngineTest, StopIsIdempotent) {
    CoreEngine engine;
    engine.start(1);
    engine.stop();
    engine.stop(); // second call must not crash or block
}

TEST(CoreEngineTest, DestructorStopsEngine) {
    // No explicit stop() call — destructor must clean up
    CoreEngine engine;
    engine.start(2);
    auto obj = std::make_shared<CountingObj>();
    engine.add_proc_obj(obj);
} // engine destroyed here

// ---------------------------------------------------------------------------
// ProcObj lifecycle
// ---------------------------------------------------------------------------

TEST(CoreEngineTest, LifecycleHooksAreCalledInOrder) {
    CoreEngine engine;
    engine.start(1);

    auto obj = std::make_shared<LifecycleObj>();
    EXPECT_FALSE(obj->configured);
    EXPECT_FALSE(obj->started);

    engine.add_proc_obj(obj);

    EXPECT_TRUE(obj->configured);
    EXPECT_TRUE(obj->started);
    EXPECT_FALSE(obj->stopped);

    engine.stop();

    EXPECT_TRUE(obj->stopped);
}

// ---------------------------------------------------------------------------
// Message processing
// ---------------------------------------------------------------------------

TEST(CoreEngineTest, SingleMessageIsProcessed) {
    CoreEngine engine;
    engine.start(1);

    auto obj = std::make_shared<CountingObj>();
    engine.add_proc_obj(obj);

    obj->post(std::make_unique<IntMessage>(42));

    ASSERT_TRUE(wait_until([&] { return obj->count.load() == 1u; }));
    EXPECT_EQ(obj->last_value.load(), 42);

    engine.stop();
}

TEST(CoreEngineTest, MessagesAreProcessedInOrder) {
    CoreEngine engine;
    engine.start(1);

    // Collect values in the order process_msg sees them
    std::vector<int32_t> received;
    std::mutex mu;

    class OrderedObj : public ProcObj {
    public:
        std::vector<int32_t>& out;
        std::mutex& mu;
        std::atomic<uint32_t> count{0};
        OrderedObj(std::vector<int32_t>& o, std::mutex& m) : out(o), mu(m) {}
        void process_msg(std::unique_ptr<Message> msg) override {
            if (auto* m = dynamic_cast<IntMessage*>(msg.get())) {
                std::lock_guard<std::mutex> lock(mu);
                out.push_back(m->value);
                ++count;
            }
        }
    };

    auto obj = std::make_shared<OrderedObj>(received, mu);
    engine.add_proc_obj(obj);

    constexpr uint32_t msg_count = 50;
    for (uint32_t msg_idx = 0; msg_idx < msg_count; ++msg_idx)
        obj->post(std::make_unique<IntMessage>(static_cast<int32_t>(msg_idx)));

    ASSERT_TRUE(wait_until([&] { return obj->count.load() == msg_count; }));

    for (uint32_t msg_idx = 0; msg_idx < msg_count; ++msg_idx)
        EXPECT_EQ(received[msg_idx], static_cast<int32_t>(msg_idx))
            << "Order violated at index " << msg_idx;

    engine.stop();
}

TEST(CoreEngineTest, BurstOfMessagesAllProcessed) {
    CoreEngine engine;
    engine.start(2);

    auto obj = std::make_shared<CountingObj>();
    engine.add_proc_obj(obj);

    constexpr uint32_t msg_count = 1000;
    for (uint32_t msg_idx = 0; msg_idx < msg_count; ++msg_idx)
        obj->post(std::make_unique<IntMessage>(static_cast<int32_t>(msg_idx)));

    ASSERT_TRUE(wait_until([&] { return obj->count.load() == msg_count; }, 5000ms));
    EXPECT_EQ(obj->count.load(), msg_count);

    engine.stop();
}

// ---------------------------------------------------------------------------
// MPSC: multiple producers, single consumer per ProcObj
// ---------------------------------------------------------------------------

TEST(CoreEngineTest, PostFromMultipleThreadsAllReceived) {
    CoreEngine engine;
    engine.start(2);

    auto obj = std::make_shared<CountingObj>();
    engine.add_proc_obj(obj);

    constexpr uint8_t  producer_count    = 8;
    constexpr uint32_t msgs_per_producer = 200;
    constexpr uint32_t total_msg_count   = producer_count * msgs_per_producer;

    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (uint8_t producer_idx = 0; producer_idx < producer_count; ++producer_idx) {
        producers.emplace_back([&obj, producer_idx] {
            for (uint32_t msg_idx = 0; msg_idx < msgs_per_producer; ++msg_idx)
                obj->post(std::make_unique<IntMessage>(
                    static_cast<int32_t>(producer_idx) * static_cast<int32_t>(msgs_per_producer)
                    + static_cast<int32_t>(msg_idx)));
        });
    }
    for (auto& producer : producers) producer.join();

    ASSERT_TRUE(wait_until([&] { return obj->count.load() == total_msg_count; }, 5000ms));
    EXPECT_EQ(obj->count.load(), total_msg_count);

    engine.stop();
}

// ---------------------------------------------------------------------------
// Multiple ProcObjs
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Shutdown responsiveness
// ---------------------------------------------------------------------------

// Regression: without the running_ check in the drain loop, stop() would block
// in join() until every queued message was processed (20 × 5ms = 100ms here).
TEST(CoreEngineTest, StopIsResponsiveDuringDrain) {
    CoreEngine engine;
    engine.start(1);

    class SlowObj : public ProcObj {
    public:
        std::atomic<uint32_t> count{0};
        void process_msg(std::unique_ptr<Message>) override {
            std::this_thread::sleep_for(5ms);
            ++count;
        }
    };

    auto obj = std::make_shared<SlowObj>();
    engine.add_proc_obj(obj);

    constexpr uint32_t msg_count = 20; // full drain = 20 × 5ms = 100ms
    for (uint32_t msg_idx = 0; msg_idx < msg_count; ++msg_idx)
        obj->post(std::make_unique<IntMessage>(static_cast<int32_t>(msg_idx)));

    auto t0 = std::chrono::steady_clock::now();
    engine.stop();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // stop() should return well before a full drain (< half of 100ms)
    EXPECT_LT(elapsed_ms, static_cast<int64_t>(50));
    // and definitely not all messages were processed
    EXPECT_LT(obj->count.load(), msg_count);
}

TEST(CoreEngineTest, MultipleProcObjsRunIndependently) {
    CoreEngine engine;
    engine.start(4);

    constexpr uint8_t  obj_count    = 4;
    constexpr uint32_t msgs_per_obj = 100;

    std::vector<std::shared_ptr<CountingObj>> objs;
    for (uint8_t obj_idx = 0; obj_idx < obj_count; ++obj_idx) {
        auto obj = std::make_shared<CountingObj>();
        engine.add_proc_obj(obj);
        objs.push_back(obj);
    }

    for (auto& obj : objs)
        for (uint32_t msg_idx = 0; msg_idx < msgs_per_obj; ++msg_idx)
            obj->post(std::make_unique<IntMessage>(static_cast<int32_t>(msg_idx)));

    for (auto& obj : objs) {
        ASSERT_TRUE(wait_until([&] { return obj->count.load() == msgs_per_obj; }, 5000ms));
        EXPECT_EQ(obj->count.load(), msgs_per_obj);
    }

    engine.stop();
}
