# CMake/Xcode DEBUG build for test 5 -- a debugger, not a benchmark.
#
# Bazel remains the ONLY build that produces publishable numbers: it pins the
# toolchain and compiles FastFHIR and every competitor arm with identical
# flags, which is the entire reason the benchmark repo is Bazel-built. This
# file exists so test 5 can be stepped through in Xcode, and it is deliberately
# -O0: never quote a timing that came out of it.
#
# The google_fhir arm is OFF by default -- it needs protobuf plus the generated
# google-fhir protos, which exist only inside the Bazel graph. The other three
# arms build here.
#
#   cmake --preset xcode-debug && cmake --build --preset xcode-debug
#   open build-xcode/FastFHIRBench.xcodeproj      # scheme: bench_test_5
cmake_minimum_required(VERSION 3.24)
project(FastFHIRBench CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_XCODE_GENERATE_SCHEME ON)

option(BENCH_ARM_FASTFHIR "Build the FastFHIR test-5 arm" ON)
option(BENCH_ARM_JSON     "Build the JSON test-5 arm (needs nlohmann/json)" ON)

# --- FastFHIR: the sibling checkout, and its already-configured build tree ---
set(FASTFHIR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../FastFHIR" CACHE PATH
    "FastFHIR source checkout")
set(FASTFHIR_BUILD "${FASTFHIR_ROOT}/build" CACHE PATH
    "A CONFIGURED FastFHIR build dir -- supplies generated_src/ and libfastfhir")

# --- nlohmann/json: system, then Bazel's cached copy, then fetch ------------
# Bazel already downloaded it; reuse that copy rather than fetching a second.
# Override with -DNLOHMANN_INCLUDE_DIR=<dir containing nlohmann/json.hpp>.
file(GLOB _bazel_nlohmann
     "/private/var/tmp/_bazel_$ENV{USER}/*/external/nlohmann_json/single_include"
     "/private/var/tmp/_bazel_$ENV{USER}/*/external/nlohmann_json/include")
find_path(NLOHMANN_INCLUDE_DIR nlohmann/json.hpp HINTS ${_bazel_nlohmann})

add_executable(bench_test_5 bench/bench_test_5.cpp cmake/arm5_hl7v2.cpp)
target_include_directories(bench_test_5 PRIVATE bench)
if(NLOHMANN_INCLUDE_DIR)
  # Every arm needs it now: the HL7v2 arm parses the JSON carried inside its
  # ZFX segments, so it is no longer nlohmann-free.
  target_include_directories(bench_test_5 PRIVATE "${NLOHMANN_INCLUDE_DIR}")
endif()
target_compile_definitions(bench_test_5 PRIVATE BENCH_NO_ARM_GOOGLE_FHIR)

if(BENCH_ARM_FASTFHIR)
  if(NOT EXISTS "${FASTFHIR_BUILD}/generated_src" AND NOT EXISTS "${FASTFHIR_ROOT}/generated_src")
    message(FATAL_ERROR
      "FastFHIR arm requested but generated_src/ is missing.\n"
      "Configure FastFHIR first:  cmake --preset ninja -S ${FASTFHIR_ROOT}\n"
      "or re-run with -DBENCH_ARM_FASTFHIR=OFF to debug the HL7v2 arm alone.")
  endif()
  target_sources(bench_test_5 PRIVATE cmake/arm5_fastfhir.cpp)
  target_include_directories(bench_test_5 PRIVATE
      "${FASTFHIR_ROOT}/include" "${FASTFHIR_ROOT}/generated_src")
  find_library(FASTFHIR_LIB NAMES fastfhir PATHS "${FASTFHIR_BUILD}" NO_DEFAULT_PATH)
  if(NOT FASTFHIR_LIB)
    message(FATAL_ERROR "libfastfhir not found in ${FASTFHIR_BUILD} -- build it "
                        "(cmake --build --preset ninja) or set -DFASTFHIR_BUILD=<dir>.")
  endif()
  target_link_libraries(bench_test_5 PRIVATE "${FASTFHIR_LIB}")
else()
  target_compile_definitions(bench_test_5 PRIVATE BENCH_NO_ARM_FASTFHIR)
endif()

if(BENCH_ARM_JSON AND NLOHMANN_INCLUDE_DIR)
  target_sources(bench_test_5 PRIVATE cmake/arm5_json.cpp)
  target_include_directories(bench_test_5 PRIVATE "${NLOHMANN_INCLUDE_DIR}")
else()
  if(BENCH_ARM_JSON)
    message(WARNING "nlohmann/json not found -- JSON arm disabled. "
                    "Set -DNLOHMANN_INCLUDE_DIR=<dir containing nlohmann/json.hpp>.")
  endif()
  target_compile_definitions(bench_test_5 PRIVATE BENCH_NO_ARM_JSON)
endif()

# Debug means debuggable: no optimisation, real symbols. The binary SAYS SO at
# runtime -- see BENCH_CMAKE_DEBUG_BUILD. Bazel pins --compilation_mode=opt
# (-O2 -DNDEBUG) and is the only build whose numbers mean anything; a -O0 result
# quoted as a benchmark is the "Debug trap" the FastFHIR CLAUDE.md documents,
# where whole-document read costs differ ~10x between -O0 and -O3.
target_compile_options(bench_test_5 PRIVATE $<$<CONFIG:Debug>:-O0 -g>)
target_compile_definitions(bench_test_5 PRIVATE BENCH_CMAKE_DEBUG_BUILD)
set_target_properties(bench_test_5 PROPERTIES
    XCODE_ATTRIBUTE_GCC_OPTIMIZATION_LEVEL "0"
    XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
