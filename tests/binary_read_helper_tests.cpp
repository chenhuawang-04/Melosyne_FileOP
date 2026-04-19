#include "Center/File/BinaryReadHelper.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
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

struct Packet {
    std::uint32_t id;
    std::uint16_t channel;
    std::uint16_t flags;
    float gain;
};

[[nodiscard]] bool operator==(const Packet& lhs_, const Packet& rhs_) noexcept {
    return lhs_.id == rhs_.id
        && lhs_.channel == rhs_.channel
        && lhs_.flags == rhs_.flags
        && lhs_.gain == rhs_.gain;
}

static_assert(std::is_trivial_v<Packet>);
static_assert(std::is_standard_layout_v<Packet>);

[[nodiscard]] std::vector<Packet> makePackets(std::size_t count_) {
    std::vector<Packet> packets{};
    packets.resize(count_);

    for (std::size_t index = 0; index < count_; ++index) {
        packets[index].id = static_cast<std::uint32_t>(1000 + index);
        packets[index].channel = static_cast<std::uint16_t>(index % 16);
        packets[index].flags = static_cast<std::uint16_t>(index % 7);
        packets[index].gain = static_cast<float>(index) * 0.125f;
    }

    return packets;
}

[[nodiscard]] Tool::File::FileStatus writePackets(const std::filesystem::path& path_, std::span<const Packet> packets_) {
    auto open_result = Tool::File::PlatformFile::openWrite(path_, true);
    if (!open_result) {
        return Tool::File::makeUnexpected(open_result.error());
    }

    const auto bytes = std::as_bytes(packets_);
    return open_result.value().writeExactAt(0, bytes);
}

void testReadFileToSpanSuccess(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "binary_read_helper_success.bin";

    auto expected_packets = makePackets(2048);
    auto write_status = writePackets(path, std::span<const Packet>{expected_packets.data(), expected_packets.size()});
    expectTrue(context_, static_cast<bool>(write_status), "写入测试文件应成功");
    if (!write_status) {
        return;
    }

    std::vector<Packet> read_packets{};
    read_packets.resize(expected_packets.size());

    auto read_status = Tool::File::readFileToSpan(path, std::span<Packet>{read_packets.data(), read_packets.size()});
    expectTrue(context_, static_cast<bool>(read_status), "readFileToSpan 成功路径应返回成功");
    if (!read_status) {
        return;
    }

    expectTrue(context_, read_packets == expected_packets, "读取后的 POD 数组应与原数据一致");
}

void testReadFileToSpanSizeMismatch(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "binary_read_helper_mismatch.bin";

    auto source_packets = makePackets(128);
    auto write_status = writePackets(path, std::span<const Packet>{source_packets.data(), source_packets.size()});
    expectTrue(context_, static_cast<bool>(write_status), "写入 mismatch 测试文件应成功");
    if (!write_status) {
        return;
    }

    std::vector<Packet> destination_packets{};
    destination_packets.resize(source_packets.size() + 1);

    auto read_status = Tool::File::readFileToSpan(path, std::span<Packet>{destination_packets.data(), destination_packets.size()});
    expectTrue(context_, !read_status, "目标 span 与文件大小不一致时应失败");
    if (!read_status) {
        expectTrue(context_, read_status.error().operation == Tool::File::FileOperation::stat, "size mismatch 的 operation 应为 stat");
        expectTrue(context_, read_status.error().code == std::make_error_code(std::errc::result_out_of_range), "size mismatch 错误码应为 result_out_of_range");
    }
}

void testReadFileToSpanEmptySuccess(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "binary_read_helper_empty.bin";

    {
        std::array<std::byte, 0> empty_bytes{};
        auto open_result = Tool::File::PlatformFile::openWrite(path, true);
        expectTrue(context_, static_cast<bool>(open_result), "创建空文件应成功");
        if (!open_result) {
            return;
        }

        auto write_status = open_result.value().writeExactAt(0, std::span<const std::byte>{empty_bytes.data(), empty_bytes.size()});
        expectTrue(context_, static_cast<bool>(write_status), "写入空文件应成功");
        if (!write_status) {
            return;
        }
    }

    std::vector<Packet> destination_packets{};
    auto read_status = Tool::File::readFileToSpan(path, std::span<Packet>{destination_packets.data(), destination_packets.size()});
    expectTrue(context_, static_cast<bool>(read_status), "空文件读取到空 span 应成功");
}

} // namespace

int main() {
    std::cout.setf(std::ios::unitbuf);

    TestContext context{};

    const std::filesystem::path root = std::filesystem::path{"artifacts"} / "binary_read_helper_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    if (ec) {
        std::cerr << "创建测试目录失败: " << ec.message() << '\n';
        return 1;
    }

    testReadFileToSpanSuccess(context, root);
    testReadFileToSpanSizeMismatch(context, root);
    testReadFileToSpanEmptySuccess(context, root);

    std::cout << "\n=== binary read helper 测试汇总 ===\n";
    std::cout << "passed=" << context.passed_count << ", failed=" << context.failed_count << '\n';

    return context.failed_count == 0 ? 0 : 1;
}

