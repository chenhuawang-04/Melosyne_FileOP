#include "Center/File/FileOp.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

inline constexpr std::size_t kib = 1024;
inline constexpr std::size_t mib = 1024 * kib;

struct BenchResult {
    std::string name{};
    double average_seconds = 0.0;
    double throughput_mib_per_sec = 0.0;
    std::uint64_t checksum = 0;
};

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

[[nodiscard]] auto checksumSparse(std::span<const std::byte> bytes_) -> std::uint64_t {
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;

    std::uint64_t value = fnv_offset;
    const std::size_t stride = 4096;
    for (std::size_t index = 0; index < bytes_.size(); index += stride) {
        value ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes_[index]));
        value *= fnv_prime;
    }
    return value;
}

[[nodiscard]] auto ensureFiles(const std::filesystem::path& root_, std::size_t file_count_, std::size_t file_size_) -> bool {
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    if (ec) {
        std::cerr << "create directories failed: " << ec.message() << '\n';
        return false;
    }

    for (std::size_t file_index = 0; file_index < file_count_; ++file_index) {
        auto path = root_ / ("batch_" + std::to_string(file_index) + ".bin");
        if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) == file_size_) {
            continue;
        }

        auto bytes = makeBytes(file_size_, static_cast<std::uint32_t>(0x12340000u + file_index));
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "open for write failed: " << path.string() << '\n';
            return false;
        }
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            std::cerr << "write failed: " << path.string() << '\n';
            return false;
        }
    }

    return true;
}

[[nodiscard]] auto buildRequests(const std::filesystem::path& root_,
                                 std::size_t file_count_,
                                 std::size_t file_size_,
                                 std::vector<std::vector<std::byte>>& outputs_,
                                 bool mapped_copy_) -> std::vector<Tool::File::ReadRequest> {
    outputs_.assign(file_count_, std::vector<std::byte>(file_size_));

    std::vector<Tool::File::ReadRequest> requests;
    requests.reserve(file_count_);

    for (std::size_t file_index = 0; file_index < file_count_; ++file_index) {
        auto path = root_ / ("batch_" + std::to_string(file_index) + ".bin");

        requests.push_back(Tool::File::ReadRequest{
            .path = path,
            .offset = 0,
            .size_bytes = static_cast<std::uint64_t>(file_size_),
            .destination = outputs_[file_index].data(),
            .destination_capacity = outputs_[file_index].size(),
            .urgency = (file_index % 4 == 0) ? Tool::File::ReadUrgency::frameCritical : Tool::File::ReadUrgency::streaming,
            .allow_split = true,
            .allow_mapped_copy = mapped_copy_,
            .allow_mapped_view = false,
            .same_frame_required = (file_index % 4 == 0),
            .startup_critical = false,
            .group_id = 1,
            .request_id = static_cast<std::uint32_t>(file_index)
        });
    }

    return requests;
}

[[nodiscard]] auto runSequentialDirect(const std::vector<Tool::File::ReadRequest>& requests_) -> Tool::File::FileResult<std::uint64_t> {
    std::uint64_t checksum = 0;
    for (const auto& request : requests_) {
        auto open_result = Tool::File::PlatformFile::openRead(request.path);
        if (!open_result) {
            return std::unexpected(open_result.error());
        }

        auto bytes = Tool::File::MutableBytes{request.destination, static_cast<std::size_t>(request.size_bytes)};
        auto status = open_result->readExactAt(request.offset, bytes);
        if (!status) {
            return std::unexpected(status.error());
        }

        checksum ^= checksumSparse(bytes);
    }
    return checksum;
}

template<typename Runner>
[[nodiscard]] auto runBench(const std::string& name_, std::size_t total_bytes_, int iterations_, Runner&& runner_) -> BenchResult {
    BenchResult result{};
    result.name = name_;

    double total_seconds = 0.0;
    for (int iteration = 0; iteration < iterations_; ++iteration) {
        const auto begin_time = Clock::now();
        auto run_result = runner_();
        const auto end_time = Clock::now();

        if (!run_result) {
            std::cerr << "benchmark run failed: " << run_result.error().code.message() << '\n';
            std::exit(2);
        }

        result.checksum ^= *run_result + 0x9E3779B97F4A7C15ull + (result.checksum << 6) + (result.checksum >> 2);
        total_seconds += Seconds(end_time - begin_time).count();
    }

    result.average_seconds = total_seconds / static_cast<double>(iterations_);
    result.throughput_mib_per_sec = (static_cast<double>(total_bytes_) / static_cast<double>(mib)) / result.average_seconds;
    return result;
}

void printResult(const BenchResult& result_) {
    std::cout << std::left << std::setw(28) << result_.name
              << " avg=" << std::setw(10) << std::fixed << std::setprecision(6) << result_.average_seconds << "s"
              << " throughput=" << std::setw(12) << std::fixed << std::setprecision(2) << result_.throughput_mib_per_sec << " MiB/s"
              << " checksum=0x" << std::hex << result_.checksum << std::dec
              << '\n';
}

} // namespace

int main() {
    const std::filesystem::path root = std::filesystem::path{"artifacts"} / "scheduler_bench";

    constexpr std::size_t file_count = 24;
    constexpr std::size_t file_size = 4 * mib;
    constexpr int iterations = 5;

    if (!ensureFiles(root, file_count, file_size)) {
        return 1;
    }

    Tool::File::PlannerConfig planner_config{};
    planner_config.worker_limit = std::max(2u, std::thread::hardware_concurrency());
    planner_config.storage.large_file_threshold = 2 * mib;
    planner_config.storage.split_chunk_bytes = 2 * mib;
    planner_config.storage.medium_file_threshold = 2 * mib;

    Tool::File::FileReadScheduler scheduler{planner_config};

    std::vector<std::vector<std::byte>> outputs_direct;
    auto requests_direct = buildRequests(root, file_count, file_size, outputs_direct, false);

    std::vector<std::vector<std::byte>> outputs_mapped;
    auto requests_mapped = buildRequests(root, file_count, file_size, outputs_mapped, true);

    const std::size_t total_bytes = file_count * file_size;

    auto sequential_result = runBench("sequentialDirectRead", total_bytes, iterations, [&]() {
        return runSequentialDirect(requests_direct);
    });

    auto scheduler_direct_result = runBench("schedulerThreadCenterDirect", total_bytes, iterations, [&]() -> Tool::File::FileResult<std::uint64_t> {
        auto status = scheduler.runRequests(requests_direct);
        if (!status) {
            return std::unexpected(status.error());
        }

        std::uint64_t checksum = 0;
        for (const auto& buffer : outputs_direct) {
            checksum ^= checksumSparse(buffer);
        }
        return checksum;
    });

    auto scheduler_mapped_result = runBench("schedulerThreadCenterMapped", total_bytes, iterations, [&]() -> Tool::File::FileResult<std::uint64_t> {
        auto status = scheduler.runRequests(requests_mapped);
        if (!status) {
            return std::unexpected(status.error());
        }

        std::uint64_t checksum = 0;
        for (const auto& buffer : outputs_mapped) {
            checksum ^= checksumSparse(buffer);
        }
        return checksum;
    });

    std::cout << "=== FileReadScheduler 并发基准 ===\n";
    std::cout << "files=" << file_count << ", file_size=" << (file_size / mib) << " MiB"
              << ", total=" << (total_bytes / mib) << " MiB"
              << ", iterations=" << iterations << "\n\n";

    printResult(sequential_result);
    printResult(scheduler_direct_result);
    printResult(scheduler_mapped_result);

    return 0;
}

