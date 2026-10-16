#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_DIR="${ROOT_DIR}/.deps"
TOOLS_DIR="${ROOT_DIR}/.tools"
BUILD_DIR="${ROOT_DIR}/build"
BIN_DIR="${ROOT_DIR}/bin"
LOCK_FILE="${ROOT_DIR}/tools/versions.lock"

if [[ ! -f "${LOCK_FILE}" ]]; then
  echo "missing ${LOCK_FILE}" >&2
  exit 1
fi

# shellcheck disable=SC1090
source "${LOCK_FILE}"

CXX="${CXX:-}"
BUILD_TYPE="Release"
CLEAN=0

usage() {
  cat <<'EOF'
Usage: ./install.sh [--compiler CXX] [--build-type Release|Debug] [--clean]

Builds compress-bench locally with -march=native.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --compiler)
      [[ $# -ge 2 ]] || { echo "--compiler requires a value" >&2; exit 2; }
      CXX="$2"
      shift 2
      ;;
    --build-type)
      [[ $# -ge 2 ]] || { echo "--build-type requires a value" >&2; exit 2; }
      BUILD_TYPE="$2"
      shift 2
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "${CLEAN}" == 1 ]]; then
  rm -rf "${BUILD_DIR}" "${BIN_DIR}" "${DEPS_DIR}" "${TOOLS_DIR}"
fi

OS="$(uname -s)"
ARCH="$(uname -m)"
case "${OS}:${ARCH}" in
  Linux:x86_64|Linux:amd64)
    CMAKE_ASSET="cmake-${CMK_VERSION}-linux-x86_64.tar.gz"
    CMAKE_SHA="${CMK_LINUX_X86_64_SHA256}"
    ;;
  Linux:aarch64|Linux:arm64)
    CMAKE_ASSET="cmake-${CMK_VERSION}-linux-aarch64.tar.gz"
    CMAKE_SHA="${CMK_LINUX_AARCH64_SHA256}"
    ;;
  Darwin:*)
    CMAKE_ASSET="cmake-${CMK_VERSION}-macos-universal.tar.gz"
    CMAKE_SHA="${CMK_MACOS_UNIVERSAL_SHA256}"
    ;;
  *)
    echo "unsupported platform: ${OS} ${ARCH}" >&2
    exit 1
    ;;
esac

if [[ -z "${CXX}" ]]; then
  for candidate in c++ g++ clang++; do
    if command -v "${candidate}" >/dev/null 2>&1; then
      CXX="${candidate}"
      break
    fi
  done
fi
if [[ -z "${CXX}" ]] || ! command -v "${CXX}" >/dev/null 2>&1; then
  echo "no C++ compiler found; install g++/clang++ or pass --compiler" >&2
  exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
  echo "curl is required to download pinned dependencies" >&2
  exit 1
fi
if ! command -v tar >/dev/null 2>&1; then
  echo "tar is required to unpack pinned dependencies" >&2
  exit 1
fi

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "no sha256 utility found" >&2
    exit 1
  fi
}

download_checked() {
  local url="$1"
  local output="$2"
  local expected="$3"
  if [[ ! -f "${output}" ]]; then
    echo "downloading $(basename "${output}")"
    curl --fail --location --retry 3 --silent --show-error "${url}" -o "${output}"
  fi
  local actual
  actual="$(sha256_file "${output}")"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "checksum mismatch for ${output}" >&2
    echo "expected: ${expected}" >&2
    echo "actual:   ${actual}" >&2
    exit 1
  fi
}

probe_native_arch() {
  local probe_dir="${DEPS_DIR}/probe"
  mkdir -p "${probe_dir}"
  printf 'int main() { return 0; }\n' | "${CXX}" -std=c++17 -march=native -x c++ - -o "${probe_dir}/native" >/dev/null 2>&1 || {
    echo "${CXX} does not support required -march=native" >&2
    exit 1
  }
}

mkdir -p "${DEPS_DIR}/archives" "${DEPS_DIR}/src" "${TOOLS_DIR}"
probe_native_arch

ZSTD_ARCHIVE="${DEPS_DIR}/archives/zstd-${ZSTD_VERSION}.tar.gz"
LZ4_ARCHIVE="${DEPS_DIR}/archives/lz4-${LZ4_VERSION}.tar.gz"
download_checked "${ZSTD_URL}" "${ZSTD_ARCHIVE}" "${ZSTD_SHA256}"
download_checked "${LZ4_URL}" "${LZ4_ARCHIVE}" "${LZ4_SHA256}"

ZSTD_SOURCE_DIR="${DEPS_DIR}/src/zstd-${ZSTD_VERSION}"
LZ4_SOURCE_DIR="${DEPS_DIR}/src/lz4-${LZ4_VERSION}"
if [[ ! -f "${ZSTD_SOURCE_DIR}/build/cmake/CMakeLists.txt" ]]; then
  rm -rf "${ZSTD_SOURCE_DIR}"
  mkdir -p "${ZSTD_SOURCE_DIR}"
  tar -xzf "${ZSTD_ARCHIVE}" -C "${ZSTD_SOURCE_DIR}" --strip-components=1
fi
if [[ ! -f "${LZ4_SOURCE_DIR}/build/cmake/CMakeLists.txt" ]]; then
  rm -rf "${LZ4_SOURCE_DIR}"
  mkdir -p "${LZ4_SOURCE_DIR}"
  tar -xzf "${LZ4_ARCHIVE}" -C "${LZ4_SOURCE_DIR}" --strip-components=1
fi

if command -v cmake >/dev/null 2>&1; then
  CMAKE="$(command -v cmake)"
else
  CMAKE_ARCHIVE="${DEPS_DIR}/archives/${CMAKE_ASSET}"
  download_checked "${CMK_BASE_URL}/${CMAKE_ASSET}" "${CMAKE_ARCHIVE}" "${CMAKE_SHA}"
  CMAKE_ROOT="${TOOLS_DIR}/cmake-${CMK_VERSION}"
  if [[ ! -x "${CMAKE_ROOT}/bin/cmake" &&
        ! -x "${CMAKE_ROOT}/CMake.app/Contents/bin/cmake" ]]; then
    rm -rf "${CMAKE_ROOT}"
    mkdir -p "${CMAKE_ROOT}"
    tar -xzf "${CMAKE_ARCHIVE}" -C "${CMAKE_ROOT}" --strip-components=1
  fi
  if [[ -x "${CMAKE_ROOT}/bin/cmake" ]]; then
    CMAKE="${CMAKE_ROOT}/bin/cmake"
  elif [[ -x "${CMAKE_ROOT}/CMake.app/Contents/bin/cmake" ]]; then
    CMAKE="${CMAKE_ROOT}/CMake.app/Contents/bin/cmake"
  else
    echo "downloaded CMake archive has an unsupported layout" >&2
    exit 1
  fi
fi
if command -v ctest >/dev/null 2>&1; then
  CTEST="$(command -v ctest)"
else
  CTEST="$(dirname "${CMAKE}")/ctest"
fi
if [[ ! -x "${CTEST}" ]]; then
  echo "ctest was not found next to ${CMAKE}" >&2
  exit 1
fi

"${CMAKE}" -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_CXX_COMPILER="${CXX}" \
  -DCOMPRESS_BENCH_ZSTD_SOURCE_DIR="${ZSTD_SOURCE_DIR}" \
  -DCOMPRESS_BENCH_LZ4_SOURCE_DIR="${LZ4_SOURCE_DIR}"

JOBS="${COMPRESS_BENCH_JOBS:-}"
if [[ -z "${JOBS}" ]]; then
  if command -v getconf >/dev/null 2>&1; then
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
  fi
  if [[ -z "${JOBS}" ]] && command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
  fi
fi
JOBS="${JOBS:-2}"
"${CMAKE}" --build "${BUILD_DIR}" --parallel "${JOBS}"

mkdir -p "${BIN_DIR}"
ln -sf "../build/compress-bench" "${BIN_DIR}/compress-bench"
"${CTEST}" --test-dir "${BUILD_DIR}" --output-on-failure
"${BIN_DIR}/compress-bench" self-test

echo
echo "Built ${BIN_DIR}/compress-bench"
echo "Platform: ${OS} ${ARCH}"
echo "Compiler: ${CXX}"
echo "Flags: $("${BIN_DIR}/compress-bench" --version | tail -1)"
