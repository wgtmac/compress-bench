#include "benchmark.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "data.h"
#include "system.h"

namespace compress_bench {
namespace {

class StartGate {
 public:
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return open_; });
  }

  void Open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool open_ = false;
};

struct WorkerState {
  std::uint64_t operations = 0;
  std::uint64_t uncompressed_bytes = 0;
  std::uint64_t compressed_bytes = 0;
  std::vector<std::uint64_t> latency_ns;
};

struct PhaseResult {
  std::uint64_t elapsed_ns = 0;
  std::vector<WorkerState> workers;
  bool ok = true;
  std::string error;
};

PhaseResult RunPhase(const Codec& codec,
                     const std::vector<CompressedPage>& pages,
                     BenchmarkMode mode,
                     int threads,
                     std::optional<std::uint64_t> duration_ms,
                     std::optional<std::uint64_t> total_iterations,
                     bool collect_latency,
                     std::size_t latency_sample_rate) {
  PhaseResult result;
  result.workers.resize(static_cast<std::size_t>(threads));
  StartGate gate;
  std::atomic<bool> failed{false};
  std::mutex error_mutex;
  std::string error;
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));
  std::uint64_t phase_start_ns = 0;

  const std::uint64_t base_iterations = total_iterations.has_value()
                                             ? *total_iterations / static_cast<std::uint64_t>(threads)
                                             : 0;
  const std::uint64_t extra_iterations = total_iterations.has_value()
                                             ? *total_iterations % static_cast<std::uint64_t>(threads)
                                             : 0;
  const std::uint64_t duration_ns = duration_ms.value_or(0) * 1000000ULL;

  for (int worker_id = 0; worker_id < threads; ++worker_id) {
    workers.emplace_back([&, worker_id] {
      WorkerState& state = result.workers[static_cast<std::size_t>(worker_id)];
      std::unique_ptr<CodecContext> context = codec.create_context();
      if (!context) {
        std::lock_guard<std::mutex> lock(error_mutex);
        failed.store(true, std::memory_order_release);
        error = "failed to create codec context";
        return;
      }
      std::vector<std::uint8_t> output(pages.front().original_size);
      const std::uint64_t worker_iterations =
          base_iterations + (static_cast<std::uint64_t>(worker_id) < extra_iterations ? 1 : 0);
      gate.Wait();
      const std::uint64_t deadline_ns = phase_start_ns + duration_ns;
      while (!failed.load(std::memory_order_acquire)) {
        if (total_iterations.has_value()) {
          if (state.operations >= worker_iterations) break;
        } else if (MonotonicNanos() >= deadline_ns) {
          break;
        }

        const std::size_t page_index = mode == BenchmarkMode::Hot
                                           ? 0
                                           : static_cast<std::size_t>(state.operations % pages.size());
        const CompressedPage& page = pages[page_index];
        const bool sample = collect_latency &&
                            (state.operations % std::max<std::size_t>(1, latency_sample_rate) == 0);
        const std::uint64_t operation_start = sample ? MonotonicNanos() : 0;
        std::size_t output_size = 0;
        std::string local_error;
        if (!codec.decompress(*context, page.bytes.data(), page.bytes.size(), output.data(),
                              output.size(), &output_size, &local_error) ||
            output_size != page.original_size) {
          std::lock_guard<std::mutex> lock(error_mutex);
          if (!failed.exchange(true, std::memory_order_acq_rel)) {
            error = local_error.empty() ? "decompressed size mismatch" : local_error;
          }
          break;
        }
        if (sample) state.latency_ns.push_back(MonotonicNanos() - operation_start);
        ++state.operations;
        state.uncompressed_bytes += page.original_size;
        state.compressed_bytes += page.bytes.size();
      }
    });
  }

  phase_start_ns = MonotonicNanos();
  gate.Open();
  for (std::thread& worker : workers) worker.join();
  result.elapsed_ns = MonotonicNanos() - phase_start_ns;
  result.ok = !failed.load(std::memory_order_acquire);
  result.error = error;
  return result;
}

std::optional<std::uint64_t> Percentile(std::vector<std::uint64_t>* values, double percentile) {
  if (values->empty()) return std::nullopt;
  std::sort(values->begin(), values->end());
  const double position = percentile * static_cast<double>(values->size() - 1);
  return (*values)[static_cast<std::size_t>(position + 0.5)];
}

}  // namespace

const char* ModeName(BenchmarkMode mode) {
  return mode == BenchmarkMode::Hot ? "hot" : "rotate";
}

BenchmarkResult RunBenchmark(const Codec& codec,
                             int level,
                             const std::vector<CompressedPage>& pages,
                             const BenchmarkOptions& options,
                             int threads) {
  BenchmarkResult result;
  result.codec = codec.name();
  result.codec_version = codec.version();
  result.mode = ModeName(options.mode);
  result.level = level;
  result.page_size = pages.empty() ? 0 : pages.front().original_size;
  result.threads = threads;

  if (pages.empty()) {
    result.error = "no pages supplied";
    return result;
  }
  if (threads <= 0) {
    result.error = "thread count must be positive";
    return result;
  }

  std::string validation_error;
  if (!codec.validate_level(level, &validation_error)) {
    result.error = validation_error;
    return result;
  }

  for (const CompressedPage& page : pages) {
    if (page.original_size != pages.front().original_size) {
      result.error = "all pages in a benchmark must have the same size";
      return result;
    }
  }

  if (options.verify) {
    std::unique_ptr<CodecContext> context = codec.create_context();
    std::vector<std::uint8_t> output(pages.front().original_size);
    if (!context) {
      result.error = "failed to create codec context for verification";
      return result;
    }
    for (const CompressedPage& page : pages) {
      std::size_t output_size = 0;
      std::string error;
      if (!codec.decompress(*context, page.bytes.data(), page.bytes.size(), output.data(),
                            output.size(), &output_size, &error) ||
          output_size != page.original_size || Checksum(output.data(), output_size) != page.checksum) {
        result.error = error.empty() ? "verification failed" : error;
        return result;
      }
    }
  }
  result.verification_passed = options.verify;

  if (options.warmup_ms > 0) {
    PhaseResult warmup = RunPhase(codec, pages, options.mode, threads, options.warmup_ms,
                                  std::nullopt, false, options.latency_sample_rate);
    if (!warmup.ok) {
      result.error = "warmup failed: " + warmup.error;
      return result;
    }
  }

  const std::uint64_t cpu_start = ProcessCpuNanos();
  PhaseResult measured = RunPhase(codec, pages, options.mode, threads, options.duration_ms,
                                  options.iterations, options.latency_sample,
                                  options.latency_sample_rate);
  const std::uint64_t cpu_end = ProcessCpuNanos();
  if (!measured.ok) {
    result.error = measured.error;
    return result;
  }

  for (const WorkerState& worker : measured.workers) {
    result.operations += worker.operations;
    result.uncompressed_bytes += worker.uncompressed_bytes;
    result.compressed_bytes += worker.compressed_bytes;
    result.average_compressed_bytes += static_cast<double>(worker.compressed_bytes);
  }
  result.cpu_ns = cpu_end >= cpu_start ? cpu_end - cpu_start : 0;
  result.peak_rss_bytes = PeakRssBytes();
  if (result.operations > 0) {
    result.average_compressed_bytes /= static_cast<double>(result.operations);
  } else {
    result.average_compressed_bytes = 0;
  }
  result.elapsed_ns = measured.elapsed_ns;
  result.compression_ratio = result.average_compressed_bytes > 0
                                 ? static_cast<double>(result.page_size) / result.average_compressed_bytes
                                 : 0;
  const double seconds = static_cast<double>(result.elapsed_ns) / 1e9;
  if (seconds > 0) {
    result.uncompressed_mib_per_second =
        static_cast<double>(result.uncompressed_bytes) / seconds / (1024.0 * 1024.0);
    result.compressed_mib_per_second =
        static_cast<double>(result.compressed_bytes) / seconds / (1024.0 * 1024.0);
    result.pages_per_second = static_cast<double>(result.operations) / seconds;
    result.average_ns_per_page = static_cast<double>(result.elapsed_ns) /
                                 static_cast<double>(std::max<std::uint64_t>(1, result.operations));
  }

  std::vector<std::uint64_t> latencies;
  for (WorkerState& worker : measured.workers) {
    latencies.insert(latencies.end(), worker.latency_ns.begin(), worker.latency_ns.end());
  }
  if (!latencies.empty()) {
    result.p50_ns = Percentile(&latencies, 0.50);
    result.p95_ns = Percentile(&latencies, 0.95);
    result.p99_ns = Percentile(&latencies, 0.99);
  }
  result.ok = true;
  return result;
}

}  // namespace compress_bench
