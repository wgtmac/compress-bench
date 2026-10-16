#include "codec.h"

#include <algorithm>
#include <limits>
#include <sstream>

#include "lz4.h"
#include "lz4hc.h"
#include "zstd.h"

namespace compress_bench {
namespace {

class ZstdContext final : public CodecContext {
 public:
  ZstdContext() : context_(ZSTD_createDCtx()) {}
  ~ZstdContext() override { ZSTD_freeDCtx(context_); }

  ZSTD_DCtx* context() const { return context_; }

 private:
  ZSTD_DCtx* context_;
};

class Lz4Context final : public CodecContext {};

class ZstdCodec final : public Codec {
 public:
  std::string name() const override { return "zstd"; }
  std::string version() const override { return ZSTD_versionString(); }

  std::size_t compress_bound(std::size_t input_size) const override {
    return ZSTD_compressBound(input_size);
  }

  bool validate_level(int level, std::string* error) const override {
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel()) {
      std::ostringstream out;
      out << "zstd level must be in [" << ZSTD_minCLevel() << ", "
          << ZSTD_maxCLevel() << "]";
      *error = out.str();
      return false;
    }
    return true;
  }

  std::unique_ptr<CodecContext> create_context() const override {
    return std::make_unique<ZstdContext>();
  }

  bool compress(const std::uint8_t* input,
                std::size_t input_size,
                int level,
                std::vector<std::uint8_t>* output,
                std::string* error) const override {
    if (!validate_level(level, error)) return false;
    output->resize(compress_bound(input_size));
    const std::size_t result = ZSTD_compress(output->data(), output->size(),
                                             input, input_size, level);
    if (ZSTD_isError(result)) {
      *error = ZSTD_getErrorName(result);
      return false;
    }
    output->resize(result);
    return true;
  }

  bool decompress(const CodecContext& context,
                  const std::uint8_t* input,
                  std::size_t input_size,
                  std::uint8_t* output,
                  std::size_t output_capacity,
                  std::size_t* output_size,
                  std::string* error) const override {
    const auto* zstd_context = dynamic_cast<const ZstdContext*>(&context);
    if (zstd_context == nullptr || zstd_context->context() == nullptr) {
      *error = "invalid zstd decompression context";
      return false;
    }
    const std::size_t result = ZSTD_decompressDCtx(
        zstd_context->context(), output, output_capacity, input, input_size);
    if (ZSTD_isError(result)) {
      *error = ZSTD_getErrorName(result);
      return false;
    }
    *output_size = result;
    return true;
  }
};

class Lz4Codec final : public Codec {
 public:
  std::string name() const override { return "lz4_raw"; }
  std::string version() const override { return LZ4_versionString(); }

  std::size_t compress_bound(std::size_t input_size) const override {
    if (input_size > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) return 0;
    return static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(input_size)));
  }

  bool validate_level(int level, std::string* error) const override {
    if (level < 1 || level > LZ4HC_CLEVEL_MAX) {
      *error = "lz4_raw level must be in [1, 12]";
      return false;
    }
    return true;
  }

  std::unique_ptr<CodecContext> create_context() const override {
    return std::make_unique<Lz4Context>();
  }

  bool compress(const std::uint8_t* input,
                std::size_t input_size,
                int level,
                std::vector<std::uint8_t>* output,
                std::string* error) const override {
    if (!validate_level(level, error)) return false;
    if (input_size > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) {
      *error = "input is larger than LZ4_MAX_INPUT_SIZE";
      return false;
    }
    const int input_size_int = static_cast<int>(input_size);
    const int bound = LZ4_compressBound(input_size_int);
    output->resize(static_cast<std::size_t>(bound));
    int result = 0;
    if (level == 1) {
      result = LZ4_compress_default(reinterpret_cast<const char*>(input),
                                    reinterpret_cast<char*>(output->data()),
                                    input_size_int, bound);
    } else {
      result = LZ4_compress_HC(reinterpret_cast<const char*>(input),
                                reinterpret_cast<char*>(output->data()),
                                input_size_int, bound, level);
    }
    if (result <= 0) {
      *error = "LZ4 compression failed";
      return false;
    }
    output->resize(static_cast<std::size_t>(result));
    return true;
  }

  bool decompress(const CodecContext& context,
                  const std::uint8_t* input,
                  std::size_t input_size,
                  std::uint8_t* output,
                  std::size_t output_capacity,
                  std::size_t* output_size,
                  std::string* error) const override {
    if (dynamic_cast<const Lz4Context*>(&context) == nullptr) {
      *error = "invalid lz4 decompression context";
      return false;
    }
    if (input_size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        output_capacity > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      *error = "LZ4 input or output exceeds int API limit";
      return false;
    }
    const int result = LZ4_decompress_safe(
        reinterpret_cast<const char*>(input), reinterpret_cast<char*>(output),
        static_cast<int>(input_size), static_cast<int>(output_capacity));
    if (result < 0) {
      *error = "LZ4 decompression failed";
      return false;
    }
    *output_size = static_cast<std::size_t>(result);
    return true;
  }
};

}  // namespace

std::unique_ptr<Codec> MakeCodec(const std::string& name) {
  if (name == "zstd") return std::make_unique<ZstdCodec>();
  if (name == "lz4" || name == "lz4_raw") return std::make_unique<Lz4Codec>();
  return nullptr;
}

std::vector<std::string> SupportedCodecNames() { return {"zstd", "lz4_raw"}; }

}  // namespace compress_bench
