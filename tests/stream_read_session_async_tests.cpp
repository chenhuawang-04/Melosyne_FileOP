#include "Center/File/StreamReadSessionAsync.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <thread>
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

void testAsyncReadAll(TestContext& context_, const std::filesystem::path& root_) {
    const auto path = root_ / "stream_async_payload.bin";
    auto payload = makeBytes(512 * 1024 + 7, 0x76543210u);
    expectTrue(context_, writeBinaryFile(path, std::span<const std::byte>{payload.data(), payload.size()}), "create payload file");

    Tool::File::StreamReadSessionAsync session{};
    Tool::File::StreamReadConfig config{};
    config.chunk_bytes = 64 * 1024;

    auto start_status = session.start(path, 0, 0, config);
    expectTrue(context_, static_cast<bool>(start_status), "async session start");
    if (!start_status) {
        return;
    }

    std::vector<std::byte> received{};
    received.reserve(payload.size());

    std::array<std::byte, 64 * 1024> temp{};
    std::size_t read_calls = 0;
    const auto test_start = std::chrono::steady_clock::now();

    while (true) {
        auto read_result = session.tryReadNext(std::span<std::byte>{temp.data(), temp.size()});
        if (!read_result) {
            expectTrue(context_, false, "async tryReadNext should succeed");
            session.stop();
            return;
        }

        if (*read_result == 0) {
            if (session.isEndOfStream()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        } else {
            received.insert(received.end(), temp.begin(), temp.begin() + static_cast<std::ptrdiff_t>(*read_result));
        }

        ++read_calls;
        if (read_calls > 200000) {
            auto debug_stats = session.stats();
            std::cout << "[DEBUG] calls=" << read_calls
                      << " bytes_read=" << debug_stats.bytes_read
                      << " produced=" << debug_stats.produced_chunks
                      << " consumed=" << debug_stats.consumed_chunks
                      << " received=" << received.size()
                      << " eos=" << (session.isEndOfStream() ? 1 : 0) << '\n';
            expectTrue(context_, false, "async read loop does not converge");
            session.stop();
            return;
        }

        const auto elapsed = std::chrono::steady_clock::now() - test_start;
        if (elapsed > std::chrono::seconds(10)) {
            auto debug_stats = session.stats();
            std::cout << "[DEBUG] timeout calls=" << read_calls
                      << " bytes_read=" << debug_stats.bytes_read
                      << " produced=" << debug_stats.produced_chunks
                      << " consumed=" << debug_stats.consumed_chunks
                      << " received=" << received.size()
                      << " eos=" << (session.isEndOfStream() ? 1 : 0) << '\n';
            expectTrue(context_, false, "async read timeout");
            session.stop();
            return;
        }
    }

    expectTrue(context_, session.isEndOfStream(), "async should reach eos");
    expectTrue(context_, received == payload, "async payload should match");

    auto stats = session.stats();
    expectTrue(context_, stats.bytes_read == payload.size(), "async stats.bytes_read should equal file size");

    session.stop();
}

} // namespace

int main() {
    std::cout.setf(std::ios::unitbuf);
    TestContext context{};

    const std::filesystem::path root = std::filesystem::path{"artifacts"} / "stream_async_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    if (ec) {
        std::cerr << "failed to create test directory: " << ec.message() << '\n';
        return 1;
    }

    testAsyncReadAll(context, root);

    std::cout << "\n=== stream read async summary ===\n";
    std::cout << "passed=" << context.passed_count << ", failed=" << context.failed_count << '\n';

    return context.failed_count == 0 ? 0 : 1;
}

