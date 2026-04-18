module;

#include <cstddef>

export module Center.File.Config;

export namespace Center::File {

inline constexpr std::size_t max_io_chunk_bytes = 1u << 30;

} // namespace Center::File
