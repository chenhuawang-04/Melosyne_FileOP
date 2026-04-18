#include "Center/File/PlatformFile.hpp"
#include "Center/File/StreamReadSession.hpp"
#include "Center/File/StreamReadSessionAsync.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t mebibyte = 1024 * 1024;

std::vector<std::byte> makePayload(std::size_t size_bytes_, std::uint32_t seed_) {
    std::vector<std::byte> bytes(size_bytes_);
    std::uint32_t state = seed_;
    for (std::size_t index = 0; index < size_bytes_; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[index] = static_cast<std::byte>(state & 0xFFu);
    }
    return bytes;
}

bool writeBinaryFile(const std::filesystem::path& path_, std::span<const std::byte> bytes_) {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    output.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
    return static_cast<bool>(output);
}

std::uint64_t foldChecksum(std::uint64_t acc_, std::span<const std::byte> bytes_) {
    for (auto byte_value : bytes_) {
        acc_ ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(byte_value));
        acc_ *= 1099511628211ull;
    }
    return acc_;
}

struct BenchResult {
    std::string name{};
    double avg_seconds = 0.0;
    double throughput_mib = 0.0;
    std::uint64_t checksum = 0;
};

BenchResult benchDirectChunked(const std::filesystem::path& path_, std::size_t file_size_, std::size_t chunk_size_, int iterations_) {
    using clock = std::chrono::steady_clock;

    std::vector<std::byte> chunk(chunk_size_);
    std::uint64_t checksum = 0;
    double total_seconds = 0.0;

    for (int iter = 0; iter < iterations_; ++iter) {
        auto open_result = Tool::File::PlatformFile::openRead(path_, Tool::File::FileHint::sequential, Tool::File::FileShare::read);
        if (!open_result) {
            std::cerr << "benchDirectChunked open failed\n";
            break;
        }

        auto file = std::move(*open_result);
        std::uint64_t offset = 0;

        auto start = clock::now();
        while (offset < file_size_) {
            const auto read_size = static_cast<std::size_t>((std::min<std::uint64_t>)(
                static_cast<std::uint64_t>(chunk_size_),
                static_cast<std::uint64_t>(file_size_ - static_cast<std::size_t>(offset))
            ));

            auto read_result = file.readAt(offset, std::span<std::byte>{chunk.data(), read_size});
            if (!read_result) {
                std::cerr << "benchDirectChunked read failed\n";
                break;
            }

            if (*read_result == 0) {
                break;
            }

            checksum = foldChecksum(checksum, std::span<const std::byte>{chunk.data(), *read_result});
            offset += static_cast<std::uint64_t>(*read_result);
        }
        auto end = clock::now();

        total_seconds += std::chrono::duration<double>(end - start).count();
    }

    const double avg_seconds = total_seconds / static_cast<double>(iterations_);
    const double throughput = (static_cast<double>(file_size_) / static_cast<double>(mebibyte)) / avg_seconds;

    return BenchResult{
        .name = "directChunkedRead",
        .avg_seconds = avg_seconds,
        .throughput_mib = throughput,
        .checksum = checksum
    };
}

BenchResult benchStreamSession(const std::filesystem::path& path_, std::size_t file_size_, std::size_t chunk_size_, int iterations_) {
    using clock = std::chrono::steady_clock;

    std::vector<std::byte> chunk(chunk_size_);
    std::uint64_t checksum = 0;
    double total_seconds = 0.0;

    for (int iter = 0; iter < iterations_; ++iter) {
        Tool::File::StreamReadSession session{};
        Tool::File::StreamReadConfig config{};
        config.chunk_bytes = chunk_size_;

        auto start_status = session.start(path_, 0, 0, config);
        if (!start_status) {
            std::cerr << "benchStreamSession start failed\n";
            break;
        }

        auto start = clock::now();
        while (true) {
            auto read_result = session.readNext(std::span<std::byte>{chunk.data(), chunk.size()});
            if (!read_result) {
                std::cerr << "benchStreamSession read failed\n";
                break;
            }
            if (*read_result == 0) {
                break;
            }
            checksum = foldChecksum(checksum, std::span<const std::byte>{chunk.data(), *read_result});
        }
        auto end = clock::now();

        session.stop();
        total_seconds += std::chrono::duration<double>(end - start).count();
    }

    const double avg_seconds = total_seconds / static_cast<double>(iterations_);
    const double throughput = (static_cast<double>(file_size_) / static_cast<double>(mebibyte)) / avg_seconds;

    return BenchResult{
        .name = "streamSessionPull",
        .avg_seconds = avg_seconds,
        .throughput_mib = throughput,
        .checksum = checksum
    };
}

BenchResult benchStreamSessionAsync(const std::filesystem::path& path_, std::size_t file_size_, std::size_t chunk_size_, int iterations_) {
    using clock = std::chrono::steady_clock;

    std::vector<std::byte> chunk(chunk_size_);
    std::uint64_t checksum = 0;
    double total_seconds = 0.0;

    for (int iter = 0; iter < iterations_; ++iter) {
        Tool::File::StreamReadSessionAsync session{};
        Tool::File::StreamReadConfig config{};
        config.chunk_bytes = chunk_size_;

        auto start_status = session.start(path_, 0, 0, config);
        if (!start_status) {
            std::cerr << "benchStreamSessionAsync start failed\n";
            break;
        }

        auto start = clock::now();
        while (true) {
            auto read_result = session.tryReadNext(std::span<std::byte>{chunk.data(), chunk.size()});
            if (!read_result) {
                std::cerr << "benchStreamSessionAsync read failed\n";
                break;
            }
            if (*read_result == 0) {
                if (session.isEndOfStream()) {
                    break;
                }
                std::this_thread::yield();
                continue;
            }
            checksum = foldChecksum(checksum, std::span<const std::byte>{chunk.data(), *read_result});
        }
        auto end = clock::now();

        session.stop();
        total_seconds += std::chrono::duration<double>(end - start).count();
    }

    const double avg_seconds = total_seconds / static_cast<double>(iterations_);
    const double throughput = (static_cast<double>(file_size_) / static_cast<double>(mebibyte)) / avg_seconds;

    return BenchResult{
        .name = "streamSessionAsync",
        .avg_seconds = avg_seconds,
        .throughput_mib = throughput,
        .checksum = checksum
    };
}

void printResult(const BenchResult& result_) {
    std::cout << std::left << std::setw(24) << result_.name
              << " avg=" << std::fixed << std::setprecision(6) << result_.avg_seconds << " s"
              << " throughput=" << std::fixed << std::setprecision(2) << result_.throughput_mib << " MiB/s"
              << " checksum=0x" << std::hex << result_.checksum << std::dec << '\n';
}

} // namespace

int main() {
    const auto artifacts_root = std::filesystem::path{"artifacts"} / "stream_bench";
    std::error_code ec;
    std::filesystem::create_directories(artifacts_root, ec);
    if (ec) {
        std::cerr << "create artifacts failed: " << ec.message() << '\n';
        return 1;
    }

    const auto payload_path = artifacts_root / "stream_payload.bin";
    constexpr std::size_t file_size = 256 * mebibyte;
    constexpr std::size_t chunk_size = 256 * 1024;
    constexpr int iterations = 6;

    if (!std::filesystem::exists(payload_path)) {
        auto payload = makePayload(file_size, 0xDADA1133u);
        if (!writeBinaryFile(payload_path, std::span<const std::byte>{payload.data(), payload.size()})) {
            std::cerr << "write payload failed\n";
            return 1;
        }
    }

    std::cout << "=== stream read benchmark (pull/async/direct) ===\n";
    std::cout << "file=" << payload_path.string() << "\n";
    std::cout << "file_size=" << (file_size / mebibyte) << " MiB"
              << ", chunk=" << (chunk_size / 1024) << " KiB"
              << ", iterations=" << iterations << "\n\n";

    auto direct_result = benchDirectChunked(payload_path, file_size, chunk_size, iterations);
    auto stream_result = benchStreamSession(payload_path, file_size, chunk_size, iterations);
    auto async_result = benchStreamSessionAsync(payload_path, file_size, chunk_size, iterations);

    printResult(direct_result);
    printResult(stream_result);
    printResult(async_result);

    if (direct_result.checksum != stream_result.checksum || direct_result.checksum != async_result.checksum) {
        std::cout << "[WARN] checksum mismatch between benchmark paths\n";
    }

    return 0;
}

