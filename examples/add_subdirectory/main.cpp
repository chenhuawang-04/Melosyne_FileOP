#include "Center/File/FileOp.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

int main() {
    std::array<std::byte, 64> buffer{};

    auto file_result = Tool::File::PlatformFile::openRead("not_exists.bin");
    if (!file_result) {
        std::cout << "Tool::File linked successfully (open failure is expected in this demo).\n";
        return 0;
    }

    auto file = std::move(*file_result);
    auto read_result = file.readAt(0, std::span<std::byte>{buffer.data(), buffer.size()});
    if (!read_result) {
        std::cout << "read failed\n";
        return 0;
    }

    std::cout << "read bytes=" << *read_result << "\n";
    return 0;
}

