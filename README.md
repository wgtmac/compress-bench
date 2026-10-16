# compress-bench

In-memory decompression benchmark for zstd and Parquet `LZ4_RAW` blocks.

## Supported platforms

- Linux x86_64 and ARM64 for cloud instances.
- macOS arm64 and x86_64 for local development and validation.

The program is compiled on the machine where it runs. Builds use `-march=native` and are not intended to be copied between machines with different CPU features.

## Build

The installer downloads pinned zstd, LZ4, and, when needed, CMake archives from GitHub. It verifies SHA256 checksums and does not require root.

```bash
./install.sh
./bin/compress-bench self-test
```

Use another compiler or build type with:

```bash
./install.sh --compiler clang++
./install.sh --build-type Debug
```

## Run

The default level is 1. The default suite covers both codecs, all page sizes, hot and rotating page modes, and an automatic thread ladder.

```bash
./bin/compress-bench run \
  --codec all \
  --level 1 \
  --page-size all \
  --threads auto \
  --mode both \
  --warmup 500ms \
  --duration 1s \
  --jsonl results.jsonl
```

Fast local validation:

```bash
./bin/compress-bench run \
  --codec all --page-size 1M --threads 1,2,4,8 \
  --mode both --warmup 100ms --duration 1s
```

Enable sampled page latency with `--latency sample`. `--iterations N` can be used instead of `--duration`; it is total measured operations distributed across workers.

The timed loop only decompresses pre-compressed in-memory pages. With verification enabled, all pages are round-tripped and checksummed before timing; the measured loop checks the decoded size without adding a checksum to every operation.

## Layout

```text
install.sh              dependency bootstrap and local build
CMakeLists.txt          cross-platform build
src/codec.*              zstd and LZ4_RAW adapters
src/data.*               deterministic mixed page generator
src/benchmark.*          worker pool and metrics
src/system.*             Linux/macOS system metrics
tools/versions.lock      pinned dependency versions and checksums
```
