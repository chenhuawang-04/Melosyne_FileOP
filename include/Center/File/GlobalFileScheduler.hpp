#pragma once

#include "Center/File/FileReadScheduler.hpp"

#include <memory>
#include <mutex>

namespace Tool::File {

class GlobalFileScheduler final {
public:
    static auto initialize(const PlannerConfig& config_) -> FileStatus {
        std::scoped_lock guard(mutexRef());
        if (instanceRef() != nullptr) {
            return {};
        }

        instanceRef() = std::make_unique<FileReadScheduler>(config_);
        return {};
    }

    static auto initializeDefault() -> FileStatus {
        return initialize(PlannerConfig{});
    }

    static auto isInitialized() -> bool {
        std::scoped_lock guard(mutexRef());
        return instanceRef() != nullptr;
    }

    static auto get() -> FileReadScheduler& {
        std::scoped_lock guard(mutexRef());
        if (instanceRef() == nullptr) {
            instanceRef() = std::make_unique<FileReadScheduler>(PlannerConfig{});
        }
        return *instanceRef();
    }

    static auto tryGet() -> FileReadScheduler* {
        std::scoped_lock guard(mutexRef());
        return instanceRef().get();
    }

    static auto shutdown() -> void {
        std::scoped_lock guard(mutexRef());
        if (instanceRef() == nullptr) {
            return;
        }

        instanceRef()->waitIdle();
        instanceRef().reset();
    }

private:
    static auto instanceRef() -> std::unique_ptr<FileReadScheduler>& {
        static std::unique_ptr<FileReadScheduler> instance;
        return instance;
    }

    static auto mutexRef() -> std::mutex& {
        static std::mutex instance_mutex;
        return instance_mutex;
    }
};

} // namespace Tool::File

