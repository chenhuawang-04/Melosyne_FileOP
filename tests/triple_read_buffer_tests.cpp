#include "Center/File/TripleReadBuffer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
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

void testRoundTrip(TestContext& context_) {
    Tool::File::TripleReadBuffer buffer{};
    auto init_status = buffer.initialize(256, nullptr);
    expectTrue(context_, static_cast<bool>(init_status), "initialize 应成功");
    if (!init_status) {
        return;
    }

    auto acquire_result = buffer.acquireEmpty();
    expectTrue(context_, static_cast<bool>(acquire_result), "acquireEmpty 应成功");
    if (!acquire_result) {
        return;
    }

    auto write_span = buffer.mutableSpan(*acquire_result);
    const std::array<std::byte, 5> payload{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}, std::byte{0x55}
    };
    std::memcpy(write_span.data(), payload.data(), payload.size());

    auto release_status = buffer.releaseFilled(*acquire_result, payload.size(), 4096, false);
    expectTrue(context_, static_cast<bool>(release_status), "releaseFilled 应成功");

    auto current_result = buffer.getCurrent();
    expectTrue(context_, static_cast<bool>(current_result), "getCurrent 应成功");
    if (!current_result) {
        return;
    }

    const auto* view = *current_result;
    expectTrue(context_, view != nullptr, "view 不应为空");
    if (view == nullptr) {
        return;
    }

    expectTrue(context_, view->bytes.size() == payload.size(), "view 应返回 actual_size");
    expectTrue(context_, view->file_offset == 4096, "view 应返回正确 file_offset");

    std::array<std::byte, 5> copied{};
    std::memcpy(copied.data(), view->bytes.data(), copied.size());
    expectTrue(context_, copied == payload, "view 数据应与写入一致");

    auto next_result = buffer.switchToNext();
    expectTrue(context_, static_cast<bool>(next_result), "switchToNext 应成功");
    if (next_result) {
        expectTrue(context_, *next_result == nullptr, "无后续 filled buffer 时应返回空指针");
    }
}

void testStopWakeup(TestContext& context_) {
    Tool::File::TripleReadBuffer buffer{};
    auto init_status = buffer.initialize(128, nullptr);
    expectTrue(context_, static_cast<bool>(init_status), "stop 测试 initialize 应成功");
    if (!init_status) {
        return;
    }

    buffer.requestStopWakeup();
    auto acquire_result = buffer.acquireEmpty();
    expectTrue(context_, !acquire_result, "stop 后 acquireEmpty 应返回取消错误");
    if (!acquire_result) {
        expectTrue(context_, acquire_result.error().code == std::make_error_code(std::errc::operation_canceled), "stop 错误码应为 operation_canceled");
    }
}

void testErrorPropagation(TestContext& context_) {
    Tool::File::TripleReadBuffer buffer{};
    auto init_status = buffer.initialize(128, nullptr);
    expectTrue(context_, static_cast<bool>(init_status), "error 测试 initialize 应成功");
    if (!init_status) {
        return;
    }

    buffer.publishError(Tool::File::FileError{
        .operation = Tool::File::FileOperation::read,
        .code = std::make_error_code(std::errc::io_error)
    });

    auto current_result = buffer.getCurrent();
    expectTrue(context_, !current_result, "有错误时 getCurrent 应失败");
    if (!current_result) {
        expectTrue(context_, current_result.error().code == std::make_error_code(std::errc::io_error), "错误码应正确传递");
    }
}

} // namespace

int main() {
    TestContext context{};

    testRoundTrip(context);
    testStopWakeup(context);
    testErrorPropagation(context);

    std::cout << "\n=== triple read buffer 测试汇总 ===\n";
    std::cout << "passed=" << context.passed_count << ", failed=" << context.failed_count << '\n';

    return context.failed_count == 0 ? 0 : 1;
}

