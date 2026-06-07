#include "common/controller/frame_controller.hpp"
#include "common/messages.hpp"
#include "core/engine/core_engine.hpp"
#include "test_utils.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <thread>

using namespace core::engine;
using common::controller::FrameControllerProcObj;

// 4×4 image, 2×2 tiles → 4 tiles per frame
static constexpr uint32_t k_img_w   = 4;
static constexpr uint32_t k_img_h   = 4;
static constexpr uint32_t k_tile_sz = 2;
static constexpr uint32_t k_tiles   = (k_img_w / k_tile_sz) * (k_img_h / k_tile_sz);

// Base config used by all tests; target_fps=0 disables the throttle sleep
static FrameControllerProcObj::Config make_cfg(float c_real = -0.7f, float c_imag = 0.27015f) {
    FrameControllerProcObj::Config cfg;
    cfg.img_width  = k_img_w;
    cfg.img_height = k_img_h;
    cfg.tile_width = k_tile_sz;
    cfg.tile_height = k_tile_sz;
    cfg.c_real     = c_real;
    cfg.c_imag     = c_imag;
    cfg.center_x   = 0.0f;
    cfg.center_y   = 0.0f;
    cfg.zoom       = 1.0f;
    cfg.max_iter   = 32;
    cfg.target_fps = 0.0f;  // unlimited – no sleep in tests
    return cfg;
}

// Post a synthetic FrameDoneMsg to the controller (simulates assembler signal)
static void send_frame_done(std::shared_ptr<ProcObj> controller, uint32_t frame_id = 0) {
    auto done           = std::make_unique<common::FrameDoneMsg>();
    done->frame_id      = frame_id;
    done->dispatch_time = std::chrono::steady_clock::now();
    controller->post(std::move(done));
}

// ---------------------------------------------------------------------------

TEST(FrameControllerTest, StartDispatchesCorrectTileCount) {
    CoreEngine engine;
    engine.start(2);

    auto capture    = std::make_shared<CaptureProcObj>();
    auto controller = std::make_shared<FrameControllerProcObj>(
        make_cfg(), std::vector<std::shared_ptr<ProcObj>>{capture});

    engine.add_proc_obj(capture);
    engine.add_proc_obj(controller);   // start() → dispatch_frame() → k_tiles msgs

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == k_tiles; }));
    engine.stop();

    EXPECT_EQ(capture->count.load(), k_tiles);
}

TEST(FrameControllerTest, TilesCoverFullImage) {
    CoreEngine engine;
    engine.start(2);

    auto capture    = std::make_shared<CaptureProcObj>();
    auto controller = std::make_shared<FrameControllerProcObj>(
        make_cfg(), std::vector<std::shared_ptr<ProcObj>>{capture});

    engine.add_proc_obj(capture);
    engine.add_proc_obj(controller);

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == k_tiles; }));
    engine.stop();

    // Sum of tile areas must equal image area
    uint32_t total_pixels = 0;
    for (auto& msg : capture->messages()) {
        auto* work = dynamic_cast<common::TileWorkMsg*>(msg.get());
        ASSERT_NE(work, nullptr);
        total_pixels += work->width * work->height;
    }
    EXPECT_EQ(total_pixels, k_img_w * k_img_h);
}

TEST(FrameControllerTest, FrameDoneMsgTriggersNextDispatch) {
    CoreEngine engine;
    engine.start(2);

    auto capture    = std::make_shared<CaptureProcObj>();
    auto controller = std::make_shared<FrameControllerProcObj>(
        make_cfg(), std::vector<std::shared_ptr<ProcObj>>{capture});

    engine.add_proc_obj(capture);
    engine.add_proc_obj(controller);

    // Wait for first frame
    ASSERT_TRUE(wait_until([&] { return capture->count.load() == k_tiles; }));

    // Simulate assembler signalling frame complete
    send_frame_done(controller, 0);

    // Second frame should arrive
    ASSERT_TRUE(wait_until([&] { return capture->count.load() == k_tiles * 2u; }));
    engine.stop();

    // All messages in the second batch should carry frame_id == 1
    auto& msgs = capture->messages();
    for (uint32_t msg_idx = k_tiles; msg_idx < k_tiles * 2u; ++msg_idx) {
        auto* work = dynamic_cast<common::TileWorkMsg*>(msgs[msg_idx].get());
        ASSERT_NE(work, nullptr);
        EXPECT_EQ(work->frame_id, 1u);
    }
}

TEST(FrameControllerTest, PauseHaltsDispatchingAfterCurrentFrame) {
    CoreEngine engine;
    engine.start(2);

    auto capture    = std::make_shared<CaptureProcObj>();
    auto controller = std::make_shared<FrameControllerProcObj>(
        make_cfg(), std::vector<std::shared_ptr<ProcObj>>{capture});

    engine.add_proc_obj(capture);
    engine.add_proc_obj(controller);

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == k_tiles; }));

    controller->pause();
    EXPECT_TRUE(controller->is_paused());

    send_frame_done(controller, 0);

    // Give worker time to run process_msg; it should NOT dispatch a new frame
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(capture->count.load(), k_tiles);

    engine.stop();
}

TEST(FrameControllerTest, ResumeRestartsPipeline) {
    CoreEngine engine;
    engine.start(2);

    auto capture    = std::make_shared<CaptureProcObj>();
    auto controller = std::make_shared<FrameControllerProcObj>(
        make_cfg(), std::vector<std::shared_ptr<ProcObj>>{capture});

    engine.add_proc_obj(capture);
    engine.add_proc_obj(controller);

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == k_tiles; }));

    controller->pause();
    send_frame_done(controller, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ASSERT_EQ(capture->count.load(), k_tiles);

    controller->resume();
    ASSERT_FALSE(controller->is_paused());

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == k_tiles * 2u; }));
    engine.stop();
}

TEST(FrameControllerTest, SetJuliaCReflectedInNextBatch) {
    CoreEngine engine;
    engine.start(2);

    auto capture    = std::make_shared<CaptureProcObj>();
    auto controller = std::make_shared<FrameControllerProcObj>(
        make_cfg(), std::vector<std::shared_ptr<ProcObj>>{capture});

    engine.add_proc_obj(capture);
    engine.add_proc_obj(controller);

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == k_tiles; }));

    constexpr float new_c_real = -0.1f;
    constexpr float new_c_imag =  0.65f;
    controller->set_julia_c(new_c_real, new_c_imag);

    send_frame_done(controller, 0);

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == k_tiles * 2u; }));
    engine.stop();

    auto& msgs = capture->messages();
    for (uint32_t msg_idx = k_tiles; msg_idx < k_tiles * 2u; ++msg_idx) {
        auto* work = dynamic_cast<common::TileWorkMsg*>(msgs[msg_idx].get());
        ASSERT_NE(work, nullptr);
        EXPECT_FLOAT_EQ(work->c_real, new_c_real);
        EXPECT_FLOAT_EQ(work->c_imag, new_c_imag);
    }
}

TEST(FrameControllerTest, TilesDistributedAcrossComputeProcs) {
    CoreEngine engine;
    engine.start(2);

    auto capture_a = std::make_shared<CaptureProcObj>();
    auto capture_b = std::make_shared<CaptureProcObj>();
    auto controller = std::make_shared<FrameControllerProcObj>(
        make_cfg(),
        std::vector<std::shared_ptr<ProcObj>>{capture_a, capture_b});

    engine.add_proc_obj(capture_a);
    engine.add_proc_obj(capture_b);
    engine.add_proc_obj(controller);

    // Wait for all tiles across both captures
    ASSERT_TRUE(wait_until([&] {
        return (capture_a->count.load() + capture_b->count.load()) == k_tiles;
    }));
    engine.stop();

    // With round-robin and 4 tiles → 2 compute procs: each gets exactly 2
    EXPECT_EQ(capture_a->count.load(), k_tiles / 2);
    EXPECT_EQ(capture_b->count.load(), k_tiles / 2);
}
