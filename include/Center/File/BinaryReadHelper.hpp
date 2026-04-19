#pragma once

#include "Center/File/Error.hpp"
#include "Center/File/PlatformFile.hpp"
#include "Center/File/Types.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>
#include <type_traits>

namespace Tool::File {

template<typename pod_type>
concept PodReadable = std::is_trivial_v<pod_type> && std::is_standard_layout_v<pod_type>;

[[nodiscard]] CENTER_FILE_ALWAYS_INLINE
FileError makeSizeMismatchError(std::uint64_t file_size_bytes_, std::size_t destination_size_bytes_) noexcept {
    FileError error{};
    error.operation = FileOperation::stat;
    error.code = std::make_error_code(std::errc::result_out_of_range);
    error.offset = 0;
    error.requested = destination_size_bytes_;
    error.processed = static_cast<std::size_t>(file_size_bytes_ > static_cast<std::uint64_t>(SIZE_MAX)
        ? SIZE_MAX
        : file_size_bytes_);
    error.end_of_file = file_size_bytes_ < destination_size_bytes_;
    return error;
}

template<PodReadable pod_type>
[[nodiscard]] CENTER_FILE_ALWAYS_INLINE
FileStatus readFileToSpan(
    const std::filesystem::path& path_,
    std::span<pod_type> destination_,
    FileHint hint_ = FileHint::sequential,
    FileShare share_ = FileShare::read
) noexcept {
#ifndef NDEBUG
    if (!destination_.empty()) {
        assert(destination_.data() != nullptr);
        const auto address_value = reinterpret_cast<std::uintptr_t>(destination_.data());
        assert((address_value % alignof(pod_type)) == 0);
    }
#endif

    const auto open_result = PlatformFile::openRead(path_, hint_, share_);
    if (!open_result) {
        return makeUnexpected(open_result.error());
    }

    const auto& file = open_result.value();

    const auto file_size_result = file.size();
    if (!file_size_result) {
        return makeUnexpected(file_size_result.error());
    }

    const auto destination_bytes = std::as_writable_bytes(destination_);
    const auto file_size_bytes = file_size_result.value();

    if (CENTER_FILE_UNLIKELY(file_size_bytes != static_cast<std::uint64_t>(destination_bytes.size()))) {
        return makeUnexpected(makeSizeMismatchError(file_size_bytes, destination_bytes.size()));
    }

    if (destination_bytes.empty()) {
        return {};
    }

    return file.readExactAt(0, destination_bytes);
}

} // namespace Tool::File

