module;

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>

export module Tool.File.StreamReadSession;

import Tool.File.PlatformFile;
import Tool.File.StreamTypes;
import Tool.File.Types;
import Tool.File.Error;

export namespace Tool::File {


class StreamReadSession {
public:
    StreamReadSession() = default;
    StreamReadSession(const StreamReadSession&) = delete;
    auto operator=(const StreamReadSession&) -> StreamReadSession& = delete;

    ~StreamReadSession() {
        stop();
    }

    [[nodiscard]] auto start(const std::filesystem::path& path_,
                             std::uint64_t start_offset_,
                             std::uint64_t max_bytes_,
                             const StreamReadConfig& config_) -> FileStatus {
        stop();

        if (config_.chunk_bytes == 0) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::invalid_argument)
            });
        }

        auto open_result = PlatformFile::openRead(path_, FileHint::sequential, FileShare::read);
        if (!open_result) {
            return makeUnexpected(open_result.error());
        }

        auto size_result = open_result->size();
        if (!size_result) {
            return makeUnexpected(size_result.error());
        }

        if (start_offset_ > *size_result) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::invalid_argument),
                .offset = start_offset_
            });
        }

        const auto remaining_from_offset = *size_result - start_offset_;
        total_readable_bytes = (max_bytes_ == 0)
            ? remaining_from_offset
            : (std::min<std::uint64_t>)(max_bytes_, remaining_from_offset);

        file_value = std::move(*open_result);
        config_value = config_;

        stream_offset = start_offset_;
        remaining_bytes = total_readable_bytes;

        produced_chunks.store(0, std::memory_order_release);
        consumed_chunks.store(0, std::memory_order_release);
        producer_block_count.store(0, std::memory_order_release);
        consumer_starve_count.store(0, std::memory_order_release);
        bytes_read.store(0, std::memory_order_release);

        paused.store(false, std::memory_order_release);
        end_of_stream.store(total_readable_bytes == 0, std::memory_order_release);

        return {};
    }

    auto stop() -> void {
        paused.store(false, std::memory_order_release);
        end_of_stream.store(true, std::memory_order_release);

        stream_offset = 0;
        remaining_bytes = 0;
        total_readable_bytes = 0;

        if (file_value.isOpen()) {
            static_cast<void>(file_value.close());
        }
    }

    auto pause() -> void {
        paused.store(true, std::memory_order_release);
    }

    auto resume() -> void {
        paused.store(false, std::memory_order_release);
    }

    [[nodiscard]] auto readNext(std::span<std::byte> destination_) -> FileResult<std::size_t> {
        if (destination_.empty()) {
            return std::size_t{0};
        }

        if (paused.load(std::memory_order_acquire)) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::operation_canceled)
            });
        }

        if (!file_value.isOpen() || end_of_stream.load(std::memory_order_acquire)) {
            return std::size_t{0};
        }

        const auto read_limit = static_cast<std::size_t>((std::min<std::uint64_t>)(
            static_cast<std::uint64_t>(destination_.size()),
            static_cast<std::uint64_t>(config_value.chunk_bytes)
        ));

        const auto request_bytes = static_cast<std::size_t>((std::min<std::uint64_t>)(
            remaining_bytes,
            static_cast<std::uint64_t>(read_limit)
        ));

        if (request_bytes == 0) {
            end_of_stream.store(true, std::memory_order_release);
            return std::size_t{0};
        }

        auto read_result = file_value.readAt(stream_offset, destination_.first(request_bytes));
        if (!read_result) {
            return makeUnexpected(read_result.error());
        }

        const auto read_size = *read_result;
        bytes_read.fetch_add(read_size, std::memory_order_relaxed);
        produced_chunks.fetch_add(1, std::memory_order_relaxed);
        consumed_chunks.fetch_add(1, std::memory_order_relaxed);

        stream_offset += static_cast<std::uint64_t>(read_size);
        remaining_bytes -= static_cast<std::uint64_t>(read_size);

        const bool read_short = read_size < request_bytes;
        if (read_short || remaining_bytes == 0) {
            end_of_stream.store(true, std::memory_order_release);
        }

        return read_size;
    }

    [[nodiscard]] auto tryReadNext(std::span<std::byte> destination_) -> FileResult<std::size_t> {
        return readNext(destination_);
    }

    [[nodiscard]] auto stats() const -> StreamStats {
        return StreamStats{
            .produced_chunks = produced_chunks.load(std::memory_order_acquire),
            .consumed_chunks = consumed_chunks.load(std::memory_order_acquire),
            .producer_block_count = producer_block_count.load(std::memory_order_acquire),
            .consumer_starve_count = consumer_starve_count.load(std::memory_order_acquire),
            .bytes_read = bytes_read.load(std::memory_order_acquire)
        };
    }

    [[nodiscard]] auto isEndOfStream() const -> bool {
        return end_of_stream.load(std::memory_order_acquire);
    }

private:
    StreamReadConfig config_value{};
    PlatformFile file_value{};

    std::uint64_t total_readable_bytes = 0;
    std::uint64_t stream_offset = 0;
    std::uint64_t remaining_bytes = 0;

    std::atomic<std::uint64_t> produced_chunks{0};
    std::atomic<std::uint64_t> consumed_chunks{0};
    std::atomic<std::uint64_t> producer_block_count{0};
    std::atomic<std::uint64_t> consumer_starve_count{0};
    std::atomic<std::uint64_t> bytes_read{0};

    std::atomic<bool> paused{false};
    std::atomic<bool> end_of_stream{true};
};

} // namespace Tool::File


