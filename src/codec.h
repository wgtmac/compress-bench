#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace compress_bench {

struct CompressedPage {
  std::vector<std::uint8_t> bytes;
  std::size_t original_size = 0;
  std::uint64_t checksum = 0;
};

class CodecContext {
 public:
  virtual ~CodecContext() = default;
};

class Codec {
 public:
  virtual ~Codec() = default;

  virtual std::string name() const = 0;
  virtual std::string version() const = 0;
  virtual std::size_t compress_bound(std::size_t input_size) const = 0;
  virtual bool validate_level(int level, std::string* error) const = 0;
  virtual std::unique_ptr<CodecContext> create_context() const = 0;
  virtual bool compress(const std::uint8_t* input,
                        std::size_t input_size,
                        int level,
                        std::vector<std::uint8_t>* output,
                        std::string* error) const = 0;
  virtual bool decompress(const CodecContext& context,
                          const std::uint8_t* input,
                          std::size_t input_size,
                          std::uint8_t* output,
                          std::size_t output_capacity,
                          std::size_t* output_size,
                          std::string* error) const = 0;
};

std::unique_ptr<Codec> MakeCodec(const std::string& name);
std::vector<std::string> SupportedCodecNames();

}  // namespace compress_bench
