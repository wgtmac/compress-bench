#include "data.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>

namespace compress_bench {
namespace {

class XorShift64 {
 public:
  explicit XorShift64(std::uint64_t state) : state_(state == 0 ? 1 : state) {}

  std::uint64_t next() {
    std::uint64_t x = state_;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    state_ = x;
    return x;
  }

 private:
  std::uint64_t state_;
};

std::string Upper(std::string value) {
  for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return value;
}

}  // namespace

const std::vector<PageSize>& AllPageSizes() {
  static const std::vector<PageSize> sizes = {
      {"64K", 64ULL * 1024},
      {"128K", 128ULL * 1024},
      {"256K", 256ULL * 1024},
      {"1M", 1ULL * 1024 * 1024},
      {"2M", 2ULL * 1024 * 1024},
      {"4M", 4ULL * 1024 * 1024},
      {"8M", 8ULL * 1024 * 1024},
  };
  return sizes;
}

bool ParsePageSize(const std::string& value, PageSize* result, std::string* error) {
  const std::string upper = Upper(value);
  for (const PageSize& size : AllPageSizes()) {
    if (upper == size.label) {
      *result = size;
      return true;
    }
  }
  *error = "unsupported page size: " + value;
  return false;
}

std::vector<std::uint8_t> GenerateMixedPage(std::size_t size,
                                             std::uint64_t seed,
                                             std::size_t page_index) {
  std::vector<std::uint8_t> data(size);
  XorShift64 rng(seed ^ (0x9e3779b97f4a7c15ULL * (page_index + 1)));
  const std::size_t dictionary_end = size * 22 / 100;
  const std::size_t delta_end = size * 47 / 100;
  const std::size_t run_end = size * 70 / 100;

  static constexpr std::uint8_t dictionary[] = {
      0x00, 0x01, 0x02, 0x03, 0x07, 0x0f, 0x10, 0x11,
      0x20, 0x21, 0x40, 0x41, 0x80, 0x81, 0xaa, 0x55,
  };
  for (std::size_t i = 0; i < dictionary_end; ++i) {
    data[i] = dictionary[(i / 7 + (rng.next() & 3)) % (sizeof(dictionary) / sizeof(dictionary[0]))];
  }

  std::uint32_t value = static_cast<std::uint32_t>(rng.next());
  for (std::size_t i = dictionary_end; i < delta_end; i += 4) {
    value += static_cast<std::uint32_t>((rng.next() % 17) + 1);
    const std::size_t remaining = std::min<std::size_t>(4, delta_end - i);
    for (std::size_t j = 0; j < remaining; ++j) {
      data[i + j] = static_cast<std::uint8_t>((value >> (j * 8)) & 0xff);
    }
  }

  for (std::size_t i = delta_end; i < run_end; ++i) {
    const std::size_t run = (i - delta_end) / 97;
    data[i] = static_cast<std::uint8_t>((run * 13 + page_index * 3) & 0xff);
  }

  for (std::size_t i = run_end; i < size; ++i) {
    data[i] = static_cast<std::uint8_t>(rng.next() & 0xff);
  }
  return data;
}

std::uint64_t Checksum(const std::uint8_t* data, std::size_t size) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

}  // namespace compress_bench
