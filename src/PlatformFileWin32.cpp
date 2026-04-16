#include "Center/File/PlatformFile.hpp"

#if !defined(CENTER_FILE_WINDOWS)
#error "PlatformFileWin32.cpp requires Windows."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace Center::File {

namespace {

[[nodiscard]] FileError makeLastError(
    FileOperation operation_,
    DWORD win32_error_,
    std::uint64_t offset_ = 0,
    std::size_t requested_ = 0,
    std::size_t processed_ = 0,
    bool end_of_file_ = false
) noexcept {
    return FileError{
        .operation = operation_,
        .code = std::error_code(static_cast<int>(win32_error_), std::system_category()),
        .offset = offset_,
        .requested = requested_,
        .processed = processed_,
        .end_of_file = end_of_file_
    };
}

[[nodiscard]] FileError makeGenericError(
    FileOperation operation_,
    std::errc ec_,
    std::uint64_t offset_ = 0,
    std::size_t requested_ = 0,
    std::size_t processed_ = 0,
    bool end_of_file_ = false
) noexcept {
    return FileError{
        .operation = operation_,
        .code = std::make_error_code(ec_),
        .offset = offset_,
        .requested = requested_,
        .processed = processed_,
        .end_of_file = end_of_file_
    };
}

[[nodiscard]] HANDLE asHandle(void* native_handle_) noexcept {
    return static_cast<HANDLE>(native_handle_);
}

[[nodiscard]] bool isValidHandle(HANDLE handle_) noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
}

[[nodiscard]] DWORD toDesiredAccess(FileAccess access_) noexcept {
    switch (access_) {
        case FileAccess::read:
            return GENERIC_READ;
        case FileAccess::write:
            return GENERIC_WRITE;
        case FileAccess::readWrite:
            return GENERIC_READ | GENERIC_WRITE;
    }
    return 0;
}

[[nodiscard]] DWORD toShareMode(FileShare share_) noexcept {
    DWORD share_mode = 0;
    if (hasFlag(share_, FileShare::read)) {
        share_mode |= FILE_SHARE_READ;
    }
    if (hasFlag(share_, FileShare::write)) {
        share_mode |= FILE_SHARE_WRITE;
    }
    if (hasFlag(share_, FileShare::remove)) {
        share_mode |= FILE_SHARE_DELETE;
    }
    return share_mode;
}

[[nodiscard]] DWORD toCreationDisposition(FileCreation creation_) noexcept {
    switch (creation_) {
        case FileCreation::openExisting:
            return OPEN_EXISTING;
        case FileCreation::createAlways:
            return CREATE_ALWAYS;
        case FileCreation::openAlways:
            return OPEN_ALWAYS;
        case FileCreation::truncateExisting:
            return TRUNCATE_EXISTING;
    }
    return OPEN_EXISTING;
}

[[nodiscard]] DWORD toFileFlags(const OpenOptions& options_) noexcept {
    DWORD flags = FILE_ATTRIBUTE_NORMAL;

    switch (options_.hint) {
        case FileHint::normal:
            break;
        case FileHint::sequential:
            flags |= FILE_FLAG_SEQUENTIAL_SCAN;
            break;
        case FileHint::random:
            flags |= FILE_FLAG_RANDOM_ACCESS;
            break;
    }

    if (options_.write_through) {
        flags |= FILE_FLAG_WRITE_THROUGH;
    }
    if (options_.temporary) {
        flags |= FILE_ATTRIBUTE_TEMPORARY;
    }
    if (options_.unbuffered) {
        flags |= FILE_FLAG_NO_BUFFERING;
    }

    return flags;
}

[[nodiscard]] OVERLAPPED makeOverlapped(std::uint64_t offset_) noexcept {
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset_ & 0xFFFFFFFFull);
    overlapped.OffsetHigh = static_cast<DWORD>(offset_ >> 32);
    return overlapped;
}

[[nodiscard]] LARGE_INTEGER toLargeInteger(std::uint64_t value_) noexcept {
    LARGE_INTEGER value{};
    value.QuadPart = static_cast<LONGLONG>(value_);
    return value;
}

[[nodiscard]] FileResult<std::size_t> doRead(
    HANDLE handle_,
    std::uint64_t offset_,
    MutableBytes destination_
) noexcept {
    std::size_t total_size = 0;

    while (total_size < destination_.size()) {
        const std::size_t remaining_size = destination_.size() - total_size;
        const DWORD chunk_size = static_cast<DWORD>(std::min<std::size_t>(remaining_size, max_io_chunk_bytes));
        OVERLAPPED overlapped = makeOverlapped(offset_ + total_size);

        DWORD transferred_size = 0;
        const BOOL ok = ::ReadFile(
            handle_,
            destination_.data() + total_size,
            chunk_size,
            &transferred_size,
            &overlapped
        );

        if (!ok) {
            return makeUnexpected(makeLastError(
                FileOperation::read,
                ::GetLastError(),
                offset_ + total_size,
                chunk_size,
                total_size
            ));
        }

        total_size += static_cast<std::size_t>(transferred_size);
        if (transferred_size < chunk_size) {
            break;
        }
    }

    return total_size;
}

[[nodiscard]] FileResult<std::size_t> doWrite(
    HANDLE handle_,
    std::uint64_t offset_,
    ConstBytes source_
) noexcept {
    std::size_t total_size = 0;

    while (total_size < source_.size()) {
        const std::size_t remaining_size = source_.size() - total_size;
        const DWORD chunk_size = static_cast<DWORD>(std::min<std::size_t>(remaining_size, max_io_chunk_bytes));
        OVERLAPPED overlapped = makeOverlapped(offset_ + total_size);

        DWORD transferred_size = 0;
        const BOOL ok = ::WriteFile(
            handle_,
            source_.data() + total_size,
            chunk_size,
            &transferred_size,
            &overlapped
        );

        if (!ok) {
            return makeUnexpected(makeLastError(
                FileOperation::write,
                ::GetLastError(),
                offset_ + total_size,
                chunk_size,
                total_size
            ));
        }

        total_size += static_cast<std::size_t>(transferred_size);
        if (transferred_size < chunk_size) {
            break;
        }
    }

    return total_size;
}

} // namespace

PlatformFile::PlatformFile(void* native_handle_) noexcept
        : native_handle_(native_handle_) {
}

PlatformFile::PlatformFile(PlatformFile&& other_) noexcept
    : native_handle_(std::exchange(other_.native_handle_, invalidHandle())) {
}

PlatformFile& PlatformFile::operator=(PlatformFile&& other_) noexcept {
    if (this != &other_) {
        if (isOpen()) {
            static_cast<void>(close());
        }
        native_handle_ = std::exchange(other_.native_handle_, invalidHandle());
    }
    return *this;
}

PlatformFile::~PlatformFile() noexcept {
    if (isOpen()) {
        static_cast<void>(close());
    }
}

void* PlatformFile::invalidHandle() noexcept {
    return INVALID_HANDLE_VALUE;
}

FileResult<PlatformFile> PlatformFile::open(
    const std::filesystem::path& path_,
    OpenOptions options_
) noexcept {
    const HANDLE handle = ::CreateFileW(
        path_.c_str(),
        toDesiredAccess(options_.access),
        toShareMode(options_.share),
        nullptr,
        toCreationDisposition(options_.creation),
        toFileFlags(options_),
        nullptr
    );

    if (!isValidHandle(handle)) {
        return makeUnexpected(makeLastError(FileOperation::open, ::GetLastError()));
    }

    return PlatformFile{handle};
}

FileResult<PlatformFile> PlatformFile::openRead(
    const std::filesystem::path& path_,
    FileHint hint_,
    FileShare share_
) noexcept {
    return open(path_, OpenOptions{
        .access = FileAccess::read,
        .creation = FileCreation::openExisting,
        .share = share_,
        .hint = hint_
    });
}

FileResult<PlatformFile> PlatformFile::openWrite(
    const std::filesystem::path& path_,
    bool truncate_,
    FileHint hint_,
    FileShare share_
) noexcept {
    return open(path_, OpenOptions{
        .access = FileAccess::write,
        .creation = truncate_ ? FileCreation::createAlways : FileCreation::openAlways,
        .share = share_,
        .hint = hint_
    });
}

FileResult<PlatformFile> PlatformFile::openReadWrite(
    const std::filesystem::path& path_,
    FileCreation creation_,
    FileHint hint_,
    FileShare share_
) noexcept {
    return open(path_, OpenOptions{
        .access = FileAccess::readWrite,
        .creation = creation_,
        .share = share_,
        .hint = hint_
    });
}

bool PlatformFile::isOpen() const noexcept {
    return isValidHandle(asHandle(native_handle_));
}

FileStatus PlatformFile::close() noexcept {
    if (!isOpen()) {
        return {};
    }

    HANDLE handle = asHandle(native_handle_);
    native_handle_ = invalidHandle();

    if (!::CloseHandle(handle)) {
        return makeUnexpected(makeLastError(FileOperation::close, ::GetLastError()));
    }

    return {};
}

FileResult<std::uint64_t> PlatformFile::size() const noexcept {
    if (!isOpen()) {
        return makeUnexpected(makeGenericError(FileOperation::stat, std::errc::bad_file_descriptor));
    }

    LARGE_INTEGER file_size{};
    if (!::GetFileSizeEx(asHandle(native_handle_), &file_size)) {
        return makeUnexpected(makeLastError(FileOperation::stat, ::GetLastError()));
    }

    return static_cast<std::uint64_t>(file_size.QuadPart);
}

FileResult<std::size_t> PlatformFile::readAt(
    std::uint64_t offset_,
    MutableBytes destination_
) const noexcept {
    if (!isOpen()) {
        return makeUnexpected(makeGenericError(FileOperation::read, std::errc::bad_file_descriptor));
    }
    if (destination_.empty()) {
        return std::size_t{0};
    }
    return doRead(asHandle(native_handle_), offset_, destination_);
}

FileStatus PlatformFile::readExactAt(
    std::uint64_t offset_,
    MutableBytes destination_
) const noexcept {
    if (destination_.empty()) {
        return {};
    }

    auto read_result = readAt(offset_, destination_);
    if (!read_result) {
        return std::unexpected(read_result.error());
    }

    if (*read_result != destination_.size()) {
        return makeUnexpected(makeGenericError(
            FileOperation::read,
            std::errc::io_error,
            offset_ + *read_result,
            destination_.size(),
            *read_result,
            true
        ));
    }

    return {};
}

FileResult<std::size_t> PlatformFile::writeAt(
    std::uint64_t offset_,
    ConstBytes source_
) const noexcept {
    if (!isOpen()) {
        return makeUnexpected(makeGenericError(FileOperation::write, std::errc::bad_file_descriptor));
    }
    if (source_.empty()) {
        return std::size_t{0};
    }
    return doWrite(asHandle(native_handle_), offset_, source_);
}

FileStatus PlatformFile::writeExactAt(
    std::uint64_t offset_,
    ConstBytes source_
) const noexcept {
    if (source_.empty()) {
        return {};
    }

    auto write_result = writeAt(offset_, source_);
    if (!write_result) {
        return std::unexpected(write_result.error());
    }

    if (*write_result != source_.size()) {
        return makeUnexpected(makeGenericError(
            FileOperation::write,
            std::errc::io_error,
            offset_ + *write_result,
            source_.size(),
            *write_result
        ));
    }

    return {};
}

FileStatus PlatformFile::resize(std::uint64_t size_bytes_) noexcept {
    if (!isOpen()) {
        return makeUnexpected(makeGenericError(FileOperation::resize, std::errc::bad_file_descriptor));
    }

    if (size_bytes_ > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        return makeUnexpected(makeGenericError(FileOperation::resize, std::errc::value_too_large));
    }

    const LARGE_INTEGER target_size = toLargeInteger(size_bytes_);
    if (!::SetFilePointerEx(asHandle(native_handle_), target_size, nullptr, FILE_BEGIN)) {
        return makeUnexpected(makeLastError(FileOperation::resize, ::GetLastError()));
    }

    if (!::SetEndOfFile(asHandle(native_handle_))) {
        return makeUnexpected(makeLastError(FileOperation::resize, ::GetLastError()));
    }

    return {};
}

FileStatus PlatformFile::flush() noexcept {
    if (!isOpen()) {
        return makeUnexpected(makeGenericError(FileOperation::flush, std::errc::bad_file_descriptor));
    }

    if (!::FlushFileBuffers(asHandle(native_handle_))) {
        return makeUnexpected(makeLastError(FileOperation::flush, ::GetLastError()));
    }

    return {};
}

} // namespace Center::File
