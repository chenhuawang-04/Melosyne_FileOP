module;

#include <filesystem>
#include <list>
#include <unordered_map>

export module Tool.File.ReadHandlePool;

import Tool.File.PlatformFile;
import Tool.File.Types;
import Tool.File.Error;

export namespace Tool::File {

class ReadHandlePool {
public:
    explicit ReadHandlePool(std::size_t capacity_ = 64)
        : capacity_(capacity_) {
    }

    [[nodiscard]] auto getReadHandle(const std::filesystem::path& path_,
                                     FileHint hint_ = FileHint::sequential,
                                     FileShare share_ = FileShare::read) -> FileResult<PlatformFile*> {
        auto& state = threadLocalState();
        state.capacity = capacity_;

        auto iterator = state.entries.find(path_);
        if (iterator != state.entries.end()) {
            touchEntry(state, iterator);
            return &iterator->second.handle;
        }

        auto open_result = PlatformFile::openRead(path_, hint_, share_);
        if (!open_result) {
            return makeUnexpected(open_result.error());
        }

        evictIfNeeded(state);

        state.lru.push_front(path_);
        auto [inserted_iterator, inserted] = state.entries.emplace(path_, Entry{
            .handle = std::move(*open_result),
            .lru_iterator = state.lru.begin()
        });
        (void)inserted;

        return &inserted_iterator->second.handle;
    }

    void clearCurrentThread() {
        auto& state = threadLocalState();
        state.entries.clear();
        state.lru.clear();
    }

    [[nodiscard]] auto sizeCurrentThread() const -> std::size_t {
        return threadLocalState().entries.size();
    }

private:
    struct Entry {
        PlatformFile handle{};
        std::list<std::filesystem::path>::iterator lru_iterator{};
    };

    struct State {
        std::unordered_map<std::filesystem::path, Entry> entries{};
        std::list<std::filesystem::path> lru{};
        std::size_t capacity = 64;
    };

    [[nodiscard]] static auto threadLocalState() -> State& {
        static thread_local State thread_local_state{};
        return thread_local_state;
    }

    static void touchEntry(State& state_, std::unordered_map<std::filesystem::path, Entry>::iterator iterator_) {
        state_.lru.splice(state_.lru.begin(), state_.lru, iterator_->second.lru_iterator);
        iterator_->second.lru_iterator = state_.lru.begin();
    }

    static void evictIfNeeded(State& state_) {
        while (state_.capacity > 0 && state_.entries.size() >= state_.capacity) {
            if (state_.lru.empty()) {
                break;
            }
            auto tail_path = std::move(state_.lru.back());
            state_.lru.pop_back();
            state_.entries.erase(tail_path);
        }
    }

    std::size_t capacity_ = 64;
};

} // namespace Tool::File

