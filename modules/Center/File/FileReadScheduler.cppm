module;

#if defined(CENTER_FILE_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <Windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

export module Tool.File.FileReadScheduler;

import Tool.File.FileReadPlanner;
import Tool.File.PlatformFile;
import Tool.File.ReadHandlePool;
import Tool.File.ThreadCenterAdapter;
import Tool.File.Types;
import Tool.File.Error;
import Tool.File.SchedulingTypes;

export namespace Tool::File {


struct ReadView {
    std::filesystem::path path{};
    std::uint64_t source_offset = 0;
    std::uint64_t size_bytes = 0;
    std::uint32_t request_id = 0;

    const std::byte* data = nullptr;
    std::shared_ptr<void> owner{};
};

using ReadViewCallback = std::function<void(const ReadView& view_)>;

class FileReadScheduler {
public:
    explicit FileReadScheduler(PlannerConfig planner_config_ = {})
        : planner_(planner_config_),
          adapter_(std::max(1u, planner_config_.worker_limit)),
          read_handle_pool_(planner_config_.handle_pool_capacity) {
    }

    [[nodiscard]] auto makePlan(const std::vector<ReadRequest>& requests_) const -> ReadPlan {
        return planner_.makePlan(requests_, currentSteadyTicks());
    }

    [[nodiscard]] auto runRequests(const std::vector<ReadRequest>& requests_) -> FileStatus {
        auto plan = planner_.makePlan(requests_, currentSteadyTicks());
        return runPlan(plan);
    }

    [[nodiscard]] auto runPlan(const ReadPlan& plan_) -> FileStatus {
        auto dispatch_status = adapter_.runPlan(plan_, [this](const PlannedReadTask& task_) {
            return executeTask(task_, nullptr, nullptr, nullptr);
        });

        if (dispatch_status) {
            return {};
        }

        if (dispatch_status.error().code == std::make_error_code(std::errc::function_not_supported)) {
            return runPlanSequential(plan_, nullptr, nullptr);
        }

        return dispatch_status;
    }

    [[nodiscard]] auto runViewRequests(const std::vector<ReadRequest>& requests_, std::vector<ReadView>& out_views_) -> FileStatus {
        return runViewRequests(requests_, out_views_, ReadViewCallback{});
    }

    [[nodiscard]] auto runViewRequests(const std::vector<ReadRequest>& requests_,
                                       std::vector<ReadView>& out_views_,
                                       const ReadViewCallback& view_callback_) -> FileStatus {
        auto plan = planner_.makePlan(requests_, currentSteadyTicks());

        std::mutex view_mutex;
        auto dispatch_status = adapter_.runPlan(plan, [this, &out_views_, &view_mutex, &view_callback_](const PlannedReadTask& task_) {
            return executeTask(task_, &out_views_, &view_mutex, &view_callback_);
        });

        if (dispatch_status) {
            return {};
        }

        if (dispatch_status.error().code == std::make_error_code(std::errc::function_not_supported)) {
            return runPlanSequential(plan, &out_views_, &view_callback_);
        }

        return dispatch_status;
    }

    void waitIdle() {
        adapter_.waitIdle();
    }

private:
#if defined(CENTER_FILE_WINDOWS)
    class ScopedWinHandle {
    public:
        ScopedWinHandle() noexcept = default;

        explicit ScopedWinHandle(HANDLE handle_) noexcept
            : handle_(handle_) {
        }

        ScopedWinHandle(const ScopedWinHandle&) = delete;
        auto operator=(const ScopedWinHandle&) -> ScopedWinHandle& = delete;

        ScopedWinHandle(ScopedWinHandle&& other_) noexcept
            : handle_(other_.handle_) {
            other_.handle_ = invalidHandle();
        }

        auto operator=(ScopedWinHandle&& other_) noexcept -> ScopedWinHandle& {
            if (this != &other_) {
                close();
                handle_ = other_.handle_;
                other_.handle_ = invalidHandle();
            }
            return *this;
        }

        ~ScopedWinHandle() noexcept {
            close();
        }

        [[nodiscard]] auto isValid() const noexcept -> bool {
            return handle_ != nullptr && handle_ != invalidHandle();
        }

        [[nodiscard]] auto get() const noexcept -> HANDLE {
            return handle_;
        }

    private:
        [[nodiscard]] static auto invalidHandle() noexcept -> HANDLE {
            return INVALID_HANDLE_VALUE;
        }

        void close() noexcept {
            if (isValid()) {
                ::CloseHandle(handle_);
                handle_ = invalidHandle();
            }
        }

        HANDLE handle_ = invalidHandle();
    };

    struct MappingBundle {
        ScopedWinHandle file_handle{};
        ScopedWinHandle mapping_handle{};
        void* mapped_base = nullptr;
        std::uint64_t aligned_offset = 0;
        std::uint64_t mapped_size = 0;

        MappingBundle() = default;
        MappingBundle(const MappingBundle&) = delete;
        auto operator=(const MappingBundle&) -> MappingBundle& = delete;

        MappingBundle(MappingBundle&& other_) noexcept
            : file_handle(std::move(other_.file_handle)),
              mapping_handle(std::move(other_.mapping_handle)),
              mapped_base(other_.mapped_base),
              aligned_offset(other_.aligned_offset),
              mapped_size(other_.mapped_size) {
            other_.mapped_base = nullptr;
            other_.aligned_offset = 0;
            other_.mapped_size = 0;
        }

        auto operator=(MappingBundle&& other_) noexcept -> MappingBundle& {
            if (this != &other_) {
                if (mapped_base != nullptr) {
                    ::UnmapViewOfFile(mapped_base);
                }
                file_handle = std::move(other_.file_handle);
                mapping_handle = std::move(other_.mapping_handle);
                mapped_base = other_.mapped_base;
                aligned_offset = other_.aligned_offset;
                mapped_size = other_.mapped_size;
                other_.mapped_base = nullptr;
                other_.aligned_offset = 0;
                other_.mapped_size = 0;
            }
            return *this;
        }

        ~MappingBundle() {
            if (mapped_base != nullptr) {
                ::UnmapViewOfFile(mapped_base);
                mapped_base = nullptr;
            }
        }
    };
#endif

    [[nodiscard]] static auto currentSteadyTicks() -> std::uint64_t {
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
    }

    [[nodiscard]] auto runPlanSequential(const ReadPlan& plan_,
                                         std::vector<ReadView>* out_views_,
                                         const ReadViewCallback* view_callback_) -> FileStatus {
        for (const auto& task : plan_.urgent_tasks) {
            auto status = executeTask(task, out_views_, nullptr, view_callback_);
            if (!status) {
                return status;
            }
        }

        for (const auto& task : plan_.normal_tasks) {
            auto status = executeTask(task, out_views_, nullptr, view_callback_);
            if (!status) {
                return status;
            }
        }

        for (const auto& task : plan_.background_tasks) {
            auto status = executeTask(task, out_views_, nullptr, view_callback_);
            if (!status) {
                return status;
            }
        }

        return {};
    }

    [[nodiscard]] auto executeTask(const PlannedReadTask& task_,
                                   std::vector<ReadView>* out_views_,
                                   std::mutex* out_views_mutex_ = nullptr,
                                   const ReadViewCallback* view_callback_ = nullptr) -> FileStatus {
        switch (task_.method) {
            case ReadMethod::directRead:
                return executeDirectReadTask(task_);
            case ReadMethod::mappedCopy:
                return executeMappedCopyTask(task_);
            case ReadMethod::mappedView:
                return executeMappedViewTask(task_, out_views_, out_views_mutex_, view_callback_);
        }

        return makeUnexpected(FileError{
            .operation = FileOperation::read,
            .code = std::make_error_code(std::errc::invalid_argument)
        });
    }

    [[nodiscard]] auto executeDirectReadTask(const PlannedReadTask& task_) -> FileStatus {
        if (task_.segments.size() <= 1) {
            if (task_.destination == nullptr || task_.size_bytes == 0) {
                return makeUnexpected(FileError{
                    .operation = FileOperation::read,
                    .code = std::make_error_code(std::errc::invalid_argument)
                });
            }

            auto file_result = read_handle_pool_.getReadHandle(task_.path, FileHint::sequential, FileShare::read);
            if (!file_result) {
                return makeUnexpected(file_result.error());
            }

            auto* file = *file_result;
            auto bytes = MutableBytes{task_.destination, static_cast<std::size_t>(task_.size_bytes)};
            return file->readExactAt(task_.offset, bytes);
        }

        auto file_result = read_handle_pool_.getReadHandle(task_.path, FileHint::sequential, FileShare::read);
        if (!file_result) {
            return makeUnexpected(file_result.error());
        }

        auto* file = *file_result;
        std::vector<std::byte> merged_buffer(static_cast<std::size_t>(task_.size_bytes));
        auto merged_span = MutableBytes{merged_buffer.data(), merged_buffer.size()};
        auto read_status = file->readExactAt(task_.offset, merged_span);
        if (!read_status) {
            return read_status;
        }

        for (const auto& segment : task_.segments) {
            if (segment.destination == nullptr || segment.size_bytes == 0) {
                continue;
            }

            const auto local_offset = segment.source_offset - task_.offset;
            std::memcpy(
                segment.destination,
                merged_buffer.data() + static_cast<std::size_t>(local_offset),
                static_cast<std::size_t>(segment.size_bytes)
            );
        }

        return {};
    }

    [[nodiscard]] auto executeMappedCopyTask(const PlannedReadTask& task_) -> FileStatus {
#if defined(CENTER_FILE_WINDOWS)
        auto mapping_result = openMapping(task_.path, task_.offset, task_.size_bytes);
        if (!mapping_result) {
            return makeUnexpected(mapping_result.error());
        }

        auto bundle = std::move(*mapping_result);
        const auto base = static_cast<const std::byte*>(bundle.mapped_base);
        const auto bias = static_cast<std::size_t>(task_.offset - bundle.aligned_offset);

        if (task_.segments.size() <= 1) {
            if (task_.destination == nullptr || task_.size_bytes == 0) {
                return makeUnexpected(FileError{
                    .operation = FileOperation::read,
                    .code = std::make_error_code(std::errc::invalid_argument)
                });
            }
            std::memcpy(task_.destination, base + bias, static_cast<std::size_t>(task_.size_bytes));
            return {};
        }

        for (const auto& segment : task_.segments) {
            if (segment.destination == nullptr || segment.size_bytes == 0) {
                continue;
            }
            const auto local_offset = static_cast<std::size_t>(segment.source_offset - bundle.aligned_offset);
            std::memcpy(segment.destination, base + local_offset, static_cast<std::size_t>(segment.size_bytes));
        }

        return {};
#else
        (void)task_;
        return makeUnexpected(FileError{
            .operation = FileOperation::read,
            .code = std::make_error_code(std::errc::function_not_supported)
        });
#endif
    }

    [[nodiscard]] auto executeMappedViewTask(const PlannedReadTask& task_,
                                             std::vector<ReadView>* out_views_,
                                             std::mutex* out_views_mutex_,
                                             const ReadViewCallback* view_callback_) -> FileStatus {
#if defined(CENTER_FILE_WINDOWS)
        if (out_views_ == nullptr) {
            return executeMappedCopyTask(task_);
        }

        auto mapping_result = openMapping(task_.path, task_.offset, task_.size_bytes);
        if (!mapping_result) {
            return makeUnexpected(mapping_result.error());
        }

        auto bundle_ptr = std::make_shared<MappingBundle>(std::move(*mapping_result));
        const auto* base = static_cast<const std::byte*>(bundle_ptr->mapped_base);

        auto push_view = [&](const ReadSegment& segment_) {
            const auto local_offset = static_cast<std::size_t>(segment_.source_offset - bundle_ptr->aligned_offset);
            ReadView view{};
            view.path = task_.path;
            view.source_offset = segment_.source_offset;
            view.size_bytes = segment_.size_bytes;
            view.request_id = segment_.request_id;
            view.data = base + local_offset;
            view.owner = std::static_pointer_cast<void>(bundle_ptr);

            if (out_views_mutex_ != nullptr) {
                std::scoped_lock guard(*out_views_mutex_);
                out_views_->push_back(view);
            } else {
                out_views_->push_back(view);
            }

            if (view_callback_ != nullptr && *view_callback_) {
                (*view_callback_)(view);
            }
        };

        if (task_.segments.empty()) {
            push_view(ReadSegment{
                .source_offset = task_.offset,
                .size_bytes = task_.size_bytes,
                .destination = nullptr,
                .request_id = task_.request_id
            });
            return {};
        }

        for (const auto& segment : task_.segments) {
            if (segment.size_bytes == 0) {
                continue;
            }
            push_view(segment);
        }

        return {};
#else
        (void)task_;
        (void)out_views_;
        (void)out_views_mutex_;
        (void)view_callback_;
        return makeUnexpected(FileError{
            .operation = FileOperation::read,
            .code = std::make_error_code(std::errc::function_not_supported)
        });
#endif
    }

#if defined(CENTER_FILE_WINDOWS)
    [[nodiscard]] auto openMapping(const std::filesystem::path& path_,
                                   std::uint64_t offset_,
                                   std::uint64_t size_bytes_) -> FileResult<MappingBundle> {
        MappingBundle bundle{};

        bundle.file_handle = ScopedWinHandle{::CreateFileW(
            path_.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr)};
        if (!bundle.file_handle.isValid()) {
            return makeUnexpected(FileError{
                .operation = FileOperation::open,
                .code = std::error_code(static_cast<int>(::GetLastError()), std::system_category()),
                .offset = offset_,
                .requested = static_cast<std::size_t>(size_bytes_),
                .processed = 0
            });
        }

        SYSTEM_INFO system_info{};
        ::GetSystemInfo(&system_info);
        const auto granularity = static_cast<std::uint64_t>(system_info.dwAllocationGranularity);

        bundle.aligned_offset = (offset_ / granularity) * granularity;
        const auto bias = offset_ - bundle.aligned_offset;
        bundle.mapped_size = size_bytes_ + bias;

        bundle.mapping_handle = ScopedWinHandle{::CreateFileMappingW(
            bundle.file_handle.get(),
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr)};
        if (!bundle.mapping_handle.isValid()) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::error_code(static_cast<int>(::GetLastError()), std::system_category()),
                .offset = offset_,
                .requested = static_cast<std::size_t>(size_bytes_),
                .processed = 0
            });
        }

        const std::uint64_t max_low_mask = 0xFFFFFFFFull;
        bundle.mapped_base = ::MapViewOfFile(
            bundle.mapping_handle.get(),
            FILE_MAP_READ,
            static_cast<DWORD>(bundle.aligned_offset >> 32),
            static_cast<DWORD>(bundle.aligned_offset & max_low_mask),
            static_cast<SIZE_T>(bundle.mapped_size));
        if (bundle.mapped_base == nullptr) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::error_code(static_cast<int>(::GetLastError()), std::system_category()),
                .offset = offset_,
                .requested = static_cast<std::size_t>(size_bytes_),
                .processed = 0
            });
        }

        return std::move(bundle);
    }
#endif

    FileReadPlanner planner_;
    ThreadCenterAdapter adapter_;
    ReadHandlePool read_handle_pool_{};
};

} // namespace Tool::File

