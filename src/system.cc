#include "system.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <thread>

#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace compress_bench {
namespace {

#if defined(__linux__)
std::string ReadFile(const char* path) {
  std::ifstream input(path);
  if (!input) return {};
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string Trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
    value.pop_back();
  }
  std::size_t first = 0;
  while (first < value.size() && value[first] == ' ') ++first;
  return value.substr(first);
}
#endif

std::string SysctlString(const char* name) {
#if defined(__APPLE__)
  std::size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
  if (!value.empty() && value.back() == '\0') value.pop_back();
  return value;
#else
  (void)name;
  return {};
#endif
}

#if defined(__linux__)
std::string LinuxCpuModel() {
  std::istringstream input(ReadFile("/proc/cpuinfo"));
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = line.substr(0, colon);
    if (key == "model name" || key == "Hardware" || key == "Processor") {
      return Trim(line.substr(colon + 1));
    }
  }
  return {};
}

std::string LinuxCpuLimit() {
  const std::string value = Trim(ReadFile("/sys/fs/cgroup/cpu.max"));
  if (value.empty()) return "unavailable";
  return value;
}
#endif

}  // namespace

SystemInfo CollectSystemInfo() {
  SystemInfo info;
  struct utsname uts {};
  if (uname(&uts) == 0) {
    info.platform = uts.sysname;
    info.architecture = uts.machine;
    info.os_version = std::string(uts.release) + " (" + uts.version + ")";
  }
  info.logical_cpus = std::max<std::size_t>(1, std::thread::hardware_concurrency());

#if defined(__APPLE__)
  info.platform = "macOS";
  info.cpu_model = SysctlString("machdep.cpu.brand_string");
  if (info.cpu_model.empty()) info.cpu_model = SysctlString("hw.model");
  info.cpu_limit = "unavailable";
  unsigned int logical = 0;
  std::size_t logical_size = sizeof(logical);
  if (sysctlbyname("hw.logicalcpu", &logical, &logical_size, nullptr, 0) == 0 && logical > 0) {
    info.logical_cpus = logical;
  }
#elif defined(__linux__)
  info.cpu_model = LinuxCpuModel();
  info.cpu_limit = LinuxCpuLimit();
#else
  info.cpu_model = "unavailable";
  info.cpu_limit = "unavailable";
#endif

  if (info.cpu_model.empty()) info.cpu_model = "unavailable";
  if (info.platform.empty()) info.platform = "unknown";
  if (info.architecture.empty()) info.architecture = "unknown";
  if (info.os_version.empty()) info.os_version = "unknown";
  return info;
}

std::uint64_t MonotonicNanos() {
  struct timespec time {};
#if defined(__linux__) && defined(CLOCK_MONOTONIC_RAW)
  clock_gettime(CLOCK_MONOTONIC_RAW, &time);
#else
  clock_gettime(CLOCK_MONOTONIC, &time);
#endif
  return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(time.tv_nsec);
}

std::uint64_t ProcessCpuNanos() {
  struct timespec time {};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0) return 0;
  return static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(time.tv_nsec);
}

std::uint64_t PeakRssBytes() {
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
}

}  // namespace compress_bench
