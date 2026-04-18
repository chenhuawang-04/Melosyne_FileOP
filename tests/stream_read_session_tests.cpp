#include "Center/File/StreamReadSession.hpp"

#include <array>
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

void testStreamReadAll(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "stream_payload.bin";
    auto payload = makeBytes(2 * 1024 * 1024 + 123, 0x1234ABCDu);
    expectTrue(context_, writeBinaryFile(path, std::span<const std::byte>{payload.data(), payload.size()}), "生成 stream_payload.bin 应成功");

    Center::File::StreamReadSession session{};

    Center::File::StreamReadConfig config{};
    config.chunk_bytes = 128 * 1024;
    config.prefetch_depth = 1;

    auto start_status = session.start(path, 0, 0, config);
    expectTrue(context_, static_cast<bool>(start_status), "session.start 应成功");
    if (!start_status) {
        return;
    }

    std::vector<std::byte> received{};
    received.reserve(payload.size());

    std::array<std::byte, 64 * 1024> temp{};
    std::size_t guard_iterations = 0;
    while (true) {
        auto read_result = session.readNext(std::span<std::byte>{temp.data(), temp.size()});
        expectTrue(context_, static_cast<bool>(read_result), "readNext 调用应成功");
        if (!read_result) {
            session.stop();
            return;
        }

        if (*read_result == 0) {
            break;
        }

        ++guard_iterations;
        if (guard_iterations > 200000) {
            expectTrue(context_, false, "readNext 迭代次数异常，疑似未收敛");
            session.stop();
            return;
        }

        received.insert(received.end(), temp.begin(), temp.begin() + static_cast<std::ptrdiff_t>(*read_result));
    }

    expectTrue(context_, session.isEndOfStream(), "读取结束后应为 EOS");
    expectTrue(context_, received == payload, "流式读取结果应与源文件一致");

    auto stats = session.stats();
    expectTrue(context_, stats.bytes_read == payload.size(), "stats.bytes_read 应等于文件大小");

    session.stop();
}

void testRangeRead(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "stream_range_payload.bin";
    auto payload = makeBytes(512 * 1024, 0x55AA11CCu);
    expectTrue(context_, writeBinaryFile(path, std::span<const std::byte>{payload.data(), payload.size()}), "生成 stream_range_payload.bin 应成功");

    constexpr std::uint64_t start_offset = 1024;
    constexpr std::uint64_t max_bytes = 70000;

    Center::File::StreamReadSession session{};
    Center::File::StreamReadConfig config{};
    config.chunk_bytes = 32 * 1024;

    auto start_status = session.start(path, start_offset, max_bytes, config);
    expectTrue(context_, static_cast<bool>(start_status), "range session.start 应成功");
    if (!start_status) {
        return;
    }

    std::vector<std::byte> received{};
    std::array<std::byte, 4096> temp{};

    while (true) {
        auto read_result = session.readNext(std::span<std::byte>{temp.data(), temp.size()});
        expectTrue(context_, static_cast<bool>(read_result), "range readNext 调用应成功");
        if (!read_result) {
            session.stop();
            return;
        }
        if (*read_result == 0) {
            break;
        }
        received.insert(received.end(), temp.begin(), temp.begin() + static_cast<std::ptrdiff_t>(*read_result));
    }

    std::vector<std::byte> expected(payload.begin() + static_cast<std::ptrdiff_t>(start_offset),
                                    payload.begin() + static_cast<std::ptrdiff_t>(start_offset + max_bytes));
    expectTrue(context_, received == expected, "range 读取结果应与期望区间一致");

    auto stats = session.stats();
    expectTrue(context_, stats.bytes_read == max_bytes, "range stats.bytes_read 应等于 max_bytes");

    session.stop();
}

void testPauseResume(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "stream_pause_payload.bin";
    auto payload = makeBytes(128 * 1024, 0xA1B2C3D4u);
    expectTrue(context_, writeBinaryFile(path, std::span<const std::byte>{payload.data(), payload.size()}), "生成 stream_pause_payload.bin 应成功");

    Center::File::StreamReadSession session{};
    Center::File::StreamReadConfig config{};
    config.chunk_bytes = 16 * 1024;

    auto start_status = session.start(path, 0, 0, config);
    expectTrue(context_, static_cast<bool>(start_status), "pause session.start 应成功");
    if (!start_status) {
        return;
    }

    session.pause();
    std::array<std::byte, 1024> temp{};
    auto paused_read = session.readNext(std::span<std::byte>{temp.data(), temp.size()});
    expectTrue(context_, !paused_read, "pause 后 readNext 应失败");
    if (!paused_read) {
        expectTrue(context_, paused_read.error().code == std::make_error_code(std::errc::operation_canceled), "pause 错误码应为 operation_canceled");
    }

    session.resume();
    auto resumed_read = session.readNext(std::span<std::byte>{temp.data(), temp.size()});
    expectTrue(context_, static_cast<bool>(resumed_read), "resume 后 readNext 应恢复成功");
    if (resumed_read) {
        expectTrue(context_, *resumed_read > 0, "resume 后应读取到正字节数");
    }

    session.stop();
}

} // namespace

int main() {
    std::cout.setf(std::ios::unitbuf);
    TestContext context{};

    const std::filesystem::path root = std::filesystem::path{"artifacts"} / "stream_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    if (ec) {
        std::cerr << "创建测试目录失败: " << ec.message() << '\n';
        return 1;
    }

    testStreamReadAll(context, root);
    testRangeRead(context, root);
    testPauseResume(context, root);

    std::cout << "\n=== stream read session 测试汇总 ===\n";
    std::cout << "passed=" << context.passed_count << ", failed=" << context.failed_count << '\n';

    return context.failed_count == 0 ? 0 : 1;
}
