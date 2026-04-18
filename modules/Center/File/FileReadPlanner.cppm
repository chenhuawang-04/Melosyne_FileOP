module;

#include <algorithm>
#include <utility>
#include <vector>
#include <string>

export module Center.File.FileReadPlanner;

import Center.File.SchedulingTypes;

export namespace Center::File {

class FileReadPlanner {
public:
    explicit FileReadPlanner(PlannerConfig config_ = {}) noexcept
        : config_(std::move(config_)) {
    }

    [[nodiscard]] auto makePlan(const std::vector<ReadRequest>& requests_) const -> ReadPlan {
        return makePlan(requests_, 0);
    }

    [[nodiscard]] auto makePlan(const std::vector<ReadRequest>& requests_, std::uint64_t now_ticks_) const -> ReadPlan {
        ReadPlan plan{};
        plan.config = config_;

        std::vector<PlannedReadTask> staged_tasks{};
        staged_tasks.reserve(requests_.size());

        for (const auto& request : requests_) {
            if (!isValidRequest(request)) {
                continue;
            }

            const auto lane = selectLane(request, now_ticks_);
            const auto method = selectMethod(request);

            if (!shouldSplit(request)) {
                staged_tasks.push_back(makeSingleTask(request, lane, method, request.offset, request.size_bytes, request.destination));
                continue;
            }

            std::uint64_t local_offset = 0;
            while (local_offset < request.size_bytes) {
                const auto chunk_size = std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(config_.storage.split_chunk_bytes),
                    request.size_bytes - local_offset
                );

                staged_tasks.push_back(makeSingleTask(
                    request,
                    lane,
                    method,
                    request.offset + local_offset,
                    chunk_size,
                    request.destination != nullptr ? request.destination + local_offset : nullptr
                ));

                local_offset += chunk_size;
            }
        }

        sortTasks(staged_tasks);

        if (config_.storage.enable_range_merge) {
            staged_tasks = mergeTasks(std::move(staged_tasks));
        }

        for (auto& task : staged_tasks) {
            switch (task.lane) {
                case ReadLane::urgent:
                    plan.urgent_tasks.push_back(std::move(task));
                    break;
                case ReadLane::normal:
                    plan.normal_tasks.push_back(std::move(task));
                    break;
                case ReadLane::background:
                    plan.background_tasks.push_back(std::move(task));
                    break;
            }
        }

        return plan;
    }

    [[nodiscard]] auto config() const noexcept -> const PlannerConfig& {
        return config_;
    }

private:
    [[nodiscard]] auto isValidRequest(const ReadRequest& request_) const noexcept -> bool {
        if (request_.size_bytes == 0) {
            return false;
        }

        const auto allow_view_without_destination = request_.allow_mapped_view && request_.destination == nullptr;
        if (!allow_view_without_destination) {
            if (request_.destination == nullptr || request_.destination_capacity < request_.size_bytes) {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]] auto makeSingleTask(const ReadRequest& request_,
                                      ReadLane lane_,
                                      ReadMethod method_,
                                      std::uint64_t offset_,
                                      std::uint64_t size_bytes_,
                                      std::byte* destination_) const -> PlannedReadTask {
        PlannedReadTask task{};
        task.path = request_.path;
        task.offset = offset_;
        task.size_bytes = size_bytes_;
        task.destination = destination_;
        task.urgency = request_.urgency;
        task.lane = lane_;
        task.method = method_;
        task.deadline_ticks = request_.deadline_ticks;
        task.group_id = request_.group_id;
        task.request_id = request_.request_id;
        task.segments.push_back(ReadSegment{
            .source_offset = offset_,
            .size_bytes = size_bytes_,
            .destination = destination_,
            .request_id = request_.request_id
        });
        return task;
    }

    void sortTasks(std::vector<PlannedReadTask>& tasks_) const {
        std::stable_sort(tasks_.begin(), tasks_.end(), [](const PlannedReadTask& lhs_, const PlannedReadTask& rhs_) {
            if (lhs_.lane != rhs_.lane) {
                return static_cast<std::uint32_t>(lhs_.lane) < static_cast<std::uint32_t>(rhs_.lane);
            }
            if (lhs_.deadline_ticks != 0 || rhs_.deadline_ticks != 0) {
                if (lhs_.deadline_ticks == 0) {
                    return false;
                }
                if (rhs_.deadline_ticks == 0) {
                    return true;
                }
                if (lhs_.deadline_ticks != rhs_.deadline_ticks) {
                    return lhs_.deadline_ticks < rhs_.deadline_ticks;
                }
            }
            if (lhs_.urgency != rhs_.urgency) {
                return urgencyScore(lhs_.urgency) > urgencyScore(rhs_.urgency);
            }
            if (lhs_.path != rhs_.path) {
                return lhs_.path.native() < rhs_.path.native();
            }
            if (lhs_.offset != rhs_.offset) {
                return lhs_.offset < rhs_.offset;
            }
            if (lhs_.size_bytes != rhs_.size_bytes) {
                return lhs_.size_bytes < rhs_.size_bytes;
            }
            return lhs_.request_id < rhs_.request_id;
        });
    }

    [[nodiscard]] auto mergeTasks(std::vector<PlannedReadTask> tasks_) const -> std::vector<PlannedReadTask> {
        if (tasks_.empty()) {
            return tasks_;
        }

        std::vector<PlannedReadTask> merged_tasks{};
        merged_tasks.reserve(tasks_.size());

        auto canMerge = [&](const PlannedReadTask& lhs_, const PlannedReadTask& rhs_) {
            if (lhs_.path != rhs_.path || lhs_.lane != rhs_.lane || lhs_.method != rhs_.method) {
                return false;
            }

            if (lhs_.deadline_ticks != rhs_.deadline_ticks) {
                return false;
            }

            const auto lhs_end = lhs_.offset + lhs_.size_bytes;
            if (rhs_.offset < lhs_.offset) {
                return false;
            }

            const auto gap = rhs_.offset > lhs_end ? (rhs_.offset - lhs_end) : 0;
            if (gap > config_.storage.merge_gap_bytes) {
                return false;
            }

            const auto new_end = std::max(lhs_end, rhs_.offset + rhs_.size_bytes);
            const auto new_span = new_end - lhs_.offset;
            if (new_span > config_.storage.merge_max_bytes) {
                return false;
            }

            if (lhs_.method == ReadMethod::directRead) {
                const auto has_null_destination = (lhs_.destination == nullptr || rhs_.destination == nullptr);
                if (has_null_destination) {
                    return false;
                }
            }

            return true;
        };

        auto consume = [&](PlannedReadTask& dst_, PlannedReadTask& src_) {
            const auto dst_end = dst_.offset + dst_.size_bytes;
            const auto src_end = src_.offset + src_.size_bytes;
            dst_.size_bytes = std::max(dst_end, src_end) - dst_.offset;

            for (auto& segment : src_.segments) {
                dst_.segments.push_back(segment);
            }

            if (dst_.segments.size() == 1) {
                dst_.destination = dst_.segments.front().destination;
            } else {
                dst_.destination = nullptr;
            }
        };

        merged_tasks.push_back(std::move(tasks_.front()));

        for (std::size_t index = 1; index < tasks_.size(); ++index) {
            auto& tail = merged_tasks.back();
            auto& current = tasks_[index];
            if (canMerge(tail, current)) {
                consume(tail, current);
            } else {
                merged_tasks.push_back(std::move(current));
            }
        }

        return merged_tasks;
    }

    [[nodiscard]] auto selectLane(const ReadRequest& request_, std::uint64_t now_ticks_) const noexcept -> ReadLane {
        if (isDeadlineUrgent(request_, now_ticks_)) {
            return ReadLane::urgent;
        }

        if (request_.urgency == ReadUrgency::immediate || request_.urgency == ReadUrgency::frameCritical || request_.same_frame_required || request_.startup_critical) {
            return ReadLane::urgent;
        }
        if (request_.urgency == ReadUrgency::streaming || request_.urgency == ReadUrgency::preload) {
            return ReadLane::normal;
        }
        return ReadLane::background;
    }

    [[nodiscard]] auto isDeadlineUrgent(const ReadRequest& request_, std::uint64_t now_ticks_) const noexcept -> bool {
        if (config_.deadline_preempt_window_ticks == 0 || request_.deadline_ticks == 0) {
            return false;
        }
        if (request_.deadline_ticks <= now_ticks_) {
            return true;
        }
        return (request_.deadline_ticks - now_ticks_) <= config_.deadline_preempt_window_ticks;
    }

    [[nodiscard]] auto selectMethod(const ReadRequest& request_) const noexcept -> ReadMethod {
        if (request_.allow_mapped_view && request_.size_bytes >= config_.storage.large_file_threshold) {
            return ReadMethod::mappedView;
        }
        if (request_.allow_mapped_copy && request_.size_bytes >= config_.storage.medium_file_threshold) {
            return ReadMethod::mappedCopy;
        }
        return ReadMethod::directRead;
    }

    [[nodiscard]] auto shouldSplit(const ReadRequest& request_) const noexcept -> bool {
        if (!request_.allow_split) {
            return false;
        }
        if (request_.size_bytes < config_.storage.large_file_threshold) {
            return false;
        }
        return config_.storage.split_chunk_bytes > 0;
    }

    PlannerConfig config_{};
};

} // namespace Center::File
