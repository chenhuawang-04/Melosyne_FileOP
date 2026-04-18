module;

#include <filesystem>

export module Center.File.PlatformFile;

import Center.File.Error;
import Center.File.Types;

export namespace Center::File {

class PlatformFile {
public:
    PlatformFile() noexcept = default;
    PlatformFile(const PlatformFile&) = delete;
    PlatformFile& operator=(const PlatformFile&) = delete;

    PlatformFile(PlatformFile&& other_) noexcept;
    PlatformFile& operator=(PlatformFile&& other_) noexcept;

    ~PlatformFile() noexcept;

    [[nodiscard]] static FileResult<PlatformFile> open(
        const std::filesystem::path& path_,
        OpenOptions options_ = {}
    ) noexcept;

    [[nodiscard]] static FileResult<PlatformFile> openRead(
        const std::filesystem::path& path_,
        FileHint hint_ = FileHint::sequential,
        FileShare share_ = FileShare::read
    ) noexcept;

    [[nodiscard]] static FileResult<PlatformFile> openWrite(
        const std::filesystem::path& path_,
        bool truncate_ = true,
        FileHint hint_ = FileHint::sequential,
        FileShare share_ = FileShare::none
    ) noexcept;

    [[nodiscard]] static FileResult<PlatformFile> openReadWrite(
        const std::filesystem::path& path_,
        FileCreation creation_ = FileCreation::openAlways,
        FileHint hint_ = FileHint::normal,
        FileShare share_ = FileShare::none
    ) noexcept;

    [[nodiscard]] bool isOpen() const noexcept;

    FileStatus close() noexcept;

    [[nodiscard]] FileResult<std::uint64_t> size() const noexcept;

    [[nodiscard]] FileResult<std::size_t> readAt(
        std::uint64_t offset_,
        MutableBytes destination_
    ) const noexcept;

    [[nodiscard]] FileStatus readExactAt(
        std::uint64_t offset_,
        MutableBytes destination_
    ) const noexcept;

    [[nodiscard]] FileResult<std::size_t> writeAt(
        std::uint64_t offset_,
        ConstBytes source_
    ) const noexcept;

    [[nodiscard]] FileStatus writeExactAt(
        std::uint64_t offset_,
        ConstBytes source_
    ) const noexcept;

    [[nodiscard]] FileStatus resize(std::uint64_t size_bytes_) noexcept;
    [[nodiscard]] FileStatus flush() noexcept;

private:
    explicit PlatformFile(void* native_handle_) noexcept;

    [[nodiscard]] static void* invalidHandle() noexcept;

    void* native_handle_ = invalidHandle();
};

} // namespace Center::File
