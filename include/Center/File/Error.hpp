#pragma once

#include "Center/File/Config.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <system_error>

namespace Tool::File {

enum class FileOperation : std::uint8_t {
    open,
    close,
    stat,
    read,
    write,
    resize,
    flush
};

struct FileError {
    FileOperation operation{};
    std::error_code code{};
    std::uint64_t offset = 0;
    std::size_t requested = 0;
    std::size_t processed = 0;
    bool end_of_file = false;
};

template<typename T>
using FileResult = std::expected<T, FileError>;

using FileStatus = FileResult<void>;

[[nodiscard]] CENTER_FILE_ALWAYS_INLINE
std::unexpected<FileError> makeUnexpected(FileError error_) noexcept {
    return std::unexpected<FileError>{error_};
}

[[nodiscard]] constexpr const char* toString(FileOperation operation_) noexcept {
    switch (operation_) {
        case FileOperation::open: return "open";
        case FileOperation::close: return "close";
        case FileOperation::stat: return "stat";
        case FileOperation::read: return "read";
        case FileOperation::write: return "write";
        case FileOperation::resize: return "resize";
        case FileOperation::flush: return "flush";
    }
    return "unknown";
}

} // namespace Tool::File

