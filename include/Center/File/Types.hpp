#pragma once

#include "Center/File/Config.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace Tool::File {

using MutableBytes = std::span<std::byte>;
using ConstBytes = std::span<const std::byte>;

enum class FileAccess : std::uint8_t {
    read,
    write,
    readWrite
};

enum class FileCreation : std::uint8_t {
    openExisting,
    createAlways,
    openAlways,
    truncateExisting
};

enum class FileShare : std::uint8_t {
    none = 0,
    read = 1 << 0,
    write = 1 << 1,
    remove = 1 << 2,
    readWrite = (1 << 0) | (1 << 1),
    all = (1 << 0) | (1 << 1) | (1 << 2)
};

enum class FileHint : std::uint8_t {
    normal,
    sequential,
    random
};

constexpr FileShare operator|(FileShare lhs_, FileShare rhs_) noexcept {
    using underlying_type = std::underlying_type_t<FileShare>;
    return static_cast<FileShare>(static_cast<underlying_type>(lhs_) | static_cast<underlying_type>(rhs_));
}

constexpr FileShare operator&(FileShare lhs_, FileShare rhs_) noexcept {
    using underlying_type = std::underlying_type_t<FileShare>;
    return static_cast<FileShare>(static_cast<underlying_type>(lhs_) & static_cast<underlying_type>(rhs_));
}

constexpr FileShare& operator|=(FileShare& lhs_, FileShare rhs_) noexcept {
    lhs_ = lhs_ | rhs_;
    return lhs_;
}

[[nodiscard]] constexpr bool hasFlag(FileShare value_, FileShare flag_) noexcept {
    using underlying_type = std::underlying_type_t<FileShare>;
    return (static_cast<underlying_type>(value_) & static_cast<underlying_type>(flag_)) == static_cast<underlying_type>(flag_);
}

struct OpenOptions {
    FileAccess access = FileAccess::read;
    FileCreation creation = FileCreation::openExisting;
    FileShare share = FileShare::read;
    FileHint hint = FileHint::normal;
    bool write_through = false;
    bool temporary = false;
    bool unbuffered = false;
};

} // namespace Tool::File

