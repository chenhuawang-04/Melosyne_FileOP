#pragma once

#include "Center/File/Error.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <thread>

namespace Tool::File {

enum class ReadUrgency : std::uint8_t {
    immediate,
    frameCritical,
    streaming,
    preload,
    background
};

enum class ReadLane : std::uint8_t {
    urgent,
    normal,
    background
};

enum class ReadMethod : std::uint8_t {
    directRead,
    mappedCopy,
    mappedView
};

struct ReadRequest {
    std::filesystem::path path{};
    std::uint64_t offset = 0;
    std::uint64_t size_bytes = 0;

    std::byte* destination = nullptr;
    std::size_t destination_capacity = 0;

    ReadUrgency urgency = ReadUrgency::background;

    bool allow_split = true;
    bool allow_mapped_copy = false;
    bool allow_mapped_view = false;

    bool same_frame_required = false;
    bool startup_critical = false;

    std::uint64_t deadline_ticks = 0;

    std::uint32_t group_id = 0;
    std::uint32_t request_id = 0;
};

template<typename request_container_type>
concept ReadRequestContainer = requires(const request_container_type& request_container_) {
    { request_container_.data() } -> std::convertible_to<const ReadRequest*>;
    { request_container_.size() } -> std::convertible_to<std::size_t>;
};

template<ReadRequestContainer request_container_type>
[[nodiscard]] inline
auto asReadRequestSpan(const request_container_type& request_container_) noexcept -> std::span<const ReadRequest> {
    return std::span<const ReadRequest>{
        request_container_.data(),
        static_cast<std::size_t>(request_container_.size())
    };
}

struct ReadSegment {
    std::uint64_t source_offset = 0;
    std::uint64_t size_bytes = 0;
    std::byte* destination = nullptr;

    std::uint32_t request_id = 0;
};

struct PlannedReadTask {
    std::filesystem::path path{};
    std::uint64_t offset = 0;
    std::uint64_t size_bytes = 0;

    std::byte* destination = nullptr;

    ReadUrgency urgency = ReadUrgency::background;
    ReadLane lane = ReadLane::background;
    ReadMethod method = ReadMethod::directRead;

    std::uint64_t deadline_ticks = 0;

    std::uint32_t group_id = 0;
    std::uint32_t request_id = 0;

    DynamicArray<ReadSegment> segments{};
};

struct StorageProfile {
    std::uint32_t preferred_parallel_reads = 4;
    std::uint32_t max_parallel_large_reads = 2;

    std::size_t small_file_threshold = 128 * 1024;
    std::size_t medium_file_threshold = 4 * 1024 * 1024;
    std::size_t large_file_threshold = 32 * 1024 * 1024;

    std::size_t split_chunk_bytes = 8 * 1024 * 1024;

    bool enable_range_merge = true;
    std::size_t merge_gap_bytes = 64 * 1024;
    std::size_t merge_max_bytes = 8 * 1024 * 1024;
};

struct PlannerConfig {
    StorageProfile storage{};
    std::uint32_t worker_limit = std::max(1u, std::thread::hardware_concurrency());

    std::uint32_t reserve_urgent_workers = 1;
    std::uint32_t reserve_background_workers = 1;

    std::size_t handle_pool_capacity = 64;

    std::uint64_t deadline_preempt_window_ticks = 0;
};

struct ReadPlan {
    PlannerConfig config{};

    DynamicArray<PlannedReadTask> urgent_tasks{};
    DynamicArray<PlannedReadTask> normal_tasks{};
    DynamicArray<PlannedReadTask> background_tasks{};

    [[nodiscard]] auto totalTaskCount() const noexcept -> std::size_t {
        return urgent_tasks.size() + normal_tasks.size() + background_tasks.size();
    }
};

[[nodiscard]] constexpr auto urgencyScore(ReadUrgency urgency_) noexcept -> std::uint32_t {
    switch (urgency_) {
        case ReadUrgency::immediate:
            return 500;
        case ReadUrgency::frameCritical:
            return 400;
        case ReadUrgency::streaming:
            return 300;
        case ReadUrgency::preload:
            return 200;
        case ReadUrgency::background:
            return 100;
    }
    return 0;
}

} // namespace Tool::File

