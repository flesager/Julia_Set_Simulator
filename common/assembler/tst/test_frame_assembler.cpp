#include "common/assembler/frame_assembler.hpp"
#include "common/messages.hpp"
#include "core/engine/core_engine.hpp"
#include "test_utils.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <thread>

using namespace core::engine;

// 4×4 image, 2×2 tiles → 4 tiles per frame
static constexpr uint32_t k_img_w    = 4;
static constexpr uint32_t k_img_h    = 4;
static constexpr uint32_t k_tile_sz  = 2;
static constexpr uint32_t k_tiles    = (k_img_w / k_tile_sz) * (k_img_h / k_tile_sz);

static std::unique_ptr<common::TileResultMsg> make_result(
    uint32_t frame_id, uint32_t tile_id,
    uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
    uint32_t fill_color = 0xFF808080u)
{
    auto r           = std::make_unique<common::TileResultMsg>();
    r->frame_id      = frame_id;
    r->tile_id       = tile_id;
    r->x0            = x0;
    r->y0            = y0;
    r->width         = w;
    r->height        = h;
    r->dispatch_time = std::chrono::steady_clock::now();
    r->pixels.assign(w * h, fill_color);
    return r;
}

// Send all 4 tiles of one frame (frame_id 0) to the assembler
static void send_full_frame(
    std::shared_ptr<common::assembler::FrameAssemblerProcObj> assembler,
    uint32_t frame_id, uint32_t fill = 0xFF808080u)
{
    uint32_t tile_id = 0;
    for (uint32_t ty = 0; ty < k_img_h; ty += k_tile_sz) {
        for (uint32_t tx = 0; tx < k_img_w; tx += k_tile_sz) {
            assembler->post(make_result(frame_id, tile_id++,
                                        tx, ty, k_tile_sz, k_tile_sz, fill));
        }
    }
}

// ---------------------------------------------------------------------------

TEST(FrameAssemblerTest, NoDoneBeforeAllTilesReceived) {
    CoreEngine engine;
    engine.start(2);

    auto ctrl = std::make_shared<CaptureProcObj>();
    auto assembler = std::make_shared<common::assembler::FrameAssemblerProcObj>(
        k_img_w, k_img_h, k_tiles);
    assembler->set_controller(ctrl);

    engine.add_proc_obj(ctrl);
    engine.add_proc_obj(assembler);

    // Send k_tiles - 1 tiles
    uint32_t tile_id = 0;
    for (uint32_t ty = 0; ty < k_img_h; ty += k_tile_sz)
        for (uint32_t tx = 0; tx < k_img_w; tx += k_tile_sz) {
            if (tile_id == k_tiles - 1u) break;
            assembler->post(make_result(0, tile_id++, tx, ty, k_tile_sz, k_tile_sz));
        }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(ctrl->count.load(), 0u);

    engine.stop();
}

TEST(FrameAssemblerTest, DoneMsgPostedAfterAllTilesReceived) {
    CoreEngine engine;
    engine.start(2);

    auto ctrl = std::make_shared<CaptureProcObj>();
    auto assembler = std::make_shared<common::assembler::FrameAssemblerProcObj>(
        k_img_w, k_img_h, k_tiles);
    assembler->set_controller(ctrl);

    engine.add_proc_obj(ctrl);
    engine.add_proc_obj(assembler);

    send_full_frame(assembler, 0);

    ASSERT_TRUE(wait_until([&] { return ctrl->count.load() == 1u; }));
    engine.stop();

    auto* done = dynamic_cast<common::FrameDoneMsg*>(ctrl->messages()[0].get());
    ASSERT_NE(done, nullptr);
    EXPECT_EQ(done->frame_id, 0u);
}

TEST(FrameAssemblerTest, FrameIdEchoedInDoneMsg) {
    CoreEngine engine;
    engine.start(2);

    auto ctrl = std::make_shared<CaptureProcObj>();
    auto assembler = std::make_shared<common::assembler::FrameAssemblerProcObj>(
        k_img_w, k_img_h, k_tiles);
    assembler->set_controller(ctrl);

    engine.add_proc_obj(ctrl);
    engine.add_proc_obj(assembler);

    constexpr uint32_t expected_frame_id = 42u;
    send_full_frame(assembler, expected_frame_id);

    ASSERT_TRUE(wait_until([&] { return ctrl->count.load() == 1u; }));
    engine.stop();

    auto* done = dynamic_cast<common::FrameDoneMsg*>(ctrl->messages()[0].get());
    ASSERT_NE(done, nullptr);
    EXPECT_EQ(done->frame_id, expected_frame_id);
}

TEST(FrameAssemblerTest, PixelsWrittenAtCorrectPosition) {
    CoreEngine engine;
    engine.start(2);

    auto ctrl = std::make_shared<CaptureProcObj>();
    auto assembler = std::make_shared<common::assembler::FrameAssemblerProcObj>(
        k_img_w, k_img_h, k_tiles);
    assembler->set_controller(ctrl);

    engine.add_proc_obj(ctrl);
    engine.add_proc_obj(assembler);

    // Fill each tile with a distinct color to verify placement
    constexpr uint32_t color_tl = 0xFF0000FFu; // top-left     tile: red   in RGBA
    constexpr uint32_t color_tr = 0xFF00FF00u; // top-right    tile: green
    constexpr uint32_t color_bl = 0xFFFF0000u; // bottom-left  tile: blue
    constexpr uint32_t color_br = 0xFFFFFFFFu; // bottom-right tile: white

    assembler->post(make_result(0, 0, 0, 0, k_tile_sz, k_tile_sz, color_tl));
    assembler->post(make_result(0, 1, 2, 0, k_tile_sz, k_tile_sz, color_tr));
    assembler->post(make_result(0, 2, 0, 2, k_tile_sz, k_tile_sz, color_bl));
    assembler->post(make_result(0, 3, 2, 2, k_tile_sz, k_tile_sz, color_br));

    ASSERT_TRUE(wait_until([&] { return ctrl->count.load() == 1u; }));
    engine.stop();

    auto frame = assembler->get_latest_frame();
    ASSERT_EQ(frame.size(), k_img_w * k_img_h);

    // Top-left pixel  (x=0, y=0) → index 0
    EXPECT_EQ(frame[0 * k_img_w + 0], color_tl);
    // Top-right pixel (x=3, y=0) → index 3
    EXPECT_EQ(frame[0 * k_img_w + 3], color_tr);
    // Bottom-left (x=0, y=3) → index 12
    EXPECT_EQ(frame[3 * k_img_w + 0], color_bl);
    // Bottom-right (x=3, y=3) → index 15
    EXPECT_EQ(frame[3 * k_img_w + 3], color_br);
}

TEST(FrameAssemblerTest, FrontBufferUpdatesAfterEachFrame) {
    CoreEngine engine;
    engine.start(2);

    auto ctrl = std::make_shared<CaptureProcObj>();
    auto assembler = std::make_shared<common::assembler::FrameAssemblerProcObj>(
        k_img_w, k_img_h, k_tiles);
    assembler->set_controller(ctrl);

    engine.add_proc_obj(ctrl);
    engine.add_proc_obj(assembler);

    constexpr uint32_t color_a = 0xFF0000FFu;
    constexpr uint32_t color_b = 0xFF00FF00u;

    send_full_frame(assembler, 0, color_a);
    ASSERT_TRUE(wait_until([&] { return ctrl->count.load() == 1u; }));

    auto frame_a = assembler->get_latest_frame();

    send_full_frame(assembler, 1, color_b);
    ASSERT_TRUE(wait_until([&] { return ctrl->count.load() == 2u; }));

    auto frame_b = assembler->get_latest_frame();

    engine.stop();

    EXPECT_EQ(frame_a[0], color_a);
    EXPECT_EQ(frame_b[0], color_b);
    EXPECT_NE(frame_a[0], frame_b[0]);
}
