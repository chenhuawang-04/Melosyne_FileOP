module;

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <semaphore>
#include <span>
#include <system_error>

export module Center.File.TripleReadBuffer;

import Center.File.Error;
import Center.File.StreamTypes;

export namespace Center::File {


class TripleReadBuffer {
public:
    static constexpr std::uint32_t buffer_count = 3;
    static constexpr std::size_t cache_line_bytes = 64;

    TripleReadBuffer() = default;
    TripleReadBuffer(const TripleReadBuffer&) = delete;
    auto operator=(const TripleReadBuffer&) -> TripleReadBuffer& = delete;

    ~TripleReadBuffer() {
        releaseStorage();
    }

    [[nodiscard]] auto initialize(std::size_t chunk_bytes_, std::pmr::memory_resource* memory_resource_) -> FileStatus {
        if (chunk_bytes_ == 0) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::invalid_argument)
            });
        }

        if (memory_resource_ == nullptr) {
            memory_resource_ = std::pmr::get_default_resource();
        }

        releaseStorage();

        const std::size_t aligned_chunk_bytes = alignUp(chunk_bytes_, cache_line_bytes);
        const std::size_t total_bytes = aligned_chunk_bytes * buffer_count;

        memory_resource_ptr = memory_resource_;
        storage_bytes = total_bytes;
        chunk_bytes_value = aligned_chunk_bytes;

        storage_ptr = static_cast<std::byte*>(memory_resource_ptr->allocate(storage_bytes, cache_line_bytes));
        if (storage_ptr == nullptr) {
            memory_resource_ptr = nullptr;
            storage_bytes = 0;
            chunk_bytes_value = 0;
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::not_enough_memory)
            });
        }

        for (std::uint32_t index = 0; index < buffer_count; ++index) {
            views[index].bytes = std::span<const std::byte>{storage_ptr + (chunk_bytes_value * index), 0};
            views[index].index = index;
            views[index].file_offset = 0;
            views[index].end_of_stream = false;
        }

        stop_requested.store(false, std::memory_order_release);
        error_ready.store(false, std::memory_order_release);
        consumer_current.store(-1, std::memory_order_release);

        empty_count.store(static_cast<int>(buffer_count), std::memory_order_release);
        filled_count.store(0, std::memory_order_release);

        resetQueues();
        normalizeSemaphores();

        return {};
    }

    auto reset() -> void {
        stop_requested.store(false, std::memory_order_release);
        error_ready.store(false, std::memory_order_release);
        consumer_current.store(-1, std::memory_order_release);

        for (auto& view : views) {
            view.bytes = std::span<const std::byte>{view.bytes.data(), 0};
            view.file_offset = 0;
            view.end_of_stream = false;
        }

        empty_count.store(static_cast<int>(buffer_count), std::memory_order_release);
        filled_count.store(0, std::memory_order_release);

        resetQueues();
        normalizeSemaphores();
    }

    [[nodiscard]] auto acquireEmpty() -> FileResult<std::uint32_t> {
        if (isStopRequested()) {
            return makeUnexpected(makeCanceledError());
        }

        while (true) {
            if (isStopRequested()) {
                return makeUnexpected(makeCanceledError());
            }

            if (empty_semaphore.try_acquire()) {
                empty_count.fetch_sub(1, std::memory_order_acq_rel);
                break;
            }

            empty_semaphore.acquire();
            empty_count.fetch_sub(1, std::memory_order_acq_rel);

            if (isStopRequested()) {
                return makeUnexpected(makeCanceledError());
            }
        }

        auto index_result = popEmptyIndex();
        if (!index_result) {
            static_cast<void>(safeRelease(empty_semaphore, empty_count, static_cast<int>(buffer_count)));
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::resource_unavailable_try_again)
            });
        }

        return *index_result;
    }

    [[nodiscard]] auto releaseFilled(std::uint32_t index_,
                                     std::size_t actual_size_bytes_,
                                     std::uint64_t file_offset_,
                                     bool end_of_stream_) -> FileStatus {
        if (index_ >= buffer_count || actual_size_bytes_ > chunk_bytes_value) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::invalid_argument)
            });
        }

        auto* data_ptr = storage_ptr + (chunk_bytes_value * index_);
        views[index_].bytes = std::span<const std::byte>{data_ptr, actual_size_bytes_};
        views[index_].file_offset = file_offset_;
        views[index_].end_of_stream = end_of_stream_;

        auto push_status = pushFilledIndex(index_);
        if (!push_status) {
            return makeUnexpected(push_status.error());
        }

        if (!safeRelease(filled_semaphore, filled_count, static_cast<int>(buffer_count))) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::value_too_large)
            });
        }

        return {};
    }

    [[nodiscard]] auto getCurrent() -> FileResult<const BufferView*> {
        return acquireCurrent(false);
    }

    [[nodiscard]] auto waitCurrent() -> FileResult<const BufferView*> {
        return acquireCurrent(true);
    }

    [[nodiscard]] auto switchToNext() -> FileResult<const BufferView*> {
        if (error_ready.load(std::memory_order_acquire)) {
            return makeUnexpected(loadError());
        }

        const int current_index = consumer_current.exchange(-1, std::memory_order_acq_rel);
        if (current_index >= 0 && current_index < static_cast<int>(buffer_count)) {
            auto push_status = pushEmptyIndex(static_cast<std::uint32_t>(current_index));
            if (!push_status) {
                return makeUnexpected(push_status.error());
            }
            static_cast<void>(safeRelease(empty_semaphore, empty_count, static_cast<int>(buffer_count)));
        }

        return getCurrent();
    }

    [[nodiscard]] auto mutableSpan(std::uint32_t index_) -> std::span<std::byte> {
        if (storage_ptr == nullptr || index_ >= buffer_count) {
            return {};
        }
        return std::span<std::byte>{storage_ptr + (chunk_bytes_value * index_), chunk_bytes_value};
    }

    auto requestStopWakeup() -> void {
        stop_requested.store(true, std::memory_order_release);

        // 鍏抽敭锛氬彧灏濊瘯 +1锛岀粷涓嶇洸鐩?+N锛岄伩鍏?counting_semaphore 涓婇檺婧㈠嚭 UB銆?
        static_cast<void>(safeRelease(empty_semaphore, empty_count, static_cast<int>(buffer_count)));
        static_cast<void>(safeRelease(filled_semaphore, filled_count, static_cast<int>(buffer_count)));
    }

    auto publishError(FileError error_value_) -> void {
        {
            std::scoped_lock guard(error_mutex);
            error_value = error_value_;
        }
        error_ready.store(true, std::memory_order_release);
        requestStopWakeup();
    }

    [[nodiscard]] auto chunkBytes() const noexcept -> std::size_t {
        return chunk_bytes_value;
    }

    [[nodiscard]] auto isStopRequested() const noexcept -> bool {
        return stop_requested.load(std::memory_order_acquire);
    }

private:
    [[nodiscard]] static auto alignUp(std::size_t value_, std::size_t align_) -> std::size_t {
        const std::size_t mask = align_ - 1;
        return (value_ + mask) & ~mask;
    }

    [[nodiscard]] static auto makeCanceledError() -> FileError {
        return FileError{
            .operation = FileOperation::read,
            .code = std::make_error_code(std::errc::operation_canceled)
        };
    }

    [[nodiscard]] auto loadError() const -> FileError {
        std::scoped_lock guard(error_mutex);
        return error_value;
    }

    auto resetQueues() -> void {
        for (std::uint32_t index = 0; index < buffer_count; ++index) {
            empty_ring[index] = index;
            filled_ring[index] = 0;
        }

        empty_read_pos.store(0, std::memory_order_release);
        empty_write_pos.store(buffer_count, std::memory_order_release);

        filled_read_pos.store(0, std::memory_order_release);
        filled_write_pos.store(0, std::memory_order_release);
    }

    [[nodiscard]] auto popEmptyIndex() -> FileResult<std::uint32_t> {
        const auto read_pos = empty_read_pos.load(std::memory_order_acquire);
        const auto write_pos = empty_write_pos.load(std::memory_order_acquire);
        if (read_pos >= write_pos) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::resource_unavailable_try_again)
            });
        }

        const auto index = empty_ring[read_pos % buffer_count];
        empty_read_pos.store(read_pos + 1, std::memory_order_release);
        return index;
    }

    [[nodiscard]] auto pushEmptyIndex(std::uint32_t index_) -> FileStatus {
        const auto write_pos = empty_write_pos.load(std::memory_order_relaxed);
        const auto read_pos = empty_read_pos.load(std::memory_order_acquire);
        if ((write_pos - read_pos) >= buffer_count) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::value_too_large)
            });
        }

        empty_ring[write_pos % buffer_count] = index_;
        empty_write_pos.store(write_pos + 1, std::memory_order_release);
        return {};
    }

    [[nodiscard]] auto popFilledIndex() -> FileResult<std::uint32_t> {
        const auto read_pos = filled_read_pos.load(std::memory_order_acquire);
        const auto write_pos = filled_write_pos.load(std::memory_order_acquire);
        if (read_pos >= write_pos) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::resource_unavailable_try_again)
            });
        }

        const auto index = filled_ring[read_pos % buffer_count];
        filled_read_pos.store(read_pos + 1, std::memory_order_release);
        return index;
    }

    [[nodiscard]] auto pushFilledIndex(std::uint32_t index_) -> FileStatus {
        const auto write_pos = filled_write_pos.load(std::memory_order_relaxed);
        const auto read_pos = filled_read_pos.load(std::memory_order_acquire);
        if ((write_pos - read_pos) >= buffer_count) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::value_too_large)
            });
        }

        filled_ring[write_pos % buffer_count] = index_;
        filled_write_pos.store(write_pos + 1, std::memory_order_release);
        return {};
    }

    [[nodiscard]] auto acquireCurrent(bool block_) -> FileResult<const BufferView*> {
        if (error_ready.load(std::memory_order_acquire)) {
            return makeUnexpected(loadError());
        }

        const int existing_index = consumer_current.load(std::memory_order_acquire);
        if (existing_index >= 0 && existing_index < static_cast<int>(buffer_count)) {
            return &views[static_cast<std::size_t>(existing_index)];
        }

        while (true) {
            if (error_ready.load(std::memory_order_acquire)) {
                return makeUnexpected(loadError());
            }

            bool acquired = false;
            if (block_) {
                filled_semaphore.acquire();
                acquired = true;
            } else {
                acquired = filled_semaphore.try_acquire();
            }

            if (!acquired) {
                return static_cast<const BufferView*>(nullptr);
            }

            filled_count.fetch_sub(1, std::memory_order_acq_rel);

            if (error_ready.load(std::memory_order_acquire)) {
                return makeUnexpected(loadError());
            }

            auto index_result = popFilledIndex();
            if (index_result) {
                consumer_current.store(static_cast<int>(*index_result), std::memory_order_release);
                return &views[*index_result];
            }

            if (isStopRequested()) {
                return makeUnexpected(makeCanceledError());
            }

            if (!block_) {
                return static_cast<const BufferView*>(nullptr);
            }
        }
    }

    [[nodiscard]] static auto safeRelease(std::counting_semaphore<buffer_count>& semaphore_,
                                          std::atomic<int>& count_,
                                          int max_count_) -> bool {
        int current_count = count_.load(std::memory_order_acquire);
        while (current_count < max_count_) {
            if (count_.compare_exchange_weak(current_count,
                                             current_count + 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                semaphore_.release();
                return true;
            }
        }
        return false;
    }

    auto normalizeSemaphores() -> void {
        while (filled_semaphore.try_acquire()) {
        }

        while (empty_semaphore.try_acquire()) {
        }

        for (std::uint32_t index = 0; index < buffer_count; ++index) {
            empty_semaphore.release();
        }

        empty_count.store(static_cast<int>(buffer_count), std::memory_order_release);
        filled_count.store(0, std::memory_order_release);
    }

    auto releaseStorage() -> void {
        if (storage_ptr != nullptr && memory_resource_ptr != nullptr && storage_bytes > 0) {
            memory_resource_ptr->deallocate(storage_ptr, storage_bytes, cache_line_bytes);
        }

        storage_ptr = nullptr;
        memory_resource_ptr = nullptr;
        storage_bytes = 0;
        chunk_bytes_value = 0;
    }

    std::pmr::memory_resource* memory_resource_ptr = nullptr;
    std::byte* storage_ptr = nullptr;
    std::size_t storage_bytes = 0;
    std::size_t chunk_bytes_value = 0;

    std::array<BufferView, buffer_count> views{};

    std::array<std::uint32_t, buffer_count> empty_ring{};
    std::array<std::uint32_t, buffer_count> filled_ring{};

    alignas(cache_line_bytes) std::atomic<std::uint32_t> empty_read_pos{0};
    alignas(cache_line_bytes) std::atomic<std::uint32_t> empty_write_pos{0};
    alignas(cache_line_bytes) std::atomic<std::uint32_t> filled_read_pos{0};
    alignas(cache_line_bytes) std::atomic<std::uint32_t> filled_write_pos{0};

    alignas(cache_line_bytes) std::counting_semaphore<buffer_count> empty_semaphore{buffer_count};
    alignas(cache_line_bytes) std::counting_semaphore<buffer_count> filled_semaphore{0};

    alignas(cache_line_bytes) std::atomic<int> empty_count{static_cast<int>(buffer_count)};
    alignas(cache_line_bytes) std::atomic<int> filled_count{0};

    alignas(cache_line_bytes) std::atomic<int> consumer_current{-1};

    alignas(cache_line_bytes) std::atomic<bool> stop_requested{false};
    alignas(cache_line_bytes) std::atomic<bool> error_ready{false};

    mutable std::mutex error_mutex{};
    FileError error_value{};
};

} // namespace Center::File

