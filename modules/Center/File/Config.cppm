module;

#include <cstddef>

#if defined(CENTER_FILE_CUSTOM_VECTOR_HEADER)
    #include CENTER_FILE_CUSTOM_VECTOR_HEADER
#else
    #include <vector>
#endif

#ifndef CENTER_FILE_VECTOR_TEMPLATE
    #define CENTER_FILE_VECTOR_TEMPLATE std::vector
#endif

export module Tool.File.Config;

export namespace Tool::File {

template<typename element_type>
using DynamicArray = CENTER_FILE_VECTOR_TEMPLATE<element_type>;

inline constexpr std::size_t max_io_chunk_bytes = 1u << 30;

} // namespace Tool::File
