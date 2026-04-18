#pragma once

#include "Center/File/Error.hpp"
#include "Center/File/SchedulingTypes.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>

namespace Tool::File {

struct StreamReadConfig {
    std::size_t chunk_bytes = 512 * 1024;
    std::size_t prefetch_depth = 1;

    ReadUrgency urgency = ReadUrgency::streaming;

    bool allow_mapped_copy = true;
    bool allow_mapped_view = false;
    bool loop_on_eof = false;

    std::pmr::memory_resource* memory_resource = std::pmr::get_default_resource();
};

struct StreamStats {
    std::uint64_t produced_chunks = 0;
    std::uint64_t consumed_chunks = 0;
    std::uint64_t producer_block_count = 0;
    std::uint64_t consumer_starve_count = 0;
    std::uint64_t bytes_read = 0;
};

struct BufferView {
    std::span<const std::byte> bytes{};
    std::uint32_t index = 0;
    std::uint64_t file_offset = 0;
    bool end_of_stream = false;
};

} // namespace Tool::File

