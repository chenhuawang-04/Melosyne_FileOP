module;

#include <cstddef>

export module Tool.File.Config;

export namespace Tool::File {

inline constexpr std::size_t max_io_chunk_bytes = 1u << 30;

} // namespace Tool::File

