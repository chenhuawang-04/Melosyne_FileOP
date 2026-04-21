#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

import Tool.File.FileOp;

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

[[nodiscard]] std::vector<std::byte> makeBytes(std::size_t size_, std::uint32_t seed_) {
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

[[nodiscard]] bool writeBinaryFile(const std::filesystem::path& path_, std::span<const std::byte> bytes_) {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    output.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
    return static_cast<bool>(output);
}

void testModuleMappedViewWindowsPath(TestContext& context_, const std::filesystem::path& root_) {
    using namespace Tool::File;

    const auto payload = makeBytes(128 * 1024, 0x2468ACE0u);
    const auto path = root_ / "module_mapped_view_payload.bin";

    expectTrue(context_, writeBinaryFile(path, std::span<const std::byte>{payload.data(), payload.size()}), "写入 module mappedView 测试文件应成功");

    PlannerConfig config{};
    config.worker_limit = 2;
    config.handle_pool_capacity = 8;
    config.storage.large_file_threshold = 1024;
    config.storage.medium_file_threshold = 512;
    config.storage.split_chunk_bytes = 4 * 1024 * 1024;

    FileReadScheduler scheduler{config};

    ReadRequest request{};
    request.path = path;
    request.offset = 0;
    request.size_bytes = payload.size();
    request.destination = nullptr;
    request.destination_capacity = 0;
    request.allow_split = false;
    request.allow_mapped_copy = false;
    request.allow_mapped_view = true;
    request.urgency = ReadUrgency::streaming;
    request.request_id = 9001;

    std::array<ReadRequest, 1> requests{request};
    DynamicArray<ReadView> out_views{};

    std::size_t callback_count = 0;
    auto status = scheduler.runViewRequests(requests, out_views, [&](const ReadView&) {
        ++callback_count;
    });

#if defined(_WIN32) || defined(_WIN64)
    expectTrue(context_, static_cast<bool>(status), "Windows + modules: mappedView 请求应成功");
    if (!status) {
        expectTrue(context_, status.error().code != std::make_error_code(std::errc::function_not_supported), "Windows + modules: 不应返回 function_not_supported");
        return;
    }

    expectTrue(context_, !out_views.empty(), "Windows + modules: mappedView 应返回至少一个 view");
    expectTrue(context_, callback_count == out_views.size(), "view callback 次数应等于返回 view 数");

    std::vector<std::byte> collected{};
    collected.reserve(payload.size());

    for (const auto& view : out_views) {
        expectTrue(context_, view.data != nullptr, "view.data 不应为空");
        const auto* begin_ptr = view.data;
        const auto* end_ptr = view.data + static_cast<std::ptrdiff_t>(view.size_bytes);
        collected.insert(collected.end(), begin_ptr, end_ptr);
    }

    expectTrue(context_, collected.size() == payload.size(), "合并后的 view 数据大小应匹配原文件");
    expectTrue(context_, collected == payload, "合并后的 view 数据应与原文件一致");
#else
    if (!status) {
        expectTrue(context_, status.error().code == std::make_error_code(std::errc::function_not_supported), "非 Windows 平台 mappedView 不支持时应返回 function_not_supported");
    }
#endif
}

} // namespace

int main() {
    TestContext context{};

    const std::filesystem::path root = std::filesystem::path{"artifacts"} / "module_mapped_view_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    if (ec) {
        std::cerr << "创建测试目录失败: " << ec.message() << '\n';
        return 1;
    }

    testModuleMappedViewWindowsPath(context, root);

    std::cout << "\n=== module mappedView 测试汇总 ===\n";
    std::cout << "passed=" << context.passed_count << ", failed=" << context.failed_count << '\n';

    return context.failed_count == 0 ? 0 : 1;
}
