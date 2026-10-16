#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "codec.h"

namespace compress_bench {

enum class BenchmarkMode { Hot, Rotate };

const char* ModeName(BenchmarkMode mode);

struct BenchmarkOptions {
  BenchmarkMode mode = BenchmarkMode::Hot;
  std::size_t working_set_pages = 8;
  std::uint64_t warmup_ms = 500;
  std::optional<std::uint64_t> duration_ms = 1000;
  std::optional<std::uint64_t> iterations;
  bool verify = true;
  bool latency_sample = false;
  std::size_t latency_sample_rate = 1000;
};

struct BenchmarkResult {
  bool ok = false;
  std::string error;
  std::string codec;
  std::string codec_version;
  std::string mode;
  int level = 1;
  std::size_t page_size = 0;
  double average_compressed_bytes = 0;
  double compression_ratio = 0;
  int threads = 1;
  std::uint64_t operations = 0;
  std::uint64_t uncompressed_bytes = 0;
  std::uint64_t compressed_bytes = 0;
  std::uint64_t elapsed_ns = 0;
  std::uint64_t cpu_ns = 0;
  std::uint64_t peak_rss_bytes = 0;
  std::uint64_t compression_ns = 0;
  double compression_mib_per_second = 0;
  double uncompressed_mib_per_second = 0;
  double compressed_mib_per_second = 0;
  double pages_per_second = 0;
  double average_ns_per_page = 0;
  std::optional<std::uint64_t> p50_ns;
  std::optional<std::uint64_t> p95_ns;
  std::optional<std::uint64_t> p99_ns;
  bool verification_passed = false;
};

BenchmarkResult RunBenchmark(const Codec& codec,
                             int level,
                             const std::vector<CompressedPage>& pages,
                             const BenchmarkOptions& options,
                             int threads);

}  // namespace compress_bench
