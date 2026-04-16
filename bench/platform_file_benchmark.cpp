#include "Center/File/PlatformFile.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

inline constexpr std::size_t kib = 1024;
inline constexpr std::size_t mib = 1024 * kib;
inline constexpr std::size_t test_file_size = 256 * mib;
inline constexpr std::size_t create_chunk_size = 4 * mib;
inline constexpr int benchmark_iterations = 6;

struct BenchmarkResult {
    std::string name;
    double total_seconds = 0.0;
    double average_seconds = 0.0;
    double throughput_mib_per_sec = 0.0;
    std::uint64_t checksum = 0;
};

class VirtualBuffer {
public:
    VirtualBuffer() noexcept = default;

    explicit VirtualBuffer(std::size_t size_bytes_) noexcept
        : size_bytes_(size_bytes_) {
        data_ = static_cast<std::byte*>(::VirtualAlloc(nullptr, size_bytes_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }

    VirtualBuffer(const VirtualBuffer&) = delete;
    VirtualBuffer& operator=(const VirtualBuffer&) = delete;

    VirtualBuffer(VirtualBuffer&& other_) noexcept
        : data_(other_.data_),
          size_bytes_(other_.size_bytes_) {
        other_.data_ = nullptr;
        other_.size_bytes_ = 0;
    }

    VirtualBuffer& operator=(VirtualBuffer&& other_) noexcept {
        if (this != &other_) {
            release();
            data_ = other_.data_;
            size_bytes_ = other_.size_bytes_;
            other_.data_ = nullptr;
            other_.size_bytes_ = 0;
        }
        return *this;
    }

    ~VirtualBuffer() noexcept {
        release();
    }

    [[nodiscard]] bool isValid() const noexcept {
        return data_ != nullptr;
    }

    [[nodiscard]] std::byte* data() noexcept {
        return data_;
    }

    [[nodiscard]] const std::byte* data() const noexcept {
        return data_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_bytes_;
    }

    [[nodiscard]] std::span<std::byte> span() noexcept {
        return std::span<std::byte>{data_, size_bytes_};
    }

private:
    void release() noexcept {
        if (data_ != nullptr) {
            ::VirtualFree(data_, 0, MEM_RELEASE);
            data_ = nullptr;
            size_bytes_ = 0;
        }
    }

    std::byte* data_ = nullptr;
    std::size_t size_bytes_ = 0;
};

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;

    explicit ScopedHandle(HANDLE handle_) noexcept
        : handle_(handle_) {
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other_) noexcept
        : handle_(other_.handle_) {
        other_.handle_ = invalidHandle();
    }

    ScopedHandle& operator=(ScopedHandle&& other_) noexcept {
        if (this != &other_) {
            close();
            handle_ = other_.handle_;
            other_.handle_ = invalidHandle();
        }
        return *this;
    }

    ~ScopedHandle() noexcept {
        close();
    }

    [[nodiscard]] bool isValid() const noexcept {
        return handle_ != invalidHandle() && handle_ != nullptr;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

private:
    static HANDLE invalidHandle() noexcept {
        return INVALID_HANDLE_VALUE;
    }

    void close() noexcept {
        if (isValid()) {
            ::CloseHandle(handle_);
            handle_ = invalidHandle();
        }
    }

    HANDLE handle_ = invalidHandle();
};

class ScopedMappedView {
public:
    ScopedMappedView() noexcept = default;

    explicit ScopedMappedView(void* view_) noexcept
        : view_(view_) {
    }

    ScopedMappedView(const ScopedMappedView&) = delete;
    ScopedMappedView& operator=(const ScopedMappedView&) = delete;

    ScopedMappedView(ScopedMappedView&& other_) noexcept
        : view_(other_.view_) {
        other_.view_ = nullptr;
    }

    ScopedMappedView& operator=(ScopedMappedView&& other_) noexcept {
        if (this != &other_) {
            close();
            view_ = other_.view_;
            other_.view_ = nullptr;
        }
        return *this;
    }

    ~ScopedMappedView() noexcept {
        close();
    }

    [[nodiscard]] bool isValid() const noexcept {
        return view_ != nullptr;
    }

    [[nodiscard]] const std::byte* data() const noexcept {
        return static_cast<const std::byte*>(view_);
    }

private:
    void close() noexcept {
        if (view_ != nullptr) {
            ::UnmapViewOfFile(view_);
            view_ = nullptr;
        }
    }

    void* view_ = nullptr;
};

[[nodiscard]] std::uint64_t sparseChecksum(std::span<const std::byte> bytes_) noexcept {
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;

    std::uint64_t value = fnv_offset;
    if (bytes_.empty()) {
        return value;
    }

    const std::size_t stride = 4096;
    for (std::size_t index = 0; index < bytes_.size(); index += stride) {
        value ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes_[index]));
        value *= fnv_prime;
    }

    value ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes_.back()));
    value *= fnv_prime;
    value ^= static_cast<std::uint64_t>(bytes_.size());
    value *= fnv_prime;
    return value;
}

[[nodiscard]] std::uint64_t fullTouchChecksum(std::span<const std::byte> bytes_) noexcept {
    std::uint64_t value = 0x9E3779B97F4A7C15ull;
    for (const auto byte_value : bytes_) {
        value ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(byte_value));
        value = (value << 7) | (value >> (64 - 7));
        value *= 0x100000001B3ull;
    }
    value ^= static_cast<std::uint64_t>(bytes_.size());
    return value;
}

[[nodiscard]] bool setReadPosition(HANDLE handle_, std::uint64_t offset_) noexcept {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset_);
    return ::SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) != FALSE;
}

[[nodiscard]] bool readRawFileExact(HANDLE handle_, std::span<std::byte> destination_) noexcept {
    std::size_t total_size = 0;
    while (total_size < destination_.size()) {
        const DWORD chunk_size = static_cast<DWORD>(std::min<std::size_t>(destination_.size() - total_size, 1u << 30));
        DWORD transferred_size = 0;
        if (!::ReadFile(handle_, destination_.data() + total_size, chunk_size, &transferred_size, nullptr)) {
            return false;
        }
        total_size += static_cast<std::size_t>(transferred_size);
        if (transferred_size == 0) {
            break;
        }
    }
    return total_size == destination_.size();
}

[[nodiscard]] ScopedHandle openReadHandle(const std::filesystem::path& path_) noexcept {
    return ScopedHandle{::CreateFileW(
        path_.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr)};
}

void fillPattern(std::span<std::byte> bytes_, std::uint32_t seed_) noexcept {
    std::uint32_t state = seed_;
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes_[index] = static_cast<std::byte>(state & 0xFFu);
    }
}

bool ensurePayloadFile(const std::filesystem::path& path_) {
    std::error_code ec;
    if (std::filesystem::exists(path_, ec) && std::filesystem::file_size(path_, ec) == test_file_size) {
        return true;
    }

    ScopedHandle handle{::CreateFileW(
        path_.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr)};
    if (!handle.isValid()) {
        std::cerr << "创建测试文件失败, error=" << ::GetLastError() << '\n';
        return false;
    }

    VirtualBuffer chunk_buffer{create_chunk_size};
    if (!chunk_buffer.isValid()) {
        std::cerr << "分配生成缓冲区失败\n";
        return false;
    }

    std::size_t remaining_size = test_file_size;
    std::uint32_t seed = 0x12345678u;
    while (remaining_size > 0) {
        const std::size_t current_chunk_size = std::min<std::size_t>(remaining_size, create_chunk_size);
        auto current_chunk = std::span<std::byte>{chunk_buffer.data(), current_chunk_size};
        fillPattern(current_chunk, seed);
        seed += 0x9E3779B9u;

        DWORD written_size = 0;
        if (!::WriteFile(handle.get(), current_chunk.data(), static_cast<DWORD>(current_chunk_size), &written_size, nullptr)) {
            std::cerr << "写入测试文件失败, error=" << ::GetLastError() << '\n';
            return false;
        }
        if (written_size != current_chunk_size) {
            std::cerr << "写入测试文件出现短写\n";
            return false;
        }
        remaining_size -= current_chunk_size;
    }

    return true;
}

template<typename Function>
BenchmarkResult runBenchmark(
    std::string name_,
    std::size_t bytes_per_iteration_,
    int iteration_count_,
    Function&& function_
) {
    BenchmarkResult result{};
    result.name = std::move(name_);

    for (int iteration = 0; iteration < iteration_count_; ++iteration) {
        const auto begin_time = Clock::now();
        const std::uint64_t checksum = function_();
        const auto end_time = Clock::now();

        result.total_seconds += Seconds(end_time - begin_time).count();
        result.checksum ^= checksum + 0x9E3779B97F4A7C15ull + (result.checksum << 6) + (result.checksum >> 2);
    }

    result.average_seconds = result.total_seconds / static_cast<double>(iteration_count_);
    result.throughput_mib_per_sec = (static_cast<double>(bytes_per_iteration_) / static_cast<double>(mib)) / result.average_seconds;
    return result;
}

void printResult(const BenchmarkResult& result_, double ideal_throughput_) {
    std::cout << std::left << std::setw(28) << result_.name
              << " avg=" << std::setw(10) << std::fixed << std::setprecision(6) << result_.average_seconds << "s"
              << " throughput=" << std::setw(12) << std::fixed << std::setprecision(2) << result_.throughput_mib_per_sec << " MiB/s"
              << " ideal_ratio=" << std::setw(8) << std::fixed << std::setprecision(3)
              << (ideal_throughput_ > 0.0 ? result_.throughput_mib_per_sec / ideal_throughput_ : 0.0)
              << " checksum=0x" << std::hex << result_.checksum << std::dec
              << '\n';
}

} // namespace

int main() {
    const std::filesystem::path artifact_dir = std::filesystem::path{"artifacts"};
    const std::filesystem::path payload_path = artifact_dir / "platform_file_bench_payload.bin";

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    if (ec) {
        std::cerr << "创建 artifacts 目录失败: " << ec.message() << '\n';
        return 1;
    }

    if (!ensurePayloadFile(payload_path)) {
        return 1;
    }

    VirtualBuffer destination_buffer{test_file_size};
    VirtualBuffer source_buffer{test_file_size};
    if (!destination_buffer.isValid() || !source_buffer.isValid()) {
        std::cerr << "预分配缓冲区失败\n";
        return 1;
    }

    fillPattern(source_buffer.span(), 0xCAFEBABEu);

    {
        auto warm_handle = openReadHandle(payload_path);
        if (!warm_handle.isValid()) {
            std::cerr << "预热打开文件失败, error=" << ::GetLastError() << '\n';
            return 1;
        }
        if (!setReadPosition(warm_handle.get(), 0) || !readRawFileExact(warm_handle.get(), destination_buffer.span())) {
            std::cerr << "预热读取失败\n";
            return 1;
        }
    }

    auto raw_handle = openReadHandle(payload_path);
    if (!raw_handle.isValid()) {
        std::cerr << "raw Win32 打开失败, error=" << ::GetLastError() << '\n';
        return 1;
    }

    auto platform_file_result = Center::File::PlatformFile::openRead(payload_path, Center::File::FileHint::sequential);
    if (!platform_file_result) {
        std::cerr << "PlatformFile 打开失败, error=" << platform_file_result.error().code.message() << '\n';
        return 1;
    }
    auto platform_file = std::move(*platform_file_result);

    std::ifstream ifstream_file{payload_path, std::ios::binary};
    if (!ifstream_file) {
        std::cerr << "ifstream 打开失败\n";
        return 1;
    }

    ScopedHandle mapping_file = openReadHandle(payload_path);
    if (!mapping_file.isValid()) {
        std::cerr << "mmap 文件打开失败, error=" << ::GetLastError() << '\n';
        return 1;
    }

    ScopedHandle mapping_handle{::CreateFileMappingW(mapping_file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr)};
    if (!mapping_handle.isValid()) {
        std::cerr << "CreateFileMappingW 失败, error=" << ::GetLastError() << '\n';
        return 1;
    }

    ScopedMappedView mapped_view{::MapViewOfFile(mapping_handle.get(), FILE_MAP_READ, 0, 0, test_file_size)};
    if (!mapped_view.isValid()) {
        std::cerr << "MapViewOfFile 失败, error=" << ::GetLastError() << '\n';
        return 1;
    }

    std::vector<BenchmarkResult> results;
    results.reserve(6);

    results.push_back(runBenchmark(
        "idealMemcpyUpperBound",
        test_file_size,
        benchmark_iterations,
        [&]() -> std::uint64_t {
            std::memcpy(destination_buffer.data(), source_buffer.data(), test_file_size);
            return sparseChecksum(std::span<const std::byte>{destination_buffer.data(), destination_buffer.size()});
        }));

    results.push_back(runBenchmark(
        "rawWin32ReadPrealloc",
        test_file_size,
        benchmark_iterations,
        [&]() -> std::uint64_t {
            if (!setReadPosition(raw_handle.get(), 0) || !readRawFileExact(raw_handle.get(), destination_buffer.span())) {
                std::cerr << "raw Win32 读取失败\n";
                std::exit(2);
            }
            return sparseChecksum(std::span<const std::byte>{destination_buffer.data(), destination_buffer.size()});
        }));

    results.push_back(runBenchmark(
        "platformFileReadPrealloc",
        test_file_size,
        benchmark_iterations,
        [&]() -> std::uint64_t {
            auto read_status = platform_file.readExactAt(0, destination_buffer.span());
            if (!read_status) {
                std::cerr << "PlatformFile 读取失败: " << read_status.error().code.message() << '\n';
                std::exit(3);
            }
            return sparseChecksum(std::span<const std::byte>{destination_buffer.data(), destination_buffer.size()});
        }));

    results.push_back(runBenchmark(
        "ifstreamReadPrealloc",
        test_file_size,
        benchmark_iterations,
        [&]() -> std::uint64_t {
            ifstream_file.clear();
            ifstream_file.seekg(0, std::ios::beg);
            ifstream_file.read(reinterpret_cast<char*>(destination_buffer.data()), static_cast<std::streamsize>(destination_buffer.size()));
            if (ifstream_file.gcount() != static_cast<std::streamsize>(destination_buffer.size())) {
                std::cerr << "ifstream 读取失败\n";
                std::exit(4);
            }
            return sparseChecksum(std::span<const std::byte>{destination_buffer.data(), destination_buffer.size()});
        }));

    results.push_back(runBenchmark(
        "mappedCopyReuseView",
        test_file_size,
        benchmark_iterations,
        [&]() -> std::uint64_t {
            std::memcpy(destination_buffer.data(), mapped_view.data(), test_file_size);
            return sparseChecksum(std::span<const std::byte>{destination_buffer.data(), destination_buffer.size()});
        }));

    results.push_back(runBenchmark(
        "mappedZeroCopyFullScan",
        test_file_size,
        benchmark_iterations,
        [&]() -> std::uint64_t {
            return fullTouchChecksum(std::span<const std::byte>{mapped_view.data(), test_file_size});
        }));

    const double ideal_throughput = results.front().throughput_mib_per_sec;

    std::cout << "=== 底层文件后端性能基准（warm cache）===\n";
    std::cout << "payload=" << payload_path.string() << '\n';
    std::cout << "file_size=" << (test_file_size / mib) << " MiB"
              << ", iterations=" << benchmark_iterations
              << ", note=idealMemcpyUpperBound 为理论内存带宽上限；mappedZeroCopyFullScan 为直接访问映射视图并完整触碰全部字节的零拷贝方案。\n\n";

    for (const auto& result : results) {
        printResult(result, ideal_throughput);
    }

    return 0;
}

