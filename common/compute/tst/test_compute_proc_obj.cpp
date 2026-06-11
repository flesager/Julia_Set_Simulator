#include "common/compute/compute_proc_obj.hpp"
#include "common/messages.hpp"
#include "core/engine/core_engine.hpp"
#include "test_utils.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>

using namespace core::engine;

// Builds a minimal TileWorkMsg with sensible defaults
static std::unique_ptr<common::TileWorkMsg> make_work(
    uint32_t frame_id = 0, uint32_t tile_id = 0,
    uint32_t x0 = 0,   uint32_t y0 = 0,
    uint32_t w  = 16,  uint32_t h  = 16)
{
    auto work         = std::make_unique<common::TileWorkMsg>();
    work->frame_id    = frame_id;
    work->tile_id     = tile_id;
    work->x0          = x0;
    work->y0          = y0;
    work->width       = w;
    work->height      = h;
    work->img_width      = 64;
    work->img_height     = 64;
    work->display_width  = 64;
    work->display_height = 64;
    work->c_real      = -0.7f;
    work->c_imag      = 0.27015f;
    work->center_x    = 0.0f;
    work->center_y    = 0.0f;
    work->zoom        = 1.0f;
    work->max_iter    = 32;
    work->dispatch_time = std::chrono::steady_clock::now();
    return work;
}

// ---------------------------------------------------------------------------

TEST(ComputeProcObjTest, MetadataIsEchoedInResult) {
    CoreEngine engine;
    engine.start(2);

    auto capture = std::make_shared<CaptureProcObj>();
    auto compute = std::make_shared<common::compute::ComputeProcObj>(capture);
    engine.add_proc_obj(capture);
    engine.add_proc_obj(compute);

    compute->post(make_work(/*frame_id=*/7, /*tile_id=*/3,
                            /*x0=*/16, /*y0=*/32, /*w=*/8, /*h=*/12));

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == 1u; }));
    engine.stop();

    auto* result = dynamic_cast<common::TileResultMsg*>(capture->messages()[0].get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frame_id, 7u);
    EXPECT_EQ(result->tile_id,  3u);
    EXPECT_EQ(result->x0,       16u);
    EXPECT_EQ(result->y0,       32u);
    EXPECT_EQ(result->width,    8u);
    EXPECT_EQ(result->height,   12u);
}

TEST(ComputeProcObjTest, PixelCountMatchesTileDimensions) {
    CoreEngine engine;
    engine.start(2);

    auto capture = std::make_shared<CaptureProcObj>();
    auto compute = std::make_shared<common::compute::ComputeProcObj>(capture);
    engine.add_proc_obj(capture);
    engine.add_proc_obj(compute);

    compute->post(make_work(0, 0, 0, 0, 20, 15));

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == 1u; }));
    engine.stop();

    auto* result = dynamic_cast<common::TileResultMsg*>(capture->messages()[0].get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->pixels.size(), 20u * 15u);
}

TEST(ComputeProcObjTest, AllPixelsHaveOpaqueAlpha) {
    CoreEngine engine;
    engine.start(2);

    auto capture = std::make_shared<CaptureProcObj>();
    auto compute = std::make_shared<common::compute::ComputeProcObj>(capture);
    engine.add_proc_obj(capture);
    engine.add_proc_obj(compute);

    compute->post(make_work());

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == 1u; }));
    engine.stop();

    auto* result = dynamic_cast<common::TileResultMsg*>(capture->messages()[0].get());
    ASSERT_NE(result, nullptr);
    for (uint32_t pixel_idx = 0; pixel_idx < result->pixels.size(); ++pixel_idx) {
        uint8_t alpha = static_cast<uint8_t>(result->pixels[pixel_idx] >> 24);
        EXPECT_EQ(alpha, 0xFFu) << "pixel " << pixel_idx << " has non-opaque alpha";
    }
}

TEST(ComputeProcObjTest, DispatchTimeIsEchoedInResult) {
    CoreEngine engine;
    engine.start(2);

    auto capture = std::make_shared<CaptureProcObj>();
    auto compute = std::make_shared<common::compute::ComputeProcObj>(capture);
    engine.add_proc_obj(capture);
    engine.add_proc_obj(compute);

    auto work = make_work();
    auto expected_time = work->dispatch_time;
    compute->post(std::move(work));

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == 1u; }));
    engine.stop();

    auto* result = dynamic_cast<common::TileResultMsg*>(capture->messages()[0].get());
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->dispatch_time, expected_time);
}

TEST(ComputeProcObjTest, MultipleWorkItemsAreAllProcessed) {
    CoreEngine engine;
    engine.start(2);

    auto capture = std::make_shared<CaptureProcObj>();
    auto compute = std::make_shared<common::compute::ComputeProcObj>(capture);
    engine.add_proc_obj(capture);
    engine.add_proc_obj(compute);

    constexpr uint32_t tile_count = 8;
    for (uint32_t tile_idx = 0; tile_idx < tile_count; ++tile_idx)
        compute->post(make_work(0, tile_idx));

    ASSERT_TRUE(wait_until([&] { return capture->count.load() == tile_count; }));
    engine.stop();

    EXPECT_EQ(capture->count.load(), tile_count);
}
