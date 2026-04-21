module;

#include "Center/File/BuildConfig.hpp"

#include <atomic>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>

#if CENTER_FILE_HAS_THREAD_CENTER
    #include <thread_center/thread_center.hpp>
#endif

export module Tool.File.ThreadCenterAdapter;

import Tool.File.Error;
import Tool.File.SchedulingTypes;

export namespace Tool::File {

using PlannedTaskExecutor = std::function<FileStatus(const PlannedReadTask& task_)>;

#if CENTER_FILE_HAS_THREAD_CENTER

class ThreadCenterAdapter {
public:
    explicit ThreadCenterAdapter(std::uint32_t workers_)
        : center_(ThreadCenter::ExecutorConfig{.workers = workers_}) {
    }

    [[nodiscard]] auto runPlan(const ReadPlan& plan_, const PlannedTaskExecutor& executor_) -> FileStatus {
        if (!executor_) {
            return makeUnexpected(FileError{
                .operation = FileOperation::read,
                .code = std::make_error_code(std::errc::invalid_argument)
            });
        }

        auto plan = center_.makePlan();

        std::mutex error_mutex;
        DynamicArray<FileError> errors{};
        std::atomic<bool> has_error{false};

        auto submitLane = [&](const DynamicArray<PlannedReadTask>& tasks_, const char* lane_name_) {
            DynamicArray<decltype(plan.task(ThreadCenter::TaskDesc{}, [] {}))> handles{};
            handles.reserve(tasks_.size());

            for (const auto& task : tasks_) {
                const auto task_name = std::string{lane_name_} + "_" + std::to_string(task.request_id);
                auto handle = plan.task(ThreadCenter::TaskDesc{.name = task_name}, [&, task]() {
                    if (has_error.load(std::memory_order_relaxed)) {
                        return;
                    }

                    auto status = executor_(task);
                    if (!status) {
                        has_error.store(true, std::memory_order_relaxed);
                        std::scoped_lock guard(error_mutex);
                        errors.push_back(status.error());
                    }
                });
                handles.push_back(handle);
            }

            return handles;
        };

        auto urgent_handles = submitLane(plan_.urgent_tasks, "urgent");
        auto normal_handles = submitLane(plan_.normal_tasks, "normal");
        auto background_handles = submitLane(plan_.background_tasks, "background");

        auto urgent_done = plan.gate(ThreadCenter::TaskDesc{.name = "urgent_done"});
        auto normal_done = plan.gate(ThreadCenter::TaskDesc{.name = "normal_done"});

        for (auto& handle : urgent_handles) {
            plan.precede(handle, urgent_done);
        }
        for (auto& handle : normal_handles) {
            plan.precede(urgent_done, handle);
            plan.precede(handle, normal_done);
        }

        if (normal_handles.empty()) {
            for (auto& handle : background_handles) {
                plan.precede(urgent_done, handle);
            }
        } else {
            for (auto& handle : background_handles) {
                plan.precede(normal_done, handle);
            }
        }

        auto run_handle = center_.dispatch(plan);
        run_handle.wait();

        if (!errors.empty()) {
            return std::unexpected(errors.front());
        }

        return {};
    }

    void waitIdle() {
        center_.waitIdle();
    }

private:
    ThreadCenter::Center center_;
};

#else

class ThreadCenterAdapter {
public:
    explicit ThreadCenterAdapter(std::uint32_t) {
    }

    [[nodiscard]] auto runPlan(const ReadPlan&, const PlannedTaskExecutor&) -> FileStatus {
        return makeUnexpected(FileError{
            .operation = FileOperation::read,
            .code = std::make_error_code(std::errc::function_not_supported)
        });
    }

    void waitIdle() {
    }
};

#endif

} // namespace Tool::File
