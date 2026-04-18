#include "Center/File/PlatformFile.hpp"

#include <algorithm>
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


void testOpenMissingFile(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "missing.bin";
    auto result = Tool::File::PlatformFile::openRead(path);
    expectTrue(context_, !result, "openRead 缺失文件应失败");
    if (!result) {
        expectTrue(context_, result.error().operation == Tool::File::FileOperation::open, "缺失文件错误操作类型应为 open");
    }
}

void testRoundTripReadWrite(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "roundtrip.bin";
    const auto original_bytes = makeBytes(64 * 1024, 0x13572468u);

    auto open_write_result = Tool::File::PlatformFile::openWrite(path, true);
    expectTrue(context_, static_cast<bool>(open_write_result), "openWrite 应成功创建 roundtrip 文件");
    if (!open_write_result) {
        return;
    }

    auto file = std::move(*open_write_result);
    auto write_status = file.writeExactAt(0, std::span<const std::byte>{original_bytes.data(), original_bytes.size()});
    expectTrue(context_, static_cast<bool>(write_status), "writeExactAt 应完整写入 roundtrip 文件");
    auto flush_status = file.flush();
    expectTrue(context_, static_cast<bool>(flush_status), "flush 应成功");
    auto close_status = file.close();
    expectTrue(context_, static_cast<bool>(close_status), "close 应成功");

    auto open_read_result = Tool::File::PlatformFile::openRead(path);
    expectTrue(context_, static_cast<bool>(open_read_result), "openRead 应成功打开 roundtrip 文件");
    if (!open_read_result) {
        return;
    }

    auto read_file = std::move(*open_read_result);
    auto size_result = read_file.size();
    expectTrue(context_, static_cast<bool>(size_result), "size 应成功返回文件大小");
    if (size_result) {
        expectTrue(context_, *size_result == original_bytes.size(), "size 应与写入大小一致");
    }

    std::vector<std::byte> read_bytes(original_bytes.size());
    auto read_status = read_file.readExactAt(0, std::span<std::byte>{read_bytes.data(), read_bytes.size()});
    expectTrue(context_, static_cast<bool>(read_status), "readExactAt 应完整读取 roundtrip 文件");
    expectTrue(context_, read_bytes == original_bytes, "读取内容应与写入内容完全一致");
}

void testReadExactPastEof(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "short_read.bin";
    const auto bytes = makeBytes(1024, 0x24681357u);
    expectTrue(context_, writeBinaryFile(path, std::span<const std::byte>{bytes.data(), bytes.size()}), "生成 short_read 测试文件应成功");

    auto open_result = Tool::File::PlatformFile::openRead(path);
    expectTrue(context_, static_cast<bool>(open_result), "openRead short_read 文件应成功");
    if (!open_result) {
        return;
    }

    std::vector<std::byte> destination(2048);
    auto read_status = open_result->readExactAt(0, std::span<std::byte>{destination.data(), destination.size()});
    expectTrue(context_, !read_status, "readExactAt 超过 EOF 时应失败");
    if (!read_status) {
        expectTrue(context_, read_status.error().end_of_file, "超过 EOF 时应标记 end_of_file");
        expectTrue(context_, read_status.error().processed == bytes.size(), "超过 EOF 时 processed 应等于实际可读字节数");
    }
}

void testResizeAndSize(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "resize.bin";
    auto open_result = Tool::File::PlatformFile::openReadWrite(path, Tool::File::FileCreation::createAlways);
    expectTrue(context_, static_cast<bool>(open_result), "openReadWrite createAlways 应成功");
    if (!open_result) {
        return;
    }

    auto file = std::move(*open_result);
    auto resize_large_status = file.resize(8192);
    expectTrue(context_, static_cast<bool>(resize_large_status), "resize 扩大文件应成功");

    auto size_after_grow = file.size();
    expectTrue(context_, static_cast<bool>(size_after_grow), "扩大后 size 应成功");
    if (size_after_grow) {
        expectTrue(context_, *size_after_grow == 8192, "扩大后文件大小应为 8192");
    }

    auto resize_small_status = file.resize(512);
    expectTrue(context_, static_cast<bool>(resize_small_status), "resize 缩小文件应成功");

    auto size_after_shrink = file.size();
    expectTrue(context_, static_cast<bool>(size_after_shrink), "缩小后 size 应成功");
    if (size_after_shrink) {
        expectTrue(context_, *size_after_shrink == 512, "缩小后文件大小应为 512");
    }
}

void testMoveSemantics(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "move_semantics.bin";
    const auto bytes = makeBytes(4096, 0xABCDEF01u);
    expectTrue(context_, writeBinaryFile(path, std::span<const std::byte>{bytes.data(), bytes.size()}), "生成 move_semantics 文件应成功");

    auto open_result = Tool::File::PlatformFile::openRead(path);
    expectTrue(context_, static_cast<bool>(open_result), "openRead move_semantics 文件应成功");
    if (!open_result) {
        return;
    }

    auto file_a = std::move(*open_result);
    expectTrue(context_, file_a.isOpen(), "move 前 file_a 应保持打开");

    Tool::File::PlatformFile file_b = std::move(file_a);
    expectTrue(context_, !file_a.isOpen(), "move construct 后 file_a 应为空");
    expectTrue(context_, file_b.isOpen(), "move construct 后 file_b 应打开");

    std::vector<std::byte> read_bytes(bytes.size());
    auto read_status = file_b.readExactAt(0, std::span<std::byte>{read_bytes.data(), read_bytes.size()});
    expectTrue(context_, static_cast<bool>(read_status), "move construct 后 file_b 应可读");
    expectTrue(context_, read_bytes == bytes, "move construct 后读取结果应正确");

    Tool::File::PlatformFile file_c;
    file_c = std::move(file_b);
    expectTrue(context_, !file_b.isOpen(), "move assign 后 file_b 应为空");
    expectTrue(context_, file_c.isOpen(), "move assign 后 file_c 应打开");
}

void testEmptyReadWrite(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "empty.bin";
    auto open_write_result = Tool::File::PlatformFile::openWrite(path, true);
    expectTrue(context_, static_cast<bool>(open_write_result), "openWrite empty 文件应成功");
    if (!open_write_result) {
        return;
    }

    std::array<std::byte, 1> dummy{};
    auto write_result = open_write_result->writeAt(0, std::span<const std::byte>{dummy.data(), 0});
    expectTrue(context_, static_cast<bool>(write_result), "空写入应成功");
    if (write_result) {
        expectTrue(context_, *write_result == 0, "空写入返回值应为 0");
    }

    auto close_status = open_write_result->close();
    expectTrue(context_, static_cast<bool>(close_status), "empty 文件 close 应成功");

    auto open_read_result = Tool::File::PlatformFile::openRead(path);
    expectTrue(context_, static_cast<bool>(open_read_result), "openRead empty 文件应成功");
    if (!open_read_result) {
        return;
    }

    auto read_result = open_read_result->readAt(0, std::span<std::byte>{dummy.data(), 0});
    expectTrue(context_, static_cast<bool>(read_result), "空读取应成功");
    if (read_result) {
        expectTrue(context_, *read_result == 0, "空读取返回值应为 0");
    }
}

void testClosedHandleErrors(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "closed_handle.bin";
    const auto bytes = makeBytes(128, 0x10203040u);
    expectTrue(context_, writeBinaryFile(path, std::span<const std::byte>{bytes.data(), bytes.size()}), "生成 closed_handle 文件应成功");

    auto open_result = Tool::File::PlatformFile::openRead(path);
    expectTrue(context_, static_cast<bool>(open_result), "openRead closed_handle 文件应成功");
    if (!open_result) {
        return;
    }

    auto file = std::move(*open_result);
    auto close_status = file.close();
    expectTrue(context_, static_cast<bool>(close_status), "显式 close 应成功");

    std::vector<std::byte> destination(bytes.size());
    auto read_status = file.readExactAt(0, std::span<std::byte>{destination.data(), destination.size()});
    expectTrue(context_, !read_status, "已关闭句柄的 readExactAt 应失败");
    if (!read_status) {
        expectTrue(context_, read_status.error().operation == Tool::File::FileOperation::read, "已关闭句柄 read 错误类型应为 read");
    }
}

} // namespace

int main() {
    TestContext context{};

    const std::filesystem::path root = std::filesystem::path{"artifacts"} / "safety_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    if (ec) {
        std::cerr << "创建测试目录失败: " << ec.message() << '\n';
        return 1;
    }

    testOpenMissingFile(context, root);
    testRoundTripReadWrite(context, root);
    testReadExactPastEof(context, root);
    testResizeAndSize(context, root);
    testMoveSemantics(context, root);
    testEmptyReadWrite(context, root);
    testClosedHandleErrors(context, root);

    std::cout << "\n=== 安全性测试汇总 ===\n";
    std::cout << "passed=" << context.passed_count << ", failed=" << context.failed_count << '\n';

    return context.failed_count == 0 ? 0 : 1;
}

