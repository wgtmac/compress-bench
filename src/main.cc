#include "benchmark.h"
#include "build_info.h"
#include "codec.h"
#include "data.h"
#include "system.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace compress_bench {
namespace {

struct AppConfig {
  bool self_test = false;
  bool quiet = false;
  std::vector<std::string> codecs = {"zstd", "lz4_raw"};
  std::vector<int> levels = {1};
  std::vector<PageSize> page_sizes = AllPageSizes();
  bool threads_auto = true;
  std::vector<int> threads;
  std::vector<BenchmarkMode> modes = {BenchmarkMode::Hot, BenchmarkMode::Rotate};
  std::size_t working_set_pages = 8;
  std::uint64_t warmup_ms = 500;
  std::optional<std::uint64_t> duration_ms = 1000;
  std::optional<std::uint64_t> iterations;
  bool verify = true;
  bool latency_sample = false;
  std::size_t latency_sample_rate = 1000;
  std::uint64_t seed = 0x123456789abcdef0ULL;
  std::string jsonl_path;
};

void PrintUsage(std::ostream& out) {
  out << R"(Usage:
  compress-bench run [options]
  compress-bench self-test
  compress-bench --version

Options:
  --codec zstd|lz4|all
  --level N[,N...]
  --page-size 64K|128K|256K|1M|2M|4M|8M|all
  --threads N[,N...]|auto
  --mode hot|rotate|both
  --working-set-pages N
  --duration 1s|500ms|100000us
  --iterations N
  --warmup 500ms
  --latency off|sample
  --latency-sample-rate N
  --seed N
  --verify / --no-verify
  --jsonl PATH
  --quiet
  --help

Default run:
  --codec all --level 1 --page-size all --threads auto --mode both
  --warmup 500ms --duration 1s --verify
)";
}

std::string Lower(std::string value) {
  for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

std::vector<std::string> Split(const std::string& value) {
  std::vector<std::string> values;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::size_t end = comma == std::string::npos ? value.size() : comma;
    if (end > start) values.push_back(value.substr(start, end - start));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return values;
}

std::uint64_t ParseUnsigned(const std::string& value, const char* name) {
  if (value.empty() || value[0] == '-') throw std::runtime_error(std::string("invalid ") + name);
  std::size_t consumed = 0;
  const unsigned long long parsed = std::stoull(value, &consumed, 0);
  if (consumed != value.size()) throw std::runtime_error(std::string("invalid ") + name + ": " + value);
  return static_cast<std::uint64_t>(parsed);
}

int ParseInt(const std::string& value, const char* name) {
  std::size_t consumed = 0;
  const long parsed = std::stol(value, &consumed, 0);
  if (consumed != value.size() || parsed < std::numeric_limits<int>::min() ||
      parsed > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("invalid ") + name + ": " + value);
  }
  return static_cast<int>(parsed);
}

std::uint64_t ParseDurationMs(const std::string& value, const char* name) {
  std::string number = value;
  std::uint64_t multiplier = 1;
  if (number.size() >= 2 && number.substr(number.size() - 2) == "ms") {
    number.resize(number.size() - 2);
    multiplier = 1;
  } else if (number.size() >= 2 && number.substr(number.size() - 2) == "us") {
    number.resize(number.size() - 2);
    const std::uint64_t micros = ParseUnsigned(number, name);
    return std::max<std::uint64_t>(1, (micros + 999) / 1000);
  } else if (!number.empty() && number.back() == 's') {
    number.pop_back();
    multiplier = 1000;
  }
  const std::uint64_t amount = ParseUnsigned(number, name);
  if (amount > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    throw std::runtime_error(std::string("duration is too large: ") + value);
  }
  return amount * multiplier;
}

std::string RequireValue(int* index, int argc, char** argv, const std::string& option) {
  if (*index + 1 >= argc) throw std::runtime_error(option + " requires a value");
  ++*index;
  return argv[*index];
}

std::vector<int> ParseIntList(const std::string& value, const char* name, bool positive) {
  std::vector<int> result;
  for (const std::string& item : Split(value)) {
    const int parsed = ParseInt(item, name);
    if (positive && parsed <= 0) throw std::runtime_error(std::string(name) + " must be positive");
    result.push_back(parsed);
  }
  if (result.empty()) throw std::runtime_error(std::string(name) + " cannot be empty");
  return result;
}

void ParseCodecs(const std::string& value, std::vector<std::string>* output) {
  if (Lower(value) == "all") {
    *output = {"zstd", "lz4_raw"};
    return;
  }
  output->clear();
  for (std::string item : Split(Lower(value))) {
    if (item == "lz4") item = "lz4_raw";
    if (item != "zstd" && item != "lz4_raw") {
      throw std::runtime_error("unsupported codec: " + item);
    }
    if (std::find(output->begin(), output->end(), item) == output->end()) output->push_back(item);
  }
  if (output->empty()) throw std::runtime_error("codec list cannot be empty");
}

void ParsePageSizes(const std::string& value, std::vector<PageSize>* output) {
  if (Lower(value) == "all") {
    *output = AllPageSizes();
    return;
  }
  output->clear();
  for (const std::string& item : Split(value)) {
    PageSize parsed;
    std::string error;
    if (!ParsePageSize(item, &parsed, &error)) throw std::runtime_error(error);
    if (std::none_of(output->begin(), output->end(), [&](const PageSize& size) {
          return size.bytes == parsed.bytes;
        })) {
      output->push_back(parsed);
    }
  }
  if (output->empty()) throw std::runtime_error("page-size list cannot be empty");
}

void ParseModes(const std::string& value, std::vector<BenchmarkMode>* output) {
  const std::string mode = Lower(value);
  if (mode == "both") {
    *output = {BenchmarkMode::Hot, BenchmarkMode::Rotate};
  } else if (mode == "hot") {
    *output = {BenchmarkMode::Hot};
  } else if (mode == "rotate") {
    *output = {BenchmarkMode::Rotate};
  } else {
    throw std::runtime_error("mode must be hot, rotate, or both");
  }
}

AppConfig ParseArgs(int argc, char** argv) {
  AppConfig config;
  if (argc < 2) throw std::runtime_error("a command is required; use --help for usage");
  int index = 1;
  const std::string command = argv[index];
  if (command == "self-test") {
    config.self_test = true;
    if (argc != 2) throw std::runtime_error("self-test does not accept options");
    return config;
  }
  if (command == "--version") {
    config.self_test = false;
    config.quiet = true;
    return config;
  }
  if (command == "run") {
    ++index;
  } else if (command == "--help" || command == "-h") {
    PrintUsage(std::cout);
    std::exit(0);
  } else {
    throw std::runtime_error("unknown command: " + command);
  }

  for (; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--help" || option == "-h") {
      PrintUsage(std::cout);
      std::exit(0);
    } else if (option == "--codec") {
      ParseCodecs(RequireValue(&index, argc, argv, option), &config.codecs);
    } else if (option == "--level") {
      const std::string value = Lower(RequireValue(&index, argc, argv, option));
      if (value == "default") {
        config.levels = {1};
      } else {
        config.levels = ParseIntList(value, "level", false);
      }
    } else if (option == "--page-size") {
      ParsePageSizes(RequireValue(&index, argc, argv, option), &config.page_sizes);
    } else if (option == "--threads") {
      const std::string value = Lower(RequireValue(&index, argc, argv, option));
      if (value == "auto") {
        config.threads_auto = true;
        config.threads.clear();
      } else {
        config.threads_auto = false;
        config.threads = ParseIntList(value, "threads", true);
      }
    } else if (option == "--mode") {
      ParseModes(RequireValue(&index, argc, argv, option), &config.modes);
    } else if (option == "--working-set-pages") {
      config.working_set_pages = ParseUnsigned(RequireValue(&index, argc, argv, option), "working-set-pages");
      if (config.working_set_pages == 0) throw std::runtime_error("working-set-pages must be positive");
    } else if (option == "--duration") {
      config.duration_ms = ParseDurationMs(RequireValue(&index, argc, argv, option), "duration");
      if (*config.duration_ms == 0) throw std::runtime_error("duration must be positive");
    } else if (option == "--iterations") {
      config.iterations = ParseUnsigned(RequireValue(&index, argc, argv, option), "iterations");
      if (*config.iterations == 0) throw std::runtime_error("iterations must be positive");
      config.duration_ms.reset();
    } else if (option == "--warmup") {
      config.warmup_ms = ParseDurationMs(RequireValue(&index, argc, argv, option), "warmup");
    } else if (option == "--latency") {
      const std::string value = Lower(RequireValue(&index, argc, argv, option));
      if (value == "off") {
        config.latency_sample = false;
      } else if (value == "sample") {
        config.latency_sample = true;
      } else {
        throw std::runtime_error("latency must be off or sample");
      }
    } else if (option == "--latency-sample-rate") {
      config.latency_sample_rate = ParseUnsigned(RequireValue(&index, argc, argv, option), "latency-sample-rate");
      if (config.latency_sample_rate == 0) throw std::runtime_error("latency-sample-rate must be positive");
    } else if (option == "--seed") {
      config.seed = ParseUnsigned(RequireValue(&index, argc, argv, option), "seed");
    } else if (option == "--verify") {
      config.verify = true;
    } else if (option == "--no-verify") {
      config.verify = false;
    } else if (option == "--jsonl") {
      config.jsonl_path = RequireValue(&index, argc, argv, option);
    } else if (option == "--quiet") {
      config.quiet = true;
    } else {
      throw std::runtime_error("unknown option: " + option);
    }
  }
  if (config.iterations.has_value() && config.duration_ms.has_value()) {
    throw std::runtime_error("--duration and --iterations are mutually exclusive");
  }
  return config;
}

std::vector<int> ResolveThreads(const AppConfig& config, std::size_t logical_cpus) {
  if (!config.threads_auto) return config.threads;
  const int maximum = static_cast<int>(std::max<std::size_t>(1, logical_cpus));
  std::vector<int> result;
  for (int value = 1; value < maximum; value *= 2) result.push_back(value);
  if (result.empty() || result.back() != maximum) result.push_back(maximum);
  return result;
}

std::size_t EstimateWorkingSetBytes(const std::vector<std::unique_ptr<Codec>>& codecs,
                                    const std::vector<int>& threads,
                                    std::size_t page_size,
                                    std::size_t working_set_pages) {
  const std::size_t max_value = std::numeric_limits<std::size_t>::max();
  if (working_set_pages > max_value / page_size) {
    throw std::runtime_error("working set size overflows memory estimate");
  }
  const std::size_t page_bytes = page_size * working_set_pages;
  std::size_t compressed_bound = 0;
  for (const std::unique_ptr<Codec>& codec : codecs) {
    compressed_bound = std::max(compressed_bound, codec->compress_bound(page_size));
  }
  if (working_set_pages > max_value / std::max<std::size_t>(1, compressed_bound)) {
    throw std::runtime_error("compressed working set size overflows memory estimate");
  }
  const std::size_t compressed_bytes = compressed_bound * working_set_pages;
  if (threads.size() > max_value / page_size) {
    throw std::runtime_error("output buffer estimate overflows");
  }
  const std::size_t output_bytes = page_size * threads.size();
  if (page_bytes > max_value - compressed_bytes ||
      page_bytes + compressed_bytes > max_value - output_bytes) {
    throw std::runtime_error("working set size overflows memory estimate");
  }
  return page_bytes + compressed_bytes + output_bytes;
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c)
              << std::dec << std::setfill(' ');
        } else {
          out << c;
        }
    }
  }
  return out.str();
}

std::string PageSizeLabel(std::size_t bytes) {
  for (const PageSize& size : AllPageSizes()) {
    if (size.bytes == bytes) return size.label;
  }
  return std::to_string(bytes);
}

void WriteOptionalJson(std::ostream& out, const std::optional<std::uint64_t>& value) {
  if (value.has_value()) out << *value;
  else out << "null";
}

void WriteJsonLine(std::ostream& out,
                   const BenchmarkResult& result,
                   const SystemInfo& system,
                   double speedup,
                   double parallel_efficiency,
                   std::uint64_t seed,
                   std::uint64_t warmup_ms,
                   const std::optional<std::uint64_t>& duration_ms,
                   const std::optional<std::uint64_t>& iterations) {
  out << std::setprecision(12);
  out << "{\"schema_version\":1"
      << ",\"platform\":\"" << JsonEscape(system.platform) << "\""
      << ",\"architecture\":\"" << JsonEscape(system.architecture) << "\""
      << ",\"os_version\":\"" << JsonEscape(system.os_version) << "\""
      << ",\"cpu_model\":\"" << JsonEscape(system.cpu_model) << "\""
      << ",\"cpu_limit\":\"" << JsonEscape(system.cpu_limit) << "\""
      << ",\"logical_cpus\":" << system.logical_cpus
      << ",\"compiler_id\":\"" << JsonEscape(COMPRESS_BENCH_COMPILER_ID) << "\""
      << ",\"compiler_version\":\"" << JsonEscape(COMPRESS_BENCH_COMPILER_VERSION) << "\""
      << ",\"compiler_path\":\"" << JsonEscape(COMPRESS_BENCH_COMPILER_PATH) << "\""
      << ",\"compile_flags\":\"" << JsonEscape(COMPRESS_BENCH_COMPILE_FLAGS) << "\""
      << ",\"native_arch\":\"" << COMPRESS_BENCH_MARCH_NATIVE << "\""
      << ",\"codec\":\"" << JsonEscape(result.codec) << "\""
      << ",\"codec_version\":\"" << JsonEscape(result.codec_version) << "\""
      << ",\"level\":" << result.level
      << ",\"mode\":\"" << result.mode << "\""
      << ",\"threads\":" << result.threads
      << ",\"page_size\":" << result.page_size
      << ",\"page_size_label\":\"" << PageSizeLabel(result.page_size) << "\""
      << ",\"profile\":\"mixed-v1\""
      << ",\"seed\":" << seed
      << ",\"warmup_ms\":" << warmup_ms
      << ",\"duration_ms\":";
  if (duration_ms.has_value()) out << *duration_ms;
  else out << "null";
  out << ",\"iterations\":";
  if (iterations.has_value()) out << *iterations;
  else out << "null";
  out
      << ",\"average_compressed_bytes\":" << result.average_compressed_bytes
      << ",\"compression_ratio\":" << result.compression_ratio
      << ",\"operations\":" << result.operations
      << ",\"uncompressed_bytes\":" << result.uncompressed_bytes
      << ",\"compressed_bytes\":" << result.compressed_bytes
      << ",\"elapsed_ns\":" << result.elapsed_ns
      << ",\"cpu_ns\":" << result.cpu_ns
      << ",\"peak_rss_bytes\":" << result.peak_rss_bytes
      << ",\"compression_ns\":" << result.compression_ns
      << ",\"compression_mib_per_second\":" << result.compression_mib_per_second
      << ",\"uncompressed_mib_per_second\":" << result.uncompressed_mib_per_second
      << ",\"compressed_mib_per_second\":" << result.compressed_mib_per_second
      << ",\"pages_per_second\":" << result.pages_per_second
      << ",\"average_ns_per_page\":" << result.average_ns_per_page
      << ",\"p50_ns\":";
  WriteOptionalJson(out, result.p50_ns);
  out << ",\"p95_ns\":";
  WriteOptionalJson(out, result.p95_ns);
  out << ",\"p99_ns\":";
  WriteOptionalJson(out, result.p99_ns);
  out << ",\"speedup\":" << speedup
      << ",\"parallel_efficiency\":" << parallel_efficiency
      << ",\"verification_passed\":" << (result.verification_passed ? "true" : "false")
      << "}\n";
}

std::string ResultKey(const BenchmarkResult& result) {
  return result.codec + ":" + std::to_string(result.level) + ":" +
         std::to_string(result.page_size) + ":" + result.mode;
}

std::string ResultKeyWithThreads(const BenchmarkResult& result) {
  return ResultKey(result) + ":" + std::to_string(result.threads);
}

void PrintResults(const std::vector<BenchmarkResult>& results,
                  const std::unordered_map<std::string, double>& speedups) {
  std::cout << std::left << std::setw(9) << "codec" << std::right << std::setw(6) << "level"
            << std::setw(8) << "page" << std::setw(8) << "mode" << std::setw(8) << "threads"
            << std::setw(10) << "ratio" << std::setw(15) << "MiB/s" << std::setw(13) << "pages/s"
            << std::setw(10) << "speedup" << std::setw(12) << "p50 ns" << '\n';
  for (const BenchmarkResult& result : results) {
    const auto speedup = speedups.find(ResultKeyWithThreads(result));
    std::cout << std::left << std::setw(9) << result.codec << std::right << std::setw(6) << result.level
              << std::setw(8) << PageSizeLabel(result.page_size) << std::setw(8) << result.mode << std::setw(8)
              << result.threads << std::setw(10) << std::fixed << std::setprecision(2)
              << result.compression_ratio << std::setw(15) << std::setprecision(1)
              << result.uncompressed_mib_per_second << std::setw(13) << std::setprecision(0)
              << result.pages_per_second << std::setw(10) << std::setprecision(2)
              << (speedup == speedups.end() ? 0.0 : speedup->second) << std::setw(12)
              << (result.p50_ns.has_value() ? std::to_string(*result.p50_ns) : "-") << '\n';
  }
}

bool SelfTest() {
  for (const PageSize& page_size : AllPageSizes()) {
    const std::vector<std::uint8_t> generated = GenerateMixedPage(page_size.bytes, 1234, 0);
    if (generated.size() != page_size.bytes) return false;
  }
  const std::vector<std::uint8_t> first = GenerateMixedPage(64 * 1024, 1234, 0);
  const std::vector<std::uint8_t> second = GenerateMixedPage(64 * 1024, 1234, 0);
  if (first != second || Checksum(first.data(), first.size()) == 0) return false;

  for (const std::string& name : SupportedCodecNames()) {
    std::unique_ptr<Codec> codec = MakeCodec(name);
    if (!codec) return false;
    std::string level_error;
    if (codec->validate_level(1, &level_error)) {
      const int invalid_level = name == "zstd" ? 100 : 13;
      if (codec->validate_level(invalid_level, &level_error)) return false;
    } else {
      return false;
    }
    std::vector<std::uint8_t> compressed;
    std::string error;
    if (!codec->compress(first.data(), first.size(), 1, &compressed, &error)) return false;
    std::unique_ptr<CodecContext> context = codec->create_context();
    std::vector<std::uint8_t> output(first.size());
    std::size_t output_size = 0;
    if (!context || !codec->decompress(*context, compressed.data(), compressed.size(), output.data(),
                                       output.size(), &output_size, &error) ||
        output_size != first.size() || Checksum(output.data(), output.size()) != Checksum(first.data(), first.size())) {
      return false;
    }
    CompressedPage page{std::move(compressed), first.size(), Checksum(first.data(), first.size())};
    BenchmarkOptions options;
    options.warmup_ms = 0;
    options.duration_ms.reset();
    options.iterations = 2;
    const BenchmarkResult result = RunBenchmark(*codec, 1, {page}, options, 1);
    if (!result.ok || result.operations != 2 || !result.verification_passed) return false;
  }
  return true;
}

int Run(const AppConfig& config) {
  const SystemInfo system = CollectSystemInfo();
  const std::vector<int> threads = ResolveThreads(config, system.logical_cpus);
  for (int thread_count : threads) {
    if (thread_count <= 0) throw std::runtime_error("thread count must be positive");
    if (thread_count > static_cast<int>(system.logical_cpus)) {
      std::cerr << "warning: thread count " << thread_count << " exceeds logical CPUs "
                << system.logical_cpus << '\n';
    }
  }

  std::vector<std::unique_ptr<Codec>> codecs;
  for (const std::string& name : config.codecs) {
    std::unique_ptr<Codec> codec = MakeCodec(name);
    if (!codec) throw std::runtime_error("failed to create codec: " + name);
    for (int level : config.levels) {
      std::string error;
      if (!codec->validate_level(level, &error)) throw std::runtime_error(error);
    }
    codecs.push_back(std::move(codec));
  }

  std::vector<BenchmarkResult> results;
  for (const PageSize& page_size : config.page_sizes) {
    const std::size_t estimated_bytes = EstimateWorkingSetBytes(
        codecs, threads, page_size.bytes, config.working_set_pages);
    constexpr std::size_t kMaxEstimatedBytes = 4ULL * 1024 * 1024 * 1024;
    if (estimated_bytes > kMaxEstimatedBytes) {
      throw std::runtime_error("estimated working set exceeds 4 GiB for page size " + page_size.label +
                               "; reduce --working-set-pages or --threads");
    }
    std::vector<std::vector<std::uint8_t>> raw_pages;
    raw_pages.reserve(config.working_set_pages);
    for (std::size_t page_index = 0; page_index < config.working_set_pages; ++page_index) {
      raw_pages.push_back(GenerateMixedPage(page_size.bytes, config.seed, page_index));
    }

    for (const std::unique_ptr<Codec>& codec : codecs) {
      for (int level : config.levels) {
        std::vector<CompressedPage> pages;
        pages.reserve(raw_pages.size());
        const std::uint64_t compression_start = MonotonicNanos();
        for (const std::vector<std::uint8_t>& raw : raw_pages) {
          CompressedPage page;
          page.original_size = raw.size();
          page.checksum = Checksum(raw.data(), raw.size());
          std::string error;
          if (!codec->compress(raw.data(), raw.size(), level, &page.bytes, &error)) {
            throw std::runtime_error(codec->name() + " compression failed: " + error);
          }
          pages.push_back(std::move(page));
        }
        const std::uint64_t compression_ns = MonotonicNanos() - compression_start;

        for (BenchmarkMode mode : config.modes) {
          for (int thread_count : threads) {
            BenchmarkOptions options;
            options.mode = mode;
            options.working_set_pages = config.working_set_pages;
            options.warmup_ms = config.warmup_ms;
            options.duration_ms = config.duration_ms;
            options.iterations = config.iterations;
            options.verify = config.verify;
            options.latency_sample = config.latency_sample;
            options.latency_sample_rate = config.latency_sample_rate;
            BenchmarkResult result = RunBenchmark(*codec, level, pages, options, thread_count);
            if (!result.ok) {
              throw std::runtime_error(result.codec + " benchmark failed: " + result.error);
            }
            result.compression_ns = compression_ns;
            if (compression_ns > 0) {
              result.compression_mib_per_second =
                  static_cast<double>(page_size.bytes * raw_pages.size()) /
                  (static_cast<double>(compression_ns) / 1e9) / (1024.0 * 1024.0);
            }
            results.push_back(std::move(result));
          }
        }
      }
    }
  }

  std::unordered_map<std::string, double> baselines;
  for (const BenchmarkResult& result : results) {
    const std::string key = ResultKey(result);
    if (result.threads == 1) baselines[key] = result.uncompressed_mib_per_second;
  }
  std::unordered_map<std::string, double> speedups;
  for (const BenchmarkResult& result : results) {
    const auto baseline = baselines.find(ResultKey(result));
    speedups[ResultKeyWithThreads(result)] = baseline == baselines.end() || baseline->second <= 0
                                      ? 0.0
                                      : result.uncompressed_mib_per_second / baseline->second;
  }

  if (!config.quiet) {
    std::cout << "Platform: " << system.platform << " " << system.architecture << "\n"
              << "CPU: " << system.cpu_model << " (" << system.logical_cpus << " logical CPUs)\n"
              << "Compiler: " << COMPRESS_BENCH_COMPILER_ID << " " << COMPRESS_BENCH_COMPILER_VERSION
              << "\nFlags: " << COMPRESS_BENCH_COMPILE_FLAGS << "\n\n";
    PrintResults(results, speedups);
  }

  if (!config.jsonl_path.empty()) {
    std::ofstream jsonl(config.jsonl_path);
    if (!jsonl) throw std::runtime_error("cannot open JSONL output: " + config.jsonl_path);
    for (const BenchmarkResult& result : results) {
      const double speedup = speedups[ResultKeyWithThreads(result)];
      const double efficiency = result.threads > 0 ? speedup / static_cast<double>(result.threads) : 0.0;
      WriteJsonLine(jsonl, result, system, speedup, efficiency, config.seed, config.warmup_ms,
                    config.duration_ms, config.iterations);
    }
  }
  return 0;
}

}  // namespace
}  // namespace compress_bench

int main(int argc, char** argv) {
  try {
    compress_bench::AppConfig config = compress_bench::ParseArgs(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "--version") {
      std::cout << "compress-bench " << COMPRESS_BENCH_VERSION << "\n"
                << COMPRESS_BENCH_COMPILER_ID << " " << COMPRESS_BENCH_COMPILER_VERSION << "\n"
                << COMPRESS_BENCH_COMPILE_FLAGS << "\n";
      return 0;
    }
    if (config.self_test) {
      const bool passed = compress_bench::SelfTest();
      std::cout << (passed ? "self-test: PASS\n" : "self-test: FAIL\n");
      return passed ? 0 : 1;
    }
    return compress_bench::Run(config);
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
