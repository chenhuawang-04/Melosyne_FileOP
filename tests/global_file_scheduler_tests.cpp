#include "Center/File/GlobalFileScheduler.hpp"
#include "Center/File/PlatformFile.hpp"

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

std::vector<std::byte> makeBytes(std::size_t size_, std::uint32_t seed_) {
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

bool writeBinaryFile(const std::filesystem::path& path_, std::span<const std::byte> bytes_) {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    output.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
    return static_cast<bool>(output);
}

void testInitializeAndShutdown(TestContext& context_) {
    using namespace Center::File;

    GlobalFileScheduler::shutdown();
    expectTrue(context_, !GlobalFileScheduler::isInitialized(), "shutdown 后应为未初始化状态");

    PlannerConfig config{};
    config.worker_limit = 2;
    config.handle_pool_capacity = 16;

    auto init_status = GlobalFileScheduler::initialize(config);
    expectTrue(context_, static_cast<bool>(init_status), "initialize 应成功");
    expectTrue(context_, GlobalFileScheduler::isInitialized(), "initialize 后应为已初始化状态");

    auto* scheduler_ptr = GlobalFileScheduler::tryGet();
    expectTrue(context_, scheduler_ptr != nullptr, "tryGet 在已初始化时应返回非空");

    auto& scheduler_ref = GlobalFileScheduler::get();
    auto* scheduler_ref_ptr = &scheduler_ref;
    expectTrue(context_, scheduler_ref_ptr == scheduler_ptr, "get 与 tryGet 应指向同一实例");

    GlobalFileScheduler::shutdown();
    expectTrue(context_, !GlobalFileScheduler::isInitialized(), "shutdown 后应恢复未初始化");
    expectTrue(context_, GlobalFileScheduler::tryGet() == nullptr, "shutdown 后 tryGet 应返回空");
}

void testRunRequestThroughGlobalScheduler(TestContext& context_, const std::filesystem::path& root_) {
    using namespace Center::File;

    const auto payload = makeBytes(64 * 1024, 0x89ABCDEFu);
    const auto path = root_ / "global_scheduler.bin";
    expectTrue(context_, writeBinaryFile(path, std::span<const std::byte>{payload.data(), payload.size()}), "写入 global_scheduler.bin 应成功");

    GlobalFileScheduler::shutdown();

    PlannerConfig config{};
    config.worker_limit = 4;
    config.storage.split_chunk_bytes = 2 * 1024 * 1024;
    config.handle_pool_capacity = 32;

    auto init_status = GlobalFileScheduler::initialize(config);
    expectTrue(context_, static_cast<bool>(init_status), "全局 scheduler 初始化应成功");

    std::vector<std::byte> destination(payload.size());
    ReadRequest request{};
    request.path = path;
    request.offset = 0;
    request.size_bytes = payload.size();
    request.destination = destination.data();
    request.destination_capacity = destination.size();
    request.urgency = ReadUrgency::streaming;
    request.allow_mapped_copy = true;

    auto status = GlobalFileScheduler::get().runRequests({request});
    expectTrue(context_, static_cast<bool>(status), "通过全局 scheduler 执行 runRequests 应成功");
    expectTrue(context_, destination == payload, "全局 scheduler 读取结果应正确");

    GlobalFileScheduler::shutdown();
}

} // namespace

int main() {
    TestContext context{};

    const std::filesystem::path root = std::filesystem::path{"artifacts"} / "global_scheduler_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    if (ec) {
        std::cerr << "创建测试目录失败: " << ec.message() << '\n';
        return 1;
    }

    testInitializeAndShutdown(context);
    testRunRequestThroughGlobalScheduler(context, root);

    std::cout << "\n=== global scheduler 测试汇总 ===\n";
    std::cout << "passed=" << context.passed_count << ", failed=" << context.failed_count << '\n';

    return context.failed_count == 0 ? 0 : 1;
}
