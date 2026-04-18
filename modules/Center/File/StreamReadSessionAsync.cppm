module;

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory_resource>
#include <mutex>
#include <span>
#include <thread>
#include <system_error>

export module Center.File.StreamReadSessionAsync;

import Center.File.PlatformFile;
import Center.File.StreamTypes;
import Center.File.Types;
import Center.File.Error;

export namespace Center::File {


class StreamReadSessionAsync {
public:
    StreamReadSessionAsync() = default;
    StreamReadSessionAsync(const StreamReadSessionAsync&) = delete;
    auto operator=(const StreamReadSessionAsync&) -> StreamReadSessionAsync& = delete;

    ~StreamReadSessionAsync() {
        stop();
        releaseStorage();
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
        const auto total_readable_bytes_local = (max_bytes_ == 0)
            ? remaining_from_offset
            : (std::min<std::uint64_t>)(max_bytes_, remaining_from_offset);

        auto alloc_status = allocateStorage(config_.chunk_bytes, config_.memory_resource);
        if (!alloc_status) {
            return alloc_status;
        }

        config_value = config_;
        file_value = std::move(*open_result);

        stream_offset.store(start_offset_, std::memory_order_release);
        remaining_bytes.store(total_readable_bytes_local, std::memory_order_release);

        produced_chunks.store(0, std::memory_order_release);
        consumed_chunks.store(0, std::memory_order_release);
        producer_block_count.store(0, std::memory_order_release);
        consumer_starve_count.store(0, std::memory_order_release);
        bytes_read.store(0, std::memory_order_release);

        paused.store(false, std::memory_order_release);
        stop_requested.store(false, std::memory_order_release);
        producer_done.store(total_readable_bytes_local == 0, std::memory_order_release);
        producer_error.store(false, std::memory_order_release);
        end_of_stream.store(total_readable_bytes_local == 0, std::memory_order_release);
        started.store(true, std::memory_order_release);

        {
            std::scoped_lock lock(queue_mutex);
            produce_index = 0;
            consume_index = 0;
            ready_count = 0;
            ready_count_atomic.store(0, std::memory_order_release);
            current_slot_index = -1;
            current_slot_offset = 0;
            for (auto& slot : slots) {
                slot.size = 0;
                slot.file_offset = 0;
                slot.end_of_stream = false;
                slot.ready = false;
            }
            producer_error_value = {};
        }

        producer_thread = std::jthread([this]() { producerLoop(); });

        return {};
    }

    auto stop() -> void {
        started.store(false, std::memory_order_release);
        paused.store(false, std::memory_order_release);
        stop_requested.store(true, std::memory_order_release);

        queue_cv_producer.notify_all();
        queue_cv_consumer.notify_all();

        if (producer_thread.joinable()) {
            producer_thread.join();
        }

        {
            std::scoped_lock lock(queue_mutex);
            produce_index = 0;
            consume_index = 0;
            ready_count = 0;
            ready_count_atomic.store(0, std::memory_order_release);
            current_slot_index = -1;
            current_slot_offset = 0;
            for (auto& slot : slots) {
                slot.size = 0;
                slot.file_offset = 0;
                slot.end_of_stream = false;
                slot.ready = false;
            }
        }

        end_of_stream.store(true, std::memory_order_release);
        producer_done.store(true, std::memory_order_release);

        stream_offset.store(0, std::memory_order_release);
        remaining_bytes.store(0, std::memory_order_release);

        if (file_value.isOpen()) {
            static_cast<void>(file_value.close());
        }
    }

    auto pause() -> void {
        paused.store(true, std::memory_order_release);
    }

    auto resume() -> void {
        paused.store(false, std::memory_order_release);
        queue_cv_producer.notify_all();
    }

    [[nodiscard]] auto readNext(std::span<std::byte> destination_) -> FileResult<std::size_t> {
        return readNextInternal(destination_, true);
    }

    [[nodiscard]] auto tryReadNext(std::span<std::byte> destination_) -> FileResult<std::size_t> {
        return readNextInternal(destination_, false);
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
    static constexpr std::size_t slot_count = 3;
    static constexpr std::size_t cache_line_bytes = 64;

    struct SlotState {
        std::size_t size = 0;
        std::uint64_t file_offset = 0;
        bool end_of_stream = false;
        bool ready = false;
    };

    [[nodiscard]] auto allocateStorage(std::size_t chunk_bytes_, std::pmr::memory_resource* memory_resource_) -> FileStatus {
        if (memory_resource_ == nullptr) {
            memory_resource_ = std::pmr::get_default_resource();
        }

        releaseStorage();

        const auto aligned_chunk_bytes = alignUp(chunk_bytes_, cache_line_bytes);
        const auto total_bytes = aligned_chunk_bytes * slot_count;

        auto* ptr = static_cast<std::byte*>(memory_resource_->allocate(total_bytes, cache_line_bytes));
        if (ptr == nullptr) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::not_enough_memory)
            });
        }

        storage_resource = memory_resource_;
        storage_ptr = ptr;
        storage_total_bytes = total_bytes;
        slot_bytes = aligned_chunk_bytes;

        return {};
    }

    auto releaseStorage() -> void {
        if (storage_ptr != nullptr && storage_resource != nullptr && storage_total_bytes > 0) {
            storage_resource->deallocate(storage_ptr, storage_total_bytes, cache_line_bytes);
        }

        storage_ptr = nullptr;
        storage_resource = nullptr;
        storage_total_bytes = 0;
        slot_bytes = 0;
    }

    [[nodiscard]] static auto alignUp(std::size_t value_, std::size_t align_) -> std::size_t {
        const auto mask = align_ - 1;
        return (value_ + mask) & ~mask;
    }

    [[nodiscard]] auto slotSpan(std::uint32_t index_) -> std::span<std::byte> {
        if (storage_ptr == nullptr || index_ >= slot_count || slot_bytes == 0) {
            return {};
        }
        return std::span<std::byte>{storage_ptr + (slot_bytes * index_), slot_bytes};
    }

    auto producerLoop() -> void {
        while (!stop_requested.load(std::memory_order_acquire)) {
            if (paused.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }

            auto left = remaining_bytes.load(std::memory_order_acquire);
            if (left == 0) {
                producer_done.store(true, std::memory_order_release);
                queue_cv_consumer.notify_one();
                break;
            }

            std::uint32_t slot_index_local = 0;
            {
                std::unique_lock lock(queue_mutex);
                if (ready_count >= slot_count) {
                    producer_block_count.fetch_add(1, std::memory_order_relaxed);
                }
                queue_cv_producer.wait(lock, [this]() {
                    return stop_requested.load(std::memory_order_acquire) || ready_count < slot_count;
                });
                if (stop_requested.load(std::memory_order_acquire)) {
                    break;
                }
                slot_index_local = produce_index;
                produce_index = (produce_index + 1) % static_cast<std::uint32_t>(slot_count);
            }

            auto write_span = slotSpan(slot_index_local);
            if (write_span.empty()) {
                std::scoped_lock lock(queue_mutex);
                producer_error_value = FileError{
                    .operation = FileOperation::read,
                    .code = std::make_error_code(std::errc::no_buffer_space)
                };
                producer_error.store(true, std::memory_order_release);
                producer_done.store(true, std::memory_order_release);
                queue_cv_consumer.notify_one();
                break;
            }

            left = remaining_bytes.load(std::memory_order_acquire);
            const auto request_bytes = static_cast<std::size_t>((std::min<std::uint64_t>)(
                left,
                static_cast<std::uint64_t>(write_span.size())
            ));

            const auto offset_before = stream_offset.load(std::memory_order_acquire);
            auto read_result = file_value.readAt(offset_before, write_span.first(request_bytes));
            if (!read_result) {
                std::scoped_lock lock(queue_mutex);
                producer_error_value = read_result.error();
                producer_error.store(true, std::memory_order_release);
                producer_done.store(true, std::memory_order_release);
                queue_cv_consumer.notify_one();
                break;
            }

            const auto read_size = *read_result;
            const bool short_read = read_size < request_bytes;

            if (read_size > 0) {
                stream_offset.fetch_add(static_cast<std::uint64_t>(read_size), std::memory_order_acq_rel);
                remaining_bytes.fetch_sub(static_cast<std::uint64_t>(read_size), std::memory_order_acq_rel);
                bytes_read.fetch_add(read_size, std::memory_order_relaxed);
            }

            produced_chunks.fetch_add(1, std::memory_order_relaxed);

            const auto left_after = remaining_bytes.load(std::memory_order_acquire);
            const bool eos = short_read || (read_size == 0) || (left_after == 0);

            {
                std::scoped_lock lock(queue_mutex);
                auto& slot = slots[slot_index_local];
                slot.size = read_size;
                slot.file_offset = offset_before;
                slot.end_of_stream = eos;
                slot.ready = true;
                ++ready_count;
                ready_count_atomic.store(ready_count, std::memory_order_release);
            }

            queue_cv_consumer.notify_one();

            if (eos) {
                producer_done.store(true, std::memory_order_release);
                queue_cv_consumer.notify_one();
                break;
            }
        }

        producer_done.store(true, std::memory_order_release);
        queue_cv_consumer.notify_all();
        queue_cv_producer.notify_all();
    }

    [[nodiscard]] auto readNextInternal(std::span<std::byte> destination_, bool block_) -> FileResult<std::size_t> {
        if (destination_.empty()) {
            return std::size_t{0};
        }

        if (!started.load(std::memory_order_acquire)) {
            return std::size_t{0};
        }

        if (paused.load(std::memory_order_acquire)) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::operation_canceled)
            });
        }

        if (!block_
            && current_slot_index < 0
            && ready_count_atomic.load(std::memory_order_acquire) == 0
            && !producer_done.load(std::memory_order_acquire)
            && !producer_error.load(std::memory_order_acquire)) {
            consumer_starve_count.fetch_add(1, std::memory_order_relaxed);
            return std::size_t{0};
        }

        std::uint32_t slot_index_local = 0;
        std::size_t offset_local = 0;
        std::size_t available_local = 0;
        bool eos_local = false;

        {
            std::unique_lock lock(queue_mutex);
            if (block_) {
                queue_cv_consumer.wait(lock, [this]() {
                    return stop_requested.load(std::memory_order_acquire)
                        || producer_error.load(std::memory_order_acquire)
                        || current_slot_index >= 0
                        || ready_count > 0
                        || producer_done.load(std::memory_order_acquire);
                });
            }

            if (producer_error.load(std::memory_order_acquire)) {
                return makeUnexpected(producer_error_value);
            }

            if (current_slot_index < 0) {
                if (ready_count == 0) {
                    if (!block_) {
                        consumer_starve_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (producer_done.load(std::memory_order_acquire)) {
                        end_of_stream.store(true, std::memory_order_release);
                    }
                    return std::size_t{0};
                }

                current_slot_index = static_cast<int>(consume_index);
                current_slot_offset = 0;
            }

            slot_index_local = static_cast<std::uint32_t>(current_slot_index);
            auto& slot = slots[slot_index_local];
            available_local = (slot.size > current_slot_offset) ? (slot.size - current_slot_offset) : 0;
            offset_local = current_slot_offset;
            eos_local = slot.end_of_stream;

            if (available_local == 0) {
                slot.ready = false;
                slot.size = 0;
                slot.file_offset = 0;
                slot.end_of_stream = false;

                if (ready_count > 0) {
                    --ready_count;
                    ready_count_atomic.store(ready_count, std::memory_order_release);
                }

                consume_index = (consume_index + 1) % static_cast<std::uint32_t>(slot_count);
                current_slot_index = -1;
                current_slot_offset = 0;

                consumed_chunks.fetch_add(1, std::memory_order_relaxed);
                queue_cv_producer.notify_all();

                if (eos_local) {
                    end_of_stream.store(true, std::memory_order_release);
                }

                return std::size_t{0};
            }
        }

        const auto copy_size = (std::min)(destination_.size(), available_local);
        auto read_span = slotSpan(slot_index_local);
        std::memcpy(destination_.data(), read_span.data() + offset_local, copy_size);

        {
            std::scoped_lock lock(queue_mutex);
            auto& slot = slots[slot_index_local];
            current_slot_offset += copy_size;

            if (current_slot_offset >= slot.size) {
                const bool slot_eos = slot.end_of_stream;

                slot.ready = false;
                slot.size = 0;
                slot.file_offset = 0;
                slot.end_of_stream = false;

                if (ready_count > 0) {
                    --ready_count;
                    ready_count_atomic.store(ready_count, std::memory_order_release);
                }

                consume_index = (consume_index + 1) % static_cast<std::uint32_t>(slot_count);
                current_slot_index = -1;
                current_slot_offset = 0;

                consumed_chunks.fetch_add(1, std::memory_order_relaxed);
                queue_cv_producer.notify_all();

                if (slot_eos) {
                    end_of_stream.store(true, std::memory_order_release);
                }
            }
        }

        return copy_size;
    }

    StreamReadConfig config_value{};
    PlatformFile file_value{};

    std::jthread producer_thread{};

    std::pmr::memory_resource* storage_resource = nullptr;
    std::byte* storage_ptr = nullptr;
    std::size_t storage_total_bytes = 0;
    std::size_t slot_bytes = 0;

    mutable std::mutex queue_mutex{};
    std::condition_variable queue_cv_producer{};
    std::condition_variable queue_cv_consumer{};
    std::array<SlotState, slot_count> slots{};

    std::uint32_t produce_index = 0;
    std::uint32_t consume_index = 0;
    std::size_t ready_count = 0;
    std::atomic<std::size_t> ready_count_atomic{0};

    int current_slot_index = -1;
    std::size_t current_slot_offset = 0;

    FileError producer_error_value{};

    std::atomic<std::uint64_t> stream_offset{0};
    std::atomic<std::uint64_t> remaining_bytes{0};

    std::atomic<std::uint64_t> produced_chunks{0};
    std::atomic<std::uint64_t> consumed_chunks{0};
    std::atomic<std::uint64_t> producer_block_count{0};
    std::atomic<std::uint64_t> consumer_starve_count{0};
    std::atomic<std::uint64_t> bytes_read{0};

    std::atomic<bool> paused{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> producer_done{true};
    std::atomic<bool> producer_error{false};
    std::atomic<bool> end_of_stream{true};
    std::atomic<bool> started{false};
};

} // namespace Center::File

