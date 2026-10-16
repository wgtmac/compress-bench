#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace compress_bench {

struct SystemInfo {
  std::string platform;
  std::string architecture;
  std::string os_version;
  std::string cpu_model;
  std::string cpu_limit;
  std::size_t logical_cpus = 1;
};

SystemInfo CollectSystemInfo();
std::uint64_t MonotonicNanos();
std::uint64_t ProcessCpuNanos();
std::uint64_t PeakRssBytes();

}  // namespace compress_bench
