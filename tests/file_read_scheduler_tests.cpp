#include "Center/File/FileOp.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

struct TestContext {
    int passed_count = 0;
    int failed_count = 0;
};

void expectTrue(TestContext& context_, bool condition_, const std::string& message_) {
    if (condition_) {
        ++context_.passed_count;
        std::cout << "[PASS] " << message_ << '\n';
    } else {
        ++context_.failed_count;
        std::cout << "[FAIL] " << message_ << '\n';
    }
}

[[nodiscard]] auto makeBytes(std::size_t size_, std::uint32_t seed_) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(size_);
    std::uint32_t state = seed_;
    for (std::size_t index = 0; index < size_; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[index] = static_cast<std::byte>(state & 0xFFu);
    }
    return bytes;
}

[[nodiscard]] auto writeBinary(const std::filesystem::path& path_, std::span<const std::byte> bytes_) -> bool {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
    return static_cast<bool>(output);
}

void testSchedulerDirect(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "scheduler_direct.bin";
    const auto original = makeBytes(8 * 1024 * 1024, 0x13572468u);
    expectTrue(context_, writeBinary(path, original), "生成 scheduler_direct.bin");

    std::vector<std::byte> destination(original.size());

    Tool::File::PlannerConfig config{};
    config.worker_limit = 6;
    config.storage.large_file_threshold = 2 * 1024 * 1024;
    config.storage.split_chunk_bytes = 1 * 1024 * 1024;

    Tool::File::FileReadScheduler scheduler{config};

    std::vector<Tool::File::ReadRequest> requests;
    requests.push_back(Tool::File::ReadRequest{
        .path = path,
        .offset = 0,
        .size_bytes = static_cast<std::uint64_t>(destination.size()),
        .destination = destination.data(),
        .destination_capacity = destination.size(),
        .urgency = Tool::File::ReadUrgency::frameCritical,
        .allow_split = true,
        .allow_mapped_copy = false,
        .allow_mapped_view = false,
        .same_frame_required = true,
        .startup_critical = false,
        .deadline_ticks = 0,
        .group_id = 1,
        .request_id = 100
    });

    auto status = scheduler.runRequests(requests);
    expectTrue(context_, static_cast<bool>(status), "FileReadScheduler direct 请求执行成功");
    expectTrue(context_, destination == original, "FileReadScheduler direct 结果正确");
}

void testSchedulerMappedCopy(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "scheduler_mapped.bin";
    const auto original = makeBytes(6 * 1024 * 1024, 0x89ABCDEFu);
    expectTrue(context_, writeBinary(path, original), "生成 scheduler_mapped.bin");

    std::vector<std::byte> destination(original.size());

    Tool::File::PlannerConfig config{};
    config.worker_limit = 4;
    config.storage.medium_file_threshold = 1 * 1024 * 1024;
    config.storage.large_file_threshold = 3 * 1024 * 1024;
    config.storage.split_chunk_bytes = 2 * 1024 * 1024;

    Tool::File::FileReadScheduler scheduler{config};

    std::vector<Tool::File::ReadRequest> requests;
    requests.push_back(Tool::File::ReadRequest{
        .path = path,
        .offset = 0,
        .size_bytes = static_cast<std::uint64_t>(destination.size()),
        .destination = destination.data(),
        .destination_capacity = destination.size(),
        .urgency = Tool::File::ReadUrgency::streaming,
        .allow_split = true,
        .allow_mapped_copy = true,
        .allow_mapped_view = false,
        .same_frame_required = false,
        .startup_critical = false,
        .deadline_ticks = 0,
        .group_id = 2,
        .request_id = 200
    });

    auto status = scheduler.runRequests(requests);
    expectTrue(context_, static_cast<bool>(status), "FileReadScheduler mapped_copy 请求执行成功");
    expectTrue(context_, destination == original, "FileReadScheduler mapped_copy 结果正确");
}

void testPlannerSplitAndLane(TestContext& context_) {
    Tool::File::PlannerConfig config{};
    config.storage.large_file_threshold = 4 * 1024 * 1024;
    config.storage.split_chunk_bytes = 2 * 1024 * 1024;
    config.storage.enable_range_merge = false;

    Tool::File::FileReadPlanner planner{config};

    std::vector<std::byte> target(8 * 1024 * 1024);

    std::vector<Tool::File::ReadRequest> requests;
    requests.push_back(Tool::File::ReadRequest{
        .path = "dummy.bin",
        .offset = 0,
        .size_bytes = static_cast<std::uint64_t>(target.size()),
        .destination = target.data(),
        .destination_capacity = target.size(),
        .urgency = Tool::File::ReadUrgency::frameCritical,
        .allow_split = true,
        .allow_mapped_copy = false,
        .allow_mapped_view = false,
        .same_frame_required = true,
        .startup_critical = false,
        .deadline_ticks = 0,
        .group_id = 3,
        .request_id = 300
    });

    auto plan = planner.makePlan(requests);
    expectTrue(context_, plan.urgent_tasks.size() == 4, "planner 应将 8MiB 请求按 2MiB 切成 4 个 urgent task");
}

void testPlannerRangeMerge(TestContext& context_) {
    Tool::File::PlannerConfig config{};
    config.storage.enable_range_merge = true;
    config.storage.merge_gap_bytes = 4 * 1024;
    config.storage.merge_max_bytes = 256 * 1024;
    config.storage.large_file_threshold = 1 * 1024 * 1024;
    config.storage.split_chunk_bytes = 2 * 1024 * 1024;

    Tool::File::FileReadPlanner planner{config};

    std::vector<std::byte> buffer_a(64 * 1024);
    std::vector<std::byte> buffer_b(64 * 1024);

    std::vector<Tool::File::ReadRequest> requests;
    requests.push_back(Tool::File::ReadRequest{
        .path = "merge.bin",
        .offset = 0,
        .size_bytes = static_cast<std::uint64_t>(buffer_a.size()),
        .destination = buffer_a.data(),
        .destination_capacity = buffer_a.size(),
        .urgency = Tool::File::ReadUrgency::streaming,
        .allow_split = false,
        .allow_mapped_copy = false,
        .allow_mapped_view = false,
        .same_frame_required = false,
        .startup_critical = false,
        .deadline_ticks = 0,
        .group_id = 4,
        .request_id = 401
    });
    requests.push_back(Tool::File::ReadRequest{
        .path = "merge.bin",
        .offset = 66 * 1024,
        .size_bytes = static_cast<std::uint64_t>(buffer_b.size()),
        .destination = buffer_b.data(),
        .destination_capacity = buffer_b.size(),
        .urgency = Tool::File::ReadUrgency::streaming,
        .allow_split = false,
        .allow_mapped_copy = false,
        .allow_mapped_view = false,
        .same_frame_required = false,
        .startup_critical = false,
        .deadline_ticks = 0,
        .group_id = 4,
        .request_id = 402
    });

    auto plan = planner.makePlan(requests);
    expectTrue(context_, plan.normal_tasks.size() == 1, "planner 应合并同路径邻近区间为单任务");
    if (!plan.normal_tasks.empty()) {
        expectTrue(context_, plan.normal_tasks.front().segments.size() == 2, "合并任务应包含 2 个 segment");
    }
}

void testPlannerDeadlinePreempt(TestContext& context_) {
    Tool::File::PlannerConfig config{};
    config.deadline_preempt_window_ticks = 1'000'000'000ull;
    Tool::File::FileReadPlanner planner{config};

    std::vector<std::byte> buffer(4096);
    const auto now_ticks = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    std::vector<Tool::File::ReadRequest> requests;
    requests.push_back(Tool::File::ReadRequest{
        .path = "deadline.bin",
        .offset = 0,
        .size_bytes = static_cast<std::uint64_t>(buffer.size()),
        .destination = buffer.data(),
        .destination_capacity = buffer.size(),
        .urgency = Tool::File::ReadUrgency::background,
        .allow_split = false,
        .allow_mapped_copy = false,
        .allow_mapped_view = false,
        .same_frame_required = false,
        .startup_critical = false,
        .deadline_ticks = now_ticks + 1000,
        .group_id = 5,
        .request_id = 451
    });

    auto plan = planner.makePlan(requests, now_ticks);
    expectTrue(context_, plan.urgent_tasks.size() == 1, "deadline 接近时应提升到 urgent lane");
}

void testSchedulerMappedView(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "scheduler_view.bin";
    const auto original = makeBytes(3 * 1024 * 1024, 0x55667788u);
    expectTrue(context_, writeBinary(path, original), "生成 scheduler_view.bin");

    Tool::File::PlannerConfig config{};
    config.worker_limit = 4;
    config.storage.large_file_threshold = 1 * 1024 * 1024;
    config.storage.split_chunk_bytes = 8 * 1024 * 1024;

    Tool::File::FileReadScheduler scheduler{config};

    std::vector<Tool::File::ReadRequest> requests;
    requests.push_back(Tool::File::ReadRequest{
        .path = path,
        .offset = 0,
        .size_bytes = static_cast<std::uint64_t>(original.size()),
        .destination = nullptr,
        .destination_capacity = 0,
        .urgency = Tool::File::ReadUrgency::streaming,
        .allow_split = false,
        .allow_mapped_copy = false,
        .allow_mapped_view = true,
        .same_frame_required = false,
        .startup_critical = false,
        .deadline_ticks = 0,
        .group_id = 6,
        .request_id = 501
    });

    std::atomic<int> callback_count{0};
    std::vector<Tool::File::ReadView> views;
    auto status = scheduler.runViewRequests(requests, views, [&](const Tool::File::ReadView&) {
        callback_count.fetch_add(1, std::memory_order_relaxed);
    });
    expectTrue(context_, static_cast<bool>(status), "runViewRequests 应成功");
    expectTrue(context_, views.size() == 1, "runViewRequests 应返回 1 个 view");
    expectTrue(context_, callback_count.load(std::memory_order_relaxed) == 1, "view callback 应触发 1 次");

    if (!views.empty()) {
        auto view_span = std::span<const std::byte>{views.front().data, static_cast<std::size_t>(views.front().size_bytes)};
        expectTrue(context_, std::equal(view_span.begin(), view_span.end(), original.begin(), original.end()), "mapped view 数据应与原文件一致");
    }
}

} // namespace

int main() {
    TestContext context{};

    const std::filesystem::path root = std::filesystem::path{"artifacts"} / "scheduler_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);

    testPlannerSplitAndLane(context);
    testPlannerRangeMerge(context);
    testPlannerDeadlinePreempt(context);
    testSchedulerDirect(context, root);
    testSchedulerMappedCopy(context, root);
    testSchedulerMappedView(context, root);

    std::cout << "\n=== scheduler 测试汇总 ===\n";
    std::cout << "passed=" << context.passed_count << ", failed=" << context.failed_count << '\n';

    return context.failed_count == 0 ? 0 : 1;
}

