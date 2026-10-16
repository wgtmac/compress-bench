#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace compress_bench {

struct PageSize {
  std::string label;
  std::size_t bytes;
};

const std::vector<PageSize>& AllPageSizes();
bool ParsePageSize(const std::string& value, PageSize* result, std::string* error);
std::vector<std::uint8_t> GenerateMixedPage(std::size_t size,
                                             std::uint64_t seed,
                                             std::size_t page_index);
std::uint64_t Checksum(const std::uint8_t* data, std::size_t size);

}  // namespace compress_bench
